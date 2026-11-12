#include "SystemHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/OkidiaAuditDatabase.h"
#include "../../common/db/OkidiaChatDatabase.h"
#include "../../common/crypto/AesGcm.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/OkidiaConfig.h"
#include "../BellScheduler.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDateTime>
#include <QTimeZone>
#include <QPointer>
#include <functional>


SystemHandler::SystemHandler(BrokerContext* ctx) : m_ctx(ctx) {}

bool SystemHandler::handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster) {
    // ── OTA_UPDATE (Ajan & Master Paketleri) ─────────────────────────────────
    if (env.type() == okidia::MessageType::OTA_UPDATE_BEGIN) {
        okidia::OtaUpdateBegin req;
        if (req.ParseFromString(env.payload())) {
            QString filename = QString::fromStdString(req.filename());
            // Eğer paket okidia-master veya okidia-broker ise sadece YM yükleyebilir!
            if (filename.startsWith("okidia-master") || filename.startsWith("okidia-broker")) {
                const QString requesterId = m_ctx->registry->authenticatedUserId(conn).isEmpty()
                                            ? peerId
                                            : m_ctx->registry->authenticatedUserId(conn);
                UserRecord u = OkidiaDatabase::instance().userById(requesterId);
                if (u.role != "YM") {
                    Logger::warning(QObject::tr("Broker: Master/Broker paketi yükleme isteği YM olmayan kullanıcı tarafından yapıldı -> engellendi: ") + requesterId);
                    okidia::OtaBrokerReport rep;
                    rep.set_success(false);
                    rep.set_message("Bu paketi Broker'a yüklemek için YM (Yönetici Master) yetkisi gereklidir.");
                    okidia::Envelope envRep;
                    envRep.set_type(okidia::MessageType::OTA_BROKER_REPORT);
                    envRep.set_payload(rep.SerializeAsString());
                    conn->sendEnvelope(envRep);
                    return true;
                }
            }
            m_ctx->transferManager->handleOtaBegin(conn, req);
        }
        return true;
    }

    if (env.type() == okidia::MessageType::OTA_UPDATE_CHUNK) {
        okidia::OtaUpdateChunk req;
        if (req.ParseFromString(env.payload())) {
            m_ctx->transferManager->handleOtaChunk(conn, req);
        }
        return true;
    }

    if (env.type() == okidia::MessageType::OTA_UPDATE_END) {
        m_ctx->transferManager->handleOtaEnd(conn, env);
        return true;
    }

    // ── BROKER_OTA_UPDATE (Broker'ın Kendi Güncellemesi - YALNIZCA YM) ────────
    if (env.type() == okidia::MessageType::BROKER_OTA_UPDATE_BEGIN) {
        const QString requesterId = m_ctx->registry->authenticatedUserId(conn).isEmpty()
                                    ? peerId
                                    : m_ctx->registry->authenticatedUserId(conn);
        UserRecord u = OkidiaDatabase::instance().userById(requesterId);
        if (u.role != "YM") {
            Logger::warning(QObject::tr("Broker: Broker OTA güncelleme isteği yetkisiz kullanıcıdan geldi -> engellendi: ") + requesterId);
            okidia::OtaBrokerReport rep;
            rep.set_success(false);
            rep.set_message("Broker güncellemesi sadece YM (Yönetici Master) tarafından yapılabilir.");
            okidia::Envelope envRep;
            envRep.set_type(okidia::MessageType::OTA_BROKER_REPORT);
            envRep.set_payload(rep.SerializeAsString());
            conn->sendEnvelope(envRep);
            return true;
        }
        okidia::OtaUpdateBegin req;
        if (req.ParseFromString(env.payload())) {
            m_ctx->transferManager->handleBrokerOtaBegin(conn, req);
        }
        return true;
    }

    if (env.type() == okidia::MessageType::BROKER_OTA_UPDATE_CHUNK) {
        okidia::OtaUpdateChunk req;
        if (req.ParseFromString(env.payload())) {
            m_ctx->transferManager->handleBrokerOtaChunk(conn, req);
        }
        return true;
    }

    if (env.type() == okidia::MessageType::BROKER_OTA_UPDATE_END) {
        m_ctx->transferManager->handleBrokerOtaEnd(conn, env);
        return true;
    }

    // ── MASTER_UPDATE_DOWNLOAD_REQ (Master'ın Broker'dan Paketi İndirmesi) ───
    if (env.type() == okidia::MessageType::MASTER_UPDATE_DOWNLOAD_REQ) {
        okidia::MasterUpdateDownloadRequest req;
        if (req.ParseFromString(env.payload())) {
            m_ctx->transferManager->handleMasterDownloadReq(conn, req);
        }
        return true;
    }

    // ── BROKER_UPDATES ───────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::BROKER_UPDATES_LIST_REQ) {
        Logger::info(QObject::tr("Broker: Master'dan BROKER_UPDATES_LIST_REQ alindi!"));
        m_ctx->transferManager->handleUpdatesListReq(conn, env);
        return true;
    }

    if (env.type() == okidia::MessageType::BROKER_UPDATES_DELETE_REQ) {
        m_ctx->transferManager->handleUpdatesDeleteReq(conn, env);
        return true;
    }

    // ── BROKER_PORT_SET_REQ ──────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::BROKER_PORT_SET_REQ) {
        okidia::BrokerPortSetRequest req;
        okidia::BrokerPortSetResponse resp;
        if (req.ParseFromString(env.payload())) {
            quint16 newPort = static_cast<quint16>(req.new_port());
            resp.set_new_port(newPort);

            bool isYM = m_ctx->isRequesterYM ? m_ctx->isRequesterYM(conn) : false;
            if (!isYM) {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece Yönetici (YM) portu değiştirebilir).");
            } else if (newPort < 1024 || newPort > 65535) {
                resp.set_success(false);
                resp.set_error_msg("Geçersiz port numarası (1024 - 65535 arasında olmalıdır).");
            } else {
                bool ok = m_ctx->changeBrokerPort ? m_ctx->changeBrokerPort(newPort) : false;
                resp.set_success(ok);
                if (!ok) {
                    resp.set_error_msg("Broker yapılandırma dosyasına yazılamadı.");
                }
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::BROKER_PORT_SET_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── BROKER_CONFIG_SET_REQ (İsim & Port Değişikliği) ─────────────────────
    if (env.type() == okidia::MessageType::BROKER_CONFIG_SET_REQ) {
        okidia::BrokerConfigSetRequest req;
        okidia::BrokerConfigSetResponse resp;
        if (req.ParseFromString(env.payload())) {
            QString newName = QString::fromStdString(req.broker_name()).trimmed();
            quint16 newPort = static_cast<quint16>(req.new_port());

            resp.set_broker_name(newName.toStdString());
            resp.set_new_port(newPort);

            bool isYM = m_ctx->isRequesterYM ? m_ctx->isRequesterYM(conn) : false;
            if (!isYM) {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece Yönetici (YM) sunucu yapılandırmasını değiştirebilir).");
            } else if (newPort > 0 && (newPort < 1024 || newPort > 65535)) {
                resp.set_success(false);
                resp.set_error_msg("Geçersiz port numarası (1024 - 65535 arasında olmalıdır).");
            } else {
                bool ok = m_ctx->changeBrokerConfig ? m_ctx->changeBrokerConfig(newName, newPort) : false;
                resp.set_success(ok);
                if (!ok) {
                    resp.set_error_msg("Broker yapılandırma dosyasına kaydedilemedi.");
                } else {
                    Logger::info(QObject::tr("Broker: Master (YM) tarafından sunucu yapılandırması güncellendi → İsim: %1, Port: %2")
                                 .arg(newName).arg(newPort));
                }
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::BROKER_CONFIG_SET_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── AUDIT_LOG_RECORD ────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::AUDIT_LOG_RECORD) {
        okidia::AuditLogRecord req;
        if (req.ParseFromString(env.payload())) {
            OkidiaAuditDatabase::instance().logCommand(
                QString::fromStdString(req.actor()),
                QString::fromStdString(req.target()),
                QString::fromStdString(req.detail()),
                QString::fromStdString(req.category()),
                QString::fromStdString(req.user_id()),
                QString::fromStdString(req.master_id()),
                static_cast<qint64>(req.timestamp())
            );
        }
        return true;
    }

    // ── AUDIT_LOG_QUERY_REQ ─────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::AUDIT_LOG_QUERY_REQ) {
        okidia::AuditLogQueryRequest req;
        if (req.ParseFromString(env.payload())) {
            okidia::AuditLogQueryResponse resp;
            int totalCount = 0;
            auto records = OkidiaAuditDatabase::instance().queryAuditLogAdvanced(
                req.from_timestamp(),
                req.to_timestamp(),
                QString::fromStdString(req.category()),
                QString::fromStdString(req.actor()),
                QString::fromStdString(req.target()),
                QString::fromStdString(req.search_query()),
                req.limit() > 0 ? req.limit() : 100,
                req.offset(),
                &totalCount
            );
            resp.set_total_count(totalCount);
            for (const auto& r : records) {
                auto* record = resp.add_records();
                record->set_actor(r.actor.toStdString());
                record->set_target(r.target.toStdString());
                record->set_detail(r.detail.toStdString());
                record->set_category(r.category.toStdString());
                record->set_user_id(r.userId.toStdString());
                record->set_master_id(r.masterId.toStdString());
                record->set_timestamp(r.timestamp);
            }
            
            okidia::Envelope re;
            re.set_type(okidia::MessageType::AUDIT_LOG_QUERY_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── SSH Kimlik Yönetimi ──────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::SSH_CRED_LIST) {
        okidia::SshCredListRequest req;
        if (req.ParseFromString(env.payload())) {
            auto creds = OkidiaDatabase::instance().sshCredentials(
                QString::fromStdString(req.requester_user_id()));

            okidia::SshCredListResponse resp;
            for (const auto& c : creds) {
                auto* pc = resp.add_credentials();
                pc->set_id(c.id.toStdString());
                pc->set_name(c.name.toStdString());
                pc->set_username(c.username.toStdString());
                pc->set_secret(c.secret.toStdString());
                pc->set_is_key(c.isKey);
                pc->set_owner_id(c.ownerUserId.toStdString());
                pc->set_target_id(c.targetId.toStdString());
                pc->set_target_type(c.targetType.toStdString());
                pc->set_machine_id(c.machineId.toStdString());
                pc->set_created_at(c.createdAt);
            }
            okidia::Envelope re;
            re.set_type(okidia::MessageType::SSH_CRED_LIST_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    if (env.type() == okidia::MessageType::SSH_CRED_ADD) {
        okidia::SshCredAddRequest req;
        if (req.ParseFromString(env.payload())) {
            SshCredentialRecord rec;
            rec.id = QString::fromStdString(req.credential().id());
            if (rec.id.isEmpty()) rec.id = OkidiaUtils::generateUuid();
            rec.name = QString::fromStdString(req.credential().name());
            rec.username = QString::fromStdString(req.credential().username());
            rec.secret = QByteArray::fromStdString(req.credential().secret());
            rec.isKey = req.credential().is_key();
            rec.ownerUserId = QString::fromStdString(req.credential().owner_id());
            rec.targetId = QString::fromStdString(req.credential().target_id());
            rec.targetType = QString::fromStdString(req.credential().target_type());
            rec.machineId = QString::fromStdString(req.credential().machine_id());
            OkidiaDatabase::instance().upsertSshCredential(rec);
            Logger::info(QObject::tr("Broker: SSH kimliği kaydedildi → ") + rec.name + " (" + rec.targetType + ":" + rec.targetId + ")");
        }
        return true;
    }

    if (env.type() == okidia::MessageType::SSH_CRED_DEL) {
        okidia::SshCredDelRequest req;
        if (req.ParseFromString(env.payload())) {
            const QString id = QString::fromStdString(req.id());
            // Basit bir yetki kontrolü: Sadece kendi kimliğini veya ortak kimliği silebilir
            SshCredentialRecord existing = OkidiaDatabase::instance().sshCredentialById(id);
            if (!existing.id.isEmpty()) {
                 if (existing.ownerUserId.isEmpty() || existing.ownerUserId == QString::fromStdString(req.requester_user_id())) {
                     OkidiaDatabase::instance().deleteSshCredential(id);
                     Logger::info(QObject::tr("Broker: SSH kimliği silindi → ") + id);
                 }
            }
        }
        return true;
    }

    // ── BROKER_SETTINGS_UPDATE ───────────────────────────────────────────────
    if (env.type() == okidia::MessageType::BROKER_SETTINGS_UPDATE) {
        okidia::BrokerSettingsUpdate bsu;
        if (bsu.ParseFromString(env.payload())) {
            bool isYM = m_ctx->isRequesterYM ? m_ctx->isRequesterYM(conn) : false;
            if (isYM) {
                if (bsu.audit_retention_days() > 0) {
                    OkidiaAuditDatabase::instance().setAuditRetentionDays(bsu.audit_retention_days());
                    Logger::info(QObject::tr("Broker: Kayıt saklama süresi güncellendi → ") 
                                 + QString::number(bsu.audit_retention_days()) + QObject::tr(" gün"));
                    if (m_ctx->purgeLogic) {
                        QTimer::singleShot(0, m_ctx->purgeLogic);
                    }
                }
                if (bsu.auto_backup_retention_count() > 0) {
                    OkidiaDatabase::instance().setMetaValue("auto_backup_retention", QString::number(bsu.auto_backup_retention_count()));
                    Logger::info(QObject::tr("Broker: Otomatik yedek saklama adedi güncellendi → ")
                                 + QString::number(bsu.auto_backup_retention_count()));
                }
                OkidiaDatabase::instance().setMetaValue("auto_backup_include_chat", bsu.auto_backup_include_chat() ? "1" : "0");
                Logger::info(QObject::tr("Broker: Otomatik yedek sohbet dahil durumu güncellendi → ")
                             + (bsu.auto_backup_include_chat() ? QObject::tr("Açık") : QObject::tr("Kapalı")));

                if (bsu.has_backup_password()) {
                    QString newPwd = QString::fromStdString(bsu.backup_password()).trimmed();
                    OkidiaDatabase::instance().setMetaValue("backup_password", newPwd);
                    if (!newPwd.isEmpty()) {
                        Logger::info(QObject::tr("Broker: Özel otomatik yedek parolası güncellendi."));
                    } else {
                        Logger::info(QObject::tr("Broker: Özel otomatik yedek parolası kaldırıldı (Varsayılan kurulum koduna dönüldü)."));
                    }
                }

                // Okul Ders & Zil Programı Senkronizasyonu
                if (!bsu.school_schedule_json().empty()) {
                    QString scheduleJson = QString::fromStdString(bsu.school_schedule_json());
                    bool prevAutoLock = OkidiaConfig::isBellAutoLockRecess();
                    bool prevRecessLogout = OkidiaConfig::isRecessLogoutStudentPCs();

                    OkidiaDatabase::instance().setMetaValue("school_schedule_json", scheduleJson);
                    OkidiaConfig::importSchoolConfigJson(scheduleJson);
                    Logger::info(QObject::tr("Broker: Okul ders ve zil programı veritabanına ve konfigürasyona kaydedildi."));
                    if (m_ctx->bellScheduler) {
                        m_ctx->bellScheduler->updateSchedule();
                    }

                    // Güncel ders programını ve kilit politikalarını bağlı tüm Agent'lara otonom önbellek için ilet
                    okidia::AgentPolicySet policyMsg;
                    policyMsg.set_disable_switch_user(OkidiaConfig::isDisableSwitchUserStudentPCs());
                    policyMsg.set_usb_protection_enabled(OkidiaConfig::isUsbProtectionEnabled());
                    policyMsg.set_school_schedule_json(scheduleJson.toStdString());
                    policyMsg.set_smartboard_lock_policy(OkidiaConfig::smartboardLockPolicy());
                    policyMsg.set_school_timezone(OkidiaDatabase::instance().metaValue("school_timezone").toStdString());
                    policyMsg.set_bell_smartboard_enabled(OkidiaConfig::isBellSmartboardEnabled());
                    policyMsg.set_bell_student_pc_enabled(OkidiaConfig::isBellStudentPcEnabled());
                    policyMsg.set_bell_auto_lock_recess(OkidiaConfig::isBellAutoLockRecess());
                    policyMsg.set_last_bell_lock_behavior(OkidiaConfig::lastBellLockBehavior());

                    okidia::Envelope polEnv;
                    polEnv.set_type(okidia::MessageType::AGENT_POLICY_SET);
                    polEnv.set_payload(policyMsg.SerializeAsString());

                    if (m_ctx->registry) {
                        for (const auto& cl : m_ctx->registry->allClients()) {
                            if (cl.connection && cl.connection->isConnected()) {
                                cl.connection->sendEnvelope(polEnv);
                            }
                        }
                    }

                    // Teneffüs kilit veya oturum kapatma kuralı kapatılmışsa kilitli/bekleyen istemcilerin kilidini derhal aç
                    bool newAutoLock = OkidiaConfig::isBellAutoLockRecess();
                    bool newRecessLogout = OkidiaConfig::isRecessLogoutStudentPCs();
                    if ((prevAutoLock && !newAutoLock) || (prevRecessLogout && !newRecessLogout)) {
                        okidia::Envelope unlockEnv;
                        unlockEnv.set_type(okidia::MessageType::SCREEN_UNLOCK);

                        if (m_ctx->registry) {
                            const auto clients = m_ctx->registry->allClients();
                            for (const auto& cl : clients) {
                                if (cl.connection && cl.connection->isConnected()) {
                                    auto agentRec = OkidiaDatabase::instance().agent(cl.machineId);
                                    bool shouldUnlock = false;
                                    if (agentRec.agentType == 3 && prevAutoLock && !newAutoLock) {
                                        shouldUnlock = true;
                                    } else if ((agentRec.agentType == 1 || agentRec.agentType == 0) && prevRecessLogout && !newRecessLogout) {
                                        shouldUnlock = true;
                                    }
                                    if (shouldUnlock) {
                                        cl.connection->sendEnvelope(unlockEnv);
                                    }
                                }
                            }
                        }
                        Logger::info(QObject::tr("Broker: Kapatılan teneffüs kuralı nedeniyle ilgili istemcilere SCREEN_UNLOCK yayınlandı."));
                    }

                    // İstemci Politikası Dağıtımı (Öğrenci PC switch-user kısıtlaması, USB koruması vb.)
                    bool disableSwitch = OkidiaConfig::isDisableSwitchUserStudentPCs();
                    bool usbProtection = OkidiaConfig::isUsbProtectionEnabled();

                    if (m_ctx->registry) {
                        const auto clients = m_ctx->registry->allClients();
                        for (const auto& cl : clients) {
                            if (cl.connection && cl.connection->isConnected()) {
                                auto agentRec = OkidiaDatabase::instance().agent(cl.machineId);
                                if (agentRec.agentType == 1 || agentRec.agentType == 0 || agentRec.agentType == 3) {
                                    okidia::AgentPolicySet policyMsg;
                                    policyMsg.set_disable_switch_user((agentRec.agentType == 1 || agentRec.agentType == 0) ? disableSwitch : false);
                                    policyMsg.set_usb_protection_enabled(usbProtection);
                                    okidia::Envelope policyEnv;
                                    policyEnv.set_type(okidia::MessageType::AGENT_POLICY_SET);
                                    policyEnv.set_payload(policyMsg.SerializeAsString());
                                    cl.connection->sendEnvelope(policyEnv);
                                }
                            }
                        }
                    }
                    Logger::info(QObject::tr("Broker: İstemci politikaları yayınlandı (Switch User: %1, USB Koruma: %2)")
                                 .arg(disableSwitch ? QObject::tr("KAPALI") : QObject::tr("AÇIK"))
                                 .arg(usbProtection ? QObject::tr("AÇIK") : QObject::tr("KAPALI")));
                }

                // Okul Saat Dilimi (Timezone)
                if (!bsu.school_timezone().empty()) {
                    QString tzId = QString::fromStdString(bsu.school_timezone());
                    OkidiaDatabase::instance().setMetaValue("school_timezone", tzId);
                    QTimeZone tz(tzId.toUtf8());
                    if (tz.isValid() && m_ctx->bellScheduler) {
                        m_ctx->bellScheduler->setSchoolTimeZone(tz);
                    }
                    Logger::info(QObject::tr("Broker: Okul saat dilimi kaydedildi: %1").arg(tzId));
                }

                // Akıllı Tahta Kilit Politikası
                if (bsu.smartboard_lock_policy() > 0) {
                    OkidiaConfig::setSmartboardLockPolicy(bsu.smartboard_lock_policy());
                    OkidiaDatabase::instance().setMetaValue("smartboard_lock_policy", QString::number(bsu.smartboard_lock_policy()));
                    Logger::info(QObject::tr("Broker: Akıllı tahta kilit politikası güncellendi: %1").arg(bsu.smartboard_lock_policy()));
                }
            }
        }
        return true;
    }

    // ── DB_BACKUP_REQUEST ────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_BACKUP_REQUEST) {
        okidia::DbBackupRequest dbr;
        if (dbr.ParseFromString(env.payload())) {
            QString requesterId = m_ctx->registry->authenticatedUserId(conn);
            if (requesterId.isEmpty()) requesterId = QString::fromStdString(dbr.requester_user_id());
            
            UserRecord u = OkidiaDatabase::instance().userById(requesterId);
            bool isYM = (u.role == "YM");
            if (!isYM && (dbr.include_audit() || dbr.include_chat())) {
                Logger::warning(QObject::tr("Broker: Yetkisiz yedekleme talebi reddedildi (YM değil) ← ") + requesterId);
                okidia::DbBackupResponse resp;
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem. Tam sistem yedeği yalnızca Yönetici (YM) tarafından alınabilir.");
                okidia::Envelope re;
                re.set_type(okidia::MessageType::DB_BACKUP_RESPONSE);
                re.set_payload(resp.SerializeAsString());
                conn->sendEnvelope(re);
                return true;
            }

            if (isYM) {
                Logger::info(QObject::tr("Broker: Sistem yedeği veya DB eşitleme isteniyor (YM) ← ") + requesterId);
            } else {
                Logger::info(QObject::tr("Broker: Veritabanı önbellek senkronizasyonu isteniyor ← ") + requesterId);
            }
            
            QString tempDb = QDir::tempPath() + "/okidia_backup_" + OkidiaUtils::generateUuid() + ".db";
            okidia::DbBackupResponse resp;
            
            if (OkidiaDatabase::instance().vacuumInto(tempDb)) {
                QFile f(tempDb);
                if (f.open(QIODevice::ReadOnly)) {
                    QByteArray data = f.readAll();
                    f.close();
                    QFile::remove(tempDb);
                    
                    QString keyPath = QFile::exists(OkidiaConfig::brokerCertDir() + "/broker.key") ? OkidiaConfig::brokerCertDir() + "/broker.key" : "../certs/test/server.key";
                    QByteArray keyRawData;
                    QFile kf(keyPath);
                    if (kf.open(QIODevice::ReadOnly)) {
                        keyRawData = kf.readAll();
                        kf.close();
                    }
                    QByteArray keyData = QCryptographicHash::hash(keyRawData, QCryptographicHash::Sha256);
                    
                    QByteArray encrypted = AesGcm::encrypt(data, keyData);
                    resp.set_success(true);
                    resp.set_encrypted_db(encrypted.toStdString());

                    // Audit veritabanı (İstenmişse ve yetkiliyse)
                    if (dbr.include_audit() && isYM) {
                        QString tempAuditDb = QDir::tempPath() + "/okidia_backup_audit_" + OkidiaUtils::generateUuid() + ".db";
                        if (OkidiaAuditDatabase::instance().vacuumInto(tempAuditDb)) {
                            QFile af(tempAuditDb);
                            if (af.open(QIODevice::ReadOnly)) {
                                QByteArray auditData = af.readAll();
                                af.close();
                                QFile::remove(tempAuditDb);
                                QByteArray encAudit = AesGcm::encrypt(auditData, keyData);
                                resp.set_encrypted_audit_db(encAudit.toStdString());
                                resp.set_has_audit(true);
                                Logger::info(QObject::tr("Broker: okidia_audit.db yedeğe eklendi."));
                            }
                        }
                    }

                    // Chat veritabanı (İstenmişse ve yetkiliyse)
                    if (dbr.include_chat() && isYM) {
                        QString tempChatDb = QDir::tempPath() + "/okidia_backup_chat_" + OkidiaUtils::generateUuid() + ".db";
                        if (OkidiaChatDatabase::instance().vacuumInto(tempChatDb)) {
                            QFile cf(tempChatDb);
                            if (cf.open(QIODevice::ReadOnly)) {
                                QByteArray chatData = cf.readAll();
                                cf.close();
                                QFile::remove(tempChatDb);
                                QByteArray encChat = AesGcm::encrypt(chatData, keyData);
                                resp.set_encrypted_chat_db(encChat.toStdString());
                                resp.set_has_chat(true);
                                Logger::info(QObject::tr("Broker: okidia_chat.db yedeğe eklendi."));
                            }
                        }
                    }

                    // Tam Kimlik Paketi (Identity Bundle) sadece YM için eklenir!
                    if (isYM) {
                        QStringList criticalFiles = {"ca.crt", "ca.key", "ca.srl", "broker.crt", "broker.key"};
                        QDir cert_dir(OkidiaConfig::brokerCertDir());
                        for (const QString& fName : criticalFiles) {
                            QFile cf(cert_dir.absoluteFilePath(fName));
                            if (cf.exists() && cf.open(QIODevice::ReadOnly)) {
                                auto* idFile = resp.add_identity_bundle();
                                idFile->set_name(fName.toStdString());
                                idFile->set_content(cf.readAll().toStdString());
                                cf.close();
                            }
                        }
                    }
                    // Geriye dönük uyumluluk için broker.key'i ayrıca da set et
                    QFile kf_legacy(keyPath);
                    if (kf_legacy.open(QIODevice::ReadOnly)) {
                        resp.set_broker_key(kf_legacy.readAll().toStdString());
                        kf_legacy.close();
                    }
                    Logger::info(QObject::tr("Broker: Sistem yedeği başarıyla tamamlandı."));
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Yedek dosyası okunamadı.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("VACUUM INTO başarısız oldu.");
            }
            
            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_BACKUP_RESPONSE);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_RESTORE_REQUEST ───────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_RESTORE_REQUEST) {
        okidia::DbRestoreRequest req;
        if (req.ParseFromString(env.payload())) {
            Logger::info(QObject::tr("Broker: Veritabanı geri yükleme talebi alındı"));

            // Yetki Kontrolü: Ya kayıtlı bir YM Master olmalı, ya da geçerli bir Setup Code göndermeli
            bool authorized = false;
            if (m_ctx->registry->isMasterConnection(conn)) {
                QString mid = m_ctx->registry->authenticatedUserId(conn);
                UserRecord mu = OkidiaDatabase::instance().userById(mid);
                if (mu.role == "YM") {
                    authorized = true;
                }
            }
            if (!authorized && !req.setup_code().empty()) {
                if (QString::fromStdString(req.setup_code()) == m_ctx->setupCode) {
                    authorized = true;
                    Logger::info(QObject::tr("Broker: Geri yükleme isteği Setup Code ile doğrulandı."));
                } else {
                    Logger::warning(QObject::tr("Broker: Hatalı Setup Code ile geri yükleme denemesi!"));
                }
            }

            if (!authorized) {
                okidia::DbRestoreResponse resp;
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz erişim. Sistem yedeği yalnızca Yönetici (YM) veya geçerli kurulum kodu ile geri yüklenebilir.");
                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::DB_RESTORE_RESPONSE);
                resEnv.set_payload(resp.SerializeAsString());
                conn->sendEnvelope(resEnv);
                return true;
            }

            // Geçici bir klasöre kimlik paketini aç
            QString tempDir = QDir::tempPath() + "/okidia_restore_" + OkidiaUtils::generateUuid();
            QDir().mkpath(tempDir);
            
            bool hasIdentity = false;
            if (req.identity_bundle_size() > 0) {
                for (int i = 0; i < req.identity_bundle_size(); ++i) {
                    const auto& idFile = req.identity_bundle(i);
                    QFile f(tempDir + "/" + QString::fromStdString(idFile.name()));
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(QByteArray::fromStdString(idFile.content()));
                        f.close();
                        hasIdentity = true;
                    }
                }
            } else if (!req.broker_key().empty()) {
                QFile kf_restore(tempDir + "/broker.key");
                if (kf_restore.open(QIODevice::WriteOnly)) {
                    kf_restore.write(QByteArray::fromStdString(req.broker_key()));
                    kf_restore.close();
                    hasIdentity = true;
                }
            }

            QByteArray encrypted = QByteArray::fromStdString(req.encrypted_db());
            okidia::DbRestoreResponse resp;

            QString kp = hasIdentity ? (tempDir + "/broker.key") : 
                        (QFile::exists(OkidiaConfig::brokerCertDir() + "/broker.key") ? OkidiaConfig::brokerCertDir() + "/broker.key" : "../certs/test/server.key");
            
            QByteArray decrypted;
            QByteArray keyData;
            if (encrypted.startsWith("SQLite format 3")) {
                decrypted = encrypted;
            } else {
                QFile kf(kp);
                if (kf.open(QIODevice::ReadOnly)) {
                    keyData = QCryptographicHash::hash(kf.readAll(), QCryptographicHash::Sha256);
                    kf.close();
                    decrypted = AesGcm::decrypt(encrypted, keyData);
                }
            }

            if (decrypted.isEmpty()) {
                resp.set_success(false);
                resp.set_error_msg("Yedek dosyası çözülemedi (geçersiz anahtar veya bozuk veri). Kimlik değiştirilmedi.");
                QDir(tempDir).removeRecursively();
            } else {
                QString tempRestoreDb = tempDir + "/okidia.db";
                QFile tf(tempRestoreDb);
                if (tf.open(QIODevice::WriteOnly)) {
                    tf.write(decrypted);
                    tf.close();

                    if (OkidiaDatabase::instance().restoreFrom(tempRestoreDb)) {
                        // Audit veritabanı geri yükle
                        if (req.restore_audit() && !req.encrypted_audit_db().empty() && !keyData.isEmpty()) {
                            QByteArray encAudit = QByteArray::fromStdString(req.encrypted_audit_db());
                            QByteArray decAudit = AesGcm::decrypt(encAudit, keyData);
                            if (!decAudit.isEmpty()) {
                                QString tempAuditDb = tempDir + "/okidia_audit.db";
                                QFile af(tempAuditDb);
                                if (af.open(QIODevice::WriteOnly)) {
                                    af.write(decAudit);
                                    af.close();
                                    OkidiaAuditDatabase::instance().restoreFrom(tempAuditDb);
                                    Logger::info(QObject::tr("Broker: okidia_audit.db başarıyla geri yüklendi."));
                                }
                            }
                        }

                        // Chat veritabanı geri yükle
                        if (req.restore_chat() && !req.encrypted_chat_db().empty() && !keyData.isEmpty()) {
                            QByteArray encChat = QByteArray::fromStdString(req.encrypted_chat_db());
                            QByteArray decChat = AesGcm::decrypt(encChat, keyData);
                            if (!decChat.isEmpty()) {
                                QString tempChatDb = tempDir + "/okidia_chat.db";
                                QFile cf(tempChatDb);
                                if (cf.open(QIODevice::WriteOnly)) {
                                    cf.write(decChat);
                                    cf.close();
                                    OkidiaChatDatabase::instance().restoreFrom(tempChatDb);
                                    Logger::info(QObject::tr("Broker: okidia_chat.db başarıyla geri yüklendi."));
                                }
                            }
                        }

                        // Sertifikaları asıl yerine taşı
                        if (hasIdentity) {
                            QDir cert_dir(OkidiaConfig::brokerCertDir());
                            if (!cert_dir.exists()) cert_dir.mkpath(".");
                            QDir tDir(tempDir);
                            for (const QString& fName : tDir.entryList(QDir::Files)) {
                                if (fName.endsWith(".db")) continue;
                                QFile::remove(cert_dir.absoluteFilePath(fName));
                                QFile::copy(tDir.absoluteFilePath(fName), cert_dir.absoluteFilePath(fName));
                                Logger::info(QObject::tr("Broker: Sertifika geri yüklendi → ") + fName);
                            }
                        }

                        resp.set_success(true);
                        Logger::info(QObject::tr("Broker: Veritabanı ve kimlik başarıyla geri yüklendi. Servis yeniden başlatılıyor..."));
                        QTimer::singleShot(1000, [](){
                            QCoreApplication::exit(0);
                        });
                    } else {
                        resp.set_success(false);
                        resp.set_error_msg("Veritabanı geri yüklenirken hata oluştu.");
                    }
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Geçici dosya oluşturulamadı.");
                }
                QDir(tempDir).removeRecursively();
            }

            okidia::Envelope resEnv;
            resEnv.set_type(okidia::MessageType::DB_RESTORE_RESPONSE);
            resEnv.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(resEnv);
        }
        return true;
    }

    // ── LOG_TAIL_REQ (Broker veya Agent logları) ─────────────────────────────
    if (env.type() == okidia::MessageType::LOG_TAIL_REQ) {
        const QString targetId = QString::fromStdString(env.target_id());
        if (targetId.isEmpty() || targetId.compare("broker", Qt::CaseInsensitive) == 0) {
            okidia::LogTailRequest req;
            if (req.ParseFromString(env.payload())) {
                okidia::LogTailResponse resp;
                resp.set_success(true);
                QString targetFile = QString::fromStdString(req.target_file());
                QString logDir = Logger::logDir();
                QString fullPath = targetFile.isEmpty() ? Logger::logFilePath() : (logDir + "/" + targetFile);
                
                QFileInfo fi(fullPath);
                if (fi.exists()) {
                    resp.set_file_name(fi.fileName().toStdString());
                    resp.set_file_size(fi.size());

                    QDateTime fromTime;
                    if (!req.from_time().empty()) {
                        fromTime = QDateTime::fromString(QString::fromStdString(req.from_time()), "yyyy-MM-dd hh:mm:ss");
                        if (!fromTime.isValid()) {
                            QTime t = QTime::fromString(QString::fromStdString(req.from_time()), "hh:mm:ss");
                            if (t.isValid()) fromTime = QDateTime(QDate::currentDate(), t);
                        }
                    }

                    QDateTime toTime;
                    if (!req.to_time().empty()) {
                        toTime = QDateTime::fromString(QString::fromStdString(req.to_time()), "yyyy-MM-dd hh:mm:ss");
                        if (!toTime.isValid()) {
                            QTime t = QTime::fromString(QString::fromStdString(req.to_time()), "hh:mm:ss");
                            if (t.isValid()) toTime = QDateTime(QDate::currentDate(), t);
                        }
                    }

                    QString levelFilter = QString::fromStdString(req.level_filter());
                    QString textFilter = QString::fromStdString(req.search_query());
                    int maxLines = req.max_lines();

                    QStringList lines = Logger::readLogLines(fullPath, maxLines, fromTime, toTime, levelFilter, textFilter);
                    for (const QString& l : lines) {
                        resp.add_lines(l.toStdString());
                    }
                } else {
                    resp.set_success(false);
                    resp.set_error_message(QObject::tr("Log dosyası bulunamadı: %1").arg(targetFile).toStdString());
                }

                auto fileList = Logger::availableLogFiles(logDir);
                for (const auto& f : fileList) {
                    resp.add_available_files(f.fileName.toStdString());
                }

                okidia::Envelope re;
                re.set_type(okidia::MessageType::LOG_TAIL_RES);
                re.set_target_id("broker");
                re.set_payload(resp.SerializeAsString());
                conn->sendEnvelope(re);
            }
            return true;
        }
        // Hedef agent ise forward et
        m_ctx->registry->forwardToClient(targetId, env);
        return true;
    }

    // ── LOG_TAIL_RES (Agent'tan Master'a yönlendir) ─────────────────────────
    if (env.type() == okidia::MessageType::LOG_TAIL_RES) {
        QString senderClientId;
        for (const auto& c : m_ctx->registry->allClients()) {
            if (c.connection == conn) {
                senderClientId = c.clientId;
                break;
            }
        }
        if (!senderClientId.isEmpty()) {
            okidia::Envelope fwd = env;
            fwd.set_target_id(senderClientId.toStdString());
            m_ctx->registry->forwardToMaster(senderClientId, fwd);
        }
        return true;
    }

    // ── ROLLBACK_TO_STABLE_REQ (Master -> Agent Yönlendirme) ──────────────────
    if (env.type() == okidia::MessageType::ROLLBACK_TO_STABLE_REQ) {
        const QString targetId = QString::fromStdString(env.target_id());
        if (!targetId.isEmpty()) {
            Logger::info(QObject::tr("SystemHandler: ROLLBACK_TO_STABLE_REQ hedef ajana iletiliyor -> %1").arg(targetId));
            m_ctx->registry->forwardToClient(targetId, env);
        }
        return true;
    }

    // ── ROLLBACK_TO_STABLE_RES (Agent -> Master Yönlendirme) ──────────────────
    if (env.type() == okidia::MessageType::ROLLBACK_TO_STABLE_RES) {
        QString senderClientId;
        for (const auto& c : m_ctx->registry->allClients()) {
            if (c.connection == conn) {
                senderClientId = c.clientId;
                break;
            }
        }
        if (!senderClientId.isEmpty()) {
            Logger::info(QObject::tr("SystemHandler: ROLLBACK_TO_STABLE_RES Master'a iletiliyor (Kaynak: %1)").arg(senderClientId));
            okidia::Envelope fwd = env;
            fwd.set_target_id(senderClientId.toStdString());
            m_ctx->registry->forwardToMaster(senderClientId, fwd);
        }
        return true;
    }

    // ── BACKUP_REPLICA_PULL (Master'ın yedeği parça parça çekmesi) ───────────
    if (env.type() == okidia::MessageType::BACKUP_REPLICA_PULL) {
        okidia::BackupReplicaPull pull;
        if (pull.ParseFromString(env.payload())) {
            const QString requesterId = m_ctx->registry->authenticatedUserId(conn).isEmpty()
                                        ? peerId
                                        : m_ctx->registry->authenticatedUserId(conn);
            UserRecord u = OkidiaDatabase::instance().userById(requesterId);
            if (u.role != "YM" && !u.isBackupReplica) {
                Logger::warning(QObject::tr("Broker: Yetkisiz kullanıcıdan BACKUP_REPLICA_PULL isteği engellendi: ") + requesterId);
                return true;
            }
            const QString connMid = m_ctx->registry->authenticatedMachineId(conn);
            const QString connHost = m_ctx->registry->authenticatedHostname(conn);
            if (!OkidiaDatabase::instance().isReplicaDeviceAuthorized(requesterId, connMid, connHost)) {
                Logger::warning(QObject::tr("Broker: %1 kullanıcısının tanımlı yedek bilgisayarı (%2) dışındaki cihazdan (%3) BACKUP_REPLICA_PULL isteği reddedildi.")
                                .arg(u.username, u.replicaHostname, connHost));
                return true;
            }

            QString reqFilename = QFileInfo(QString::fromStdString(pull.filename())).fileName();
            QString fullPath = OkidiaConfig::dbDir() + "/backups/" + reqFilename;
            QFileInfo fi(fullPath);
            if (!fi.exists() || !fi.isFile()) {
                Logger::warning(QObject::tr("Broker: İstenen replika yedeği bulunamadı: ") + fullPath);
                return true;
            }

            qint64 totalSize = fi.size();
            qint64 startOffset = static_cast<qint64>(pull.offset());
            Logger::info(QObject::tr("Broker: Replika akışı başlatılıyor → %1 (Boyut: %2 KB, Başlangıç ofseti: %3)")
                         .arg(reqFilename).arg(totalSize / 1024).arg(startOffset));

            // Chunked streaming with throttling (512 KB dilimler, 10 ms aralık)
            QPointer<Connection> safeConn(conn);
            auto sendNextChunk = std::make_shared<std::function<void(qint64)>>();
            *sendNextChunk = [safeConn, fullPath, reqFilename, totalSize, sendNextChunk](qint64 offset) {
                if (!safeConn) {
                    Logger::warning(QObject::tr("Broker: Replika akışı sırasında bağlantı koptu → ") + reqFilename);
                    return;
                }

                QFile file(fullPath);
                if (!file.open(QIODevice::ReadOnly)) {
                    Logger::error(QObject::tr("Broker: Replika dosyası açılamadı → ") + fullPath);
                    return;
                }

                if (!file.seek(offset)) {
                    Logger::error(QObject::tr("Broker: Replika dosyasında seek hatası → ") + QString::number(offset));
                    return;
                }

                const qint64 CHUNK_SIZE = 512 * 1024; // 512 KB
                QByteArray chunkData = file.read(CHUNK_SIZE);
                file.close();

                bool isLast = (offset + chunkData.size() >= totalSize) || chunkData.isEmpty();

                okidia::BackupReplicaPush push;
                push.set_filename(reqFilename.toStdString());
                push.set_total_size(static_cast<uint64_t>(totalSize));
                push.set_offset(static_cast<uint64_t>(offset));
                push.set_is_last_chunk(isLast);
                push.set_package_data(chunkData.toStdString());

                okidia::Envelope chunkEnv;
                chunkEnv.set_type(okidia::MessageType::BACKUP_REPLICA_PUSH);
                chunkEnv.set_payload(push.SerializeAsString());

                safeConn->sendEnvelope(chunkEnv);

                if (isLast) {
                    Logger::info(QObject::tr("Broker: Replika akışı başarıyla tamamlandı → ") + reqFilename);
                } else {
                    qint64 nextOffset = offset + chunkData.size();
                    QTimer::singleShot(10, [sendNextChunk, nextOffset]() {
                        (*sendNextChunk)(nextOffset);
                    });
                }
            };

            (*sendNextChunk)(startOffset);
        }
        return true;
    }

    return false;
}

