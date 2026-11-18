#include "DeployPanelCommon.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../session/SessionManager.h"
#include "SshCredSelectorWidget.h"
#include <QTableWidgetItem>

int rowForIp(QTableWidget *table, const QString &ip) {
  if (!table) return -1;
  for (int r = 0; r < table->rowCount(); ++r) {
    if (auto *it = table->item(r, 0)) {
      if (it->text().trimmed() == ip.trimmed())
        return r;
    }
  }
  return -1;
}

bool isRowEligibleForAgentDeploy(QTableWidget *table, int row) {
  if (!table || row < 0 || row >= table->rowCount())
    return false;
  auto *st = table->item(row, 4);
  if (!st)
    return false;
  if (!st->flags().testFlag(Qt::ItemIsSelectable) ||
      !st->flags().testFlag(Qt::ItemIsEnabled)) {
    return false;
  }
  const int rankVal = st->data(Qt::UserRole).toInt();
  const auto rank = static_cast<DeployStatusRank>(rankVal);
  if (rank == DeployStatusRank::Ready ||
      rank == DeployStatusRank::Installed ||
      rank == DeployStatusRank::Installing) {
    return true;
  }
  if (rank == DeployStatusRank::Failed &&
      st->text() == QObject::tr("Kurulum Hatası")) {
    return true;
  }
  return false;
}

void clearRowPackageCell(QTableWidget *table, int row) {
  if (!table || row < 0 || row >= table->rowCount())
    return;
  auto *pkgItem = table->item(row, 5);
  if (!pkgItem) {
    pkgItem = new OkidiaTableItem("");
    table->setItem(row, 5, pkgItem);
  } else {
    pkgItem->setText("");
  }
  pkgItem->setIcon(QIcon());
  pkgItem->setData(Qt::UserRole, QString());
  pkgItem->setForeground(QBrush());
}

void setRowStatus(QTableWidget *table, int row,
                  const QString &statusText, DeployStatusRank rank,
                  const QColor &fgColor,
                  const QString &tooltip) {
  if (!table || row < 0 || row >= table->rowCount())
    return;
  auto *it = table->item(row, 4);
  if (!it) {
    it = new OkidiaTableItem(statusText);
    table->setItem(row, 4, it);
  } else {
    it->setText(statusText);
  }

  switch (rank) {
  case DeployStatusRank::Ready:
  case DeployStatusRank::Installed:
    it->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
    break;
  case DeployStatusRank::SshTesting:
    it->setIcon(ThemeManager::icon(":/ui/hourglass.svg"));
    break;
  case DeployStatusRank::Installing:
    it->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    break;
  case DeployStatusRank::Failed:
    it->setIcon(ThemeManager::icon(":/ui/error_circle.svg"));
    break;
  case DeployStatusRank::SshMissing:
    it->setIcon(ThemeManager::icon(":/ui/warning.svg"));
    break;
  case DeployStatusRank::Conflict:
  case DeployStatusRank::Unsupported:
    it->setIcon(ThemeManager::icon(":/ui/forbidden.svg"));
    break;
  default:
    it->setIcon(QIcon());
    break;
  }

  it->setData(Qt::UserRole, static_cast<int>(rank));
  if (fgColor.isValid()) {
    it->setForeground(fgColor);
  } else {
    it->setForeground(QBrush());
  }
  if (!tooltip.isEmpty()) {
    it->setToolTip(tooltip);
  } else {
    it->setToolTip("");
  }

  // İstemci dağıtımına uygun olmayan durumlarda "Kurulacak Paket" sütununu boş tut
  const bool keepPkg = (rank == DeployStatusRank::Ready ||
                        rank == DeployStatusRank::Installed ||
                        rank == DeployStatusRank::Installing ||
                        (rank == DeployStatusRank::Failed &&
                         statusText == QObject::tr("Kurulum Hatası")));
  if (!keepPkg) {
    clearRowPackageCell(table, row);
  }
}

SshClient::Config
buildConfigForTarget(SessionManager *session, const QString &ip,
                     const QString &hostname,
                     SshCredSelectorWidget *fallbackSelector) {
  SshClient::Config cfg;
  cfg.host = ip;
  cfg.port = 22;
  cfg.connectTimeoutMs = 6000;
  cfg.hostKeyCheck = false;

  QString user, pass;
  bool isKey = false;
  bool found = false;

  if (session) {
    const auto brokerCreds = session->sshCredentials();
    const auto connectedAgents = session->allClientStates();

    QString machineId, classId;
    for (const auto &agent : connectedAgents) {
      if (agent.ip == ip) {
        machineId = agent.machineId;
        classId = session->clientGroup(machineId);
        break;
      }
    }
    if (machineId.isEmpty() && !hostname.isEmpty()) {
      const auto dbAgents = OkidiaDatabase::instance().allAgents();
      for (const auto &agent : dbAgents) {
        if (agent.hostname.toLower() == hostname.toLower()) {
          machineId = agent.machineId;
          classId = agent.classId;
          break;
        }
      }
    }

    for (const auto &c : brokerCreds) {
      if (c.targetId == ip && c.targetType == "agent") {
        user = c.username;
        pass = QString::fromUtf8(c.secret);
        isKey = c.isKey;
        found = true;
        break;
      }
    }
    if (!found && !machineId.isEmpty()) {
      for (const auto &c : brokerCreds) {
        if ((c.targetId == machineId || c.machineId == machineId) &&
            c.targetType == "agent") {
          user = c.username;
          pass = QString::fromUtf8(c.secret);
          isKey = c.isKey;
          found = true;
          break;
        }
      }
    }
    if (!found && !classId.isEmpty()) {
      for (const auto &c : brokerCreds) {
        if (c.targetId == classId && c.targetType == "class") {
          user = c.username;
          pass = QString::fromUtf8(c.secret);
          isKey = c.isKey;
          found = true;
          break;
        }
      }
    }
  }

  if (found) {
    cfg.username = user;
    if (isKey) {
      cfg.authMethod = SshClient::AuthMethod::PrivateKey;
      cfg.privateKeyPath = pass;
      cfg.password.clear();
    } else {
      cfg.authMethod = SshClient::AuthMethod::Password;
      cfg.password = pass;
      cfg.privateKeyPath.clear();
    }
  } else if (fallbackSelector) {
    cfg = fallbackSelector->getConfig(ip);
  } else {
    cfg.username = "root";
    cfg.authMethod = SshClient::AuthMethod::Password;
    cfg.password = "";
  }
  return cfg;
}
