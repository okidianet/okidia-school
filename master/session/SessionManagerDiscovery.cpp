// master/session/SessionManagerDiscovery.cpp
// Discovery akışı, sertifika onay/red/iptal ve revoke yönetimi.
// approveDiscovery, ignoreDiscovery, rejectDiscovery, revokeClient,
// cleanupRejectedClient, loadRevokedList, saveRevokedList,
// startRecording, stopRecording metodlarını içerir.
#include "SessionManager.h"
#include "../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QDateTime>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <signal.h>


void SessionManager::approveDiscovery(const QString& targetId,
                                       const QByteArray& /*csrPem*/,
                                       uint32_t agentType) {
    // Yeni akış: broker CSR'ı imzalar.
    // Master sadece "onayla" komutunu broker'a gönderir (sequence_id yok — agent'a yönlendirme değil).
    if (!m_conn || !m_conn->isConnected()) {
        Logger::warning(QObject::tr("SessionManager: approveDiscovery — broker bağlı değil"));
        return;
    }
    okidia::DiscoveryApprove appr;
    appr.set_target_id(targetId.toStdString());
    appr.set_agent_type(agentType);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DISCOVERY_APPROVE);
    env.set_payload(appr.SerializeAsString());
    // sequence_id=0 set etmiyoruz — broker bunu broadcast olarak değil,
    // DISCOVERY_APPROVE handler'ında target_id'ye göre işler.
    m_conn->sendEnvelope(env);

    // Pending data temizle (DB kaydı broker tarafında yapılıyor)
    m_pendingDiscoveryData.remove(targetId);
    
    // Eğer daha önce sistemden çıkarılmışsa, yeniden onaylandığında bellekten temizle
    m_ignoredMachineIds.remove(targetId);
    m_revokedMachineIds.remove(targetId);

    Logger::info(QObject::tr("SessionManager: Discovery onayı broker'a iletildi → ") + targetId + 
                 QObject::tr(" (Tip: ") + QString::number(agentType) + ")");

    // Broker tarafında agent DB'ye ekleneceği için kısa bir süre sonra DB senkronizasyonu iste
    QTimer::singleShot(1000, this, &SessionManager::requestDbBackup);
}

void SessionManager::ignoreDiscovery(const QString& targetId,
                                      const QString& machineId) {
    m_ignoredMachineIds.insert(machineId);
    // DB'ye kalıcı kayıt
    AgentRecord rec;
    rec.machineId = machineId;
    if (m_pendingDiscoveryData.contains(targetId)) {
        const auto& req = m_pendingDiscoveryData[targetId];
        rec.hostname     = QString::fromStdString(req.hostname());
        rec.ip           = QString::fromStdString(req.ip_address());
        rec.mac          = QString::fromStdString(req.mac_address());
        rec.osInfo       = QString::fromStdString(req.os_info());
        rec.osArch       = QString::fromStdString(req.os_arch());
        rec.agentVer     = QString::fromStdString(req.agent_version());
        m_pendingDiscoveryData.remove(targetId);
    }
    rec.statusReason = "DISCOVERY_IGNORE";
    rec.approvedAt   = OkidiaUtils::nowSecs();
    OkidiaDatabase::instance().addToBlacklist(rec);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DISCOVERY_IGNORE);
    env.set_target_id(targetId.toStdString());
    broadcastCommand(env);

    // Yanlışlıkla oluşmuş ClientState / thumbnail varsa temizle
    cleanupRejectedClient(machineId);

    Logger::info(QObject::tr("SessionManager: Discovery yoksayıldı (kalıcı) → ") + targetId +
                 " (" + machineId + ")");
}

// Onaylanmamış istemcinin broker bağlantısı kesilmeden önce session timer veya
// heartbeat timer CLIENT_INFO göndermiş olabilir. Bu metot yanlışlıkla oluşan
// ClientState ve hash kaydını temizler; MainWindow'a thumbnail kaldırması sinyali gönderilir.
void SessionManager::cleanupRejectedClient(const QString& machineId) {
    if (machineId.isEmpty()) return;
    if (!m_clientStates.contains(machineId)) return;

    m_clientStates.remove(machineId);
    m_hashToClientId.remove(qHash(machineId));
    unregisterThumbnail(machineId);  // SessionManager map'ten çıkar
    // clientDisconnected yerine clientRemoved: bu istemci hiç sisteme kabul edilmedi.
    // MainWindow thumbnail widget'ını da siler.
    emit clientRemoved(machineId);
    Logger::info(QObject::tr("SessionManager: Reddedilen/yoksayılan istemci temizlendi → ") + machineId);
}

void SessionManager::rejectDiscovery(const QString& targetId) {
    // Reddet: bağlantıyı kapat, kara listeye ALMA.
    // Agent sertifikasız kalır; servis yeniden başlatılınca tekrar istek yollar.

    // machineId'yi pendingDiscoveryData'dan al (varsa)
    QString machineId;
    if (m_pendingDiscoveryData.contains(targetId))
        machineId = QString::fromStdString(
            m_pendingDiscoveryData[targetId].machine_id());

    m_pendingDiscoveryData.remove(targetId);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DISCOVERY_REJECT);
    env.set_target_id(targetId.toStdString());
    broadcastCommand(env);

    // Yanlışlıkla oluşmuş ClientState / thumbnail varsa temizle
    if (!machineId.isEmpty())
        cleanupRejectedClient(machineId);

    Logger::info(QObject::tr("SessionManager: Discovery reddedildi (geçici) → ") + targetId);
}

void SessionManager::removeFromIgnoreList(const QString& machineId) {
    m_ignoredMachineIds.remove(machineId);
    m_revokedMachineIds.remove(machineId);
    OkidiaDatabase::instance().removeFromBlacklist(machineId);
    
    // Broker'a bildir: bu makineyi kara listeden çıkar
    okidia::ClientUnrevokeCommand cmd;
    cmd.set_machine_id(machineId.toStdString());
    
    okidia::Envelope env;
    env.set_type(okidia::MessageType::CLIENT_UNREVOKE);
    env.set_payload(cmd.SerializeAsString());
    broadcastCommand(env);
    
    Logger::info(QObject::tr("SessionManager: Kara listeden çıkarıldı → ") + machineId);
}


// ── İstemci iptali ────────────────────────────────────────────────────────

void SessionManager::revokeClient(const QString& clientId, bool uninstall)
{
    if (!m_clientStates.contains(clientId)) return;

    const QString machineId = m_clientStates.value(clientId).machineId.isEmpty()
                              ? clientId
                              : m_clientStates.value(clientId).machineId;

    Logger::info(QObject::tr("SessionManager: Revoke → ") + clientId
                 + QObject::tr(" (machine: ") + machineId + ")");

    m_revokedMachineIds.remove(machineId);
    m_ignoredMachineIds.remove(machineId);
    OkidiaDatabase::instance().removeFromBlacklist(machineId);
    // DB'den agent kaydını sil
    OkidiaDatabase::instance().removeAgent(machineId);

    // Broker'a gönder: CLIENT_REVOKE + machine_id
    // Broker bu machine_id'nin bağlantısını keser fakat kara listeye eklemez.
    okidia::ClientRevokeCommand cmd;
    cmd.set_machine_id(machineId.toStdString());
    if (uninstall) cmd.set_uninstall(true);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CLIENT_REVOKE);
    env.set_payload(cmd.SerializeAsString());
    broadcastCommand(env);

    // Master tarafında state'i temizle
    m_clientStates.remove(clientId);
    m_hashToClientId.remove(qHash(clientId));
    if (m_thumbnails.contains(clientId))
        unregisterThumbnail(clientId);

    emit clientDisconnected(clientId);
    Logger::info(QObject::tr("SessionManager: İstemci sistemden çıkarıldı → ") + clientId);

    // DB değişikliği için senkronizasyon iste (silinmenin DB'den tam yansıması için)
    QTimer::singleShot(1000, this, &SessionManager::requestDbBackup);
}

void SessionManager::resetAllClients(bool uninstall)
{
    Logger::info(QObject::tr("SessionManager: Tüm istemciler sıfırlanıyor (")
                 + QString::number(m_clientStates.size()) + QObject::tr(" istemci)"));

    okidia::ClientRevokeCommand cmd;
    cmd.set_all(true);
    if (uninstall) cmd.set_uninstall(true);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CLIENT_REVOKE);
    env.set_payload(cmd.SerializeAsString());
    broadcastCommand(env);

    // Tüm state'leri temizle
    const QStringList ids = m_clientStates.keys();
    m_clientStates.clear();
    m_hashToClientId.clear();
    m_revokedMachineIds.clear();
    m_ignoredMachineIds.clear();
    saveRevokedList();

    for (const QString& id : ids) {
        if (m_thumbnails.contains(id))
            unregisterThumbnail(id);
        emit clientDisconnected(id);
    }
    Logger::info(QObject::tr("SessionManager: Tüm istemciler sıfırlandı"));
}

void SessionManager::clearRevokedList()
{
    m_revokedMachineIds.clear();
    m_ignoredMachineIds.clear();
    saveRevokedList();
    Logger::info(QObject::tr("SessionManager: Revoked liste temizlendi"));
}

void SessionManager::loadRevokedList()
{
    QSettings cfg("/etc/okidia/revoked_clients.conf",
                  QSettings::IniFormat);
    const QStringList ids = cfg.value("revoked/machine_ids").toStringList();
    for (const QString& id : ids) {
        if (!id.isEmpty()) {
            m_revokedMachineIds.insert(id);
            m_ignoredMachineIds.insert(id);
        }
    }
    if (!ids.isEmpty())
        Logger::info(QObject::tr("SessionManager: Revoked liste yüklendi → ")
                     + QString::number(ids.size()) + tr(" kayıt"));
}

void SessionManager::saveRevokedList() const
{
    QSettings cfg("/etc/okidia/revoked_clients.conf",
                  QSettings::IniFormat);
    cfg.setValue("revoked/machine_ids",
                 QStringList(m_revokedMachineIds.values()));
    cfg.sync();
}


// ===========================================================================
// Faz 4: Yardımcı Metodlar (Uzak Kontrol)
// ===========================================================================

bool SessionManager::sendRemoteControlRequest(const QString& clientId, bool isControl) {
    okidia::RemoteControlRequest req;
    req.set_is_control(isControl);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::REMOTE_CONTROL_REQUEST);
    env.set_payload(req.SerializeAsString());
    return sendCommand(clientId, env);
}


// ============================================================================
// Çoklu monitör — ekran kaynağı seçimi
// ============================================================================

bool SessionManager::sendScreenSelect(const QString& clientId,
                                       int monitorIndex) {
    okidia::ScreenSelectCommand cmd;
    cmd.set_source(okidia::ScreenSelectCommand::MONITOR);
    cmd.set_monitor_index(static_cast<uint32_t>(monitorIndex));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_SELECT);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendScreenSelectWindow(const QString& clientId,
                                             quint64 windowId) {
    okidia::ScreenSelectCommand cmd;
    cmd.set_source(okidia::ScreenSelectCommand::WINDOW);
    cmd.set_window_id(windowId);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_SELECT);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendScreenSelectAll(const QString& clientId) {
    okidia::ScreenSelectCommand cmd;
    cmd.set_source(okidia::ScreenSelectCommand::ALL);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_SELECT);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendScreenInfoRequest(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::SCREEN_INFO_REQUEST));
}

void SessionManager::handleScreenInfo(const okidia::Envelope& env,
                                       const QString& clientId) {
    okidia::ScreenInfoResponse info;
    if (!info.ParseFromString(env.payload())) return;

    Logger::info(QObject::tr("SessionManager: ScreenInfo ← ") + clientId +
                 " → " + QString::number(info.monitors_size()) +
                 tr(" monitör, ") + QString::number(info.windows_size()) +
                 QObject::tr(" pencere"));

    emit screenInfoReceived(clientId, info);
}

void SessionManager::handleCursorShapeChange(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::CursorShapeChange csc;
    if (!csc.ParseFromString(env.payload())) return;

    emit cursorShapeChanged(clientId, static_cast<int>(csc.cursor_id()),
                            static_cast<int>(csc.x()), static_cast<int>(csc.y()));
}

// ============================================================================
// C2C Sohbet — SessionManager
// ============================================================================

// ---------------------------------------------------------------------------
// Grup bilgisi yönetimi
// ---------------------------------------------------------------------------
void SessionManager::setClientGroup(const QString& clientId,
                                     const QString& groupId) {
    m_clientGroups[clientId] = groupId;
}

QString SessionManager::clientGroup(const QString& clientId) const {
    QString gid = m_clientGroups.value(clientId);
    if (gid.isEmpty() || gid == "__root__") {
        if (OkidiaDatabase::instance().isOpen()) {
            QString dbGid = OkidiaDatabase::instance().agent(clientId).classId;
            if (!dbGid.isEmpty()) {
                const_cast<SessionManager*>(this)->m_clientGroups[clientId] = dbGid;
                return dbGid;
            }
        }
        return "__root__";
    }
    return gid;
}

QStringList SessionManager::clientsInGroup(const QString& groupId) const {
    QStringList list;
    for (auto it = m_clientGroups.constBegin();
         it != m_clientGroups.constEnd(); ++it) {
        if (it.value() == groupId && m_clientStates.value(it.key()).connected)
            list << it.key();
    }
    return list;
}

// ---------------------------------------------------------------------------
// CHAT_LIST_REQUEST: Aynı gruptaki online clientları döndür
// ---------------------------------------------------------------------------
// ── Özel sohbet handler'ları ─────────────────────────────────────────────────

void SessionManager::handlePrivateChatRequest(const okidia::Envelope& env,
                                               const QString& fromClientId) {
    okidia::PrivateChatRequest msg;
    if (!msg.ParseFromString(env.payload())) return;
    emit privateChatRequested(
        fromClientId,
        QString::fromStdString(msg.from_display()),
        QString::fromStdString(msg.from_host()),
        QString::fromStdString(msg.from_user()),
        QString::fromStdString(msg.request_id()));
}

void SessionManager::handlePrivateChatAccept(const okidia::Envelope& env,
                                              const QString& fromClientId) {
    okidia::PrivateChatAccept msg;
    if (!msg.ParseFromString(env.payload())) return;
    emit privateChatAccepted(
        QString::fromStdString(msg.request_id()),
        QString::fromStdString(msg.session_id()));
}

void SessionManager::handlePrivateChatReject(const okidia::Envelope& env,
                                              const QString& fromClientId) {
    okidia::PrivateChatReject msg;
    if (!msg.ParseFromString(env.payload())) return;
    emit privateChatRejected(
        QString::fromStdString(msg.request_id()),
        static_cast<int>(msg.cooldown_seconds()));
}

void SessionManager::handlePrivateChatOpen(const okidia::Envelope& env,
                                            const QString& fromClientId) {
    okidia::PrivateChatOpen msg;
    if (!msg.ParseFromString(env.payload())) return;
    emit privateChatOpenedRemotely(
        QString::fromStdString(msg.session_id()),
        fromClientId,
        QString::fromStdString(msg.from_display()),
        QString::fromStdString(msg.from_host()));
}

void SessionManager::handlePrivateChatMessage(const okidia::Envelope& env,
                                               const QString& fromClientId) {
    okidia::PrivateChatMessage msg;
    if (!msg.ParseFromString(env.payload())) return;
    emit privateChatMessageReceived(
        QString::fromStdString(msg.session_id()),
        QString::fromStdString(msg.content()),
        static_cast<qint64>(msg.timestamp()));
}

void SessionManager::handlePrivateChatClose(const okidia::Envelope& env,
                                             const QString& fromClientId) {
    okidia::PrivateChatClose msg;
    if (!msg.ParseFromString(env.payload())) return;
    emit privateChatClosed(QString::fromStdString(msg.session_id()));
}

void SessionManager::handleChatWindowState(const okidia::Envelope& env,
                                            const QString& fromClientId) {
    okidia::ChatWindowState msg;
    if (!msg.ParseFromString(env.payload())) return;
    m_agentWindowOpenMap[fromClientId] = msg.open();
    emit agentChatWindowChanged(fromClientId, msg.open());
}

bool SessionManager::isMasterOnline(const QString& masterId) const {
    return m_knownOnlineMasters.value(masterId, false);
}

bool SessionManager::isAgentWindowOpen(const QString& clientId) const {
    return m_agentWindowOpenMap.value(clientId, false);
}

// ── Yardımcılar ───────────────────────────────────────────────────────────────


