// master/session/SessionManagerCommands.cpp
// Agent'lara gönderilen tüm komut metodları.
// sendScreen*, sendInput*, sendPower*, sendAudio*, sendWeb*, sendExam*,
// broadcastScreen*, broadcastInput*, broadcastAudio*, broadcastWeb*,
// sendAnons, sendFile*, sendChat*, sendMouseEvent, sendKeyEvent vb.
#include "SessionManager.h"
#include "../transfer/FileSender.h"
#include "../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include "../common/crypto/AesGcm.h"
#include "../../common/db/OkidiaDatabase.h"


// ============================================================================
// Faz 4 — Kolaylık metodları
// ============================================================================

// Yardımcı: boş envelope oluştur

// ---------------------------------------------------------------------------
// Ekran
// ---------------------------------------------------------------------------
bool SessionManager::sendScreenLock(const QString& clientId,
                                     const QString& message,
                                     uint32_t lockPolicy) {
    okidia::ScreenLockCommand cmd;
    cmd.set_message(message.toStdString());

    QSettings s("okidia", "master");
    int defaultPolicy = s.value("smartboard/lockPolicy", 1).toInt(); // 0 = Perde (1), 1 = QR (2)
    uint32_t effectivePolicy = (lockPolicy > 0) ? lockPolicy : (defaultPolicy == 0 ? 1 : 2);
    cmd.set_lock_policy(effectivePolicy);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_LOCK);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendScreenUnlock(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::SCREEN_UNLOCK));
}

bool SessionManager::sendScreenCapture(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::SCREEN_CAPTURE));
}

bool SessionManager::sendAppCloseByWid(const QString& clientId,
                                          const QString& wid) {
    okidia::AppCommand cmd;
    cmd.set_name(wid.toStdString());  // wid string olarak

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_CLOSE);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendAppListRequest(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::APP_LIST_REQUEST));
}

bool SessionManager::sendRefreshRequest(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::REFRESH_REQUEST));
}

bool SessionManager::broadcastScreenLock(const QString& message,
                                        uint32_t lockPolicy) {
    okidia::ScreenLockCommand cmd;
    cmd.set_message(message.toStdString());

    QSettings s("okidia", "master");
    int defaultPolicy = s.value("smartboard/lockPolicy", 1).toInt(); // 0 = Perde (1), 1 = QR (2)
    uint32_t effectivePolicy = (lockPolicy > 0) ? lockPolicy : (defaultPolicy == 0 ? 1 : 2);
    cmd.set_lock_policy(effectivePolicy);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_LOCK);
    env.set_payload(cmd.SerializeAsString());
    return broadcastCommand(env);
}

bool SessionManager::broadcastScreenUnlock() {
    return broadcastCommand(makeEnv(okidia::MessageType::SCREEN_UNLOCK));
}

bool SessionManager::sendScreenConfig(const QString& clientId, int intervalMs, int quality, int maxWidth) {
    okidia::ScreenConfig cmd;
    cmd.set_interval_ms(static_cast<uint32_t>(intervalMs));
    cmd.set_jpeg_quality(static_cast<uint32_t>(quality));
    cmd.set_max_width(static_cast<uint32_t>(maxWidth));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_FPS_SET);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::broadcastScreenConfig(int intervalMs, int quality) {
    okidia::ScreenConfig cmd;
    cmd.set_interval_ms(static_cast<uint32_t>(intervalMs));
    cmd.set_jpeg_quality(static_cast<uint32_t>(quality));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_FPS_SET);
    env.set_payload(cmd.SerializeAsString());
    return broadcastCommand(env);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
bool SessionManager::sendInputLock(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::INPUT_LOCK));
}

bool SessionManager::sendInputUnlock(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::INPUT_UNLOCK));
}

bool SessionManager::sendTakeControl(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::INPUT_TAKE_CONTROL));
}

bool SessionManager::sendRemoteControlStop(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::REMOTE_CONTROL_STOP));
}

bool SessionManager::broadcastInputLock() {
    return broadcastCommand(makeEnv(okidia::MessageType::INPUT_LOCK));
}

bool SessionManager::broadcastInputUnlock() {
    return broadcastCommand(makeEnv(okidia::MessageType::INPUT_UNLOCK));
}

// ---------------------------------------------------------------------------
// Fare / Klavye enjeksiyonu
// ---------------------------------------------------------------------------
bool SessionManager::sendMouseEvent(const QString& clientId,
                                     const okidia::MouseEvent& evt) {
    okidia::Envelope env;
    env.set_type(okidia::MessageType::MOUSE_EVENT);
    env.set_payload(evt.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendKeyEvent(const QString& clientId,
                                   const okidia::KeyEvent& evt) {
    okidia::Envelope env;
    env.set_type(okidia::MessageType::KEY_EVENT);
    env.set_payload(evt.SerializeAsString());
    return sendCommand(clientId, env);
}

// ---------------------------------------------------------------------------
// Güç
// ---------------------------------------------------------------------------
static okidia::Envelope makePowerEnv(okidia::MessageType type,
                                      int delaySecs,
                                      const QString& reason) {
    okidia::PowerCommand cmd;
    cmd.set_delay_s(static_cast<uint32_t>(delaySecs));
    cmd.set_reason(reason.toStdString());

    okidia::Envelope env;
    env.set_type(type);
    env.set_payload(cmd.SerializeAsString());
    return env;
}

bool SessionManager::sendWakeOnLan(const QString& clientId) {
    if (m_clientStates.contains(clientId) && m_clientStates[clientId].agentType == 2) {
        Logger::warning(QObject::tr("SessionManager: İdari PC için Wake-on-LAN engellendi → ") + clientId);
        return false;
    }

    QString mac;
    QString ip;
    if (OkidiaDatabase::instance().isOpen()) {
        AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
        if (rec.agentType == 2) {
            Logger::warning(QObject::tr("SessionManager: İdari PC (DB) için Wake-on-LAN engellendi → ") + clientId);
            return false;
        }
        mac = rec.mac;
        ip = rec.ip;
    }
    if (mac.isEmpty() && m_clientStates.contains(clientId)) {
        mac = m_clientStates[clientId].mac;
        if (ip.isEmpty()) ip = m_clientStates[clientId].ip;
    }

    // 1. Master yerel ağından doğrudan UDP Magic Packet yayını
    if (!mac.isEmpty()) {
        OkidiaUtils::sendWakeOnLan(mac, ip);
    }

    // 2. Broker sunucusu üzerinden de yerel ağda Magic Packet yayını
    if (m_conn && m_conn->isConnected()) {
        okidia::Envelope env;
        env.set_type(okidia::MessageType::POWER_WAKE_ON_LAN);
        env.set_target_id(clientId.toStdString());
        env.set_payload(mac.toStdString());
        m_conn->sendEnvelope(env);
    }
    return true;
}

bool SessionManager::broadcastWakeOnLan() {
    if (OkidiaDatabase::instance().isOpen()) {
        const auto allAgents = OkidiaDatabase::instance().allAgents();
        for (const auto& ag : allAgents) {
            if (ag.agentType != 2 && !ag.mac.isEmpty()) {
                OkidiaUtils::sendWakeOnLan(ag.mac, ag.ip);
            }
        }
    }
    for (auto it = m_clientStates.begin(); it != m_clientStates.end(); ++it) {
        if (it.value().agentType != 2 && !it.value().mac.isEmpty()) {
            OkidiaUtils::sendWakeOnLan(it.value().mac, it.value().ip);
        }
    }
    if (m_conn && m_conn->isConnected()) {
        okidia::Envelope env;
        env.set_type(okidia::MessageType::POWER_WAKE_ON_LAN);
        m_conn->sendEnvelope(env);
    }
    return true;
}

bool SessionManager::sendShutdown(const QString& clientId,
                                   int delaySecs, const QString& reason) {
    return sendCommand(clientId,
        makePowerEnv(okidia::MessageType::POWER_SHUTDOWN, delaySecs, reason));
}

bool SessionManager::sendRestart(const QString& clientId,
                                  int delaySecs, const QString& reason) {
    return sendCommand(clientId,
        makePowerEnv(okidia::MessageType::POWER_RESTART, delaySecs, reason));
}

bool SessionManager::sendLogout(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::POWER_LOGOUT));
}

bool SessionManager::sendHibernate(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::POWER_HIBERNATE));
}

bool SessionManager::broadcastShutdown(int delaySecs) {
    return broadcastCommand(
        makePowerEnv(okidia::MessageType::POWER_SHUTDOWN, delaySecs, ""));
}

bool SessionManager::broadcastRestart(int delaySecs) {
    return broadcastCommand(
        makePowerEnv(okidia::MessageType::POWER_RESTART, delaySecs, ""));
}

// ---------------------------------------------------------------------------
// Ses
// ---------------------------------------------------------------------------
bool SessionManager::sendAudioMute(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::AUDIO_MUTE));
}

bool SessionManager::sendAudioUnmute(const QString& clientId) {
    return sendCommand(clientId,
                       makeEnv(okidia::MessageType::AUDIO_UNMUTE));
}

bool SessionManager::sendAudioSetVolume(const QString& clientId,
                                         int volume) {
    okidia::AudioVolumeCommand cmd;
    cmd.set_volume(static_cast<uint32_t>(qBound(0, volume, 100)));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_SET_VOLUME);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

// ---------------------------------------------------------------------------
// Mesaj popup
// ---------------------------------------------------------------------------
static okidia::Envelope makePopupEnv(const QString& message,
                                      const QString& sender) {
    okidia::ChatMessage msg;
    msg.set_content(message.toStdString());
    msg.set_from(sender.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::MSG_POPUP);
    env.set_payload(msg.SerializeAsString());
    return env;
}

bool SessionManager::sendPopup(const QString& clientId,
                                const QString& message,
                                const QString& senderName) {
    return sendCommand(clientId, makePopupEnv(message, senderName));
}

bool SessionManager::broadcastPopup(const QString& message,
                                     const QString& senderName) {
    return broadcastCommand(makePopupEnv(message, senderName));
}

// ---------------------------------------------------------------------------
// Uygulama yönetimi
// ---------------------------------------------------------------------------
bool SessionManager::sendAppClose(const QString& clientId,
                                   const QString& appName) {
    okidia::AppCommand cmd;
    cmd.set_name(appName.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_CLOSE);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendAppCloseAll(const QString& clientId) {
    return sendAppClose(clientId, "__ALL__");
}

bool SessionManager::sendAppBlockByName(const QString& clientId,
                                         const QString& appName) {
    okidia::AppCommand cmd;
    cmd.set_name(appName.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_BLOCK_BYNAME);
    env.set_payload(cmd.SerializeAsString());
    bool ok = sendCommand(clientId, env);
    if (ok) {
        setClientAppBlocked(clientId, true);
    }
    return ok;
}

bool SessionManager::sendAppBlockList(const QString& clientId,
                                       const QStringList& appList) {
    okidia::AppCommand cmd;
    for (const QString& app : appList) {
        if (!app.trimmed().isEmpty())
            cmd.add_blacklist(app.trimmed().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_BLOCK_LIST);
    env.set_payload(cmd.SerializeAsString());
    bool ok = sendCommand(clientId, env);
    if (ok) {
        setClientAppBlocked(clientId, !appList.isEmpty());
    }
    return ok;
}

bool SessionManager::sendAppBlockByHash(const QString& clientId,
                                         const QString& sha256Hash) {
    okidia::AppCommand cmd;
    cmd.set_hash(sha256Hash.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_BLOCK_BYHASH);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendAppWhitelist(const QString& clientId,
                                       const QStringList& allowedApps) {
    okidia::AppCommand cmd;
    for (const QString& app : allowedApps)
        cmd.add_whitelist(app.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_WHITELIST_SET);
    env.set_payload(cmd.SerializeAsString());
    return sendCommand(clientId, env);
}

bool SessionManager::sendAppUnblock(const QString& clientId) {
    bool ok = sendCommand(clientId,
                          makeEnv(okidia::MessageType::APP_UNBLOCK));
    if (ok) {
        setClientAppBlocked(clientId, false);
    }
    return ok;
}
// ============================================================================
// Faz 5 — Sohbet, El kaldırma
// ============================================================================

void SessionManager::handleChatMessage(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::ChatMessage msg;
    if (!msg.ParseFromString(env.payload())) return;

    QString from      = QString::fromStdString(msg.from());
    QString content   = QString::fromStdString(msg.content());
    QString roomId    = QString::fromStdString(msg.room_id());
    qint64  ts        = static_cast<qint64>(msg.timestamp());
    int     role      = static_cast<int>(msg.sender_role());
    bool    isSystem  = (msg.msg_type() == okidia::CHAT_MSG_SYSTEM);

    if (from.isEmpty()) from = clientId;

    Logger::info(QObject::tr("SessionManager: Chat ← ") + from + " (" + roomId + "): " + content);

    emit chatMessageReceived(clientId, roomId, from, content, ts, role, isSystem);
}

void SessionManager::handleHandRaise(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::HandRaiseEvent evt;
    if (!evt.ParseFromString(env.payload())) return;

    QString msg = QString::fromStdString(evt.message());
    Logger::info(QObject::tr("SessionManager: El kaldırıldı ← ") + clientId);

    setClientHandRaised(clientId, true, msg);   // thumbnail ikonu güncelle
    emit handRaiseReceived(clientId, msg);
}

void SessionManager::handleHandLower(const QString& clientId) {
    Logger::info(QObject::tr("SessionManager: El indirildi ← ") + clientId);
    setClientHandRaised(clientId, false);
    emit handLowerReceived(clientId);
}

bool SessionManager::sendChatMessage(const QString& roomId,
                                      const QString& content,
                                      const QString& from) {
    okidia::ChatMessage msg;
    msg.set_room_id(roomId.toStdString());
    msg.set_content(content.toStdString());
    msg.set_from(from.toStdString());
    msg.set_timestamp(
        OkidiaUtils::nowSecsU64());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_MESSAGE);
    env.set_sequence_id(0); // broadcast
    env.set_payload(msg.SerializeAsString());
    return broadcastCommand(env);
}

// ============================================================================
// Faz 6 — Web filtreleme, Sınav, Ses yayını
// ============================================================================

// ---------------------------------------------------------------------------
// Web filtreleme
static okidia::Envelope makeWebFilterEnv(
    okidia::MessageType type,
    const QString& reason = {}) {

    okidia::WebFilterCommand cmd;
    cmd.set_reason(reason.toStdString());

    okidia::Envelope env;
    env.set_type(type);
    env.set_payload(cmd.SerializeAsString());
    return env;
}

static okidia::Envelope makeWebFilterDomainsEnv(
    const QStringList& domains,
    const QString& reason = {}) {

    okidia::WebFilterCommand cmd;
    cmd.set_reason(reason.toStdString());
    for (const QString& d : domains) {
        if (!d.trimmed().isEmpty())
            cmd.add_domains(d.trimmed().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::WEB_BLOCK_DOMAINS);
    env.set_payload(cmd.SerializeAsString());
    return env;
}

bool SessionManager::sendWebBlockAll(const QString& clientId,
                                      const QString& reason) {
    bool ok = sendCommand(clientId,
        makeWebFilterEnv(okidia::MessageType::WEB_BLOCK_ALL, reason));
    if (ok) {
        setClientWebFiltered(clientId, true);
    }
    return ok;
}

bool SessionManager::sendWebBlockDomains(const QString& clientId,
                                          const QStringList& domains,
                                          const QString& reason) {
    bool ok = sendCommand(clientId,
        makeWebFilterDomainsEnv(domains, reason));
    if (ok) {
        setClientWebFiltered(clientId, !domains.isEmpty());
    }
    return ok;
}

bool SessionManager::sendWebUnblock(const QString& clientId) {
    bool ok = sendCommand(clientId,
        makeWebFilterEnv(okidia::MessageType::WEB_UNBLOCK));
    if (ok) {
        setClientWebFiltered(clientId, false);
    }
    return ok;
}

bool SessionManager::broadcastWebBlockAll(const QString& reason) {
    auto env = makeWebFilterEnv(okidia::MessageType::WEB_BLOCK_ALL,
                                reason);
    return broadcastCommand(env);
}

bool SessionManager::broadcastWebBlockDomains(const QStringList& domains,
                                               const QString& reason) {
    return broadcastCommand(makeWebFilterDomainsEnv(domains, reason));
}

bool SessionManager::broadcastWebUnblock() {
    return broadcastCommand(
        makeWebFilterEnv(okidia::MessageType::WEB_UNBLOCK));
}

// ---------------------------------------------------------------------------
// Sınav
// ---------------------------------------------------------------------------
static okidia::Envelope makeExamEnv(okidia::MessageType type,
                                     const QString& examId = {},
                                     const QString& title  = {},
                                     int            dur    = 0) {
    okidia::ExamCommand cmd;
    cmd.set_exam_id(examId.toStdString());
    cmd.set_title(title.toStdString());
    cmd.set_duration_s(static_cast<uint32_t>(dur));

    okidia::Envelope env;
    env.set_type(type);
    env.set_payload(cmd.SerializeAsString());
    return env;
}

bool SessionManager::sendExamStart(const QString& clientId,
                                    const QString& examId,
                                    const QString& title,
                                    int durationSeconds) {
    return sendCommand(clientId,
        makeExamEnv(okidia::MessageType::EXAM_START,
                    examId, title, durationSeconds));
}

bool SessionManager::broadcastExamStart(const QString& examId,
                                         const QString& title,
                                         int durationSeconds) {
    return broadcastCommand(
        makeExamEnv(okidia::MessageType::EXAM_START,
                    examId, title, durationSeconds));
}

bool SessionManager::broadcastExamCollect(const QString& examId) {
    return broadcastCommand(
        makeExamEnv(okidia::MessageType::EXAM_COLLECT, examId));
}

bool SessionManager::broadcastExamEnd() {
    return broadcastCommand(
        makeExamEnv(okidia::MessageType::EXAM_END));
}

// ── SSH Kimlik Yönetimi ────────────────────────────────────────────────────────

void SessionManager::fetchSshCredentials() {
    if (!m_conn || !m_conn->isConnected() || m_isOfflineMode) {
        // Çevrimdışı (SSH Modu): Broker aktif değilse yerel kopyadan yükle
        if (m_isOfflineMode && OkidiaDatabase::instance().isOpen()) {
            m_sshCredentials = OkidiaDatabase::instance().sshCredentials(m_activeUser.id);
            Logger::info(QObject::tr("SessionManager: Çevrimdışı modda yerel veritabanından %1 adet SSH kimliği yüklendi")
                         .arg(m_sshCredentials.size()));
            emit sshCredentialsLoaded();
        }
        return;
    }

    okidia::SshCredListRequest req;
    req.set_requester_user_id(m_activeUser.id.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SSH_CRED_LIST);
    env.set_payload(req.SerializeAsString());
    m_conn->sendEnvelope(env);
    Logger::info(QObject::tr("SessionManager: SSH kimlik listesi istendi"));
}

void SessionManager::loadLocalSshCredentials() {
    if (!OkidiaDatabase::instance().isOpen()) return;

    // Yerel SQLite veritabanındaki kayıtları yükle
    QList<SshCredentialRecord> localCreds = OkidiaDatabase::instance().sshCredentials(m_activeUser.id);
    
    // Şifre çözme: Eğer veritabanı Broker'dan tam DB senkronizasyonu ile geldiyse
    // secret kolonu AES-GCM ile şifrelenmiş BLOB olarak saklanıyor olabilir.
    // Şifreli ise sistem anahtarıyla anında düz metne çöz!
    for (auto cred : localCreds) {
        if (!cred.secret.isEmpty() && cred.secret.size() >= 28) {
            QByteArray decrypted = AesGcm::decrypt(cred.secret, AesGcm::defaultSystemKey());
            if (!decrypted.isEmpty()) {
                cred.secret = decrypted;
            }
        }

        bool exists = false;
        for (int i = 0; i < m_sshCredentials.size(); ++i) {
            if (m_sshCredentials[i].id == cred.id) {
                m_sshCredentials[i] = cred;
                exists = true;
                break;
            }
        }
        if (!exists) {
            m_sshCredentials.append(cred);
        }
    }

    Logger::info(QString(tr("SessionManager: Yerel veritabanından %1 adet SSH kimlik kaydı yüklendi (Disaster Recovery hazır)."))
                 .arg(m_sshCredentials.size()));
    emit sshCredentialsLoaded();
}

void SessionManager::syncPendingSshCredentials() {
    if (!m_conn || !m_conn->isConnected()) return;

    // 1. Bekleyen silmeleri uygula
    QString pendingDelsStr = OkidiaDatabase::instance().metaValue("pending_ssh_cred_dels");
    if (!pendingDelsStr.isEmpty()) {
        QStringList delIds = pendingDelsStr.split(',', Qt::SkipEmptyParts);
        for (const QString& id : delIds) {
            okidia::SshCredDelRequest req;
            req.set_id(id.toStdString());
            req.set_requester_user_id(m_activeUser.id.toStdString());

            okidia::Envelope env;
            env.set_type(okidia::MessageType::SSH_CRED_DEL);
            env.set_payload(req.SerializeAsString());
            m_conn->sendEnvelope(env);
            Logger::info(QObject::tr("SessionManager: Bekleyen SSH kimlik silme Broker'a iletildi → ") + id);
        }
        OkidiaDatabase::instance().setMetaValue("pending_ssh_cred_dels", "");
    }

    // 2. Bekleyen eklemeleri/güncellemeleri Broker'a gönder
    QString pendingAddsStr = OkidiaDatabase::instance().metaValue("pending_ssh_cred_adds");
    if (!pendingAddsStr.isEmpty()) {
        QStringList addIds = pendingAddsStr.split(',', Qt::SkipEmptyParts);
        for (const QString& id : addIds) {
            SshCredentialRecord cred = OkidiaDatabase::instance().sshCredentialById(id);
            if (!cred.id.isEmpty()) {
                okidia::SshCredAddRequest req;
                auto* pc = req.mutable_credential();
                pc->set_id(cred.id.toStdString());
                pc->set_name(cred.name.toStdString());
                pc->set_username(cred.username.toStdString());
                
                QByteArray encrypted = AesGcm::encrypt(cred.secret, AesGcm::defaultSystemKey());
                pc->set_secret(encrypted.toStdString());
                pc->set_is_key(cred.isKey);
                pc->set_owner_id(cred.ownerUserId.toStdString());
                pc->set_target_id(cred.targetId.toStdString());
                pc->set_target_type(cred.targetType.toStdString());
                pc->set_machine_id(cred.machineId.toStdString());
                pc->set_created_at(cred.createdAt);

                okidia::Envelope env;
                env.set_type(okidia::MessageType::SSH_CRED_ADD);
                env.set_payload(req.SerializeAsString());
                m_conn->sendEnvelope(env);

                Logger::info(QObject::tr("SessionManager: Bekleyen SSH kimlik ekleme Broker'a iletildi → ") + cred.name);
            }
        }
        OkidiaDatabase::instance().setMetaValue("pending_ssh_cred_adds", "");
    }
}

void SessionManager::addSshCredential(const SshCredentialRecord& cred) {
    // 1. Önce her zaman yerel SQLite veritabanına ve yerel belleğe yaz (Local-First Persistence)
    if (OkidiaDatabase::instance().isOpen()) {
        OkidiaDatabase::instance().upsertSshCredential(cred);
    }

    // Yerel bellek önbelleğini anında güncelle
    bool found = false;
    for (int i = 0; i < m_sshCredentials.size(); ++i) {
        if (m_sshCredentials[i].id == cred.id) {
            m_sshCredentials[i] = cred;
            found = true;
            break;
        }
    }
    if (!found) {
        m_sshCredentials.append(cred);
    }
    emit sshCredentialsLoaded();

    // 2. Eğer Broker bağlıysa anında gönder; değilse bekleme listesine (pending_ssh_cred_adds) kaydet
    if (m_conn && m_conn->isConnected()) {
        okidia::SshCredAddRequest req;
        auto* pc = req.mutable_credential();
        pc->set_id(cred.id.toStdString());
        pc->set_name(cred.name.toStdString());
        pc->set_username(cred.username.toStdString());
        
        // Şifreyi şifrele
        QByteArray encrypted = AesGcm::encrypt(cred.secret, AesGcm::defaultSystemKey());
        pc->set_secret(encrypted.toStdString());

        pc->set_is_key(cred.isKey);
        pc->set_owner_id(cred.ownerUserId.toStdString());
        pc->set_target_id(cred.targetId.toStdString());
        pc->set_target_type(cred.targetType.toStdString());
        pc->set_machine_id(cred.machineId.toStdString());
        pc->set_created_at(cred.createdAt);

        okidia::Envelope env;
        env.set_type(okidia::MessageType::SSH_CRED_ADD);
        env.set_payload(req.SerializeAsString());
        m_conn->sendEnvelope(env);
        
        Logger::info(QObject::tr("SessionManager: SSH kimliği Broker'a gönderildi → ") + cred.name + " (" + cred.targetType + ":" + cred.targetId + ")");
    } else {
        // Çevrimdışı: Broker bağlanınca gönderilmek üzere bekleyenlere ekle
        QString pending = OkidiaDatabase::instance().metaValue("pending_ssh_cred_adds");
        QStringList list = pending.split(',', Qt::SkipEmptyParts);
        if (!list.contains(cred.id)) {
            list.append(cred.id);
            OkidiaDatabase::instance().setMetaValue("pending_ssh_cred_adds", list.join(','));
        }
        Logger::info(QObject::tr("SessionManager: Çevrimdışı mod: SSH kimliği yerel veritabanına kaydedildi (Broker bağlantısında eşitlenecek) → ") + cred.name);
    }
}

void SessionManager::deleteSshCredential(const QString& id) {
    // 1. Önce yerel veritabanından ve bellekten sil
    if (OkidiaDatabase::instance().isOpen()) {
        OkidiaDatabase::instance().deleteSshCredential(id);
    }
    for (int i = 0; i < m_sshCredentials.size(); ++i) {
        if (m_sshCredentials[i].id == id) {
            m_sshCredentials.removeAt(i);
            break;
        }
    }
    emit sshCredentialsLoaded();

    // 2. Broker bağlıysa silme isteğini gönder, değilse bekleyenlere ekle
    if (m_conn && m_conn->isConnected()) {
        okidia::SshCredDelRequest req;
        req.set_id(id.toStdString());
        req.set_requester_user_id(m_activeUser.id.toStdString());

        okidia::Envelope env;
        env.set_type(okidia::MessageType::SSH_CRED_DEL);
        env.set_payload(req.SerializeAsString());
        m_conn->sendEnvelope(env);
        
        Logger::info(QObject::tr("SessionManager: SSH kimliği silme Broker'a gönderildi → ") + id);
    } else {
        QString pending = OkidiaDatabase::instance().metaValue("pending_ssh_cred_dels");
        QStringList list = pending.split(',', Qt::SkipEmptyParts);
        if (!list.contains(id)) {
            list.append(id);
            OkidiaDatabase::instance().setMetaValue("pending_ssh_cred_dels", list.join(','));
        }
        Logger::info(QObject::tr("SessionManager: Çevrimdışı mod: SSH kimliği yerelden silindi, Broker silmesi kuyruğa alındı → ") + id);
    }
}

void SessionManager::requestJoinCode(const QString& userId) {
    if (!m_conn || !m_conn->isConnected()) return;

    okidia::JoinCodeRequest req;
    req.set_user_id(userId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::JOIN_CODE_REQUEST);
    env.set_payload(req.SerializeAsString());
    m_conn->sendEnvelope(env);
    Logger::info(QObject::tr("SessionManager: Davet kodu talep edildi → ") + userId);
}

bool SessionManager::sendSetBrokerPort(quint16 newPort) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::BrokerPortSetRequest req;
    req.set_new_port(newPort);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROKER_PORT_SET_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Broker port değiştirme isteği gönderiliyor → %1").arg(newPort));
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendSetBrokerConfig(const QString& name, quint16 port) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::BrokerConfigSetRequest req;
    req.set_broker_name(name.toStdString());
    req.set_new_port(port);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROKER_CONFIG_SET_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Broker yapılandırma değiştirme isteği gönderiliyor → %1:%2").arg(name).arg(port));
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserAdd(const QString& username, const QString& displayName, const QString& password, const QString& role, bool isBackupReplica, const QString& userId, const QString& replicaMachineId, const QString& replicaHostname) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserAddRequest req;
    req.set_username(username.toStdString());
    req.set_display_name(displayName.toStdString());
    req.set_password_hash(password.toStdString());
    req.set_role(role.toStdString());
    req.set_is_backup_replica(isBackupReplica);
    if (!userId.trimmed().isEmpty()) {
        req.set_user_id(userId.trimmed().toStdString());
    }
    if (!replicaMachineId.trimmed().isEmpty()) {
        req.set_replica_machine_id(replicaMachineId.trimmed().toStdString());
    }
    if (!replicaHostname.trimmed().isEmpty()) {
        req.set_replica_hostname(replicaHostname.trimmed().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_ADD_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Kullanıcı ekleme isteği gönderiliyor -> ") + username);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserUpdate(const QString& userId, const QString& displayName, const QString& role, const QString& passwordHash, bool isBackupReplica, const QString& replicaMachineId, const QString& replicaHostname) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserUpdateRequest req;
    req.set_user_id(userId.toStdString());
    req.set_display_name(displayName.toStdString());
    req.set_role(role.toStdString());
    req.set_password_hash(passwordHash.toStdString());
    req.set_is_backup_replica(isBackupReplica);
    if (!replicaMachineId.trimmed().isEmpty()) {
        req.set_replica_machine_id(replicaMachineId.trimmed().toStdString());
    }
    if (!replicaHostname.trimmed().isEmpty()) {
        req.set_replica_hostname(replicaHostname.trimmed().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_UPDATE_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Kullanıcı güncelleme isteği gönderiliyor -> ") + userId + " (" + role + ")");
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserSetReplica(const QString& userId, bool isBackupReplica, const QString& replicaMachineId, const QString& replicaHostname) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserSetReplicaRequest req;
    req.set_user_id(userId.toStdString());
    req.set_is_backup_replica(isBackupReplica);
    if (!replicaMachineId.trimmed().isEmpty()) {
        req.set_replica_machine_id(replicaMachineId.trimmed().toStdString());
    }
    if (!replicaHostname.trimmed().isEmpty()) {
        req.set_replica_hostname(replicaHostname.trimmed().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_SET_REPLICA_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Kullanıcı ikincil yedek yetkisi değiştiriliyor -> ") + userId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserDelete(const QString& userId) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserDelRequest req;
    req.set_user_id(userId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_DEL_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Kullanıcı silme isteği gönderiliyor -> ") + userId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserPwReset(const QString& userId, const QString& password, bool isAdminReset) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserPwResetRequest req;
    req.set_user_id(userId.toStdString());
    req.set_password_hash(password.toStdString());
    req.set_is_admin_reset(isAdminReset);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_PW_RESET_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Şifre sıfırlama isteği gönderiliyor -> ") + userId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbClassAdd(const QString& name, const QString& icon, const QString& parentId, const QString& type, const QString& id) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbClassAddRequest req;
    req.set_name(name.toStdString());
    req.set_icon(icon.toStdString());
    req.set_parent_id(parentId.toStdString());
    req.set_type(type.toStdString());
    if (!id.isEmpty()) {
        req.set_id(id.toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_CLASS_ADD_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Sınıf/grup ekleme isteği gönderiliyor -> ") + name);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbClassDelete(const QString& classId) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbClassDelRequest req;
    req.set_class_id(classId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_CLASS_DEL_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Sınıf/grup silme isteği gönderiliyor -> ") + classId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbClassRename(const QString& classId, const QString& newName) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbClassRenameRequest req;
    req.set_class_id(classId.toStdString());
    req.set_new_name(newName.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_CLASS_RENAME_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Sınıf/grup isim değiştirme isteği gönderiliyor -> ") + classId + QObject::tr(" to ") + newName);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbAgentAssign(const QString& agentId, const QString& groupId) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbAgentAssignRequest req;
    req.set_agent_id(agentId.toStdString());
    req.set_group_id(groupId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_AGENT_ASSIGN_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: İstemci sınıf atama isteği gönderiliyor -> client: ") + agentId + QObject::tr(" to group: ") + groupId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserGrant(const QString& userId, const QString& classId, const QString& level) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserGrantRequest req;
    req.set_user_id(userId.toStdString());
    req.set_class_id(classId.toStdString());
    req.set_level(level.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_GRANT_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Kullanıcı yetki verme isteği gönderiliyor -> user: ") + userId + QObject::tr(" class: ") + classId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbUserRevoke(const QString& userId, const QString& classId) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbUserRevokeRequest req;
    req.set_user_id(userId.toStdString());
    req.set_class_id(classId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_USER_REVOKE_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Kullanıcı yetki kaldırma isteği gönderiliyor -> user: ") + userId + QObject::tr(" class: ") + classId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbTeacherUpsert(const TeacherRecord& teacher) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbTeacherUpsertRequest req;
    req.set_teacher_id(teacher.id.toStdString());
    req.set_username(teacher.username.toStdString());
    req.set_display_name(teacher.displayName.toStdString());
    req.set_branch(teacher.branch.toStdString());
    req.set_seed_hex(teacher.seedHex.toStdString());
    req.set_password(teacher.password.toStdString());
    req.set_is_active(teacher.isActive);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_TEACHER_UPSERT_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Öğretmen kayıt isteği gönderiliyor -> ") + teacher.displayName + " (" + teacher.username + ")");
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbTeacherDelete(const QString& teacherId) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbTeacherDeleteRequest req;
    req.set_teacher_id(teacherId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_TEACHER_DEL_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Öğretmen silme isteği gönderiliyor -> ") + teacherId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendTeacherKeySyncToAll() {
    if (!m_conn || !m_conn->isConnected()) return false;
    QList<TeacherRecord> teachers = OkidiaDatabase::instance().allTeachers();
    okidia::TeacherKeySyncReq req;
    for (const auto& t : teachers) {
        if (!t.isActive) continue;
        auto* item = req.add_teachers();
        item->set_teacher_id(t.id.toStdString());
        item->set_display_name(t.displayName.toStdString());
        item->set_username(t.username.toStdString());
        item->set_seed_hex(t.seedHex.toStdString());
        item->set_password(t.password.toStdString());
    }


    okidia::Envelope env;
    env.set_type(okidia::MessageType::TEACHER_KEY_SYNC_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: %1 öğretmenin tohum listesi tahtalara senkronize edilmek üzere gönderiliyor...").arg(teachers.size()));
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendQueryLocalUsers(const QString& clientId) {
    if (!m_conn || !m_conn->isConnected() || clientId.isEmpty()) return false;
    okidia::QueryLocalUsersReq req;
    req.set_client_id(clientId.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::QUERY_LOCAL_USERS_REQ);
    env.set_target_id(clientId.toStdString());
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Yerel kullanıcı sorgusu gönderiliyor -> ") + clientId);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendAuditLogRecord(const QString& actor, const QString& target, const QString& detail, const QString& category, const QString& userId, const QString& masterId, qint64 timestamp) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::AuditLogRecord req;
    req.set_actor(actor.toStdString());
    req.set_target(target.toStdString());
    req.set_detail(detail.toStdString());
    req.set_category(category.toStdString());
    req.set_user_id(userId.toStdString());
    req.set_master_id(masterId.toStdString());
    req.set_timestamp(static_cast<uint64_t>(timestamp));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIT_LOG_RECORD);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendAuditLogQuery(qint64 fromTimestamp,
                                       qint64 toTimestamp,
                                       const QString& category,
                                       int limit,
                                       int offset,
                                       const QString& searchQuery,
                                       const QString& actor,
                                       const QString& target) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::AuditLogQueryRequest req;
    req.set_from_timestamp(static_cast<uint64_t>(fromTimestamp));
    req.set_to_timestamp(static_cast<uint64_t>(toTimestamp));
    req.set_category(category.toStdString());
    req.set_limit(limit);
    req.set_offset(offset);
    req.set_search_query(searchQuery.toStdString());
    req.set_actor(actor.toStdString());
    req.set_target(target.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIT_LOG_QUERY_REQ);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendChatHistoryRequest(const QString& roomId, int limit, qint64 beforeTimestamp) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::ChatHistoryRequest req;
    req.set_room_id(roomId.toStdString());
    req.set_limit(limit);
    req.set_before_timestamp(static_cast<uint64_t>(beforeTimestamp));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_HISTORY_REQ);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendRoomJoinRequest(const QString& requestId, const QString& roomId, const QString& roomName,
                                         const QString& fromUsername, const QString& fromDisplay, const QString& targetUsername) {
    okidia::RoomJoinRequest req;
    req.set_request_id(requestId.toStdString());
    req.set_room_id(roomId.toStdString());
    req.set_room_name(roomName.toStdString());
    req.set_from_username(fromUsername.toStdString());
    req.set_from_display(fromDisplay.toStdString());
    req.set_target_username(targetUsername.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::ROOM_JOIN_REQUEST);
    env.set_payload(req.SerializeAsString());
    return broadcastCommand(env);
}

bool SessionManager::sendRoomJoinResponse(const QString& requestId, const QString& roomId, const QString& fromUsername,
                                          const QString& targetUsername, bool accepted, const QString& reason) {
    okidia::RoomJoinResponse res;
    res.set_request_id(requestId.toStdString());
    res.set_room_id(roomId.toStdString());
    res.set_from_username(fromUsername.toStdString());
    res.set_target_username(targetUsername.toStdString());
    res.set_accepted(accepted);
    res.set_reason(reason.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::ROOM_JOIN_RESPONSE);
    env.set_payload(res.SerializeAsString());
    return broadcastCommand(env);
}



bool SessionManager::requestBrokerUpdatesList() {
    if (!m_tls || !m_tls->isConnected()) return false;
    
    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROKER_UPDATES_LIST_REQ);
    
    okidia::BrokerUpdatesListRequest req;
    env.set_payload(req.SerializeAsString());
    
    Logger::info(QObject::tr("SessionManager: BrokerUpdatesListRequest gonderiliyor..."));
    
    m_tls->connection()->sendEnvelope(env);
    return true;
}

bool SessionManager::requestBrokerUpdatesDelete(const QStringList& filenames) {
    if (!m_tls || !m_tls->isConnected()) return false;
    
    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROKER_UPDATES_DELETE_REQ);
    
    okidia::BrokerUpdatesDeleteRequest req;
    for (const QString& f : filenames) {
        req.add_filenames(f.toStdString());
    }
    env.set_payload(req.SerializeAsString());
    
    m_tls->connection()->sendEnvelope(env);
    return true;
}

bool SessionManager::sendLogTailRequest(const QString& targetId,
                                        int maxLines,
                                        const QString& fromTime,
                                        const QString& toTime,
                                        const QString& levelFilter,
                                        const QString& searchQuery,
                                        const QString& targetFile) {
    if (!m_tls || !m_tls->isConnected() || !m_tls->connection()) return false;

    okidia::LogTailRequest req;
    req.set_max_lines(maxLines);
    if (!fromTime.isEmpty()) req.set_from_time(fromTime.toStdString());
    if (!toTime.isEmpty()) req.set_to_time(toTime.toStdString());
    if (!levelFilter.isEmpty()) req.set_level_filter(levelFilter.toStdString());
    if (!searchQuery.isEmpty()) req.set_search_query(searchQuery.toStdString());
    if (!targetFile.isEmpty()) req.set_target_file(targetFile.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::LOG_TAIL_REQ);
    env.set_target_id(targetId.toStdString());
    env.set_payload(req.SerializeAsString());

    return m_tls->connection()->sendEnvelope(env);
}

bool SessionManager::sendAgentUninstallPassword(const QString& hash) {
    if (!m_tls || !m_tls->isConnected() || !m_tls->connection()) return false;

    okidia::AgentUninstallPassSet req;
    req.set_password_hash(hash.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AGENT_UNINSTALL_PASS_SET);
    env.set_payload(req.SerializeAsString());

    Logger::info(QObject::tr("SessionManager: İstemci kaldırma parolası değiştirme isteği Broker'a gönderiliyor"));
    return m_tls->connection()->sendEnvelope(env);
}

bool SessionManager::sendRollbackToStable(const QString& clientId, const QString& targetVersion) {
    if (!m_tls || !m_tls->isConnected() || !m_tls->connection()) return false;

    okidia::RollbackToStableRequest req;
    req.set_client_id(clientId.toStdString());
    QString target = targetVersion.isEmpty() ? m_brokerVersion : targetVersion;
    req.set_target_version(target.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::ROLLBACK_TO_STABLE_REQ);
    env.set_target_id(clientId.toStdString());
    env.set_payload(req.SerializeAsString());

    Logger::info(QObject::tr("SessionManager: Kararlı sürüme geri alma (Rollback) isteği gönderiliyor -> %1 (Hedef: %2)")
                 .arg(clientId, target));
    return m_tls->connection()->sendEnvelope(env);
}

bool SessionManager::sendCustomProfileUpsert(const CustomProfileRecord& prof) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::CustomProfileUpsertReq req;
    auto* item = req.mutable_profile();
    item->set_id(prof.id);
    item->set_name(prof.name.toStdString());
    item->set_play_bell(prof.playBell);
    item->set_recess_lock(prof.recessLock);
    item->set_chat_enabled(prof.chatEnabled);
    item->set_icon_name(prof.iconName.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CUSTOM_PROFILE_UPSERT_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Özel profil upsert isteği gönderiliyor -> ") + prof.name);
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendCustomProfileDelete(int profileId) {
    if (!m_conn || !m_conn->isConnected() || profileId <= 0) return false;
    okidia::CustomProfileDeleteReq req;
    req.set_profile_id(profileId);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CUSTOM_PROFILE_DELETE_REQ);
    env.set_payload(req.SerializeAsString());
    Logger::info(QObject::tr("SessionManager: Özel profil silme isteği gönderiliyor -> ") + QString::number(profileId));
    return m_conn->sendEnvelope(env);
}



