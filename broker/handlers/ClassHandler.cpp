#include "ClassHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"

ClassHandler::ClassHandler(BrokerContext* ctx) : m_ctx(ctx) {}

bool ClassHandler::handle(const okidia::Envelope& env, Connection* conn, const QString& /*peerId*/, bool* /*isMaster*/) {
    // ── DB_CLASS_ADD_REQ ─────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_CLASS_ADD_REQ) {
        okidia::DbClassAddRequest req;
        okidia::DbClassAddResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYMOrOM && m_ctx->isRequesterYMOrOM(conn)) {
                ClassRecord c;
                QString reqId = QString::fromStdString(req.id()).trimmed();
                c.id = reqId.isEmpty() ? OkidiaUtils::generateUuid() : reqId;
                c.name = QString::fromStdString(req.name());
                c.icon = QString::fromStdString(req.icon());
                c.parentId = QString::fromStdString(req.parent_id());
                c.type = QString::fromStdString(req.type());
                c.createdBy = m_ctx->registry->authenticatedUserId(conn);
                c.createdAt = OkidiaUtils::nowSecs();

                if (OkidiaDatabase::instance().upsertClass(c)) {
                    Logger::info(QObject::tr("Broker DB: Sınıf/grup eklendi → ") + c.name);
                    resp.set_success(true);

                    // OM kendi oluşturduğu sınıfa veya gruba otomatik atansın
                    UserRecord reqUser = OkidiaDatabase::instance().userById(c.createdBy);
                    if (reqUser.role == "OM") {
                        if (OkidiaDatabase::instance().grantPermission(c.createdBy, c.id, "full")) {
                            Logger::info(QObject::tr("Broker DB: OM kullanıcısına yeni oluşturduğu sınıf/grup için otomatik yetki verildi → ") + c.createdBy);
                        }
                    }

                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı sınıf yazma hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem.");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_CLASS_ADD_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_CLASS_DEL_REQ ─────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_CLASS_DEL_REQ) {
        okidia::DbClassDelRequest req;
        okidia::DbClassDelResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYMOrOM && m_ctx->isRequesterYMOrOM(conn)) {
                QString cid = QString::fromStdString(req.class_id());
                if (OkidiaDatabase::instance().removeClass(cid)) {
                    Logger::info(QObject::tr("Broker DB: Sınıf/grup silindi → ") + cid);
                    resp.set_success(true);
                    if (m_ctx->broadcastRoomClosed) m_ctx->broadcastRoomClosed(cid);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı sınıf silme hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem.");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_CLASS_DEL_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_CLASS_RENAME_REQ ──────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_CLASS_RENAME_REQ) {
        okidia::DbClassRenameRequest req;
        okidia::DbClassRenameResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYMOrOM && m_ctx->isRequesterYMOrOM(conn)) {
                QString cid = QString::fromStdString(req.class_id());
                QString newName = QString::fromStdString(req.new_name());
                
                ClassRecord c = OkidiaDatabase::instance().classById(cid);
                if (!c.id.isEmpty()) {
                    c.name = newName;
                    if (OkidiaDatabase::instance().upsertClass(c)) {
                        Logger::info(QObject::tr("Broker DB: Sınıf/grup yeniden adlandırıldı → ") + newName);
                        resp.set_success(true);
                        if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                    } else {
                        resp.set_success(false);
                        resp.set_error_msg("Veritabanı güncelleme hatası.");
                    }
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Sınıf bulunamadı.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem.");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_CLASS_RENAME_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_AGENT_ASSIGN_REQ ──────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_AGENT_ASSIGN_REQ) {
        okidia::DbAgentAssignRequest req;
        okidia::DbAgentAssignResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYMOrOM && m_ctx->isRequesterYMOrOM(conn)) {
                QString aid = QString::fromStdString(req.agent_id());
                QString gid = QString::fromStdString(req.group_id());
                
                const AgentRecord oldRec = OkidiaDatabase::instance().agent(aid);
                const QString oldClassId = oldRec.classId;

                if (OkidiaDatabase::instance().updateAgentClass(aid, gid)) {
                    Logger::info(QObject::tr("Broker DB: İstemci sınıfı güncellendi → ") + aid + QObject::tr(" to ") + gid);
                    
                    if (!oldClassId.isEmpty() && oldClassId != gid && m_ctx->broadcastAgentLeftRoom) {
                        m_ctx->broadcastAgentLeftRoom(aid, oldClassId);
                    }
                    if (!gid.isEmpty() && oldClassId != gid && m_ctx->broadcastAgentJoinedRoom) {
                        m_ctx->broadcastAgentJoinedRoom(aid, gid);
                    }

                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı istemci grubu atama hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem.");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_AGENT_ASSIGN_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    return false;
}
