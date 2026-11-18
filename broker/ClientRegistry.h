// broker/ClientRegistry.h
#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QDateTime>
#include "../common/network/Connection.h"
#include "../common/db/OkidiaDatabase.h"
#include "messages.pb.h"

struct RegisteredClient {
    QString     clientId;
    QString     hostname;
    QString     username;
    QString     displayName;
    QString     machineId;
    Connection* connection   = nullptr;
    qint64      lastHeartbeat = 0;  // Unix timestamp (sn) — 0 = henüz heartbeat gelmedi
};

struct AgentLock {
    QString masterUserId;
    QString masterDisplayName;
    QString lockType; // "Exam", "ScreenLock", "InputLock", "RemoteControl", "Audio", "Broadcast"
    qint64  lockedAt = 0;
};

class ClientRegistry : public QObject {
    Q_OBJECT

public:
    explicit ClientRegistry(QObject* parent = nullptr);

    // İstemci kaydet
    void registerClient(const QString& clientId,
                        const RegisteredClient& client);

    // İstemci sil
    void unregisterByConnection(Connection* conn);

    // Heartbeat zaman damgasını güncelle (broker/main.cpp'den çağrılır)
    void updateHeartbeat(const QString& clientId);

    // Heartbeat timeout izleyiciyi başlat (broker başlangıcında çağrılır)
    // timeoutSec: bu süre heartbeat gelmezse bağlantı kesilir (varsayılan 90 sn)
    void startHeartbeatWatchdog(int timeoutSec = 90);
    void stopHeartbeatWatchdog();

    // Master bağlantısı ayarla
    void addMasterConnection(Connection* conn);
    void removeMasterConnection(Connection* conn);
    void sendAllClientsToMaster(Connection* conn);
    bool hasMaster() const { return !m_masterConnections.isEmpty(); }
    bool isMasterConnection(Connection* conn) const { return m_masterConnections.contains(conn); }

    // Mesajı master'a ilet (clientId ekleyerek)
    void forwardToMaster(const QString& clientId,
                         const okidia::Envelope& envelope);

    // Mesajı belirli bir istemciye ilet
    void forwardToClient(const QString& clientId,
                         const okidia::Envelope& envelope);

    // sequence_id hash'inden clientId bul
    QString clientIdByHash(uint hash) const;

    // Discovery — sertifikasız bekleyen bağlantılar (machineId → bağlantı)
    void addPendingDiscovery(const QString& machineId, Connection* conn);
    void storePendingRequest(const QString& machineId,
                             const okidia::DiscoveryRequest& req);
    bool isPendingConnection(Connection* conn) const;
    void forwardToPending(const QString& machineId,
                          const okidia::Envelope& envelope);
    void removePending(const QString& machineId);
    void removePendingByConnection(Connection* conn);
    void disconnectPending(const QString& machineId);

    // Pending discovery verisi
    QByteArray                  pendingCsr(const QString& machineId) const;
    okidia::DiscoveryRequest    pendingRequest(const QString& machineId) const;
    QStringList                 pendingIds() const;

    // Master bağlantısı erişimi (tray durum bildirimi için)
    QSet<Connection*> masterConnections() const { return m_masterConnections; }

    // Tüm kayıtlı istemciler
    QList<RegisteredClient> allClients() const;
    bool hasClient(const QString& clientId) const;
    RegisteredClient client(const QString& clientId) const { return m_clients.value(clientId); }
    QList<Connection*> allMasterConnections() const { return m_masterConnections.values(); }

    // İstemci iptali (revoke)
    void revokeClient(const QString& machineId, bool uninstall = false);
    void unrevokeClient(const QString& machineId);
    void revokeAll(bool uninstall = false);
    bool isRevoked(const QString& machineId) const;
    // DB entegrasyonu — broker başlangıcında çağrılır
    void loadFromDb();

    // Ağ Login Desteği
    void setAuthenticatedUser(Connection* conn, const QString& userId, const QString& username = QString(), const QString& displayName = QString(), const QString& role = QString(), const QString& machineId = QString(), const QString& hostname = QString());
    QString authenticatedUserId(Connection* conn) const;
    QString authenticatedUsername(Connection* conn) const;
    QString authenticatedDisplayName(Connection* conn) const;
    QString authenticatedRole(Connection* conn) const;
    QString authenticatedMachineId(Connection* conn) const;
    QString authenticatedHostname(Connection* conn) const;
    void clearAuthentication(Connection* conn);
    
    // Agent Lock Yönetimi
    bool acquireAgentLock(const QString& clientId, const QString& masterUserId, const QString& masterDisplayName, const QString& lockType);
    void releaseAgentLock(const QString& clientId);
    void releaseAllLocksForMaster(const QString& masterUserId);
    bool isAgentLockedByOther(const QString& clientId, const QString& masterUserId) const;
    QMap<QString, AgentLock> activeLocks() const { return m_agentLocks; }

    // ── Pub/Sub Abonelik Yönetimi (Ekran ve Telemetri Çoklama) ───────────
    bool subscribeScreen(Connection* masterConn, const QString& agentId, bool* isFirst = nullptr);
    bool unsubscribeScreen(Connection* masterConn, const QString& agentId, bool* isLast = nullptr);
    bool subscribeMetrics(Connection* masterConn, const QString& agentId, bool* isFirst = nullptr);
    bool unsubscribeMetrics(Connection* masterConn, const QString& agentId, bool* isLast = nullptr);
    void unsubscribeAll(Connection* masterConn, QStringList* screenEmptyAgents = nullptr, QStringList* metricsEmptyAgents = nullptr);
    QSet<Connection*> screenSubscribers(const QString& agentId) const;
    QSet<Connection*> metricsSubscribers(const QString& agentId) const;
    bool hasScreenSubscribers(const QString& agentId) const;
    bool hasMetricsSubscribers(const QString& agentId) const;

    // Eski dosya tabanlı metodlar — geriye dönük uyumluluk (kullanılmayacak)
    void loadRevokedList() { loadFromDb(); }
    void saveRevokedList() const {}  // artık DB otomatik yazar

signals:
    void clientRegistered(const QString& clientId);
    void clientUnregistered(const QString& clientId);
    void brokerStatusChanged();
    void clientRevokedBroadcast(const QString& machineId);
    void agentLocksChanged();

private slots:
    void onHeartbeatWatchdog();  // periyodik kontrol

private:
    QMap<QString, RegisteredClient>         m_clients;
    QMap<QString, Connection*>              m_pending;           // machineId → bağlantı
    QMap<QString, okidia::DiscoveryRequest> m_pendingRequests;   // machineId → DiscoveryRequest (CSR dahil)
    QSet<QString>                           m_revoked;
    QSet<Connection*>                       m_masterConnections;
    QTimer*                                 m_hbWatchdog    = nullptr;  // heartbeat timeout timer
    struct AuthenticatedMaster {
        QString userId;
        QString username;
        QString displayName;
        QString role;
        QString machineId;
        QString hostname;
    };
    QMap<Connection*, AuthenticatedMaster>  m_authenticated; // Baglanti -> User Bilgisi
    int                                     m_hbTimeoutSec  = 90;       // varsayılan 90 sn
    QMap<QString, AgentLock>                m_agentLocks;       // clientId -> AgentLock
    QHash<QString, QSet<Connection*>>       m_screenSubscribers;   // agentId -> Set<MasterConnection>
    QHash<QString, QSet<Connection*>>       m_metricsSubscribers;  // agentId -> Set<MasterConnection>
};