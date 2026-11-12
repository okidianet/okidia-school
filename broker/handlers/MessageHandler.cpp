#include "MessageHandler.h"
#include "AuthHandler.h"
#include "ClientHandler.h"
#include "ClassHandler.h"
#include "ChatHandler.h"
#include "SystemHandler.h"
#include "../BellScheduler.h"

#include "../../common/utils/Logger.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/OkidiaAuditDatabase.h"
#include "../../common/crypto/AesGcm.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QSet>

MessageHandler::MessageHandler(BrokerContext* ctx)
    : m_ctx(ctx)
{
    // Yardımcı lambda'ları BrokerContext'e bağla
    m_ctx->broadcastDbSyncSignal = [this]() {
        okidia::Envelope syncEnv;
        syncEnv.set_type(okidia::MessageType::REFRESH_REQUEST);
        syncEnv.set_target_id("__sync_masters__");
        for (Connection* mConn : m_ctx->registry->masterConnections()) {
            mConn->sendEnvelope(syncEnv);
        }
        Logger::info(QObject::tr("Broker: Tüm Master'lara DB eşitleme sinyali gönderildi"));
    };

    m_ctx->isRequesterYM = [this](Connection* c) -> bool {
        QString rId = m_ctx->registry->authenticatedUserId(c);
        if (rId.isEmpty()) return false;
        UserRecord reqUser = OkidiaDatabase::instance().userById(rId);
        return (reqUser.role == "YM");
    };

    m_ctx->isRequesterYMOrOM = [this](Connection* c) -> bool {
        QString rId = m_ctx->registry->authenticatedUserId(c);
        if (rId.isEmpty()) return false;
        UserRecord reqUser = OkidiaDatabase::instance().userById(rId);
        return (reqUser.role == "YM" || reqUser.role == "OM");
    };

    m_ctx->broadcastAgentLeftRoom = [this](const QString& machineId, const QString& classId) {
        if (classId.isEmpty()) return;
        const QString roomKey = "room_" + classId;
        QString clientId;
        Connection* agentConn = nullptr;
        for (const auto& c : m_ctx->registry->allClients()) {
            if (c.machineId == machineId) {
                clientId = c.clientId;
                agentConn = c.connection;
                break;
            }
        }
        if (clientId.isEmpty()) return;

        (*m_ctx->activeRoomMembers)[roomKey].remove(clientId);
        if (agentConn && (*m_ctx->connToRoomMembers).contains(agentConn)) {
            (*m_ctx->connToRoomMembers)[agentConn].remove({roomKey, clientId});
        }

        okidia::ChatMemberEvent leaveEvt;
        leaveEvt.set_room_id(roomKey.toStdString());
        leaveEvt.set_client_id(clientId.toStdString());
        leaveEvt.set_hostname("AC");
        leaveEvt.set_username("AC");
        okidia::Envelope leaveEnv;
        leaveEnv.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
        leaveEnv.set_payload(leaveEvt.SerializeAsString());

        m_ctx->registry->forwardToClient(clientId, leaveEnv);
        for (const auto& otherC : m_ctx->registry->allClients()) {
            if (otherC.clientId != clientId) {
                const AgentRecord otherRec = OkidiaDatabase::instance().agent(otherC.machineId);
                if (otherRec.classId == classId) {
                    m_ctx->registry->forwardToClient(otherC.clientId, leaveEnv);
                }
            }
        }
        for (Connection* mConn : m_ctx->registry->masterConnections()) {
            mConn->sendEnvelope(leaveEnv);
        }
    };

    m_ctx->broadcastAgentJoinedRoom = [this](const QString& machineId, const QString& classId) {
        if (classId.isEmpty()) return;
        AgentRecord rec = OkidiaDatabase::instance().agent(machineId);
        if (rec.machineId.isEmpty()) return;

        QString clientId;
        Connection* conn = nullptr;
        for (const auto& c : m_ctx->registry->allClients()) {
            if (c.machineId == machineId) {
                clientId = c.clientId;
                conn = c.connection;
                break;
            }
        }
        if (clientId.isEmpty() || !conn) return;

        ClassRecord clsRec = OkidiaDatabase::instance().classById(classId);
        if (clsRec.id.isEmpty()) return;

        struct TargetRoom {
            QString roomId;
            QString roomName;
            QString parentRoomId;
            QString classIdForPermission;
            QSet<QString> memberClassIds;
        };
        QList<TargetRoom> targetRooms;

        if (!clsRec.parentId.isEmpty()) {
            ClassRecord parentCls = OkidiaDatabase::instance().classById(clsRec.parentId);
            TargetRoom parentRoom;
            parentRoom.roomId = "room_" + clsRec.parentId;
            parentRoom.roomName = parentCls.name.isEmpty() ? QObject::tr("Sınıf Sohbeti") : parentCls.name;
            parentRoom.parentRoomId = "";
            parentRoom.classIdForPermission = clsRec.parentId;
            parentRoom.memberClassIds.insert(clsRec.parentId);
            for (const ClassRecord& c : OkidiaDatabase::instance().allClasses()) {
                if (c.parentId == clsRec.parentId) {
                    parentRoom.memberClassIds.insert(c.id);
                }
            }
            targetRooms.append(parentRoom);

            TargetRoom groupRoom;
            groupRoom.roomId = "room_" + classId;
            groupRoom.roomName = clsRec.name.isEmpty() ? QObject::tr("Grup Sohbeti") : clsRec.name;
            groupRoom.parentRoomId = "room_" + clsRec.parentId;
            groupRoom.classIdForPermission = clsRec.parentId;
            groupRoom.memberClassIds.insert(classId);
            targetRooms.append(groupRoom);
        } else {
            TargetRoom classRoom;
            classRoom.roomId = "room_" + classId;
            classRoom.roomName = clsRec.name.isEmpty() ? QObject::tr("Sınıf Sohbeti") : clsRec.name;
            classRoom.parentRoomId = "";
            classRoom.classIdForPermission = classId;
            classRoom.memberClassIds.insert(classId);
            for (const ClassRecord& c : OkidiaDatabase::instance().allClasses()) {
                if (c.parentId == classId) {
                    classRoom.memberClassIds.insert(c.id);
                }
            }
            targetRooms.append(classRoom);
        }

        const QList<UserRecord> allDbUsers = OkidiaDatabase::instance().allUsers();
        const QList<AgentRecord> allAgs = OkidiaDatabase::instance().allAgents();
        const QString thisDisp = rec.hostname.isEmpty() ? clientId : rec.hostname;

        for (const auto& tr : targetRooms) {
            okidia::ChatRoomCreated roomProto;
            roomProto.set_room_id(tr.roomId.toStdString());
            roomProto.set_room_name(tr.roomName.toStdString());
            roomProto.set_parent_room_id(tr.parentRoomId.toStdString());

            QSet<QString> addedMasterIds;
            for (const auto& u : allDbUsers) {
                if (u.role == "YM" || (u.role == "OM" && OkidiaDatabase::instance().classIdsForUser(u.id).contains(tr.classIdForPermission))) {
                    const QString mId = "master:" + u.username;
                    if (!addedMasterIds.contains(mId)) {
                        addedMasterIds.insert(mId);
                        auto* mb = roomProto.add_members();
                        mb->set_client_id(mId.toStdString());
                        mb->set_hostname(u.displayName.isEmpty() ? u.username.toStdString() : u.displayName.toStdString());
                        mb->set_username(u.role.toStdString());
                    }
                }
            }

            for (const AgentRecord& ag : allAgs) {
                if (!tr.memberClassIds.contains(ag.classId)) continue;
                auto* mb = roomProto.add_members();
                mb->set_client_id(ag.machineId.toStdString());
                const QString disp = ag.hostname.isEmpty() ? ag.machineId : ag.hostname;
                mb->set_hostname(disp.toStdString());
                mb->set_username("AC");
            }

            okidia::Envelope roomEnv;
            roomEnv.set_type(okidia::MessageType::CHAT_ROOM_CREATED);
            roomEnv.set_payload(roomProto.SerializeAsString());
            conn->sendEnvelope(roomEnv);

            if (m_ctx->activeRoomMembers && m_ctx->activeRoomMembers->contains(tr.roomId)) {
                for (const okidia::ChatMemberEvent& evt : (*m_ctx->activeRoomMembers)[tr.roomId]) {
                    okidia::Envelope joinedEnv;
                    joinedEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
                    joinedEnv.set_payload(evt.SerializeAsString());
                    conn->sendEnvelope(joinedEnv);
                }
            }

            okidia::ChatMemberEvent thisAgentJoinEvt;
            thisAgentJoinEvt.set_room_id(tr.roomId.toStdString());
            thisAgentJoinEvt.set_client_id(clientId.toStdString());
            thisAgentJoinEvt.set_hostname(thisDisp.toStdString());
            thisAgentJoinEvt.set_username("AC");

            if (m_ctx->activeRoomMembers) {
                (*m_ctx->activeRoomMembers)[tr.roomId][clientId] = thisAgentJoinEvt;
            }
            if (m_ctx->connToRoomMembers) {
                (*m_ctx->connToRoomMembers)[conn].insert({tr.roomId, clientId});
            }

            okidia::Envelope thisAgentJoinEnv;
            thisAgentJoinEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
            thisAgentJoinEnv.set_payload(thisAgentJoinEvt.SerializeAsString());

            for (Connection* mConn : m_ctx->registry->masterConnections()) {
                mConn->sendEnvelope(thisAgentJoinEnv);
            }

            for (const auto& c : m_ctx->registry->allClients()) {
                if (c.clientId != clientId) {
                    const AgentRecord otherRec = OkidiaDatabase::instance().agent(c.machineId);
                    if (tr.memberClassIds.contains(otherRec.classId)) {
                        const QString otherDisp = otherRec.hostname.isEmpty() ? c.clientId : otherRec.hostname;
                        okidia::ChatMemberEvent otherJoinEvt;
                        otherJoinEvt.set_room_id(tr.roomId.toStdString());
                        otherJoinEvt.set_client_id(c.clientId.toStdString());
                        otherJoinEvt.set_hostname(otherDisp.toStdString());
                        otherJoinEvt.set_username("AC");

                        okidia::Envelope otherJoinEnv;
                        otherJoinEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
                        otherJoinEnv.set_payload(otherJoinEvt.SerializeAsString());
                        conn->sendEnvelope(otherJoinEnv);
                        m_ctx->registry->forwardToClient(c.clientId, thisAgentJoinEnv);
                    }
                }
            }
        }
    };

    m_ctx->broadcastRoomClosed = [this](const QString& classId) {
        if (classId.isEmpty()) return;
        const QString roomKey = "room_" + classId;

        okidia::ChatMemberEvent leaveEvt;
        leaveEvt.set_room_id(roomKey.toStdString());
        leaveEvt.set_client_id("__room_closed__");
        leaveEvt.set_hostname("SYSTEM");
        leaveEvt.set_username("SYSTEM");
        okidia::Envelope leaveEnv;
        leaveEnv.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
        leaveEnv.set_payload(leaveEvt.SerializeAsString());

        if (m_ctx->activeRoomMembers) {
            m_ctx->activeRoomMembers->remove(roomKey);
        }

        for (const auto& c : m_ctx->registry->allClients()) {
            m_ctx->registry->forwardToClient(c.clientId, leaveEnv);
        }
        for (Connection* mConn : m_ctx->registry->masterConnections()) {
            mConn->sendEnvelope(leaveEnv);
        }
    };

    m_authHandler = std::make_unique<AuthHandler>(m_ctx);
    m_clientHandler = std::make_unique<ClientHandler>(m_ctx);
    m_classHandler = std::make_unique<ClassHandler>(m_ctx);
    m_chatHandler = std::make_unique<ChatHandler>(m_ctx);
    m_systemHandler = std::make_unique<SystemHandler>(m_ctx);
}

MessageHandler::~MessageHandler() = default;

bool MessageHandler::isAllowedAgentMessageType(okidia::MessageType type) const {
    switch (type) {
        case okidia::MessageType::CLIENT_INFO:
        case okidia::MessageType::HEARTBEAT:
        case okidia::MessageType::SCREEN_FRAME:
        case okidia::MessageType::SCREEN_TILE_UPDATE:
        case okidia::MessageType::SCREEN_CAPTURE_RESULT:
        case okidia::MessageType::SCREEN_INFO_RESPONSE:
        case okidia::MessageType::REMOTE_FRAME:
        case okidia::MessageType::REMOTE_CONTROL_RESPONSE:
        case okidia::MessageType::COMMAND_RESULT:
        case okidia::MessageType::SYSTEM_METRICS_REPORT:
        case okidia::MessageType::REPORT_EVENT:
        case okidia::MessageType::APP_LIST_RESPONSE:
        case okidia::MessageType::HAND_RAISE:
        case okidia::MessageType::HAND_LOWER:
        case okidia::MessageType::TIME_SYNC_REQUEST:
        case okidia::MessageType::DISCOVERY_REQUEST:
        case okidia::MessageType::ENROLL_REQUEST:
        case okidia::MessageType::CHAT_MESSAGE:
        case okidia::MessageType::PRIVATE_CHAT_REQUEST:
        case okidia::MessageType::PRIVATE_CHAT_ACCEPT:
        case okidia::MessageType::PRIVATE_CHAT_REJECT:
        case okidia::MessageType::PRIVATE_CHAT_MESSAGE:
        case okidia::MessageType::PRIVATE_CHAT_CLOSE:
        case okidia::MessageType::CHAT_WINDOW_STATE:
        case okidia::MessageType::AGENT_TYPE_SET_RESULT:
        case okidia::MessageType::AGENT_STATUS:
        case okidia::MessageType::CURSOR_SHAPE_CHANGE:
        case okidia::MessageType::LOG_TAIL_RES:
        case okidia::MessageType::TEACHER_KEY_SYNC_RES:
        case okidia::MessageType::SMARTBOARD_LOCK_PIN_REPORT:
        case okidia::MessageType::SMARTBOARD_UNLOCK_NOTIFY:
        case okidia::MessageType::ROLLBACK_TO_STABLE_RES:
        case okidia::MessageType::QUERY_LOCAL_USERS_RES:
        case okidia::MessageType::FILE_TRANSFER_CHUNK:
        case okidia::MessageType::FILE_TRANSFER_END:
        case okidia::MessageType::OTA_UPDATE_INSTALL:
            return true;
        default:
            return false;
    }
}

bool MessageHandler::isAuthorizedForMasterMessage(const okidia::Envelope& env, Connection* conn, const QString& role) {
    const okidia::MessageType type = env.type();

    // 1. Sadece YM (Yönetici Master) yetkisine açık kritik yönetim komutları
    static const QSet<okidia::MessageType> ymOnlyTypes = {
        okidia::MessageType::CLIENT_REVOKE,
        okidia::MessageType::CLIENT_UNREVOKE,
        okidia::MessageType::BROKER_PORT_SET_REQ,
        okidia::MessageType::BROKER_CONFIG_SET_REQ,
        okidia::MessageType::BROKER_UPDATES_DELETE_REQ,
        okidia::MessageType::BROKER_OTA_UPDATE_BEGIN,
        okidia::MessageType::BROKER_OTA_UPDATE_CHUNK,
        okidia::MessageType::BROKER_OTA_UPDATE_END,
        okidia::MessageType::BROKER_OTA_UPDATE_INSTALL,
        okidia::MessageType::AGENT_UNINSTALL_PASS_SET,
        okidia::MessageType::DB_RESTORE_REQUEST,
        okidia::MessageType::DB_USER_ADD_REQ,
        okidia::MessageType::DB_USER_DEL_REQ,
        okidia::MessageType::DB_USER_UPDATE_REQ,
        okidia::MessageType::DB_USER_SET_REPLICA_REQ,
        okidia::MessageType::DB_USER_GRANT_REQ,
        okidia::MessageType::DB_USER_REVOKE_REQ,
        okidia::MessageType::DB_TEACHER_UPSERT_REQ,
        okidia::MessageType::DB_TEACHER_DEL_REQ,
        okidia::MessageType::JOIN_CODE_REQUEST,
        okidia::MessageType::AUDIT_LOG_QUERY_REQ,
        okidia::MessageType::BROKER_SETTINGS_UPDATE,
        okidia::MessageType::CUSTOM_PROFILE_UPSERT_REQ,
        okidia::MessageType::CUSTOM_PROFILE_DELETE_REQ
    };

    if (ymOnlyTypes.contains(type)) {
        return (role == "YM");
    }

    // 2. YM ve OM (Öğretmen Master) için açık operasyonel komutlar
    // Gözlemci (Observer) veya yetkisiz roller bu komutları çalıştıramaz
    static const QSet<okidia::MessageType> ymOrOmTypes = {
        okidia::MessageType::SCREEN_LOCK,
        okidia::MessageType::SCREEN_UNLOCK,
        okidia::MessageType::INPUT_LOCK,
        okidia::MessageType::INPUT_UNLOCK,
        okidia::MessageType::MOUSE_EVENT,
        okidia::MessageType::KEY_EVENT,
        okidia::MessageType::INPUT_TAKE_CONTROL,
        okidia::MessageType::REMOTE_CONTROL_STOP,
        okidia::MessageType::REMOTE_CONTROL_REQUEST,
        okidia::MessageType::COMMAND_EXEC,
        okidia::MessageType::POWER_SHUTDOWN,
        okidia::MessageType::POWER_RESTART,
        okidia::MessageType::POWER_LOGOUT,
        okidia::MessageType::POWER_HIBERNATE,
        okidia::MessageType::POWER_WAKE_ON_LAN,
        okidia::MessageType::FILE_TRANSFER_BEGIN,
        okidia::MessageType::FILE_TRANSFER_CHUNK,
        okidia::MessageType::FILE_TRANSFER_END,
        okidia::MessageType::AUDIO_BROADCAST,
        okidia::MessageType::AUDIO_STOP,
        okidia::MessageType::AUDIO_MUTE,
        okidia::MessageType::AUDIO_UNMUTE,
        okidia::MessageType::AUDIO_SET_VOLUME,
        okidia::MessageType::AUDIO_BROADCAST_LIVE,
        okidia::MessageType::AUDIO_BROADCAST_FILE,
        okidia::MessageType::AUDIO_BROADCAST_STOP,
        okidia::MessageType::AUDIO_UNMUTE_ONLY,
        okidia::MessageType::AUDIO_PLAY_LOCAL,
        okidia::MessageType::EXAM_START,
        okidia::MessageType::EXAM_COLLECT,
        okidia::MessageType::EXAM_END,
        okidia::MessageType::MSG_POPUP,
        okidia::MessageType::MSG_BROADCAST,
        okidia::MessageType::APP_OPEN,
        okidia::MessageType::APP_CLOSE,
        okidia::MessageType::APP_BLOCK_BYNAME,
        okidia::MessageType::APP_BLOCK_BYHASH,
        okidia::MessageType::APP_WHITELIST_SET,
        okidia::MessageType::APP_UNBLOCK,
        okidia::MessageType::BROADCAST_BEGIN,
        okidia::MessageType::BROADCAST_FRAME,
        okidia::MessageType::BROADCAST_END,
        okidia::MessageType::WEB_BLOCK_ALL,
        okidia::MessageType::WEB_UNBLOCK,
        okidia::MessageType::SCHEDULE_COMMAND,
        okidia::MessageType::DISCOVERY_APPROVE,
        okidia::MessageType::DISCOVERY_REJECT,
        okidia::MessageType::DISCOVERY_IGNORE,
        okidia::MessageType::DB_CLASS_ADD_REQ,
        okidia::MessageType::DB_CLASS_DEL_REQ,
        okidia::MessageType::DB_CLASS_RENAME_REQ,
        okidia::MessageType::DB_AGENT_ASSIGN_REQ,
        okidia::MessageType::ROLLBACK_TO_STABLE_REQ,
        okidia::MessageType::AGENT_TYPE_SET,
        okidia::MessageType::AGENT_ALIAS_SET,
        okidia::MessageType::PRIVATE_CHAT_OPEN,
        okidia::MessageType::CHAT_LOCK_STATE,
        okidia::MessageType::ROOM_JOIN_REQUEST,
        okidia::MessageType::ROOM_JOIN_RESPONSE,
        okidia::MessageType::QUERY_LOCAL_USERS_REQ
    };

    if (ymOrOmTypes.contains(type)) {
        return (role == "YM" || role == "OM");
    }

    // Diğer bilgi alma, canlı izleme ve genel mesajlar (tüm yetkili rollere açık)
    return true;
}

bool MessageHandler::checkAuthorization(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool isMaster) {
    const okidia::MessageType type = env.type();
    const QString peerIp = conn->peerAddress();

    // ── KURAL 1: OTURUM ÖNCESİ (PRE-AUTH) MESAJLAR ─────────────────────────────
    // Bu mesajlar el sıkışma, ilk kurulum, şifre denetimi veya keşif süreçlerine aittir.
    static const QSet<okidia::MessageType> preAuthAllowedTypes = {
        okidia::MessageType::CLIENT_INFO,
        okidia::MessageType::LOGIN_REQUEST,
        okidia::MessageType::SETUP_REQUEST,
        okidia::MessageType::VERIFY_SETUP_CODE_REQUEST,
        okidia::MessageType::TIME_SYNC_REQUEST,
        okidia::MessageType::DISCOVERY_REQUEST,
        okidia::MessageType::ENROLL_REQUEST,
        okidia::MessageType::PASSWORD_RESET_REQ,
        okidia::MessageType::JOIN_REQUEST
    };

    if (preAuthAllowedTypes.contains(type)) {
        return true;
    }

    // ── KURAL 2: MASTER SOKETİ VE OTURUM KONTROLÜ ──────────────────────────────
    if (isMaster) {
        // Master olduğunu iddia eden veya soketi Master olarak işaretlenmiş bağlantı:
        QString authenticatedUserId = m_ctx->registry->authenticatedUserId(conn);
        if (authenticatedUserId.isEmpty()) {
            // Eğer soket oturum açmamışsa (login olmamışsa), komut çalıştırması kesinlikle yasaktır!
            Logger::warning(QObject::tr("Broker GÜVENLİK: Oturum açmamış Master'dan yetkisiz mesaj engellendi!")
                            + " IP: " + peerIp + " Tip: " + QString::number(type) + " PeerId: " + peerId);

            OkidiaAuditDatabase::instance().logCommand(
                peerIp,
                peerId.isEmpty() ? QStringLiteral("Unknown") : peerId,
                QObject::tr("Oturumsuz Master bağlantısından yetkisiz komut denemesi engellendi (Tip: %1)").arg(static_cast<int>(type)),
                QStringLiteral("Security")
            );

            // Oturumu henüz tamamlanmamış veya login aşamasındaki Master'dan gelen komutları sessizce engelle/düşür.
            // ASLA SESSION_RESTRICT_MODE gönderme: Çünkü SESSION_RESTRICT_MODE yalnızca aynı hesaptan başka bir
            // cihazda oturum açıldığında önceki oturumu kısıtlamak için ayrılmıştır.
            return false;
        }

        // Oturum açılmış; kullanıcının rolünü al
        QString role = m_ctx->registry->authenticatedRole(conn);
        if (role.isEmpty()) {
            UserRecord u = OkidiaDatabase::instance().userById(authenticatedUserId);
            role = u.role;
        }

        if (!isAuthorizedForMasterMessage(env, conn, role)) {
            Logger::warning(QObject::tr("Broker GÜVENLİK: Rol yetkisi yetersiz! Kullanıcı: %1 (%2) Tip: %3")
                            .arg(authenticatedUserId, role, QString::number(type)));

            OkidiaAuditDatabase::instance().logCommand(
                peerIp,
                authenticatedUserId,
                QObject::tr("Yetersiz rol yetkisiyle komut çalıştırma denemesi engellendi (Rol: %1, Tip: %2)").arg(role).arg(static_cast<int>(type)),
                QStringLiteral("Security"),
                authenticatedUserId
            );
            return false;
        }

        return true;
    }

    // ── KURAL 3: AGENT SOKETİ KONTROLÜ VE SPOOFING KORUMASI ───────────────────
    // Soket bir Master değil, yani bir Agent (istemci) soketidir.
    if (!isAllowedAgentMessageType(type)) {
        // Bir Agent asla yönetimsel Master komutları (örn. SCREEN_LOCK, POWER_SHUTDOWN, COMMAND_EXEC) basamaz!
        Logger::error(QObject::tr("Broker KRİTİK GÜVENLİK ALARMI: Agent soketinden yetkisiz Master komutu tespit edildi! Bağlantı kesiliyor.")
                      + " IP: " + peerIp + " PeerId: " + peerId + " Tip: " + QString::number(type));

        OkidiaAuditDatabase::instance().logCommand(
            peerIp,
            peerId.isEmpty() ? QStringLiteral("UnknownAgent") : peerId,
            QObject::tr("Agent soketinden yetkisiz Master komutu enjeksiyon denemesi tespit edildi ve bağlantı kesildi (Tip: %1)").arg(static_cast<int>(type)),
            QStringLiteral("Security")
        );

        // Kötü niyetli girişim şüphesi ile soketi derhal kes
        conn->disconnect();
        return false;
    }

    // Agent'ın kayıtlı kimliği ile soket eşleşmesi (CN Spoofing Guard)
    if (!peerId.isEmpty()) {
        RegisteredClient regClient = m_ctx->registry->client(peerId);
        if (regClient.connection && regClient.connection != conn) {
            Logger::error(QObject::tr("Broker GÜVENLİK: Agent kimlik sahteciliği (Spoofing) şüphesi! Soket ile kayıtlı istemci uyuşmuyor: ") + peerId);
            OkidiaAuditDatabase::instance().logCommand(
                peerIp,
                peerId,
                QObject::tr("Agent kimlik sahteciliği (Spoofing) denemesi engellendi."),
                QStringLiteral("Security")
            );
            conn->disconnect();
            return false;
        }
    }

    return true;
}

void MessageHandler::process(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster) {
    const bool masterSender = (isMaster && *isMaster) || m_ctx->registry->isMasterConnection(conn);

    // 0. Merkezi Yetki ve Kimlik Doğrulama Katmanı (Authorization & Identity Guard)
    if (!checkAuthorization(env, conn, peerId, masterSender)) {
        return;
    }

    // 1. Modüler alt işleyiciler
    if (m_authHandler->handle(env, conn, peerId, isMaster)) return;
    if (m_clientHandler->handle(env, conn, peerId, isMaster)) return;
    if (m_classHandler->handle(env, conn, peerId, isMaster)) return;
    if (m_chatHandler->handle(env, conn, peerId, isMaster)) return;
    if (m_systemHandler->handle(env, conn, peerId, isMaster)) return;

    // 2. Master komut yönlendirmeleri ve kilit yönetimi
    if (masterSender) {
        const QString targetId = QString::fromStdString(env.target_id());
        const uint64_t seq     = env.sequence_id();

        QString masterUserId = m_ctx->registry->authenticatedUserId(conn);
        QString masterDisplayName = "Master";
        QString masterRole = m_ctx->registry->authenticatedRole(conn);
        if (masterUserId.isEmpty()) {
            masterUserId = peerId;
            masterDisplayName = peerId;
        } else {
            UserRecord u = OkidiaDatabase::instance().userById(masterUserId);
            if (!u.id.isEmpty() && !u.displayName.isEmpty()) {
                masterDisplayName = u.displayName;
            } else if (!u.id.isEmpty() && !u.username.isEmpty()) {
                masterDisplayName = u.username;
            }
            if (masterRole.isEmpty()) {
                masterRole = u.role;
            }
        }

        // OM rolü için istemci bazında yetki denetimi (Yetkisiz sınıftaki istemciye müdahale engeli)
        if (masterRole == "OM" && !targetId.isEmpty() && !targetId.startsWith("__") && !targetId.startsWith("master:")) {
            static const QSet<okidia::MessageType> omRestrictedCommands = {
                okidia::MessageType::SCREEN_LOCK,
                okidia::MessageType::SCREEN_UNLOCK,
                okidia::MessageType::INPUT_LOCK,
                okidia::MessageType::INPUT_UNLOCK,
                okidia::MessageType::MOUSE_EVENT,
                okidia::MessageType::KEY_EVENT,
                okidia::MessageType::INPUT_TAKE_CONTROL,
                okidia::MessageType::REMOTE_CONTROL_STOP,
                okidia::MessageType::REMOTE_CONTROL_REQUEST,
                okidia::MessageType::COMMAND_EXEC,
                okidia::MessageType::POWER_SHUTDOWN,
                okidia::MessageType::POWER_RESTART,
                okidia::MessageType::POWER_LOGOUT,
                okidia::MessageType::POWER_HIBERNATE,
                okidia::MessageType::POWER_WAKE_ON_LAN,
                okidia::MessageType::FILE_TRANSFER_BEGIN,
                okidia::MessageType::FILE_TRANSFER_CHUNK,
                okidia::MessageType::FILE_TRANSFER_END,
                okidia::MessageType::AUDIO_BROADCAST,
                okidia::MessageType::AUDIO_STOP,
                okidia::MessageType::AUDIO_MUTE,
                okidia::MessageType::AUDIO_UNMUTE,
                okidia::MessageType::AUDIO_SET_VOLUME,
                okidia::MessageType::AUDIO_BROADCAST_LIVE,
                okidia::MessageType::AUDIO_BROADCAST_FILE,
                okidia::MessageType::AUDIO_BROADCAST_STOP,
                okidia::MessageType::AUDIO_UNMUTE_ONLY,
                okidia::MessageType::AUDIO_PLAY_LOCAL,
                okidia::MessageType::EXAM_START,
                okidia::MessageType::EXAM_COLLECT,
                okidia::MessageType::EXAM_END,
                okidia::MessageType::MSG_POPUP,
                okidia::MessageType::APP_OPEN,
                okidia::MessageType::APP_CLOSE,
                okidia::MessageType::APP_BLOCK_BYNAME,
                okidia::MessageType::APP_BLOCK_BYHASH,
                okidia::MessageType::APP_WHITELIST_SET,
                okidia::MessageType::APP_UNBLOCK,
                okidia::MessageType::WEB_BLOCK_ALL,
                okidia::MessageType::WEB_UNBLOCK,
                okidia::MessageType::SCHEDULE_COMMAND,
                okidia::MessageType::AGENT_TYPE_SET,
                okidia::MessageType::AGENT_ALIAS_SET,
                okidia::MessageType::PRIVATE_CHAT_OPEN
            };

            if (omRestrictedCommands.contains(env.type()) &&
                !OkidiaDatabase::instance().isUserAuthorizedForClient(masterUserId, targetId)) {
                Logger::warning(QObject::tr("Broker GÜVENLİK: OM kullanıcısı yetkisiz olduğu istemciye komut gönderemez! Kullanıcı: %1 İstemci: %2 Tip: %3")
                                .arg(masterUserId, targetId, QString::number(env.type())));
                okidia::CommandResult cmdRes;
                cmdRes.set_command_id(std::to_string(env.sequence_id()));
                cmdRes.set_success(false);
                cmdRes.set_error_message(QObject::tr("Bu istemci üzerinde yönetim yetkiniz bulunmamaktadır.").toStdString());
                okidia::Envelope rejEnv;
                rejEnv.set_type(okidia::MessageType::COMMAND_RESULT);
                rejEnv.set_target_id(targetId.toStdString());
                rejEnv.set_payload(cmdRes.SerializeAsString());
                conn->sendEnvelope(rejEnv);
                return;
            }
        }

        // Kilit alma/bırakma işlemleri
        if (!targetId.isEmpty()) {
            if (env.type() == okidia::MessageType::SCREEN_LOCK) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "ScreenLock");
            } else if (env.type() == okidia::MessageType::SCREEN_UNLOCK) {
                if (!m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                    m_ctx->registry->releaseAgentLock(targetId);
                    if (m_ctx->bellScheduler) {
                        m_ctx->bellScheduler->notifyManualUnlock(targetId);
                    }
                }
            } else if (env.type() == okidia::MessageType::INPUT_LOCK) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "InputLock");
            } else if (env.type() == okidia::MessageType::INPUT_UNLOCK) {
                if (!m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                    m_ctx->registry->releaseAgentLock(targetId);
                }
            } else if (env.type() == okidia::MessageType::INPUT_TAKE_CONTROL) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "RemoteControl");
            } else if (env.type() == okidia::MessageType::SCREEN_SELECT) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "ScreenView");
            } else if (env.type() == okidia::MessageType::REMOTE_CONTROL_STOP) {
                if (!m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                    m_ctx->registry->releaseAgentLock(targetId);
                    reconcileAgentState(targetId);
                }
            } else if (env.type() == okidia::MessageType::EXAM_START) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "Exam");
            } else if (env.type() == okidia::MessageType::EXAM_END || env.type() == okidia::MessageType::EXAM_COLLECT) {
                if (!m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                    m_ctx->registry->releaseAgentLock(targetId);
                    reconcileAgentState(targetId);
                }
            } else if (env.type() == okidia::MessageType::BROADCAST_BEGIN) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "Broadcast");
            } else if (env.type() == okidia::MessageType::BROADCAST_END) {
                if (!m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                    m_ctx->registry->releaseAgentLock(targetId);
                }
            } else if (env.type() == okidia::MessageType::AUDIO_BROADCAST || env.type() == okidia::MessageType::AUDIO_BROADCAST_LIVE || env.type() == okidia::MessageType::AUDIO_BROADCAST_FILE) {
                m_ctx->registry->acquireAgentLock(targetId, masterUserId, masterDisplayName, "Audio");
            } else if (env.type() == okidia::MessageType::AUDIO_STOP || env.type() == okidia::MessageType::AUDIO_BROADCAST_STOP) {
                if (!m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                    m_ctx->registry->releaseAgentLock(targetId);
                }
            }
        } else {
            auto acquireBroadcastLock = [&](const QString& lockType) {
                for (const auto& c : m_ctx->registry->allClients()) {
                    if (masterRole == "OM" && !OkidiaDatabase::instance().isUserAuthorizedForClient(masterUserId, c.clientId)) {
                        continue;
                    }
                    if (!m_ctx->registry->isAgentLockedByOther(c.clientId, masterUserId)) {
                        m_ctx->registry->acquireAgentLock(c.clientId, masterUserId, masterDisplayName, lockType);
                    }
                }
            };
            auto releaseBroadcastLock = [&]() {
                for (const auto& c : m_ctx->registry->allClients()) {
                    if (masterRole == "OM" && !OkidiaDatabase::instance().isUserAuthorizedForClient(masterUserId, c.clientId)) {
                        continue;
                    }
                    if (!m_ctx->registry->isAgentLockedByOther(c.clientId, masterUserId)) {
                        m_ctx->registry->releaseAgentLock(c.clientId);
                    }
                }
            };

            if (env.type() == okidia::MessageType::SCREEN_LOCK) {
                acquireBroadcastLock("ScreenLock");
            } else if (env.type() == okidia::MessageType::SCREEN_UNLOCK) {
                releaseBroadcastLock();
                if (m_ctx->bellScheduler) {
                    for (const auto& c : m_ctx->registry->allClients()) {
                        m_ctx->bellScheduler->notifyManualUnlock(c.clientId);
                    }
                }
            } else if (env.type() == okidia::MessageType::INPUT_LOCK) {
                acquireBroadcastLock("InputLock");
            } else if (env.type() == okidia::MessageType::INPUT_UNLOCK) {
                releaseBroadcastLock();
            } else if (env.type() == okidia::MessageType::EXAM_START) {
                acquireBroadcastLock("Exam");
            } else if (env.type() == okidia::MessageType::EXAM_END || env.type() == okidia::MessageType::EXAM_COLLECT) {
                releaseBroadcastLock();
            } else if (env.type() == okidia::MessageType::BROADCAST_BEGIN) {
                okidia::BroadcastBegin bBegin;
                QStringList targets;
                if (bBegin.ParseFromString(env.payload()) && bBegin.target_ids_size() > 0) {
                    for (int i = 0; i < bBegin.target_ids_size(); ++i) {
                        targets << QString::fromStdString(bBegin.target_ids(i));
                    }
                }
                ActiveBroadcastSession session;
                session.masterUserId = masterUserId;
                session.masterDisplayName = masterDisplayName;
                session.targetClientIds = targets;
                session.lastBeginEnvelope = env;
                m_activeBroadcastSessions[masterUserId] = session;

                if (!targets.isEmpty()) {
                    for (const QString& tid : targets) {
                        if (!m_ctx->registry->isAgentLockedByOther(tid, masterUserId)) {
                            m_ctx->registry->acquireAgentLock(tid, masterUserId, masterDisplayName, "Broadcast");
                        }
                    }
                } else {
                    acquireBroadcastLock("Broadcast");
                }
            } else if (env.type() == okidia::MessageType::BROADCAST_END) {
                const auto session = m_activeBroadcastSessions.take(masterUserId);
                const QStringList targets = session.targetClientIds;
                if (!targets.isEmpty()) {
                    for (const QString& tid : targets) {
                        if (!m_ctx->registry->isAgentLockedByOther(tid, masterUserId)) {
                            m_ctx->registry->releaseAgentLock(tid);
                        }
                    }
                } else {
                    releaseBroadcastLock();
                }
            } else if (env.type() == okidia::MessageType::AUDIO_BROADCAST || env.type() == okidia::MessageType::AUDIO_BROADCAST_LIVE || env.type() == okidia::MessageType::AUDIO_BROADCAST_FILE) {
                acquireBroadcastLock("Audio");
            } else if (env.type() == okidia::MessageType::AUDIO_STOP || env.type() == okidia::MessageType::AUDIO_BROADCAST_STOP) {
                releaseBroadcastLock();
            }
        }

        // Yetki kontrolü (kilit)
        if (!targetId.isEmpty()) {
            if (m_ctx->registry->isAgentLockedByOther(targetId, masterUserId)) {
                static const QSet<okidia::MessageType> allowedReadOnly = {
                    okidia::MessageType::REFRESH_REQUEST,
                    okidia::MessageType::SCREEN_INFO_REQUEST,
                    okidia::MessageType::APP_LIST_REQUEST,
                    okidia::MessageType::SCREEN_CAPTURE,
                    okidia::MessageType::QUERY_LOCAL_USERS_REQ
                };
                if (!allowedReadOnly.contains(env.type())) {
                    Logger::warning(QObject::tr("Broker: Kilitli istemciye yetkisiz komut engellendi -> Client: ") + targetId + QObject::tr(" Master: ") + masterUserId + QObject::tr(" Tip: ") + QString::number(env.type()));
                    okidia::CommandResult cmdRes;
                    cmdRes.set_command_id(std::to_string(env.sequence_id()));
                    cmdRes.set_success(false);
                    cmdRes.set_error_message(QObject::tr("Bu istemci şu anda başka bir Master tarafından kilitlenmiş veya yönetiliyor.").toStdString());
                    okidia::Envelope rejEnv;
                    rejEnv.set_type(okidia::MessageType::COMMAND_RESULT);
                    rejEnv.set_target_id(targetId.toStdString());
                    rejEnv.set_payload(cmdRes.SerializeAsString());
                    conn->sendEnvelope(rejEnv);
                    return;
                }
            }
        }

        if (!targetId.isEmpty()) {
            if (targetId.startsWith("__multicast__:") || targetId.startsWith("__multicast_anons__:")) {
                m_ctx->multicast->handleEnvelope(conn, env);
            } else if (targetId == "__sync_masters__") {
                m_ctx->registry->forwardToMaster(peerId, env);
            } else if (targetId.startsWith("master:")) {
                const QString targetUser = targetId.mid(7);
                bool routed = false;
                for (Connection* mConn : m_ctx->registry->masterConnections()) {
                    if (m_ctx->registry->authenticatedUsername(mConn) == targetUser ||
                        m_ctx->registry->authenticatedUserId(mConn) == targetUser) {
                        mConn->sendEnvelope(env);
                        routed = true;
                    }
                }
            } else {
                // Pub/Sub Abonelik Yönetimi (Talep Tabanlı Dağıtım):
                if (env.type() == okidia::MessageType::SCREEN_FPS_SET) {
                    bool isFirst = false;
                    m_ctx->registry->subscribeScreen(conn, targetId, &isFirst);
                    m_ctx->registry->forwardToClient(targetId, env);
                    return;
                } else if (env.type() == okidia::MessageType::SCREEN_STREAM_PAUSE) {
                    bool isLast = false;
                    m_ctx->registry->unsubscribeScreen(conn, targetId, &isLast);
                    if (isLast) {
                        m_ctx->registry->forwardToClient(targetId, env);
                        Logger::info(QObject::tr("Broker: %1 için tüm ekran izleyicileri ayrıldı, SCREEN_STREAM_PAUSE iletildi").arg(targetId));
                    }
                    return;
                } else if (env.type() == okidia::MessageType::METRICS_STREAM_START) {
                    bool isFirst = false;
                    m_ctx->registry->subscribeMetrics(conn, targetId, &isFirst);
                    if (isFirst) {
                        m_ctx->registry->forwardToClient(targetId, env);
                    }
                    return;
                } else if (env.type() == okidia::MessageType::METRICS_STREAM_STOP) {
                    bool isLast = false;
                    m_ctx->registry->unsubscribeMetrics(conn, targetId, &isLast);
                    if (isLast) {
                        m_ctx->registry->forwardToClient(targetId, env);
                    }
                    return;
                }

                m_ctx->registry->forwardToClient(targetId, env);
            }
        } else if (targetId.isEmpty()) {
            // Merkezi Varlık Depolama (Zil / Logo) Dinlemesi
            if (env.type() == okidia::MessageType::FILE_TRANSFER_BEGIN) {
                okidia::FileTransferBegin bMsg;
                if (bMsg.ParseFromString(env.payload()) && bMsg.destination() == "assets") {
                    if (m_ctx->transferManager) {
                        m_ctx->transferManager->handleAssetBegin(env.sequence_id(), bMsg);
                    }
                }
            } else if (env.type() == okidia::MessageType::FILE_TRANSFER_CHUNK) {
                okidia::FileTransferChunk cMsg;
                if (cMsg.ParseFromString(env.payload())) {
                    if (m_ctx->transferManager) {
                        m_ctx->transferManager->handleAssetChunk(env.sequence_id(), cMsg);
                    }
                }
            } else if (env.type() == okidia::MessageType::FILE_TRANSFER_END) {
                if (m_ctx->transferManager) {
                    m_ctx->transferManager->handleAssetEnd(env.sequence_id());
                }
            }

            if (env.type() == okidia::MessageType::REFRESH_REQUEST) {
                m_ctx->registry->sendAllClientsToMaster(conn);
            }

            // Ekran yayını için hedef listesi varsa sadece hedeflenen istemcilere ilet
            if (env.type() == okidia::MessageType::BROADCAST_BEGIN ||
                env.type() == okidia::MessageType::BROADCAST_FRAME ||
                env.type() == okidia::MessageType::BROADCAST_END) {
                
                if (env.type() == okidia::MessageType::BROADCAST_FRAME) {
                    if (m_activeBroadcastSessions.contains(masterUserId)) {
                        m_activeBroadcastSessions[masterUserId].lastFrameEnvelope = env;
                    }
                }

                if (m_activeBroadcastSessions.contains(masterUserId)) {
                    const QStringList targets = m_activeBroadcastSessions[masterUserId].targetClientIds;
                    if (!targets.isEmpty()) {
                        for (const QString& tid : targets) {
                            if (m_ctx->registry->isAgentLockedByOther(tid, masterUserId)) {
                                continue;
                            }
                            // Backpressure / Frame drop: Eğer istemci soket tamponu doluysa (yavaş ağ/istemci),
                            // broker'ın RAM şişmesini önlemek için yeni kareyi bu istemciye göndermeyip atla.
                            if (env.type() == okidia::MessageType::BROADCAST_FRAME) {
                                RegisteredClient rc = m_ctx->registry->client(tid);
                                if (rc.connection && rc.connection->bytesToWrite() > 300 * 1024) {
                                    continue;
                                }
                            }
                            m_ctx->registry->forwardToClient(tid, env);
                        }
                        return;
                    }
                }
            }

            for (const auto& c : m_ctx->registry->allClients()) {
                if (masterRole == "OM" && !OkidiaDatabase::instance().isUserAuthorizedForClient(masterUserId, c.clientId)) {
                    static const QSet<okidia::MessageType> allowedReadOnly = {
                        okidia::MessageType::REFRESH_REQUEST,
                        okidia::MessageType::SCREEN_INFO_REQUEST,
                        okidia::MessageType::APP_LIST_REQUEST,
                        okidia::MessageType::SCREEN_CAPTURE,
                        okidia::MessageType::QUERY_LOCAL_USERS_REQ
                    };
                    if (!allowedReadOnly.contains(env.type())) {
                        continue;
                    }
                }

                if (m_ctx->registry->isAgentLockedByOther(c.clientId, masterUserId)) {
                    static const QSet<okidia::MessageType> allowedReadOnly = {
                        okidia::MessageType::REFRESH_REQUEST,
                        okidia::MessageType::SCREEN_INFO_REQUEST,
                        okidia::MessageType::APP_LIST_REQUEST,
                        okidia::MessageType::SCREEN_CAPTURE,
                        okidia::MessageType::QUERY_LOCAL_USERS_REQ
                    };
                    if (!allowedReadOnly.contains(env.type())) {
                        continue;
                    }
                }

                // Backpressure / Frame drop: Genel ekran yayınında soket tamponu dolu istemcileri atla
                if (env.type() == okidia::MessageType::BROADCAST_FRAME) {
                    if (c.connection && c.connection->bytesToWrite() > 300 * 1024) {
                        continue;
                    }
                }

                // Eğer bu bir adaptif FPS yayınıysa, halihazırda uzaktan izlenen/yönetilen veya ekran yayını alan istemcileri koru
                if (env.type() == okidia::MessageType::SCREEN_FPS_SET) {
                    auto locks = m_ctx->registry->activeLocks();
                    if (locks.contains(c.clientId)) {
                        const QString lt = locks[c.clientId].lockType;
                        if (lt == "RemoteControl" || lt == "ScreenView" || lt == "Broadcast") {
                            continue;
                        }
                    }
                }

                m_ctx->registry->forwardToClient(c.clientId, env);
            }
        } else {
            const QString tid = m_ctx->registry->clientIdByHash(static_cast<uint>(seq));
            if (!tid.isEmpty()) {
                if (m_ctx->registry->isAgentLockedByOther(tid, masterUserId)) {
                    static const QSet<okidia::MessageType> allowedReadOnly = {
                        okidia::MessageType::REFRESH_REQUEST,
                        okidia::MessageType::SCREEN_INFO_REQUEST,
                        okidia::MessageType::APP_LIST_REQUEST,
                        okidia::MessageType::SCREEN_CAPTURE,
                        okidia::MessageType::QUERY_LOCAL_USERS_REQ
                    };
                    if (!allowedReadOnly.contains(env.type())) {
                        Logger::warning(QObject::tr("Broker: Kilitli istemciye yetkisiz komut engellendi (hash) -> Client: ") + tid + QObject::tr(" Master: ") + masterUserId + QObject::tr(" Tip: ") + QString::number(env.type()));
                        return;
                    }
                }
                m_ctx->registry->forwardToClient(tid, env);
            } else {
                Logger::warning(QObject::tr("Broker: Hedef bulunamadı → hash:") + QString::number(seq));
            }
        }

    } else {
        // Agent'tan gelen mesaj
        QString senderClientId;
        for (const auto& c : m_ctx->registry->allClients()) {
            if (c.connection == conn) {
                senderClientId = c.clientId;
                break;
            }
        }
        if (senderClientId.isEmpty()) return;

        // HEARTBEAT
        if (env.type() == okidia::MessageType::HEARTBEAT) {
            m_ctx->registry->updateHeartbeat(senderClientId);
            return;
        }

        // SMARTBOARD_UNLOCK_NOTIFY: Akıllı tahta PIN ile açıldığında Broker kilit kaydını serbest bırak
        if (env.type() == okidia::MessageType::SMARTBOARD_UNLOCK_NOTIFY) {
            m_ctx->registry->releaseAgentLock(senderClientId);
            if (m_ctx->bellScheduler) {
                m_ctx->bellScheduler->notifyManualUnlock(senderClientId);
            }
            Logger::info(QObject::tr("Broker: Akıllı Tahta (%1) PIN ile açıldı, kilit kaydı düşürüldü.").arg(senderClientId));
        }

        // Pub/Sub Fan-Out: Ekran paketleri (SCREEN_FRAME / SCREEN_TILE_UPDATE)
        if (env.type() == okidia::MessageType::SCREEN_FRAME ||
            env.type() == okidia::MessageType::SCREEN_TILE_UPDATE) {
            auto subs = m_ctx->registry->screenSubscribers(senderClientId);
            if (subs.isEmpty()) {
                // İzleyici yoksa gereksiz ekran akışını durdur
                okidia::Envelope pEnv;
                pEnv.set_type(okidia::MessageType::SCREEN_STREAM_PAUSE);
                pEnv.set_target_id(senderClientId.toStdString());
                conn->sendEnvelope(pEnv);
                return;
            }
            okidia::Envelope fwd = env;
            fwd.set_sequence_id(qHash(senderClientId));
            fwd.set_target_id(senderClientId.toStdString());
            for (Connection* mConn : subs) {
                mConn->sendEnvelope(fwd);
            }
            return;
        }

        // Pub/Sub Fan-Out: Canlı Sistem Metrikleri (SYSTEM_METRICS_REPORT)
        if (env.type() == okidia::MessageType::SYSTEM_METRICS_REPORT) {
            auto subs = m_ctx->registry->metricsSubscribers(senderClientId);
            if (subs.isEmpty()) {
                // İzleyici yoksa gereksiz metrik akışını durdur
                okidia::Envelope mEnv;
                mEnv.set_type(okidia::MessageType::METRICS_STREAM_STOP);
                mEnv.set_target_id(senderClientId.toStdString());
                conn->sendEnvelope(mEnv);
                return;
            }
            okidia::Envelope fwd = env;
            fwd.set_sequence_id(qHash(senderClientId));
            fwd.set_target_id(senderClientId.toStdString());
            for (Connection* mConn : subs) {
                mConn->sendEnvelope(fwd);
            }
            return;
        }

        // Diğer tüm agent mesajları Master'a iletilir
        m_ctx->registry->forwardToMaster(senderClientId, env);
    }
}

void MessageHandler::reconcileAgentState(const QString& clientId) {
    if (clientId.isEmpty()) return;

    // 1. İstemci halen kilitli mi kontrol et (Eğer RemoteControl veya başka bir kilit varsa dokunma)
    auto locks = m_ctx->registry->activeLocks();
    if (locks.contains(clientId)) {
        const QString curLock = locks[clientId].lockType;
        if (curLock == "RemoteControl" || curLock == "ScreenView" || curLock == "Exam") {
            Logger::info(QObject::tr("Broker: %1 için durum uzlaşması atlandı, aktif kilit mevcut: %2").arg(clientId, curLock));
            return;
        }
    }

    // 2. Aktif ekran yayını oturumu var mı kontrol et (TV yayını / catch-up mantığı)
    bool joinedBroadcast = false;
    if (!m_activeBroadcastSessions.isEmpty()) {
        for (auto it = m_activeBroadcastSessions.begin(); it != m_activeBroadcastSessions.end(); ++it) {
            const ActiveBroadcastSession& session = it.value();
            bool isTarget = false;

            if (session.targetClientIds.isEmpty()) {
                // Genel yayın: Tüm okul veya sınıfa açık yayın
                isTarget = true;
            } else if (session.targetClientIds.contains(clientId)) {
                // Hedefli yayın: Bu istemci açıkça hedef listesinde
                isTarget = true;
            }

            if (isTarget) {
                Logger::info(QObject::tr("Broker: Durum Uzlaşması (Catch-up) tetiklendi -> %1 istemcisi %2 tarafından yapılan canlı yayına otonom dahil ediliyor.")
                             .arg(clientId, session.masterDisplayName));

                // Kilit al ve ajana yayın başlangıç zarfını ilet
                m_ctx->registry->acquireAgentLock(clientId, session.masterUserId, session.masterDisplayName, "Broadcast");
                
                okidia::Envelope beginEnv = session.lastBeginEnvelope;
                beginEnv.set_target_id(clientId.toStdString());
                m_ctx->registry->forwardToClient(clientId, beginEnv);

                // Eğer yakalanmış son kare varsa anında tek kare gönder (I-Frame garantisi)
                if (!session.lastFrameEnvelope.payload().empty()) {
                    okidia::Envelope frameEnv = session.lastFrameEnvelope;
                    frameEnv.set_target_id(clientId.toStdString());
                    m_ctx->registry->forwardToClient(clientId, frameEnv);
                }
                joinedBroadcast = true;
                break;
            }
        }
    }

    // 3. Aktif yayın yoksa: Okul zil / teneffüs / sabah ilk ders öncesi kilit uzlaşmasını çalıştır
    if (!joinedBroadcast && m_ctx->bellScheduler) {
        m_ctx->bellScheduler->reconcileClientBellState(clientId);
    }
}

