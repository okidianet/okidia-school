#include "DeployPanel.h"
#include "DeployPanelCommon.h"
#include "DeployProgressGrid.h"
#include "HostInstallCard.h"
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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSet>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

void DeployPanel::appendLog(const QString &msg) {
  if (m_installLog) {
    if (!m_installLog->isVisible())
      m_installLog->setVisible(true);
    m_installLog->append(
        QString("[%1] %2")
            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
            .arg(msg));
  }
}

void DeployPanel::onScanNetwork() {
  QStringList ranges;
  if (m_subnetTree) {
    QTreeWidgetItemIterator it(m_subnetTree);
    while (*it) {
      if ((*it)->parent() != nullptr && (*it)->checkState(0) == Qt::Checked)
        ranges << (*it)->data(0, Qt::UserRole).toString();
      ++it;
    }
  }
  const QString manual =
      m_rangeEdit ? m_rangeEdit->text().trimmed() : QString();
  if (!manual.isEmpty())
    ranges << manual;

  if (ranges.isEmpty()) {
    QMessageBox::information(
        this, tr("Tarama"),
        tr("Lütfen en az bir subnet seçin veya manuel aralık girin."));
    return;
  }

  cancelPendingProbes();

  if (m_scanner) {
    m_scanner->stop();
    m_scanner->wait();
    delete m_scanner;
    m_scanner = nullptr;
  }
  m_unifiedHostTable->setRowCount(0);
  m_scanProgress->setVisible(true);
  m_scanProgress->setMaximum(100);
  m_scanProgress->setValue(0);
  m_scanBtn->setEnabled(false);
  if (m_stopScanBtn)
    m_stopScanBtn->setEnabled(true);
  m_scanStatus->setText(tr("Taranıyor..."));

  m_scanner = new NetworkScanner(ranges, this);
  connect(m_scanner, &NetworkScanner::progress, this,
          &DeployPanel::onScanProgress, Qt::QueuedConnection);
  connect(m_scanner, &NetworkScanner::hostFound, this,
          &DeployPanel::onHostFound, Qt::QueuedConnection);
  connect(m_scanner, &NetworkScanner::hostUpdated, this,
          &DeployPanel::onHostUpdated, Qt::QueuedConnection);
  connect(m_scanner, &NetworkScanner::finished, this,
          &DeployPanel::onScanFinished, Qt::QueuedConnection);
  m_scanner->start();
}

void DeployPanel::onScanProgress(int current, int total) {
  m_scanProgress->setMaximum(total);
  m_scanProgress->setValue(current);
  m_scanStatus->setText(QString(tr("%1 / %2 tarandı")).arg(current).arg(total));
}

void DeployPanel::onHostFound(const QString &ip) {
  m_unifiedHostTable->setSortingEnabled(false);
  const int row = m_unifiedHostTable->rowCount();
  m_unifiedHostTable->insertRow(row);
  m_unifiedHostTable->setItem(row, 0, new OkidiaTableItem(ip));
  m_unifiedHostTable->setItem(row, 1, new OkidiaTableItem("..."));
  m_unifiedHostTable->setItem(row, 2, new OkidiaTableItem("..."));
  m_unifiedHostTable->setItem(row, 3, new OkidiaTableItem("-"));

  auto *stItem = new OkidiaTableItem("Bilinmiyor");
  stItem->setData(Qt::UserRole, static_cast<int>(DeployStatusRank::Unknown));
  m_unifiedHostTable->setItem(row, 4, stItem);

  m_unifiedHostTable->setItem(row, 5, new OkidiaTableItem(""));
  m_scanStatus->setText(
      QString("%1 makine bulundu").arg(m_unifiedHostTable->rowCount()));
}

void DeployPanel::onHostUpdated(const QString &ip, const QString &hostname, const QString &osHint) {
  QString actualHostname = hostname;
  if (actualHostname.endsWith(".local", Qt::CaseInsensitive)) {
    actualHostname.chop(6);
  }

  bool isHidden = false;
  if (actualHostname == "Bilinmeyen" || actualHostname == "Gizli Cihaz" || actualHostname == "Bilinmeyen Cihaz" ||
      actualHostname == tr("Bilinmeyen") || actualHostname == tr("Gizli Cihaz") || actualHostname == tr("Bilinmeyen Cihaz")) {
    if (!osHint.isEmpty() && !osHint.contains("Akıllı") && !osHint.contains("IoT")) {
      actualHostname = osHint + " PC";
    } else {
      actualHostname = tr("Bilinmeyen Cihaz");
      isHidden = true;
    }
  }

  QString lowerHost = actualHostname.toLower();
  QString lowerOs = osHint.toLower();
  bool isUnsupported =
      lowerOs.contains("akıllı") || lowerOs.contains("iot") || lowerOs.contains("smart") ||
      lowerHost.contains("android") || lowerHost.contains("webos") ||
      lowerHost.contains("tizen") || lowerHost.contains("smarttv") ||
      lowerHost.contains("smart-tv") || lowerHost.contains("appletv") ||
      lowerHost.contains("modem") || lowerHost.contains("router") ||
      lowerHost.contains("printer") || lowerHost.contains("switch") ||
      lowerHost.contains("accesspoint") || lowerHost.contains("gateway") ||
      lowerHost.contains("fırın") || lowerHost.contains("oven") ||
      lowerHost.contains("bsh");

  bool isSshMissing = osHint.contains("SSH Kapalı");

  for (int row = 0; row < m_unifiedHostTable->rowCount(); ++row) {
    auto *ipItem = m_unifiedHostTable->item(row, 0);
    if (!ipItem || ipItem->text() != ip)
      continue;

    if (auto *it = m_unifiedHostTable->item(row, 1)) {
      it->setText(actualHostname);
    }

    if (isUnsupported) {
      for (int col = 0; col < m_unifiedHostTable->columnCount(); ++col) {
        if (auto *it = m_unifiedHostTable->item(row, col)) {
          it->setForeground(QBrush(QColor(150, 150, 150))); // Soluk gri
          it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
        }
      }
      if (auto *it = m_unifiedHostTable->item(row, 2))
        it->setText(tr("Akıllı Cihaz / IoT"));
      setRowStatus(m_unifiedHostTable, row, tr("Desteklenmiyor"),
                   DeployStatusRank::Unsupported,
                   ThemeManager::instance().warningColor(),
                   tr("Cihaz türü sebebiyle Okidia istemcisi kurulamayacağı "
                      "tespit edildi."));
      break;
    } else if (isSshMissing) {
      for (int col = 0; col < m_unifiedHostTable->columnCount(); ++col) {
        if (auto *it = m_unifiedHostTable->item(row, col)) {
          it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
        }
      }
      if (auto *it = m_unifiedHostTable->item(row, 2)) {
        it->setText(osHint);
        if (m_session) {
          m_session->setHostOs(ip, osHint.toLower());
        }
      }
      QString sshHelp;
      if (osHint.contains("Windows")) {
        sshHelp = tr("Windows makinede OpenSSH Sunucusu etkinleştirilmemiş veya Port 22 kapalı.\n"
                     "Çözüm: Windows Ayarları -> Sistem -> İsteğe Bağlı Özellikler -> OpenSSH Sunucusu'nu ekleyin ve servisi başlatın.");
      } else {
        sshHelp = tr("Linux makinede SSH servisi (Port 22) kapalı veya OpenSSH yüklü değil.\n"
                     "Çözüm: Hedef makinede 'sudo apt install openssh-server && sudo systemctl enable --now ssh' komutunu çalıştırın.");
      }
      setRowStatus(m_unifiedHostTable, row, tr("SSH Servisi Kapalı"),
                   DeployStatusRank::SshMissing,
                   ThemeManager::instance().warningColor(),
                   sshHelp);
      break;
    } else {
      if (auto *it = m_unifiedHostTable->item(row, 2)) {
        if (!osHint.isEmpty()) {
          it->setText(osHint);
          if (m_session) {
            m_session->setHostOs(ip, osHint.toLower());
          }
        } else {
          it->setText(tr("Bilinmiyor"));
        }
      }
    }

    if (!hasMatchingCredential(ip, actualHostname)) {
      setRowStatus(m_unifiedHostTable, row, tr("Erişim Bekleniyor"),
                   DeployStatusRank::AwaitingCred, QColor(),
                   tr("SSH bilgisi kayıtlı değil. 'SSH Bilgisi Gir' butonu ile "
                      "kimlik ekleyebilirsiniz."));
      break;
    }

    setRowStatus(m_unifiedHostTable, row, tr("SSH Deneniyor"),
                 DeployStatusRank::SshTesting);

    SshClient::Config cfg =
        buildConfigForTarget(m_session, ip, actualHostname, m_credSelector);

    probeRowViaSsh(row, ip, cfg);
    break;
  }
}

void DeployPanel::onScanFinished() {
  m_scanBtn->setEnabled(true);
  if (m_stopScanBtn)
    m_stopScanBtn->setEnabled(false);
  m_scanProgress->setVisible(false);
  m_scanStatus->setText(
      QString(tr("%1 makine bulundu")).arg(m_unifiedHostTable->rowCount()));
  m_unifiedHostTable->setSortingEnabled(true);
  Logger::info(QObject::tr("DeployPanel: Tarama tamamlandı: ") +
               QString::number(m_unifiedHostTable->rowCount()) + QObject::tr(" makine"));
}

void DeployPanel::onRefreshTableClicked() {
  if (!m_unifiedHostTable || m_unifiedHostTable->rowCount() == 0) {
    return;
  }

  cancelPendingProbes();

  int count = m_unifiedHostTable->rowCount();
  int staggeredIdx = 0;
  for (int row = 0; row < count; ++row) {
    auto *ipItem = m_unifiedHostTable->item(row, 0);
    if (!ipItem) continue;
    QString ip = ipItem->text().trimmed();
    if (ip.isEmpty()) continue;

    auto *hostItem = m_unifiedHostTable->item(row, 1);
    QString hostname = hostItem ? hostItem->text().trimmed() : QString();

    if (!hasMatchingCredential(ip, hostname)) {
      setRowStatus(m_unifiedHostTable, row, tr("Erişim Bekleniyor"),
                   DeployStatusRank::AwaitingCred, QColor(),
                   tr("SSH bilgisi kayıtlı değil. 'SSH Bilgisi Gir' butonu ile kimlik ekleyebilirsiniz."));
      continue;
    }

    setRowStatus(m_unifiedHostTable, row, tr("SSH Deneniyor"),
                 DeployStatusRank::SshTesting);

    SshClient::Config cfg =
        buildConfigForTarget(m_session, ip, hostname, m_credSelector);

    // Zayıf ağlarda ani AP yükünü önlemek için kademeli gecikme (staggered delay)
    int delayMs = staggeredIdx * 150;
    staggeredIdx++;

    QTimer::singleShot(delayMs, this, [this, ip, cfg]() {
      int curRow = rowForIp(m_unifiedHostTable, ip);
      if (curRow >= 0) {
        probeRowViaSsh(curRow, ip, cfg);
      }
    });
  }

  appendLog(tr("Tablodaki %1 cihazın durumu ve SSH bilgileri kademeli olarak sorgulanıyor...").arg(count));
  if (m_scanStatus) {
    m_scanStatus->setText(tr("%1 cihaz sorgulanıyor...").arg(count));
  }
}

void DeployPanel::onAssignBulkCredentials() {
  QList<QTableWidgetItem *> selectedItems = m_unifiedHostTable->selectedItems();
  if (selectedItems.isEmpty())
    return;

  QSet<int> selectedRows;
  for (auto *item : selectedItems)
    selectedRows.insert(item->row());

  QStringList ips;
  for (int row : selectedRows) {
    if (auto *ipItem = m_unifiedHostTable->item(row, 0)) {
      ips << ipItem->text().trimmed();
    }
  }

  onOpenCredentialDialog(ips);
}

void DeployPanel::onOpenCredentialDialog(const QStringList &ips) {
  if (ips.isEmpty())
    return;

  QDialog dlg(this);
  dlg.setWindowTitle(tr("SSH Kimlik Bilgisi Ata"));
  dlg.setFixedWidth(400);
  auto *lay = new QVBoxLayout(&dlg);
  lay->setSpacing(12);

  auto *info = new QLabel(
      QString(tr("<b>%1</b> adet makine için SSH bilgilerini ayarlayın:"))
          .arg(ips.size()),
      &dlg);
  lay->addWidget(info);

  auto *selector = new SshCredSelectorWidget(m_session, &dlg);
  selector->loadSettings("DeployPanel");
  lay->addWidget(selector);

  auto *bb = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  lay->addWidget(bb);
  connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

  if (dlg.exec() != QDialog::Accepted)
    return;

  selector->saveSettings("DeployPanel");
  if (m_credSelector) {
    m_credSelector->loadSettings("DeployPanel");
  }

  QString user = selector->username();
  QString pass = selector->secret();
  bool isKey = selector->isPrivateKey();

  SshClient::Config baseCfg = selector->getConfig("");

  for (int i = 0; i < ips.size(); ++i) {
    QString ip = ips[i];

    // Dialog exec() sırasında tablo sıralanmış olabilir — stale satır indeksi
    // yerine IP'ye göre güncel satır numarasını taze olarak bul.
    int row = rowForIp(m_unifiedHostTable, ip);
    if (row < 0)
      continue; // IP artık tabloda yok (teorik durum)

    setRowStatus(m_unifiedHostTable, row, tr("SSH Deneniyor"),
                 DeployStatusRank::SshTesting);

    if (!user.isEmpty() && m_session) {
      QString existingId;
      QString possibleMachineId;
      for (const auto &a : OkidiaDatabase::instance().allAgents()) {
        if (a.ip == ip) {
          possibleMachineId = a.machineId;
          break;
        }
      }

      for (const auto &c : m_session->sshCredentials()) {
        if (c.targetType == "agent") {
          if (!possibleMachineId.isEmpty() &&
              c.machineId == possibleMachineId) {
            existingId = c.id;
            break;
          }
          if (c.targetId == ip) {
            existingId = c.id;
            break;
          }
        }
      }

      // Hostname'i güncel satırdan oku (rowForIp ile bulundu)
      QString hostname;
      if (m_unifiedHostTable->item(row, 1))
        hostname = m_unifiedHostTable->item(row, 1)->text();

      SshCredentialRecord rec;
      rec.id = existingId.isEmpty() ? OkidiaUtils::generateUuid() : existingId;
      if (selector->currentCredentialId().isEmpty()) {
        rec.name = "Manuel: " + hostname;
      } else {
        QString tplName = tr("Şablon");
        for (const auto &c : m_session->sshCredentials()) {
          if (c.id == selector->currentCredentialId()) {
            tplName = c.name;
            break;
          }
        }
        rec.name = tr("Şablon (") + tplName + "): " + hostname;
      }
      rec.username = user;
      rec.secret = pass.toUtf8();
      rec.targetId = ip;
      rec.targetType = "agent";
      rec.machineId = possibleMachineId;
      rec.isKey = isKey;
      rec.ownerUserId = m_session->activeUser().id;
      m_session->addSshCredential(rec);
      if (m_unifiedHostTable && m_unifiedHostTable->item(row, 2)) {
        QString curOs = m_unifiedHostTable->item(row, 2)->text().trimmed();
        if (!curOs.isEmpty() && curOs != tr("Bilinmiyor") && curOs != tr("Akıllı Cihaz / Diğer")) {
          m_session->setHostOs(ip, curOs.toLower());
        }
      }
    }

    SshClient::Config cfg = baseCfg;
    cfg.host = ip;

    // Zayıf ağlarda ani bağlantı fırtınasını önlemek için kademeli (staggered) tetikleme
    int delayMs = i * 150;
    QTimer::singleShot(delayMs, this, [this, ip, cfg]() {
      int curRow = rowForIp(m_unifiedHostTable, ip);
      if (curRow >= 0) {
        probeRowViaSsh(curRow, ip, cfg);
      }
    });
  }
}

bool DeployPanel::hasMatchingCredential(const QString &ip,
                                        const QString &hostname) const {
  if (!m_session)
    return false;
  const auto brokerCreds = m_session->sshCredentials();
  const auto connectedAgents = m_session->allClientStates();

  QString machineId, classId;
  for (const auto &agent : connectedAgents) {
    if (agent.ip == ip) {
      machineId = agent.machineId;
      classId = m_session->clientGroup(machineId);
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
    if ((c.targetId == ip ||
         (!machineId.isEmpty() &&
          (c.targetId == machineId || c.machineId == machineId))) &&
        c.targetType == "agent") {
      return true;
    }
  }
  if (!classId.isEmpty()) {
    for (const auto &c : brokerCreds) {
      if (c.targetId == classId && c.targetType == "class") {
        return true;
      }
    }
  }
  return false;
}

void DeployPanel::onUnifiedTableSelectionChanged() {
  QList<QTableWidgetItem *> selected = m_unifiedHostTable->selectedItems();
  QSet<int> rows;
  for (auto *item : selected)
    rows.insert(item->row());

  if (m_floatingBar) {
    m_floatingBar->setVisible(!rows.isEmpty());
    if (m_selectionCountLbl) {
      m_selectionCountLbl->setText(
          QString(tr("%1 cihaz seçildi")).arg(rows.size()));
    }
  }

  // Seçilenlerden kuruluma uygun (Hazır, Kuruldu, Hata vb.) olan var mı?
  bool readyExists = false;
  for (int r : rows) {
    if (auto *st = m_unifiedHostTable->item(r, 4)) { // 4 = Durum
      QString t = st->text();
      if (t.contains(tr("Hazır")) || t.contains("Kuruldu") ||
          t.contains("Hata")) {
        readyExists = true;
        break;
      }
    }
  }

  if (m_installBtn)
    m_installBtn->setEnabled(readyExists);
}

void DeployPanel::onDeselectAll() { m_unifiedHostTable->clearSelection(); }

void DeployPanel::onHostDoubleClicked(QTableWidgetItem *item) {
  if (!item)
    return;
  int row = item->row();
  if (auto *ipItem = m_unifiedHostTable->item(row, 0)) {
    onOpenCredentialDialog({ipItem->text().trimmed()});
  }
}

void DeployPanel::onTableContextMenu(const QPoint &pos) {
  QTableWidgetItem *item = m_unifiedHostTable->itemAt(pos);
  if (!item)
    return;

  int row = item->row();
  auto *statusItem = m_unifiedHostTable->item(row, 4);
  if (!statusItem)
    return;

  QMenu menu(this);

  // Hızlı seçim aksiyonları
  QAction *selectReadyAct = menu.addAction(
      ThemeManager::icon(":/ui/check_circle.svg"), tr("Hazır Cihazları Seç"));
  connect(selectReadyAct, &QAction::triggered, this, [this]() {
    m_unifiedHostTable->clearSelection();
    for (int r = 0; r < m_unifiedHostTable->rowCount(); ++r) {
      if (auto *st = m_unifiedHostTable->item(r, 4)) {
        if (st->data(Qt::UserRole).toInt() ==
            static_cast<int>(DeployStatusRank::Ready)) {
          m_unifiedHostTable->selectRow(r);
        }
      }
    }
  });

  QAction *selectAllAct = menu.addAction(tr("Tümünü Seç"));
  connect(selectAllAct, &QAction::triggered, this,
          [this]() { m_unifiedHostTable->selectAll(); });

  QAction *deselectAct = menu.addAction(tr("Seçimi Temizle"));
  connect(deselectAct, &QAction::triggered, this, &DeployPanel::onDeselectAll);

  menu.addSeparator();

  if (statusItem->data(Qt::UserRole).toInt() ==
      static_cast<int>(DeployStatusRank::Unsupported)) {
    QAction *forceEnableAct =
        menu.addAction(tr("Zorla Etkinleştir (Kuruluma Aç)"));
    connect(forceEnableAct, &QAction::triggered, this, [this, row]() {
      for (int col = 0; col < m_unifiedHostTable->columnCount(); ++col) {
        if (auto *it = m_unifiedHostTable->item(row, col)) {
          it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
          it->setForeground(QBrush()); // Varsayılan renk
        }
      }
      if (auto *it = m_unifiedHostTable->item(row, 2))
        it->setText("...");
      setRowStatus(m_unifiedHostTable, row, tr("Erişim Bekleniyor"),
                   DeployStatusRank::AwaitingCred, QColor(),
                   tr("Kullanıcı tarafından zorla etkinleştirildi."));

      QString ip = m_unifiedHostTable->item(row, 0)->text();
      QString hostname = m_unifiedHostTable->item(row, 1)->text();

      if (hasMatchingCredential(ip, hostname)) {
        setRowStatus(m_unifiedHostTable, row, tr("SSH Deneniyor"),
                     DeployStatusRank::SshTesting);
        SshClient::Config cfg =
            buildConfigForTarget(m_session, ip, hostname, m_credSelector);
        probeRowViaSsh(row, ip, cfg);
      }
    });
  } else {
    QString ip = m_unifiedHostTable->item(row, 0)->text().trimmed();

    QAction *sshCredAct = menu.addAction(tr("SSH Kimlik Bilgisi Ata"));
    connect(sshCredAct, &QAction::triggered, this,
            [this, ip]() { onOpenCredentialDialog({ip}); });

    menu.addSeparator();

    QAction *updateBrokerAct = menu.addAction(
        ThemeManager::icon(":/ui/network.svg"), tr("Broker Adresini Güncelle (L3 / VLAN)"));
    connect(updateBrokerAct, &QAction::triggered, this, &DeployPanel::onUpdateBrokerHostOnSelected);

    menu.addSeparator();

    QAction *installAct = menu.addAction(tr("Bu Cihaza Kurulum Yap"));

    bool canInstall = false;
    if (auto *pkgIt = m_unifiedHostTable->item(row, 5)) {
      QString rowPkgPath = pkgIt->data(Qt::UserRole).toString();
      if (!rowPkgPath.isEmpty() && QFile::exists(rowPkgPath)) {
        canInstall = true;
      }
    }

    QString t = statusItem->text();
    bool isReady =
        t.contains(tr("Hazır")) || t.contains("Kuruldu") || t.contains("Hata");

    if (!canInstall || !isReady) {
      installAct->setEnabled(false);
    }

    connect(installAct, &QAction::triggered, this, [this, row]() {
      m_unifiedHostTable->clearSelection();
      m_unifiedHostTable->selectRow(row);
      onInstallSelected();
    });

    if (m_progressGrid) {
      HostInstallCard *savedCard = m_progressGrid->card(ip);
      if (savedCard) {
        menu.addSeparator();
        QAction *logAct = menu.addAction(
            ThemeManager::icon(":/ui/file.svg"),
            tr("Son Kurulum Günlüğünü İncele"));
        connect(logAct, &QAction::triggered, this, [savedCard]() {
          savedCard->showLogDialog();
        });
      }
    }
  }

  menu.exec(m_unifiedHostTable->viewport()->mapToGlobal(pos));
}

void DeployPanel::onAddSavedHosts() {
  if (!m_session)
    return;
  QList<SshCredentialRecord> allCreds = m_session->sshCredentials();
  QList<SshCredentialRecord> agentCreds;
  for (const auto &cred : allCreds) {
    if (cred.targetType == "agent" && !cred.targetId.isEmpty()) {
      agentCreds.append(cred);
    }
  }

  if (agentCreds.isEmpty()) {
    QMessageBox::information(this, tr("Kayıtlı Cihaz Yok"),
                             tr("Veritabanında 'agent' tipinde kayıtlı hiçbir "
                                "SSH bilgisi bulunamadı."));
    return;
  }

  QDialog dlg(this);
  dlg.setWindowTitle(tr("Tüm Cihazları Ekle"));
  dlg.resize(400, 300);
  auto *lay = new QVBoxLayout(&dlg);
  lay->addWidget(
      new QLabel(tr("Listeye eklemek istediğiniz cihazları seçin:")));

  QTableWidget *table = new QTableWidget(agentCreds.size(), 2, &dlg);
  table->setHorizontalHeaderLabels({tr("Cihaz Adı"), tr("IP Adresi")});
  table->horizontalHeader()->setStretchLastSection(true);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);

  for (int i = 0; i < agentCreds.size(); ++i) {
    QString displayName = agentCreds[i].name;
    int lastColon = displayName.lastIndexOf(": ");
    if (lastColon != -1) {
      displayName = displayName.mid(lastColon + 2).trimmed();
    }

    // Eğer m_session içinde bu IP'ye ait güncel bir ClientState varsa veya kayıtlı adı generic ise, gerçek hostname'i al ve güncelle
    if (m_session) {
      for (const auto &cs : m_session->clientStates()) {
        if (cs.ip == agentCreds[i].targetId && !cs.hostname.isEmpty() &&
            cs.hostname != "Linux PC" && cs.hostname != "Windows PC" && cs.hostname != "Bilinmeyen Cihaz" && cs.hostname != "Gizli Cihaz" &&
            cs.hostname != tr("Bilinmeyen Cihaz") && cs.hostname != tr("Gizli Cihaz")) {
          if (displayName != cs.hostname) {
            displayName = cs.hostname;
            int colonIdx = agentCreds[i].name.lastIndexOf(": ");
            QString prefix = (colonIdx != -1) ? agentCreds[i].name.left(colonIdx + 2) : "Manuel: ";
            agentCreds[i].name = prefix + displayName;
            m_session->addSshCredential(agentCreds[i]);
          }
          break;
        }
      }
    }

    auto *nameItem = new OkidiaTableItem(displayName);
    auto *ipItem = new OkidiaTableItem(agentCreds[i].targetId);
    ipItem->setData(Qt::UserRole, agentCreds[i].targetId);
    bool ok = false;
    quint32 ipv4 = QHostAddress(agentCreds[i].targetId).toIPv4Address(&ok);
    if (ok) {
      ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(ipv4));
    }
    table->setItem(i, 0, nameItem);
    table->setItem(i, 1, ipItem);
  }
  table->setSortingEnabled(true);
  table->horizontalHeader()->setSortIndicatorShown(true);
  table->horizontalHeader()->setSectionsClickable(true);
  table->sortByColumn(0, Qt::AscendingOrder);
  lay->addWidget(table);

  auto *btnLay = new QHBoxLayout();
  auto *okBtn = new QPushButton(tr("Seçilenleri Ekle"), &dlg);
  auto *cancelBtn = new QPushButton(tr("İptal"), &dlg);
  btnLay->addStretch();
  btnLay->addWidget(cancelBtn);
  btnLay->addWidget(okBtn);
  lay->addLayout(btnLay);

  connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
  connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

  if (dlg.exec() == QDialog::Accepted) {
    QList<QTableWidgetItem *> selectedItems = table->selectedItems();
    QSet<int> rows;
    for (auto *item : selectedItems) {
      rows.insert(item->row());
    }

    for (int r : rows) {
      QString ip = table->item(r, 1)->data(Qt::UserRole).toString();
      // Aynı IP listede var mı kontrol et
      bool exists = false;
      for (int i = 0; i < m_unifiedHostTable->rowCount(); ++i) {
        if (m_unifiedHostTable->item(i, 0)->text() == ip) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        onHostFound(ip);
        onHostUpdated(ip, table->item(r, 0)->text());
      }
    }
  }
}

void DeployPanel::onUpdateBrokerHostOnSelected() {
  if (!m_unifiedHostTable) return;

  QSet<int> selectedRows;
  for (auto *it : m_unifiedHostTable->selectedItems()) {
    selectedRows.insert(it->row());
  }
  if (selectedRows.isEmpty() && m_unifiedHostTable->currentRow() >= 0) {
    selectedRows.insert(m_unifiedHostTable->currentRow());
  }

  if (selectedRows.isEmpty()) {
    QMessageBox::information(this, tr("Seçim Yapılmadı"), tr("Lütfen Broker adresi güncellenecek cihazları seçin."));
    return;
  }

  QStringList candidateIps;
  int brokerPort = 11200;
  if (m_session) {
    if (m_session->tlsClient()) {
      brokerPort = m_session->tlsClient()->port();
      const QString host = m_session->tlsClient()->host();
      if (!host.isEmpty() && host != "127.0.0.1" && host != "localhost") {
        candidateIps.append(host);
      }
    }
    for (const QString &bIp : m_session->brokerAllIps()) {
      if (!candidateIps.contains(bIp) && bIp != "127.0.0.1" && bIp != "localhost") {
        candidateIps.append(bIp);
      }
    }
  }

  if (candidateIps.isEmpty()) {
    QMessageBox::warning(this, tr("Hata"), tr("Aktif Broker IP adresi bulunamadı."));
    return;
  }

  appendLog(tr("Seçili %1 cihazda Broker adresi (L3/VLAN) güncelleme başlatılıyor...").arg(selectedRows.size()));

  QPointer<QTableWidget> tableGuard = m_unifiedHostTable;
  QPointer<DeployPanel> selfGuard = this;

  for (int row : selectedRows) {
    const QString ip = m_unifiedHostTable->item(row, 0)->text().trimmed();
    const QString hostname = m_unifiedHostTable->item(row, 1) ? m_unifiedHostTable->item(row, 1)->text().trimmed() : QString();
    const QString osInfo = m_unifiedHostTable->item(row, 2) ? m_unifiedHostTable->item(row, 2)->text().trimmed() : QString();

    SshClient::Config cfg = buildConfigForTarget(m_session, ip, hostname, m_credSelector);
    if (cfg.username.isEmpty()) {
      appendLog(tr("UYARI [%1]: SSH kimlik bilgisi atanmadığı için atlandı.").arg(ip));
      continue;
    }

    QStringList targetCandidateIps = candidateIps;
    const QString targetPrefix = ip.section('.', 0, 2) + ".";
    for (int i = 0; i < targetCandidateIps.size(); ++i) {
      if (targetCandidateIps[i].startsWith(targetPrefix)) {
        if (i > 0) targetCandidateIps.swapItemsAt(0, i);
        break;
      }
    }

    setRowStatus(m_unifiedHostTable, row, tr("Broker Güncelleniyor..."), DeployStatusRank::Installing);

    std::ignore = QtConcurrent::run(probeThreadPool(), [tableGuard, selfGuard, row, ip, osInfo, cfg, targetCandidateIps, brokerPort]() {
      SshClient client;
      client.connectToHost(cfg);
      if (!client.isConnected()) {
        QMetaObject::invokeMethod(qApp, [tableGuard, selfGuard, row, ip, err = client.lastError()]() {
          if (tableGuard) setRowStatus(tableGuard, row, tr("SSH Hatası"), DeployStatusRank::Failed, QColor(), err);
          if (selfGuard) selfGuard->appendLog(tr("HATA [%1]: SSH bağlantısı kurulamadı: %2").arg(ip, err));
        });
        return;
      }

      const bool isWindows = osInfo.contains("Windows", Qt::CaseInsensitive);
      QString foundIp;

      if (isWindows) {
        QStringList quotedIps;
        for (const QString &cip : targetCandidateIps) quotedIps << QString("'%1'").arg(cip);
        QString psProbeCmd = QString(
            "powershell -Command \""
            "$port = %1; $ips = @(%2); $found = ''; "
            "foreach ($ip in $ips) { "
            "  try { "
            "    $c = New-Object System.Net.Sockets.TcpClient; "
            "    $a = $c.BeginConnect($ip, $port, $null, $null); "
            "    if ($a.AsyncWaitHandle.WaitOne(1000, $false) -and $c.Connected) { "
            "      $c.EndConnect($a); $c.Close(); $found = $ip; break; "
            "    } "
            "    $c.Close(); "
            "  } catch {} "
            "} "
            "if ($found -ne '') { "
            "  $hosts = \\\"$env:SystemRoot\\System32\\drivers\\etc\\hosts\\\"; "
            "  $lines = (Get-Content $hosts -ErrorAction SilentlyContinue) | Where-Object { $_ -notmatch 'okidia-broker' }; "
            "  $lines += \\\"$found`tokidia-broker\\\"; "
            "  Set-Content -Path $hosts -Value $lines -Force; "
            "  Restart-Service OkidiaAgent -ErrorAction SilentlyContinue; "
            "  Write-Output $found; "
            "}\""
        ).arg(brokerPort).arg(quotedIps.join(", "));
        foundIp = client.executeSync(psProbeCmd, 15000).trimmed();
      } else {
        QString escPass = cfg.password;
        escPass.replace("'", "'\\''");
        QString probeScript = QString(
            "for ip in %1; do "
            "  if timeout 1 bash -c \"</dev/tcp/$ip/%2\" 2>/dev/null; then "
            "    echo $ip; break; "
            "  fi; "
            "done"
        ).arg(targetCandidateIps.join(" ")).arg(brokerPort);

        foundIp = client.executeSync(probeScript, 10000).trimmed();
        if (!foundIp.isEmpty()) {
          QString updateCmd;
          if (cfg.username == "root") {
            updateCmd = QString("sed -i '/okidia-broker/d' /etc/hosts && echo '%1 okidia-broker' >> /etc/hosts && systemctl restart okidia-agent.service").arg(foundIp);
          } else {
            updateCmd = QString("echo '%1' | sudo -S sh -c \"sed -i '/okidia-broker/d' /etc/hosts && echo '%2 okidia-broker' >> /etc/hosts && systemctl restart okidia-agent.service\"").arg(escPass, foundIp);
          }
          client.executeSync(updateCmd, 12000);
        }
      }

      QMetaObject::invokeMethod(qApp, [tableGuard, selfGuard, row, ip, foundIp]() {
        if (!foundIp.isEmpty()) {
          if (tableGuard) setRowStatus(tableGuard, row, tr("Broker Güncellendi"), DeployStatusRank::Ready, QColor(), foundIp);
          if (selfGuard) selfGuard->appendLog(tr("BAŞARILI [%1]: Broker adresi doğrulandı ve güncellendi → %2").arg(ip, foundIp));
        } else {
          if (tableGuard) setRowStatus(tableGuard, row, tr("Erişilemez"), DeployStatusRank::Failed, QColor(), tr("Broker portuna ulaşılamadı"));
          if (selfGuard) selfGuard->appendLog(tr("UYARI [%1]: Broker portuna (11200) ulaşılamadı. Okul ağ/VLAN yönlendirmesini kontrol edin.").arg(ip));
        }
      });
    });
  }
}
