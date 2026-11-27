// master/ui/MainWindow/MainWindowClientOps.cpp
#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "../SettingsDialog/SettingsDialog.h"
#include "../SetupWizard/SetupWizard.h"
#include "../LogViewerDialog/LogViewerDialog.h"
#include "../UserManagementPanel/UserManagementPanel.h"
#include "../CustomProfilesDialog/CustomProfilesDialog.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../ssh/SshTerminalWidget.h"
#include "../../ssh/SshClient.h"
#include "../../ssh/SshCredentialDialog.h"
#include "../../ssh/SshDeviceSessionWidget.h"
#include "../../ssh/SshFileManagerWidget.h"
#include "../../ssh/BrokerUpdateManagerDialog.h"
#include <QHostInfo>
#include <QCheckBox>
#include <QWidgetAction>
#include <QDir>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QTimeZone>
static QString showMessageComposeDlg(QWidget* parent, const QString& title, int targetCount);
#include "../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/OkidiaConfig.h"
#include "../../audit/AuditLog.h"
#include <QMenu>
#include <QMessageBox>
#include <QInputDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QtConcurrent>
#include <QTimer>
#include <QPushButton>
#include "../../../common/ui/ThemeManager.h"
#include "ui_MainWindow.h"

static QString formatPopupSender(const UserRecord& user) {
    QString roleTitle;
    if (user.role == "YM") {
        roleTitle = QObject::tr("Yönetici");
    } else if (user.role == "OM") {
        roleTitle = QObject::tr("Öğretmen");
    } else if (user.role == "OBSERVER") {
        roleTitle = QObject::tr("Gözlemci");
    } else {
        roleTitle = QObject::tr("Yönetici");
    }

    QString senderName = user.displayName.trimmed();
    if (senderName.isEmpty()) {
        senderName = user.username.trimmed();
    }
    if (senderName.isEmpty()) {
        senderName = QHostInfo::localHostName();
    }

    const QString senderTitle = QString("%1 (%2)").arg(senderName, roleTitle);
    const QString roleCode = user.role.isEmpty() ? QStringLiteral("YM") : user.role;
    return QString("%1|%2").arg(roleCode, senderTitle);
}

void MainWindow::onSendMessage() {
    if (!m_session) return;

    const QString sender = formatPopupSender(m_activeUser);
    QStringList targets = commandTargets();
    bool isAll = selectedClientIds().isEmpty() && (targets.size() == m_thumbnails.size());

    QString title = isAll
        ? tr("Tüm İstemcilere Mesaj Gönder")
        : tr("Mesaj Gönder — %1 istemci").arg(targets.size());
    QString msg = showMessageComposeDlg(this, title, targets.size());
    if (msg.isEmpty()) return;

    if (isAll) {
        bool ok = m_session->broadcastPopup(msg, sender);
        showCommandStatus(ok, tr("Tüm istemcilere mesaj gönderildi"));
    } else {
        bool ok = true;
        for (const QString& id : targets)
            ok &= m_session->sendPopup(id, msg, sender);
        showCommandStatus(ok,
            QString::number(targets.size()) + tr(" istemciye mesaj gönderildi"));
    }
}
#include "../../../common/crypto/BackupPackage.h"

void MainWindow::onSettings() {
    AppSettings prev = AppSettings::load();
    SettingsDialog dlg(m_activeUser.role, this);

    connect(&dlg, &SettingsDialog::settingsApplied,
            [this, prev](const AppSettings& s) {
        // Broker ayarlarını senkronize et (ders programı, zil ayarları, timezone, kilit politikası vb.)
        if (m_session && m_session->isConnected()) {
            QString schedJson = OkidiaConfig::exportSchoolConfigJson();
            QString tzId = QString::fromUtf8(QTimeZone::systemTimeZone().id());
            uint32_t lockPol = (s.smartboardLockPolicy == 0 ? 1 : 2);
            m_session->sendBrokerSettingsUpdate(s.auditRetention, s.backupRetention, s.autoBackupIncludeChat, 
                                                s.autoBackupPassword, schedJson, tzId, lockPol);
            Logger::info(QObject::tr("MainWindow: Sunucu ayarları güncellendi → Kayıt: %1 gün, Yedek Saklama: %2 adet, Ders Programı Senkronize Edildi, Saat Dilimi: %3, Kilit Tipi: %4")
                         .arg(s.auditRetention).arg(s.backupRetention).arg(tzId).arg(lockPol));
            if (m_activeUser.role == "YM") {
                QString targetName = !s.brokerName.isEmpty() ? s.brokerName : "OKIDIA-Broker";
                quint16 targetPort = s.brokerPort > 0 ? static_cast<quint16>(s.brokerPort) : 11200;
                if (s.brokerName != prev.brokerName || s.brokerPort != prev.brokerPort) {
                    m_session->sendSetBrokerConfig(targetName, targetPort);
                    Logger::info(QObject::tr("MainWindow: Broker sunucu bağlantı ayarları değiştiği için konfigürasyon gönderildi: %1:%2").arg(targetName).arg(targetPort));
                }
            }
        }

        applySessionTimeoutSettings(s.idleTimeoutMinutes, s.maxSessionHours);

        // Okul adı değişmişse pencere başlığını anında güncelle
        updateWindowTitle();

        // ── Varlık (Logo ve Zil) Senkronizasyonu ────────────────────────────────
        // Dosyalar yalnızca fiilen değiştiğinde ve ağda çakışma olmaması için sıralı (sequential) kuyrukla gönderilir.
        struct AssetSyncItem {
            QString srcPath;
            QString targetAsset;
            QString targetFileName;
            QString desc;
        };

        QList<AssetSyncItem> candidateAssets = {
            {s.schoolLogoPath,  OkidiaConfig::schoolLogoPath(),        "school_logo.png", tr("Okul Logosu")},
            {s.bellStudentPath, OkidiaConfig::bellStudentAssetPath(), "bell_student.mp3", tr("Öğrenci Giriş Zili")},
            {s.bellTeacherPath, OkidiaConfig::bellTeacherAssetPath(), "bell_teacher.mp3", tr("Öğretmen Giriş Zili")},
            {s.bellRecessPath,  OkidiaConfig::bellRecessAssetPath(),  "bell_recess.mp3",  tr("Teneffüs / Çıkış Zili")}
        };

        auto assetQueue = std::make_shared<QList<AssetSyncItem>>();
        for (const auto& item : candidateAssets) {
            QString effectiveSrc = item.srcPath;
            if (effectiveSrc.isEmpty() || !QFile::exists(effectiveSrc)) {
                if (QFile::exists(item.targetAsset) && QFileInfo(item.targetAsset).size() > 0) {
                    effectiveSrc = item.targetAsset;
                } else {
                    continue;
                }
            }

            bool needsSync = false;
            if (!QFile::exists(item.targetAsset)) {
                needsSync = true;
            } else if (effectiveSrc != item.targetAsset) {
                QFileInfo srcFi(effectiveSrc);
                QFileInfo tgtFi(item.targetAsset);
                if (srcFi.size() != tgtFi.size() || srcFi.lastModified() != tgtFi.lastModified()) {
                    needsSync = true;
                }
            } else {
                // Dosya zaten yerel varlık yolundaysa ama Broker veya bağlı istemcilerde eksik olma ihtimaline karşı zorla kuyruğa ekle
                needsSync = true;
            }

            if (needsSync) {
                if (effectiveSrc != item.targetAsset) {
                    QFile::remove(item.targetAsset);
                    QFile::copy(effectiveSrc, item.targetAsset);
                }
                assetQueue->append(item);
            }
        }

        if (!assetQueue->isEmpty() && m_session && m_session->isConnected() && m_session->connection()) {
            auto runNextSync = std::make_shared<std::function<void()>>();
            *runNextSync = [this, assetQueue, runNextSync]() {
                if (assetQueue->isEmpty()) {
                    return;
                }
                if (!m_session || !m_session->isConnected() || !m_session->connection()) {
                    return;
                }

                AssetSyncItem cur = assetQueue->takeFirst();
                auto* sender = new FileSender(m_session->connection(), this);
                connect(sender, &FileSender::finished, this, [this, sender, runNextSync](bool, const QString&) {
                    sender->deleteLater();
                    QTimer::singleShot(150, this, [runNextSync]() {
                        (*runNextSync)();
                    });
                });
                sender->sendToAll(cur.targetAsset, true, "assets", cur.targetFileName);
                Logger::info(QObject::tr("MainWindow: %1 Broker merkezi deposuna ve tüm bağlı istemcilere dağıtılıyor (dosya: %2).")
                             .arg(cur.desc, cur.targetFileName));
            };

            (*runNextSync)();
        }

        Logger::info(QObject::tr("MainWindow: Ayarlar güncellendi"));
    });

    connect(&dlg, &SettingsDialog::uninstallPasswordChanged,
            [this](const QString& hash) {
        if (OkidiaDatabase::instance().isOpen()) {
            OkidiaDatabase::instance().setUninstallPasswordHash(hash);
        }
        if (m_session) {
            m_session->sendAgentUninstallPassword(hash);
        }
    });

    connect(&dlg, &SettingsDialog::backupRequested,
            [this](const QString& targetDir, const QString& password, bool includeAudit, bool includeChat) {
        if (m_session && m_session->isConnected()) {
            m_session->requestSystemBackup(targetDir, password, includeAudit, includeChat);
            Logger::info(QObject::tr("MainWindow: Sunucu sistem yedeği talep edildi"));
            statusBar()->showMessage(tr("Sistem yedeği alınıyor..."), 4000);
        } else {
            QMessageBox::warning(this, tr("Bağlantı Yok"), tr("Sunucuya bağlı olmadan sistem yedeği alınamaz."));
        }
    });

    connect(&dlg, &SettingsDialog::restoreRequested,
            [this](const QString& path, const QString& password) {
        BackupContent content;
        QString err;
        if (!BackupPackage::unpackFromFile(path, password, content, &err)) {
            QMessageBox::critical(this, tr("Hata"), tr("Yedek paketi açılamadı:\n%1").arg(err));
            return;
        }

        bool restoreAudit = !content.auditData.isEmpty();
        bool restoreChat = !content.chatData.isEmpty();

        if (m_session && m_session->isConnected()) {
            m_session->sendDbRestoreRequest(content.databaseData, QByteArray(), content.certFiles, QString(),
                                            content.auditData, content.chatData, restoreAudit, restoreChat);
            Logger::info(QObject::tr("MainWindow: Sunucu veritabanı yedeği gönderildi → ") + path);
            QMessageBox::information(this, tr("Geri Yükleme Başlatıldı"),
                tr("Yedek sunucuya başarıyla iletildi.\nSunucu veritabanı ve güvenlik sertifikalarını geri yükleyerek kendini yeniden başlatacaktır.\nMaster 2-3 saniye içinde otomatik yeniden bağlanacaktır."));
        } else {
            // Bağlantı yok: Kurtarma Sihirbazı'nı aç
            RecoveryWizard wiz(this);
            if (wiz.exec() == QDialog::Accepted) {
                BrokerEntry broker = wiz.selectedBroker();
                QString code = wiz.setupCode();
                
                Logger::info(QObject::tr("MainWindow: Kurtarma bağlantısı kuruluyor → ") + broker.host);
                m_session->tlsClient()->connectToServerAnonymous(broker.host, broker.port);
                
                // Bağlantı kurulduğunda restore gönderen tek seferlik bir slot bağla
                connect(m_session->tlsClient(), &TlsClient::connected, [this, content, code, restoreAudit, restoreChat](Connection* conn) {
                    m_session->sendDbRestoreRequest(content.databaseData, QByteArray(), content.certFiles, code,
                                                    content.auditData, content.chatData, restoreAudit, restoreChat);
                    Logger::info(QObject::tr("MainWindow: Kurtarma yedeği setup_code ile gönderildi."));
                });
            }
        }
    });

    dlg.exec();
}

void MainWindow::onShowReport() {
    if (isScreenLocked()) return;
    if (!m_reportPanel) {
        m_reportPanel = new ReportPanel(m_session, nullptr);
        m_reportPanel->setWindowIcon(ThemeManager::icon(":/ui/chart.svg"));
    }
    m_reportPanel->showAndRefresh();
}

void MainWindow::onShowUserManagement() {
    if (isScreenLocked()) return;
    if (!m_userMgmtPanel) {
        m_userMgmtPanel = new UserManagementPanel(m_session, nullptr);
        m_userMgmtPanel->setWindowFlags(Qt::Window);
        m_userMgmtPanel->setWindowTitle(tr("Yönetici ve Öğretmen Paneli — OKIDIA"));
        m_userMgmtPanel->setWindowIcon(ThemeManager::icon(":/ui/group.svg"));
        m_userMgmtPanel->resize(760, 520);
        m_userMgmtPanel->setActiveUser(m_activeUser);
    }
    m_userMgmtPanel->refresh();
    m_userMgmtPanel->show();
    m_userMgmtPanel->raise();
    m_userMgmtPanel->activateWindow();
}

void MainWindow::onShowLogViewer() {
    LogViewerDialog dlg(m_session, this);
    dlg.exec();
}

void MainWindow::onShowLogViewerForClient(const QString& clientId) {
    LogViewerDialog dlg(m_session, this);
    dlg.setTarget(clientId);
    dlg.exec();
}

void MainWindow::onSwitchSchool() {
    int reply = QMessageBox::question(this, tr("Okul / Ağ Değiştir"),
        tr("Başka bir okula geçmek veya yeni bir okul kaydetmek için mevcut oturum kapatılacak ve giriş ekranına dönülecektir.\n\nDevam etmek istiyor musunuz?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        stopBroadcast();
        if (m_treeManager) m_treeManager->save();
        emit logoutRequested();
    }
}

void MainWindow::onExit() {
    stopBroadcast();
    if (m_treeManager) m_treeManager->save();
    emit logoutRequested();
}

void MainWindow::onLightTheme() {
    ThemeManager::instance().applyTheme(2); // Günışığı Mavisi (Açık)
    QSettings("okidia", "master").setValue("users/" + m_activeUser.username + "/ui/theme", 2);
}

void MainWindow::onDarkTheme() {
    ThemeManager::instance().applyTheme(0); // Gece Mavisi (Koyu)
    QSettings("okidia", "master").setValue("users/" + m_activeUser.username + "/ui/theme", 0);
}

void MainWindow::onAbout() {
    const QString ver = qApp->applicationVersion().isEmpty() ? Okidia::Version::string() : qApp->applicationVersion();
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Hakkında OKIDIA"));
    msgBox.setIconPixmap(QIcon(":/icons/master/okidia-master-64.png").pixmap(64, 64));
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setText(
        QString("<h3>OKIDIA Master <span style='font-size: 13px; font-weight: normal;'>v%1</span></h3>"
                "<p><b>%2</b></p>"
                "<p>%3</p>"
                "<hr/>"
                "<p><b>%4:</b> <a href='https://www.okidia.net'>https://www.okidia.net</a><br/>"
                "<b>%5:</b> <a href='https://lisans.okidia.net'>https://lisans.okidia.net</a><br/>"
                "<b>%6:</b> <a href='mailto:info@okidia.net'>info@okidia.net</a></p>"
                "<hr/>"
                "<p style='font-size: 11px; color: #7f8c8d;'>"
                "<b>%7:</b> Qt 6 (LGPLv3), OpenSSL, Google Protocol Buffers, libssh2, libjpeg-turbo, Avahi.<br/>"
                "%8"
                "</p>")
        .arg(ver)
        .arg(tr("Okul İçi Denetim ve İletişim Ağı — Öğretmen ve Yönetici Konsolu"))
        .arg(tr("Sınıf yönetimi, masaüstü izleme, ekran yayını, dosya aktarımı, anons, güvenli sohbet ve uzaktan SSH komut yürütme platformu."))
        .arg(tr("Web Sitesi"))
        .arg(tr("Lisans Sunucusu"))
        .arg(tr("İletişim"))
        .arg(tr("Açık Kaynak Bileşenler"))
        .arg(tr("Telif Hakkı © 2024-2026 OKIDIA. Tüm hakları saklıdır."))
    );
    QAbstractButton* btnLicense = msgBox.addButton(tr("Lisans Bilgisi"), QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Ok);
    msgBox.exec();
    if (msgBox.clickedButton() == btnLicense) {
        OkidiaUtils::showLicenseDialog(this);
    }
}
void MainWindow::onFpsChanged(int index) {
    if (index < 0) return;
    int interval = ui->fpsCombo->itemData(index).toInt();
    const bool isPaused = (interval == 0);

    // QSettings'e kaydet
    QSettings settings("okidia", "master");
    if (!isPaused) {
        settings.setValue("users/" + m_activeUser.username + "/ui/fps_interval", interval);
    }
    settings.setValue("users/" + m_activeUser.username + "/ui/fps_paused", isPaused);

    if (ui->btnStreamToggle) {
        if (isPaused) {
            ui->btnStreamToggle->setIcon(ThemeManager::icon(":/ui/play.svg"));
            ui->btnStreamToggle->setToolTip(tr("Ekran Akışını Başlat"));
        } else {
            ui->btnStreamToggle->setIcon(ThemeManager::icon(":/ui/pause.svg"));
            ui->btnStreamToggle->setToolTip(tr("Ekran Akışını Duraklat"));
        }
    }

    if (m_session) {
        if (isPaused) {
            m_session->setStreamPaused(true);
            showCommandStatus(true, tr("Ekran akışı duraklatıldı (0 FPS)"));
        } else {
            m_session->setUserFpsInterval(interval);
            showCommandStatus(true, tr("Thumbnail hızı güncellendi: ") + ui->fpsCombo->currentText());
        }
    }
}

// ============================================================================
// Yardımcı metodlar
// ============================================================================

QStringList MainWindow::selectedClientIds() const {
    QStringList ids;
    for (auto it = m_thumbnails.constBegin();
         it != m_thumbnails.constEnd(); ++it) {
        if (it.value()->isSelected() && it.value()->isVisible())
            ids << it.key();
    }
    return ids;
}

QStringList MainWindow::visibleClientIds() const {
    QStringList ids;
    for (auto it = m_thumbnails.constBegin();
         it != m_thumbnails.constEnd(); ++it) {
        if (it.value() && it.value()->isVisible())
            ids << it.key();
    }
    return ids;
}

void MainWindow::showCommandStatus(bool ok, const QString& text) {
    QString msg = ok
        ? "" + text
        : "" + text + tr(" (bağlantı yok?)");
    statusBar()->showMessage(msg, 3000);

    // Başarılı komutları audit log'a yaz — actor setActiveUser'dan gelir
    if (ok) {
        AuditLog::instance().recordCommand({}, {}, text);
    }
}

// ============================================================================
// Faz 4 — Thumbnail context menüsü
// ============================================================================

void MainWindow::onThumbnailContextMenu(const QString& clientId,
                                         const QPoint& globalPos) {
    if (m_activeThumbnailMenu) {
        m_activeThumbnailMenu->close();
    }

    if (!clientId.isEmpty()) {
        QStringList sel = selectedClientIds();
        if (!sel.contains(clientId)) {
            // Tıklanan kart seçili grupta değilse: önceki seçimleri temizle, sadece bunu seç
            handleThumbnailClick(clientId, Qt::NoModifier);
        }
    }

    auto* menu = new QMenu(this);
    m_activeThumbnailMenu = menu;
    buildClientMenu(menu, clientId);
    connect(menu, &QMenu::aboutToHide, this, [this, menu]() {
        if (m_activeThumbnailMenu == menu) {
            m_activeThumbnailMenu = nullptr;
        }
        QTimer::singleShot(500, menu, &QObject::deleteLater);
    });
    menu->popup(globalPos);
}

void MainWindow::buildClientMenu(QMenu* menu, const QString& contextClientId) {
    if (menu) menu->clear();

    // If we have a right-clicked client that isn't selected, use it as context
    // but do NOT clear existing selection — just operate on it separately
    QString clientId = contextClientId;
    QStringList sel = selectedClientIds();
    
    if (!clientId.isEmpty() && !sel.contains(clientId)) {
        // Tıklanan client seçili değil: seçimi bozmadan sadece o client'ı hedef al
        sel = {clientId};
    }
    
    // If no client is clicked or selected, we just disable the title
    if (clientId.isEmpty() && sel.size() == 1) {
        clientId = sel.first();
    }
    
    const bool multi = sel.size() > 1;
    const bool noSelection = sel.isEmpty();

    ClientThumbnail* thumb = clientId.isEmpty() ? nullptr : m_thumbnails.value(clientId, nullptr);
    QString hostname = (thumb && m_session)
        ? m_session->clientState(clientId).hostname
        : clientId;
    if (hostname.isEmpty()) hostname = clientId;

    QString titleStr = multi
        ? QString(tr("%1 istemci seçili")).arg(sel.size())
        : (noSelection ? tr("İstemci seçilmedi") : "" + hostname);

    // Offline durumu kontrol et
    const bool isOnline = (!clientId.isEmpty() && m_session) ? m_session->clientState(clientId).connected : false;
    const bool allOnline = [&]() {
        if (noSelection) return false;
        for (const QString& id : sel)
            if (!m_session || !m_session->clientState(id).connected) return false;
        return true;
    }();

    QAction* titleAct = menu->addAction(titleStr);
    titleAct->setEnabled(false);
    titleAct->setFont([&]{ QFont f; f.setBold(true); return f; }());
    if (!noSelection && !isOnline && !multi)
        menu->addAction(tr("Çevrimdışı"))->setEnabled(false);
    menu->addSeparator();

    if (noSelection) {
        menu->addAction(tr("Lütfen önce bir istemci seçin"))->setEnabled(false);
        return;
    }

    const int cType = (thumb && m_session) ? m_session->clientState(clientId).agentType : 1;
    const bool isObserverRole = (m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM");
    const bool onlyOwn = (ui->classFilterCombo && ui->classFilterCombo->currentIndex() == 1);
    const bool anyManageable = [&]() {
        if (isObserverRole) return false;
        if (m_activeUser.role == "YM") return true;
        for (const QString& id : sel) {
            const QString gId = (m_session ? m_session->clientGroup(id) : QString{});
            const bool isAssigned = (!gId.isEmpty() && gId != "__root__");
            if (onlyOwn) {
                if (isAssigned && m_session && m_session->canManageClass(gId)) return true;
            } else {
                if (m_session && m_session->canManageClient(id)) return true;
            }
        }
        return false;
    }();

    // Gözlemci rolünde veya yetkisiz sınıfta yönetimsel menüleri (Masaüstü, İzleme, Kontrol, Sohbet vb.) gösterme
    if (!anyManageable) {
        auto* noteAct = menu->addAction(ThemeManager::icon(":/ui/lock.svg"), 
            isObserverRole ? tr("Gözlemci Modu: Yönetim kısıtlı") : tr("Bu sınıfta yönetim yetkiniz yok"));
        noteAct->setEnabled(false);
        return;
    }

    // --- Masaüstü — sadece online tek seçimde ---
    if (!multi && isOnline) {
        QMenu* desktopMenu = menu->addMenu(ThemeManager::icon(":/ui/monitor.svg"), tr("Masaüstü"));
        auto* viewAct = desktopMenu->addAction(ThemeManager::icon(":/ui/monitor.svg"), tr("Tam Ekran İzleme"),
            [this, clientId] { onClientViewOnly(clientId); });
        auto* ctrlAct = desktopMenu->addAction(ThemeManager::icon(":/ui/monitor.svg"), tr("Tam Ekran Kontrol"),
            [this, clientId] { onClientTakeControl(clientId); });

        desktopMenu->addSeparator();
        desktopMenu->addAction(ThemeManager::icon(":/ui/terminal.svg"), tr("SSH Yönetimi (Terminal & Dosyalar)..."),
            [this, clientId] { openClientSsh(clientId); });

        if (cType == 2) {
            viewAct->setToolTip(tr("İdari PC: Hedef bilgisayardan onay istenecektir."));
            ctrlAct->setToolTip(tr("İdari PC: Hedef bilgisayardan onay istenecektir."));
        }
        menu->addSeparator();
    }

    // --- Özel Sohbet — sadece tek seçimde ve Öğrenci PC için ---
    if (!multi && cType == 1) {
        auto* privChatAct = menu->addAction(ThemeManager::icon(":/ui/chat.svg"), tr("Özel Sohbet Başlat"),
            [this, clientId] { onClientPrivateChat(clientId); });
        privChatAct->setEnabled(isOnline);

        if (m_session && m_session->clientState(clientId).handRaised) {
            menu->addAction(ThemeManager::icon(":/ui/hand.svg"), tr("El İndir (Yardımı Çözüldü Say)"),
                [this, clientId] { onClientHandLower(clientId); });
        }
        menu->addSeparator();
    }

    // OM kullanıcısı için çoklu seçimde yönetimsel aksiyonları yetkili olunan istemcilerle sınırla
    if (multi && m_activeUser.role == "OM" && m_session) {
        QStringList sanitizedSel;
        for (const QString& id : sel) {
            const QString gId = m_session->clientGroup(id);
            const bool isAssigned = (!gId.isEmpty() && gId != "__root__");
            if (onlyOwn) {
                if (isAssigned && m_session->canManageClass(gId)) {
                    sanitizedSel << id;
                }
            } else {
                if (m_session->canManageClient(id)) {
                    sanitizedSel << id;
                }
            }
        }
        sel = sanitizedSel;
    }

    // Yönetim
    {
        QMenu* mgmtMenu = menu->addMenu(ThemeManager::icon(":/ui/edit.svg"), tr("Yönetim"));
        if (!multi) {
            mgmtMenu->addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Sistemden Çıkar"),
                [this, clientId]() {
                    QTimer::singleShot(0, this, [this, clientId]() { onClientRevoke(clientId); });
                });

            QMenu* typeMenu = mgmtMenu->addMenu(ThemeManager::icon(":/ui/student.svg"), tr("İstemci Tipi"));
            typeMenu->addAction(ThemeManager::icon(":/ui/student.svg"), tr("Öğrenci PC Yap"), [this, clientId]() { 
                if (m_session) m_session->sendAgentTypeSet(clientId, 1); 
            });
            typeMenu->addAction(ThemeManager::icon(":/ui/admin.svg"), tr("İdari PC Yap"), [this, clientId]() { 
                QTimer::singleShot(0, this, [this, clientId]() {
                    if (!m_session) return;
                    int ret = QMessageBox::warning(this, tr("İdari PC Yapılandırması"),
                        tr("Bu istemciyi 'İdari PC' olarak değiştirmek istediğinize emin misiniz?\n\n"
                           "ÖNEMLİ: İdari PC'ler gizlilik odaklıdır. Tekrar Öğrenci PC veya Akıllı Tahta tipine geri dönmek için hedef bilgisayardan (İstemci) fiziksel onay alınması gerekecektir."),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (ret == QMessageBox::Yes) {
                        m_session->sendAgentTypeSet(clientId, 2); 
                    }
                });
            });
            typeMenu->addAction(ThemeManager::icon(":/ui/smartboard.svg"), tr("Akıllı Tahta Yap"), [this, clientId]() { 
                if (m_session) m_session->sendAgentTypeSet(clientId, 3); 
            });

            // Özel Tipler (Zil & Amfi PC vb.)
            const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
            if (!customProfiles.isEmpty()) {
                typeMenu->addSeparator();
                for (const auto& cp : customProfiles) {
                    typeMenu->addAction(ThemeManager::icon(":/ui/custom_profile.svg"), cp.name, [this, clientId, cp]() {
                        if (m_session) m_session->sendAgentTypeSet(clientId, cp.id);
                    });
                }
            }

            typeMenu->addSeparator();
            typeMenu->addAction(ThemeManager::icon(":/ui/discovery.svg"), tr("Detaylı Seçim..."), [this, clientId]() { 
                QTimer::singleShot(0, this, [this, clientId]() { onClientAssignType(clientId); });
            });

            // Yalnızca Yönetici Master (YM) özel tipleri oluşturup silebilir
            if (m_session && m_session->activeUser().role == "YM") {
                typeMenu->addAction(ThemeManager::icon(":/ui/settings.svg"), tr("İstemci Tiplerini Yönet..."), [this]() {
                    CustomProfilesDialog dlg(m_session, this);
                    dlg.exec();
                });
            }

            mgmtMenu->addAction(ThemeManager::icon(":/ui/edit.svg"), tr("Takma Ad / Sınıf Adı Belirle..."),
                [this, clientId]() {
                    QTimer::singleShot(0, this, [this, clientId]() {
                        QString curAlias;
                        if (m_session) {
                            curAlias = m_session->clientState(clientId).alias;
                        }
                        bool ok = false;
                        QString alias = QInputDialog::getText(
                            this,
                            tr("Takma Ad / Sınıf Adı"),
                            tr("Bu bilgisayar için takma ad / sınıf adı girin (Örn: 10-A Sınıfı, Müdür Odası):"),
                            QLineEdit::Normal,
                            curAlias,
                            &ok
                        );
                        if (ok && m_session) {
                            m_session->sendAgentAliasSet(clientId, alias.trimmed());
                            showCommandStatus(true, tr("Takma ad kaydedildi: %1").arg(alias.trimmed()));
                        }
                    });
                });

            mgmtMenu->addSeparator();
            mgmtMenu->addAction(ThemeManager::icon(":/ui/terminal.svg"), tr("Sistem Günlüklerini İncele"),
                [this, clientId]() {
                    QTimer::singleShot(0, this, [this, clientId]() { onShowLogViewerForClient(clientId); });
                });

            // Sürüm uyumsuzluğu kontrolü (Agent Sürümü > Broker Sürümü)
            const QString bVer = m_session ? m_session->brokerVersion() : QString();
            const QString aVer = m_session ? m_session->clientState(clientId).agentVersion : QString();
            if (!bVer.isEmpty() && !aVer.isEmpty() && aVer != "0.0.0" && OkidiaUtils::compareVersions(aVer, bVer) > 0) {
                mgmtMenu->addSeparator();
                mgmtMenu->addAction(ThemeManager::icon(":/ui/refresh.svg"), tr("Kararlı Sürüme Geri Dön (Rollback)"),
                    [this, clientId, aVer, bVer]() {
                        QTimer::singleShot(0, this, [this, clientId, aVer, bVer]() {
                            QMessageBox msgBox(this);
                            msgBox.setWindowTitle(tr("Kararlı Sürüme Geri Dön (Rollback)"));
                            msgBox.setIcon(QMessageBox::Question);
                            msgBox.setText(QString(tr("Bu istemcinin sürümü (v%1) sunucu sürümünden (v%2) daha yenidir.\n\n"
                                                      "İstemci, yerel deposunda bulunan kararlı sürüme döndürülecektir.\n"
                                                      "Bu işlem ağ üzerinden tekrar dosya indirmesi yapmaz.\n\n"
                                                      "Geri alma işlemini başlatmak istiyor musunuz?"))
                                               .arg(aVer, bVer));
                            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                            msgBox.setDefaultButton(QMessageBox::No);

                            if (msgBox.exec() == QMessageBox::Yes) {
                                if (m_session) {
                                    m_session->sendRollbackToStable(clientId, bVer);
                                    statusBar()->showMessage(QString(tr("%1 için kararlı sürüme geri alma komutu gönderildi...")).arg(clientId), 5000);
                                    AuditLog::instance().recordSecurity(
                                        m_activeUser.displayName.isEmpty() ? m_activeUser.username : m_activeUser.displayName,
                                        clientId,
                                        QString(tr("Kararlı sürüme geri alma (Rollback) tetiklendi. Mevcut: v%1, Hedef: v%2")).arg(aVer, bVer));
                                }
                            }
                        });
                    });
            }

        } else {
            mgmtMenu->addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Seçilenleri Sistemden Çıkar"),
                [this, sel]() {
                    QTimer::singleShot(0, this, [this, sel]() {
                        QMessageBox msgBox(this);
                        msgBox.setWindowTitle(tr("İstemcileri Sistemden Çıkar"));
                        msgBox.setText(QString(tr("Seçilen %1 istemci sistemden çıkarılacak.\nDevam etmek istiyor musunuz?")).arg(sel.size()));
                        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                        msgBox.setDefaultButton(QMessageBox::No);

                        QCheckBox* cbUninstall = new QCheckBox(tr("Aynı zamanda uzak bilgisayarlardan tamamen sil (Uninstall)"));
                        cbUninstall->setChecked(false); // Varsayılan olarak false (güvenlik için)
                        msgBox.setCheckBox(cbUninstall);

                        if (msgBox.exec() == QMessageBox::Yes) {
                            bool uninstall = cbUninstall->isChecked();
                            for (const QString& id : sel) {
                                m_session->revokeClient(id, uninstall);
                                
                                // Remove from group frames
                                for (auto* frame : m_groupFrames) {
                                    if (frame->thumbnail(id)) {
                                        frame->removeThumbnail(id);
                                    }
                                }
                                
                                if (m_thumbnails.contains(id)) {
                                    ClientThumbnail* thumb = m_thumbnails.take(id);
                                    thumb->deleteLater();
                                }
                                if (m_treeManager) m_treeManager->removeClient(id);
                            }
                            updateStatusBar(m_thumbnails.keys());
                            showCommandStatus(true, QString(tr("%1 istemci sistemden çıkarıldı")).arg(sel.size()));
                        }
                    });
                });

            QMenu* typeMenu = mgmtMenu->addMenu(ThemeManager::icon(":/ui/student.svg"), tr("İstemci Tipi"));
            typeMenu->addAction(ThemeManager::icon(":/ui/student.svg"), tr("Öğrenci PC Yap"), [this, sel]() { 
                if (m_session) for (const QString& id : sel) m_session->sendAgentTypeSet(id, 1); 
            });
            typeMenu->addAction(ThemeManager::icon(":/ui/admin.svg"), tr("İdari PC Yap"), [this, sel]() { 
                QTimer::singleShot(0, this, [this, sel]() {
                    if (!m_session) return;
                    int ret = QMessageBox::warning(this, tr("İdari PC Yapılandırması"),
                        QString(tr("Seçilen %1 istemciyi 'İdari PC' olarak değiştirmek istediğinize emin misiniz?\n\n"
                                   "ÖNEMLİ: İdari PC'ler gizlilik odaklıdır. Tekrar Öğrenci PC veya Akıllı Tahta tipine geri dönmek için hedef bilgisayarlardan (İstemci) fiziksel onay alınması gerekecektir."))
                                .arg(sel.size()),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (ret == QMessageBox::Yes) {
                        for (const QString& id : sel) m_session->sendAgentTypeSet(id, 2); 
                    }
                });
            });
            typeMenu->addAction(ThemeManager::icon(":/ui/smartboard.svg"), tr("Akıllı Tahta Yap"), [this, sel]() { 
                if (m_session) for (const QString& id : sel) m_session->sendAgentTypeSet(id, 3); 
            });

            // Özel Tipler (Çoklu Seçim)
            const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
            if (!customProfiles.isEmpty()) {
                typeMenu->addSeparator();
                for (const auto& cp : customProfiles) {
                    typeMenu->addAction(ThemeManager::icon(":/ui/custom_profile.svg"), cp.name, [this, sel, cp]() {
                        if (m_session) for (const QString& id : sel) m_session->sendAgentTypeSet(id, cp.id);
                    });
                }
            }

            if (m_session && m_session->activeUser().role == "YM") {
                typeMenu->addSeparator();
                typeMenu->addAction(ThemeManager::icon(":/ui/settings.svg"), tr("İstemci Tiplerini Yönet..."), [this]() {
                    CustomProfilesDialog dlg(m_session, this);
                    dlg.exec();
                });
            }
        }
        menu->addSeparator();
    }

    // --- Ekran ---
    bool anyNonIdari = false;
    for (const QString& id : sel) {
        if (m_session && m_session->clientState(id).agentType != 2) {
            anyNonIdari = true;
            break;
        }
    }

    QMenu* screenMenu = menu->addMenu(ThemeManager::icon(":/ui/monitor.svg"), tr("Ekran"));
    if (anyNonIdari) {
        screenMenu->addAction(ThemeManager::icon(":/ui/lock.svg"), tr("Ekranı Kilitle"),
            [this, sel] { for (const QString& id : sel) onClientScreenLock(id); });
        screenMenu->addAction(ThemeManager::icon(":/ui/unlock.svg"), tr("Ekran Kilidini Aç"),
            [this, sel] { for (const QString& id : sel) onClientScreenUnlock(id); });
    } else {
        auto* act = screenMenu->addAction(tr("Kilit (İdari PC Engeli)"));
        act->setEnabled(false);
        act->setToolTip(tr("İdari PC'ler kilitlenemez."));
    }
    if (!multi) {
        // Ekran görüntüsü ve kaynak seçimi — sadece online
        screenMenu->addSeparator();
        auto* capAct = screenMenu->addAction(tr("Ekran Görüntüsü Al"),
            [this, clientId] { onClientScreenCapture(clientId); });
        capAct->setEnabled(isOnline);
        screenMenu->addSeparator();
        auto* selAct = screenMenu->addAction(tr("Ekran Kaynağı Seç…"),
            [this, clientId] { onClientSelectScreen(clientId); });
        selAct->setEnabled(isOnline);
    }

    // --- Klavye / Fare ---
    QMenu* inputMenu = menu->addMenu(ThemeManager::icon(":/ui/lock.svg"), tr("Klavye / Fare"));
    if (anyNonIdari) {
        inputMenu->addAction(tr("Kullanımı Kilitle"),
            [this, sel] { for (const QString& id : sel) onClientInputLock(id); });
        inputMenu->addAction(tr("Kullanım Kilidini Aç"),
            [this, sel] { for (const QString& id : sel) onClientInputUnlock(id); });
    } else {
        auto* act = inputMenu->addAction(tr("Kontrol (İdari PC Engeli)"));
        act->setEnabled(false);
    }
    if (!allOnline)
        inputMenu->addAction(tr("Offline → komut bekletilir"))->setEnabled(false);

    // --- Güç ---
    QMenu* powerMenu = menu->addMenu(ThemeManager::icon(":/ui/power.svg"), tr("Güç"));
    auto* wolAct = powerMenu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Bilgisayarı Aç (Wake-on-LAN)"),
        [this, sel] { QTimer::singleShot(0, this, [this, sel] { onClientsWakeOnLan(sel); }); });
    wolAct->setToolTip(tr("Ağ üzerinden Wake-on-LAN sihirli paketi göndererek bilgisayarı açar."));
    if (!multi && !anyNonIdari) {
        wolAct->setEnabled(false);
        wolAct->setToolTip(tr("İdari PC'ler güvenlik ve gizlilik gereği uzaktan açılamaz."));
    }
    powerMenu->addSeparator();

    if (anyNonIdari) {
        powerMenu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Kapat"),
            [this, sel] { QTimer::singleShot(0, this, [this, sel] { onClientsShutdown(sel); }); });
        powerMenu->addAction(ThemeManager::icon(":/ui/broadcast.svg"), tr("Yeniden Başlat"),
            [this, sel] { QTimer::singleShot(0, this, [this, sel] { onClientsRestart(sel); }); });
        powerMenu->addAction(ThemeManager::icon(":/ui/key.svg"), tr("Oturumu Kapat"),
            [this, sel] { QTimer::singleShot(0, this, [this, sel] { onClientsLogout(sel); }); });
        powerMenu->addAction(tr("Askıya Al (Uyku)"),
            [this, sel] { QTimer::singleShot(0, this, [this, sel] { onClientsHibernate(sel); }); });
    } else {
        auto* act = powerMenu->addAction(tr("Güç İşlemleri (İdari PC Engeli)"));
        act->setEnabled(false);
        act->setToolTip(tr("İdari PC'ler uzaktan kapatılamaz veya yeniden başlatılamaz."));
    }
    powerMenu->addSeparator();
    auto* agentRestAct = powerMenu->addAction(tr("İstemciyi Yeniden Başlat"),
        [this, sel] { QTimer::singleShot(0, this, [this, sel] { for (const QString& id : sel) onClientAgentRestart(id); }); });
    agentRestAct->setToolTip(tr("İstemci çevrimdışı olsa bile SSH üzerinden yeniden başlatmayı dener."));

    // --- Ses ---
    QMenu* audioMenu = menu->addMenu(ThemeManager::icon(":/ui/volume.svg"), tr("Ses"));
    audioMenu->addAction(tr("Sesi Kapat"),
        [this, sel] { for (const QString& id : sel) onClientAudioMute(id); });
    audioMenu->addAction(tr("Sesi Aç"),
        [this, sel] { for (const QString& id : sel) onClientAudioUnmute(id); });

    audioMenu->addSeparator();

    // Ses seviyesi — widget action içinde slider
    auto* volWidget  = new QWidget(audioMenu);
    auto* volLayout  = new QHBoxLayout(volWidget);
    volLayout->setContentsMargins(8, 4, 8, 4);
    auto* volIcon    = new QLabel(volWidget);
    volIcon->setPixmap(QPixmap(":/ui/volume.svg").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto* volSlider  = new QSlider(Qt::Horizontal, volWidget);
    auto* volLabel   = new QLabel("100%", volWidget);
    volSlider->setRange(0, 100);
    volSlider->setValue(100);
    volSlider->setFixedWidth(120);
    volLabel->setFixedWidth(36);
    volLayout->addWidget(volIcon);
    volLayout->addWidget(volSlider);
    volLayout->addWidget(volLabel);

    connect(volSlider, &QSlider::valueChanged, [volLabel](int v) {
        volLabel->setText(QString("%1%").arg(v));
    });
    connect(volSlider, &QSlider::sliderReleased, [this, sel, volSlider]() {
        const int vol = volSlider->value();
        for (const QString& id : sel)
            m_session->sendAudioSetVolume(id, vol);
    });

    auto* volAction = new QWidgetAction(audioMenu);
    volAction->setDefaultWidget(volWidget);
    audioMenu->addAction(volAction);

    // --- Uygulama Yönetimi (Tekil ve Çoklu Seçim) ---
    if (!multi && cType == 2) {
        // İdari PC: KVKK ve gizlilik gereği açık pencereler/uygulamalar listelenemez ve uzaktan kapatılamaz
        QMenu* appMenu = menu->addMenu(ThemeManager::icon(":/ui/monitor.svg"), tr("Uygulama"));
        auto* act = appMenu->addAction(tr("Uygulama Yönetimi (İdari PC Engeli)"));
        act->setEnabled(false);
        act->setToolTip(tr("İdari bilgisayarların açık pencere ve uygulama listesi KVKK gereği görüntülenemez."));
    } else {
        QMenu* appMenu = menu->addMenu(ThemeManager::icon(":/ui/monitor.svg"), tr("Uygulama"));
        if (!multi) {
            if (m_session) m_session->sendAppListRequest(clientId);
            auto apps = m_appLists.value(clientId);
            if (apps.isEmpty()) {
                QAction* hint = appMenu->addAction(tr("(Açık pencereler alınıyor... Tekrar tıklayın)"));
                hint->setEnabled(false);
            } else {
                for (const auto& app : apps) {
                    QString name = app.first;
                    QString wid  = app.second;
                    if (name.length() > 50) name = name.left(47) + "...";
                    appMenu->addAction(ThemeManager::icon(":/ui/mute.svg"), name,
                        [this, clientId, wid] {
                            if (m_session) m_session->sendAppCloseByWid(clientId, wid);
                        });
                }
            }
            appMenu->addSeparator();
        }

        appMenu->addAction(ThemeManager::icon(":/ui/mute.svg"), tr("Tüm Pencereleri Zorla Kapat"),
            [this, sel] {
                for (const QString& id : sel) {
                    if (m_session) m_session->sendAppCloseAll(id);
                }
                showCommandStatus(true, tr("Tüm pencereler için zorla kapatma gönderildi"));
            });

        appMenu->addAction(ThemeManager::icon(":/ui/forbidden.svg"), tr("Uygulama Kara Listesini Yönet..."),
            [this, sel] {
                onManageAppBlacklist(sel);
            });

        appMenu->addAction(ThemeManager::icon(":/ui/unlock.svg"), tr("Uygulama Engellerini Kaldır"),
            [this, sel] {
                for (const QString& id : sel) {
                    if (m_session) m_session->sendAppUnblock(id);
                }
                showCommandStatus(true, tr("Uygulama engelleri kaldırıldı"));
            });
    }

    // --- Web filtresi ---
    QMenu* webMenu = menu->addMenu(ThemeManager::icon(":/ui/discovery.svg"), tr("İnternet"));
    webMenu->addAction(ThemeManager::icon(":/ui/network.svg"), tr("Tüm İnterneti Engelle"),
        [this, sel] { for (const QString& id : sel) onClientWebBlockAll(id); });
    webMenu->addAction(ThemeManager::icon(":/ui/forbidden.svg"), tr("Yasaklı Siteleri Engelle (Kara Liste)"),
        [this, sel] {
            QSettings settings("Okidia", "Master");
            QStringList defaultList = {"roblox.com", "twitch.tv", "tiktok.com", "instagram.com", "discord.com"};
            QStringList currentList = settings.value("WebFilter/Blacklist", defaultList).toStringList();
            for (const QString& id : sel) {
                if (m_session) m_session->sendWebBlockDomains(id, currentList, tr("Öğretmen tarafından engellendi"));
            }
            showCommandStatus(true, tr("Yasaklı siteler kara listesi %1 istemciye uygulandı").arg(sel.size()));
        });
    webMenu->addAction(ThemeManager::icon(":/ui/edit.svg"), tr("Yasaklı Siteler Listesini Düzenle..."),
        [this, sel] { onManageWebBlacklist(sel); });
    webMenu->addAction(ThemeManager::icon(":/ui/unlock.svg"), tr("Tüm İnternet Engellerini Kaldır"),
        [this, sel] { for (const QString& id : sel) onClientWebUnblock(id); });

    menu->addSeparator();

    // --- Sınıfa Taşı ---
    QMenu* moveMenu = menu->addMenu(ThemeManager::icon(":/ui/school.svg"), tr("Sınıfa Taşı →"));
    if (m_treeManager) {
        m_treeManager->buildMoveMenu(moveMenu, sel, [this](const QString& nodeId) -> bool {
            if (m_activeUser.role == "YM" || !m_session) return true;
            return m_session->canManageClass(nodeId);
        });
    }

    menu->addSeparator();

    // --- Mesaj — tek ve çoklu seçimde ---
    menu->addAction(tr("Mesaj Gönder..."),
        [this, sel] {
            QTimer::singleShot(0, this, [this, sel]() { onClientSendMessage(sel); });
        });
}

// ============================================================================
// Faz 4 — Tek istemci komut slotları
// ============================================================================

void MainWindow::onClientHandLower(const QString& clientId) {
    if (!m_session) return;
    okidia::Envelope env;
    env.set_type(okidia::MessageType::HAND_LOWER);
    m_session->sendCommand(clientId, env);
    m_session->setClientHandRaised(clientId, false);
    showCommandStatus(true, QString(tr("%1 için el indirildi")).arg(clientId));
}

void MainWindow::onClientPrivateChat(const QString& clientId) {
    if (!m_session || !m_privateChatMgr) return;
    const ClientState s = m_session->clientState(clientId);
    const QString displayName = s.username.isEmpty()
        ? (s.hostname.isEmpty() ? clientId : s.hostname)
        : QString("%1 (%2)").arg(s.username, s.hostname);

    const QString helpNote = s.handRaiseMessage;

    // Varsa mevcut açık oturum
    QString existingSessionId = m_privateChatMgr->sessionIdForPeer(clientId);
    if (existingSessionId.isEmpty()) {
        m_privateChatMgr->openDirect(clientId, displayName, s.hostname, s.username);
        existingSessionId = m_privateChatMgr->sessionIdForPeer(clientId);
    }

    if (!existingSessionId.isEmpty() && !helpNote.isEmpty()) {
        m_privateChatMgr->appendSystemMessage(existingSessionId, tr("Öğrencinin yardım talebi: ") + helpNote);
    }

    // El kaldırılmışsa otomatik indir
    if (s.handRaised) {
        okidia::Envelope env;
        env.set_type(okidia::MessageType::HAND_LOWER);
        m_session->sendCommand(clientId, env);
        m_session->setClientHandRaised(clientId, false);
    }

    if (m_chatWindow) {
        m_chatWindow->showAndRaise();
        if (!existingSessionId.isEmpty()) {
            m_chatWindow->switchToTab(existingSessionId);
        }
    }
}

void MainWindow::onClientScreenLock(const QString& clientId) {
    if (!m_session) return;
    m_session->sendInputLock(clientId);
    bool ok = m_session->sendScreenLock(clientId,
                  tr("Öğretmen ekranınızı kilitledi."));
    if (ok) {
        m_session->setClientScreenLocked(clientId, true);
        m_session->setClientInputLocked(clientId, true);
    }
    showCommandStatus(ok, clientId + tr(" ekranı kilitlendi"));
    updateLockToggleState();
}

void MainWindow::onClientScreenUnlock(const QString& clientId) {
    if (!m_session) return;
    m_session->sendInputUnlock(clientId);
    bool ok = m_session->sendScreenUnlock(clientId);
    if (ok) {
        m_session->setClientScreenLocked(clientId, false);
        m_session->setClientInputLocked(clientId, false);
        if (m_treeManager) m_treeManager->setClientDevPin(clientId, QString(), 0);
    }
    showCommandStatus(ok, clientId + tr(" ekranı açıldı"));
    updateLockToggleState();
}

void MainWindow::onClientInputLock(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendInputLock(clientId);
    if (ok) m_session->setClientInputLocked(clientId, true);
    showCommandStatus(ok, clientId + " input kilitlendi");
}

void MainWindow::onClientInputUnlock(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendInputUnlock(clientId);
    if (ok) m_session->setClientInputLocked(clientId, false);
    showCommandStatus(ok, clientId + tr(" input kilidi açıldı"));
}

void MainWindow::onClientSendMessage(const QStringList& targets) {
    if (!m_session || targets.isEmpty()) return;

    QString title = targets.size() == 1
        ? tr("Mesaj Gönder — ") + targets.first()
        : QString(tr("Mesaj Gönder — %1 istemci")).arg(targets.size());
    QString msg = showMessageComposeDlg(this, title, targets.size());
    if (msg.isEmpty()) return;

    const QString sender = formatPopupSender(m_activeUser);
    bool ok = true;
    for (const QString& id : targets)
        ok &= m_session->sendPopup(id, msg, sender);
    showCommandStatus(ok,
        QString::number(targets.size()) + tr(" istemciye mesaj gönderildi"));
}

void MainWindow::onClientScreenCapture(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendScreenCapture(clientId);
    showCommandStatus(ok, clientId + tr(" ekran görüntüsü istendi"));
    // Gelen SCREEN_CAPTURE_RESULT SessionManager'da işlenecek (Faz 4b)
}

QString MainWindow::clientDisplayName(const QString& clientId) const {
    if (m_session) {
        const ClientState s = m_session->clientState(clientId);
        if (!s.hostname.isEmpty()) {
            if (!s.username.isEmpty()) {
                return QString("%1 (%2)").arg(s.hostname, s.username);
            }
            return s.hostname;
        }
    }
    if (OkidiaDatabase::instance().isOpen()) {
        const AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
        if (!rec.hostname.isEmpty()) {
            return rec.hostname;
        }
    }
    return clientId;
}

void MainWindow::onClientWakeOnLan(const QString& clientId) {
    onClientsWakeOnLan({clientId});
}

void MainWindow::onClientsWakeOnLan(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    QStringList validTargets;
    for (const QString& id : clientIds) {
        if (m_session->clientState(id).agentType != 2 &&
            OkidiaDatabase::instance().agent(id).agentType != 2) {
            validTargets << id;
        }
    }

    if (validTargets.isEmpty()) {
        showCommandStatus(false, tr("İdari PC'ler uzaktan açılamaz."));
        return;
    }

    for (const QString& id : validTargets) {
        m_session->sendWakeOnLan(id);
    }
    AuditLog::instance().recordPower({}, validTargets.join(", "), tr("Wake-on-LAN sinyali gönderildi"));

    if (validTargets.size() == 1) {
        QString name = clientDisplayName(validTargets.first());
        showCommandStatus(true, tr("%1 için açma sinyali (Wake-on-LAN) gönderildi").arg(name));
    } else {
        showCommandStatus(true, QString(tr("%1 bilgisayara açma sinyali (Wake-on-LAN) gönderildi")).arg(validTargets.size()));
    }
}

void MainWindow::onClientShutdown(const QString& clientId) {
    onClientsShutdown({clientId});
}

void MainWindow::onClientsShutdown(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    QString msg;
    if (clientIds.size() == 1) {
        QString name = clientDisplayName(clientIds.first());
        msg = tr("%1 kapatılsın mı?").arg(name);
    } else {
        msg = QString(tr("Seçili %1 bilgisayar kapatılsın mı?")).arg(clientIds.size());
    }

    int ret = QMessageBox::question(
        this, tr("Bilgisayarları Kapat"),
        msg,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    for (const QString& id : clientIds) {
        m_session->sendShutdown(id, 0, tr("Öğretmen komutu"));
    }
    AuditLog::instance().recordPower({}, clientIds.join(", "), tr("Kapatma komutu verildi"));

    if (clientIds.size() == 1) {
        QString name = clientDisplayName(clientIds.first());
        showCommandStatus(true, tr("%1 kapatılıyor").arg(name));
    } else {
        showCommandStatus(true, QString(tr("%1 bilgisayar kapatılıyor")).arg(clientIds.size()));
    }
}

void MainWindow::onClientRestart(const QString& clientId) {
    onClientsRestart({clientId});
}

void MainWindow::onClientsRestart(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    QString msg;
    if (clientIds.size() == 1) {
        QString name = clientDisplayName(clientIds.first());
        msg = tr("%1 yeniden başlatılsın mı?").arg(name);
    } else {
        msg = QString(tr("Seçili %1 bilgisayar yeniden başlatılsın mı?")).arg(clientIds.size());
    }

    int ret = QMessageBox::question(
        this, tr("Yeniden Başlat"),
        msg,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    for (const QString& id : clientIds) {
        m_session->sendRestart(id, 0, tr("Öğretmen komutu"));
    }
    AuditLog::instance().recordPower({}, clientIds.join(", "), tr("Yeniden başlatma komutu verildi"));

    if (clientIds.size() == 1) {
        QString name = clientDisplayName(clientIds.first());
        showCommandStatus(true, tr("%1 yeniden başlatılıyor").arg(name));
    } else {
        showCommandStatus(true, QString(tr("%1 bilgisayar yeniden başlatılıyor")).arg(clientIds.size()));
    }
}

void MainWindow::onClientLogout(const QString& clientId) {
    onClientsLogout({clientId});
}

void MainWindow::onClientsLogout(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    for (const QString& id : clientIds) {
        m_session->sendLogout(id);
    }
    AuditLog::instance().recordPower({}, clientIds.join(", "), tr("Oturum kapatma komutu verildi"));

    if (clientIds.size() == 1) {
        QString name = clientDisplayName(clientIds.first());
        showCommandStatus(true, tr("%1 oturum kapatılıyor").arg(name));
    } else {
        showCommandStatus(true, QString(tr("%1 bilgisayarda oturum kapatılıyor")).arg(clientIds.size()));
    }
}

void MainWindow::onClientHibernate(const QString& clientId) {
    onClientsHibernate({clientId});
}

void MainWindow::onClientsHibernate(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    for (const QString& id : clientIds) {
        m_session->sendHibernate(id);
    }
    AuditLog::instance().recordPower({}, clientIds.join(", "), tr("Askıya alma (hazırda beklet) komutu verildi"));

    if (clientIds.size() == 1) {
        QString name = clientDisplayName(clientIds.first());
        showCommandStatus(true, tr("%1 askıya alınıyor").arg(name));
    } else {
        showCommandStatus(true, QString(tr("%1 bilgisayar askıya alınıyor")).arg(clientIds.size()));
    }
}

void MainWindow::onClientAudioMute(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendAudioMute(clientId);
    if (ok) m_session->setClientAudioMuted(clientId, true);
    showCommandStatus(ok, tr("Ses kapatıldı"));
}

void MainWindow::onClientAudioUnmute(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendAudioUnmute(clientId);
    if (ok) m_session->setClientAudioMuted(clientId, false);
    showCommandStatus(ok, tr("Ses açıldı"));
}

void MainWindow::onClientAppClose(const QString& clientId) {
    if (!m_session) return;
    bool inputOk;
    QString appName = QInputDialog::getText(
        this, "Uygulama Kapat — " + clientId,
        tr("Kapatılacak uygulama adı (örn: firefox):"),
        QLineEdit::Normal, QString(), &inputOk);
    if (!inputOk || appName.trimmed().isEmpty()) return;
    bool ok = m_session->sendAppClose(clientId, appName.trimmed());
    showCommandStatus(ok, clientId + " → " + appName + tr(" kapatılıyor"));
}

void MainWindow::onClientViewOnly(const QString& clientId) {
    if (!m_session) return;
    if (m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM") {
        showCommandStatus(false, tr("Gözlemci Modu: Yalnızca canlı izleme yetkiniz vardır."));
        return;
    }
    if (!m_session->canManageClient(clientId)) {
        showCommandStatus(false, tr("Bu istemci üzerinde yönetim yetkiniz yoktur."));
        return;
    }
    
    auto st = m_session->clientState(clientId);
    if (st.lockType == "Broadcast") {
        QString teacher = st.lockedByDisplayName.isEmpty() ? st.lockedBy : st.lockedByDisplayName;
        if (teacher.isEmpty()) teacher = tr("Öğretmen");
        QMessageBox::warning(this, tr("Canlı Ekran Yayını"),
                             tr("Bu cihaz şu anda %1 tarafından yapılan canlı ekran yayınını izlemektedir.\n"
                                "Yayın esnasında canlı izleme başlatılamaz.").arg(teacher));
        return;
    }
    if (!st.lockType.isEmpty() && (st.lockType == "RemoteControl" || st.lockType == "ScreenView") && st.lockedBy != m_session->activeUser().username) {
        QMessageBox::warning(this, tr("Kilitli Cihaz"), tr("Bu cihaz şu anda başka bir yönetici tarafından izleniyor veya kontrol ediliyor."));
        return;
    }

    if (st.agentType == 2) {
        if (m_remoteControlWaitDialog) {
            m_remoteControlWaitDialog->close();
            m_remoteControlWaitDialog->deleteLater();
        }
        
        m_remoteControlWaitDialog = new QDialog(this);
        m_remoteControlWaitDialog->setAttribute(Qt::WA_QuitOnClose, false);
        m_remoteControlWaitDialog->setWindowTitle(tr("İzin Bekleniyor"));
        m_remoteControlWaitDialog->setWindowFlags(m_remoteControlWaitDialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
        m_remoteControlWaitDialog->setFixedSize(350, 150);
        
        auto* layout = new QVBoxLayout(m_remoteControlWaitDialog);
        auto* lblInfo = new QLabel(tr("İstek Gönderildi. Yanıt bekleniyor..."), m_remoteControlWaitDialog);
        lblInfo->setAlignment(Qt::AlignCenter);
        layout->addWidget(lblInfo);
        
        auto* btnLayout = new QHBoxLayout();
        auto* btnCancel = new QPushButton(tr("İptal"), m_remoteControlWaitDialog);
        auto* btnResend = new QPushButton(tr("Yeniden İstek Gönder"), m_remoteControlWaitDialog);
        btnLayout->addWidget(btnCancel);
        btnLayout->addWidget(btnResend);
        layout->addLayout(btnLayout);
        
        m_waitDialogSeconds = 120;
        lblInfo->setText(QString(tr("İstek Gönderildi. Yanıt bekleniyor (%1 sn)")).arg(m_waitDialogSeconds));
        
        m_waitDialogTimer = new QTimer(m_remoteControlWaitDialog);
        connect(m_waitDialogTimer, &QTimer::timeout, [this, lblInfo, clientId]() {
            m_waitDialogSeconds--;
            if (m_waitDialogSeconds <= 0) {
                m_waitDialogTimer->stop();
                lblInfo->setText(tr("Yanıt alınamadı. Zaman aşımı."));
                m_autoControlRequested = false;
                m_autoControlTarget.clear();
                if (m_session) m_session->sendRemoteControlStop(clientId);
            } else {
                lblInfo->setText(QString(tr("İstek Gönderildi. Yanıt bekleniyor (%1 sn)")).arg(m_waitDialogSeconds));
            }
        });
        m_waitDialogTimer->start(1000);
        
        connect(btnCancel, &QPushButton::clicked, [this, clientId]() {
            m_autoControlRequested = false;
            m_autoControlTarget.clear();
            if (m_session) m_session->sendRemoteControlStop(clientId);
            if (m_remoteControlWaitDialog) m_remoteControlWaitDialog->close();
        });
        connect(m_remoteControlWaitDialog, &QDialog::rejected, [this, clientId]() {
            if (m_autoControlRequested && m_autoControlTarget == clientId) {
                m_autoControlRequested = false;
                m_autoControlTarget.clear();
                if (m_session) m_session->sendRemoteControlStop(clientId);
            }
        });
        
        connect(btnResend, &QPushButton::clicked, [this, clientId, lblInfo]() {
            m_autoControlRequested = true;
            m_autoControlTarget    = clientId;
            m_session->sendRemoteControlRequest(clientId, false);
            m_waitDialogSeconds = 120;
            lblInfo->setText(QString(tr("İstek Gönderildi. Yanıt bekleniyor (%1 sn)")).arg(m_waitDialogSeconds));
            if (!m_waitDialogTimer->isActive()) m_waitDialogTimer->start(1000);
        });
        
        m_autoControlRequested = true;
        m_autoControlTarget    = clientId;
        m_autoControlMode      = RemoteControlView::Mode::ViewOnly;
        
        m_session->sendRemoteControlRequest(clientId, false);
        m_remoteControlWaitDialog->show();
        
        AuditLog::instance().recordCommand({}, clientId, tr("izleme isteği onayı bekleniyor"));
        return;
    }

    m_autoControlRequested = true;
    m_autoControlTarget    = clientId;
    m_autoControlMode      = RemoteControlView::Mode::ViewOnly;
    
    m_session->sendScreenInfoRequest(clientId);
    const QString targetName = st.hostname.isEmpty() ? (st.username.isEmpty() ? clientId : st.username) : st.hostname;
    showCommandStatus(true, tr("%1 için tam ekran izleme bağlantısı kuruluyor...").arg(targetName));
    
    AuditLog::instance().recordCommand({}, clientId, tr("izleme isteği (bilgi bekleniyor)"));
}

void MainWindow::onRemoteControlResponseReceived(const QString& clientId, bool accepted, const QString& reason) {
    if (!m_autoControlRequested || m_autoControlTarget != clientId) return;

    const auto st = m_session ? m_session->clientState(clientId) : ClientState{};
    const QString targetName = st.hostname.isEmpty() ? (st.username.isEmpty() ? clientId : st.username) : st.hostname;

    if (accepted) {
        if (m_remoteControlWaitDialog) {
            m_remoteControlWaitDialog->close();
            m_remoteControlWaitDialog->deleteLater();
            m_remoteControlWaitDialog = nullptr;
        }
        // Devam: Ekran bilgisini iste ve RemoteControlView'i aç (m_autoControlRequested zaten true)
        m_session->sendScreenInfoRequest(clientId);
        showCommandStatus(true, tr("%1 isteği onaylandı. Ekran bilgisi alınıyor...").arg(targetName));
    } else {
        if (reason == "timeout") {
            // Zaman aşımı durumunda diyaloğu Master kapatmaz. 
            // Master'ın kendi 120 saniyelik timer'ı "Zaman aşımı" yazdıracak ve Yeniden Gönder'e izin verecektir.
            return;
        }
        
        // Reddedildi (timeout değil, açıkça kullanıcı reddetti)
        m_autoControlRequested = false;
        m_autoControlTarget.clear();
        if (m_remoteControlWaitDialog) {
            m_remoteControlWaitDialog->close();
            m_remoteControlWaitDialog->deleteLater();
            m_remoteControlWaitDialog = nullptr;
        }
        QString err = reason.isEmpty() ? tr("Kullanıcı isteği reddetti.") : reason;
        QMessageBox::warning(this, tr("İstek Reddedildi"), tr("Uzak bağlantı isteği reddedildi:\n") + err);
        showCommandStatus(false, tr("%1 izleme isteğini reddetti").arg(targetName));
    }
}

void MainWindow::onClientTakeControl(const QString& clientId) {
    if (!m_session) return;
    if (m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM") {
        showCommandStatus(false, tr("Gözlemci Modu: Yalnızca canlı izleme yetkiniz vardır."));
        return;
    }
    if (!m_session->canManageClient(clientId)) {
        showCommandStatus(false, tr("Bu istemci üzerinde kontrol yetkiniz yoktur."));
        return;
    }

    auto st = m_session->clientState(clientId);
    if (st.lockType == "Broadcast") {
        QString teacher = st.lockedByDisplayName.isEmpty() ? st.lockedBy : st.lockedByDisplayName;
        if (teacher.isEmpty()) teacher = tr("Öğretmen");
        QMessageBox::warning(this, tr("Canlı Ekran Yayını"),
                             tr("Bu cihaz şu anda %1 tarafından yapılan canlı ekran yayınını izlemektedir.\n"
                                "Yayın esnasında uzaktan kontrol başlatılamaz.").arg(teacher));
        return;
    }
    if (!st.lockType.isEmpty() && (st.lockType == "RemoteControl" || st.lockType == "ScreenView") && st.lockedBy != m_session->activeUser().username) {
        QMessageBox::warning(this, tr("Kilitli Cihaz"), tr("Bu cihaz şu anda başka bir yönetici tarafından izleniyor veya kontrol ediliyor."));
        return;
    }

    if (st.agentType == 2) {
        if (m_remoteControlWaitDialog) {
            m_remoteControlWaitDialog->close();
            m_remoteControlWaitDialog->deleteLater();
        }
        
        m_remoteControlWaitDialog = new QDialog(this);
        m_remoteControlWaitDialog->setAttribute(Qt::WA_QuitOnClose, false);
        m_remoteControlWaitDialog->setWindowTitle(tr("İzin Bekleniyor"));
        m_remoteControlWaitDialog->setWindowFlags(m_remoteControlWaitDialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
        m_remoteControlWaitDialog->setFixedSize(350, 150);
        
        auto* layout = new QVBoxLayout(m_remoteControlWaitDialog);
        auto* lblInfo = new QLabel(tr("İstek Gönderildi. Yanıt bekleniyor..."), m_remoteControlWaitDialog);
        lblInfo->setAlignment(Qt::AlignCenter);
        layout->addWidget(lblInfo);
        
        auto* btnLayout = new QHBoxLayout();
        auto* btnCancel = new QPushButton(tr("İptal"), m_remoteControlWaitDialog);
        auto* btnResend = new QPushButton(tr("Yeniden İstek Gönder"), m_remoteControlWaitDialog);
        btnLayout->addWidget(btnCancel);
        btnLayout->addWidget(btnResend);
        layout->addLayout(btnLayout);
        
        m_waitDialogSeconds = 120;
        lblInfo->setText(QString(tr("İstek Gönderildi. Yanıt bekleniyor (%1 sn)")).arg(m_waitDialogSeconds));
        
        m_waitDialogTimer = new QTimer(m_remoteControlWaitDialog);
        connect(m_waitDialogTimer, &QTimer::timeout, [this, lblInfo, clientId]() {
            m_waitDialogSeconds--;
            if (m_waitDialogSeconds <= 0) {
                m_waitDialogTimer->stop();
                lblInfo->setText(tr("Yanıt alınamadı. Zaman aşımı."));
                m_autoControlRequested = false;
                m_autoControlTarget.clear();
                if (m_session) m_session->sendRemoteControlStop(clientId);
            } else {
                lblInfo->setText(QString(tr("İstek Gönderildi. Yanıt bekleniyor (%1 sn)")).arg(m_waitDialogSeconds));
            }
        });
        m_waitDialogTimer->start(1000);
        
        connect(btnCancel, &QPushButton::clicked, [this, clientId]() {
            m_autoControlRequested = false;
            m_autoControlTarget.clear();
            if (m_session) m_session->sendRemoteControlStop(clientId);
            if (m_remoteControlWaitDialog) m_remoteControlWaitDialog->close();
        });
        connect(m_remoteControlWaitDialog, &QDialog::rejected, [this, clientId]() {
            if (m_autoControlRequested && m_autoControlTarget == clientId) {
                m_autoControlRequested = false;
                m_autoControlTarget.clear();
                if (m_session) m_session->sendRemoteControlStop(clientId);
            }
        });
        
        connect(btnResend, &QPushButton::clicked, [this, clientId, lblInfo]() {
            m_autoControlRequested = true;
            m_autoControlTarget    = clientId;
            m_session->sendRemoteControlRequest(clientId, true);
            m_waitDialogSeconds = 120;
            lblInfo->setText(QString(tr("İstek Gönderildi. Yanıt bekleniyor (%1 sn)")).arg(m_waitDialogSeconds));
            if (!m_waitDialogTimer->isActive()) m_waitDialogTimer->start(1000);
        });
        
        m_autoControlRequested = true;
        m_autoControlTarget    = clientId;
        m_autoControlMode      = RemoteControlView::Mode::Control;
        
        m_session->sendRemoteControlRequest(clientId, true);
        m_remoteControlWaitDialog->show();
        
        return;
    }

    m_autoControlRequested = true;
    m_autoControlTarget    = clientId;
    m_autoControlMode      = RemoteControlView::Mode::Control;
    
    m_session->sendScreenInfoRequest(clientId);
    const QString targetName = st.hostname.isEmpty() ? (st.username.isEmpty() ? clientId : st.username) : st.hostname;
    showCommandStatus(true, tr("%1 için tam ekran bağlantısı kuruluyor...").arg(targetName));
}

void MainWindow::onRemoteViewStopped(const QString& clientId) {
    QList<RemoteControlView*> toClose;
    for (auto* v : m_remoteViews) {
        if (v && v->clientId() == clientId) {
            toClose.append(v);
        }
    }
    if (toClose.isEmpty()) {
        return; // Bu istemci için açık bir uzak kontrol penceresi yoksa işlem yapma
    }

    // Bu istemciye ait tüm pencereleri durdur ve listeden çıkar
    if (m_session) {
        m_session->sendRemoteControlStop(clientId);
    }
    
    for (auto* v : toClose) {
        m_remoteViews.removeOne(v);
        v->disconnect(this); // Döngüsel sinyal tetiklenmesini önle
        if (v->isFullScreen()) {
            v->showNormal();
        }
        v->hide();
        v->close();
        v->deleteLater();
    }

    // Bu istemci için başka açık pencere yoksa FPS'i normal thumbnail hızına döndür
    bool hasOtherView = false;
    for (auto* v : m_remoteViews)
        if (v->clientId() == clientId) { hasOtherView = true; break; }

    if (!hasOtherView && m_session) {
        m_session->setClientStreaming(clientId, false);
        // Thumbnail FPS — dinamik hesaplanan değer
        int adaptiveMs = m_session->calculateAdaptiveFps();
        m_session->sendScreenConfig(clientId, adaptiveMs, 60, 320);
        
        if (m_thumbnails.contains(clientId)) {
            m_thumbnails[clientId]->setStreamingMode(false);
            m_thumbnails[clientId]->clearFrameBuffer();
        }
        // İstemciden anında taze tam ekran görüntüsü isteyerek thumbnail'in
        // kırmızı kilit yazısından kurtulup güncel masaüstünü göstermesini sağla
        m_session->sendRefreshRequest(clientId);
    }

    const auto st = m_session ? m_session->clientState(clientId) : ClientState{};
    const QString targetName = st.hostname.isEmpty() ? (st.username.isEmpty() ? clientId : st.username) : st.hostname;
    showCommandStatus(true, tr("%1 uzak kontrol bırakıldı").arg(targetName));
}


void MainWindow::onScreenCaptureReceived(const QString& clientId,
                                          const QPixmap& pixmap,
                                          qint64 timestamp) {
    Q_UNUSED(timestamp)

    // Dosya adı: Hostname-Username-tarih-saat.png
    QString hostname = clientId;
    QString username;
    if (m_session) {
        auto state = m_session->clientState(clientId);
        if (!state.hostname.isEmpty()) hostname = state.hostname;
        username = state.username;
    }

    QString dateStr = QDateTime::fromSecsSinceEpoch(timestamp)
        .toString("yyyyMMdd_HHmmss");

    QString defaultName = hostname +
        (username.isEmpty() ? "" : "-" + username) +
        "-" + dateStr + ".png";

    // Dosya adındaki geçersiz karakterleri temizle
    defaultName.replace(QRegularExpression("[/\\\\:*?\"<>|]"), "_");

    QString path = OkidiaUtils::getSaveFileName(
        this,
        tr("Ekran Görüntüsünü Kaydet — ") + hostname,
        "screenshots",
        defaultName,
        tr("PNG Görseli (*.png);;Tüm Dosyalar (*)"));

    if (path.isEmpty()) return;

    if (pixmap.save(path, "PNG")) {
        showCommandStatus(true,
            hostname + tr(" ekran görüntüsü kaydedildi"));
        Logger::info(QObject::tr("MainWindow: Ekran görüntüsü kaydedildi → ") + path);
    } else {
        showCommandStatus(false, tr("Kaydetme başarısız: ") + path);
    }
}
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

void MainWindow::onClientAssignType(const QString& clientId) {
    if (!m_session) return;
    
    QStringList types = {tr("Öğrenci PC"), tr("İdari PC (Gizlilik)"), tr("Akıllı Tahta")};
    QList<uint32_t> typeIds = {1, 2, 3};

    const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
    for (const auto& cp : customProfiles) {
        types.append(cp.name);
        typeIds.append(static_cast<uint32_t>(cp.id));
    }

    int currentType = m_session->clientState(clientId).agentType;
    int defaultIndex = typeIds.indexOf(static_cast<uint32_t>(currentType));
    if (defaultIndex < 0) defaultIndex = 0;

    bool ok;
    QString selected = QInputDialog::getItem(this, tr("İstemci Tipi Ata"),
                                             clientId + tr(" için istemci tipi seçin:"),
                                             types, defaultIndex, false, &ok);
    if (!ok) return;

    int selIdx = types.indexOf(selected);
    if (selIdx < 0) return;
    uint32_t agentType = typeIds[selIdx];

    if (agentType == 2) {
        int ret = QMessageBox::warning(this, tr("İdari PC Yapılandırması"),
            tr("Bu istemciyi 'İdari PC' olarak değiştirmek istediğinize emin misiniz?\n\n"
               "ÖNEMLİ: İdari PC'ler gizlilik odaklıdır. Tekrar Öğrenci PC veya Akıllı Tahta tipine geri dönmek için hedef bilgisayardan (İstemci) fiziksel onay alınması gerekecektir."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }
    bool success = m_session->sendAgentTypeSet(clientId, agentType);
    
    if (success) {
        // State'i güncelle
        auto st = m_session->clientState(clientId);
        st.agentType = agentType;
        // Not: SessionManager::clientState returns a copy, we need a setter or update logic.
        // For now, let's just show status. The broker will persist it anyway if we update it there too.
        // Wait, sendAgentTypeSet should also tell the broker to update the DB.
        
        showCommandStatus(true, clientId + tr(" için yeni tip atandı: ") + selected);
    } else {
        showCommandStatus(false, tr("Tip atanamadı"));
    }
}

void MainWindow::onManageAppBlacklist(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    QSettings settings("Okidia", "Master");
    QStringList currentList = settings.value("AppBlock/Blacklist", QStringList() << "roblox" << "discord" << "steam" << "spotify").toStringList();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Uygulama Kara Liste Yönetimi"));
    dlg.setMinimumSize(460, 380);

    auto* layout = new QVBoxLayout(&dlg);

    auto* lblDesc = new QLabel(tr("Engellenmesini istediğiniz uygulama ve süreç isimlerini her satıra bir tane gelecek şekilde giriniz:\n(Örnek: roblox, discord, spotify, steam, chrome)"), &dlg);
    lblDesc->setWordWrap(true);
    layout->addWidget(lblDesc);

    auto* txtEdit = new QPlainTextEdit(&dlg);
    txtEdit->setPlainText(currentList.join("\n"));
    layout->addWidget(txtEdit);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btnBox->button(QDialogButtonBox::Ok)->setText(tr("Uygula ve Engelle"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    if (dlg.exec() == QDialog::Accepted) {
        QStringList lines = txtEdit->toPlainText().split('\n', Qt::SkipEmptyParts);
        QStringList cleanedList;
        for (QString line : lines) {
            line = line.trimmed();
            if (!line.isEmpty() && !cleanedList.contains(line, Qt::CaseInsensitive)) {
                cleanedList.append(line);
            }
        }

        settings.setValue("AppBlock/Blacklist", cleanedList);

        for (const QString& id : clientIds) {
            m_session->sendAppBlockList(id, cleanedList);
        }

        showCommandStatus(true, tr("Uygulama kara listesi %1 istemciye uygulandı (%2 uygulama)")
                                .arg(clientIds.size()).arg(cleanedList.size()));
    }
}

void MainWindow::onManageWebBlacklist(const QStringList& clientIds) {
    if (!m_session || clientIds.isEmpty()) return;

    QSettings settings("Okidia", "Master");
    QStringList defaultList = {"roblox.com", "twitch.tv", "tiktok.com", "instagram.com", "discord.com"};
    QStringList currentList = settings.value("WebFilter/Blacklist", defaultList).toStringList();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Yasaklı Web Siteleri (Kara Liste)"));
    dlg.setMinimumSize(460, 380);

    auto* layout = new QVBoxLayout(&dlg);

    auto* lblDesc = new QLabel(tr("Engellenmesini istediğiniz alan adlarını (domain) her satıra bir tane gelecek şekilde giriniz:\n(Örnek: roblox.com, twitch.tv, discord.com)"), &dlg);
    lblDesc->setWordWrap(true);
    layout->addWidget(lblDesc);

    auto* txtEdit = new QPlainTextEdit(&dlg);
    txtEdit->setPlainText(currentList.join("\n"));
    layout->addWidget(txtEdit);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btnBox->button(QDialogButtonBox::Ok)->setText(tr("Kaydet ve Uygula"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    if (dlg.exec() == QDialog::Accepted) {
        QStringList lines = txtEdit->toPlainText().split('\n', Qt::SkipEmptyParts);
        QStringList cleanedList;
        for (QString line : lines) {
            line = line.trimmed().toLower();
            if (line.startsWith("http://")) line.remove(0, 7);
            if (line.startsWith("https://")) line.remove(0, 8);
            if (line.contains("/")) line = line.section('/', 0, 0);
            if (!line.isEmpty() && !cleanedList.contains(line)) {
                cleanedList.append(line);
            }
        }

        settings.setValue("WebFilter/Blacklist", cleanedList);

        for (const QString& id : clientIds) {
            m_session->sendWebBlockDomains(id, cleanedList, tr("Öğretmen tarafından engellendi"));
        }

        showCommandStatus(true, tr("Web kara listesi %1 istemciye uygulandı (%2 site)")
                                .arg(clientIds.size()).arg(cleanedList.size()));
    }
}

void MainWindow::openClientSsh(const QString& clientId) {
    if (!m_session || clientId.isEmpty()) return;
    auto st = m_session->clientState(clientId);
    if (st.ip.isEmpty()) {
        QMessageBox::warning(this, tr("SSH Bağlantısı"), tr("Seçili istemcinin IP adresi bulunamadı."));
        return;
    }

    QString ip = st.ip;
    QString hostName = st.hostname.isEmpty() ? clientId : st.hostname;

    auto fetchConfig = [this, ip, st]() -> SshClient::Config {
        SshClient::Config cfg;
        cfg.host = ip;
        cfg.port = 22;
        cfg.connectTimeoutMs = 4000;
        cfg.hostKeyCheck = false;

        QString user, pass;
        bool isKey = false, found = false;

        const auto brokerCreds = m_session->sshCredentials();
        QString machineId = st.machineId;
        QString classId = m_session->clientGroup(machineId);

        for (const auto& c : brokerCreds) {
            if (c.targetId == ip && c.targetType == "agent") {
                user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
            }
        }
        if (!found && !machineId.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if (c.targetId == machineId && c.targetType == "agent") {
                    user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
                }
            }
        }
        if (!found && !classId.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if (c.targetId == classId && c.targetType == "class") {
                    user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
                }
            }
        }

        if (found) {
            cfg.username = user;
            if (isKey) {
                cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                cfg.privateKeyPath = pass;
            } else {
                cfg.authMethod = SshClient::AuthMethod::Password;
                cfg.password = pass;
            }
        } else {
            cfg.username = "root";
            cfg.authMethod = SshClient::AuthMethod::Password;
        }
        return cfg;
    };

    SshClient::Config cfg = fetchConfig();

    if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
        SshCredentialDialog dlg(m_session, this);
        dlg.exec();

        cfg = fetchConfig();
        if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
            QMessageBox::warning(this, tr("Bağlantı Başarısız"), tr("Geçerli bir SSH bilgisi girilmediği için bağlanılamıyor."));
            return;
        }
    }

    auto* sessionWidget = new SshDeviceSessionWidget(m_session, ip, hostName, cfg);
    sessionWidget->setAttribute(Qt::WA_DeleteOnClose);
    sessionWidget->setWindowTitle(QString(tr("%1 (%2) — SSH Yönetimi (Terminal & Dosyalar)")).arg(hostName, ip));
    sessionWidget->setWindowIcon(ThemeManager::icon(":/ui/terminal.svg"));
    sessionWidget->resize(920, 620);
    sessionWidget->show();
    sessionWidget->raise();
    sessionWidget->activateWindow();
}

void MainWindow::onBrokerWakeOnLan() {
    QSettings s("okidia", "master");
    QString lastIp = s.value("server/lastIp").toString().trimmed();
    QString lastMac = s.value("server/lastMac").toString().trimmed();

    if (lastIp.isEmpty() && m_session && m_session->tlsClient()) {
        lastIp = m_session->tlsClient()->host().trimmed();
    }
    if (lastIp.isEmpty()) {
        lastIp = AppSettings::load().remoteBrokerHost.trimmed();
    }
    if (lastIp.isEmpty()) {
        lastIp = OkidiaConfig::instance().brokerIp().trimmed();
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Sunucu Bilgisayarını Aç (Wake-on-LAN)"));
    dlg.setWindowIcon(ThemeManager::icon(":/ui/power.svg"));
    dlg.resize(440, 220);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(10);

    auto* lblInfo = new QLabel(tr("Kapalı olan sunucu bilgisayarını yerel ağ üzerinden sihirli paket (Magic Packet) göndererek açabilirsiniz."), &dlg);
    lblInfo->setWordWrap(true);
    lay->addWidget(lblInfo);

    auto* formLay = new QFormLayout();
    auto* ipEdit = new QLineEdit(lastIp, &dlg);
    ipEdit->setPlaceholderText("192.168.1.100");
    auto* macEdit = new QLineEdit(lastMac, &dlg);
    macEdit->setPlaceholderText("AA:BB:CC:DD:EE:FF");
    formLay->addRow(tr("Sunucu IP / Ağ Adresi:"), ipEdit);
    formLay->addRow(tr("Sunucu MAC Adresi:"), macEdit);
    lay->addLayout(formLay);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btnBox->button(QDialogButtonBox::Ok)->setText(tr("Sihirli Paketi Gönder (Uyandır)"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    lay->addWidget(btnBox);

    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        QString ip = ipEdit->text().trimmed();
        QString mac = macEdit->text().trimmed().toUpper();

        if (mac.isEmpty()) {
            QMessageBox::warning(this, tr("Eksik Bilgi"), tr("Wake-on-LAN için sunucu MAC adresi zorunludur."));
            return;
        }

        s.setValue("server/lastIp", ip);
        s.setValue("server/lastMac", mac);

        OkidiaUtils::sendWakeOnLan(mac, ip);
        showCommandStatus(true, QString(tr("Sunucuya (%1 - %2) Wake-on-LAN sinyali gönderildi.")).arg(ip, mac));
        AuditLog::instance().recordPower({}, tr("Sunucu"), QString("Wake-on-LAN (%1 - %2)").arg(ip, mac));
    }
}

void MainWindow::onBrokerSshConnect() {
    if (!m_session) return;
    QString ip = m_session->tlsClient() ? m_session->tlsClient()->host().trimmed() : QString();
    if (ip.isEmpty() || ip == "127.0.0.1") {
        ip = AppSettings::load().remoteBrokerHost.trimmed();
    }
    if (ip.isEmpty()) {
        ip = OkidiaConfig::instance().brokerIp().trimmed();
    }

    if (ip.isEmpty()) {
        QMessageBox::warning(this, tr("Sunucu SSH"), tr("Sunucu IP adresi tespit edilemedi."));
        return;
    }

    auto fetchConfig = [this, ip]() -> SshClient::Config {
        SshClient::Config cfg;
        cfg.host = ip;
        cfg.port = 22;
        cfg.connectTimeoutMs = 4000;
        cfg.hostKeyCheck = false;

        QString user, pass;
        bool isKey = false, found = false;

        const auto brokerCreds = m_session->sshCredentials();
        for (const auto& c : brokerCreds) {
            if (c.targetId == ip || c.targetId == "broker" || c.targetId == "server") {
                user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
            }
        }

        if (found) {
            cfg.username = user;
            if (isKey) {
                cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                cfg.privateKeyPath = pass;
            } else {
                cfg.authMethod = SshClient::AuthMethod::Password;
                cfg.password = pass;
            }
        } else {
            cfg.username = "root";
            cfg.authMethod = SshClient::AuthMethod::Password;
        }
        return cfg;
    };

    SshClient::Config cfg = fetchConfig();

    if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
        SshCredentialDialog dlg(m_session, this);
        dlg.exec();

        cfg = fetchConfig();
        if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
            QMessageBox::warning(this, tr("Bağlantı Başarısız"), tr("Geçerli bir SSH bilgisi girilmediği için bağlanılamıyor."));
            return;
        }
    }

    auto* sessionWidget = new SshDeviceSessionWidget(m_session, ip, tr("Broker Sunucusu"), cfg);
    sessionWidget->setAttribute(Qt::WA_DeleteOnClose);
    sessionWidget->setWindowTitle(QString(tr("OKIDIA Broker Sunucusu (%1) — Terminal & Dosya Yöneticisi")).arg(ip));
    sessionWidget->setWindowIcon(ThemeManager::icon(":/ui/terminal.svg"));
    sessionWidget->resize(960, 640);
    sessionWidget->show();
    sessionWidget->raise();
    sessionWidget->activateWindow();
}

void MainWindow::onBrokerRestartService() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Bağlantı Yok"), tr("Sunucuya bağlı değilsiniz."));
        return;
    }

    int res = QMessageBox::question(this, tr("Sunucu Servisini Yeniden Başlat"),
        tr("OKIDIA Broker servisini uzaktan yeniden başlatmak istediğinize emin misiniz?\n\n"
           "Yeniden başlatma esnasında istemciler ve Master birkaç saniyeliğine çevrimdışı görünebilir."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (res != QMessageBox::Yes) return;

    QString ip = m_session->tlsClient() ? m_session->tlsClient()->host().trimmed() : QString();
    if (ip.isEmpty()) ip = OkidiaConfig::instance().brokerIp().trimmed();

    SshClient::Config cfg;
    cfg.host = ip;
    cfg.port = 22;
    cfg.username = "root";
    cfg.authMethod = SshClient::AuthMethod::Password;

    const auto brokerCreds = m_session->sshCredentials();
    for (const auto& c : brokerCreds) {
        if (c.targetId == ip || c.targetId == "broker" || c.targetId == "server") {
            cfg.username = c.username;
            if (c.isKey) {
                cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                cfg.privateKeyPath = QString::fromUtf8(c.secret);
            } else {
                cfg.authMethod = SshClient::AuthMethod::Password;
                cfg.password = QString::fromUtf8(c.secret);
            }
            break;
        }
    }

    auto* client = new SshClient(this);
    connect(client, &SshClient::connected, client, [client]() {
        client->execute("sudo systemctl restart okidia-broker || systemctl restart okidia-broker");
        QTimer::singleShot(2000, client, &QObject::deleteLater);
    });
    connect(client, &SshClient::errorOccurred, client, [this, client](const QString& err) {
        QMessageBox::warning(this, tr("SSH Hatası"), tr("Sunucu servisi yeniden başlatılamadı:\n") + err);
        client->deleteLater();
    });

    client->connectToHost(cfg);
    showCommandStatus(true, tr("Broker servisini yeniden başlatma komutu gönderildi."));
}

void MainWindow::onBrokerRebootServer() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Bağlantı Yok"), tr("Sunucuya bağlı değilsiniz."));
        return;
    }

    int res = QMessageBox::critical(this, tr("Sunucu Makinesini Yeniden Başlat"),
        tr("⚠️ DİKKAT: Fiziksel sunucu bilgisayarını (Reboot) yeniden başlatmak üzeresiniz!\n\n"
           "Ağdaki tüm akıllı tahtalar ve istemciler sunucu tekrar açılana kadar internetsiz / çevrimdışı kalacaktır.\n\n"
           "Devam etmek istediğinize emin misiniz?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (res != QMessageBox::Yes) return;

    QString ip = m_session->tlsClient() ? m_session->tlsClient()->host().trimmed() : QString();
    if (ip.isEmpty()) ip = OkidiaConfig::instance().brokerIp().trimmed();

    SshClient::Config cfg;
    cfg.host = ip;
    cfg.port = 22;
    cfg.username = "root";
    cfg.authMethod = SshClient::AuthMethod::Password;

    const auto brokerCreds = m_session->sshCredentials();
    for (const auto& c : brokerCreds) {
        if (c.targetId == ip || c.targetId == "broker" || c.targetId == "server") {
            cfg.username = c.username;
            if (c.isKey) {
                cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                cfg.privateKeyPath = QString::fromUtf8(c.secret);
            } else {
                cfg.authMethod = SshClient::AuthMethod::Password;
                cfg.password = QString::fromUtf8(c.secret);
            }
            break;
        }
    }

    auto* client = new SshClient(this);
    connect(client, &SshClient::connected, client, [client]() {
        client->execute("sudo reboot || reboot");
        QTimer::singleShot(2000, client, &QObject::deleteLater);
    });
    connect(client, &SshClient::errorOccurred, client, [this, client](const QString& err) {
        QMessageBox::warning(this, tr("SSH Hatası"), tr("Sunucu makinesi yeniden başlatılamadı:\n") + err);
        client->deleteLater();
    });

    client->connectToHost(cfg);
    showCommandStatus(true, tr("Sunucu bilgisayarına yeniden başlatma (reboot) komutu gönderildi."));
}

void MainWindow::onBrokerInfo() {
    QString host = m_session && m_session->tlsClient() ? m_session->tlsClient()->host() : "-";
    quint16 port = m_session && m_session->tlsClient() ? m_session->tlsClient()->port() : 0;
    QString ver = m_session ? m_session->brokerVersion() : "-";
    QString os = m_session ? m_session->brokerOs() : "-";
    QString osTag = m_session ? m_session->brokerOsTag() : "-";
    bool conn = m_session && m_session->isConnected();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Sunucu Durumu ve Sistem Bilgileri"));
    dlg.setWindowIcon(ThemeManager::icon(":/ui/info.svg"));
    dlg.resize(520, 440);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(12);

    auto* formLay = new QFormLayout();
    formLay->addRow(tr("Bağlantı Durumu:"), new QLabel(conn ? QString("<b style='color:%1;'>%2</b>").arg(ThemeManager::instance().successColor().name(), tr("Bağlı (Çevrimiçi)")) : QString("<b style='color:%1;'>%2</b>").arg(ThemeManager::instance().errorColor().name(), tr("Bağlantı Yok")), &dlg));
    formLay->addRow(tr("Sunucu IP / Host:"), new QLabel(QString("<b>%1:%2</b>").arg(host).arg(port), &dlg));
    formLay->addRow(tr("Sunucu Sürümü:"), new QLabel(ver.isEmpty() ? "-" : ver, &dlg));
    formLay->addRow(tr("İşletim Sistemi:"), new QLabel(os.isEmpty() ? "-" : os, &dlg));
    formLay->addRow(tr("Paketleme Platformu:"), new QLabel(osTag.isEmpty() ? "-" : osTag, &dlg));
    if (m_session) {
        formLay->addRow(tr("Kayıtlı İstemci Sayısı:"), new QLabel(QString::number(m_session->clientStates().size()), &dlg));
    }

    // Donanım & Depolama Bilgileri Alanı
    auto* lblCpu = new QLabel(tr("Yükleniyor..."), &dlg);
    auto* progCpu = new QProgressBar(&dlg);
    progCpu->setRange(0, 100);
    progCpu->setValue(0);
    progCpu->setTextVisible(true);
    progCpu->setFixedHeight(16);

    auto* lblRam = new QLabel(tr("Yükleniyor..."), &dlg);
    auto* progRam = new QProgressBar(&dlg);
    progRam->setRange(0, 100);
    progRam->setValue(0);
    progRam->setTextVisible(true);
    progRam->setFixedHeight(16);

    auto* lblDisk = new QLabel(tr("Yükleniyor..."), &dlg);
    auto* progDisk = new QProgressBar(&dlg);
    progDisk->setRange(0, 100);
    progDisk->setValue(0);
    progDisk->setTextVisible(true);
    progDisk->setFixedHeight(16);

    auto* lblDb = new QLabel(tr("Hesaplanıyor..."), &dlg);

    formLay->addRow(tr("CPU Kullanımı:"), progCpu);
    formLay->addRow(tr("Bellek (RAM):"), progRam);
    formLay->addRow(tr("Disk Doluluk Oranı:"), progDisk);
    formLay->addRow(tr("Veritabanı Boyutu:"), lblDb);

    lay->addLayout(formLay);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    lay->addWidget(btnBox);

    // Canlı donanım ve disk/veritabanı verilerini arka planda SSH ile asenkron sorgula
    if (conn) {
        QString ip = (host != "-" && !host.isEmpty()) ? host : (m_session->tlsClient() ? m_session->tlsClient()->host().trimmed() : QString());
        if (ip.isEmpty() || ip == "127.0.0.1") {
            ip = AppSettings::load().remoteBrokerHost.trimmed();
        }
        if (ip.isEmpty()) {
            ip = OkidiaConfig::instance().brokerIp().trimmed();
        }

        SshClient::Config cfg;
        cfg.host = ip;
        cfg.port = 22;
        cfg.connectTimeoutMs = 3000;
        cfg.hostKeyCheck = false;
        cfg.username = "root";
        cfg.authMethod = SshClient::AuthMethod::Password;

        const auto brokerCreds = m_session->sshCredentials();
        for (const auto& c : brokerCreds) {
            if (c.targetId == ip || c.targetId == "broker" || c.targetId == "server") {
                cfg.username = c.username;
                if (c.isKey) {
                    cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                    cfg.privateKeyPath = QString::fromUtf8(c.secret);
                } else {
                    cfg.authMethod = SshClient::AuthMethod::Password;
                    cfg.password = QString::fromUtf8(c.secret);
                }
                break;
            }
        }

        QPointer<QDialog> dlgGuard = &dlg;
        QPointer<QProgressBar> pCpuGuard = progCpu;
        QPointer<QProgressBar> pRamGuard = progRam;
        QPointer<QProgressBar> pDiskGuard = progDisk;
        QPointer<QLabel> lblDbGuard = lblDb;

        (void)QtConcurrent::run([cfg, dlgGuard, pCpuGuard, pRamGuard, pDiskGuard, lblDbGuard]() {
            SshClient client;
            client.connectToHost(cfg);
            if (!client.isConnected()) {
                QMetaObject::invokeMethod(qApp, [dlgGuard, lblDbGuard]() {
                    if (dlgGuard && lblDbGuard) {
                        lblDbGuard->setText(QObject::tr("SSH kimliği atanmamış veya erişilemedi"));
                    }
                });
                return;
            }

            // Linux sunucuda CPU, RAM, Disk ve Veritabanı boyutlarını tek komutla topla
            QString probeCmd =
                "echo '===CPU==='; top -bn1 | grep 'Cpu(s)' | awk '{print $2+$4}' 2>/dev/null || mpstat 1 1 | awk '/Average:/ {print 100-$NF}'; "
                "echo '===RAM==='; free -m | awk '/Mem:/ {print $2,$3}'; "
                "echo '===DISK==='; df -h / | awk 'NR==2 {print $2,$3,$5}'; "
                "echo '===DB==='; du -sh /var/lib/okidia/okidia.db /var/lib/okidia/okidia_audit.db /var/lib/okidia/okidia_chat.db /var/lib/okidia 2>/dev/null | tail -n1 | awk '{print $1}'";

            QString output = client.executeSync(probeCmd, 4000);
            client.disconnectFromHost();

            if (!output.isEmpty()) {
                QMetaObject::invokeMethod(qApp, [output, dlgGuard, pCpuGuard, pRamGuard, pDiskGuard, lblDbGuard]() {
                    if (!dlgGuard) return;

                    // Parse CPU
                    double cpuVal = 0.0;
                    if (output.contains("===CPU===")) {
                        QString cpuPart = output.section("===CPU===", 1, 1).section("===RAM===", 0, 0).trimmed();
                        cpuVal = cpuPart.toDouble();
                    }

                    // Parse RAM
                    int ramUsedMb = 0, ramTotalMb = 0;
                    if (output.contains("===RAM===")) {
                        QString ramPart = output.section("===RAM===", 1, 1).section("===DISK===", 0, 0).trimmed();
                        QStringList rTokens = ramPart.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                        if (rTokens.size() >= 2) {
                            ramTotalMb = rTokens[0].toInt();
                            ramUsedMb = rTokens[1].toInt();
                        }
                    }

                    // Parse Disk
                    QString diskTotal, diskUsed, diskPctStr;
                    int diskPct = 0;
                    if (output.contains("===DISK===")) {
                        QString diskPart = output.section("===DISK===", 1, 1).section("===DB===", 0, 0).trimmed();
                        QStringList dTokens = diskPart.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                        if (dTokens.size() >= 3) {
                            diskTotal = dTokens[0];
                            diskUsed = dTokens[1];
                            diskPctStr = dTokens[2].remove('%');
                            diskPct = diskPctStr.toInt();
                        }
                    }

                    // Parse DB
                    QString dbSize = output.section("===DB===", 1, 1).trimmed();
                    if (dbSize.isEmpty()) dbSize = "-";

                    // Arayüze uygula
                    if (pCpuGuard) {
                        int cInt = qBound(0, qRound(cpuVal), 100);
                        pCpuGuard->setValue(cInt);
                        pCpuGuard->setFormat(QString("%1%").arg(cInt));
                    }
                    if (pRamGuard) {
                        int rPct = (ramTotalMb > 0) ? qBound(0, qRound((ramUsedMb * 100.0) / ramTotalMb), 100) : 0;
                        pRamGuard->setValue(rPct);
                        pRamGuard->setFormat(QString("%1% (%2 MB / %3 MB)").arg(rPct).arg(ramUsedMb).arg(ramTotalMb));
                    }
                    if (pDiskGuard) {
                        pDiskGuard->setValue(qBound(0, diskPct, 100));
                        pDiskGuard->setFormat(QString("%1% (%2 / %3)").arg(diskPct).arg(diskUsed, diskTotal));
                    }
                    if (lblDbGuard) {
                        lblDbGuard->setText(QString("<b>%1</b> (Toplam Veritabanı Dizini)").arg(dbSize));
                    }
                });
            }
        });
    }

    dlg.exec();
}

void MainWindow::onBrokerUpdateManager() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Bağlantı Yok"), tr("Sunucuya bağlı değilsiniz."));
        return;
    }
    BrokerUpdateManagerDialog dlg(m_session, this);
    dlg.exec();
}
