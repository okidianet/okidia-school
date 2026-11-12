// broker/ClientRegistry.cpp
#include "ClientRegistry.h"
#include "../common/utils/Logger.h"
#include "../common/utils/OkidiaUtils.h"

ClientRegistry::ClientRegistry(QObject* parent)
    : QObject(parent) {}

void ClientRegistry::registerClient(const QString& clientId,
                                     const RegisteredClient& client) {
    m_clients[clientId] = client;
    Logger::info(QObject::tr("ClientRegistry: Kayıt → ") + clientId +
                 " (" + client.hostname + ")");
    emit clientRegistered(clientId);

    // Eğer bu agent için önceden kayıtlı Master ekran abonesi varsa akışı hemen tetikle
    if (hasScreenSubscribers(clientId)) {
        okidia::ScreenConfig cfg;
        cfg.set_interval_ms(500);
        okidia::Envelope fpsEnv;
        fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
        fpsEnv.set_payload(cfg.SerializeAsString());
        fpsEnv.set_target_id(clientId.toStdString());
        forwardToClient(clientId, fpsEnv);
        Logger::info(QObject::tr("ClientRegistry: Yeni bağlanan %1 için aktif ekran abonesi mevcut, SCREEN_FPS_SET tetiklendi").arg(clientId));
    }
    if (hasMetricsSubscribers(clientId)) {
        okidia::Envelope metEnv;
        metEnv.set_type(okidia::MessageType::METRICS_STREAM_START);
        metEnv.set_target_id(clientId.toStdString());
        forwardToClient(clientId, metEnv);
    }
}

void ClientRegistry::unregisterByConnection(Connection* conn) {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->connection == conn) {
            QString clientId = it.key();
            m_clients.erase(it);
            // m_screenSubscribers ve m_metricsSubscribers kayıtlarını silmiyoruz;
            // Master halen bağlı ve bu istemciyi izlemek istiyor olabilir.
            // İstemci yeniden bağlandığında hasScreenSubscribers() ile akış anında yeniden başlar.
            // Master koptuğunda ise removeMasterConnection() zaten temizliği yapar.
            Logger::info(QObject::tr("ClientRegistry: Silindi → ") + clientId);
            emit clientUnregistered(clientId);
            releaseAgentLock(clientId);

            // Master'a bildir
            okidia::Envelope env;
            env.set_type(okidia::MessageType::CLIENT_INFO);
            env.set_sequence_id(0);
            // clientId'yi payload olarak gönder
            okidia::ClientInfo info;
            info.set_machine_id(clientId.toStdString());
            info.set_hostname("__disconnected__");
            env.set_payload(info.SerializeAsString());
            forwardToMaster(clientId, env);
            return;
        }
    }
}

// ── Heartbeat Yönetimi ──────────────────────────────────────────────────────────────

void ClientRegistry::updateHeartbeat(const QString& clientId) {
    if (m_clients.contains(clientId)) {
        m_clients[clientId].lastHeartbeat = OkidiaUtils::nowSecs();
    }
}

void ClientRegistry::startHeartbeatWatchdog(int timeoutSec) {
    m_hbTimeoutSec = timeoutSec;
    if (!m_hbWatchdog) {
        m_hbWatchdog = new QTimer(this);
        connect(m_hbWatchdog, &QTimer::timeout,
                this, &ClientRegistry::onHeartbeatWatchdog);
    }
    // Her 30 saniyede bir kontrol et
    m_hbWatchdog->start(30000);
    Logger::info(QObject::tr("ClientRegistry: Heartbeat watchdog başlatıldı (timeout=") +
                 QString::number(timeoutSec) + QObject::tr("s)"));
}

void ClientRegistry::stopHeartbeatWatchdog() {
    if (m_hbWatchdog) m_hbWatchdog->stop();
}

void ClientRegistry::onHeartbeatWatchdog() {
    const qint64 now     = OkidiaUtils::nowSecs();
    QStringList  timedOut;

    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        const auto& c = it.value();
        if (c.lastHeartbeat == 0) continue;  // Henüz hiç heartbeat gelmedi — bağlantı taze
        if (now - c.lastHeartbeat > m_hbTimeoutSec) {
            Logger::warning(QObject::tr("ClientRegistry: Heartbeat timeout → ") + it.key() +
                            " (" + QString::number(now - c.lastHeartbeat) + QObject::tr(" sn)"));
            timedOut << it.key();
        }
    }

    for (const QString& clientId : timedOut) {
        if (!m_clients.contains(clientId)) continue;
        Connection* conn = m_clients[clientId].connection;
        m_clients.remove(clientId);
        emit clientUnregistered(clientId);
        releaseAgentLock(clientId);

        // Master'a bildir
        okidia::Envelope env;
        env.set_type(okidia::MessageType::CLIENT_INFO);
        env.set_sequence_id(0);
        okidia::ClientInfo info;
        info.set_machine_id(clientId.toStdString());
        info.set_hostname("__disconnected__");
        env.set_payload(info.SerializeAsString());
        forwardToMaster(clientId, env);

        // Bağlantıyı zorla kapat — agent’ın disconnected algılamasını sağlar
        if (conn) conn->disconnect();
    }
}

void ClientRegistry::addMasterConnection(Connection* conn) {
    if (m_masterConnections.contains(conn)) return;
    m_masterConnections.insert(conn);
    Logger::info(QObject::tr("ClientRegistry: Master bağlandı, toplam master: ") + QString::number(m_masterConnections.size()));

    // Mevcut istemcileri master'a bildir (sequence_id = qHash(clientId) ile)
    sendAllClientsToMaster(conn);

    // Bekleyen discovery isteklerini yeni master bağlantısına doğrudan ilet.
    // ESKİ DAVRANIS (HATALI): Pending bağlantıları koparıp yeniden bağlanmaya zorla.
    //   → Agent 5sn sonra yeni DISCOVERY_REQUEST gönderir → kullanıcı 2. kez onaylayabilir → kopya kayıt.
    // YENİ DAVRANIS: Mevcut pending DISCOVERY_REQUEST'leri yeni master'a tekrar gönder, bağlantıyı kesme.
    if (!m_pendingRequests.isEmpty()) {
        Logger::info(QObject::tr("ClientRegistry: ") +
                     QString::number(m_pendingRequests.size()) +
                     QObject::tr(" bekleyen discovery isteği yeni master'a iletiliyor (bağlantılar koparılmıyor)"));
        for (auto it = m_pendingRequests.constBegin(); it != m_pendingRequests.constEnd(); ++it) {
            const QString& machineId = it.key();
            const okidia::DiscoveryRequest& req = it.value();

            okidia::Envelope env;
            env.set_type(okidia::MessageType::DISCOVERY_REQUEST);
            env.set_target_id(machineId.toStdString());
            env.set_payload(req.SerializeAsString());
            // sequence_id: forwardToMaster bunu otomatik set eder
            // Sadece bu master bağlantısına gönder (broadcast değil)
            okidia::Envelope fwd = env;
            fwd.set_sequence_id(qHash(machineId));
            fwd.set_target_id(machineId.toStdString());
            conn->sendEnvelope(fwd);
            Logger::info(QObject::tr("ClientRegistry: Pending discovery master'a iletildi → ") + machineId);
        }
    }
    emit agentLocksChanged();
}

void ClientRegistry::sendAllClientsToMaster(Connection* conn) {
    if (!conn || !conn->isConnected()) return;

    for (const auto& client : m_clients) {
        okidia::ClientInfo info;
        info.set_machine_id(client.clientId.toStdString());
        info.set_hostname(client.hostname.toStdString());
        info.set_username(client.username.toStdString());
        info.set_display_name(client.displayName.toStdString());

        const AgentRecord rec = OkidiaDatabase::instance().agent(client.clientId);
        if (!rec.machineId.isEmpty()) {
            if (rec.agentType > 0) {
                info.set_agent_type(rec.agentType);
            }
            if (!rec.alias.isEmpty()) {
                info.set_alias(rec.alias.toStdString());
            }
            if (!rec.ip.isEmpty()) {
                info.set_ip_address(rec.ip.toStdString());
            }
            if (!rec.osInfo.isEmpty()) {
                info.set_os_tag(rec.osInfo.toStdString());
            }
            if (!rec.osPrettyName.isEmpty()) {
                info.set_os_version(rec.osPrettyName.toStdString());
            }
            if (!rec.osArch.isEmpty()) {
                info.set_os_arch(rec.osArch.toStdString());
            }
            if (!rec.agentVer.isEmpty()) {
                info.set_agent_version(rec.agentVer.toStdString());
            }
        }
        if (client.connection) {
            info.set_ip_address(client.connection->peerAddress().toStdString());
        }

        okidia::Envelope env;
        env.set_type(okidia::MessageType::CLIENT_INFO);
        env.set_sequence_id(qHash(client.clientId));
        env.set_target_id(client.clientId.toStdString());
        env.set_payload(info.SerializeAsString());
        conn->sendEnvelope(env);
    }
}

void ClientRegistry::removeMasterConnection(Connection* conn) {
    QString uId = m_authenticated.value(conn).userId;
    m_masterConnections.remove(conn);
    Logger::info(QObject::tr("ClientRegistry: Master bağlantısı kesildi, kalan master: ") + QString::number(m_masterConnections.size()));
    if (!uId.isEmpty()) {
        releaseAllLocksForMaster(uId);
        clearAuthentication(conn);
    }

    // Bu Master'a ait tüm ekran ve telemetri aboneliklerini düşür
    QStringList screenEmpty;
    QStringList metricsEmpty;
    unsubscribeAll(conn, &screenEmpty, &metricsEmpty);

    for (const QString& agentId : screenEmpty) {
        okidia::Envelope pEnv;
        pEnv.set_type(okidia::MessageType::SCREEN_STREAM_PAUSE);
        pEnv.set_target_id(agentId.toStdString());
        forwardToClient(agentId, pEnv);
        Logger::info(QObject::tr("ClientRegistry: Master koptu, %1 için izleyici kalmadı → SCREEN_STREAM_PAUSE").arg(agentId));
    }
    for (const QString& agentId : metricsEmpty) {
        okidia::Envelope mEnv;
        mEnv.set_type(okidia::MessageType::METRICS_STREAM_STOP);
        mEnv.set_target_id(agentId.toStdString());
        forwardToClient(agentId, mEnv);
        Logger::info(QObject::tr("ClientRegistry: Master koptu, %1 için metrik izleyici kalmadı → METRICS_STREAM_STOP").arg(agentId));
    }
}

void ClientRegistry::forwardToMaster(const QString& clientId,
                                      const okidia::Envelope& envelope) {
    if (m_masterConnections.isEmpty()) return;

    okidia::Envelope fwd = envelope;
    fwd.set_sequence_id(qHash(clientId));
    fwd.set_target_id(clientId.toStdString());
        
    for (Connection* conn : m_masterConnections) {
        conn->sendEnvelope(fwd);
    }
}

void ClientRegistry::forwardToClient(const QString& clientId,
                                      const okidia::Envelope& envelope) {
    if (!m_clients.contains(clientId)) return;
    auto& client = m_clients[clientId];
    if (client.connection) {
        client.connection->sendEnvelope(envelope);
    }
}

QList<RegisteredClient> ClientRegistry::allClients() const {
    return m_clients.values();
}

bool ClientRegistry::hasClient(const QString& clientId) const {
    return m_clients.contains(clientId);
}

QString ClientRegistry::clientIdByHash(uint hash) const {
    for (auto it = m_clients.constBegin();
         it != m_clients.constEnd(); ++it) {
        if (qHash(it.key()) == hash)
            return it.key();
    }
    return QString();
}
void ClientRegistry::addPendingDiscovery(const QString& machineId,
                                          Connection* conn) {
    m_pending[machineId] = conn;
    Logger::info(QObject::tr("ClientRegistry: Pending discovery → ") + machineId);
}

void ClientRegistry::storePendingRequest(const QString& machineId,
                                          const okidia::DiscoveryRequest& req) {
    m_pendingRequests[machineId] = req;
}

QByteArray ClientRegistry::pendingCsr(const QString& machineId) const {
    auto it = m_pendingRequests.find(machineId);
    if (it == m_pendingRequests.end()) return {};
    return QByteArray::fromStdString(it->csr_pem());
}

okidia::DiscoveryRequest ClientRegistry::pendingRequest(const QString& machineId) const {
    return m_pendingRequests.value(machineId);
}

QStringList ClientRegistry::pendingIds() const {
    return m_pending.keys();
}

bool ClientRegistry::isPendingConnection(Connection* conn) const {
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        if (it.value() == conn) return true;
    return false;
}

void ClientRegistry::forwardToPending(const QString& machineId,
                                       const okidia::Envelope& envelope) {
    auto it = m_pending.find(machineId);
    if (it == m_pending.end() || !it.value()) {
        Logger::warning(QObject::tr("ClientRegistry: Pending bulunamadı → ") + machineId);
        return;
    }
    it.value()->sendEnvelope(envelope);
    Logger::info(QObject::tr("ClientRegistry: Pending → gönderildi: ") + machineId);
}

void ClientRegistry::removePending(const QString& machineId) {
    m_pending.remove(machineId);
    m_pendingRequests.remove(machineId);
}

void ClientRegistry::removePendingByConnection(Connection* conn) {
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it.value() == conn) {
            const QString id = it.key();
            Logger::info(QObject::tr("ClientRegistry: Pending soket kapandı (CSR ve istek korunuyor) → ") + id);
            m_pending.erase(it);
            // NOT: m_pendingRequests.remove(id) buradan kaldırıldı.
            // Ağ kopması veya servis yeniden başlamasında ajanın CSR ve istek bilgisi
            // yönetici onaylayana/reddedene kadar bellekte korunmalıdır.
            return;
        }
    }
}

void ClientRegistry::disconnectPending(const QString& machineId) {
    auto it = m_pending.find(machineId);
    if (it == m_pending.end()) return;
    Connection* conn = it.value();
    m_pending.erase(it);
    if (conn) {
        Logger::info(QObject::tr("ClientRegistry: Pending bağlantı kapatılıyor → ") + machineId);
        conn->disconnect();  // agent disconnect sinyali alır, reconnect timer başlatır
    }
}

// ── İstemci iptali ────────────────────────────────────────────────────────

void ClientRegistry::revokeClient(const QString& machineId, bool uninstall)
{
    Logger::info(QObject::tr("ClientRegistry: Revoke → ") + machineId + (uninstall ? QObject::tr(" (UNINSTALL)") : ""));
    releaseAgentLock(machineId);

    // Bağlı istemciyi bul
    QString foundId;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        if (it.value().machineId == machineId || it.key() == machineId) {
            foundId = it.key();
            break;
        }
    }

    const bool isOnline = (!foundId.isEmpty() && m_clients.contains(foundId) && m_clients[foundId].connection);

    if (uninstall) {
        if (isOnline) {
            // İstemci çevrimiçi: Doğrudan CLIENT_UNINSTALL gönder, sistemden kaldırılacağı için kara listeye ALINMAZ.
            auto& client = m_clients[foundId];
            Connection* conn = client.connection;
            if (conn) {
                okidia::Envelope env;
                env.set_type(okidia::MessageType::CLIENT_UNINSTALL);
                okidia::ClientRevokeCommand cmd;
                cmd.set_machine_id(machineId.toStdString());
                cmd.set_uninstall(true);
                env.set_payload(cmd.SerializeAsString());
                conn->sendEnvelope(env);
                QTimer::singleShot(1000, conn, &Connection::disconnect);
            }
            m_clients.remove(foundId);
            emit clientUnregistered(foundId);
            m_revoked.remove(machineId);
            OkidiaDatabase::instance().removeFromBlacklist(machineId);
            OkidiaDatabase::instance().removeAgent(machineId);
        } else {
            // İstemci çevrimdışı: Tekrar bağlandığında kendini silmesi için PENDING_UNINSTALL ile pusuya al.
            m_revoked.insert(machineId);
            AgentRecord rec;
            rec.machineId    = machineId;
            rec.statusReason = "PENDING_UNINSTALL";
            rec.approvedAt   = OkidiaUtils::nowSecs();
            OkidiaDatabase::instance().addToBlacklist(rec);
            OkidiaDatabase::instance().removeAgent(machineId);
        }
    } else {
        // uninstall = false: Yalnızca yetki iptali (Revoke).
        // İstemci çevrimiçiyse CLIENT_REVOKE gönder (sertifikaları silip anonymous moda geçer).
        // Tekrar katılmak isterse yeni istek oluşturabilmesi için kara listeye ALINMAZ.
        if (isOnline) {
            auto& client = m_clients[foundId];
            Connection* conn = client.connection;
            if (conn) {
                okidia::Envelope env;
                env.set_type(okidia::MessageType::CLIENT_REVOKE);
                okidia::ClientRevokeCommand cmd;
                cmd.set_machine_id(machineId.toStdString());
                cmd.set_uninstall(false);
                env.set_payload(cmd.SerializeAsString());
                conn->sendEnvelope(env);
                QTimer::singleShot(1000, conn, &Connection::disconnect);
            }
            m_clients.remove(foundId);
            emit clientUnregistered(foundId);
        }
        m_revoked.remove(machineId);
        OkidiaDatabase::instance().removeFromBlacklist(machineId);
        OkidiaDatabase::instance().removeAgent(machineId);
    }
    
    // Tüm master'lara yayın yap (thumbnail ve tree'den silinmesi için)
    emit clientRevokedBroadcast(machineId);
}

void ClientRegistry::unrevokeClient(const QString& machineId)
{
    Logger::info(QObject::tr("ClientRegistry: Unrevoke → ") + machineId);
    m_revoked.remove(machineId);

    // DB'den kalıcı olarak çıkar
    OkidiaDatabase::instance().removeFromBlacklist(machineId);
}

void ClientRegistry::revokeAll(bool uninstall)
{
    Logger::info(QObject::tr("ClientRegistry: Tüm istemciler revoke ediliyor (")
                 + QString::number(m_clients.size()) + QObject::tr(" istemci)"));

    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        const QString mid = it.value().machineId.isEmpty()
                            ? it.key() : it.value().machineId;
        releaseAgentLock(mid);
        if (it.value().connection) {
            okidia::Envelope env;
            env.set_type(uninstall ? okidia::MessageType::CLIENT_UNINSTALL : okidia::MessageType::CLIENT_REVOKE);
            okidia::ClientRevokeCommand cmd;
            cmd.set_all(true);
            cmd.set_uninstall(uninstall);
            env.set_payload(cmd.SerializeAsString());
            it.value().connection->sendEnvelope(env);
        }
        OkidiaDatabase::instance().removeAgent(mid);
        m_revoked.remove(mid);
        OkidiaDatabase::instance().removeFromBlacklist(mid);
    }
    m_clients.clear();
    emit clientUnregistered("__all__");
}

bool ClientRegistry::isRevoked(const QString& machineId) const
{
    if (machineId.isEmpty()) return false;
    // Önce hafıza cache, sonra DB
    if (m_revoked.contains(machineId)) return true;
    return OkidiaDatabase::instance().isBlacklisted(machineId);
}

void ClientRegistry::loadFromDb()
{
    // Kara listeyi DB'den hafıza cache'ine yükle
    const auto entries = OkidiaDatabase::instance().blacklist();
    for (const auto& rec : entries)
        m_revoked.insert(rec.machineId);

    // Onaylı agent'ları da hafızaya al (bağlantı kontrolü için)
    const auto agents = OkidiaDatabase::instance().allAgents();
    Logger::info(QObject::tr("ClientRegistry: DB'den yüklendi → ")
                 + QString::number(m_revoked.size()) + QObject::tr(" kara liste, ")
                 + QString::number(agents.size()) + QObject::tr(" onaylı agent"));
}

void ClientRegistry::setAuthenticatedUser(Connection* conn, const QString& userId, const QString& username, const QString& displayName, const QString& role, const QString& machineId, const QString& hostname) {
    if (!conn) return;
    AuthenticatedMaster auth;
    auth.userId = userId;
    auth.username = username;
    auth.displayName = displayName;
    auth.role = role;
    auth.machineId = machineId;
    auth.hostname = hostname;
    m_authenticated[conn] = auth;
    // Bağlantı koptuğunda temizle
    connect(conn, &Connection::disconnected, this, [this, conn]() {
        AuthenticatedMaster authInfo = m_authenticated.value(conn);
        clearAuthentication(conn);
        if (!authInfo.userId.isEmpty()) {
            releaseAllLocksForMaster(authInfo.userId);
            // Kalan tüm master'lara üyenin ayrıldığını bildir
            if (!authInfo.username.isEmpty() && !m_masterConnections.isEmpty()) {
                okidia::ChatMemberEvent leaveEvt;
                leaveEvt.set_room_id("room_admin");
                leaveEvt.set_client_id(("master:" + authInfo.username).toStdString());
                leaveEvt.set_hostname(authInfo.displayName.isEmpty() ? authInfo.username.toStdString() : authInfo.displayName.toStdString());
                leaveEvt.set_username(authInfo.role.toStdString());
                okidia::Envelope env;
                env.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
                env.set_payload(leaveEvt.SerializeAsString());
                for (Connection* mConn : m_masterConnections) {
                    if (mConn != conn) mConn->sendEnvelope(env);
                }
            }
        }
    });
}

QString ClientRegistry::authenticatedUserId(Connection* conn) const {
    return m_authenticated.value(conn).userId;
}

QString ClientRegistry::authenticatedUsername(Connection* conn) const {
    return m_authenticated.value(conn).username;
}

QString ClientRegistry::authenticatedDisplayName(Connection* conn) const {
    return m_authenticated.value(conn).displayName;
}

QString ClientRegistry::authenticatedRole(Connection* conn) const {
    return m_authenticated.value(conn).role;
}

QString ClientRegistry::authenticatedMachineId(Connection* conn) const {
    return m_authenticated.value(conn).machineId;
}

QString ClientRegistry::authenticatedHostname(Connection* conn) const {
    return m_authenticated.value(conn).hostname;
}

void ClientRegistry::clearAuthentication(Connection* conn) {
    m_authenticated.remove(conn);
}

bool ClientRegistry::acquireAgentLock(const QString& clientId, const QString& masterUserId, const QString& masterDisplayName, const QString& lockType) {
    if (isAgentLockedByOther(clientId, masterUserId)) {
        return false;
    }
    AgentLock l;
    l.masterUserId = masterUserId;
    l.masterDisplayName = masterDisplayName;
    l.lockType = lockType;
    l.lockedAt = OkidiaUtils::nowSecs();
    m_agentLocks[clientId] = l;
    Logger::info(QObject::tr("ClientRegistry: Kilit alındı -> Client: ") + clientId + QObject::tr(" Master: ") + masterDisplayName + QObject::tr(" Tip: ") + lockType);
    emit agentLocksChanged();
    return true;
}

void ClientRegistry::releaseAgentLock(const QString& clientId) {
    if (m_agentLocks.contains(clientId)) {
        Logger::info(QObject::tr("ClientRegistry: Kilit bırakıldı -> Client: ") + clientId + QObject::tr(" (Eski kilit sahibi: ") + m_agentLocks[clientId].masterDisplayName + ")");
        m_agentLocks.remove(clientId);
        emit agentLocksChanged();
    }
}

void ClientRegistry::releaseAllLocksForMaster(const QString& masterUserId) {
    bool changed = false;
    for (auto it = m_agentLocks.begin(); it != m_agentLocks.end(); ) {
        if (it->masterUserId == masterUserId) {
            Logger::info(QObject::tr("ClientRegistry: Master bağlantısı kesildiği için otomatik kilit bırakıldı -> Client: ") + it.key() + QObject::tr(" Master: ") + it->masterDisplayName);
            it = m_agentLocks.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        emit agentLocksChanged();
    }
}

bool ClientRegistry::isAgentLockedByOther(const QString& clientId, const QString& masterUserId) const {
    if (!m_agentLocks.contains(clientId)) {
        return false;
    }
    const QString& lockedBy = m_agentLocks[clientId].masterUserId;
    if (lockedBy.startsWith("system")) {
        return false; // Sistem/Zil kilitleri tüm yetkili Master'lar tarafından doğrudan açılabilir ve yönetilebilir
    }
    return lockedBy != masterUserId;
}

// ── Pub/Sub Abonelik Yönetimi (Ekran ve Telemetri Çoklama) ───────────

bool ClientRegistry::subscribeScreen(Connection* masterConn, const QString& agentId, bool* isFirst) {
    if (!masterConn || agentId.isEmpty()) return false;
    auto& subs = m_screenSubscribers[agentId];
    bool wasEmpty = subs.isEmpty();
    subs.insert(masterConn);
    if (isFirst) *isFirst = wasEmpty;
    return true;
}

bool ClientRegistry::unsubscribeScreen(Connection* masterConn, const QString& agentId, bool* isLast) {
    if (!masterConn || agentId.isEmpty() || !m_screenSubscribers.contains(agentId)) return false;
    auto& subs = m_screenSubscribers[agentId];
    subs.remove(masterConn);
    bool nowEmpty = subs.isEmpty();
    if (nowEmpty) {
        m_screenSubscribers.remove(agentId);
    }
    if (isLast) *isLast = nowEmpty;
    return true;
}

bool ClientRegistry::subscribeMetrics(Connection* masterConn, const QString& agentId, bool* isFirst) {
    if (!masterConn || agentId.isEmpty()) return false;
    auto& subs = m_metricsSubscribers[agentId];
    bool wasEmpty = subs.isEmpty();
    subs.insert(masterConn);
    if (isFirst) *isFirst = wasEmpty;
    return true;
}

bool ClientRegistry::unsubscribeMetrics(Connection* masterConn, const QString& agentId, bool* isLast) {
    if (!masterConn || agentId.isEmpty() || !m_metricsSubscribers.contains(agentId)) return false;
    auto& subs = m_metricsSubscribers[agentId];
    subs.remove(masterConn);
    bool nowEmpty = subs.isEmpty();
    if (nowEmpty) {
        m_metricsSubscribers.remove(agentId);
    }
    if (isLast) *isLast = nowEmpty;
    return true;
}

void ClientRegistry::unsubscribeAll(Connection* masterConn, QStringList* screenEmptyAgents, QStringList* metricsEmptyAgents) {
    if (!masterConn) return;

    for (auto it = m_screenSubscribers.begin(); it != m_screenSubscribers.end(); ) {
        it.value().remove(masterConn);
        if (it.value().isEmpty()) {
            if (screenEmptyAgents) *screenEmptyAgents << it.key();
            it = m_screenSubscribers.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_metricsSubscribers.begin(); it != m_metricsSubscribers.end(); ) {
        it.value().remove(masterConn);
        if (it.value().isEmpty()) {
            if (metricsEmptyAgents) *metricsEmptyAgents << it.key();
            it = m_metricsSubscribers.erase(it);
        } else {
            ++it;
        }
    }
}

QSet<Connection*> ClientRegistry::screenSubscribers(const QString& agentId) const {
    return m_screenSubscribers.value(agentId);
}

QSet<Connection*> ClientRegistry::metricsSubscribers(const QString& agentId) const {
    return m_metricsSubscribers.value(agentId);
}

bool ClientRegistry::hasScreenSubscribers(const QString& agentId) const {
    if (!m_screenSubscribers.contains(agentId)) return false;
    for (Connection* mConn : m_screenSubscribers[agentId]) {
        if (mConn && mConn->isConnected() && m_masterConnections.contains(mConn)) {
            return true;
        }
    }
    return false;
}

bool ClientRegistry::hasMetricsSubscribers(const QString& agentId) const {
    if (!m_metricsSubscribers.contains(agentId)) return false;
    for (Connection* mConn : m_metricsSubscribers[agentId]) {
        if (mConn && mConn->isConnected() && m_masterConnections.contains(mConn)) {
            return true;
        }
    }
    return false;
}


