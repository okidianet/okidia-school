#include "ClientHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/OkidiaAuditDatabase.h"
#include "../../common/OkidiaConfig.h"

#include <QTimer>

ClientHandler::ClientHandler(BrokerContext* ctx) : m_ctx(ctx) {}

bool ClientHandler::handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster) {
    // ── CLIENT_INFO ──────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::CLIENT_INFO) {
        okidia::ClientInfo info;
        info.ParseFromString(env.payload());
        const QString hostname = QString::fromStdString(info.hostname());
        if (hostname == "__master__") {
            if (!peerId.isEmpty()) {
                if (peerId.startsWith("okidia-master-")) {
                    *isMaster = true;
                    m_ctx->registry->addMasterConnection(conn);
                    Logger::info(QObject::tr("Broker: Master Node (ana sertifika) baglandi <- ") + conn->peerAddress() + " (" + peerId + ")");
                } else {
                    UserRecord u = OkidiaDatabase::instance().userById(peerId);
                    *isMaster = true;
                    m_ctx->registry->addMasterConnection(conn);
                    Logger::info(QObject::tr("Broker: Master bağlandı ← ") + conn->peerAddress() + " (" + u.username + ")");
                }
            } else {
                *isMaster = true;
                m_ctx->registry->addMasterConnection(conn);
                Logger::info(QObject::tr("Broker: Anonim Master bağlandı (Sertifika/Giriş bekleniyor) ← ") + conn->peerAddress());
            }
            if (m_ctx->broadcastStatus) m_ctx->broadcastStatus();
            return true;
        }
        if (m_ctx->registry->isPendingConnection(conn)) {
            Logger::warning(QObject::tr("Broker: Pending bağlantıdan CLIENT_INFO → reddedildi"));
            return true;
        }
        QString clientId = !info.machine_id().empty()
            ? QString::fromStdString(info.machine_id())
            : hostname;
            
        // ── YETKİ KONTROLÜ VE SPOOFING KORUMASI ─────────────────────────
        if (!*isMaster) {
            if (peerId.isEmpty()) {
                Logger::warning(QObject::tr("Broker: Anonim veya kayıtsız istemci CLIENT_INFO gönderdi → Bağlantı kesiliyor"));
                conn->disconnect();
                return true;
            }

            AgentRecord agent = OkidiaDatabase::instance().agent(peerId);
            // 1. Veritabanında kayıtlı mı kontrolü (kullanıcı silmiş olabilir)
            if (agent.machineId.isEmpty()) {
                Logger::warning(QObject::tr("Broker: Yetkisiz veya silinmiş istemci bağlantısı reddedildi! Sertifika CN: ") + peerId);
                
                QTimer::singleShot(500, conn, [conn]() {
                    conn->disconnect();
                });
                return true;
            }
            // 2. Kimlik sahteciliği (Spoofing) kontrolü
            if (peerId != clientId) {
                Logger::warning(QObject::tr("Broker: Agent payload machine_id (") + clientId + QObject::tr(") sertifika CN (") + peerId + QObject::tr(") ile uyuşmuyor! Sertifika CN'ye zorlanıyor."));
                clientId = peerId;
                info.set_machine_id(peerId.toStdString());
            }
        }
        
        RegisteredClient client;
        client.clientId   = clientId;
        client.hostname   = hostname;
        client.username   = QString::fromStdString(info.username());
        client.displayName = QString::fromStdString(info.display_name());
        client.machineId  = clientId;
        client.connection = conn;
        m_ctx->registry->registerClient(clientId, client);
        if (m_ctx->broadcastStatus) m_ctx->broadcastStatus();
        if (m_ctx->multicast) m_ctx->multicast->handleClientReconnect(clientId);

        // Ajanın kendi yolladığı IP yanıltıcı olabilir (NAT vb.), her zaman gerçek soket IP'sini zorla
        QString ipStr = conn->peerAddress();
        info.set_ip_address(ipStr.toStdString());
        
        // DB'deki IP, osInfo, osPrettyName, osArch ve agentVer'i güncel tut
        QString osPrettyNameStr = QString::fromStdString(info.os_version());
        QString rawOsTag = QString::fromStdString(info.os_tag());
        QString prettyDerivedTag = OkidiaUtils::normalizeOsTag(osPrettyNameStr);
        QString osInfoStr = OkidiaUtils::normalizeOsTag(rawOsTag);
        if (prettyDerivedTag == "debian12" || prettyDerivedTag == "debian13" ||
            prettyDerivedTag == "debian11" || prettyDerivedTag == "debian10" ||
            prettyDerivedTag == "ubuntu24" || prettyDerivedTag == "ubuntu22" ||
            prettyDerivedTag == "ubuntu20" || prettyDerivedTag == "windows") {
            osInfoStr = prettyDerivedTag;
        }
        info.set_os_tag(osInfoStr.toStdString());
        QString osArchStr = OkidiaUtils::normalizeArch(QString::fromStdString(info.os_arch()));
        info.set_os_arch(osArchStr.toStdString());
        QString agentVerStr = QString::fromStdString(info.agent_version());
        bool dbUpdated = false;
        
        AgentRecord dbRec = OkidiaDatabase::instance().agent(clientId);
        if (!dbRec.machineId.isEmpty()) {
            if (!ipStr.isEmpty() && dbRec.ip != ipStr) {
                dbRec.ip = ipStr;
                dbUpdated = true;
            }
            if (!osInfoStr.isEmpty() && dbRec.osInfo != osInfoStr) {
                dbRec.osInfo = osInfoStr;
                dbUpdated = true;
            }
            if (!osPrettyNameStr.isEmpty() && dbRec.osPrettyName != osPrettyNameStr) {
                dbRec.osPrettyName = osPrettyNameStr;
                dbUpdated = true;
            }
            if (!osArchStr.isEmpty() && dbRec.osArch != osArchStr) {
                dbRec.osArch = osArchStr;
                dbUpdated = true;
            }
            if (!agentVerStr.isEmpty() && dbRec.agentVer != agentVerStr) {
                dbRec.agentVer = agentVerStr;
                dbUpdated = true;
            }
            if (dbUpdated) {
                OkidiaDatabase::instance().upsertAgent(dbRec);
                Logger::info(QObject::tr("Broker: Agent bilgileri güncellendi → ") + clientId);
            }
        }

        // Agent'a tipini bildir — Yalnızca ajanın mevcut tipi veritabanındakinden farklıysa bildir
        const AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
        if (rec.agentType > 0 && rec.agentType != static_cast<int>(info.agent_type())) {
            okidia::Envelope typeEnv;
            typeEnv.set_type(okidia::MessageType::AGENT_TYPE_SET);
            typeEnv.set_payload(std::to_string(rec.agentType));
            conn->sendEnvelope(typeEnv);
            Logger::info(QObject::tr("Broker: Agent tipi bildirildi → ") + clientId + " : " + QString::number(rec.agentType));
        }

        if (!rec.alias.isEmpty() && rec.alias != QString::fromStdString(info.alias())) {
            okidia::AgentAliasSet aliasMsg;
            aliasMsg.set_machine_id(clientId.toStdString());
            aliasMsg.set_alias(rec.alias.toStdString());
            okidia::Envelope aliasEnv;
            aliasEnv.set_type(okidia::MessageType::AGENT_ALIAS_SET);
            aliasEnv.set_payload(aliasMsg.SerializeAsString());
            conn->sendEnvelope(aliasEnv);
            Logger::info(QObject::tr("Broker: Agent takma adı bildirildi → ") + clientId + " : " + rec.alias);
        }

        // Tip 1, 0 veya 3 bağlandığında güncel sistem politikalarını ve otonom ders programını senkronize et
        if (rec.agentType == 1 || rec.agentType == 0 || rec.agentType == 3) {
            bool disableSwitch = (rec.agentType == 1 || rec.agentType == 0) ? OkidiaConfig::isDisableSwitchUserStudentPCs() : false;
            bool usbProtection = OkidiaConfig::isUsbProtectionEnabled();
            okidia::AgentPolicySet policyMsg;
            policyMsg.set_disable_switch_user(disableSwitch);
            policyMsg.set_usb_protection_enabled(usbProtection);
            policyMsg.set_school_schedule_json(OkidiaDatabase::instance().metaValue("school_schedule_json").toStdString());
            policyMsg.set_smartboard_lock_policy(OkidiaConfig::smartboardLockPolicy());
            policyMsg.set_school_timezone(OkidiaDatabase::instance().metaValue("school_timezone").toStdString());
            policyMsg.set_bell_smartboard_enabled(OkidiaConfig::isBellSmartboardEnabled());
            policyMsg.set_bell_student_pc_enabled(OkidiaConfig::isBellStudentPcEnabled());
            policyMsg.set_bell_auto_lock_recess(OkidiaConfig::isBellAutoLockRecess());
            policyMsg.set_last_bell_lock_behavior(OkidiaConfig::lastBellLockBehavior());
            okidia::Envelope policyEnv;
            policyEnv.set_type(okidia::MessageType::AGENT_POLICY_SET);
            policyEnv.set_payload(policyMsg.SerializeAsString());
            conn->sendEnvelope(policyEnv);
        }

        // Tip 3: Akıllı Tahta bağlandığında aktif öğretmen tohum listesini otonom senkronize et
        if (rec.agentType == 3) {
            QList<TeacherRecord> teachers = OkidiaDatabase::instance().allTeachers();
            if (!teachers.isEmpty()) {
                okidia::TeacherKeySyncReq syncReq;
                for (const auto& t : teachers) {
                    if (!t.isActive) continue;
                    auto* item = syncReq.add_teachers();
                    item->set_teacher_id(t.id.toStdString());
                    item->set_display_name(t.displayName.toStdString());
                    item->set_username(t.username.toStdString());
                    item->set_seed_hex(t.seedHex.toStdString());
                    item->set_password(t.password.toStdString());
                }

                okidia::Envelope syncEnv;
                syncEnv.set_type(okidia::MessageType::TEACHER_KEY_SYNC_REQ);
                syncEnv.set_payload(syncReq.SerializeAsString());
                conn->sendEnvelope(syncEnv);
                Logger::info(QObject::tr("Broker: Tip 3 Akıllı Tahtaya (%1) %2 öğretmen tohumu otonom senkronize edildi.")
                             .arg(clientId).arg(syncReq.teachers_size()));
            }
        }

        // Tip 3 (Akıllı Tahta) ve Tip 1/0 bağlandığında merkezi zil ve okul varlıklarını otonom senkronize et
        if (rec.agentType != 2 && m_ctx->transferManager) {
            QTimer::singleShot(1500, m_ctx->transferManager, [ctx = m_ctx, conn, agentType = rec.agentType]() {
                if (conn && conn->isConnected() && ctx->transferManager) {
                    ctx->transferManager->syncAssetsToClient(conn, agentType);
                }
            });
        }

        // Windows İstemci için kaldırma parolası hash'ini gönder
        bool isWinClient = osPrettyNameStr.contains("Windows", Qt::CaseInsensitive) ||
                           osInfoStr.contains("Windows", Qt::CaseInsensitive) ||
                           rec.osPrettyName.contains("Windows", Qt::CaseInsensitive) ||
                           rec.osInfo.contains("Windows", Qt::CaseInsensitive);
        if (isWinClient) {
            QString uninstHash = OkidiaDatabase::instance().uninstallPasswordHash();
            okidia::AgentUninstallPassSet passMsg;
            passMsg.set_password_hash(uninstHash.toStdString());
            okidia::Envelope passEnv;
            passEnv.set_type(okidia::MessageType::AGENT_UNINSTALL_PASS_SET);
            passEnv.set_payload(passMsg.SerializeAsString());
            conn->sendEnvelope(passEnv);
            Logger::info(QObject::tr("Broker: Windows İstemci kaldırma koruma hash'i gönderildi → ") + clientId);
        }

        // Master'a iletmeden önce agent_type ve alias'ı ekle
        if (rec.agentType > 0) {
            info.set_agent_type(rec.agentType);
        }
        if (!rec.alias.isEmpty()) {
            info.set_alias(rec.alias.toStdString());
        }
        okidia::Envelope masterEnv = env;
        masterEnv.set_payload(info.SerializeAsString());
        m_ctx->registry->forwardToMaster(clientId, masterEnv);

        // Sınıf ve Grup sohbet odalarını ve üyelerini Agent'a gönder
        if (!rec.classId.isEmpty()) {
            struct TargetRoom {
                QString roomId;
                QString roomName;
                QString parentRoomId;
                QString classIdForPermission;
                QSet<QString> memberClassIds;
            };

            QList<TargetRoom> targetRooms;
            const ClassRecord clsRec = OkidiaDatabase::instance().classById(rec.classId);

            if (clsRec.type == "group" && !clsRec.parentId.isEmpty()) {
                const ClassRecord parentCls = OkidiaDatabase::instance().classById(clsRec.parentId);
                QString parentName = parentCls.name.isEmpty() ? "Sınıf Sohbeti" : parentCls.name;

                // 1. Üst Sınıf Odası
                TargetRoom parentRoom;
                parentRoom.roomId = "room_" + clsRec.parentId;
                parentRoom.roomName = parentName;
                parentRoom.parentRoomId = "";
                parentRoom.classIdForPermission = clsRec.parentId;
                parentRoom.memberClassIds.insert(clsRec.parentId);
                for (const ClassRecord& c : OkidiaDatabase::instance().allClasses()) {
                    if (c.parentId == clsRec.parentId) {
                        parentRoom.memberClassIds.insert(c.id);
                    }
                }
                targetRooms.append(parentRoom);

                // 2. Alt Grup Odası
                TargetRoom groupRoom;
                groupRoom.roomId = "room_" + rec.classId;
                groupRoom.roomName = clsRec.name.isEmpty() ? "Grup Sohbeti" : clsRec.name;
                groupRoom.parentRoomId = "room_" + clsRec.parentId;
                groupRoom.classIdForPermission = clsRec.parentId;
                groupRoom.memberClassIds.insert(rec.classId);
                targetRooms.append(groupRoom);
            } else {
                // Doğrudan sınıf
                TargetRoom classRoom;
                classRoom.roomId = "room_" + rec.classId;
                classRoom.roomName = clsRec.name.isEmpty() ? "Sınıf Sohbeti" : clsRec.name;
                classRoom.parentRoomId = "";
                classRoom.classIdForPermission = rec.classId;
                classRoom.memberClassIds.insert(rec.classId);
                for (const ClassRecord& c : OkidiaDatabase::instance().allClasses()) {
                    if (c.parentId == rec.classId) {
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

                // 1. Öğretmenler ve Yöneticiler
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

                // 2. Ajanlar
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

                // 3. Online Master durumları
                if (m_ctx->activeRoomMembers && m_ctx->activeRoomMembers->contains(tr.roomId)) {
                    for (const okidia::ChatMemberEvent& evt : (*m_ctx->activeRoomMembers)[tr.roomId]) {
                        okidia::Envelope joinedEnv;
                        joinedEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
                        joinedEnv.set_payload(evt.SerializeAsString());
                        conn->sendEnvelope(joinedEnv);
                    }
                }

                // 4. Bu ajanı aktif odaya kaydet ve bildir
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

                // 5. Aynı odadaki diğer ajanlarla karşılıklı durum bildirimi
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

                            if (c.connection) {
                                c.connection->sendEnvelope(thisAgentJoinEnv);
                            }
                        }
                    }
                }
            }

            // 6. Sınıfın kilit durumunu Agent'a bildir
            const QString mainClassId = (clsRec.type == "group" && !clsRec.parentId.isEmpty()) ? clsRec.parentId : rec.classId;
            const ChatLockRecord lockRec = OkidiaDatabase::instance().chatLockState(mainClassId);
            okidia::ChatLockState lockProto;
            lockProto.set_class_id(mainClassId.toStdString());
            lockProto.set_chat_locked(lockRec.chatLocked);
            lockProto.set_private_locked(lockRec.privateLocked);

            okidia::Envelope lockEnv;
            lockEnv.set_type(okidia::MessageType::CHAT_LOCK_STATE);
            lockEnv.set_payload(lockProto.SerializeAsString());
            conn->sendEnvelope(lockEnv);
        }
        
        // OTA kontrolü yap
        if (m_ctx->transferManager) {
            m_ctx->transferManager->checkAndSendUpdateToAgent(conn, QString::fromStdString(info.os_tag()), QString::fromStdString(info.os_arch()), QString::fromStdString(info.agent_version()), clientId);
        }

        // Bekleyen komutları gönder
        const auto pending = OkidiaDatabase::instance().pendingCommands(clientId);
        if (!pending.isEmpty()) {
            Logger::info(QObject::tr("Broker: ") + QString::number(pending.size())
                         + QObject::tr(" bekleyen komut gönderiliyor → ") + clientId);
            for (const auto& pcmd : pending) {
                okidia::Envelope penv;
                if (penv.ParseFromArray(pcmd.payload.constData(), pcmd.payload.size())) {
                    m_ctx->registry->forwardToClient(clientId, penv);
                    OkidiaDatabase::instance().deletePendingCommand(pcmd.id);
                }
            }
        }

        // ── Durum Uzlaşması (State Reconciliation & Auto Catch-up) ──
        // İstemci açıldığında / yeniden bağlandığında sınıfta aktif bir yayın veya durum varsa otonom katılsın
        if (m_ctx->reconcileAgentState) {
            m_ctx->reconcileAgentState(clientId);
        }

        return true;
    }

    // ── DISCOVERY_IGNORE ─────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DISCOVERY_IGNORE) {
        const QString targetId = QString::fromStdString(env.target_id());
        m_ctx->registry->forwardToPending(targetId, env);
        m_ctx->registry->disconnectPending(targetId);
        if (m_ctx->broadcastStatus) m_ctx->broadcastStatus();
        return true;
    }

    // ── DISCOVERY_REJECT ─────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DISCOVERY_REJECT) {
        const QString targetId = QString::fromStdString(env.target_id());
        m_ctx->registry->forwardToPending(targetId, env);
        m_ctx->registry->disconnectPending(targetId);
        if (m_ctx->broadcastStatus) m_ctx->broadcastStatus();
        return true;
    }

    // ── CERT_PROVISION (Eski — geriye dönük uyumluluk) ───────────────
    if (env.type() == okidia::MessageType::CERT_PROVISION) {
        m_ctx->registry->forwardToPending(QString::fromStdString(env.target_id()), env);
        return true;
    }

    // ── CLIENT_REVOKE ────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::CLIENT_REVOKE) {
        const QString requesterId = m_ctx->registry->authenticatedUserId(conn).isEmpty()
                                    ? peerId
                                    : m_ctx->registry->authenticatedUserId(conn);
        UserRecord u = OkidiaDatabase::instance().userById(requesterId);
        if (u.role != "YM") {
            Logger::warning(QObject::tr("Broker: Yetkisiz CLIENT_REVOKE denemesi → ") + requesterId);
            return true;
        }
        
        okidia::ClientRevokeCommand cmd;
        if (cmd.ParseFromString(env.payload())) {
            if (cmd.all()) {
                for (const auto& c : m_ctx->registry->allClients()) {
                    const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                    if (!rec.classId.isEmpty() && m_ctx->broadcastAgentLeftRoom) {
                        m_ctx->broadcastAgentLeftRoom(c.machineId, rec.classId);
                    }
                }
                m_ctx->registry->revokeAll(cmd.uninstall());
                Logger::info(QObject::tr("Broker: Tüm istemciler revoke edildi"));
            } else if (!cmd.machine_id().empty()) {
                const QString mid = QString::fromStdString(cmd.machine_id());
                const AgentRecord rec = OkidiaDatabase::instance().agent(mid);
                if (!rec.classId.isEmpty() && m_ctx->broadcastAgentLeftRoom) {
                    m_ctx->broadcastAgentLeftRoom(mid, rec.classId);
                }
                m_ctx->registry->revokeClient(mid, cmd.uninstall());
                Logger::info(QObject::tr("Broker: İstemci revoke edildi → ") + mid);
            }
        }
        if (m_ctx->broadcastStatus) m_ctx->broadcastStatus();
        return true;
    }

    // ── CLIENT_UNREVOKE ──────────────────────────────────────────────
    if (env.type() == okidia::MessageType::CLIENT_UNREVOKE) {
        const QString requesterId = m_ctx->registry->authenticatedUserId(conn).isEmpty()
                                    ? peerId
                                    : m_ctx->registry->authenticatedUserId(conn);
        UserRecord u = OkidiaDatabase::instance().userById(requesterId);
        if (u.role != "YM") {
            Logger::warning(QObject::tr("Broker: Yetkisiz CLIENT_UNREVOKE denemesi → ") + requesterId);
            return true;
        }
        
        okidia::ClientUnrevokeCommand cmd;
        if (cmd.ParseFromString(env.payload())) {
            if (!cmd.machine_id().empty()) {
                const QString mid = QString::fromStdString(cmd.machine_id());
                m_ctx->registry->unrevokeClient(mid);
                Logger::info(QObject::tr("Broker: İstemci yasağı kaldırıldı → ") + mid);
            }
        }
        if (m_ctx->broadcastStatus) m_ctx->broadcastStatus();
        return true;
    }

    // ── AGENT_ALIAS_SET (Master → Broker → Agent/Masters) ────────────
    if (env.type() == okidia::MessageType::AGENT_ALIAS_SET) {
        okidia::AgentAliasSet aliasMsg;
        if (aliasMsg.ParseFromString(env.payload())) {
            QString machineId = QString::fromStdString(aliasMsg.machine_id());
            QString aliasStr  = QString::fromStdString(aliasMsg.alias()).trimmed();
            if (!machineId.isEmpty()) {
                OkidiaDatabase::instance().updateAgentAlias(machineId, aliasStr);
                Logger::info(QObject::tr("Broker: İstemci takma adı güncellendi → ") + machineId + " : " + aliasStr);

                // Hedef ajana ilet
                m_ctx->registry->forwardToClient(machineId, env);

                // Tüm bağlı Master'lara senkronize et
                m_ctx->registry->forwardToMaster("", env);

                if (m_ctx->broadcastDbSyncSignal) {
                    m_ctx->broadcastDbSyncSignal();
                }
            }
        }
        return true;
    }

    // ── AGENT_TYPE_SET (Master → Broker → Agent/Masters) ─────────────
    if (env.type() == okidia::MessageType::AGENT_TYPE_SET) {
        QString targetId = QString::fromStdString(env.target_id());
        uint32_t reqType = 1;
        try {
            reqType = std::stoul(env.payload());
        } catch (...) {}

        if (!targetId.isEmpty() && reqType > 0) {
            OkidiaDatabase::instance().updateAgentType(targetId, static_cast<int>(reqType));
            Logger::info(QObject::tr("Broker: İstemci tipi Master tarafından veritabanında güncellendi → ") + targetId + " : " + QString::number(reqType));
            if (m_ctx->broadcastDbSyncSignal) {
                m_ctx->broadcastDbSyncSignal();
            }
        }

        m_ctx->registry->forwardToClient(targetId, env);
        m_ctx->registry->forwardToMaster(targetId, env);
        return true;
    }

    // ── AGENT_TYPE_SET_RESULT (Agent → Broker/Master) ────────────────
    if (env.type() == okidia::MessageType::AGENT_TYPE_SET_RESULT) {
        okidia::AgentTypeSetResult res;
        if (res.ParseFromString(env.payload())) {
            QString senderClientId = m_ctx->registry->hasClient(peerId) ? peerId : QString();
            for (const auto& c : m_ctx->registry->allClients()) {
                if (c.connection == conn) {
                    senderClientId = c.clientId;
                    break;
                }
            }
            QString machineId = QString::fromStdString(env.target_id());
            if (machineId.isEmpty()) {
                machineId = senderClientId;
            }
            if (res.approved()) {
                AgentRecord rec = OkidiaDatabase::instance().agent(machineId);
                if (!rec.machineId.isEmpty()) {
                    if (rec.agentType != static_cast<int>(res.requested_type())) {
                        rec.agentType = res.requested_type();
                        OkidiaDatabase::instance().upsertAgent(rec);
                        Logger::info(QObject::tr("Broker: İstemci tipi onaylandı ve DB güncellendi → ") + machineId + " : " + QString::number(rec.agentType));
                        if (m_ctx->broadcastDbSyncSignal) {
                            m_ctx->broadcastDbSyncSignal();
                        }
                    }
                }
            } else {
                Logger::info(QObject::tr("Broker: İstemci tipi reddedildi → ") + machineId + " : " + QString::fromStdString(res.reason()));
            }
            okidia::Envelope forwardEnv = env;
            forwardEnv.set_target_id(machineId.toStdString());
            m_ctx->registry->forwardToMaster(machineId, forwardEnv);
        }
        return true;
    }

    // ── CUSTOM_PROFILE_UPSERT_REQ (Master(YM) → Broker) ──────────────
    if (env.type() == okidia::MessageType::CUSTOM_PROFILE_UPSERT_REQ) {
        okidia::CustomProfileUpsertReq req;
        okidia::CustomProfileUpsertRes resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                CustomProfileRecord rec;
                rec.id = req.profile().id();
                rec.name = QString::fromStdString(req.profile().name()).trimmed();
                rec.playBell = req.profile().play_bell();
                rec.recessLock = req.profile().recess_lock();
                rec.chatEnabled = req.profile().chat_enabled();
                rec.iconName = QString::fromStdString(req.profile().icon_name()).trimmed();
                if (rec.iconName.isEmpty()) rec.iconName = "custom_profile";

                if (rec.name.isEmpty()) {
                    resp.set_success(false);
                    resp.set_error_msg("Profil adı boş olamaz.");
                } else if (OkidiaDatabase::instance().upsertCustomProfile(rec)) {
                    int finalId = rec.id;
                    if (finalId <= 0) {
                        for (const auto& p : OkidiaDatabase::instance().allCustomProfiles()) {
                            if (p.name == rec.name) { finalId = p.id; break; }
                        }
                    }
                    resp.set_success(true);
                    resp.set_id(finalId);
                    Logger::info(QObject::tr("Broker: Özel profil kaydedildi → ") + QString::number(finalId) + " (" + rec.name + ")");
                    if (m_ctx->broadcastDbSyncSignal) {
                        m_ctx->broadcastDbSyncSignal();
                    }
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı kayıt hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem: Sadece YM özel profil yönetebilir.");
            }

            okidia::Envelope resEnv;
            resEnv.set_type(okidia::MessageType::CUSTOM_PROFILE_UPSERT_RES);
            resEnv.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(resEnv);
        }
        return true;
    }

    // ── CUSTOM_PROFILE_DELETE_REQ (Master(YM) → Broker) ──────────────
    if (env.type() == okidia::MessageType::CUSTOM_PROFILE_DELETE_REQ) {
        okidia::CustomProfileDeleteReq req;
        okidia::CustomProfileDeleteRes resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                int pid = req.profile_id();
                if (pid > 0 && OkidiaDatabase::instance().deleteCustomProfile(pid)) {
                    resp.set_success(true);
                    Logger::info(QObject::tr("Broker: Özel profil silindi ve bağlı istemciler Öğrenci PC tipine çekildi → ") + QString::number(pid));
                    if (m_ctx->broadcastDbSyncSignal) {
                        m_ctx->broadcastDbSyncSignal();
                    }
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Profil silinemedi.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem: Sadece YM özel profil yönetebilir.");
            }

            okidia::Envelope resEnv;
            resEnv.set_type(okidia::MessageType::CUSTOM_PROFILE_DELETE_RES);
            resEnv.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(resEnv);
        }
        return true;
    }

    // ── AGENT_UNINSTALL_PASS_SET ─────────────────────────────────────
    if (env.type() == okidia::MessageType::AGENT_UNINSTALL_PASS_SET) {
        okidia::AgentUninstallPassSet req;
        QString newHash;
        if (req.ParseFromString(env.payload())) {
            newHash = QString::fromStdString(req.password_hash()).trimmed().toLower();
        } else {
            newHash = QString::fromStdString(env.payload()).trimmed().toLower();
        }

        okidia::AgentUninstallPassResponse resp;
        if (!m_ctx->isRequesterYM || !m_ctx->isRequesterYM(conn)) {
            resp.set_success(false);
            resp.set_error_msg("Yetkisiz işlem (Sadece Yönetici (YM) istemci kaldırma parolasını değiştirebilir).");
            Logger::warning(QObject::tr("Broker: Yetkisiz istemci kaldırma parolası değiştirme denemesi!"));
        } else {
            if (newHash.isEmpty()) {
                newHash = OkidiaDatabase::DEFAULT_UNINSTALL_PASS_HASH;
            }
            bool ok = OkidiaDatabase::instance().setUninstallPasswordHash(newHash);
            resp.set_success(ok);
            if (!ok) {
                resp.set_error_msg("Veritabanına yazılamadı.");
            } else {
                Logger::info(QObject::tr("Broker: İstemci kaldırma parolası hash'i güncellendi"));
                QString rId = m_ctx->registry->authenticatedUserId(conn);
                OkidiaAuditDatabase::instance().logCommand(
                    "YM", "SYSTEM", "İstemci kaldırma parolası güncellendi", "SETTINGS_CHANGE",
                    rId, conn->peerAddress(), OkidiaUtils::nowSecs());

                // Tüm çevrim içi Windows istemcilerine dağıt
                okidia::AgentUninstallPassSet broadcastMsg;
                broadcastMsg.set_password_hash(newHash.toStdString());
                okidia::Envelope agentEnv;
                agentEnv.set_type(okidia::MessageType::AGENT_UNINSTALL_PASS_SET);
                agentEnv.set_payload(broadcastMsg.SerializeAsString());

                const auto clients = m_ctx->registry->allClients();
                int winCount = 0;
                for (const auto& cl : clients) {
                    AgentRecord dbRec = OkidiaDatabase::instance().agent(cl.clientId);
                    bool isWin = dbRec.osPrettyName.contains("Windows", Qt::CaseInsensitive) ||
                                 dbRec.osInfo.contains("Windows", Qt::CaseInsensitive);
                    if (isWin && cl.connection && cl.connection->isConnected()) {
                        cl.connection->sendEnvelope(agentEnv);
                        winCount++;
                    }
                }
                Logger::info(QObject::tr("Broker: Kaldırma parolası hash'i %1 adet aktif Windows istemciye iletildi.").arg(winCount));

                for (Connection* mConn : m_ctx->registry->masterConnections()) {
                    if (mConn != conn) {
                        mConn->sendEnvelope(agentEnv);
                    }
                }
            }
        }

        okidia::Envelope re;
        re.set_type(okidia::MessageType::AGENT_UNINSTALL_PASS_RES);
        re.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(re);
        return true;
    }

    // ── POWER_WAKE_ON_LAN ────────────────────────────────────────────
    if (env.type() == okidia::MessageType::POWER_WAKE_ON_LAN) {
        QString targetId = QString::fromStdString(env.target_id());
        QString mac = QString::fromStdString(env.payload());
        QString ip;
        if (!targetId.isEmpty()) {
            AgentRecord rec = OkidiaDatabase::instance().agent(targetId);
            if (rec.agentType == 2) {
                Logger::warning(QObject::tr("Broker: İdari PC için Wake-on-LAN engellendi → ") + targetId);
                return true;
            }
            if (mac.isEmpty()) mac = rec.mac;
            ip = rec.ip;
        } else {
            const auto allAgents = OkidiaDatabase::instance().allAgents();
            for (const auto& ag : allAgents) {
                if (ag.agentType != 2 && !ag.mac.isEmpty()) {
                    OkidiaUtils::sendWakeOnLan(ag.mac, ag.ip);
                }
            }
            Logger::info(QObject::tr("Broker: Tüm sınıfa (İdari PC'ler hariç) Wake-on-LAN sinyali yayınlandı."));
            return true;
        }
        if (!mac.isEmpty()) {
            OkidiaUtils::sendWakeOnLan(mac, ip);
            Logger::info(QObject::tr("Broker: Sunucu yerel ağından Wake-on-LAN sinyali yayınlandı → ") + targetId + " (" + mac + ")");
        }
        return true;
    }

    return false;
}
