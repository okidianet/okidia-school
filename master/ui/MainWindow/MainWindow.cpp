// master/ui/MainWindow/MainWindow.cpp
// Core: constructor, setup, temel arayüz grid işlemleri.
// Not: Bu sınıfın implementasyonu çok sayıda dosyaya bölünmüştür:
//   MainWindowLayout.cpp, MainWindowCommands.cpp, MainWindowChat.cpp vb.
#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "../../chat/RoomManager.h"
#include "../../chat/PrivateChatManager.h"
#include "../../ssh/SshManagerPanel.h"
#include "../../ssh/DeployPanel.h"
#include "../../ssh/OtaUpdatePanel.h"
#include "../../transfer/FileSender.h"
#include "../ChatWindow/ChatWindow.h"
#include "../BroadcastOverlay/BroadcastOverlayWidget.h"
#include "../Widgets/MasterUpdateDialog.h"
#include "../../session/SessionManager.h"
#include <utils/Logger.h>

#include <QMenu>
#include "../../../common/ui/ThemeManager.h"
#include <QSettings>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include "../../common/utils/OkidiaUtils.h"
#include <QDateTime>
#include <QTimeZone>
#include <QSet>
#include <QApplication>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QFile>
#include <QBuffer>
#include <QResizeEvent>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QWidgetAction>
#include <QListWidget>
#include <QPushButton>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include "../AnnounceRecordDialog/AnnounceRecordDialog.h"
#include <QLabel>
#include <QLineEdit>
#include <QDrag>
#include <QMimeData>
#include <QPlainTextEdit>
#include "../../ssh/DeployPanel.h"
#include "../../ssh/SshManagerPanel.h"
#include "../UserManagementPanel/UserManagementPanel.h"
#include <QHostInfo>
#include <QHeaderView>
#include <QTabWidget>

MainWindow::MainWindow(SessionManager* session, QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_session(session) {

    ui->setupUi(this);

    // FPS Combo Box Sıralı Liste (Duraklatıldı, 0.2, 0.5, 1, 5, 10, 15, Adaptif)
    ui->fpsCombo->clear();
    ui->fpsCombo->addItem(tr("Duraklatıldı (0 FPS)"), 0);
    ui->fpsCombo->addItem(tr("0.2 FPS (5 sn)"), 5000);
    ui->fpsCombo->addItem(tr("0.5 FPS (2 sn)"), 2000);
    ui->fpsCombo->addItem(tr("1 FPS (1 sn)"), 1000);
    ui->fpsCombo->addItem(tr("5 FPS (200 ms)"), 200);
    ui->fpsCombo->addItem(tr("10 FPS (100 ms)"), 100);
    ui->fpsCombo->addItem(tr("15 FPS (67 ms)"), 67);
    ui->fpsCombo->addItem(tr("Otomatik (Adaptif)"), -1);

    // QSettings ile kayıtlı FPS değerini yükle
    QSettings settings("okidia", "master");
    int savedInterval = settings.value("ui/fps_interval", 1000).toInt();
    bool savedPaused = settings.value("ui/fps_paused", false).toBool();

    int defaultIndex = 3; // 1 FPS (Fallback)
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

    setupConnections();
    setupMainMenu();
    setupSplitter();
    if (OkidiaDatabase::instance().isOpen()) {
        syncUiWithDatabase();
    }
    setupTreeDock();   // Tree'yi dock'a taşı
    setupDiscoveryPanel(); // Discovery panelini hazırla

    // Toolbar butonlarına ikon atama
    ui->btnLockToggle->setIcon(ThemeManager::icon(":/ui/lock.svg"));
    ui->btnBroadcast->setIcon(ThemeManager::icon(":/ui/broadcast.svg"));
    ui->btnExam->setIcon(ThemeManager::icon(":/ui/student.svg"));
    ui->btnSendMessage->setIcon(ThemeManager::icon(":/ui/chat.svg"));
    ui->btnSendFile->setIcon(ThemeManager::icon(":/ui/file.svg"));
    ui->btnAudio->setIcon(ThemeManager::icon(":/ui/volume.svg"));
    ui->btnSchedule->setIcon(ThemeManager::icon(":/ui/clock.svg"));
    ui->btnReport->setIcon(ThemeManager::icon(":/ui/chart.svg"));
    ui->btnChat->setIcon(ThemeManager::icon(":/ui/chat.svg"));

    ui->btnExam->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->btnAudio->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->btnPower->setIcon(ThemeManager::icon(":/ui/power.svg"));
    ui->btnPower->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // Discovery sinyalini bağla
    connect(m_session, &SessionManager::discoveryRequest,
            this, &MainWindow::onDiscoveryRequest);
    connect(m_session, &SessionManager::discoveryRevoked,
            this, &MainWindow::onDiscoveryRevoked);
    connect(m_session, &SessionManager::discoveryResolved,
            this, &MainWindow::onDiscoveryResolved);
    // setupDock() ChatRoomManager oluşturulduktan sonra çağrılır
    updateStatusBar({});
    setupStatusBarTargetBadge();
    setupStatusBarNetworkMode();
    setupSessionSecurity();
    qApp->installEventFilter(this);

    // Session sinyallerini bağla
    connect(m_session, &SessionManager::brokerConnected, this, [this]() {
        statusBar()->showMessage(tr("Sunucu bağlandı"));
        updateNetworkModeBadge();
    });

    connect(m_session, &SessionManager::brokerDisconnected, this, [this]() {
        statusBar()->showMessage(tr("Sunucu bağlantısı kesildi"));
        if (!m_session->isKickedToOffline()) {
            applyOfflineModeConstraints(true);
        }
        updateNetworkModeBadge();
    });

    connect(m_session, &SessionManager::connectionModeChanged, this, [this](SessionManager::ConnectionMode) {
        updateNetworkModeBadge();
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        updateNetworkModeBadge();
        updateSelectionTargetBadge();
    });

    connect(m_session, &SessionManager::databaseReloaded, this, &MainWindow::syncUiWithDatabase);

    connect(m_session, &SessionManager::schoolNameChanged, this, &MainWindow::updateWindowTitle);

    // Delege süresi dolunca tree + thumbnail görünümünü güncelle
    connect(m_session, &SessionManager::delegationExpired, this, [this]() {
        statusBar()->showMessage(tr("Yetki süresi doldu — görünüm güncellendi"), 5000);
        applyOMPermissions();
    });

    connect(m_session, &SessionManager::screenCaptureReceived,
            this, &MainWindow::onScreenCaptureReceived);

    // RemoteControlView için tile güncelleme (bağlantı açıksa)
    connect(m_session, &SessionManager::clientTileReceived, this,
            [this](const QString& clientId,
                    int x, int y, int w, int h,
                    const QByteArray& data) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId) {
                view->updateTile(x, y, w, h, data);
            }
        }
    });

    connect(m_session, &SessionManager::clientTileBatchFinished, this,
            [this](const QString& clientId) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId) {
                view->refreshDisplay();
            }
        }
    });

    connect(m_session, &SessionManager::clientStreamSizeReceived, this,
            [this](const QString& clientId, int w, int h) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId) {
                view->setStreamSize(w, h);
            }
        }
    });

    connect(m_session, &SessionManager::clientFrameReceived, this,
            [this](const QString& clientId, const QByteArray& data) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId) {
                view->updateFrame(data);
            }
        }
    });

    // Offline Mod Senkronizasyon
    connect(m_session, &SessionManager::switchedToOnlineMode, this, [this]() {
        applyOfflineModeConstraints(false);
        statusBar()->showMessage(tr("Sunucu bağlandı"));
        updateNetworkModeBadge();

        // Broker'a okul ders programını, saat dilimini ve kilit politikasını otonom senkronize et
        // (Yalnızca yetkili YM oturumu Broker tarafından doğrulandıktan sonra!)
        if (m_session && m_session->isConnected() && m_session->isYM() && !m_session->isOfflineMode()) {
            QString schedJson = OkidiaConfig::exportSchoolConfigJson();
            QString tzId = QString::fromUtf8(QTimeZone::systemTimeZone().id());
            AppSettings s = AppSettings::load();
            uint32_t lockPol = (s.smartboardLockPolicy == 0 ? 1 : 2);
            m_session->sendBrokerSettingsUpdate(s.auditRetention, s.backupRetention, s.autoBackupIncludeChat, 
                                                s.autoBackupPassword, schedJson, tzId, lockPol);
            Logger::info(QObject::tr("MainWindow: Broker bağlantısı ve YM oturumu doğrulandı; okul ders programı, saat dilimi (%1) ve kilit politikası (%2) otonom senkronize edildi.")
                         .arg(tzId).arg(lockPol));
        }
    });

    connect(m_session, &SessionManager::offlineLoginFailed, this, [this](const QString& reason) {
        Logger::warning(QObject::tr("MainWindow: Arka planda sunucu girişi yapılamadı: ") + reason);
        statusBar()->showMessage(tr("Arka plan sunucu bağlantısı kurulamadı: %1").arg(reason), 5000);
        applyOfflineModeConstraints(true);
    });

    connect(m_session, &SessionManager::kickedToOfflineMode, this, [this]() {
        QMessageBox::warning(this, tr("Oturum Kısıtlandı"), tr("Hesabınızla başka bir bilgisayardan giriş yapıldı. Mevcut oturumunuz kısıtlı (çevrimdışı) moda düşürüldü. Sadece arka plan işlemleri ve SSH özellikleri aktif kalacaktır."));
        applyOfflineModeConstraints(true);
    });

    // Uygulama listesi
    connect(m_session, &SessionManager::appListReceived, this,
            [this](const QString& clientId,
                   const QList<QPair<QString,QString>>& apps) {
        m_appLists[clientId] = apps;
    });

    // Uzak kontrol stream — ilgili tüm pencerelere ilet
    connect(m_session, &SessionManager::remoteFrameReceived, this,
            [this](const QString& clientId, const QByteArray& data,
                   int w, int h, int cx, int cy, bool hasCursor) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId) {
                view->updateRemoteFrame(data, w, h);
                if (hasCursor) {
                    view->updateRemoteCursor(0, cx, cy);
                }
            }
        }
    });

    connect(m_session, &SessionManager::cursorShapeChanged, this,
            [this](const QString& clientId, int cursorId, int x, int y) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId)
                view->updateRemoteCursor(cursorId, x, y);
        }
    });

    // İstemci durum/kilit değişimi: Eğer aktif izlenen bir cihazda ekran yayını başladıysa uzaktan kontrol penceresine hafif rozet düşür
    connect(m_session, &SessionManager::clientStateChanged, this,
            [this](const QString& clientId, const ClientState& st) {
        for (auto* view : m_remoteViews) {
            if (view->clientId() == clientId) {
                if (st.lockType == "Broadcast") {
                    QString teacher = st.lockedByDisplayName.isEmpty() ? st.lockedBy : st.lockedByDisplayName;
                    view->setBroadcastNotice(true, teacher);
                } else {
                    view->setBroadcastNotice(false);
                }
            }
        }
    });

    // Faz 5: Sohbet ve el kaldırma sinyalleri
    connect(m_session, &SessionManager::chatMessageReceived,
            this, &MainWindow::onChatMessageReceived);
    connect(m_session, &SessionManager::handRaiseReceived,
            this, &MainWindow::onHandRaiseReceived);
    connect(m_session, &SessionManager::handLowerReceived,
            this, &MainWindow::onHandLowerReceived);


    // Çoklu monitör: ekran bilgisi
    connect(m_session, &SessionManager::screenInfoReceived,
            this, &MainWindow::onScreenInfoReceived);
    
    // Uzak kontrol onay yanıtı
    connect(m_session, &SessionManager::remoteControlResponseReceived,
            this, &MainWindow::onRemoteControlResponseReceived);

    // RoomManager — yeni sohbet odası yönetimi (TreeManager hazır olmalı)
    m_roomMgr = new RoomManager(m_treeManager, m_session, this);
    m_privateChatMgr = new PrivateChatManager(m_session, this);

    // Kilit durumu değişince tree emojilerini güncelle
    connect(m_roomMgr, &RoomManager::lockStateChanged,
            [this](const QString& classNodeId, bool chatLocked, bool privateLocked) {
        updateTreeLockEmojis(classNodeId, chatLocked, privateLocked);
    });

    // ChatWindow — bağımsız sohbet penceresi (toolbar butonuyla açılır, bağımsız top-level)
    m_chatWindow = new ChatWindow(m_roomMgr, m_privateChatMgr, m_session, nullptr);
    m_chatWindow->setWindowIcon(ThemeManager::icon(":/ui/chat.svg"));

    // Özel sohbet: SessionManager → PrivateChatManager
    connect(m_session, &SessionManager::privateChatRequested, this,
            [this](const QString& fromId, const QString& fromDisplay,
                   const QString& fromHost, const QString& fromUser,
                   const QString& requestId) {
        if (m_privateChatMgr)
            m_privateChatMgr->onIncomingRequest(fromId, fromDisplay,
                                                 fromHost, fromUser, requestId);
    });
    connect(m_session, &SessionManager::privateChatAccepted, this,
            [this](const QString& requestId, const QString& sessionId) {
        if (m_privateChatMgr)
            m_privateChatMgr->onRequestAccepted(requestId, sessionId);
    });
    connect(m_session, &SessionManager::privateChatOpenedRemotely, this,
            [this](const QString& sessionId, const QString& fromId,
                   const QString& fromDisplay, const QString& fromHost) {
        if (m_privateChatMgr)
            m_privateChatMgr->onIncomingOpen(sessionId, fromId, fromDisplay, fromHost);
    });
    connect(m_session, &SessionManager::privateChatRejected, this,
            [this](const QString& requestId, int /*cooldown*/) {
        if (m_privateChatMgr)
            m_privateChatMgr->onRequestRejected(requestId);
    });
    connect(m_session, &SessionManager::privateChatMessageReceived, this,
            [this](const QString& sessionId, const QString& content,
                   qint64 timestamp) {
        if (m_privateChatMgr)
            m_privateChatMgr->onIncomingMessage(sessionId, content, timestamp);
    });
    connect(m_session, &SessionManager::privateChatClosed, this,
            [this](const QString& sessionId) {
        if (m_privateChatMgr)
            m_privateChatMgr->onSessionClosed(sessionId);
    });

    // Agent sohbet penceresi açık/kapalı → RoomManager'a bildir
    connect(m_session, &SessionManager::agentChatWindowChanged, this,
            [this](const QString& clientId, bool open) {
        if (m_roomMgr) m_roomMgr->onAgentWindowState(clientId, open);
    });

    // Özel sohbet isteği gelince: tray bildirim + ChatWindow'da göster
    connect(m_privateChatMgr, &PrivateChatManager::requestReceived,
            [this](const PendingRequest& req) {
        // Özel sohbet isteği gelince: tray badge (sonraki fazda implemente edilecek)
        Q_UNUSED(req)
    });

    // Master güncelleme bildirimi
    connect(m_session, &SessionManager::masterUpdateAvailableReceived,
            this, &MainWindow::onMasterUpdateAvailable);

    // Broker eski sürümde kaldığında YM uyarısı
    connect(m_session, &SessionManager::brokerOutdatedWarning,
            this, &MainWindow::onBrokerOutdatedWarning);

    // Zaman Senkronizasyonu & Donanım Arıza Bildirimleri
    connect(m_session, &SessionManager::brokerTimeWarningReceived,
            this, &MainWindow::onBrokerTimeWarning);
    connect(m_session, &SessionManager::clientHardwareAlertReceived,
            this, &MainWindow::onClientHardwareAlert);
    connect(m_session, &SessionManager::brokerStorageWarningReceived,
            this, &MainWindow::onBrokerStorageWarning);

    // Yetki / Rol Değişikliği Bildirimi (Re-login)
    connect(m_session, &SessionManager::userRoleChangedReceived,
            this, &MainWindow::onUserRoleChanged);

    // Akıllı Tahta Dev PIN Tooltip Bildirimi
    connect(m_session, &SessionManager::smartboardLockPinReceived, this,
            [this](const QString& clientId, const QString& pin, int remainingSeconds) {
        if (m_treeManager) {
            m_treeManager->setClientDevPin(clientId, pin, remainingSeconds);
        }
    });

    // Akıllı Tahta PIN ile Açılınca Master Durumunu Eşitle
    connect(m_session, &SessionManager::smartboardUnlockedReceived, this,
            [this](const QString& clientId, const QString& username, const QString& displayName) {
        if (m_treeManager) {
            m_treeManager->setClientDevPin(clientId, QString(), 0);
        }
        updateLockToggleState();
        QString teacher = displayName.isEmpty() ? username : displayName;
        showCommandStatus(true, tr("Akıllı Tahta (%1) kilidi %2 tarafından PIN ile açıldı.").arg(clientId, teacher));
    });

    connect(m_session, &SessionManager::agentTypeSetResult, this,
            [this](const QString& clientId, bool approved, const QString& reason) {
        QString hostname = clientId;
        if (m_session) hostname = m_session->clientState(clientId).hostname;
        
        if (approved) {
            showCommandStatus(true, hostname + tr(" için tip değişikliği onaylandı."));
            if (m_treeManager && m_session) {
                m_treeManager->setClientAgentType(clientId, m_session->clientState(clientId).agentType);
            }
        } else {
            QMessageBox::warning(this, tr("Değişiklik Reddedildi"),
                hostname + tr(" kullanıcısı tip değişikliğini reddetti.\n\nSebep: ") + reason);
            showCommandStatus(false, hostname + tr(" değişikliği reddetti."));
        }
    });

    connect(m_session, &SessionManager::commandResultReceived, this,
            [this](const QString& clientId, bool success, const QString& errorMsg) {
        QString hostname = clientId;
        if (m_session) hostname = m_session->clientState(clientId).hostname;
        if (!success) {
            showCommandStatus(false, hostname + ": " + errorMsg);
        }
    });

    connect(m_session, &SessionManager::dbRestoreResult, this,
            [this](bool success, const QString& errorMsg) {
        if (success) {
            QMessageBox::information(this, tr("Geri Yükleme"),
                tr("Veritabanı ve güvenlik sertifikaları başarıyla geri yüklendi.\n"
                "Sunucu servisi yeni verilerle yeniden başlatılıyor."));
        } else {
            QMessageBox::critical(this, tr("Geri Yükleme Hatası"),
                tr("Veritabanı geri yüklenemedi!\n\nHata: ") + errorMsg);
        }
    });

    connect(m_session, &SessionManager::systemBackupFinished, this,
            [this](bool success, const QString& filePath, const QString& errorMsg) {
        if (success) {
            QMessageBox::information(this, tr("Sistem Yedeği Alındı"),
                tr("OKIDIA sistem yedeği başarıyla oluşturuldu:\n\n%1\n\nBu tek dosyayı (.okbak) güvenli bir harici bellekte veya bulut ortamında saklayabilirsiniz.").arg(filePath));
            showCommandStatus(true, tr("Sistem yedeği başarıyla kaydedildi."));
        } else {
            QMessageBox::critical(this, tr("Yedekleme Hatası"),
                tr("Sistem yedeği oluşturulamadı!\n\nHata: ") + errorMsg);
            showCommandStatus(false, tr("Yedekleme başarısız."));
        }
    });

    connect(m_session, &SessionManager::backupReplicaReceived, this, [this](const QString& filePath) {
        QFileInfo fi(filePath);
        statusBar()->showMessage(tr("Sistem veritabanı yedeği güvenle alındı: %1").arg(fi.fileName()), 8000);
        showCommandStatus(true, tr("Sistem veritabanı yedeği senkronize edildi."));
    });

    connect(m_session, &SessionManager::brokerPortChanged, this,
            [this](bool success, quint16 newPort, const QString& errorMsg) {
        if (success) {
            QMessageBox::information(this, tr("Sunucu Port Güncellendi"),
                tr("Sunucu portu başarıyla %1 olarak güncellendi.\n"
                   "Sunucu yeni port üzerinden hizmet vermeye başladı.").arg(newPort));
        } else {
            QMessageBox::critical(this, tr("Sunucu Port Hatası"),
                tr("Sunucu portu güncellenemedi!\n\nHata: ") + errorMsg);
        }
    });

    setupDock();       // Sadece Deploy dock'u kurulur artık

    // --- Çevrimdışı Mod Kontrolü ---
    if (m_session && m_session->isOfflineMode()) {
        applyOfflineModeConstraints(true);
    }

    // ScreenBroadcaster
    m_broadcaster = new ScreenBroadcaster(m_session->tlsClient() ? m_session->tlsClient()->connection() : nullptr, this);
    connect(m_broadcaster, &ScreenBroadcaster::started, this, [this]() {
        ui->btnBroadcast->setChecked(true);
        ui->btnBroadcast->setIcon(ThemeManager::icon(":/ui/stop.svg"));
        ui->btnBroadcast->setText(tr("Yayını Durdur"));
        ui->btnBroadcast->setToolTip(tr("Ekran yayınını durdur"));
        // Yayın süresince ağ tasarrufu için thumbnail isteklerini geçici olarak durdur (0 FPS)
        if (m_session) m_session->broadcastScreenConfig(0, 0);

        // Kayan canlı yayın kontrol çubuğunu aç
        if (!m_broadcastOverlay) {
            m_broadcastOverlay = new BroadcastOverlayWidget(this);
            connect(m_broadcastOverlay, &BroadcastOverlayWidget::qualityChanged,
                    m_broadcaster, &ScreenBroadcaster::setQuality);
            connect(m_broadcastOverlay, &BroadcastOverlayWidget::stopRequested,
                    this, &MainWindow::stopBroadcast);
        }

        QScreen* targetScreen = nullptr;
        if (m_broadcaster->currentSource().type == BroadcastSource::Monitor) {
            const auto screens = QGuiApplication::screens();
            int idx = m_broadcaster->currentSource().monitorIndex;
            if (idx >= 0 && idx < screens.size()) {
                targetScreen = screens[idx];
            }
        }
        m_broadcastOverlay->setTargetScreen(targetScreen);
        m_broadcastOverlay->setInitialQuality(m_broadcaster->quality());
        m_broadcastOverlay->showOverlay();
    });
    connect(m_broadcaster, &ScreenBroadcaster::stopped, this, [this]() {
        ui->btnBroadcast->setChecked(false);
        ui->btnBroadcast->setIcon(ThemeManager::icon(":/ui/broadcast.svg"));
        ui->btnBroadcast->setText(tr("Yayın"));
        ui->btnBroadcast->setToolTip(tr("Ekran yayını başlat"));
        showCommandStatus(true, tr("Ekran yayını durduruldu"));
        // Yayın bitince normal thumbnail akışını geri yükle
        if (m_session) m_session->updateAdaptiveFps();

        // Kayan kontrol çubuğunu gizle
        if (m_broadcastOverlay) {
            m_broadcastOverlay->hideOverlay();
        }
    });

    connect(m_session, &SessionManager::brokerConnected, this, [this]() {
        statusBar()->showMessage(tr("Sunucu bağlandı"));
        if (m_broadcaster && m_session->tlsClient()) {
            m_broadcaster->setConnection(m_session->tlsClient()->connection());
        }
    });

    // ScheduleManager
    m_scheduler = new ScheduleManager(m_session, this);
    connect(m_scheduler, &ScheduleManager::taskExecuted,
            [this](const QString&, const QString& label) {
        showCommandStatus(true, label + " " + tr("çalıştırıldı"));
    });

    // Rubber band seçimi — thumbnailContainer mouse eventlerini dinle
    ui->thumbnailContainer->installEventFilter(this);
    ui->thumbnailScrollArea->installEventFilter(this);
    ui->thumbnailContainer->setMouseTracking(true);
    m_rubberBand = new QRubberBand(QRubberBand::Rectangle,
                                    ui->thumbnailContainer);
    m_rubberBand->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_rubberBand->installEventFilter(this);
                                    
    // Sağ tık panel menüsü
    ui->thumbnailScrollArea->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->thumbnailScrollArea, &QWidget::customContextMenuRequested,
            this, &MainWindow::onPanelContextMenuRequested);
    connect(ui->thumbnailScrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            this, []() { ClientThumbnail::hideLiveTooltip(); });

    // ── Sticky sınıf başlığı — scroll alanının üstüne ───────────────────
    // thumbnailScrollArea'nın parent layout'unu bul ve araya ekleme yap
    if (auto* scrollParentLayout = qobject_cast<QVBoxLayout*>(
            ui->thumbnailScrollArea->parentWidget()
                ? ui->thumbnailScrollArea->parentWidget()->layout()
                : nullptr)) {
        m_classHeaderLabel = new QLabel(this);
        m_classHeaderLabel->setContentsMargins(8, 4, 8, 4);
        m_classHeaderLabel->setProperty("class", "headerLabel");
        m_classHeaderLabel->hide();
        const int scrollIdx = scrollParentLayout->indexOf(ui->thumbnailScrollArea);
        scrollParentLayout->insertWidget(scrollIdx, m_classHeaderLabel);
    }

    // ReportPanel (bağımsız top-level pencere)
    m_reportPanel = new ReportPanel(m_session, nullptr);
    m_reportPanel->setWindowIcon(ThemeManager::icon(":/ui/chart.svg"));

    // Lisans uyarısı
    connect(&LicenseManager::instance(),
            &LicenseManager::licenseExpiringSoon,
            [this](int daysLeft) {
        statusBar()->showMessage(
            tr("Lisans ") + QString::number(daysLeft) +
            tr(" gün sonra doluyor!"), 10000);
    });
    connect(&LicenseManager::instance(),
            &LicenseManager::licenseExpired, [this]() {
        statusBar()->showMessage(tr("Lisans süresi doldu!"), 0);
    });

    // DB'den yüklenen offline agent'lar — thumbnail + tree'ye ekle
    connect(m_session, &SessionManager::clientLoadedOffline, this,
            [this](const QString& machineId, const ClientState& state) {
        // Thumbnail: yoksa oluştur, varsa sadece state güncelle (DB sync sonrası yeniden gelir)
        if (!m_thumbnails.contains(machineId)) {
            addThumbnail(machineId, state.hostname, {});
        }
        if (auto* t = m_thumbnails.value(machineId))
            t->updateState(state);

        // TreeManager: m_clientMeta'yı doldur — bu olmadan buildClassView() boş liste üzerinde döner
        if (m_treeManager) {
            const AgentRecord rec = OkidiaDatabase::instance().agent(machineId);
            m_treeManager->assignClient(
                machineId,
                state.hostname,
                state.username,
                state.ip,
                rec.classId,
                state.agentType);
            m_treeManager->setClientOnline(machineId, false);
        }
    });

    connect(m_session, &SessionManager::clientConnected, this,
            [this](const QString& clientId, const ClientState& state) {
        if (!m_thumbnails.contains(clientId)) {
            addThumbnail(clientId, state.hostname, state.username);
            updateStatusBar(m_thumbnails.keys());
            if (!state.initialFrame.isEmpty()) {
                m_thumbnails[clientId]->updateFrame(state.initialFrame);
            }
        } else {
            // Offline thumbnail zaten vardı → online'a geçir
            m_session->registerThumbnail(clientId, m_thumbnails[clientId]);
            if (!state.initialFrame.isEmpty()) {
                m_thumbnails[clientId]->updateFrame(state.initialFrame);
            }
        }
        m_thumbnails[clientId]->setBrokerVersion(m_session->brokerVersion());
        m_thumbnails[clientId]->updateState(state);
        if (m_treeManager) {
            const AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
            m_treeManager->assignClient(
                clientId,
                state.hostname,
                state.username,
                state.ip,
                rec.classId,
                state.agentType);
            m_treeManager->setClientOnline(clientId, true);
        }
        if (m_roomMgr && !OkidiaUtils::isDmUser(state.username)) {
            const QString disp = RoomManager::formatStudentDisplayName(
                state.userDisplayName, state.username, state.hostname, clientId);
            m_roomMgr->onClientConnected(clientId, disp, state.ip);
        }
        if (state.agentType != 2) {
            QTimer::singleShot(1000, this, [this, clientId]() {
                if (m_session) m_session->sendAppListRequest(clientId);
            });
            // Not: Yeni istemci (akıllı tahta/öğrenci pc) bağlandığında zil sesleri ve okul logosu
            // merkezi sunucu (Broker) tarafından otonom olarak (Master kapalı olsa dahi) fısıldanmaktadır.
        }
    });

    connect(m_session, &SessionManager::rollbackCompleted, this,
            [this](const QString& clientId, bool success, const QString& errorMsg, const QString& restoredVersion) {
        if (success) {
            QMessageBox::information(this, tr("Geri Alma Başarılı"),
                QString(tr("<b>%1</b> istemcisi yerel kararlı sürüme (v%2) geri döndürüldü.<br>Servis yeniden başlatılıyor..."))
                    .arg(clientId, restoredVersion.isEmpty() ? tr("Kararlı") : restoredVersion));
            statusBar()->showMessage(QString(tr("%1 başarıyla kararlı sürüme döndürüldü.")).arg(clientId), 6000);
        } else {
            QMessageBox::warning(this, tr("Geri Alma Başarısız"),
                QString(tr("<b>%1</b> istemcisinde geri alma işlemi gerçekleştirilemedi:<br>%2"))
                    .arg(clientId, errorMsg.isEmpty() ? tr("Bilinmeyen hata") : errorMsg));
            statusBar()->showMessage(QString(tr("%1 geri alma başarısız: %2")).arg(clientId, errorMsg), 6000);
        }
    });

    connect(m_session, &SessionManager::clientDisconnected, this,
            [this](const QString& clientId) {

        if (m_treeManager) m_treeManager->setClientOnline(clientId, false);
        if (m_roomMgr)     m_roomMgr->onClientDisconnected(clientId);
        if (auto* thumb = m_thumbnails.value(clientId, nullptr)) {
            ClientState state = thumb->state();
            state.connected = false;
            thumb->updateState(state);
            updateStatusBar(m_thumbnails.keys());
        }
        // İstemci koptuğunda (OTA güncelleme, servis yeniden başlama vb.)
        // açık olan uzak kontrol pencerelerini otomatik kapat ve Broker kilidini düşür
        onRemoteViewStopped(clientId);
    });

    // Onaylanmadan silinen istemci (reject/ignore) → thumbnail'i tamamen kaldır
    connect(m_session, &SessionManager::clientRemoved, this,
            [this](const QString& clientId) {
        // Önce grup frame'lerinden çıkar
        for (auto* frame : m_groupFrames)
            if (frame->thumbnail(clientId))
                frame->removeThumbnail(clientId);

        if (auto* thumb = m_thumbnails.take(clientId)) {
            thumb->deleteLater();
            updateStatusBar(m_thumbnails.keys());
        }
        if (m_treeManager) m_treeManager->removeClient(clientId);
    });

    // Diğer master'lardan veya ağ login'den gelen DB eşitleme sinyalini dinle
    connect(m_session, &SessionManager::dbSyncRequested, this, &MainWindow::syncUiWithDatabase);

    // Eğer oturum baştan offline başlatılmışsa UI'ı kısıtla
    if (m_session->isOfflineMode()) {
        applyOfflineModeConstraints(true);
    }

    // İlk açılışta hiçbir sınıf seçili olmadığından boş durum görünümünü güvenle başlat
    rebuildGroupLayout(nullptr);
}

void MainWindow::syncUiWithDatabase() {
    if (!m_session || !m_treeManager || !OkidiaDatabase::instance().isOpen()) return;

    Logger::info(QObject::tr("MainWindow: DB senkronizasyon sinyali alindi, arayuz yenileniyor..."));

    // 0. İzinleri tazele
    m_session->refreshPermissions();

    // 1. Veritabanındaki tüm ajanları tek seferde belleğe al (N adet disk sorgusu yerine O(1) hash map)
    const auto agents = OkidiaDatabase::instance().allAgents();
    QHash<QString, AgentRecord> agentMap;
    agentMap.reserve(agents.size());
    for (const auto& a : agents) {
        agentMap.insert(a.machineId, a);
    }

    // 2. Ağaç yapısını tamamen temizle ve yeniden yükle
    m_treeManager->reloadTree();

    // 3. Mevcut thumbnail'lar için sınıf atamalarını güncelle ve ağaca yerleştir
    for (auto it = m_thumbnails.constBegin(); it != m_thumbnails.constEnd(); ++it) {
        const QString& clientId = it.key();
        const ClientState cs = m_session->clientState(clientId);
        const QString classId = agentMap.value(clientId).classId;

        m_treeManager->assignClient(clientId, cs.hostname, cs.username, cs.ip, classId, cs.agentType);
        m_treeManager->setClientOnline(clientId, cs.connected);

        // Thumbnail'e ait son güncel durumu da set et
        if (it.value()) {
            it.value()->updateState(cs);
        }
    }

    // 4. Veritabanındaki diğer kayıtlı offline agent'ları da arayüze/ağaca yükle
    for (const AgentRecord& a : agents) {
        if (!m_thumbnails.contains(a.machineId)) {
            addThumbnail(a.machineId, a.hostname, {});
            if (auto* t = m_thumbnails.value(a.machineId)) {
                ClientState state;
                state.clientId  = a.machineId;
                state.hostname  = a.hostname;
                state.machineId = a.machineId;
                state.ip        = a.ip;
                state.agentType = a.agentType;
                state.connected = false;
                t->updateState(state);
            }
            m_treeManager->assignClient(a.machineId, a.hostname, "", a.ip, a.classId, a.agentType);
            m_treeManager->setClientOnline(a.machineId, false);
        }
    }

    // 5. OM/YM yetkilendirmelerini ve görünümlerini uygula
    if (m_activeUser.role != "YM") {
        applyOMPermissions();
        if (ui->classFilterCombo) {
            onClassFilterChanged(ui->classFilterCombo->currentIndex());
        }
    } else {
        updateThumbnailVisibility();
    }

    // 6. Yan panelleri, odaları ve sohbet yöneticilerini yenile
    if (m_userMgmtPanel) {
        m_userMgmtPanel->refresh();
    }
    if (m_roomMgr && !m_activeUser.username.isEmpty()) {
        m_roomMgr->setMasterInfo(m_activeUser.username, m_activeUser.displayName, m_activeUser.role);
        m_roomMgr->syncRoomsFromTree();
        m_roomMgr->reloadLockStates();
    }
    if (m_privateChatMgr && !m_activeUser.username.isEmpty()) {
        m_privateChatMgr->setMasterInfo(m_activeUser.username, m_activeUser.displayName, m_activeUser.role);
    }
    if (m_chatWindow) {
        m_chatWindow->buildRoomTree();
    }
    if (m_otaUpdatePanel) {
        m_otaUpdatePanel->refreshAgentList();
    }

    // 7. Aktif seçili sınıfın görünümünü güncelle (Broker'a YALNIZCA o sınıfın aboneliği iletilir)
    QTreeWidgetItem* activeNode = currentClassScopeNode();
    rebuildGroupLayout(activeNode);
    if (m_selectionSource != SelectionSource::Thumbnail) {
        m_selectionSource = activeNode ? SelectionSource::Tree : SelectionSource::None;
    }

    updateWindowTitle();
    updateLockToggleState();
    updateSelectionTargetBadge();

    QTimer::singleShot(50, this, &MainWindow::refreshAllTreeLockEmojis);
    Logger::info(QObject::tr("MainWindow: DB senkronizasyon yenilemesi tamamlandi."));
}

MainWindow::~MainWindow() {
    if (m_broadcaster && m_broadcaster->isRunning()) {
        m_broadcaster->stop();
    }
    // KVKK: Ekran görüntülerini ve önbellekleri bellekten derhal temizle
    for (auto* thumb : m_thumbnails) {
        if (thumb) {
            thumb->clearFrameBuffer();
        }
    }
    if (m_session) {
        m_session->clearThumbnails();
    }
    delete ui;
}


// setupTree → setupTreeDock içine taşındı


void MainWindow::setupTreeDock() {
    // ── Görünüm toggle widget'ını (Sınıf / Ağ) doğrudan sol panelin üstüne ekle ──
    auto* toggleRow  = new QHBoxLayout;
    toggleRow->setContentsMargins(0, 0, 0, 2);
    toggleRow->setSpacing(4);

    auto* btnClass  = new QPushButton(tr("Sınıf"), ui->leftPanel);
    auto* btnSubnet = new QPushButton(tr("Ağ"),    ui->leftPanel);
    btnClass ->setObjectName("btnClass");
    btnSubnet->setObjectName("btnSubnet");
    btnClass ->setCheckable(true);
    btnSubnet->setCheckable(true);
    btnClass ->setChecked(true);

    toggleRow->addWidget(btnClass);
    toggleRow->addWidget(btnSubnet);
    toggleRow->addStretch();
    ui->leftLayout->insertLayout(0, toggleRow);

    ui->leftPanel->setMinimumWidth(190);
    ui->leftPanel->setMaximumWidth(340);

    m_toggleTreeAction = new QAction(tr("İstemci Ağacı"), this);
    m_toggleTreeAction->setCheckable(true);
    m_toggleTreeAction->setChecked(true);
    m_toggleTreeAction->setShortcut(QKeySequence("Ctrl+Shift+T"));
    connect(m_toggleTreeAction, &QAction::toggled, this, [this](bool visible) {
        if (ui && ui->leftPanel) {
            ui->leftPanel->setVisible(visible);
        }
    });

    ui->classTree->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->classTree->setHeaderHidden(true);
    ui->classTree->setIconSize(QSize(16, 16));
    // 3 sütun: isim | sohbet kilidi | özel sohbet kilidi
    ui->classTree->setColumnCount(3);
    ui->classTree->header()->setStretchLastSection(false);
    ui->classTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->classTree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    ui->classTree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->classTree->header()->resizeSection(1, 20);
    ui->classTree->header()->resizeSection(2, 20);
    ui->classTree->setColumnWidth(1, 20);
    ui->classTree->setColumnWidth(2, 20);

    // Sütun 1-2 tıklaması — toggle
    connect(ui->classTree, &QTreeWidget::itemClicked,
            [this](QTreeWidgetItem* item, int col) {
        if (!item || !m_treeManager || !m_roomMgr) return;
        const QString type = m_treeManager->nodeType(item);
        if (type != TreeManager::kTypeClass && type != TreeManager::kTypeGroup) return;
        if (col != 1 && col != 2) return;
        // Sadece YM/OM toggle edebilir (OM sadece kendi sınıflarını yönetebilir)
        if (m_activeUser.role != "YM" && m_activeUser.role != "OM") return;

        const QString nodeId = m_treeManager->publicStableId(item);
        if (m_session && !m_session->canManageClass(nodeId)) return;

        ChatLockRecord state = m_roomMgr->chatLockState(nodeId);

        if (col == 1) {
            // Sohbet toggle — kilitlenince özel sohbet de kilitlenir
            const bool newLocked = !state.chatLocked;
            m_roomMgr->setChatLock(nodeId, newLocked,
                newLocked ? true : state.privateLocked);
        } else {
            // Özel sohbet toggle — sadece sohbet açıkken ayrıca kilitleneblir
            const bool newPrivateLocked = !state.privateLocked;
            m_roomMgr->setChatLock(nodeId, state.chatLocked, newPrivateLocked);
        }
    });

    // ── TreeManager ────────────────────────────────────────────────────────
    m_treeManager = new TreeManager(ui->classTree, this);
    m_treeManager->setSession(m_session);
    m_treeManager->load();

    // Kaydedilmiş görünüm modunu uygula
    {
        QSettings s("okidia", "master");
        const int mode = s.value("ui/treeViewMode", 0).toInt();
        if (mode == 1) {
            m_treeManager->setViewMode(TreeManager::SubnetView);
            btnSubnet->setChecked(true);
            btnClass->setChecked(false);
        }
    }

    // Toggle bağlantıları
    connect(btnClass, &QPushButton::clicked, [this, btnClass, btnSubnet]() {
        btnClass->setChecked(true);
        btnSubnet->setChecked(false);
        m_treeManager->setViewMode(TreeManager::ClassView);
        QSettings("okidia", "master").setValue("users/" + m_activeUser.username + "/ui/treeViewMode", 0);
        updateThumbnailVisibility();
    });
    connect(btnSubnet, &QPushButton::clicked, [this, btnClass, btnSubnet]() {
        btnSubnet->setChecked(true);
        btnClass->setChecked(false);
        m_treeManager->setViewMode(TreeManager::SubnetView);
        QSettings("okidia", "master").setValue("users/" + m_activeUser.username + "/ui/treeViewMode", 1);
        updateThumbnailVisibility();
    });

    // Client taşınınca SessionManager'a bildir + görünürlük güncelle
    connect(m_treeManager, &TreeManager::clientMoved, this,
        [this](const QString& clientId, QTreeWidgetItem* newNode) {
            if (m_session && m_treeManager) {
                const QString nodeType = m_treeManager->nodeType(newNode);
                QString groupId;
                if (nodeType == TreeManager::kTypeClass || nodeType == TreeManager::kTypeGroup) {
                    groupId = m_treeManager->publicStableId(newNode);
                }
                m_session->setClientGroup(clientId, groupId);
            }
            scheduleRebuildGroupLayout();
            updateSelectionTargetBadge();
        });

    // Görünüm değişince OM yetki renklerini yeniden uygula
    connect(m_treeManager, &TreeManager::viewModeChanged, this, [this](TreeManager::ViewMode) {
        if (m_activeUser.role != "YM") applyOMPermissions();
        else updateThumbnailVisibility();
    });

    // Tree yapısı değişince: UserManagementPanel + thumbnail alanı güncelle
    connect(m_treeManager, &TreeManager::structureChanged, this, [this]() {
        if (m_userMgmtPanel) m_userMgmtPanel->refresh();
        updateThumbnailVisibility();
        QTimer::singleShot(0, this, &MainWindow::refreshAllTreeLockEmojis);
    });

    // TreeView boşluğuna tıklandığında veya Escape'e basıldığında: Kapsamı sıfırla ve Tüm İstemciler'e dön
    connect(m_treeManager, &TreeManager::emptyAreaClicked, this, [this]() {
        m_currentScopeNode = nullptr;
        m_selectionSource = SelectionSource::None;
        for (auto* t : m_thumbnails) {
            t->setSelected(false);
        }
        rebuildGroupLayout(nullptr);
        updateLockToggleState();
        updateSelectionTargetBadge();
    });

    // Subnet çift tık → isim değiştir
    connect(ui->classTree, &QTreeWidget::itemDoubleClicked,
            [this](QTreeWidgetItem* item, int) {
        if (!item) return;
        if (m_treeManager->nodeType(item) != TreeManager::kTypeSubnet) return;
        const QString cidr = item->data(0, Qt::UserRole + 1).toString();
        const QString cur  = m_treeManager->subnetDisplayName(cidr);
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Ağ İsmini Düzenle"),
            tr("Ağ adı (") + cidr + "):", QLineEdit::Normal, cur, &ok);
        if (ok) m_treeManager->setSubnetName(cidr, name.trimmed());
    });

    // Sınıf görünüm filtresi (Tüm Sınıflar / Kendi Sınıflarım)
    if (ui->classFilterCombo) {
        ui->classFilterCombo->clear();
        ui->classFilterCombo->addItem(ThemeManager::icon(":/ui/group.svg"), tr("Tüm Sınıflar"));
        ui->classFilterCombo->addItem(ThemeManager::icon(":/ui/school.svg"), tr("Kendi Sınıflarım"));
        ui->classFilterCombo->setStyleSheet(
            "QComboBox { padding: 3px 6px; border-radius: 4px; font-size: 11px; }"
        );
        ui->classFilterCombo->hide();
        connect(ui->classFilterCombo, &QComboBox::currentIndexChanged,
                this, &MainWindow::onClassFilterChanged);
    }

    // Arama kutusu
    connect(ui->searchBox, &QLineEdit::textChanged,
            this, [this](const QString& text) {
        if (m_treeManager) m_treeManager->setTextFilter(text);
    });

    if (auto* panellerMenu = menuBar()->findChild<QMenu*>("menuPaneller")) {
        if (m_toggleTreeAction) {
            panellerMenu->addAction(m_toggleTreeAction);
        }
    }
}

void MainWindow::setupDock() {
    // ── Sistem & Ağ Yönetimi Bağımsız Alt Penceresi (Deploy + OTA + SSH) ─────
    m_sysMgmtWindow = new QWidget(nullptr, Qt::Window);
    m_sysMgmtWindow->setObjectName("sysMgmtWindow");
    m_sysMgmtWindow->setWindowTitle(tr("Sistem & Ağ Yönetimi — OKIDIA"));
    m_sysMgmtWindow->setWindowIcon(ThemeManager::icon(":/ui/settings.svg"));
    m_sysMgmtWindow->setMinimumSize(840, 540);
    m_sysMgmtWindow->resize(1180, 740);

    auto* winLay = new QVBoxLayout(m_sysMgmtWindow);
    winLay->setContentsMargins(6, 6, 6, 6);
    winLay->setSpacing(0);

    m_sysMgmtTabs = new QTabWidget(m_sysMgmtWindow);
    m_sysMgmtTabs->setObjectName("sysMgmtTabs");
    winLay->addWidget(m_sysMgmtTabs);

    m_deployPanel = new DeployPanel(m_session, m_sysMgmtTabs);
    m_deployPanel->hide();
    connect(m_deployPanel, &DeployPanel::discoveryPanelRequested,
            this, &MainWindow::showDiscoveryPanel);
    connect(m_deployPanel, &DeployPanel::manageClientsRequested,
            this, &MainWindow::onManageClients);
    connect(m_deployPanel, &DeployPanel::agentInstalledOn,
            [this](const QString& ip, const QString& hostname) {
        showCommandStatus(true, tr("İstemci kuruldu → ") + hostname + " (" + ip + ")");
    });
    connect(m_deployPanel, &DeployPanel::globalContextMenuRequested, [this](const QPoint& pos) {
        Q_UNUSED(pos)
        QMenu* menu = createPanelToggleMenu();
        menu->exec(QCursor::pos());
        menu->deleteLater();
    });

    m_sshManagerPanel = new SshManagerPanel(m_session, m_sysMgmtTabs);
    m_sshManagerPanel->hide();
    m_otaUpdatePanel = new OtaUpdatePanel(m_session, m_sysMgmtTabs);
    m_otaUpdatePanel->hide();

    m_sysMgmtWindow->hide();

    // Sekmeler arası geçişlerde yalnızca seçili sekmenin görünür kalmasını garanti et
    auto syncTabVisibility = [this]() {
        if (!m_sysMgmtTabs) return;
        int cur = m_sysMgmtTabs->currentIndex();
        for (int i = 0; i < m_sysMgmtTabs->count(); ++i) {
            if (auto* w = m_sysMgmtTabs->widget(i)) {
                w->setVisible(i == cur);
            }
        }
    };
    connect(m_sysMgmtTabs, &QTabWidget::currentChanged, this, [syncTabVisibility](int) {
        syncTabVisibility();
    });

    setupSysMgmtTabs();
}

void MainWindow::showSystemManagementWindow(QWidget* targetTab) {
    if (!m_sysMgmtWindow || m_activeUser.role == "OBSERVER") return;

    if (targetTab && m_sysMgmtTabs) {
        int idx = m_sysMgmtTabs->indexOf(targetTab);
        if (idx != -1) {
            m_sysMgmtTabs->setCurrentIndex(idx);
        }
    }

    // Ekran kilitliyse pencereyi kilit ekranının üstüne çıkarma; kilit açılınca gösterilmek üzere kuyruğa al
    if (isScreenLocked()) {
        if (!m_hiddenWindowsOnLock.contains(m_sysMgmtWindow)) {
            m_hiddenWindowsOnLock.append(m_sysMgmtWindow);
        }
        return;
    }

    if (m_sysMgmtWindow->isMinimized()) {
        m_sysMgmtWindow->showNormal();
    }
    m_sysMgmtWindow->show();
    m_sysMgmtWindow->raise();
    m_sysMgmtWindow->activateWindow();
}

void MainWindow::setupSysMgmtTabs() {
    if (!m_sysMgmtWindow || !m_sysMgmtTabs) return;

    // Tazelemeden önce kullanıcının çalıştığı aktif sekmeyi hatırla
    QWidget* prevActiveTab = m_sysMgmtTabs->currentWidget();

    // Önceki tüm panelleri gizle
    if (m_deployPanel)     m_deployPanel->hide();
    if (m_otaUpdatePanel)  m_otaUpdatePanel->hide();
    if (m_sshManagerPanel) m_sshManagerPanel->hide();

    m_sysMgmtTabs->clear();

    // Rol bazlı sekme izolasyonu:
    // İstemci Dağıtımı (DeployPanel) YM ve OM için görünür; Sistem Güncelleme (OtaUpdatePanel) yalnızca YM için görünür
    if (m_activeUser.role == "YM") {
        m_sysMgmtWindow->setWindowTitle(tr("Sistem & Ağ Yönetimi — OKIDIA"));
        if (m_deployPanel)     m_sysMgmtTabs->addTab(m_deployPanel, tr("İstemci Dağıtımı"));
        if (m_otaUpdatePanel)  m_sysMgmtTabs->addTab(m_otaUpdatePanel, tr("Sistem Güncelleme"));
        if (m_sshManagerPanel) m_sysMgmtTabs->addTab(m_sshManagerPanel, tr("Gelişmiş SSH"));
    } else if (m_activeUser.role != "OBSERVER") {
        // Öğretmen (OM) için İstemci Dağıtımı ve Gelişmiş SSH görünür
        m_sysMgmtWindow->setWindowTitle(tr("Sistem & Ağ Yönetimi — OKIDIA"));
        if (m_deployPanel)     m_sysMgmtTabs->addTab(m_deployPanel, tr("İstemci Dağıtımı"));
        if (m_sshManagerPanel) m_sysMgmtTabs->addTab(m_sshManagerPanel, tr("Gelişmiş SSH"));
    } else {
        // Gözlemci (GM) için pencere tamamen gizlenir
        m_sysMgmtWindow->hide();
        return;
    }

    // Role uygun olmayan panelleri açıkça gizli tut
    if (m_deployPanel && m_sysMgmtTabs->indexOf(m_deployPanel) == -1) {
        m_deployPanel->hide();
    }
    if (m_otaUpdatePanel && m_sysMgmtTabs->indexOf(m_otaUpdatePanel) == -1) {
        m_otaUpdatePanel->hide();
    }
    if (m_sshManagerPanel && m_sysMgmtTabs->indexOf(m_sshManagerPanel) == -1) {
        m_sshManagerPanel->hide();
    }

    // Kullanıcının çalıştığı aktif sekmeyi koru; yoksa ilk sekmeye odaklan
    int targetIdx = (prevActiveTab != nullptr) ? m_sysMgmtTabs->indexOf(prevActiveTab) : 0;
    if (targetIdx == -1) targetIdx = 0;

    // Sekmelerin görünürlüğünü senkronize et
    if (m_sysMgmtTabs->count() > 0) {
        m_sysMgmtTabs->setCurrentIndex(targetIdx);
        for (int i = 0; i < m_sysMgmtTabs->count(); ++i) {
            if (auto* w = m_sysMgmtTabs->widget(i)) {
                w->setVisible(i == targetIdx);
            }
        }
    }
}

void MainWindow::setupSplitter() {
    ui->mainSplitter->setStretchFactor(0, 0);
    ui->mainSplitter->setStretchFactor(1, 1);
    QList<int> sizes;
    sizes << 220 << 1060;
    ui->mainSplitter->setSizes(sizes);
}

void MainWindow::updateStatusBar(const QStringList& clientIds) {
    int onlineCount = 0;
    int totalCount = clientIds.size();
    for (const QString& id : clientIds) {
        if (m_thumbnails.contains(id) && m_thumbnails[id]->state().connected) {
            onlineCount++;
        }
    }
    const QString text = QString::number(onlineCount) + " / " + 
                         QString::number(totalCount) + tr(" İstemci Bağlandı");
    statusBar()->showMessage(text);

    if (m_session) {
        m_session->setVisibleClients(QSet<QString>(clientIds.begin(), clientIds.end()));
    }
}

void MainWindow::setupStatusBarNetworkMode() {
    QWidget* container = new QWidget(this);
    QHBoxLayout* layout = new QHBoxLayout(container);
    layout->setContentsMargins(6, 0, 8, 0);
    layout->setSpacing(6);

    m_netModeTitle = new QLabel(tr("Bağlantı Biçimi :"), container);
    m_netModeTitle->setStyleSheet("color: #71717a; font-size: 11px; font-weight: 500;");

    m_netModeBadge = new QLabel(container);
    m_netModeBadge->setObjectName("netModeBadge");
    m_netModeBadge->setAlignment(Qt::AlignCenter);
    m_netModeBadge->setCursor(Qt::PointingHandCursor);

    m_masterUpdateBtn = new QPushButton(container);
    m_masterUpdateBtn->setObjectName("masterUpdateBtn");
    m_masterUpdateBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_masterUpdateBtn->setProperty("class", "primary");
    m_masterUpdateBtn->setVisible(false);

    layout->addWidget(m_masterUpdateBtn);
    layout->addWidget(m_netModeTitle);
    layout->addWidget(m_netModeBadge);

    connect(m_masterUpdateBtn, &QPushButton::clicked, this, &MainWindow::onMasterUpdateBtnClicked);

    statusBar()->addPermanentWidget(container);
    updateNetworkModeBadge();
}

void MainWindow::updateNetworkModeBadge() {
    if (!m_netModeBadge || !m_session) return;

    SessionManager::ConnectionMode mode = m_session->connectionMode();
    QString text = m_session->connectionModeString();
    QString tooltip = m_session->connectionDetails();

    m_netModeBadge->setText(text);
    m_netModeBadge->setToolTip(tooltip);

    bool isDark = (ThemeManager::instance().currentMode() == ThemeManager::ThemeMode::Dark);

    switch (mode) {
        case SessionManager::ConnectionMode::Local:
            if (isDark) {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: rgba(34, 197, 94, 0.22);"
                    "  color: #4ade80;"
                    "  border: 1px solid rgba(74, 222, 128, 0.45);"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: bold;"
                    "}"
                );
            } else {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: #dcfce7;"
                    "  color: #15803d;"
                    "  border: 1px solid #86efac;"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: bold;"
                    "}"
                );
            }
            break;
        case SessionManager::ConnectionMode::Remote:
            if (isDark) {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: rgba(168, 85, 247, 0.22);"
                    "  color: #c084fc;"
                    "  border: 1px solid rgba(192, 132, 252, 0.45);"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: bold;"
                    "}"
                );
            } else {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: #f3e8ff;"
                    "  color: #7e22ce;"
                    "  border: 1px solid #d8b4fe;"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: bold;"
                    "}"
                );
            }
            break;
        case SessionManager::ConnectionMode::SshOnly:
            if (isDark) {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: rgba(249, 115, 22, 0.22);"
                    "  color: #fb923c;"
                    "  border: 1px solid rgba(251, 146, 60, 0.45);"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: bold;"
                    "}"
                );
            } else {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: #ffedd5;"
                    "  color: #c2410c;"
                    "  border: 1px solid #fdba74;"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: bold;"
                    "}"
                );
            }
            break;
        case SessionManager::ConnectionMode::Disconnected:
        default:
            if (isDark) {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: rgba(148, 163, 184, 0.15);"
                    "  color: #94a3b8;"
                    "  border: 1px solid rgba(148, 163, 184, 0.35);"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: 500;"
                    "}"
                );
            } else {
                m_netModeBadge->setStyleSheet(
                    "QLabel#netModeBadge {"
                    "  background-color: #f1f5f9;"
                    "  color: #64748b;"
                    "  border: 1px solid #cbd5e1;"
                    "  border-radius: 10px;"
                    "  padding: 2px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: 500;"
                    "}"
                );
            }
            break;
    }
}

void MainWindow::addThumbnail(const QString& clientId,
                               const QString& hostname,
                               const QString& username) {
    const ClientState& cs = m_session ? m_session->clientState(clientId) : ClientState{};
    qDebug() << "MainWindow::addThumbnail called with clientId:" << clientId << "hostname:" << hostname;
    auto* thumb = new ClientThumbnail(clientId, ui->thumbnailContainer);
    thumb->hide();
    ClientState state = cs;
    state.clientId  = clientId;
    if (!hostname.isEmpty()) state.hostname = hostname;
    if (!username.isEmpty()) state.username = username;
    thumb->updateState(state);

    connect(thumb, &ClientThumbnail::clicked,
            [this](const QString& id) { Q_UNUSED(id) });
    connect(thumb, &ClientThumbnail::clickedWithModifiers,
            [this](const QString& id, Qt::KeyboardModifiers mods) {
        handleThumbnailClick(id, mods);
    });
    connect(thumb, &ClientThumbnail::doubleClicked,
            [this](const QString& id) {
        if (m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM") return;
        if (!m_session || !m_session->canManageClient(id)) return;
        onClientTakeControl(id);
    });
    connect(thumb, &ClientThumbnail::metricsStreamRequested,
            [this](const QString& id, bool start) {
        if (m_session) {
            okidia::Envelope env;
            env.set_type(start ? okidia::MessageType::METRICS_STREAM_START : okidia::MessageType::METRICS_STREAM_STOP);
            m_session->sendCommand(id, env);
        }
    });
    connect(thumb, &ClientThumbnail::contextMenuRequested,
            this, &MainWindow::onThumbnailContextMenu);
    connect(thumb, &ClientThumbnail::handRaiseClicked,
            this, &MainWindow::onClientPrivateChat);
    connect(thumb, &ClientThumbnail::dragInitiated, this,
        [this](const QString& srcId) {
            QStringList ids = selectedClientIds();
            if (!ids.contains(srcId)) ids = {srcId};
            auto* mime = new QMimeData;
            mime->setData(TreeManager::kMimeClientIds, ids.join('\n').toUtf8());
            auto* drag = new QDrag(this);
            drag->setMimeData(mime);
            drag->exec(Qt::MoveAction);
        });

    m_thumbnails[clientId] = thumb;
    if (m_session) {
        thumb->setBrokerVersion(m_session->brokerVersion());
    }


    const AgentRecord rec = OkidiaDatabase::instance().agent(
        cs.machineId.isEmpty() ? clientId : cs.machineId);

    if (m_treeManager) {
        m_treeManager->assignClient(clientId, hostname, username, cs.ip, rec.classId, cs.agentType);
    }

    if (m_session && m_treeManager) {
        auto* node = m_treeManager->nodeForClient(clientId);
        QString groupId;
        if (node && (m_treeManager->nodeType(node) == TreeManager::kTypeClass || m_treeManager->nodeType(node) == TreeManager::kTypeGroup)) {
            groupId = m_treeManager->publicStableId(node);
        }
        m_session->setClientGroup(clientId, groupId);
    }
    if (m_session) m_session->registerThumbnail(clientId, thumb);

    scheduleRebuildGroupLayout();
}

QMenu* MainWindow::createPanelToggleMenu() {
    auto* menu = new QMenu(tr("Pencereler"), this);
    menu->setIcon(ThemeManager::icon(":/ui/settings.svg"));

    // 1. Ana Ekran
    QAction* actMain = menu->addAction(ThemeManager::icon(":/ui/monitor.svg"), tr("Ana Ekran (İstemci İzleme)"));
    connect(actMain, &QAction::triggered, this, [this]() {
        if (isMinimized()) showNormal();
        raise();
        activateWindow();
    });

    // 2. Sohbet
    QAction* actChat = menu->addAction(ThemeManager::icon(":/ui/chat.svg"), tr("Sohbet"));
    actChat->setCheckable(true);
    actChat->setChecked(m_chatWindow && m_chatWindow->isVisible());
    connect(actChat, &QAction::triggered, this, &MainWindow::onShowChat);

    // 3. Sistem & Ağ Yönetimi
    if (m_activeUser.role != "OBSERVER" && m_sysMgmtWindow) {
        QString sysTitle = (m_activeUser.role == "YM") ? tr("Sistem ve Ağ Yönetimi") : tr("Gelişmiş SSH");
        QAction* actSys = menu->addAction(ThemeManager::icon(":/ui/settings.svg"), sysTitle);
        actSys->setCheckable(true);
        actSys->setChecked(m_sysMgmtWindow->isVisible());
        connect(actSys, &QAction::triggered, this, [this](bool checked) {
            if (checked || !m_sysMgmtWindow->isVisible()) {
                showSystemManagementWindow();
            } else {
                m_sysMgmtWindow->hide();
            }
        });
    }

    // 4. Kullanıcı Yönetimi
    if (m_activeUser.role == "YM") {
        QAction* actUser = menu->addAction(ThemeManager::icon(":/ui/group.svg"), tr("Kullanıcı Yönetimi"));
        actUser->setCheckable(true);
        actUser->setChecked(m_userMgmtPanel && m_userMgmtPanel->isVisible());
        connect(actUser, &QAction::triggered, this, [this]() {
            onShowUserManagement();
            if (m_userMgmtPanel) {
                if (m_userMgmtPanel->isMinimized()) m_userMgmtPanel->showNormal();
                m_userMgmtPanel->raise();
                m_userMgmtPanel->activateWindow();
            }
        });
    }

    // 5. İstemci Yönetimi
    if (m_activeUser.role != "OBSERVER") {
        QAction* actCliMgmt = menu->addAction(ThemeManager::icon(":/ui/discovery.svg"), tr("İstemci Yönetimi (Kayıtlılar & Kara Liste)"));
        actCliMgmt->setCheckable(true);
        actCliMgmt->setChecked(m_clientMgmtDialog && m_clientMgmtDialog->isVisible());
        connect(actCliMgmt, &QAction::triggered, this, &MainWindow::onManageClients);
    }

    // 6. Raporlar
    if (m_activeUser.role != "OBSERVER") {
        QAction* actReport = menu->addAction(ThemeManager::icon(":/ui/chart.svg"), tr("Raporlar"));
        actReport->setCheckable(true);
        actReport->setChecked(m_reportPanel && m_reportPanel->isVisible());
        connect(actReport, &QAction::triggered, this, [this]() {
            onShowReport();
            if (m_reportPanel) {
                if (m_reportPanel->isMinimized()) m_reportPanel->showNormal();
                m_reportPanel->raise();
                m_reportPanel->activateWindow();
            }
        });
    }

    // 7. Açık Tam Ekran İzleme / Kontrol Pencereleri
    bool hasRemoteViews = false;
    for (auto* view : m_remoteViews) {
        if (view && view->isVisible()) {
            if (!hasRemoteViews) {
                menu->addSeparator();
                hasRemoteViews = true;
            }
            QString vTitle = view->windowTitle().isEmpty() ? tr("Tam Ekran İzleme") : view->windowTitle();
            QAction* actRemote = menu->addAction(ThemeManager::icon(":/ui/monitor.svg"), vTitle);
            connect(actRemote, &QAction::triggered, view, [view]() {
                if (view->isMinimized()) view->showNormal();
                view->raise();
                view->activateWindow();
            });
        }
    }

    // 8. Yeni İstemci İstekleri
    int pendingDiscovery = m_discoveryTable ? m_discoveryTable->rowCount() : 0;
    menu->addSeparator();
    QString discText = (pendingDiscovery > 0)
        ? QString(tr("Yeni İstemci İstekleri (%1 Bekleyen)...")).arg(pendingDiscovery)
        : tr("Yeni İstemci İstekleri Paneli...");
    QAction* actDisc = menu->addAction(ThemeManager::icon(":/ui/network.svg"), discText);
    actDisc->setEnabled(!isObserver());
    connect(actDisc, &QAction::triggered, this, [this]() {
        if (m_discoveryPanel) {
            m_discoveryPanel->show();
            m_discoveryPanel->raise();
            m_discoveryPanel->activateWindow();
            const QPoint center = this->geometry().center();
            m_discoveryPanel->move(
                center.x() - m_discoveryPanel->width()  / 2,
                center.y() - m_discoveryPanel->height() / 2);
        }
    });

    menu->addSeparator();

    // Toplu İşlemler
    QAction* actBringAll = menu->addAction(tr("Tüm Pencereleri Öne Getir"));
    connect(actBringAll, &QAction::triggered, this, [this]() {
        if (isMinimized()) showNormal();
        raise();
        if (m_chatWindow && m_chatWindow->isVisible()) {
            if (m_chatWindow->isMinimized()) m_chatWindow->showNormal();
            m_chatWindow->raise();
        }
        if (m_sysMgmtWindow && m_sysMgmtWindow->isVisible()) {
            if (m_sysMgmtWindow->isMinimized()) m_sysMgmtWindow->showNormal();
            m_sysMgmtWindow->raise();
        }
        if (m_userMgmtPanel && m_userMgmtPanel->isVisible()) {
            if (m_userMgmtPanel->isMinimized()) m_userMgmtPanel->showNormal();
            m_userMgmtPanel->raise();
        }
        if (m_clientMgmtDialog && m_clientMgmtDialog->isVisible()) {
            if (m_clientMgmtDialog->isMinimized()) m_clientMgmtDialog->showNormal();
            m_clientMgmtDialog->raise();
        }
        if (m_reportPanel && m_reportPanel->isVisible()) {
            if (m_reportPanel->isMinimized()) m_reportPanel->showNormal();
            m_reportPanel->raise();
        }
        for (auto* view : m_remoteViews) {
            if (view && view->isVisible()) {
                if (view->isMinimized()) view->showNormal();
                view->raise();
            }
        }
    });

    return menu;
}

void MainWindow::onPanelContextMenuRequested(const QPoint& pos) {
    // Eğer bir thumbnail üzerine tıklandıysa bu menü açılmasın (onThumbnailContextMenu çalışır)
    QWidget* child = ui->thumbnailContainer->childAt(pos);
    if (child && (qobject_cast<ClientThumbnail*>(child) || qobject_cast<ClientThumbnail*>(child->parent()))) {
        return;
    }

    QMenu* menu = createPanelToggleMenu();
    menu->exec(ui->thumbnailScrollArea->mapToGlobal(pos));
    menu->deleteLater();
}


void MainWindow::applyOfflineModeConstraints(bool offline) {
    if (!m_offlineBanner) {
        m_offlineBanner = new QLabel(this);
        m_offlineBanner->setProperty("status", "error");
        m_offlineBanner->setAlignment(Qt::AlignCenter);
        m_offlineBanner->setContentsMargins(0, 4, 0, 4);
        m_offlineBanner->setText(tr("Çevrimdışı SSH Modu: Sunucu bağlantısı yok. Yalnızca İstemci Dağıtımı ve Gelişmiş SSH araçları aktiftir."));
        
        // rightPanel'in en üstüne (actionLayout'un hemen üstüne) ekle
        if (auto* rightLay = qobject_cast<QVBoxLayout*>(ui->rightPanel->layout())) {
            rightLay->insertWidget(0, m_offlineBanner);
        }
    }

    m_offlineBanner->setText(tr("Çevrimdışı SSH Modu: Sunucu bağlantısı yok. Yalnızca İstemci Dağıtımı ve Gelişmiş SSH araçları aktiftir."));
    m_offlineBanner->setVisible(offline);

    if (m_menuKisitli) {
        m_menuKisitli->menuAction()->setVisible(offline && m_session && m_session->isKickedToOffline());
    }

    if (m_actUninstallPass) {
        m_actUninstallPass->setEnabled(!offline);
    }

    // Ağaç/Sınıf panelini pasif yap
    if (ui->leftPanel) {
        ui->leftPanel->setEnabled(!offline);
    }
    
    // Toolbar butonları
    const bool isObs = (m_activeUser.role == "OBSERVER");
    const bool enableOps = !offline && !isObs;

    // ── Gözlemci (OBSERVER / GM) İzolasyonu ────────────────────────────────
    // Gözlemci yalnızca Mesaj, Anons, Sohbet ve Rapor butonlarını görür.
    // Diğer tüm toolbar butonları setVisible(false) ile tamamen gizlenir.
    if (ui->btnLockToggle) { ui->btnLockToggle->setVisible(!isObs); ui->btnLockToggle->setEnabled(enableOps); }
    if (ui->btnBroadcast)  { ui->btnBroadcast->setVisible(!isObs);  ui->btnBroadcast->setEnabled(enableOps); }
    if (ui->btnExam)       { ui->btnExam->setVisible(!isObs);       ui->btnExam->setEnabled(enableOps); }
    if (ui->btnSendFile)   { ui->btnSendFile->setVisible(!isObs);   ui->btnSendFile->setEnabled(enableOps); }
    if (ui->btnSchedule)   { ui->btnSchedule->setVisible(!isObs);   ui->btnSchedule->setEnabled(enableOps); }
    if (ui->btnPower)      { ui->btnPower->setVisible(!isObs);      ui->btnPower->setEnabled(enableOps); }

    // Gözlemci (GM/Rehberlik) Mesaj, Anons, Sohbet ve Rapor'a erişebilir
    if (ui->btnSendMessage) { ui->btnSendMessage->setVisible(true); ui->btnSendMessage->setEnabled(!offline); }
    if (ui->btnAudio)       { ui->btnAudio->setVisible(true);       ui->btnAudio->setEnabled(!offline); }
    if (ui->btnChat)        { ui->btnChat->setVisible(true);        ui->btnChat->setEnabled(!offline); }
    if (ui->btnReport)      { ui->btnReport->setVisible(true);      ui->btnReport->setEnabled(!offline); }

    // Eğer offline moda girdiyse Sistem & Ağ Yönetimi penceresini otomatik aç ve DeployPanel veya SshManagerPanel'i öne çıkar
    if (offline) {
        if (m_deployPanel && m_sysMgmtTabs && m_sysMgmtTabs->indexOf(m_deployPanel) != -1) {
            showSystemManagementWindow(m_deployPanel);
        } else if (m_sshManagerPanel && m_sysMgmtTabs && m_sysMgmtTabs->indexOf(m_sshManagerPanel) != -1) {
            showSystemManagementWindow(m_sshManagerPanel);
        }
    }
}

void MainWindow::setupStatusBarTargetBadge() {
    QWidget* container = new QWidget(this);
    QHBoxLayout* layout = new QHBoxLayout(container);
    layout->setContentsMargins(6, 0, 8, 0);
    layout->setSpacing(6);

    m_targetBadgeTitle = new QLabel(tr("Komut Hedefi :"), container);
    m_targetBadgeTitle->setStyleSheet("color: #71717a; font-size: 11px; font-weight: 500;");

    m_targetBadge = new QLabel(container);
    m_targetBadge->setObjectName("targetBadge");
    m_targetBadge->setAlignment(Qt::AlignCenter);

    layout->addWidget(m_targetBadgeTitle);
    layout->addWidget(m_targetBadge);

    statusBar()->addPermanentWidget(container);
    updateSelectionTargetBadge();
}

void MainWindow::updateSelectionTargetBadge() {
    if (!m_targetBadge) return;

    QString text;
    QString tooltip;
    const bool isDark = (ThemeManager::instance().currentMode() == ThemeManager::ThemeMode::Dark);

    if (m_selectionSource == SelectionSource::Thumbnail) {
        QStringList sel = selectedClientIds();
        if (sel.isEmpty()) {
            m_selectionSource = SelectionSource::None;
        } else if (sel.size() == 1) {
            text = clientDisplayName(sel.first()) + " (" + tr("İstemci") + ")";
            tooltip = tr("Komutlar sadece seçili istemciye gönderilecek: ") + sel.first();
        } else {
            text = QString(tr("%1 İstemci Seçili")).arg(sel.size());
            tooltip = tr("Komutlar seçilen %1 istemciye gönderilecek:\n").arg(sel.size()) + sel.join(", ");
        }
    }

    if (m_selectionSource == SelectionSource::Tree) {
        QTreeWidgetItem* node = (ui && ui->classTree && !ui->classTree->selectedItems().isEmpty())
            ? ui->classTree->selectedItems().first() : nullptr;
        if (!node) {
            m_selectionSource = SelectionSource::None;
        } else {
            QString type = m_treeManager ? m_treeManager->nodeType(node) : QString{};
            QString name = m_treeManager ? m_treeManager->displayName(node) : QString{};
            QStringList clients = m_treeManager ? m_treeManager->clientsInNode(node) : QStringList{};
            if (type == TreeManager::kTypeClient) {
                text = name + " (" + tr("İstemci") + ")";
                tooltip = tr("Komutlar ağaçta seçili istemciye gönderilecek: ") + name;
            } else if (type == TreeManager::kTypeClass) {
                text = name + QString(" (%1 %2)").arg(clients.size()).arg(tr("İstemci"));
                tooltip = tr("Komutlar %1 sınıfındaki %2 istemciye gönderilecek").arg(name).arg(clients.size());
            } else if (type == TreeManager::kTypeGroup) {
                QString parentClassName;
                QTreeWidgetItem* parentItem = node->parent();
                while (parentItem) {
                    if (m_treeManager && m_treeManager->nodeType(parentItem) == TreeManager::kTypeClass) {
                        parentClassName = m_treeManager->displayName(parentItem);
                        break;
                    }
                    parentItem = parentItem->parent();
                }

                if (!parentClassName.isEmpty()) {
                    text = QString("%1 - %2 (%3 %4)").arg(parentClassName, name).arg(clients.size()).arg(tr("İstemci"));
                    tooltip = tr("Komutlar %1 - %2 grubundaki %3 istemciye gönderilecek").arg(parentClassName, name).arg(clients.size());
                } else {
                    text = name + QString(" (%1 %2)").arg(clients.size()).arg(tr("İstemci"));
                    tooltip = tr("Komutlar %1 grubundaki %2 istemciye gönderilecek").arg(name).arg(clients.size());
                }
            } else {
                text = name;
                tooltip = tr("Komutlar seçili ağdaki istemcilere gönderilecek: ") + name;
            }
        }
    }

    if (m_selectionSource == SelectionSource::None) {
        QStringList visible = visibleClientIds();
        if (m_classHeaderLabel && m_classHeaderLabel->isVisible() && !m_classHeaderLabel->text().trimmed().isEmpty()) {
            QString className = m_classHeaderLabel->text().trimmed();
            text = className + QString(" (%1 %2)").arg(visible.size()).arg(tr("İstemci"));
            tooltip = tr("Açık olan %1 sınıfındaki tüm (%2) istemci hedef alındı.").arg(className).arg(visible.size());
        } else {
            text = tr("Tüm Görünür İstemciler") + QString(" (%1)").arg(visible.size());
            tooltip = tr("Özel bir seçim yapılmadı. Komutlar ekranda görünen tüm (%1) istemciye gönderilecek.").arg(visible.size());
        }
    }

    m_targetBadge->setText(text);
    m_targetBadge->setToolTip(tooltip);

    if (isDark) {
        m_targetBadge->setStyleSheet(
            "QLabel#targetBadge {"
            "  background-color: rgba(59, 130, 246, 0.22);"
            "  color: #60a5fa;"
            "  border: 1px solid rgba(96, 165, 250, 0.45);"
            "  border-radius: 10px;"
            "  padding: 2px 10px;"
            "  font-size: 11px;"
            "  font-weight: 600;"
            "}");
    } else {
        m_targetBadge->setStyleSheet(
            "QLabel#targetBadge {"
            "  background-color: #eff6ff;"
            "  color: #1d4ed8;"
            "  border: 1px solid #bfdbfe;"
            "  border-radius: 10px;"
            "  padding: 2px 10px;"
            "  font-size: 11px;"
            "  font-weight: 600;"
            "}");
    }
}


