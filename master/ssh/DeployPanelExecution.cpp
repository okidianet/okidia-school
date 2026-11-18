#include "DeployPanel.h"
#include "DeployPanelCommon.h"
#include "DeployProgressGrid.h"
#include "DeployWorker.h"
#include "HostInstallCard.h"
#include "../audit/AuditLog.h"
#include "../session/SessionManager.h"
#include "../../common/ui/ThemeManager.h"

#include <QApplication>
#include <QFile>
#include <QHostAddress>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPointer>
#include <QStackedWidget>
#include <QThreadPool>
#include <atomic>

static QThreadPool* deployThreadPool() {
    static QThreadPool* pool = []() {
        auto* p = new QThreadPool();
        p->setMaxThreadCount(6); // Zayıf ağlarda aşırı TCP/SSH oturum patlamasını önle
        return p;
    }();
    return pool;
}

void DeployPanel::onInstallSelected() {
  QList<int> selected;
  for (auto *item : m_unifiedHostTable->selectedItems()) {
    int row = item->row();
    if (!selected.contains(row)) {
      // Sadece Hazır, Kuruldu veya Hata (SSH doğrulanmış) olanları listeye al
      if (auto *st = m_unifiedHostTable->item(row, 4)) {
        QString t = st->text();
        if (t.contains(tr("Hazır")) || t.contains("Kuruldu") ||
            t.contains("Hata")) {
          selected << row;
        }
      }
    }
  }

  if (selected.isEmpty()) {
    QMessageBox::information(
        this, tr("Kurulum"),
        tr("Lütfen SSH bağlantısı başarılı (Hazır) cihazlar seçin."));
    return;
  }

  struct Target {
    QString ip;
    QString hostname;
    QString osInfo;
    QString pkgPath;
  };
  QList<Target> targets;

  // Hedef bilgileri, tablo sıralaması bozulmadan topla
  for (int row : selected) {
    QString ip = m_unifiedHostTable->item(row, 0)->text();
    QString hostname = m_unifiedHostTable->item(row, 1)->text();
    QString osInfo = m_unifiedHostTable->item(row, 2)
                         ? m_unifiedHostTable->item(row, 2)->text()
                         : "";
    QString rowPkgPath;
    if (auto *it = m_unifiedHostTable->item(row, 5)) {
      rowPkgPath = it->data(Qt::UserRole).toString();
    }
    if (rowPkgPath.isEmpty() || !QFile::exists(rowPkgPath)) {
      QMessageBox::warning(this, tr("Hata"),
                           QString(tr("Lütfen listedeki seçili cihazlara (Örn: "
                                      "%1 IP'li) geçerli bir paket atayın."))
                               .arg(ip));
      return;
    }
    targets.append({ip, hostname, osInfo, rowPkgPath});
  }

  m_installBtn->setEnabled(false);
  m_installProg->setVisible(true);
  m_installProg->setMaximum(targets.size());
  m_installProg->setValue(0);
  appendLog(tr("Paralel kurulum işlemi başlatıldı (%1 cihaz)...")
                .arg(targets.size()));

  // Grid panelini hazırla ve Grid görünümüne geç
  if (m_progressGrid) {
    m_progressGrid->clearCards();
    for (const auto &tgt : targets) {
      auto *card = new HostInstallCard(tgt.ip, tgt.hostname, tgt.osInfo,
                                       tgt.pkgPath, m_progressGrid);
      m_progressGrid->addCard(card);
    }
  }
  if (m_rightStacked) {
    m_rightStacked->setCurrentIndex(1); // Grid görünümüne geç
  }

  auto *doneCount = new std::atomic<int>(0);
  int totalCount = targets.size();

  QPointer<QTableWidget> tableGuard = m_unifiedHostTable;
  QPointer<QProgressBar> progGuard = m_installProg;
  QPointer<QPushButton> btnGuard = m_installBtn;
  QPointer<DeployProgressGrid> gridGuard = m_progressGrid;

  int targetIdx = 0;
  for (const Target &tgt : targets) {
    int targetRow = -1;
    for (int r = 0; r < m_unifiedHostTable->rowCount(); ++r) {
      if (m_unifiedHostTable->item(r, 0) &&
          m_unifiedHostTable->item(r, 0)->text() == tgt.ip) {
        targetRow = r;
        break;
      }
    }

    if (targetRow != -1) {
      setRowStatus(m_unifiedHostTable, targetRow, tr("Kuruluyor..."),
                   DeployStatusRank::Installing);
    }

    SshClient::Config cfg =
        buildConfigForTarget(m_session, tgt.ip, tgt.hostname, m_credSelector);

    DeployWorker::TargetInfo info;
    info.ip = tgt.ip;
    info.hostname = tgt.hostname;
    info.osInfo = tgt.osInfo;
    info.pkgPath = tgt.pkgPath;
    info.sshConfig = cfg;
    if (m_session) {
      if (m_session->tlsClient()) {
        info.brokerPort = m_session->tlsClient()->port();
        const QString host = m_session->tlsClient()->host();
        if (!host.isEmpty() && host != "127.0.0.1" && host != "localhost") {
          info.brokerCandidateIps.append(host);
        }
      }
      for (const QString &bIp : m_session->brokerAllIps()) {
        if (!info.brokerCandidateIps.contains(bIp) && bIp != "127.0.0.1" && bIp != "localhost") {
          info.brokerCandidateIps.append(bIp);
        }
      }
    }
    // Hedef cihaz ile aynı /24 alt ağında Broker IP'si varsa onu en başa taşı
    const QString targetPrefix = tgt.ip.section('.', 0, 2) + ".";
    for (int i = 0; i < info.brokerCandidateIps.size(); ++i) {
      if (info.brokerCandidateIps[i].startsWith(targetPrefix)) {
        if (i > 0) {
          info.brokerCandidateIps.swapItemsAt(0, i);
        }
        break;
      }
    }

    auto *worker = new DeployWorker(info);
    QString ip = tgt.ip;

    // Grid Kartı ile bağlantılar
    if (m_progressGrid) {
      auto *card = m_progressGrid->card(ip);
      if (card) {
        connect(worker, &DeployWorker::stepChanged, card,
                &HostInstallCard::onStepChanged, Qt::QueuedConnection);
        connect(worker, &DeployWorker::uploadProgress, card,
                &HostInstallCard::onUploadProgress, Qt::QueuedConnection);
        connect(worker, &DeployWorker::logMessage, card,
                &HostInstallCard::onLogMessage, Qt::QueuedConnection);
        connect(worker, &DeployWorker::finished, card,
                &HostInstallCard::onFinished, Qt::QueuedConnection);
      }
    }

    // DeployPanel ana log ve tablo bağlantıları
    connect(
        worker, &DeployWorker::logMessage, this,
        [this](const QString &hostIp, const QString &msg) {
          appendLog(QString("[%1] %2").arg(hostIp, msg));
        },
        Qt::QueuedConnection);

    connect(
        worker, &DeployWorker::finished, this,
        [this, tableGuard, progGuard, btnGuard, gridGuard, ip, doneCount,
         totalCount](const QString &hostIp, bool success,
                     const QString &verOrErr, const QString &fullLog) {
          Q_UNUSED(fullLog);
          if (success) {
            AuditLog::instance().recordDeployment({}, hostIp, tr("SSH uzaktan istemci kurulumu başarılı"));
          } else {
            AuditLog::instance().recordDeployment({}, hostIp, tr("SSH kurulum hatası: ") + verOrErr);
          }
          if (tableGuard) {
            for (int r = 0; r < tableGuard->rowCount(); ++r) {
              if (tableGuard->item(r, 0) &&
                  tableGuard->item(r, 0)->text() == hostIp) {
                setRowStatus(tableGuard, r,
                             success ? tr("Kuruldu") : tr("Kurulum Hatası"),
                             success ? DeployStatusRank::Installed
                                     : DeployStatusRank::Failed,
                             success ? ThemeManager::instance().successColor()
                                     : ThemeManager::instance().errorColor());
                if (success && !verOrErr.isEmpty()) {
                  if (auto *it = tableGuard->item(r, 3)) {
                    it->setText(verOrErr);
                  }
                }
                break;
              }
            }
          }

          if (gridGuard) {
            gridGuard->updateSummary();
          }

          (*doneCount)++;
          int currentDone = doneCount->load();

          if (progGuard) {
            progGuard->setValue(currentDone);
          }

          if (currentDone >= totalCount) {
            if (btnGuard)
              btnGuard->setEnabled(true);
            appendLog(tr("Tüm paralel kurulum işlemleri tamamlandı."));
            delete doneCount;
          }
        },
        Qt::QueuedConnection);

    int delayMs = targetIdx * 250;
    targetIdx++;
    QTimer::singleShot(delayMs, this, [worker]() {
      deployThreadPool()->start(worker);
    });
  }
}
