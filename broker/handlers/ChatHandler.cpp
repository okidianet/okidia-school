#include "ChatHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/OkidiaAuditDatabase.h"
#include "../../common/db/OkidiaChatDatabase.h"
#include "../../common/utils/OkidiaUtils.h"

namespace {
constexpr qint64 kSpamWindowSecs        = 10;
constexpr int    kSpamMaxMsgsInWindow   = 5;
constexpr int    kSpamIdenticalMax      = 3;
constexpr qint64 kSpamIdenticalWindow   = 30;
constexpr qint64 kViolationDecaySecs    = 3600; // 60 dk temiz kullanım sonrası seviye sıfırlanır
constexpr int    kMaxMessageLength      = 500;
constexpr int    kMaxMessageNewlines    = 5;
constexpr qint64 kHandRaiseCooldownSecs = 30;

int muteDurationForLevel(int level) {
    if (level <= 1) return 60;   // 1 dk
    if (level == 2) return 300;  // 5 dk
    return 600;                  // 10 dk
}
} // namespace

ChatHandler::ChatHandler(BrokerContext* ctx) : m_ctx(ctx) {}

QString ChatHandler::makeUserKey(const QString& machineId, const QString& username) {
    const QString normUser = username.trimmed().toLower();
    return machineId + ":" + (normUser.isEmpty() ? QStringLiteral("__unknown__") : normUser);
}

QString ChatHandler::formatStudentDisplayName(const QString& userDisplayName,
                                              const QString& username,
                                              const QString& hostname,
                                              const QString& fallbackId) {
    const QString namePart = !userDisplayName.trimmed().isEmpty()
        ? userDisplayName.trimmed()
        : username.trimmed();
    const QString hostPart = hostname.trimmed();
    if (!namePart.isEmpty() && !hostPart.isEmpty()) {
        return QString("%1 - %2").arg(namePart, hostPart);
    } else if (!namePart.isEmpty()) {
        return namePart;
    } else if (!hostPart.isEmpty()) {
        return hostPart;
    }
    return fallbackId;
}

void ChatHandler::sendMuteState(const QString& clientId,
                                const QString& username,
                                bool muted,
                                qint64 mutedUntil,
                                int durationSecs,
                                int violationLevel,
                                const QString& reason) {
    okidia::ChatMuteState proto;
    proto.set_client_id(clientId.toStdString());
    proto.set_username(username.toStdString());
    proto.set_muted(muted);
    proto.set_muted_until(mutedUntil > 0 ? static_cast<uint64_t>(mutedUntil) : 0);
    proto.set_duration_secs(durationSecs > 0 ? static_cast<uint32_t>(durationSecs) : 0);
    proto.set_violation_level(violationLevel > 0 ? static_cast<uint32_t>(violationLevel) : 0);
    proto.set_reason(reason.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_MUTE_STATE);
    env.set_target_id(clientId.toStdString());
    env.set_payload(proto.SerializeAsString());

    if (m_ctx->registry->hasClient(clientId)) {
        m_ctx->registry->forwardToClient(clientId, env);
    }
    for (Connection* mConn : m_ctx->registry->masterConnections()) {
        mConn->sendEnvelope(env);
    }
}

void ChatHandler::syncMuteStateForClient(const QString& clientId) {
    if (!m_ctx || !m_ctx->registry || !m_ctx->registry->hasClient(clientId)) return;
    const RegisteredClient rc = m_ctx->registry->client(clientId);
    const QString userKey = makeUserKey(rc.machineId.isEmpty() ? clientId : rc.machineId, rc.username);
    const qint64 now = OkidiaUtils::nowSecs();

    auto it = m_spamTrackers.find(userKey);
    if (it != m_spamTrackers.end() && it->mutedUntil > now) {
        sendMuteState(clientId, rc.username, true, it->mutedUntil,
                      static_cast<int>(it->mutedUntil - now), it->violationLevel, it->muteReason);
    } else {
        // Farklı kullanıcı oturum açtıysa (ör. 5-A → 6-A) veya süre dolduysa kilidi temizle
        sendMuteState(clientId, rc.username, false, 0, 0, 0, QString());
    }
}

bool ChatHandler::handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster) {
    // ── CHAT_MUTE_STATE (Master'dan manuel susturma / susturma kaldırma) ────
    if (env.type() == okidia::MessageType::CHAT_MUTE_STATE) {
        const bool senderIsMaster = (isMaster && *isMaster) || m_ctx->registry->isMasterConnection(conn);
        if (!senderIsMaster) {
            return true; // Agent kendi mute durumunu değiştiremez
        }
        okidia::ChatMuteState req;
        if (!req.ParseFromString(env.payload())) return true;

        const QString targetClientId = QString::fromStdString(req.client_id());
        if (targetClientId.isEmpty()) return true;

        const QString senderUserId = m_ctx->registry->authenticatedUserId(conn);
        const QString senderRole   = m_ctx->registry->authenticatedRole(conn);
        const AgentRecord targetRec = OkidiaDatabase::instance().agent(targetClientId);

        bool authorized = (senderRole == "YM");
        if (!authorized && senderRole == "OM" && !targetRec.classId.isEmpty()) {
            const QStringList allowedClasses = OkidiaDatabase::instance().classIdsForUser(senderUserId);
            if (allowedClasses.contains(targetRec.classId)) {
                authorized = true;
            } else {
                const ClassRecord cr = OkidiaDatabase::instance().classById(targetRec.classId);
                if (!cr.parentId.isEmpty() && allowedClasses.contains(cr.parentId)) {
                    authorized = true;
                }
            }
        }
        if (!authorized) {
            Logger::warning(QObject::tr("Broker: Yetkisiz sohbet susturma talebi engellendi → ") + senderUserId);
            return true;
        }

        const RegisteredClient rc = m_ctx->registry->client(targetClientId);
        const QString activeUsername = !rc.username.isEmpty()
            ? rc.username
            : QString::fromStdString(req.username());
        const QString userKey = makeUserKey(targetClientId, activeUsername);
        const qint64 now = OkidiaUtils::nowSecs();

        if (req.muted()) {
            const int dur = qBound(60, static_cast<int>(req.duration_secs() > 0 ? req.duration_secs() : 300), 3600);
            auto& tracker = m_spamTrackers[userKey];
            tracker.mutedUntil = now + dur;
            tracker.muteReason = QString::fromStdString(req.reason());
            if (tracker.muteReason.isEmpty()) {
                tracker.muteReason = QObject::tr("Öğretmen tarafından %1 dakika sohbet kısıtlaması uygulandı.").arg(dur / 60);
            }
            Logger::info(QObject::tr("Broker: Manuel sohbet susturması → %1 (%2 sn)").arg(userKey).arg(dur));
            sendMuteState(targetClientId, activeUsername, true, tracker.mutedUntil, dur, 0, tracker.muteReason);
        } else {
            if (m_spamTrackers.contains(userKey)) {
                auto& tracker = m_spamTrackers[userKey];
                tracker.mutedUntil = 0;
                tracker.violationLevel = 0;
                tracker.recentTimestamps.clear();
                tracker.consecutiveSameCount = 0;
                tracker.muteReason.clear();
            }
            Logger::info(QObject::tr("Broker: Sohbet susturması kaldırıldı → %1").arg(userKey));
            sendMuteState(targetClientId, activeUsername, false, 0, 0, 0, QString());
        }
        return true;
    }

    // ── HAND_RAISE (Agent bekleme süresi kontrolü) ──────────────────────────
    if (env.type() == okidia::MessageType::HAND_RAISE) {
        const bool senderIsMaster = (isMaster && *isMaster) || m_ctx->registry->isMasterConnection(conn);
        if (!senderIsMaster) {
            QString senderClientId = m_ctx->registry->clientIdByConnection(conn);
            if (!senderClientId.isEmpty()) {
                const RegisteredClient rc = m_ctx->registry->client(senderClientId);
                const QString userKey = makeUserKey(rc.machineId.isEmpty() ? senderClientId : rc.machineId, rc.username);
                const qint64 now = OkidiaUtils::nowSecs();
                const qint64 lastAt = m_lastHandRaise.value(userKey, 0);
                if (lastAt > 0 && (now - lastAt) < kHandRaiseCooldownSecs) {
                    Logger::warning(QObject::tr("Broker: Yardım talebi (HAND_RAISE) bekleme süresi dolmadı → %1").arg(userKey));
                    return true; // Düşür
                }
                m_lastHandRaise[userKey] = now;
            }
        }
        return false; // Normal yönlendirme devam etsin
    }

    // ── CHAT_HISTORY_REQ ────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::CHAT_HISTORY_REQ) {
        okidia::ChatHistoryRequest req;
        if (req.ParseFromString(env.payload())) {
            okidia::ChatHistoryResponse resp;
            QString roomId = QString::fromStdString(req.room_id());
            resp.set_room_id(roomId.toStdString());
            
            auto records = OkidiaChatDatabase::instance().getChatMessages(
                roomId,
                req.limit(),
                req.before_timestamp()
            );
            for (const auto& r : records) {
                auto* record = resp.add_records();
                record->set_id(r.id.toStdString());
                record->set_room_id(r.roomId.toStdString());
                record->set_from_host(r.fromHost.toStdString());
                record->set_sender_role(r.senderRole);
                record->set_content(r.content.toStdString());
                record->set_timestamp(r.timestamp);
                record->set_is_system(r.isSystem);
            }
            
            okidia::Envelope re;
            re.set_type(okidia::MessageType::CHAT_HISTORY_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── CHAT_LOCK_STATE ─────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::CHAT_LOCK_STATE) {
        okidia::ChatLockState cls;
        if (cls.ParseFromString(env.payload())) {
            const QString classId = QString::fromStdString(cls.class_id());
            QString senderUserId = m_ctx->registry->authenticatedUserId(conn);
            QString senderRole   = m_ctx->registry->authenticatedRole(conn);
            if (senderUserId.isEmpty()) {
                UserRecord u = OkidiaDatabase::instance().userById(peerId);
                if (u.id.isEmpty()) u = OkidiaDatabase::instance().userByUsername(peerId);
                senderUserId = u.id;
                senderRole   = u.role;
            }

            // Yetki Kontrolü:
            // YM her kilide müdahale edebilirken OM'ler sadece kendilerine atanmış sınıf ve grupların kilit durumlarını değiştirebilsinler.
            bool authorized = false;
            if (senderRole == "YM") {
                authorized = true;
            } else if (senderRole == "OM") {
                const QStringList allowedClasses = OkidiaDatabase::instance().classIdsForUser(senderUserId);
                if (allowedClasses.contains(classId)) {
                    authorized = true;
                }
            }

            if (!authorized) {
                Logger::warning(QObject::tr("Broker: Yetkisiz sohbet kilidi değiştirme engellendi! Kullanıcı: ") +
                                senderUserId + " (" + senderRole + QObject::tr(") Sınıf: ") + classId);
                return true;
            }

            OkidiaDatabase::instance().setChatLockState(
                classId, cls.chat_locked(), cls.private_locked());

            // Sınıfa ait tüm agent'lara gönder
            for (const auto& c : m_ctx->registry->allClients()) {
                const AgentRecord rec =
                    OkidiaDatabase::instance().agent(c.machineId);
                if (rec.classId == classId)
                    m_ctx->registry->forwardToClient(c.clientId, env);
            }

            // Diğer tüm Master bağlantılarına da ilet (arayüzleri anında güncellensin)
            for (Connection* mConn : m_ctx->registry->masterConnections()) {
                if (mConn != conn) {
                    mConn->sendEnvelope(env);
                }
            }

            if (m_ctx->broadcastDbSyncSignal) {
                m_ctx->broadcastDbSyncSignal();
            }
        }
        return true;
    }

    // ── CHAT_MEMBER_JOINED / CHAT_MEMBER_LEFT ───────────────────────────────
    if (env.type() == okidia::MessageType::CHAT_MEMBER_JOINED ||
        env.type() == okidia::MessageType::CHAT_MEMBER_LEFT) {
        okidia::ChatMemberEvent evt;
        if (evt.ParseFromString(env.payload())) {
            const QString roomId = QString::fromStdString(evt.room_id());
            const QString clientId = QString::fromStdString(evt.client_id());

            if (env.type() == okidia::MessageType::CHAT_MEMBER_JOINED) {
                (*m_ctx->activeRoomMembers)[roomId][clientId] = evt;
                (*m_ctx->connToRoomMembers)[conn].insert({roomId, clientId});
            } else {
                (*m_ctx->activeRoomMembers)[roomId].remove(clientId);
                (*m_ctx->connToRoomMembers)[conn].remove({roomId, clientId});
            }

            // 1. Diğer tüm Master bağlantılarına ilet
            for (Connection* mConn : m_ctx->registry->masterConnections()) {
                if (mConn != conn) {
                    mConn->sendEnvelope(env);
                }
            }

            // 2. İlgili Agent'lara ilet (Öğretmenler Odası hariç)
            if (roomId != "room_admin") {
                QString targetClassId;
                if (roomId.startsWith("room_")) {
                    targetClassId = roomId.mid(5);
                }
                for (const auto& c : m_ctx->registry->allClients()) {
                    const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                    if (targetClassId.isEmpty() || rec.classId == targetClassId) {
                        m_ctx->registry->forwardToClient(c.clientId, env);
                    }
                }
            }
        }
        return true;
    }

    // ── ROOM_JOIN_REQUEST / ROOM_JOIN_RESPONSE ──────────────────────────────
    if (env.type() == okidia::MessageType::ROOM_JOIN_REQUEST ||
        env.type() == okidia::MessageType::ROOM_JOIN_RESPONSE) {
        for (Connection* mConn : m_ctx->registry->masterConnections()) {
            if (mConn != conn) {
                mConn->sendEnvelope(env);
            }
        }
        return true;
    }

    // ── PRIVATE_CHAT_OPEN / ACCEPT / CLOSE (Audit Loglama) ───────────────────
    if (env.type() == okidia::MessageType::PRIVATE_CHAT_OPEN ||
        (env.type() == okidia::MessageType::PRIVATE_CHAT_ACCEPT && (isMaster && *isMaster))) {
        QString sessionId;
        if (env.type() == okidia::MessageType::PRIVATE_CHAT_OPEN) {
            okidia::PrivateChatOpen pco;
            if (pco.ParseFromString(env.payload())) {
                sessionId = QString::fromStdString(pco.session_id());
            }
        } else {
            okidia::PrivateChatAccept pca;
            if (pca.ParseFromString(env.payload())) {
                sessionId = QString::fromStdString(pca.session_id());
            }
        }

        if (!sessionId.isEmpty()) {
            QString masterUserId = m_ctx->registry->authenticatedUserId(conn);
            UserRecord mu = OkidiaDatabase::instance().userById(masterUserId);
            QString targetId = QString::fromStdString(env.target_id());
            if (targetId.startsWith("master:")) {
                QString targetUser = targetId.mid(7);
                UserRecord tu = OkidiaDatabase::instance().userByUsername(targetUser);
                if (tu.id.isEmpty()) tu = OkidiaDatabase::instance().userById(targetUser);
                OkidiaAuditDatabase::instance().logPrivateChatSession(
                    sessionId,
                    masterUserId, mu.username, mu.displayName,
                    tu.id.isEmpty() ? targetUser : tu.id,
                    tu.username.isEmpty() ? targetUser : tu.username,
                    "Master"
                );
            } else {
                AgentRecord ar = OkidiaDatabase::instance().agent(targetId);
                OkidiaAuditDatabase::instance().logPrivateChatSession(
                    sessionId,
                    masterUserId, mu.username, mu.displayName,
                    targetId, ar.hostname, "Agent"
                );
            }
        }
    }

    if (env.type() == okidia::MessageType::PRIVATE_CHAT_CLOSE && (isMaster && *isMaster)) {
        okidia::PrivateChatClose pcc;
        if (pcc.ParseFromString(env.payload())) {
            QString sessionId = QString::fromStdString(pcc.session_id());
            OkidiaAuditDatabase::instance().closePrivateChatSession(sessionId);
        }
    }

    // ── PRIVATE_CHAT_REQUEST / ACCEPT / REJECT / OPEN / MESSAGE / CLOSE (Yönlendirme) ─
    if (env.type() == okidia::MessageType::PRIVATE_CHAT_REQUEST ||
        env.type() == okidia::MessageType::PRIVATE_CHAT_ACCEPT ||
        env.type() == okidia::MessageType::PRIVATE_CHAT_REJECT ||
        env.type() == okidia::MessageType::PRIVATE_CHAT_OPEN ||
        env.type() == okidia::MessageType::PRIVATE_CHAT_MESSAGE ||
        env.type() == okidia::MessageType::PRIVATE_CHAT_CLOSE) {

        // Gönderen kimliğini tespit et (Master ise "master:username", Agent ise clientId)
        QString senderId;
        const bool senderIsMaster = (isMaster && *isMaster) || m_ctx->registry->isMasterConnection(conn);
        if (senderIsMaster) {
            const QString uName = m_ctx->registry->authenticatedUsername(conn);
            senderId = uName.isEmpty() ? ("master:" + m_ctx->registry->authenticatedUserId(conn)) : ("master:" + uName);
        } else {
            for (const auto& c : m_ctx->registry->allClients()) {
                if (c.connection == conn) {
                    senderId = c.clientId;
                    break;
                }
            }
        }

        if (!senderIsMaster && (env.type() == okidia::MessageType::PRIVATE_CHAT_REQUEST ||
                                env.type() == okidia::MessageType::PRIVATE_CHAT_MESSAGE)) {
            // Güvenlik: İstemciden istemciye özel sohbet sunucu tarafında da kapalı
            const QString reqTargetId = QString::fromStdString(env.target_id());
            if (!reqTargetId.isEmpty() && !reqTargetId.startsWith("master:") && reqTargetId != "master") {
                Logger::warning(QObject::tr("Broker: İstemciden istemciye özel sohbet engellendi → %1 → %2")
                                .arg(senderId, reqTargetId));
                return true;
            }

            // Güvenlik: Eğer agent'ın sınıfında özel sohbet kilitliyse engelle
            const AgentRecord senderRec = OkidiaDatabase::instance().agent(m_ctx->registry->client(senderId).machineId);
            if (!senderRec.classId.isEmpty()) {
                const ChatLockRecord lockState = OkidiaDatabase::instance().chatLockState(senderRec.classId);
                if (lockState.privateLocked) {
                    Logger::warning(QObject::tr("Broker: Özel sohbet kilitli, istek/mesaj engellendi → ") + senderId);
                    return true;
                }
            }
        }

        const QString targetId = QString::fromStdString(env.target_id());
        if (!targetId.isEmpty()) {
            if (targetId.startsWith("master:") || targetId == "master") {
                const QString targetUser = targetId.startsWith("master:") ? targetId.mid(7) : "";
                bool routed = false;
                if (!targetUser.isEmpty()) {
                    for (Connection* mConn : m_ctx->registry->masterConnections()) {
                        if (mConn != conn && (m_ctx->registry->authenticatedUsername(mConn) == targetUser ||
                                             m_ctx->registry->authenticatedUserId(mConn) == targetUser)) {
                            okidia::Envelope fwd = env;
                            fwd.set_sequence_id(qHash(senderId));
                            fwd.set_target_id(senderId.toStdString());
                            mConn->sendEnvelope(fwd);
                            routed = true;
                            break;
                        }
                    }
                }
                if (!routed) {
                    if (!targetUser.isEmpty()) {
                        okidia::Envelope fwd = env;
                        fwd.set_sequence_id(qHash(senderId));
                        fwd.set_target_id(senderId.toStdString());
                        QByteArray sData(fwd.SerializeAsString().data(), fwd.ByteSizeLong());
                        OkidiaDatabase::instance().addPendingCommand(targetId, fwd.type(), sData, 86400 * 7);
                    } else {
                        m_ctx->registry->forwardToMaster(senderId, env);
                    }
                }
            } else if (m_ctx->registry->hasClient(targetId)) {
                m_ctx->registry->forwardToClient(targetId, env);
            } else {
                m_ctx->registry->forwardToMaster(senderId, env);
            }
        } else {
            m_ctx->registry->forwardToMaster(senderId, env);
        }
        return true;
    }

    // ── CHAT_WINDOW_STATE (Agent'tan gelen durum bildirimi) ──────────────────
    if (env.type() == okidia::MessageType::CHAT_WINDOW_STATE) {
        QString senderClientId;
        for (const auto& c : m_ctx->registry->allClients()) {
            if (c.connection == conn) {
                senderClientId = c.clientId;
                break;
            }
        }

        // Master'a ilet
        m_ctx->registry->forwardToMaster(senderClientId, env);

        okidia::ChatWindowState ws;
        if (ws.ParseFromString(env.payload())) {
            (*m_ctx->agentWindowOpenMap)[senderClientId] = ws.open();

            const AgentRecord senderRec = OkidiaDatabase::instance().agent(m_ctx->registry->client(senderClientId).machineId);
            if (!senderRec.classId.isEmpty()) {
                for (const auto& c : m_ctx->registry->allClients()) {
                    if (c.clientId != senderClientId) {
                        const AgentRecord peerRec = OkidiaDatabase::instance().agent(c.machineId);
                        if (peerRec.classId == senderRec.classId) {
                            okidia::Envelope peerEnv = env;
                            peerEnv.set_target_id(senderClientId.toStdString());
                            m_ctx->registry->forwardToClient(c.clientId, peerEnv);
                        }
                    }
                }
            }
        }
        return true;
    }

    // ── CHAT_MESSAGE (Master veya Agent) ────────────────────────────────────
    if (env.type() == okidia::MessageType::CHAT_MESSAGE) {
        const bool senderIsMaster = (isMaster && *isMaster) || m_ctx->registry->isMasterConnection(conn);

        if (senderIsMaster) {
            okidia::ChatMessage cm;
            if (cm.ParseFromString(env.payload())) {
                const QString roomId = QString::fromStdString(cm.room_id());

                // OM Yetki kontrolü: Kendisine atanmamış ve aktif üye/misafir olmadığı sınıfa mesaj atamaz
                const QString senderRole = m_ctx->registry->authenticatedRole(conn);
                const QString senderUserId = m_ctx->registry->authenticatedUserId(conn);
                const QString senderUsername = m_ctx->registry->authenticatedUsername(conn);
                const QString senderMasterId = "master:" + senderUsername;
                if (senderRole == "OM" && roomId != "room_admin" && roomId.startsWith("room_")) {
                    const QString targetClassId = roomId.mid(5);
                    const bool isAssigned = OkidiaDatabase::instance().classIdsForUser(senderUserId).contains(targetClassId);
                    const bool isActiveMember = (*m_ctx->activeRoomMembers).contains(roomId) &&
                        ((*m_ctx->activeRoomMembers)[roomId].contains(senderMasterId) ||
                         (*m_ctx->activeRoomMembers)[roomId].contains(senderUserId) ||
                         (*m_ctx->activeRoomMembers)[roomId].contains(senderUsername));
                    if (!isAssigned && !isActiveMember) {
                        Logger::warning(QObject::tr("Broker: Yetkisiz sohbet mesajı engellendi! Kullanıcı: ") +
                                        senderUserId + QObject::tr(" Sınıf: ") + targetClassId);
                        return true;
                    }
                }

                // 1. Diğer tüm Master bağlantılarına ilet
                for (Connection* mConn : m_ctx->registry->masterConnections()) {
                    if (mConn != conn) {
                        mConn->sendEnvelope(env);
                    }
                }

                // 2. İlgili Agent'lara ilet (Öğretmenler Odası hariç)
                if (roomId != "room_admin") {
                    QString targetClassId;
                    if (roomId.startsWith("room_")) {
                        targetClassId = roomId.mid(5);
                    }
                    for (const auto& c : m_ctx->registry->allClients()) {
                        const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                        if (targetClassId.isEmpty() || rec.classId == targetClassId) {
                            m_ctx->registry->forwardToClient(c.clientId, env);
                        }
                    }
                }

                // 3. chatBatch'e ekle — 5sn'de bir DB'ye yazılır
                (*m_ctx->chatBatch).append({
                    OkidiaUtils::generateUuid(),
                    roomId,
                    QString::fromStdString(cm.from()),
                    static_cast<int>(cm.sender_role()),
                    QString::fromStdString(cm.content()),
                    cm.timestamp() > 0
                        ? static_cast<qint64>(cm.timestamp())
                        : OkidiaUtils::nowSecs(),
                    cm.msg_type() == okidia::CHAT_MSG_SYSTEM
                });
            }
            return true;
        } else {
            // Agent'tan gelen mesaj
            QString senderClientId = m_ctx->registry->clientIdByConnection(conn);
            if (senderClientId.isEmpty()) {
                for (const auto& c : m_ctx->registry->allClients()) {
                    if (c.connection == conn) {
                        senderClientId = c.clientId;
                        break;
                    }
                }
            }
            if (senderClientId.isEmpty()) return true;

            okidia::ChatMessage cm;
            if (!cm.ParseFromString(env.payload())) return true;

            const RegisteredClient senderClient = m_ctx->registry->client(senderClientId);
            const QString machineId = senderClient.machineId.isEmpty() ? senderClientId : senderClient.machineId;
            const AgentRecord senderRec = OkidiaDatabase::instance().agent(machineId);

            // 1. Güvenlik: Oda yetkisi doğrulaması (room_admin ve yabancı sınıflar kesin reddedilir)
            const QString roomId = QString::fromStdString(cm.room_id());
            if (roomId == "room_admin" || senderRec.classId.isEmpty()) {
                Logger::warning(QObject::tr("Broker: Yetkisiz/geçersiz oda mesajı engellendi → %1 (%2)")
                                .arg(senderClientId, roomId));
                return true;
            }

            QString targetClassId = roomId.startsWith("room_") ? roomId.mid(5) : senderRec.classId;
            const ClassRecord senderClassRec = OkidiaDatabase::instance().classById(senderRec.classId);
            const bool roomAllowed = (targetClassId == senderRec.classId)
                || (!senderClassRec.parentId.isEmpty() && targetClassId == senderClassRec.parentId);
            if (!roomAllowed) {
                Logger::warning(QObject::tr("Broker: Yabancı sınıfa mesaj engellendi → %1 (hedef: %2, sınıf: %3)")
                                .arg(senderClientId, targetClassId, senderRec.classId));
                return true;
            }

            // 2. Güvenlik: Sınıf sohbeti kilitli ise Master dahil HİÇBİR YERE iletme
            const ChatLockRecord targetLock = OkidiaDatabase::instance().chatLockState(targetClassId);
            const ChatLockRecord ownLock = (targetClassId != senderRec.classId)
                ? OkidiaDatabase::instance().chatLockState(senderRec.classId)
                : ChatLockRecord{};
            if (targetLock.chatLocked || ownLock.chatLocked) {
                Logger::warning(QObject::tr("Broker: Sohbet kilitli, agent mesajı engellendi → ") + senderClientId + " (" + roomId + ")");
                return true;
            }

            // 3. Kullanıcı bazlı (<machineId>:<username>) spam & susturma kontrolü
            const qint64 now = OkidiaUtils::nowSecs();
            const QString userKey = makeUserKey(machineId, senderClient.username);
            auto& tracker = m_spamTrackers[userKey];

            if (tracker.mutedUntil > now) {
                Logger::warning(QObject::tr("Broker: Susturulmuş kullanıcı mesajı engellendi → %1 (%2 sn kaldı)")
                                .arg(userKey).arg(tracker.mutedUntil - now));
                sendMuteState(senderClientId, senderClient.username, true, tracker.mutedUntil,
                              static_cast<int>(tracker.mutedUntil - now), tracker.violationLevel, tracker.muteReason);
                return true;
            }

            // 4. İçerik doğrulama: boş mesaj, uzunluk (500) ve satır sayısı (5) sınırı
            QString content = QString::fromStdString(cm.content()).trimmed();
            if (content.isEmpty()) return true;
            if (content.size() > kMaxMessageLength) {
                content = content.left(kMaxMessageLength);
            }
            {
                QStringList lines = content.split('\n');
                if (lines.size() > kMaxMessageNewlines + 1) {
                    lines = lines.mid(0, kMaxMessageNewlines + 1);
                    content = lines.join('\n').trimmed();
                }
            }
            if (content.isEmpty()) return true;

            // 5. Kayan pencere + ardışık özdeş mesaj spam tespiti
            while (!tracker.recentTimestamps.isEmpty() && (now - tracker.recentTimestamps.first()) > kSpamWindowSecs) {
                tracker.recentTimestamps.removeFirst();
            }

            const QString normMsg = content.simplified().toLower();
            if (!normMsg.isEmpty() && normMsg == tracker.lastNormalizedMsg && (now - tracker.lastSameMsgAt) <= kSpamIdenticalWindow) {
                tracker.consecutiveSameCount++;
            } else {
                tracker.lastNormalizedMsg = normMsg;
                tracker.consecutiveSameCount = 1;
            }
            tracker.lastSameMsgAt = now;
            tracker.recentTimestamps.append(now);

            const bool floodTriggered = (tracker.recentTimestamps.size() >= kSpamMaxMsgsInWindow);
            const bool repeatTriggered = (tracker.consecutiveSameCount >= kSpamIdenticalMax);

            if (floodTriggered || repeatTriggered) {
                if (tracker.lastViolationAt > 0 && (now - tracker.lastViolationAt) >= kViolationDecaySecs) {
                    tracker.violationLevel = 0;
                }
                tracker.violationLevel = qMin(tracker.violationLevel + 1, 3);
                const int durSecs = muteDurationForLevel(tracker.violationLevel);
                tracker.mutedUntil = now + durSecs;
                tracker.lastViolationAt = now;
                tracker.recentTimestamps.clear();
                tracker.consecutiveSameCount = 0;
                tracker.muteReason = QObject::tr("Ardışık sık mesaj nedeniyle %1 dakika sohbet kısıtlaması uygulandı.")
                                     .arg(durSecs / 60);

                Logger::warning(QObject::tr("Broker: Spam tespit edildi → %1 | Seviye: %2 | Süre: %3 dk")
                                .arg(userKey).arg(tracker.violationLevel).arg(durSecs / 60));

                sendMuteState(senderClientId, senderClient.username, true, tracker.mutedUntil,
                              durSecs, tracker.violationLevel, tracker.muteReason);
                return true;
            }

            // 6. Gönderen bilgisini ve rolü otoriter olarak yeniden yaz ("Görünen İsim - hostname")
            const QString authDisplay = formatStudentDisplayName(
                senderClient.displayName,
                senderClient.username,
                senderClient.hostname.isEmpty() ? senderRec.hostname : senderClient.hostname,
                senderClientId);

            cm.set_room_id(("room_" + targetClassId).toStdString());
            cm.set_from(authDisplay.toStdString());
            cm.set_content(content.toStdString());
            cm.set_sender_role(okidia::SenderRole::ROLE_CLIENT);
            cm.set_msg_type(okidia::CHAT_MSG_TEXT);
            cm.set_timestamp(static_cast<uint64_t>(now));

            okidia::Envelope outEnv = env;
            outEnv.set_payload(cm.SerializeAsString());

            // Master'a ilet
            m_ctx->registry->forwardToMaster(senderClientId, outEnv);

            // Aynı sınıftaki/gruptaki diğer agent'lara da ilet
            for (const auto& c : m_ctx->registry->allClients()) {
                if (c.clientId != senderClientId) {
                    const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                    bool sameRoom = (rec.classId == targetClassId);
                    if (!sameRoom && !rec.classId.isEmpty()) {
                        const ClassRecord peerCr = OkidiaDatabase::instance().classById(rec.classId);
                        sameRoom = (peerCr.parentId == targetClassId);
                    }
                    if (sameRoom) {
                        m_ctx->registry->forwardToClient(c.clientId, outEnv);
                    }
                }
            }

            (*m_ctx->chatBatch).append({
                OkidiaUtils::generateUuid(),
                "room_" + targetClassId,
                authDisplay,
                static_cast<int>(okidia::SenderRole::ROLE_CLIENT),
                content,
                now,
                false
            });
            return true;
        }
    }

    return false;
}
