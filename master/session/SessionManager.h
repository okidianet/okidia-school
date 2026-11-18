// master/session/SessionManager.h
#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QAudioFormat>
#include "../../common/network/TlsClient.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/DbRecords.h"
#include "../../common/network/MdnsManager.h"
#include "../../common/network/Connection.h"
#include "NetworkWatcher.h"
#include "../ui/ClientThumbnail/ClientThumbnail.h"

struct AuditEvent;
struct RoomMessage;

inline okidia::Envelope makeEnv(okidia::MessageType type) {
    okidia::Envelope env;
    env.set_type(type);
    return env;
}

class SessionManager : public QObject {
    Q_OBJECT

public:
    explicit SessionManager(QObject* parent = nullptr);
    ~SessionManager();

    // ── Bağlantı Biçimi (Modu) ───────────────────────────────────────────
    enum class ConnectionMode {
        Disconnected, // Bağlantı Yok / Aranıyor
        Local,        // Yerel Ağ
        Remote,       // Uzak Ağ
        SshOnly       // SSH Yönetimi (Çevrimdışı)
    };
    Q_ENUM(ConnectionMode)

    ConnectionMode connectionMode() const;
    QString connectionModeString() const;
    QString connectionDetails() const;

    // ── Aktif kullanıcı (login sonrası set edilir) ─────────────────────────
    void           setActiveUser(const UserRecord& user);
    bool isConnected() const;
    TlsClient* tlsClient() const { return m_tls; }
    Connection* connection() const { return (m_tls && m_tls->connection()) ? m_tls->connection() : m_conn; }

    const UserRecord& activeUser() const { return m_activeUser; }
    bool           isYM() const { return m_activeUser.role == "YM"; }
    bool           isOM() const { return m_activeUser.role == "OM"; }
    bool           isObserver() const { return m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM"; }

    // OM yetki kontrolü — YM için her zaman true, OBSERVER için her zaman false
    // classId boşsa (root/genel) YM=true, OM/OBSERVER=false
    bool canManageClass(const QString& classId) const;
    bool canManageClient(const QString& clientId) const;
    const QSet<QString>& managedClassIds() const { return m_managedClassIds; }

    // Delege listesini yenile (login sonrası + periyodik çağrılır)
    void refreshPermissions();

    // Broker'a bağlan
    void loadFromDatabase();
    void clearCredentials();  // Logout sonrası otomatik giriş engeli için kimlik bilgilerini temizle
    void connectToBroker(const QString& certPath,
                         const QString& keyPath,
                         const QString& caCertPath);
    void connectToBrokerAnonymous(const QString& host, quint16 port);

    // Broker'a bağlandığında tüm agent'lardan CLIENT_INFO + frame yenile
    // Yeniden bağlantı sonrası veya ağ değişimi sonrası çağrılır.
    void sendRefreshBroadcast();
    void setVisibleClients(const QSet<QString>& visibleClients);
    QSet<QString> visibleClients() const { return m_visibleClients; }
    void updateAdaptiveFps();
    int calculateAdaptiveFps() const;
    int calculateAdaptiveFps(int visibleCount) const;

    void setClientStreaming(const QString& clientId, bool streaming);
    bool isClientStreaming(const QString& clientId) const;

    // mDNS ile otomatik broker bul ve bağlan
    void startAutoDiscovery(const QString& certPath,
                            const QString& keyPath,
                            const QString& caCertPath);
    void tryRemoteBrokerFallback();
    QString activeNetworkKey() const;
    void switchNetwork(const QString& networkKey);

    // İstemci thumbnail'larını kaydet
    void registerThumbnail(const QString& clientId,
                           ClientThumbnail* thumbnail);
    void unregisterThumbnail(const QString& clientId);
    void clearThumbnails();

    // Komut gönder
    bool sendCommand(const QString& clientId,
                     const okidia::Envelope& envelope);
    bool broadcastCommand(const okidia::Envelope& envelope);

    // --- Faz 4 kolaylık metodları ---

    // Remote Control
    bool sendRemoteControlRequest(const QString& clientId, bool isControl);
    // Ekran
    bool sendScreenLock(const QString& clientId,
                        const QString& message = QString(),
                        uint32_t lockPolicy = 0);
    bool sendScreenUnlock(const QString& clientId);
    bool sendScreenCapture(const QString& clientId);
    bool sendAppListRequest(const QString& clientId);
    bool sendAppCloseByWid(const QString& clientId, const QString& wid);
    bool sendRefreshRequest(const QString& clientId);
    bool sendScreenConfig(const QString& clientId, int intervalMs, int quality = 0, int maxWidth = 0);
    bool broadcastScreenLock(const QString& message = QString(),
                             uint32_t lockPolicy = 0);
    bool broadcastScreenUnlock();
    bool broadcastScreenConfig(int intervalMs, int quality = 0);

    // Input
    bool sendInputLock(const QString& clientId);
    bool sendInputUnlock(const QString& clientId);
    bool sendTakeControl(const QString& clientId);
    bool sendRemoteControlStop(const QString& clientId);
    bool broadcastInputLock();
    bool broadcastInputUnlock();

    // Fare / Klavye enjeksiyonu (uzak kontrol)
    bool sendMouseEvent(const QString& clientId,
                        const okidia::MouseEvent& evt);
    bool sendKeyEvent(const QString& clientId,
                      const okidia::KeyEvent& evt);

    // Güç
    bool sendWakeOnLan(const QString& clientId);
    bool broadcastWakeOnLan();
    bool sendShutdown(const QString& clientId, int delaySecs = 0,
                      const QString& reason = QString());
    bool sendRestart(const QString& clientId,  int delaySecs = 0,
                     const QString& reason = QString());
    bool sendLogout(const QString& clientId);
    bool sendHibernate(const QString& clientId);
    bool broadcastShutdown(int delaySecs = 0);
    bool broadcastRestart(int delaySecs = 0);

    // Ses
    bool sendAudioMute(const QString& clientId);
    bool sendAudioUnmute(const QString& clientId);
    bool sendAudioSetVolume(const QString& clientId, int volume);
    bool broadcastAudioMute();
    bool broadcastAudioUnmute();

    // Faz 6: Ses yayını
    bool broadcastAudioFile(const QString& filePath,
                            const QStringList& targets = QStringList(),
                            int repeatCount = 1);
    bool broadcastAudioStop();

    // Anons Kaydet
    bool startRecording(const QAudioDevice& device = QAudioDevice());
    QString stopRecording();
    bool isRecording() const { return m_audioSource != nullptr; }
    QString lastAudioWavPath() const { return m_audioWavPath; }

    // Canlı Ses Yayını (Live Audio Streaming / Push-to-Talk)
    bool startLiveAudioBroadcast(const QStringList& targets = QStringList(),
                                 const QAudioDevice& device = QAudioDevice());
    void stopLiveAudioBroadcast();
    bool isLiveAudioBroadcasting() const { return m_liveAudioSource != nullptr; }

    // Anons gönder: file transfer → /tmp → AUDIO_PLAY_LOCAL
    void sendAnons(const QString& wavPath,
                   const QStringList& targets,
                   int repeatCount = 1);

    // Discovery — onay/yoksayma
    void approveDiscovery(const QString& targetId,
                          const QByteArray& csrPem,
                          uint32_t agentType = 0);
    void ignoreDiscovery(const QString& targetId,
                         const QString& machineId);
    // Reddet: bağlantıyı kapat, kara listeye almaz, tekrar istek yollayabilir
    void rejectDiscovery(const QString& targetId);
    // Kara liste yönetimi
    QStringList ignoredMachineIds() const { return m_ignoredMachineIds.values(); }
    void removeFromIgnoreList(const QString& machineId);

    // İstemci iptali
    // revokeClient: tek istemciyi sistemden çıkar (sertifikasını geçersiz kıl)
    // resetAllClients: tüm kayıtlı istemcileri sıfırla
    void revokeClient(const QString& clientId, bool uninstall = false);
    void resetAllClients(bool uninstall = false);
    QStringList revokedMachineIds() const { return m_revokedMachineIds.values(); }
    void clearRevokedList();

    // Mesaj popup
    bool sendPopup(const QString& clientId, const QString& message,
                   const QString& senderName = QString());
    bool broadcastPopup(const QString& message,
                        const QString& senderName = QString());

    // Uygulama
    bool sendAppClose(const QString& clientId, const QString& appName);
    bool sendAppCloseAll(const QString& clientId);
    bool sendAppBlockByName(const QString& clientId,
                            const QString& appName);
    bool sendAppBlockList(const QString& clientId,
                          const QStringList& appList);
    bool sendAppBlockByHash(const QString& clientId,
                            const QString& sha256Hash);
    bool sendAppWhitelist(const QString& clientId,
                          const QStringList& allowedApps);
    bool sendAppUnblock(const QString& clientId);
    bool sendAgentTypeSet(const QString& clientId, uint32_t type);
    bool sendAgentAliasSet(const QString& machineId, const QString& alias);
    bool sendCustomProfileUpsert(const CustomProfileRecord& prof);
    bool sendCustomProfileDelete(int profileId);

    // SSH Kimlik Yönetimi
    void loadLocalSshCredentials();
    void syncPendingSshCredentials();
    void fetchSshCredentials();
    void addSshCredential(const SshCredentialRecord& cred);
    void deleteSshCredential(const QString& id);
    QList<SshCredentialRecord> sshCredentials() const { return m_sshCredentials; }
    QMap<QString, ClientState> clientStates() const { return m_clientStates; }
    
    // Host OS önbelleği (IP → osTag)
    void setHostOs(const QString& ip, const QString& osTag);
    QString hostOs(const QString& ip) const;
    
    // Broker ayarları ve yedekleme
    bool sendSetBrokerPort(quint16 newPort);
    bool sendSetBrokerConfig(const QString& name, quint16 port);
    bool sendBrokerSettingsUpdate(int retentionDays, int backupRetentionCount = 7, bool includeChat = false, 
                                 const QString& backupPassword = QString(),
                                 const QString& schoolScheduleJson = QString(),
                                 const QString& schoolTimezone = QString(),
                                 uint32_t smartboardLockPolicy = 0);
    bool requestDbBackup();
    bool requestSystemBackup(const QString& targetDir, const QString& password, bool includeAudit = true, bool includeChat = false);
    bool sendDbRestoreRequest(const QByteArray& encryptedData, const QByteArray& brokerKey = QByteArray(), 
                              const QMap<QString, QByteArray>& identityBundle = {}, const QString& setupCode = QString(),
                              const QByteArray& encryptedAuditData = QByteArray(), const QByteArray& encryptedChatData = QByteArray(),
                              bool restoreAudit = true, bool restoreChat = true);

    // Broker Güncelleme Yönetimi
    bool requestBrokerUpdatesList();
    bool requestBrokerUpdatesDelete(const QStringList& filenames);

    // --- Uzaktan Veritabanı Değişiklik Komutları (Remote DB Mutation RPC) ---
    bool sendDbUserAdd(const QString& username, const QString& displayName, const QString& password, const QString& role, bool isBackupReplica = false, const QString& userId = QString(), const QString& replicaMachineId = QString(), const QString& replicaHostname = QString());
    bool sendDbUserUpdate(const QString& userId, const QString& displayName, const QString& role, const QString& passwordHash = QString(), bool isBackupReplica = false, const QString& replicaMachineId = QString(), const QString& replicaHostname = QString());
    bool sendDbUserSetReplica(const QString& userId, bool isBackupReplica, const QString& replicaMachineId = QString(), const QString& replicaHostname = QString());
    bool sendDbUserDelete(const QString& userId);
    bool sendDbUserPwReset(const QString& userId, const QString& password, bool isAdminReset);
    bool sendDbClassAdd(const QString& name, const QString& icon, const QString& parentId, const QString& type, const QString& id = QString());
    bool sendDbClassDelete(const QString& classId);
    bool sendDbClassRename(const QString& classId, const QString& newName);
    bool sendDbAgentAssign(const QString& agentId, const QString& groupId);
    bool sendDbUserGrant(const QString& userId, const QString& classId, const QString& level);
    bool sendDbUserRevoke(const QString& userId, const QString& classId);
    bool sendDbTeacherUpsert(const TeacherRecord& teacher);
    bool sendDbTeacherDelete(const QString& teacherId);
    bool sendTeacherKeySyncToAll();
    bool sendQueryLocalUsers(const QString& clientId);
    bool sendAuditLogRecord(const QString& actor, const QString& target, const QString& detail, const QString& category, const QString& userId, const QString& masterId, qint64 timestamp);
    bool sendAuditLogQuery(qint64 fromTimestamp,
                           qint64 toTimestamp,
                           const QString& category,
                           int limit,
                           int offset = 0,
                           const QString& searchQuery = QString(),
                           const QString& actor = QString(),
                           const QString& target = QString());
    bool sendChatHistoryRequest(const QString& roomId, int limit, qint64 beforeTimestamp);

    void requestJoinCode(const QString& userId);
    void sendDbSyncSignal();
    bool pushDatabase();
    bool sendAgentUninstallPassword(const QString& hash);

    // Log Görüntüleyici (Tail / Query)
    bool sendLogTailRequest(const QString& targetId,
                            int maxLines = 500,
                            const QString& fromTime = QString(),
                            const QString& toTime = QString(),
                            const QString& levelFilter = QString(),
                            const QString& searchQuery = QString(),
                            const QString& targetFile = QString());

    // Kararlı Sürüme Geri Dön (Rollback)
    bool sendRollbackToStable(const QString& clientId, const QString& targetVersion = QString());


    // --- Ağ Login ve Otomatik Sertifika ---
    void requestLogin(const QString& username, const QString& password);
    void updateCachedPassword(const QString& hash);
    const QString& passwordHash() const { return m_passwordHash; }
    void requestEnroll(const QString& csrPem, const QString& hostname);
    void requestSetRecoveryKey(const QString& keyHash);
    void requestPasswordReset(const QString& recoveryKey, const QString& newPasswordHash);

    // --- Çevrimdışı (Offline) Mod ---
    void setPendingCredentials(const QString& username, const QString& hash);
    void setOfflineMode(bool offline) { m_isOfflineMode = offline; }
    bool isOfflineMode() const;
    bool isConnecting() const { return m_connecting; }
    void triggerReconnect();
    void setKickedToOffline(bool kicked) { m_isKickedToOffline = kicked; }
    bool isKickedToOffline() const { return m_isKickedToOffline; }

    // --- Ekran Akışı ve FPS Kontrolü ---
    void setStreamPaused(bool paused);
    bool isStreamPaused() const { return m_isStreamPaused; }
    void setUserFpsInterval(int intervalMs);
    int  userFpsInterval() const { return m_userConfiguredIntervalMs; }
    bool isUserFpsFixed() const { return m_isUserFpsFixed; }

    // --- Faz tamamlama: ClientState erişimi ---
    // Anlık durum sorgulama
    ClientState clientState(const QString& clientId) const;
    QList<ClientState> allClientStates() const;
    QStringList connectedClientIds() const;
    int connectedClientCount() const;
    bool isMasterOnline(const QString& masterId) const;
    bool isAgentWindowOpen(const QString& clientId) const;

    // Durum bayraklarını güncelle (komut sonrası UI sync için)
    void setClientScreenLocked(const QString& clientId, bool locked);
    void setClientInputLocked(const QString& clientId, bool locked);
    void setClientAudioMuted(const QString& clientId, bool muted);
    void setClientWebFiltered(const QString& clientId, bool filtered);
    void setClientAppBlocked(const QString& clientId, bool blocked);
    bool isClientAppBlocked(const QString& clientId) const;
    void setClientHandRaised(const QString& clientId, bool raised, const QString& message = QString());
    void setClientExamActive(const QString& clientId, bool active);

    // Sınıf sohbeti katılım izni istek ve yanıtı
    bool sendRoomJoinRequest(const QString& requestId, const QString& roomId, const QString& roomName,
                             const QString& fromUsername, const QString& fromDisplay, const QString& targetUsername = QString());
    bool sendRoomJoinResponse(const QString& requestId, const QString& roomId, const QString& fromUsername,
                              const QString& targetUsername, bool accepted, const QString& reason = QString());

    struct MasterUpdateInfo {
        QString version;
        QString filename;
        quint64 size = 0;
        QString osTag;
        QString osArch;
        QByteArray signature;
        QByteArray fileHash;
        QByteArray masterCert;
        bool available = false;
    };

    MasterUpdateInfo availableMasterUpdate() const { return m_availableMasterUpdate; }
    class MasterUpdater* masterUpdater() const { return m_masterUpdater; }
    bool requestMasterUpdateDownload(const QString& version, const QString& osTag, const QString& osArch);

    QString brokerVersion() const { return m_brokerVersion; }
    QString brokerOs() const { return m_brokerOs; }
    QString brokerOsTag() const { return m_brokerOsTag; }
    QString brokerArch() const { return m_brokerArch; }
    QStringList brokerAllIps() const { return m_brokerAllIps; }
    bool isBrokerOutdated() const { return m_isBrokerOutdated; }
    QString schoolName() const { return m_schoolName; }

signals:
    void schoolNameChanged(const QString& newSchoolName);

    // Discovery akışı
    void discoveryRequest(const QString& targetId,
                          const okidia::DiscoveryRequest& req);
    void discoveryRevoked(const QString& targetId);
    void discoveryResolved(const QString& targetId,
                           const QString& resolvedBy,
                           const QString& action,
                           uint32_t agentType,
                           const QString& hostname);
    void brokerConnected();
    void brokerDisconnected();
    void connectionModeChanged(SessionManager::ConnectionMode mode);
    void databaseReloaded();  // DB override edildiğinde UI'ın yenilenmesi için

    void delegationExpired();  // OM delegesi süresi doldu → MainWindow yeniler
    void clientConnected(const QString& clientId,
                         const ClientState& state);
    void clientLoadedOffline(const QString& machineId,
                              const ClientState& state);  // DB'den yüklenen kayıtlı agent
    void clientDisconnected(const QString& clientId);
    void agentTypeSetResult(const QString& clientId, bool approved, const QString& reason);
    void clientRemoved(const QString& clientId);  // onaylanmadan silinen (reject/ignore)
    void clientFrameReceived(const QString& clientId,
                             const QByteArray& jpegData);
    void clientTileReceived(const QString& clientId,
                            int x, int y, int w, int h,
                            const QByteArray& jpegData);
    void clientTileBatchFinished(const QString& clientId);
    void clientStreamSizeReceived(const QString& clientId, int width, int height);
    void remoteControlResponseReceived(const QString& clientId, bool accepted, const QString& reason);
    void appListReceived(const QString& clientId,
                          const QList<QPair<QString,QString>>& apps); // name, wid
    void localUsersReceived(const QString& clientId, bool success, const QString& errorMsg,
                            const QList<QPair<QString,QString>>& users); // username, display_name
    void remoteFrameReceived(const QString& clientId,
                              const QByteArray& jpegData,
                              int width, int height,
                              int cursorX = -1,
                              int cursorY = -1,
                              bool hasCursor = false);
    void cursorShapeChanged(const QString& clientId, int cursorId, int x, int y);
    void dbRestoreResult(bool success, const QString& errorMsg);
    void brokerPortChanged(bool success, quint16 newPort, const QString& errorMsg);
    void joinCodeReceived(const QString& code, bool success, const QString& errorMsg);
    void customProfileUpsertResult(bool success, const QString& errorMsg, int id);
    void customProfileDeleteResult(bool success, const QString& errorMsg);
    void dbSyncRequested();
    void auditLogQueryReceived(const QList<AuditEvent>& records, int totalCount);
    void chatHistoryReceived(const QString& roomId, const QList<RoomMessage>& records);
    void otaBrokerReportReceived(const QString& message, bool success);
    void masterUpdateAvailableReceived(const QString& version, const QString& filename, quint64 size, const QString& osTag, const QString& osArch);
    void brokerOutdatedWarning(const QString& brokerVer, const QString& masterVer);
    void userRoleChangedReceived(const QString& newRole, const QString& displayName, const QString& message);
    
    // Broker Güncelleme Deposu Sinyalleri
    void brokerUpdatesListReceived(const QVariantList& packages);
    void brokerUpdatesDeleteResult(bool success, const QString& errorMsg, const QStringList& deletedFiles);
    void hostOsUpdated(const QString& ip, const QString& osTag);

    // Ağ Login ve Enroll Sonuçları
    void loginResult(bool success, const QString& userId, const QString& role, const QString& displayName, const QString& errorMsg, bool mustChangePw = false);
    void enrollResult(bool success, const QString& certPem, const QString& caPem, const QString& errorMsg);
    void passwordResetResult(bool success, const QString& errorMsg);
    void dbUserPwResetResult(bool success, const QString& errorMsg);
    void dbUserGrantResult(bool success, const QString& errorMsg);
    void dbUserRevokeResult(bool success, const QString& errorMsg);
    void systemBackupFinished(bool success, const QString& filePath, const QString& errorMsg);
    void backupReplicaReceived(const QString& filePath);

    // Çevrimdışı (Offline) mod sinyalleri
    void switchedToOnlineMode();
    void offlineLoginFailed(const QString& reason);
    void kickedToOfflineMode();


    // Faz 4b: Ekran görüntüsü (SCREEN_CAPTURE_RESULT)
    void screenCaptureReceived(const QString& clientId,
                                const QPixmap& pixmap,
                                qint64 timestamp);

    // Oda sohbeti & Üye senkronizasyonu
    void chatMessageReceived(const QString& clientId,
                              const QString& roomId,
                              const QString& from,
                              const QString& content,
                              qint64         timestamp,
                              int            senderRole = 0,
                              bool           isSystem = false);
    void chatMemberJoinedReceived(const QString& roomId,
                                   const QString& clientId,
                                   const QString& displayName,
                                   const QString& roleStr);
    void chatMemberLeftReceived(const QString& roomId,
                                 const QString& clientId);
    void chatLockStateReceived (const QString& classId,
                                 bool chatLocked,
                                 bool privateLocked);

    // El kaldırma
    void handRaiseReceived(const QString& clientId,
                            const QString& message);
    void handLowerReceived(const QString& clientId);

    // Durum değişince (ikon güncellemesi için)
    void clientStateChanged(const QString& clientId,
                             const ClientState& state);

    // Çoklu monitör
    void screenInfoReceived(const QString& clientId,
                             const okidia::ScreenInfoResponse& info);

    // Özel sohbet sinyalleri
    void privateChatRequested(const QString& fromClientId,
                               const QString& fromDisplay,
                               const QString& fromHost,
                               const QString& fromUser,
                               const QString& requestId);
    void privateChatAccepted (const QString& requestId,
                               const QString& sessionId);
    void privateChatRejected (const QString& requestId,
                               int cooldownSeconds);
    void privateChatOpenedRemotely(const QString& sessionId,
                                   const QString& fromId,
                                   const QString& fromDisplay,
                                   const QString& fromHost);
    void privateChatMessageReceived(const QString& sessionId,
                                     const QString& content,
                                     qint64 timestamp);
    void privateChatClosed   (const QString& sessionId);

    // Sınıf sohbetine misafir katılım istek ve yanıt sinyalleri
    void roomJoinRequestReceived(const QString& requestId, const QString& roomId, const QString& roomName,
                                 const QString& fromUsername, const QString& fromDisplay, const QString& targetUsername);
    void roomJoinResponseReceived(const QString& requestId, const QString& roomId, const QString& fromUsername,
                                  const QString& targetUsername, bool accepted, const QString& reason);

    // Agent sohbet penceresi açık/kapalı
    void agentChatWindowChanged(const QString& clientId, bool open);

    // SSH Kimlikleri
    void sshCredentialsLoaded();

    // Log Görüntüleyici (Tail / Query)
    void logTailReceived(const QString& sourceId, const okidia::LogTailResponse& resp);

    // Windows İstemci Kaldırma Koruması
    void agentUninstallPassResult(bool success, const QString& errorMsg);

    // Zaman Senkronizasyonu & Donanım Arıza Bildirimleri
    void brokerTimeWarningReceived(bool isDesync, int remainingLogins, const QString& warningMsg, bool isLocked);
    void clientHardwareAlertReceived(const QString& machineId, const QString& hostname, const QString& alertType, int count, const QString& details);
    void brokerStorageWarningReceived(quint64 availableBytes, quint64 requiredBytes, const QString& message);

    // Akıllı Tahta (Tip 3) Dev PIN Telemetrisi
    void smartboardLockPinReceived(const QString& clientId, const QString& pin, int remainingSeconds);
    void smartboardUnlockedReceived(const QString& clientId, const QString& username, const QString& displayName);

    // Kararlı Sürüme Geri Dön (Rollback) Yanıtı
    void rollbackCompleted(const QString& clientId, bool success, const QString& errorMsg, const QString& restoredVersion);

    // Komut Sonucu / Kilit Ret Bildirimi
    void commandResultReceived(const QString& clientId, bool success, const QString& errorMsg);


private slots:
    void onBrokerConnected(Connection* conn);
    void onBrokerDisconnected();
    void onMessageReceived(const okidia::Envelope& envelope);
    void onServiceFound(const QString& name,
                        const QString& host,
                        quint16 port,
                        const QString& role = "SB",
                        const QString& caFingerprint = QString());

private:
    TlsClient*      m_tls        = nullptr;
    NetworkWatcher* m_netWatcher = nullptr;
    MdnsManager* m_mdns = nullptr;
    Connection*  m_conn = nullptr;

    QString m_certPath;
    QString m_keyPath;
    QString m_caCertPath;

    // clientId → thumbnail
    QMap<QString, ClientThumbnail*> m_thumbnails;

    // clientId → son bilinen durum
    QMap<QString, ClientState> m_clientStates;
    QMap<uint, QString> m_hashToClientId;
    QMap<QString, bool> m_knownOnlineMasters;
    QMap<QString, bool> m_agentWindowOpenMap;
    bool                m_connecting = false;
    bool                m_isSyncingDb = false;
    bool                m_isManualBackup = false;
    QString             m_manualBackupDir;
    QString             m_manualBackupPassword;
    bool                m_manualBackupIncludeAudit = true;
    bool                m_manualBackupIncludeChat = false;
    QByteArray          m_brokerKey;

    // SSH Kimlikleri
    QList<SshCredentialRecord> m_sshCredentials;
    QMap<QString, QString>     m_hostOsMap; // IP -> osTag

    // Anons kaydı — Saf Qt6 QAudioSource (cross-platform standart ses kaydı)
    class QAudioSource*         m_audioSource    = nullptr;
    class QFile*                m_audioRecordFile = nullptr;
    QAudioFormat                m_recordFormat;
    QString                     m_audioWavPath;

    // Canlı Anons Yayını — Saf Qt6 QAudioSource (Push-to-Talk)
    class QAudioSource*         m_liveAudioSource = nullptr;
    class QIODevice*            m_liveIoDevice    = nullptr;
    QAudioFormat                m_liveFormat;
    QStringList                 m_liveBroadcastTargets;
    // Client grup bilgisi (tree node id)
    QMap<QString, QString>     m_clientGroups;
    QSet<QString>              m_ignoredMachineIds;
    // targetId → DiscoveryRequest (approve sırasında agent bilgisi lazım)
    QMap<QString, okidia::DiscoveryRequest> m_pendingDiscoveryData;
    QSet<QString>              m_revokedMachineIds;  // kalıcı iptal listesi
    QSet<QString>              m_visibleClients;
    QSet<QString>              m_streamingClients;
    QSet<QString>              m_pendingVisibleClients;
    QTimer*                    m_visibleDebounceTimer = nullptr;
    int                        m_lastSentIntervalMs = -1;
    int                        m_userConfiguredIntervalMs = 1000; // Varsayılan 1 FPS (1000ms)
    bool                       m_isUserFpsFixed = true;          // Kullanıcı manuel FPS seçti mi?
    bool                       m_isStreamPaused = false;         // Akış tamamen duraklatıldı mı?
    void onVisibleDebounceTimeout();

     // ── Aktif kullanıcı ve yetki önbelleği ────────────────────────────────
     UserRecord          m_activeUser;
     QString             m_username;
     QString             m_passwordHash;

     // Çevrimdışı mod durumu
     bool                m_isOfflineMode = false;
     bool                m_isKickedToOffline = false;
     bool                m_isRemoteConnection = false;
     bool                m_isProbingRemote = false;
     QString             m_localBrokerHost;
     quint16             m_localBrokerPort = 11200;
     QTimer*             m_remoteProbeTimer = nullptr;

     static bool isReachableOrValidRemoteHost(const QString& host);

     QSet<QString>       m_managedClassIds;
     QMap<QString, qint64> m_delegationExpiry;
     QTimer*             m_delegationTimer = nullptr;  // delege bitiş kontrolü
     QTimer*             m_dbSyncDebounceTimer = nullptr; // DB sync sinyallerini debounce etmek için

     class MasterUpdater* m_masterUpdater = nullptr;
     MasterUpdateInfo     m_availableMasterUpdate;
     QString              m_brokerVersion;
     QString              m_brokerOs;
     QString              m_brokerOsTag;
     QString              m_brokerArch;
     QStringList          m_brokerAllIps;
     bool                 m_isBrokerOutdated = false;
     QString              m_schoolName;

    void loadRevokedList();
    void saveRevokedList() const;
    void checkDelegationExpiry();  // timer ile çağrılır, süresi biten delegeleri temizler

    void handleClientInfo(const okidia::Envelope& env);
    void cleanupRejectedClient(const QString& machineId);
    void handleScreenFrame(const okidia::Envelope& env,
                           const QString& clientId);
    void handleTileUpdate(const okidia::Envelope& env,
                          const QString& clientId);
    void handleHeartbeat(const okidia::Envelope& env,
                         const QString& clientId);
    void handleScreenCaptureResult(const okidia::Envelope& env,
                                    const QString& clientId);
    // Faz 5
    void handleChatMessage(const okidia::Envelope& env,
                            const QString& clientId);
    void handleHandRaise(const okidia::Envelope& env,
                          const QString& clientId);
    void handleHandLower(const QString& clientId);

    // Özel sohbet handler'ları
    void handlePrivateChatRequest (const okidia::Envelope& env, const QString& fromClientId);
    void handlePrivateChatAccept  (const okidia::Envelope& env, const QString& fromClientId);
    void handlePrivateChatReject  (const okidia::Envelope& env, const QString& fromClientId);
    void handlePrivateChatOpen    (const okidia::Envelope& env, const QString& fromClientId);
    void handlePrivateChatMessage (const okidia::Envelope& env, const QString& fromClientId);
    void handlePrivateChatClose   (const okidia::Envelope& env, const QString& fromClientId);
    void handleChatWindowState    (const okidia::Envelope& env, const QString& fromClientId);

    void handleScreenInfo(const okidia::Envelope& env,
                           const QString& clientId);
    void handleCursorShapeChange(const okidia::Envelope& env,
                                  const QString& clientId);

    void handleSshCredList(const okidia::Envelope& env);
    void handleJoinCodeResponse(const okidia::Envelope& env);

public:
    // Faz 5: Sohbet gönder
    bool sendChatMessage(const QString& roomId,
                         const QString& content,
                         const QString& from = "Öğretmen");

    // Faz 6: Web filtreleme
    bool sendWebBlockAll(const QString& clientId,
                         const QString& reason = QString());
    bool sendWebBlockDomains(const QString& clientId,
                             const QStringList& domains,
                             const QString& reason = QString());
    bool sendWebUnblock(const QString& clientId);
    bool broadcastWebBlockAll(const QString& reason = QString());
    bool broadcastWebBlockDomains(const QStringList& domains,
                                  const QString& reason = QString());
    bool broadcastWebUnblock();

    // Faz 6: Sınav
    bool sendExamStart(const QString& clientId,
                       const QString& examId,
                       const QString& title,
                       int durationSeconds = 0);
    bool broadcastExamStart(const QString& examId,
                             const QString& title,
                             int durationSeconds = 0);
    bool broadcastExamCollect(const QString& examId);
    bool broadcastExamEnd();

    // Faz: Çoklu monitör — ekran kaynağı seçimi
    bool sendScreenSelect(const QString& clientId,
                          int monitorIndex);
    bool sendScreenSelectWindow(const QString& clientId,
                                quint64 windowId);
    bool sendScreenSelectAll(const QString& clientId);
    bool sendScreenInfoRequest(const QString& clientId);

    // Client grup bilgisi (MainWindow tree'sinden sync edilir)
    void setClientGroup(const QString& clientId,
                         const QString& groupId);
    QString clientGroup(const QString& clientId) const;
    QStringList clientsInGroup(const QString& groupId) const;
};