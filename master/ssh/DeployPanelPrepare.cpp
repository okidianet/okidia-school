#include "DeployPanel.h"
#include "DeployPanelCommon.h"
#include "GitHubFetcher.h"
#include "PackageRequirementSolver.h"
#include "../ui/DownloadManagerDialog.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../session/SessionManager.h"
#include "SshCredSelectorWidget.h"
#include "SshCredStore.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QThreadPool>

QThreadPool* DeployPanel::probeThreadPool() {
  static QThreadPool* pool = []() {
    auto* p = new QThreadPool();
    p->setMaxThreadCount(2); // Zayıf Wi-Fi ve paylaşımlı AP'leri tıkamamak için azami 2 eşzamanlı probe
    return p;
  }();
  return pool;
}

void DeployPanel::cancelPendingProbes() {
  probeThreadPool()->clear();
}

void DeployPanel::onBrowsePackage() {
  QString type = m_pkgSourceCombo->currentData().toString();
  if (type == "file") {
    QString p = OkidiaUtils::getOpenFileName(
        this, tr("Agent Paketi Seç"), "deploy_packages",
        tr("Kurulum Paketleri (*.deb *.exe);;Debian Paketleri (*.deb);;Windows Paketleri (*.exe);;Tüm Dosyalar (*)"));
    if (!p.isEmpty()) {
      m_pkgPathEdit->setText(p);
      m_agentPkgPath = p;

      QFileInfo fi(p);
      QString filename = fi.fileName();

      QList<QTableWidgetItem *> selected = m_unifiedHostTable->selectedItems();
      QSet<int> selectedRows;
      for (auto *item : selected)
        selectedRows.insert(item->row());

      for (int row : selectedRows) {
        if (!isRowEligibleForAgentDeploy(m_unifiedHostTable, row)) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue;
        }
        QTableWidgetItem *pkgItem =
            m_unifiedHostTable->item(row, 5); // 5 = Paket
        if (!pkgItem) {
          pkgItem = new OkidiaTableItem(filename);
          m_unifiedHostTable->setItem(row, 5, pkgItem);
        } else {
          pkgItem->setText(filename);
        }
        pkgItem->setIcon(ThemeManager::icon(":/ui/file.svg"));
        pkgItem->setData(Qt::UserRole, p);
        pkgItem->setForeground(QBrush());
      }
      onUnifiedTableSelectionChanged();
    }
  } else if (type == "folder") {
    QString dirPath = OkidiaUtils::getExistingDirectory(
        this,
        tr("Agent Paketlerinin Bulunduğu Klasörü Seçin"),
        "deploy_packages",
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks | QFileDialog::DontUseNativeDialog
    );
    if (!dirPath.isEmpty()) {
      m_pkgPathEdit->setText(dirPath);
      m_agentPkgPath = dirPath;

      QDir dir(dirPath);
      QStringList filters;
      filters << "okidia-agent_*.deb" << "*okidia*agent*.exe" << "*okidia*setup*.exe";
      dir.setNameFilters(filters);
      QFileInfoList fileList = dir.entryInfoList();

      for (int row = 0; row < m_unifiedHostTable->rowCount(); ++row) {
        if (!isRowEligibleForAgentDeploy(m_unifiedHostTable, row)) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue;
        }

        QTableWidgetItem *osItem = m_unifiedHostTable->item(row, 2);
        if (!osItem) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue;
        }

        QStringList osData = osItem->data(Qt::UserRole + 1).toStringList();
        if (osData.isEmpty()) {
          osData = osItem->data(Qt::UserRole).toStringList();
        }
        if (osData.size() < 2) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue; // OS bilgisi yoksa atla
        }

        QString osTag = osData[0];
        QString arch = osData[1];

        QRegularExpression regex(
            OkidiaUtils::getAgentPackageRegex(osTag, arch));

        QString matchedFile;
        QString matchedPath;
        QString highestVer = "0.0.0";

        for (const QFileInfo &fi : fileList) {
          if (regex.match(fi.fileName()).hasMatch()) {
            QString fileVer = "0.0.0";
            QStringList parts = fi.fileName().split('_');
            if (parts.size() >= 3) {
              fileVer = parts[2];
            } else {
              static const QRegularExpression verRegex(R"((\d+\.\d+\.\d+))");
              auto match = verRegex.match(fi.fileName());
              if (match.hasMatch()) {
                fileVer = match.captured(1);
              }
            }
            if (OkidiaUtils::compareVersions(fileVer, highestVer) > 0) {
              highestVer = fileVer;
              matchedFile = fi.fileName();
              matchedPath = fi.absoluteFilePath();
            } else if (matchedFile.isEmpty()) {
              // Fallback
              matchedFile = fi.fileName();
              matchedPath = fi.absoluteFilePath();
            }
          }
        }

        QTableWidgetItem *pkgItem = m_unifiedHostTable->item(row, 5);
        if (!pkgItem) {
          pkgItem = new OkidiaTableItem();
          m_unifiedHostTable->setItem(row, 5, pkgItem);
        }

        if (!matchedFile.isEmpty()) {
          pkgItem->setText(matchedFile);
          pkgItem->setIcon(ThemeManager::icon(":/ui/file.svg"));
          pkgItem->setData(Qt::UserRole, matchedPath);
          pkgItem->setForeground(QBrush());
        } else {
          pkgItem->setText(tr("Paket Bulunamadı"));
          pkgItem->setIcon(ThemeManager::icon(":/ui/error_circle.svg"));
          pkgItem->setData(Qt::UserRole, "");
          pkgItem->setForeground(ThemeManager::instance().errorColor());
        }
      }
      onUnifiedTableSelectionChanged();
    }
  } else if (type == "smart_download") {
    // Taranan tablodaki yalnızca istemci dağıtımına uygun hedef makineleri analiz et
    QList<TargetNode> nodes;
    for (int row = 0; row < m_unifiedHostTable->rowCount(); ++row) {
      if (!isRowEligibleForAgentDeploy(m_unifiedHostTable, row)) {
        clearRowPackageCell(m_unifiedHostTable, row);
        continue;
      }
      QTableWidgetItem *ipItem = m_unifiedHostTable->item(row, 0);
      QTableWidgetItem *hostItem = m_unifiedHostTable->item(row, 1);
      QTableWidgetItem *osItem = m_unifiedHostTable->item(row, 2);
      if (!osItem) continue;

      QStringList osData = osItem->data(Qt::UserRole + 1).toStringList();
      if (osData.isEmpty()) {
        osData = osItem->data(Qt::UserRole).toStringList();
      }
      if (osData.size() < 2) continue;

      TargetNode node;
      node.id = ipItem ? ipItem->text() : QString::number(row);
      node.name = hostItem ? hostItem->text() : node.id;
      node.osTag = osData[0];
      node.arch = osData[1];
      node.role = "agent";
      nodes.append(node);
    }

    if (nodes.isEmpty()) {
      QMessageBox::warning(this, tr("Cihaz Bulunamadı"),
                           tr("Lütfen önce ağı tarayarak veya cihaz ekleyerek istemci kurulumuna hazır (SSH doğrulanmış) hedef makineler bulun."));
      return;
    }

    // Başlangıç gereksinimlerini oluştur
    QStringList searchDirs = { PackageRequirementSolver::defaultPackageCacheDir() };
    if (!m_agentPkgPath.isEmpty() && !searchDirs.contains(m_agentPkgPath)) {
      searchDirs.append(m_agentPkgPath);
    }
    QList<PackageRequirement> reqs = PackageRequirementSolver::solve(nodes, {}, searchDirs);

    auto* dlg = new DownloadManagerDialog(reqs, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    connect(dlg, &DownloadManagerDialog::packagesReady, this, [this](const QStringList& paths) {
      if (paths.isEmpty()) return;

      m_pkgPathEdit->setText(paths.first());
      m_agentPkgPath = paths.first();

      // Tablodaki yalnızca istemci dağıtımına uygun satırları eşleştir
      for (int row = 0; row < m_unifiedHostTable->rowCount(); ++row) {
        if (!isRowEligibleForAgentDeploy(m_unifiedHostTable, row)) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue;
        }

        QTableWidgetItem *osItem = m_unifiedHostTable->item(row, 2);
        if (!osItem) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue;
        }

        QStringList osData = osItem->data(Qt::UserRole + 1).toStringList();
        if (osData.isEmpty()) osData = osItem->data(Qt::UserRole).toStringList();
        if (osData.size() < 2) {
          clearRowPackageCell(m_unifiedHostTable, row);
          continue;
        }

        QString osTag = osData[0];
        QString arch = osData[1];
        QRegularExpression regex(OkidiaUtils::getAgentPackageRegex(osTag, arch));

        QString matchedFile;
        QString matchedPath;
        for (const QString &path : paths) {
          QFileInfo fi(path);
          if (regex.match(fi.fileName()).hasMatch()) {
            matchedFile = fi.fileName();
            matchedPath = fi.absoluteFilePath();
            break;
          }
        }

        QTableWidgetItem *pkgItem = m_unifiedHostTable->item(row, 5);
        if (!pkgItem) {
          pkgItem = new OkidiaTableItem();
          m_unifiedHostTable->setItem(row, 5, pkgItem);
        }

        if (!matchedFile.isEmpty()) {
          pkgItem->setText(matchedFile);
          pkgItem->setIcon(ThemeManager::icon(":/ui/file.svg"));
          pkgItem->setData(Qt::UserRole, matchedPath);
          pkgItem->setForeground(QBrush());
        } else {
          pkgItem->setText(tr("Paket Bulunamadı"));
          pkgItem->setIcon(ThemeManager::icon(":/ui/error_circle.svg"));
          pkgItem->setData(Qt::UserRole, "");
          pkgItem->setForeground(ThemeManager::instance().errorColor());
        }
      }
      onUnifiedTableSelectionChanged();
    });

    dlg->exec();
  } else if (type == "github") {
    GitHubFetcher *fetcher = new GitHubFetcher(this);
    connect(fetcher, &GitHubFetcher::packagesDownloaded, this,
            [this](const QStringList &paths) {
              if (paths.isEmpty())
                return;

              m_pkgPathEdit->setText(paths.first());
              m_agentPkgPath = paths.first();

              QList<QTableWidgetItem *> selected =
                  m_unifiedHostTable->selectedItems();
              QSet<int> selectedRows;
              for (auto *item : selected)
                selectedRows.insert(item->row());

              for (int row : selectedRows) {
                if (!isRowEligibleForAgentDeploy(m_unifiedHostTable, row)) {
                  clearRowPackageCell(m_unifiedHostTable, row);
                  continue;
                }

                QTableWidgetItem *osItem = m_unifiedHostTable->item(row, 2);
                if (!osItem) {
                  clearRowPackageCell(m_unifiedHostTable, row);
                  continue;
                }

                QStringList osData =
                    osItem->data(Qt::UserRole + 1).toStringList();
                if (osData.isEmpty()) {
                  osData = osItem->data(Qt::UserRole).toStringList();
                }
                if (osData.size() < 2) {
                  clearRowPackageCell(m_unifiedHostTable, row);
                  continue; // OS bilgisi yoksa atla
                }

                QString osTag = osData[0];
                QString arch = osData[1];

                QRegularExpression regex(
                    OkidiaUtils::getAgentPackageRegex(osTag, arch));

                QString matchedFile;
                QString matchedPath;

                for (const QString &path : paths) {
                  QFileInfo fi(path);
                  if (regex.match(fi.fileName()).hasMatch()) {
                    matchedFile = fi.fileName();
                    matchedPath = fi.absoluteFilePath();
                    break;
                  }
                }

                QTableWidgetItem *pkgItem = m_unifiedHostTable->item(row, 5);
                if (!pkgItem) {
                  pkgItem = new OkidiaTableItem();
                  m_unifiedHostTable->setItem(row, 5, pkgItem);
                }

                if (!matchedFile.isEmpty()) {
                  pkgItem->setText(matchedFile);
                  pkgItem->setIcon(ThemeManager::icon(":/ui/file.svg"));
                  pkgItem->setData(Qt::UserRole, matchedPath);
                  pkgItem->setForeground(QBrush());
                } else {
                  pkgItem->setText(tr("Paket Bulunamadı"));
                  pkgItem->setIcon(ThemeManager::icon(":/ui/error_circle.svg"));
                  pkgItem->setData(Qt::UserRole, "");
                  pkgItem->setForeground(ThemeManager::instance().errorColor());
                }
              }
              onUnifiedTableSelectionChanged();
            });
    fetcher->showDialogAndFetch();
  }
}

void DeployPanel::probeRowViaSsh(int row, const QString &ip,
                                 const SshClient::Config &cfg) {
  QPointer<DeployPanel> selfGuard = this;
  QPointer<QTableWidget> tableGuard = m_unifiedHostTable;
  QPointer<SessionManager> sessionGuard = m_session;

  // Copy cfg values needed inside thread
  QString cfgUsername = cfg.username;
  QString cfgPassword = cfg.password;
  QString hostname = "-";
  if (m_unifiedHostTable && m_unifiedHostTable->item(row, 1)) {
    hostname = m_unifiedHostTable->item(row, 1)->text();
  }

  SshClient::Config probeCfg = cfg;
  // Zayıf Wi-Fi ağları için minimum 10 saniye bağlantı zaman aşımı
  if (probeCfg.connectTimeoutMs < 10000) {
    probeCfg.connectTimeoutMs = 10000;
  }

  std::ignore = QtConcurrent::run(probeThreadPool(), [selfGuard, tableGuard, sessionGuard, row, ip, probeCfg,
                                   cfgUsername, cfgPassword, hostname]() {
    SshClient client;
    bool sshOk = false;

    // Zayıf ağlarda anlık paket kaybını tolere etmek için 1 kez otomatik retry
    for (int attempt = 0; attempt < 2 && !sshOk; ++attempt) {
      if (attempt > 0) {
        QThread::msleep(1200); // Wi-Fi soket toparlanma beklemesi
      }
      client.connectToHost(probeCfg);
      sshOk = client.isConnected();
    }

    QString errDetail = client.lastError().isEmpty() ? tr("SSH bağlantısı başarısız.") : client.lastError();
    if (errDetail.contains("-9") || errDetail.contains("timeout", Qt::CaseInsensitive) ||
        errDetail.contains("disconnect", Qt::CaseInsensitive)) {
      errDetail = tr("Zaman Aşımı / Paket Kaybı (Zayıf Bağlantı: %1)").arg(errDetail);
    }

    if (!sshOk) {
      Logger::warning(QObject::tr("DeployPanel: ") + ip + QObject::tr(" SSH bağlantısı kurulamadı: ") + errDetail);
      QMetaObject::invokeMethod(
          qApp,
          [tableGuard, ip, errDetail]() {
            if (tableGuard) {
              for (int r = 0; r < tableGuard->rowCount(); ++r) {
                if (tableGuard->item(r, 0) &&
                    tableGuard->item(r, 0)->text() == ip) {
                  setRowStatus(tableGuard, r, tr("Başarısız"),
                               DeployStatusRank::Failed,
                               ThemeManager::instance().errorColor(),
                               errDetail);
                  QColor bg = ThemeManager::instance().errorColor();
                  bg.setAlpha(40);
                  for (int c = 0; c < tableGuard->columnCount(); ++c) {
                    if (auto *item = tableGuard->item(r, c)) {
                      item->setBackground(bg);
                      item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                    }
                  }
                  break;
                }
              }
            }
          },
          Qt::QueuedConnection);
      return;
    }

    QString resolvedHostname = hostname;
    QString osId, osVersion, osTag, arch, masterVer, brokerVer, friendlyOs;
    QStringList installedOkidia;

    // ── Resilient Single-Command Batch Probe (Linux Öncelikli) ──
    // 7 ayrı SSH komutu yerine tek bir kanalda tüm sistem ve paket verilerini tek seferde çeker.
    // Bu sayede ağ trafiği %85 azalır ve Wi-Fi AP soket kilitlenmesi önlenir.
    const QString batchLinuxCmd =
        "echo '===OKIDIA_PROBE==='; "
        "hostname 2>/dev/null; "
        "echo '===ARCH==='; uname -m 2>/dev/null; "
        "echo '===OS_REL==='; cat /etc/os-release 2>/dev/null; "
        "echo '===DEB_VER==='; cat /etc/debian_version 2>/dev/null; "
        "echo '===PKGS==='; dpkg-query -W -f='${Package}:${Version}\\n' okidia-agent okidia-master okidia-broker 2>/dev/null; "
        "echo '===END_PROBE==='";

    int execExit = 0;
    QString batchOut = client.executeSync(batchLinuxCmd, 8000, &execExit).trimmed();

    if (batchOut.contains("===OKIDIA_PROBE===")) {
      // ── Linux Batch Yanıtı Ayrıştırma ──
      QString section;
      QString remoteHost;
      QString remoteArch;
      QString osReleaseRaw;
      QString debVerRaw;
      QStringList pkgLines;

      for (const QString &rawLine : batchOut.split('\n')) {
        QString line = rawLine.trimmed();
        if (line == "===OKIDIA_PROBE===") { section = "HOST"; continue; }
        if (line == "===ARCH===") { section = "ARCH"; continue; }
        if (line == "===OS_REL===") { section = "OS_REL"; continue; }
        if (line == "===DEB_VER===") { section = "DEB_VER"; continue; }
        if (line == "===PKGS===") { section = "PKGS"; continue; }
        if (line == "===END_PROBE===") { break; }

        if (section == "HOST" && remoteHost.isEmpty() && !line.isEmpty()) {
          remoteHost = line;
        } else if (section == "ARCH" && remoteArch.isEmpty() && !line.isEmpty()) {
          remoteArch = line;
        } else if (section == "OS_REL") {
          osReleaseRaw += line + "\n";
        } else if (section == "DEB_VER" && debVerRaw.isEmpty() && !line.isEmpty()) {
          debVerRaw = line;
        } else if (section == "PKGS" && !line.isEmpty()) {
          pkgLines.append(line);
        }
      }

      if (!remoteHost.isEmpty() && !remoteHost.contains(" ") &&
          remoteHost != "Bilinmeyen" && remoteHost != "Gizli Cihaz" &&
          !remoteHost.contains("belirtilen", Qt::CaseInsensitive)) {
        resolvedHostname = remoteHost;
      }

      arch = OkidiaUtils::normalizeArch(remoteArch);

      QMap<QString, QString> osReleaseFields;
      for (const QString &line : osReleaseRaw.split('\n')) {
        QString l = line.trimmed();
        if (l.isEmpty() || l.startsWith("#")) continue;
        int eqIdx = l.indexOf('=');
        if (eqIdx > 0) {
          QString key = l.left(eqIdx).trimmed();
          QString val = l.mid(eqIdx + 1).trimmed();
          if ((val.startsWith('"') && val.endsWith('"')) ||
              (val.startsWith('\'') && val.endsWith('\''))) {
            val = val.mid(1, val.length() - 2);
          }
          osReleaseFields[key] = val;
        }
      }

      friendlyOs = osReleaseFields.value("PRETTY_NAME");
      if (friendlyOs.isEmpty()) {
        const QString rawName = osReleaseFields.value("NAME");
        const QString rawVer = osReleaseFields.value("VERSION_ID");
        friendlyOs = rawName.isEmpty() ? "Linux" : rawName;
        if (!rawVer.isEmpty()) {
          friendlyOs += " " + rawVer;
        }
      }

      const QString rawName = osReleaseFields.value("NAME");
      const QString rawVer = osReleaseFields.value("VERSION_ID");
      osId = rawName.isEmpty() ? "Linux" : rawName;
      osVersion = rawVer;
      osTag = OkidiaUtils::parseOsTag(osReleaseFields, debVerRaw);

      for (const QString &pkgLine : pkgLines) {
        int cIdx = pkgLine.indexOf(':');
        if (cIdx <= 0) continue;
        QString pName = pkgLine.left(cIdx).trimmed();
        QString pVer = pkgLine.mid(cIdx + 1).trimmed();
        if (pVer.isEmpty() || pVer.contains("belirtilen", Qt::CaseInsensitive)) continue;

        if (pName == "okidia-agent") {
          installedOkidia << "Agent v" + pVer;
        } else if (pName == "okidia-master") {
          masterVer = pVer;
          installedOkidia << "Master v" + pVer;
        } else if (pName == "okidia-broker") {
          brokerVer = pVer;
          installedOkidia << "Broker v" + pVer;
        }
      }
    } else {
      // ── Windows Fallback (OpenSSH) ──
      QString winHost = client.executeSync("cmd.exe /c echo %COMPUTERNAME%", 6000).trimmed();
      if (!winHost.isEmpty() && !winHost.contains(" ") && winHost != "%COMPUTERNAME%") {
        resolvedHostname = winHost;
      }

      QString winVerOut = client.executeSync("cmd.exe /c ver", 6000).trimmed();
      if (winVerOut.contains("Windows", Qt::CaseInsensitive) || winVerOut.contains("Microsoft", Qt::CaseInsensitive)) {
        osId = "Windows";
        osTag = "windows";
        osVersion = "10";

        if (winVerOut.contains("10.0.")) {
          QRegularExpression verRe(R"(\[Version\s+10\.0\.(\d+))", QRegularExpression::CaseInsensitiveOption);
          auto m = verRe.match(winVerOut);
          if (m.hasMatch() && m.captured(1).toInt() >= 22000) {
            osVersion = "11";
          } else {
            osVersion = "10";
          }
        }

        QString archOut = client.executeSync("cmd.exe /c echo %PROCESSOR_ARCHITECTURE%", 6000).trimmed();
        arch = OkidiaUtils::normalizeArch(archOut.isEmpty() ? "x86_64" : archOut);
        friendlyOs = QString("Windows %1").arg(osVersion);

        QString scOut = client.executeSync(OkidiaUtils::remoteServiceQueryCmd("OkidiaAgent", true), 6000).trimmed();
        if (scOut.contains("RUNNING", Qt::CaseInsensitive)) {
          installedOkidia << "Agent (Aktif)";
        } else if (scOut.contains("STOPPED", Qt::CaseInsensitive)) {
          installedOkidia << "Agent (Durduruldu)";
        }
      } else {
        osId = "Bilinmeyen OS";
        osTag = "unknown";
        arch = "x86_64";
        friendlyOs = "Bilinmeyen OS";
      }
    }

    // Eğer etapadmin ile girdiysek güncel hostname ile kaydet
    if (cfgUsername == "etapadmin" && sessionGuard) {
      QMetaObject::invokeMethod(
          qApp, [sessionGuard, ip, cfgUsername, cfgPassword, resolvedHostname]() {
            QString existingId;
            for (const auto &c : sessionGuard->sshCredentials()) {
              if (c.targetId == ip && c.targetType == "agent") {
                existingId = c.id;
                break;
              }
            }

            SshCredentialRecord rec;
            rec.id =
                existingId.isEmpty() ? OkidiaUtils::generateUuid() : existingId;
            rec.name = "Otomatik: " + resolvedHostname;
            rec.username = cfgUsername;
            rec.secret = cfgPassword.toUtf8();
            rec.targetId = ip;
            rec.targetType = "agent";
            rec.isKey = false;
            rec.ownerUserId = sessionGuard->activeUser().id;
            sessionGuard->addSshCredential(rec);
          });
    }

    QString displayVer =
        installedOkidia.isEmpty() ? "-" : installedOkidia.join(", ");

    client.disconnectFromHost();

    QMetaObject::invokeMethod(
        qApp,
        [selfGuard, tableGuard, sessionGuard, ip, friendlyOs, osId, osVersion, osTag, arch, displayVer, masterVer,
         brokerVer, resolvedHostname]() {
          if (tableGuard) {
            for (int r = 0; r < tableGuard->rowCount(); ++r) {
              if (tableGuard->item(r, 0) &&
                  tableGuard->item(r, 0)->text() == ip) {
                if (!resolvedHostname.isEmpty() && resolvedHostname != "Gizli Cihaz" && resolvedHostname != "Bilinmeyen" && resolvedHostname != "Bilinmeyen Cihaz" &&
                    resolvedHostname != tr("Bilinmeyen Cihaz") && resolvedHostname != tr("Gizli Cihaz") && resolvedHostname != tr("Bilinmeyen")) {
                  if (auto *it = tableGuard->item(r, 1)) {
                    it->setText(resolvedHostname);
                  }
                  if (sessionGuard) {
                    for (auto cred : sessionGuard->sshCredentials()) {
                      if (cred.targetId == ip && cred.targetType == "agent") {
                        int lastColon = cred.name.lastIndexOf(": ");
                        QString prefix = (lastColon != -1) ? cred.name.left(lastColon + 2) : "Manuel: ";
                        QString newName = prefix + resolvedHostname;
                        if (cred.name != newName) {
                          cred.name = newName;
                          sessionGuard->addSshCredential(cred);
                        }
                      }
                    }
                  }
                }
                bool hasConflict = !masterVer.isEmpty() || !brokerVer.isEmpty();

                if (auto *it = tableGuard->item(r, 2)) {
                  QString displayOs = friendlyOs.isEmpty() ? (osId + (osVersion.isEmpty() ? "" : (" " + osVersion))) : friendlyOs;
                  if (!arch.isEmpty() && !displayOs.contains(arch, Qt::CaseInsensitive)) {
                    displayOs += QString(" (%1)").arg(arch);
                  }
                  it->setText(displayOs);

                  if (hasConflict) {
                    it->setData(Qt::UserRole + 1, QVariant());
                    it->setToolTip("");
                  } else {
                    it->setData(Qt::UserRole + 1, QStringList() << osTag << arch);

                    // Tooltip: Manuel paket yöneticilerine rehberlik
                    QString expectedPkg;
                    if (osTag == "windows") {
                      expectedPkg = QString("okidia-agent_%1.exe veya okidia-agent_%2_%1.exe").arg(arch, osTag);
                    } else {
                      expectedPkg = QString("okidia-agent_%1_*_%2.deb").arg(osTag, arch);
                    }
                    it->setToolTip(tr("Mimari: %1\nOS Etiketi: %2\nBeklenen Paket Şablonu: %3")
                                       .arg(arch, osTag, expectedPkg));
                  }
                }
                if (sessionGuard && !osTag.isEmpty()) {
                  sessionGuard->setHostOs(ip, osTag.toLower());
                }
                if (auto *it = tableGuard->item(r, 3)) {
                  it->setText(displayVer);
                }

                if (hasConflict) {
                  QString conflictReason;
                  if (!masterVer.isEmpty() && !brokerVer.isEmpty())
                    conflictReason = tr("Master ve Broker Kurulu");
                  else if (!masterVer.isEmpty())
                    conflictReason = tr("Master Kurulu");
                  else
                    conflictReason = tr("Broker Kurulu");

                  setRowStatus(tableGuard, r, conflictReason,
                               DeployStatusRank::Conflict,
                               ThemeManager::instance().warningColor(),
                               tr("Ajan paketi, Master veya Broker kurulu "
                                  "makinelere kurulamaz (çakışma)."));

                  QColor bg = ThemeManager::instance().warningColor();
                  bg.setAlpha(40);
                  for (int c = 0; c < tableGuard->columnCount(); ++c) {
                    if (auto *item = tableGuard->item(r, c)) {
                      item->setBackground(bg);
                      item->setFlags(item->flags() & ~Qt::ItemIsSelectable &
                                     ~Qt::ItemIsEnabled);
                    }
                  }
                  clearRowPackageCell(tableGuard, r);
                } else {
                  setRowStatus(tableGuard, r, tr("Hazır"),
                               DeployStatusRank::Ready,
                               ThemeManager::instance().successColor());

                  QColor bg = ThemeManager::instance().successColor();
                  bg.setAlpha(40);
                  for (int c = 0; c < tableGuard->columnCount(); ++c) {
                    if (auto *item = tableGuard->item(r, c)) {
                      item->setBackground(bg);
                      item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                      item->setForeground(QBrush());
                    }
                  }

                  // Eğer kullanıcı daha önceden bir paket klasörü seçmişse, yeni "Hazır" olan bu satırı da otomatik eşleştir
                  if (selfGuard && !selfGuard->m_agentPkgPath.isEmpty()) {
                    QFileInfo pkgDirFi(selfGuard->m_agentPkgPath);
                    if (pkgDirFi.exists() && pkgDirFi.isDir()) {
                      QDir dir(selfGuard->m_agentPkgPath);
                      QStringList filters;
                      filters << "okidia-agent_*.deb" << "*okidia*agent*.exe" << "*okidia*setup*.exe";
                      dir.setNameFilters(filters);
                      QFileInfoList fileList = dir.entryInfoList();

                      QRegularExpression regex(OkidiaUtils::getAgentPackageRegex(osTag, arch));
                      QString matchedFile;
                      QString matchedPath;
                      QString highestVer = "0.0.0";
                      for (const QFileInfo &fi : fileList) {
                        if (regex.match(fi.fileName()).hasMatch()) {
                          QString fileVer = OkidiaUtils::extractPackageVersion(fi.fileName());
                          if (fileVer.isEmpty()) fileVer = "0.0.0";
                          if (OkidiaUtils::compareVersions(fileVer, highestVer) > 0 || matchedFile.isEmpty()) {
                            highestVer = fileVer;
                            matchedFile = fi.fileName();
                            matchedPath = fi.absoluteFilePath();
                          }
                        }
                      }
                      QTableWidgetItem *pkgItem = tableGuard->item(r, 5);
                      if (!pkgItem) {
                        pkgItem = new OkidiaTableItem();
                        tableGuard->setItem(r, 5, pkgItem);
                      }
                      if (!matchedFile.isEmpty()) {
                        pkgItem->setText(matchedFile);
                        pkgItem->setIcon(ThemeManager::icon(":/ui/file.svg"));
                        pkgItem->setData(Qt::UserRole, matchedPath);
                        pkgItem->setForeground(QBrush());
                      } else {
                        pkgItem->setText(tr("Paket Bulunamadı"));
                        pkgItem->setIcon(ThemeManager::icon(":/ui/error_circle.svg"));
                        pkgItem->setData(Qt::UserRole, "");
                        pkgItem->setForeground(ThemeManager::instance().errorColor());
                      }
                    }
                  }
                }
                break;
              }
            }
          }
        },
        Qt::QueuedConnection);
  });
}
