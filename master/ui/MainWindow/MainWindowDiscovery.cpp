// master/ui/MainWindow/MainWindowDiscovery.cpp
#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "../UserManagementPanel/UserManagementPanel.h"
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QHostAddress>
#include <QTableWidget>
#include <QHeaderView>
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../../common/ui/ThemeManager.h"
#include "../AgentTypeSelectionDialog/AgentTypeSelectionDialog.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../ssh/OtaUpdatePanel.h"
#include "../../ssh/DeployPanel.h"
#include <QScrollBar>
// ============================================================================
void MainWindow::setupDiscoveryPanel() {
    // ── Kayan Bildirim Rozeti (Floating Discovery Pill) ────────────────────
    m_floatingDiscoveryPill = new QPushButton(ui->thumbnailScrollArea);
    m_floatingDiscoveryPill->setObjectName("floatingDiscoveryPill");
    m_floatingDiscoveryPill->setIcon(ThemeManager::icon(":/ui/discovery.svg"));
    m_floatingDiscoveryPill->setIconSize(QSize(18, 18));
    m_floatingDiscoveryPill->setCursor(Qt::PointingHandCursor);
    m_floatingDiscoveryPill->setStyleSheet(
        "QPushButton#floatingDiscoveryPill {"
        "    background-color: #2e7d32;"
        "    color: #ffffff;"
        "    border: 1px solid #1b5e20;"
        "    border-radius: 18px;"
        "    padding: 6px 14px;"
        "    font-weight: bold;"
        "    font-size: 13px;"
        "}"
        "QPushButton#floatingDiscoveryPill:hover {"
        "    background-color: #388e3c;"
        "}"
        "QPushButton#floatingDiscoveryPill:pressed {"
        "    background-color: #1b5e20;"
        "}"
    );
    m_floatingDiscoveryPill->hide();
    connect(m_floatingDiscoveryPill, &QPushButton::clicked, this, &MainWindow::showDiscoveryPanel);

    // ── Yeni İstemci İstekleri Paneli ─────────────────────────────────────
    m_discoveryPanel = new QDialog(this);
    m_discoveryPanel->setWindowTitle(tr("Yeni İstemci İstekleri"));
    m_discoveryPanel->setWindowIcon(ThemeManager::icon(":/ui/discovery.svg"));
    m_discoveryPanel->resize(840, 440);
    m_discoveryPanel->setMinimumSize(720, 360);
    m_discoveryPanel->setAttribute(Qt::WA_DeleteOnClose, false);

    auto* lay = new QVBoxLayout(m_discoveryPanel);
    lay->setContentsMargins(16, 14, 16, 14);
    lay->setSpacing(10);

    auto* lbl = new QLabel(
        tr("Aşağıdaki bilgisayarlar ağa katılmak için onay bekliyor.\n"
           "Tek tek veya çoklu seçim yaparak onaylayabilir ya da reddedebilirsiniz."),
        m_discoveryPanel);
    lbl->setProperty("status", "info");
    lay->addWidget(lbl);

    // Çoklu seçim destekli Okidia tablosu
    m_discoveryTable = new QTableWidget(0, 6, m_discoveryPanel);
    m_discoveryTable->setHorizontalHeaderLabels({
        tr("Bilgisayar Adı"),
        tr("IP Adresi"),
        tr("Kullanıcı"),
        tr("İşletim Sistemi"),
        tr("MAC Adresi"),
        tr("Sürüm")
    });
    m_discoveryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_discoveryTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_discoveryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_discoveryTable->setAlternatingRowColors(true);
    m_discoveryTable->verticalHeader()->setVisible(false);
    m_discoveryTable->setShowGrid(false);
    OkidiaUtils::setupTableHeaders(m_discoveryTable, "discovery_requests_table", {150, 120, 100, 160, 140, 80}, true);
    m_discoveryTable->setSortingEnabled(true);
    m_discoveryTable->sortByColumn(0, Qt::AscendingOrder);
    lay->addWidget(m_discoveryTable, 1);

    // Seçime bağlı butonlar — liste seçimi yokken disabled
    auto* btnAppAll  = new QPushButton(tr("Tümünü Onayla"), m_discoveryPanel);
    btnAppAll->setIcon(ThemeManager::icon(":/ui/checked.svg"));

    auto* btnReject  = new QPushButton(tr("Reddet"),         m_discoveryPanel);
    btnReject->setIcon(ThemeManager::icon(":/ui/clear.svg"));

    auto* btnIgnore  = new QPushButton(tr("Yoksay"),        m_discoveryPanel);
    btnIgnore->setIcon(ThemeManager::icon(":/ui/forbidden.svg"));
    btnIgnore->setToolTip(tr("İstemciyi reddeder ve kalıcı kara listeye ekler"));

    auto* btnApprove = new QPushButton(tr("Onayla"),        m_discoveryPanel);
    btnApprove->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
    btnApprove->setProperty("class", "primary");

    auto* btnClose   = new QPushButton(tr("Kapat"),         m_discoveryPanel);

    // Başlangıçta seçim yok → disabled
    btnApprove->setEnabled(false);
    btnReject ->setEnabled(false);
    btnIgnore ->setEnabled(false);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);
    btnRow->addWidget(btnAppAll);
    btnRow->addStretch();
    btnRow->addWidget(btnReject);
    btnRow->addWidget(btnIgnore);
    btnRow->addWidget(btnApprove);
    btnRow->addWidget(btnClose);
    lay->addLayout(btnRow);

    connect(btnClose, &QPushButton::clicked, m_discoveryPanel, &QDialog::hide);

    // Seçim değişince butonları güncelle
    connect(m_discoveryTable, &QTableWidget::itemSelectionChanged,
            [this, btnApprove, btnReject, btnIgnore]() {
        const bool hasSel = !m_discoveryTable->selectionModel()->selectedRows().isEmpty();
        btnApprove->setEnabled(hasSel);
        btnReject ->setEnabled(hasSel);
        btnIgnore ->setEnabled(hasSel);
    });

    // Çift tıklayınca onayla
    connect(m_discoveryTable, &QTableWidget::itemDoubleClicked, [btnApprove](QTableWidgetItem*) {
        if (btnApprove->isEnabled()) {
            btnApprove->click();
        }
    });

    // Onayla — seçili tüm öğeler
    connect(btnApprove, &QPushButton::clicked, [this]() {
        const auto selectedIndices = m_discoveryTable->selectionModel()->selectedRows();
        if (selectedIndices.isEmpty()) return;

        QList<DiscoveryAgentInfo> agents;
        for (const auto& idx : selectedIndices) {
            int r = idx.row();
            auto* item0 = m_discoveryTable->item(r, 0);
            if (!item0) continue;
            DiscoveryAgentInfo info;
            info.targetId = item0->data(Qt::UserRole).toString();
            info.csr = item0->data(Qt::UserRole + 1).toByteArray();
            info.hostname = item0->text().trimmed();
            info.originalItem = item0;
            agents.append(info);
        }
        
        AgentTypeSelectionDialog dlg(agents, this);
        if (dlg.exec() != QDialog::Accepted) return;
        
        uint32_t agentType = dlg.selectedAgentType();
        auto checked = dlg.checkedAgents();
        
        for (const auto& agent : checked) {
            if (m_session) m_session->approveDiscovery(agent.targetId, agent.csr, agentType);
            m_pendingCsrMap.remove(agent.targetId);
            AuditLog::instance().recordCommand({}, agent.targetId, tr("İstemci onaylandı (Tip: ") + QString::number(agentType) + ")");
            showCommandStatus(true, tr("Onaylandı: ") + agent.targetId);
            
            for (int r = m_discoveryTable->rowCount() - 1; r >= 0; --r) {
                if (m_discoveryTable->item(r, 0) && m_discoveryTable->item(r, 0)->data(Qt::UserRole).toString() == agent.targetId) {
                    m_discoveryTable->removeRow(r);
                    break;
                }
            }
        }
        updateDiscoveryBadge();
    });

    // Tümünü onayla
    connect(btnAppAll, &QPushButton::clicked, [this]() {
        if (m_discoveryTable->rowCount() == 0) return;

        QStringList types = {tr("Öğrenci PC"), tr("İdari PC (Gizlilik)"), tr("Akıllı Tahta")};
        QList<uint32_t> typeIds = {1, 2, 3};

        const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
        for (const auto& cp : customProfiles) {
            types.append(cp.name);
            typeIds.append(static_cast<uint32_t>(cp.id));
        }

        bool ok;
        QString selected = QInputDialog::getItem(m_discoveryPanel, tr("Tümünü Onayla"),
                                                 tr("Tüm istemciler için ortak tip seçin:"),
                                                 types, 0, false, &ok);
        if (!ok) return;
        int selIdx = types.indexOf(selected);
        if (selIdx < 0) return;
        uint32_t agentType = typeIds[selIdx];

        for (int r = 0; r < m_discoveryTable->rowCount(); ++r) {
            auto* item0 = m_discoveryTable->item(r, 0);
            if (!item0) continue;
            QString targetId = item0->data(Qt::UserRole).toString();
            QByteArray csr = item0->data(Qt::UserRole + 1).toByteArray();
            if (m_session) m_session->approveDiscovery(targetId, csr, agentType);
            m_pendingCsrMap.remove(targetId);
            AuditLog::instance().recordCommand({}, targetId, tr("İstemci onaylandı (Tip: ") + QString::number(agentType) + ")");
        }
        m_discoveryTable->setRowCount(0);
        updateDiscoveryBadge();
        showCommandStatus(true, tr("Tüm istemciler onaylandı (") + selected + ")");
    });

    // Reddet — geçici, kara listeye almaz
    connect(btnReject, &QPushButton::clicked, [this]() {
        const auto selectedIndices = m_discoveryTable->selectionModel()->selectedRows();
        QStringList toReject;
        for (const auto& idx : selectedIndices) {
            if (auto* item = m_discoveryTable->item(idx.row(), 0)) {
                toReject.append(item->data(Qt::UserRole).toString());
            }
        }
        for (const auto& targetId : toReject) {
            onDiscoveryReject(targetId);
            for (int r = m_discoveryTable->rowCount() - 1; r >= 0; --r) {
                if (m_discoveryTable->item(r, 0) && m_discoveryTable->item(r, 0)->data(Qt::UserRole).toString() == targetId) {
                    m_discoveryTable->removeRow(r);
                    break;
                }
            }
        }
        updateDiscoveryBadge();
    });

    // Yoksay — kalıcı kara liste
    connect(btnIgnore, &QPushButton::clicked, [this]() {
        const auto selectedIndices = m_discoveryTable->selectionModel()->selectedRows();
        QList<QPair<QString, QString>> toIgnore;
        for (const auto& idx : selectedIndices) {
            if (auto* item = m_discoveryTable->item(idx.row(), 0)) {
                toIgnore.append({item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 2).toString()});
            }
        }
        for (const auto& pair : toIgnore) {
            onDiscoveryIgnore(pair.first, pair.second);
            for (int r = m_discoveryTable->rowCount() - 1; r >= 0; --r) {
                if (m_discoveryTable->item(r, 0) && m_discoveryTable->item(r, 0)->data(Qt::UserRole).toString() == pair.first) {
                    m_discoveryTable->removeRow(r);
                    break;
                }
            }
        }
        updateDiscoveryBadge();
    });

}

void MainWindow::showDiscoveryPanel() {
    if (!m_discoveryPanel) return;
    m_discoveryPanel->show();
    m_discoveryPanel->raise();
    m_discoveryPanel->activateWindow();
    const QPoint center = this->geometry().center();
    m_discoveryPanel->move(
        center.x() - m_discoveryPanel->width()  / 2,
        center.y() - m_discoveryPanel->height() / 2);
}

void MainWindow::updateFloatingDiscoveryPillGeometry() {
    if (!m_floatingDiscoveryPill || !ui->thumbnailScrollArea) return;
    if (!m_floatingDiscoveryPill->isVisible()) return;

    m_floatingDiscoveryPill->adjustSize();
    const int pillW = m_floatingDiscoveryPill->width();
    const int pillH = m_floatingDiscoveryPill->height();
    const int margin = 20;

    int rightMargin = margin;
    int bottomMargin = margin;

    if (auto* vsb = ui->thumbnailScrollArea->verticalScrollBar()) {
        if (vsb->isVisible()) {
            rightMargin += vsb->width();
        }
    }
    if (auto* hsb = ui->thumbnailScrollArea->horizontalScrollBar()) {
        if (hsb->isVisible()) {
            bottomMargin += hsb->height();
        }
    }

    const int x = ui->thumbnailScrollArea->width() - pillW - rightMargin;
    const int y = ui->thumbnailScrollArea->height() - pillH - bottomMargin;
    m_floatingDiscoveryPill->move(qMax(10, x), qMax(10, y));
    m_floatingDiscoveryPill->raise();
}


void MainWindow::onDiscoveryRequest(const QString& targetId,
                                     const okidia::DiscoveryRequest& req) {
    // Gözlemci (GM/OBSERVER) yeni istemci isteklerini asla görmemelidir
    if (isObserver()) return;

    if (!m_discoveryTable || !m_discoveryPanel) {
        return; // Güvenlik kontrolü: Arayüz elemanları henüz yoksa çökmesini engelle
    }

    const QString machineId = QString::fromStdString(req.machine_id());
    if (machineId.isEmpty() && targetId.isEmpty()) return;

    // Zaten tabloda var mı?
    for (int i = 0; i < m_discoveryTable->rowCount(); ++i) {
        if (m_discoveryTable->item(i, 0) && m_discoveryTable->item(i, 0)->data(Qt::UserRole).toString() == targetId)
            return;
    }

    // CSR sakla
    m_pendingCsrMap[targetId] = QByteArray::fromStdString(req.csr_pem());

    QString osInfo = QString::fromStdString(req.os_info());
    if (osInfo.contains("Raspberry", Qt::CaseInsensitive) || osInfo.contains("Raspbian", Qt::CaseInsensitive)) {
        if (osInfo.contains("Raspbian GNU/Linux", Qt::CaseInsensitive)) {
            osInfo.replace("Raspbian GNU/Linux", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (osInfo.contains("Raspbian", Qt::CaseInsensitive)) {
            osInfo.replace("Raspbian", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (osInfo.contains("Debian GNU/Linux", Qt::CaseInsensitive)) {
            osInfo.replace("Debian GNU/Linux", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (osInfo.contains("Debian", Qt::CaseInsensitive)) {
            osInfo.replace("Debian", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (!osInfo.contains("Raspberry Pi OS", Qt::CaseInsensitive)) {
            osInfo = "Raspberry Pi OS (" + osInfo + ")";
        }
    } else {
        if (osInfo.contains("Debian GNU/Linux", Qt::CaseInsensitive)) {
            osInfo.replace("Debian GNU/Linux", "Debian", Qt::CaseInsensitive);
        }
    }

    int r = m_discoveryTable->rowCount();
    m_discoveryTable->insertRow(r);

    QString hostStr = QString::fromStdString(req.hostname());
    auto* hostItem = new OkidiaTableItem(ThemeManager::icon(":/ui/monitor.svg"), hostStr);
    hostItem->setData(Qt::UserRole, targetId);
    hostItem->setData(Qt::UserRole + 1, m_pendingCsrMap[targetId]);
    hostItem->setData(Qt::UserRole + 2, machineId);
    hostItem->setToolTip("Machine-ID: " + machineId);

    auto* ipItem = new OkidiaTableItem(targetId);
    QHostAddress addr(targetId);
    if (!addr.isNull() && addr.protocol() == QAbstractSocket::IPv4Protocol) {
        ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(addr.toIPv4Address()));
    }

    QString userStr = QString::fromStdString(req.username());
    auto* userItem = new OkidiaTableItem(userStr.isEmpty() ? "-" : userStr);

    QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
    if (osInfo.contains("win", Qt::CaseInsensitive)) {
        osIcon = ThemeManager::icon(":/ui/windows.svg");
    } else if (osInfo.contains("linux", Qt::CaseInsensitive) || osInfo.contains("ubuntu", Qt::CaseInsensitive) ||
               osInfo.contains("debian", Qt::CaseInsensitive) || osInfo.contains("pardus", Qt::CaseInsensitive) ||
               osInfo.contains("raspberry", Qt::CaseInsensitive)) {
        osIcon = ThemeManager::icon(":/ui/linux.svg");
    }
    auto* osItem = new OkidiaTableItem(osIcon, osInfo.isEmpty() ? "-" : osInfo);

    QString macStr = QString::fromStdString(req.mac_address());
    auto* macItem = new OkidiaTableItem(macStr.isEmpty() ? "-" : macStr);

    QString verStr = QString::fromStdString(req.agent_version());
    auto* verItem = new OkidiaTableItem(verStr.isEmpty() ? "-" : verStr);

    m_discoveryTable->setItem(r, 0, hostItem);
    m_discoveryTable->setItem(r, 1, ipItem);
    m_discoveryTable->setItem(r, 2, userItem);
    m_discoveryTable->setItem(r, 3, osItem);
    m_discoveryTable->setItem(r, 4, macItem);
    m_discoveryTable->setItem(r, 5, verItem);

    updateDiscoveryBadge();
    showCommandStatus(true,
        tr("Yeni bağlantı isteği: ") + hostStr);
}


void MainWindow::onDiscoveryRevoked(const QString& targetId) {
    if (!m_discoveryTable || targetId.isEmpty()) return;
    
    for (int i = 0; i < m_discoveryTable->rowCount(); ++i) {
        if (m_discoveryTable->item(i, 0) && m_discoveryTable->item(i, 0)->data(Qt::UserRole).toString() == targetId) {
            m_discoveryTable->removeRow(i);
            m_pendingCsrMap.remove(targetId);
            updateDiscoveryBadge();
            break;
        }
    }
}

void MainWindow::onDiscoveryResolved(const QString& targetId,
                                     const QString& resolvedBy,
                                     const QString& action,
                                     uint32_t agentType,
                                     const QString& hostname) {
    if (!m_discoveryTable || targetId.isEmpty()) return;

    // Hedef istemciyi tablodan ve CSR önbelleğinden kaldır
    for (int i = 0; i < m_discoveryTable->rowCount(); ++i) {
        auto* item0 = m_discoveryTable->item(i, 0);
        if (item0 && (item0->data(Qt::UserRole).toString() == targetId ||
                      item0->data(Qt::UserRole + 2).toString() == targetId)) {
            m_discoveryTable->removeRow(i);
            break;
        }
    }
    m_pendingCsrMap.remove(targetId);
    updateDiscoveryBadge();

    // Başka bir Master tarafından onaylanmışsa ve biz YM isek bilgilendir (KVKK ve yetki takibi)
    if (!resolvedBy.isEmpty() && resolvedBy != m_activeUser.username && action == "approve") {
        QString typeName = (agentType == 1) ? tr("Öğrenci PC")
                         : (agentType == 2) ? tr("İdari PC")
                         : (agentType == 3) ? tr("Akıllı Tahta")
                         : (agentType >= 100) ? OkidiaDatabase::instance().customProfile(agentType).name
                         : tr("Standart PC");
        if (typeName.isEmpty()) typeName = tr("Özel Profil (%1)").arg(agentType);
        QString hostDisp = hostname.isEmpty() ? targetId : hostname;
        
        QString notifMsg = QString(tr("%1 bilgisayarı %2 tarafından '%3' olarak onaylandı."))
                               .arg(hostDisp, resolvedBy, typeName);

        showCommandStatus(true, notifMsg);
        AuditLog::instance().recordSecurity(resolvedBy, targetId, notifMsg);
    }
}

void MainWindow::onDiscoveryReject(const QString& targetId) {
    if (m_session) m_session->rejectDiscovery(targetId);
    m_pendingCsrMap.remove(targetId);
    AuditLog::instance().recordCommand({}, targetId, tr("İstemci reddedildi"));
    showCommandStatus(true, tr("Reddedildi: ") + targetId);
}

void MainWindow::onDiscoveryIgnore(const QString& targetId,
                                    const QString& machineId) {
    if (m_session) m_session->ignoreDiscovery(targetId, machineId);
    m_pendingCsrMap.remove(targetId);
    AuditLog::instance().recordCommand({}, targetId,
        tr("İstemci kara listeye alındı → ") + machineId);
    showCommandStatus(true, tr("Yoksayıldı: ") + targetId);
}

void MainWindow::updateDiscoveryBadge() {
    const int count = m_discoveryTable ? m_discoveryTable->rowCount() : 0;

    if (m_deployPanel) {
        m_deployPanel->updateDiscoveryCount(count);
    }

    if (m_floatingDiscoveryPill) {
        if (count > 0 && !isObserver()) {
            m_floatingDiscoveryPill->setText(QString(tr("%1 Yeni İstemci Onay Bekliyor")).arg(count));
            m_floatingDiscoveryPill->show();
            updateFloatingDiscoveryPillGeometry();
        } else {
            m_floatingDiscoveryPill->hide();
        }
    }

    if (count == 0 && m_discoveryPanel)
        m_discoveryPanel->hide();
}

void MainWindow::updateWindowTitle() {
    const QString roleStr = (m_activeUser.role == "YM") ? tr("Yönetici")
                          : (m_activeUser.role == "OBSERVER") ? tr("Gözlemci")
                          : tr("Öğretmen");
    const QString verStr = qApp->applicationVersion().isEmpty() ? "" : " - v" + qApp->applicationVersion();
    const QString nameStr = m_activeUser.displayName.isEmpty() ? m_activeUser.username : m_activeUser.displayName;

    QString schoolStr;
    if (m_session && !m_session->schoolName().isEmpty()) {
        schoolStr = m_session->schoolName();
    } else if (OkidiaDatabase::instance().isOpen()) {
        schoolStr = OkidiaDatabase::instance().metaValue("school_name").trimmed();
    }
    if (schoolStr.isEmpty()) {
        QSettings sApp("okidia", "master");
        schoolStr = sApp.value("school/name", "").toString().trimmed();
    }

    if (!schoolStr.isEmpty()) {
        setWindowTitle(QString("OKIDIA%1 — %2 | %3 (%4)").arg(verStr, schoolStr, nameStr, roleStr));
    } else {
        setWindowTitle(QString("OKIDIA%1 - %2 (%3)").arg(verStr, nameStr, roleStr));
    }
}

void MainWindow::setActiveUser(const UserRecord& user) {
    m_activeUser = user;

    // SessionManager'a bildir — yetki önbelleği yüklenir
    if (m_session) m_session->setActiveUser(user);

    // AuditLog'a kullanıcıyı bildir
    AuditLog::instance().setActiveUser(user.id, user.username);

    // RoomManager ve PrivateChatManager'a kullanıcı bilgisini bildir (mesaj gönderiminde actor)
    if (m_roomMgr) {
        m_roomMgr->setMasterInfo(user.username, user.displayName, user.role);
        QTimer::singleShot(50, this, &MainWindow::refreshAllTreeLockEmojis);
        if (m_chatWindow) {
            m_chatWindow->buildRoomTree();
        }
    }
    if (m_privateChatMgr) {
        m_privateChatMgr->setMasterInfo(user.username, user.displayName, user.role);
    }

    // TreeManager'a kullanıcı id'sini bildir (addNode'da createdBy için)
    if (m_treeManager) {
        m_treeManager->setActiveUserId(user.id);
        m_treeManager->reloadTree(); // Otomatik DB senkronizasyonunun yarış durumunu (race condition) önlemek için sınıf yapısını yeniden yükle
        // YM ise callback null (hepsi serbest), OM ise SessionManager kontrolü
        if (user.role == "YM") {
            m_treeManager->setPermissionChecker(nullptr);
            m_treeManager->setClassPermissionChecker(nullptr);
        } else {
            m_treeManager->setPermissionChecker(
                [this](const QString& clientId) {
                    return m_session && m_session->canManageClient(clientId);
                });
            m_treeManager->setClassPermissionChecker(
                [this](const QString& classId) {
                    return m_session && m_session->canManageClass(classId);
                });
        }
    }

    // UserManagementPanel'e bildir — butonlar rol'e göre ayarlanır
    if (m_userMgmtPanel) m_userMgmtPanel->setActiveUser(user);

    // Ana menüyü aktif kullanıcının rolüne göre yeniden oluştur
    setupMainMenu();

    // Sistem & Ağ Yönetimi sekmelerini aktif kullanıcının rolüne göre yeniden oluştur (YM: Deploy + OTA + SSH, OM: SSH, GM: Gizli)
    setupSysMgmtTabs();

    // Toolbar butonlarının görünürlüğünü ve erişilebilirliğini aktif role göre ayarla
    // (OBSERVER: Kilitle/Yayın/Sınav/Dosya/Zamanlı/Güç/İstemciler gizlenir; Mesaj/Anons/Sohbet/Rapor görünür)
    bool isCurrentlyOffline = m_session && m_session->isOfflineMode();
    applyOfflineModeConstraints(isCurrentlyOffline);

    if (m_otaUpdatePanel) {
        m_otaUpdatePanel->refreshAgentList();
    }

    // Broker sürümü Master'dan eski ise ve kullanıcı YM ise uyarı penceresini tetikle
    if (m_session && m_session->isBrokerOutdated() && user.role == "YM") {
        QString masterVer = Okidia::Version::string();
        onBrokerOutdatedWarning(m_session->brokerVersion(), masterVer);
    }

    // Windows istemci kaldırma parolası varsayılan ('www.okidia.net') değerdeyse, kullanıcı YM ise ve çevrimdışı modda değilse ısrarla uyar
    if (user.role == "YM" && (!m_session || !m_session->isOfflineMode())) {
        QTimer::singleShot(800, this, &MainWindow::checkDefaultUninstallPasswordWarning);
    }

    // YM değilse Kullanıcı Yönetimi menüsünü gizle
    if (ui->actionKullaniciYonetimi)
        ui->actionKullaniciYonetimi->setVisible(user.role == "YM");

    // Pencere başlığına versiyon, okul adı, kullanıcı adı ve rol ekle
    updateWindowTitle();

    // OM veya OBSERVER ise tree'yi ve thumbnail grid'ini yetki durumuna göre güncelle
    if (user.role != "YM") {
        applyOMPermissions();
    } else {
        // YM: tüm node'lar ve thumbnail'lar görünür, toolbar aktif
        if (m_treeManager) {
            // Tüm node'ları normal görünüme döndür
            m_treeManager->applyPermissionView({});  // boş map = hepsini resetle
        }
        updateThumbnailVisibility();
    }

    // ── Kullanıcıya Özel Arayüz Ayarlarını Yükle ve Uygula ──────────
    if (!user.username.isEmpty()) {
        QSettings s("okidia", "master");
        
        // 1. Tema
        int themeIdx = s.value("users/" + user.username + "/ui/theme", 0).toInt();
        ThemeManager::instance().applyTheme(themeIdx);

        // 2. FPS Hızı ve Duraklatma Durumu
        int savedInterval = s.value("users/" + user.username + "/ui/fps_interval", 1000).toInt();
        bool savedPaused  = s.value("users/" + user.username + "/ui/fps_paused", false).toBool();
        int defaultIndex  = 3; // 1 FPS fallback (data: 1000)
        for (int i = 0; i < ui->fpsCombo->count(); ++i) {
            if (ui->fpsCombo->itemData(i).toInt() == savedInterval) {
                defaultIndex = i;
                break;
            }
        }
        if (savedPaused) {
            ui->fpsCombo->setCurrentIndex(0);
            if (ui->btnStreamToggle) {
                ui->btnStreamToggle->setIcon(ThemeManager::icon(":/ui/play.svg"));
                ui->btnStreamToggle->setToolTip(tr("Ekran Akışını Başlat"));
            }
            if (m_session) m_session->setStreamPaused(true);
        } else {
            ui->fpsCombo->setCurrentIndex(defaultIndex);
            if (ui->btnStreamToggle) {
                ui->btnStreamToggle->setIcon(ThemeManager::icon(":/ui/pause.svg"));
                ui->btnStreamToggle->setToolTip(tr("Ekran Akışını Duraklat"));
            }
            if (m_session) m_session->setUserFpsInterval(savedInterval);
        }

        // 3. Sınıf Görünüm Filtresi (Yalnızca OM için görünür; YM ve Gözlemci tüm sınıflara bakar)
        if (ui->classFilterCombo) {
            const bool isOM = (user.role == "OM");
            ui->classFilterCombo->setVisible(isOM);
            if (isOM) {
                int defaultFilter = 1; // OM varsayılan: Kendi Sınıflarım (1)
                int filterIdx = s.value("users/" + user.username + "/ui/classFilterMode", defaultFilter).toInt();
                if (filterIdx < 0 || filterIdx >= ui->classFilterCombo->count()) filterIdx = defaultFilter;
                ui->classFilterCombo->setCurrentIndex(filterIdx);
                onClassFilterChanged(filterIdx);
            } else {
                if (m_treeManager) {
                    m_treeManager->setOwnershipFilter(false, {});
                }
            }
        }

        // 4. Tree Görünüm Modu (Sınıf / Ağ)
        int mode = s.value("users/" + user.username + "/ui/treeViewMode", 0).toInt();
        if (m_treeManager) {
            auto* btnClass = findChild<QPushButton*>("btnClass");
            auto* btnSubnet = findChild<QPushButton*>("btnSubnet");
            if (mode == 1) {
                m_treeManager->setViewMode(TreeManager::SubnetView);
                if (btnClass) btnClass->setChecked(false);
                if (btnSubnet) btnSubnet->setChecked(true);
            } else {
                m_treeManager->setViewMode(TreeManager::ClassView);
                if (btnClass) btnClass->setChecked(true);
                if (btnSubnet) btnSubnet->setChecked(false);
            }
        }
    }

    Logger::info(QObject::tr("MainWindow: Aktif kullanıcı → ") + user.username);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (isScreenLocked()) {
        event->ignore();
        return;
    }
    Logger::info(QObject::tr("MainWindow: closeEvent tetiklendi, ana pencere kapatılıyor."));
    if (m_sysMgmtWindow) m_sysMgmtWindow->close();
    if (m_userMgmtPanel) m_userMgmtPanel->close();
    if (m_reportPanel)   m_reportPanel->close();
    if (m_chatWindow)    m_chatWindow->close();
    // Tree yapısını kaydet
    if (m_treeManager) m_treeManager->save();
    // Kapanmadan önce yayını durdur — agent'lar overlay'de kalmasın
    stopBroadcast();
    QMainWindow::closeEvent(event);
    emit windowClosed();
}
// ---------------------------------------------------------------------------
// Mesaj gönderme — shared helper (toolbar + sağ tık menüsü)
// ---------------------------------------------------------------------------

static QString showMessageComposeDlg(QWidget* parent,
                                     const QString& title,
                                     int targetCount) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumSize(480, 240);
    dlg.setWindowFlags(
        (dlg.windowFlags() | Qt::CustomizeWindowHint)
        & ~Qt::WindowMinMaxButtonsHint);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(10);
    lay->setContentsMargins(16, 16, 16, 12);

    QString hint = targetCount > 1
        ? QObject::tr("%1 istemciye gönderilecek").arg(targetCount)
        : QObject::tr("Mesajınız:");
    lay->addWidget(new QLabel(hint, &dlg));

    auto* edit = new QPlainTextEdit(&dlg);
    edit->setPlaceholderText(QObject::tr("Mesajınızı buraya yazın..."));
    edit->setMinimumHeight(130);
    lay->addWidget(edit, 1);

    auto* bbox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(bbox);
    QObject::connect(bbox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bbox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return {};
    return edit->toPlainText().trimmed();
}
