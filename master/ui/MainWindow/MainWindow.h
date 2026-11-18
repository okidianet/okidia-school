// master/ui/MainWindow/MainWindow.h
#pragma once

#include <QMainWindow>
#include <QTreeWidgetItem>
#include <QMap>
#include <QLabel>
#include "../ClientThumbnail/ClientThumbnail.h"
#include "../ThumbnailGroupFrame/ThumbnailGroupFrame.h"
#include "../../common/utils/Logger.h"
#include "../RemoteControlView/RemoteControlView.h"
#include "../../broadcast/ScreenBroadcaster.h"
#include "../../transfer/FileSender.h"
#include "../../exam/ExamDialog.h"
#include "../../schedule/ScheduleManager.h"
#include "../../audit/AuditLog.h"
#include "../../license/LicenseManager.h"
#include "../ReportPanel/ReportPanel.h"
#include "../SettingsDialog/SettingsDialog.h"
#include "../../backup/BackupManager.h"
#include "../../tree/TreeManager.h"          // ← YENİ
#include "../../chat/RoomManager.h"
#include "../../chat/PrivateChatManager.h"
#include "../ChatWindow/ChatWindow.h"
#include <QRubberBand>
#include <QPointer>
#include <QPair>
#include <QEnterEvent>
#include <QDockWidget>
#include <QTabWidget>
#include <QListWidget>
#include <QToolButton>
#include "../../../common/db/DbRecords.h"

class DeployPanel;  // forward declaration — DeployPanel.h MainWindow.cpp'de include edilir
class OtaUpdatePanel;
class SshManagerPanel;
class BroadcastOverlayWidget;

// Forward declaration
class SessionManager;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(SessionManager* session,
                        QWidget* parent = nullptr);
    ~MainWindow();
    void setActiveUser(const UserRecord& user);
    void applyOfflineModeConstraints(bool offline);
    void updateWindowTitle();

signals:
    void examStateChanged(bool active);
    void logoutRequested();  // Çıkış → login ekranına dön
    void windowClosed();     // Pencere X ile kapatıldığında tetiklenir

private slots:
    // --- Toolbar butonları ---
    void onLockAll();
    void onUnlockAll();
    void onLockToggle();
    void onBroadcast();
    void onSendMessage();
    void onSendFile(bool readOnly = false);
    void onTreeSelectionChanged();
    void onQualityChanged(int value);
    void onSettings();
    void onSwitchSchool();
    void onExit();
    void onLightTheme();
    void onDarkTheme();
    void onAbout();
    void onFpsChanged(int index);

    // --- Tree yönetimi ---
    void onTreeContextMenu(const QPoint& pos);
    void onTreeAddClass();
    void onTreeAddGroup();
    void onTreeRename();
    void onTreeDelete();

    // --- Faz 4: Thumbnail context menüsü ---
    void onThumbnailContextMenu(const QString& clientId,
                                const QPoint& globalPos);

    QString clientDisplayName(const QString& clientId) const;

    // --- Faz 4: Tek istemci komutları ---
    void onClientScreenLock(const QString& clientId);
    void onClientScreenUnlock(const QString& clientId);
    void onClientInputLock(const QString& clientId);
    void onClientInputUnlock(const QString& clientId);
    void onClientSendMessage(const QStringList& targets);
    void onClientScreenCapture(const QString& clientId);
    void onClientWakeOnLan(const QString& clientId);
    void onClientsWakeOnLan(const QStringList& clientIds);
    void onClientShutdown(const QString& clientId);
    void onClientsShutdown(const QStringList& clientIds);
    void onClientRestart(const QString& clientId);
    void onClientsRestart(const QStringList& clientIds);
    void onClientLogout(const QString& clientId);
    void onClientsLogout(const QStringList& clientIds);
    void onClientHibernate(const QString& clientId);
    void onClientsHibernate(const QStringList& clientIds);
    void onClientAudioMute(const QString& clientId);
    void onClientAudioUnmute(const QString& clientId);
    void onClientAppClose(const QString& clientId);
    void onClientViewOnly(const QString& clientId);
    void onClientTakeControl(const QString& clientId);
    void onClientAgentRestart(const QString& clientId);  // ← YENİ: SSH ile agent restart
    void onClientAssignType(const QString& clientId);
    void onClientPrivateChat(const QString& clientId);
    void onClientHandLower(const QString& clientId);

    // Faz 4b: ekran görüntüsü gelince
    void onScreenCaptureReceived(const QString& clientId,
                                  const QPixmap& pixmap,
                                  qint64 timestamp);

    // Faz 5: Sohbet
    void onChatMessageReceived(const QString& clientId,
                                const QString& roomId,
                                const QString& from,
                                const QString& content,
                                qint64         timestamp,
                                int            senderRole = 0,
                                bool           isSystem = false);
    // Faz 5: El kaldırma
    void onShowChat();
    void onDiscoveryRequest(const QString& targetId,
                            const okidia::DiscoveryRequest& req);
    void onDiscoveryRevoked(const QString& targetId);
    void onDiscoveryResolved(const QString& targetId,
                             const QString& resolvedBy,
                             const QString& action,
                             uint32_t agentType,
                             const QString& hostname);
    void onDiscoveryReject(const QString& targetId);
    void onDiscoveryIgnore(const QString& targetId, const QString& machineId);
    void setupDiscoveryPanel();
    void showDiscoveryPanel();
    void updateFloatingDiscoveryPillGeometry();
    void updateDiscoveryBadge();
    void onClientRevoke(const QString& clientId);
    void onManageClients();
    void onHandRaiseReceived(const QString& clientId,
                              const QString& message);
    void onHandLowerReceived(const QString& clientId);

    // Faz 5: Ekran yayını
    void onBroadcastToggle();

    // Çoklu monitör: istemci ekran kaynağı seçimi
    void onClientSelectScreen(const QString& clientId);
    void onScreenInfoReceived(const QString& clientId,
                               const okidia::ScreenInfoResponse& info);

    // Faz 6: Sınav
    void onStartExam();
    void onCollectExam();
    void onEndExam();

    void onOpenSshTerminalForClient(const QString& clientId);
    void onRemoteControlResponseReceived(const QString& clientId, bool accepted, const QString& reason);

    // Sunucu (Broker) Yönetimi — Yalnızca YM
    void onBrokerWakeOnLan();
    void onBrokerSshConnect();
    void onBrokerRestartService();
    void onBrokerRebootServer();
    void onBrokerInfo();
    void onBrokerUpdateManager();

    // Faz 6: Web filtreleme (context menüsünden)
    void onClientWebBlockAll(const QString& clientId);
    void onClientWebUnblock(const QString& clientId);
    void onManageAppBlacklist(const QStringList& clientIds);
    void onManageWebBlacklist(const QStringList& clientIds);

    // Faz 6: Ses yayını
    void onSendAudio();
    void onAudioLiveToggle();

    // Faz 6: Zamanlı komut
    void onScheduleCommand();

    // Faz 6: Güç yönetimi (Toolbar)
    void onPowerWakeOnLan();
    void onPowerShutdown();
    void onPowerRestart();
    void onPowerLogout();
    void onPowerHibernate();
    void onPowerWakeOnLanAll();
    void onPowerShutdownAll();

    // Faz 7: Raporlama
    void onShowReport();
    void onShowUserManagement();
    void onShowLogViewer();
    void onShowLogViewerForClient(const QString& clientId);
    void onPanelContextMenuRequested(const QPoint& pos);

    // Master OTA Güncelleme Bildirimi
    void onMasterUpdateAvailable(const QString& version, const QString& filename, quint64 size, const QString& osTag, const QString& osArch);
    void onMasterUpdateBtnClicked();
    void onBrokerOutdatedWarning(const QString& brokerVer, const QString& masterVer);

    // Windows Ajan Kaldırma Koruması
    void onChangeUninstallPassword();
    void checkDefaultUninstallPasswordWarning();

    // Zaman Senkronizasyonu & Donanım Arıza Bildirimleri
    void onBrokerTimeWarning(bool isDesync, int remainingLogins, const QString& warningMsg, bool isLocked);
    void onClientHardwareAlert(const QString& machineId, const QString& hostname, const QString& alertType, int count, const QString& details);
    void onBrokerStorageWarning(quint64 availableBytes, quint64 requiredBytes, const QString& warningMsg);

    // Yetki / Rol Değişikliği Bildirimi (Re-login)
    void onUserRoleChanged(const QString& newRole, const QString& displayName, const QString& message);

private:
    Ui::MainWindow*                  ui;
    SessionManager*                  m_session = nullptr;
    UserRecord                       m_activeUser;
    QMap<QString, ClientThumbnail*>  m_thumbnails;

    QPushButton*                     m_masterUpdateBtn = nullptr;

    QMenu* m_menuKisitli = nullptr;
    QAction* m_actReconnect = nullptr;
    QAction* m_actUninstallPass = nullptr;
    void syncUiWithDatabase();

    // Uzak kontrol pencereleri
    QList<RemoteControlView*> m_remoteViews;
    QDialog*                  m_remoteControlWaitDialog = nullptr;
    QTimer*                   m_waitDialogTimer = nullptr;
    int                       m_waitDialogSeconds = 0;
    
    // Çift tıkla otomatik açma takibi
    bool                      m_autoControlRequested = false;
    QString                   m_autoControlTarget;
    RemoteControlView::Mode   m_autoControlMode = RemoteControlView::Mode::Control;
    ChatWindow*         m_chatWindow       = nullptr;
    RoomManager*        m_roomMgr          = nullptr;
    PrivateChatManager* m_privateChatMgr   = nullptr;
    class QDialog*      m_discoveryPanel   = nullptr;
    class QTableWidget* m_discoveryTable   = nullptr;
    QPushButton*        m_floatingDiscoveryPill = nullptr;
    // peerIp → CSR map
    QMap<QString, QByteArray> m_pendingCsrMap;
    ScreenBroadcaster*      m_broadcaster      = nullptr;
    BroadcastOverlayWidget* m_broadcastOverlay = nullptr;
    ScheduleManager*    m_scheduler        = nullptr;
    QString             m_activeExamId;
    ReportPanel*        m_reportPanel      = nullptr;
    QTimer*             m_audioResetTimer  = nullptr;  // ses bitince UI sıfırla
    DeployPanel*        m_deployPanel      = nullptr;
    OtaUpdatePanel*     m_otaUpdatePanel   = nullptr;
    SshManagerPanel*    m_sshManagerPanel  = nullptr;
    FileSender*         m_fileSender       = nullptr;
    int                 m_thumbPx          = 220;
    QPointer<QMenu>     m_activeThumbnailMenu;

    // Tree yöneticisi — hiyerarşi, kalıcı kayıt, sürükle-bırak
    TreeManager*        m_treeManager      = nullptr;
    class UserManagementPanel* m_userMgmtPanel = nullptr;
    QPointer<QDialog>   m_clientMgmtDialog;
    void openClientSsh(const QString& clientId);

    // ── Grup layout yönetimi ──────────────────────────────────────────────
    QLabel*  m_classHeaderLabel = nullptr;  // sticky — scroll dışı
    QMap<QString, class ThumbnailGroupFrame*> m_groupFrames;  // nodeId → frame
    QTimer*  m_rebuildLayoutTimer = nullptr;
    QTreeWidgetItem* m_currentScopeNode = nullptr;
    QTreeWidgetItem* currentClassScopeNode() const;
    void scheduleRebuildGroupLayout(QTreeWidgetItem* classNode = nullptr);
    void rebuildGroupLayout(QTreeWidgetItem* classNode);
    void clearGroupLayout();
    ThumbnailGroupFrame* getOrCreateGroupFrame(const QString& nodeId,
                                                const QString& name,
                                                bool isUnassigned = false);
    void updateTreeLockEmojis(const QString& classNodeId,
                               bool chatLocked, bool privateLocked);
    void refreshAllTreeLockEmojis();

    // Alt pencereler ve sol ağaç kontrolü
    QMap<QString, QList<QPair<QString,QString>>> m_appLists;
    QMap<QString, bool>          m_nodeLockState;  // nodeId → kilitli mi
    QAction*            m_toggleTreeAction = nullptr;
    QWidget*            m_sysMgmtWindow    = nullptr;
    QTabWidget*         m_sysMgmtTabs      = nullptr;

    void setupConnections();
    void setupMainMenu();
    void buildClientMenu(QMenu* menu, const QString& singleClientId);
    void setupTree();
    void setupSplitter();
    void setupDock();
    void setupTreeDock();
    void setupSysMgmtTabs();
    void showSystemManagementWindow(QWidget* targetTab = nullptr);
    QMenu* createPanelToggleMenu();
    void updateStatusBar(const QStringList& clientIds);
    void addTestClients();
    void addThumbnail(const QString& clientId,
                      const QString& hostname,
                      const QString& username);

    void applyThumbnailSize(int px);
    void relayoutThumbnails();

    void handleThumbnailClick(const QString& clientId,
                               Qt::KeyboardModifiers mods);

    void updateLockToggleState();
    QStringList commandTargets() const;
    QString commandTargetsDescription() const;
    QStringList selectedClientIds() const;
    QStringList visibleClientIds() const;

    void showCommandStatus(bool ok, const QString& text);
    void stopBroadcast();

    bool isObserver() const { return m_activeUser.role == "OBSERVER"; }
    void onClassFilterChanged(int index);

    // Tree görünüm filtresi (TreeManager::filterItem'ı çağırır)
    void filterThumbnailsByNode(QTreeWidgetItem* node);

    // OM yetki uygulama
    void applyOMPermissions();         // tree görünümü + thumbnail filtrelemesi
    void updateThumbnailVisibility();  // mevcut seçime göre thumbnail'ları güncelle

    // Çoklu seçim
    QRubberBand* m_rubberBand    = nullptr;
    QPoint       m_rubberOrigin;
    QString      m_lastClickedId;
    QList<QPair<ClientThumbnail*, QRect>> m_cachedThumbRects;
    void onRemoteViewStopped(const QString& clientId);

    // Çevrimdışı Mod
    QLabel* m_offlineBanner = nullptr;

    // Boş Durum (Empty State) ve Canlı Akış Kontrolü
    QLabel*      m_emptyStateLabel = nullptr;
    QPushButton* m_btnStreamToggle = nullptr;
    void showEmptyStateMessage(const QString& msg);
    void hideEmptyStateMessage();

    // Bağlantı Biçimi Göstergesi
    QLabel* m_netModeTitle = nullptr;
    QLabel* m_netModeBadge = nullptr;
    void setupStatusBarNetworkMode();
    void updateNetworkModeBadge();

    // Seçim Kaynağı ve Hedef Göstergesi (Last-Active Selection Context)
    enum class SelectionSource {
        None,
        Tree,
        Thumbnail
    };
    SelectionSource m_selectionSource = SelectionSource::None;
    QLabel* m_targetBadgeTitle = nullptr;
    QLabel* m_targetBadge = nullptr;
    void setupStatusBarTargetBadge();
    void updateSelectionTargetBadge();

    // Oturum Güvenliği ve Kilit Ekranı (Idle Lock & Session Timeout)
    class MasterLockOverlay* m_lockOverlay = nullptr;
    QTimer* m_idleTimer = nullptr;
    QTimer* m_maxSessionTimer = nullptr;
    qint64  m_lastActivityTime = 0;
    int     m_idleTimeoutMinutes = 15;
    int     m_maxSessionHours = 8;
    QList<QPointer<QWidget>> m_hiddenWindowsOnLock;
    void setupSessionSecurity();
    void applySessionTimeoutSettings(int idleMinutes, int maxHours);

public slots:
    void lockScreen();
    void unlockScreen();
    bool isScreenLocked() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
};
