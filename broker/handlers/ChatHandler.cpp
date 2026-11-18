#include "ChatHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/OkidiaAuditDatabase.h"
#include "../../common/db/OkidiaChatDatabase.h"
#include "../../common/utils/OkidiaUtils.h"

ChatHandler::ChatHandler(BrokerContext* ctx) : m_ctx(ctx) {}

bool ChatHandler::handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster) {
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
            QString senderClientId;
            for (const auto& c : m_ctx->registry->allClients()) {
                if (c.connection == conn) {
                    senderClientId = c.clientId;
                    break;
                }
            }

            // Önce Master'a ilet
            m_ctx->registry->forwardToMaster(senderClientId, env);

            okidia::ChatMessage cm;
            if (cm.ParseFromString(env.payload())) {
                const QString roomId = QString::fromStdString(cm.room_id());
                QString targetClassId;
                if (roomId.startsWith("room_")) {
                    targetClassId = roomId.mid(5);
                } else {
                    const AgentRecord senderRec = OkidiaDatabase::instance().agent(m_ctx->registry->client(senderClientId).machineId);
                    targetClassId = senderRec.classId;
                }

                // Güvenlik: Eğer sınıfın genel sohbeti kilitli ise mesajı engelle
                if (!targetClassId.isEmpty()) {
                    const ChatLockRecord lockState = OkidiaDatabase::instance().chatLockState(targetClassId);
                    if (lockState.chatLocked) {
                        Logger::warning(QObject::tr("Broker: Sohbet kilitli, agent mesajı engellendi → ") + senderClientId + " (" + roomId + ")");
                        return true;
                    }
                }

                // Aynı sınıftaki diğer agent'lara da ilet
                for (const auto& c : m_ctx->registry->allClients()) {
                    if (c.clientId != senderClientId) {
                        const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                        if (!targetClassId.isEmpty() && rec.classId == targetClassId) {
                            m_ctx->registry->forwardToClient(c.clientId, env);
                        }
                    }
                }

                (*m_ctx->chatBatch).append({
                    OkidiaUtils::generateUuid(),
                    roomId,
                    senderClientId,
                    static_cast<int>(cm.sender_role()),
                    QString::fromStdString(cm.content()),
                    cm.timestamp() > 0
                        ? static_cast<qint64>(cm.timestamp())
                        : OkidiaUtils::nowSecs(),
                    cm.msg_type() == okidia::CHAT_MSG_SYSTEM
                });
            }
            return true;
        }
    }

    return false;
}
