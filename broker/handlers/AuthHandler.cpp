#include "AuthHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/OkidiaAuditDatabase.h"
#include "../../common/OkidiaConfig.h"
#include "../CertSigner.h"

#include <QFile>
#include <QSysInfo>
#include <QDateTime>
#include <QNetworkInterface>
#include <QUuid>

AuthHandler::AuthHandler(BrokerContext* ctx) : m_ctx(ctx) {}

bool AuthHandler::handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster) {
    // ── VERIFY_SETUP_CODE_REQUEST ──────────────────────────────────────────
    if (env.type() == okidia::MessageType::VERIFY_SETUP_CODE_REQUEST) {
        okidia::VerifySetupCodeRequest req;
        if (!req.ParseFromString(env.payload())) {
            Logger::error(QObject::tr("Broker: VERIFY_SETUP_CODE_REQUEST parse hatası"));
            return true;
        }
        okidia::VerifySetupCodeResponse resp;
        const QString code = OkidiaDatabase::instance().metaValue("setup_code");
        const bool used    = OkidiaDatabase::instance().metaValue("setup_code_used") == "1";
        if (used || code != QString::fromStdString(req.setup_code())) {
            resp.set_success(false);
            resp.set_error_msg(used ? "Kurulum kodu daha önce kullanılmış."
                                    : "Geçersiz kurulum kodu.");
        } else {
            resp.set_success(true);
        }
        okidia::Envelope re;
        re.set_type(okidia::MessageType::VERIFY_SETUP_CODE_RESPONSE);
        re.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(re);
        return true;
    }

    // ── SETUP_REQUEST ──────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::SETUP_REQUEST) {
        Logger::info(QObject::tr("Broker: SETUP_REQUEST ← ") + conn->peerAddress());
        okidia::SetupRequest req;
        if (!req.ParseFromString(env.payload())) {
            Logger::error(QObject::tr("Broker: SETUP_REQUEST parse hatası"));
            return true;
        }
        okidia::SetupResponse resp;
        const QString code = OkidiaDatabase::instance().metaValue("setup_code");
        const bool used    = OkidiaDatabase::instance().metaValue("setup_code_used") == "1";
        Logger::info(QObject::tr("Broker: Gelen kod → ")
            + QString::fromStdString(req.setup_code())
            + QObject::tr(" | Beklenen → ") + code
            + QObject::tr(" | Kullanıldı: ") + (used ? QObject::tr("evet") : QObject::tr("hayır")));
        if (used || code != QString::fromStdString(req.setup_code())) {
            resp.set_success(false);
            resp.set_error_msg(used ? "Kurulum kodu daha önce kullanılmış."
                                    : "Geçersiz kurulum kodu.");
            Logger::warning(QObject::tr("Broker: SETUP_REQUEST reddedildi"));
        } else {
            QString err;
            const QByteArray certPem = CertSigner::signCsr(
                QByteArray::fromStdString(req.csr_pem()), err);
            if (certPem.isEmpty()) {
                resp.set_success(false);
                resp.set_error_msg("Sertifika imzalama başarısız: " + err.toStdString());
            } else {
                QByteArray caPem;
                QFile caFile(OkidiaConfig::brokerCertDir() + "/ca.crt");
                if (caFile.open(QIODevice::ReadOnly)) {
                    caPem = caFile.readAll();
                    caFile.close();
                }
                resp.set_success(true);
                resp.set_master_cert(certPem.toStdString());
                resp.set_ca_cert(caPem.toStdString());
                OkidiaDatabase::instance().setMetaValue("setup_code_used", "1");
                QFile::remove(OkidiaConfig::dbDir() + "/setup_code.txt");
#if defined(Q_OS_WIN)
                QFile::remove(OkidiaConfig::systemBasePath() + "/Broker/setup_code.txt");
#endif
                m_ctx->setupCode.clear();
                if (m_ctx->broadcastStatus) {
                    m_ctx->broadcastStatus();
                }
                if (!req.username().empty()) {
                    UserRecord ym;
                    ym.id           = OkidiaUtils::generateUuid();
                    ym.username     = QString::fromStdString(req.username());
                    ym.passwordHash = QString::fromStdString(req.password_hash());
                    ym.role         = "YM";
                    ym.displayName  = QString::fromStdString(req.display_name());
                    ym.mustChangePw = false;
                    ym.isActive     = true;
                    ym.createdAt    = OkidiaUtils::nowSecs();
                    OkidiaDatabase::instance().upsertUser(ym);
                    Logger::info(QObject::tr("Broker: YM hesabı oluşturuldu → ") + ym.username);

                    if (!req.recovery_key_hash().empty()) {
                        OkidiaDatabase::instance().setMetaValue("recovery_key_hash", QString::fromStdString(req.recovery_key_hash()));
                        Logger::info(QObject::tr("Broker: Sistem kurtarma kodu kaydedildi."));
                    }
                }

                const QString schoolName = QString::fromStdString(req.school_name()).trimmed();
                if (!schoolName.isEmpty()) {
                    OkidiaDatabase::instance().setMetaValue("school_name", schoolName);
                    resp.set_school_name(schoolName.toStdString());
                    Logger::info(QObject::tr("Broker: Okul/Kurum adı kaydedildi → ") + schoolName);
                }

                Logger::info(QObject::tr("Broker: Master sertifikası imzalandı"));
            }
        }
        Logger::info(QObject::tr("Broker: SETUP_RESPONSE → success=")
            + QString(resp.success() ? QObject::tr("true") : QObject::tr("false")));
        okidia::Envelope re;
        re.set_type(okidia::MessageType::SETUP_RESPONSE);
        re.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(re);
        return true;
    }

    // ── LOGIN_REQUEST ──────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::LOGIN_REQUEST) {
        *isMaster = true;
        m_ctx->registry->addMasterConnection(conn);
        m_ctx->broadcastStatus();

        okidia::LoginRequest req;
        req.ParseFromString(env.payload());
        const QString username = QString::fromStdString(req.username());
        const QString password = QString::fromStdString(req.password_hash());
        const QString masterVer = QString::fromStdString(req.app_version());
        const QString rawMasterOsTag = QString::fromStdString(req.os_tag());
        const QString masterOsPretty = QString::fromStdString(req.os_pretty_name());
        QString masterOsTag = OkidiaUtils::normalizeOsTag(rawMasterOsTag);
        if (masterOsTag.isEmpty() || masterOsTag == "unknown") {
            masterOsTag = OkidiaUtils::normalizeOsTag(masterOsPretty);
        }
        const QString masterArch = OkidiaUtils::normalizeArch(QString::fromStdString(req.os_arch()));
        const QString masterOsDisplay = masterOsPretty.isEmpty() ? masterOsTag : masterOsPretty;

        const QString brokerVer = Okidia::Version::string();
        const QString brokerOsPretty = OkidiaUtils::detectLocalOsPrettyName();
        const QString brokerOsTag = OkidiaUtils::detectLocalOsTag();
        const QString brokerArch = OkidiaUtils::detectLocalOsArch();

        // Broker'ın tüm aktif, loopback olmayan IPv4 adreslerini topla
        QStringList brokerIpList;
        for (const auto& iface : QNetworkInterface::allInterfaces()) {
            if (!iface.flags().testFlag(QNetworkInterface::IsUp) || 
                iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const auto& entry : iface.addressEntries()) {
                QHostAddress ip = entry.ip();
                if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback()) {
                    QString ipStr = ip.toString();
                    if (!brokerIpList.contains(ipStr)) {
                        brokerIpList.append(ipStr);
                    }
                }
            }
        }

        UserRecord u = OkidiaDatabase::instance().userByUsername(username);
        okidia::LoginResponse resp;
        resp.set_broker_version(brokerVer.toStdString());
        resp.set_broker_os(brokerOsPretty.toStdString());
        resp.set_broker_os_tag(brokerOsTag.toStdString());
        resp.set_broker_arch(brokerArch.toStdString());
        for (const QString& bip : brokerIpList) {
            resp.add_broker_ips(bip.toStdString());
        }

        // ── RATE LIMITING & BRUTE FORCE KORUMASI ──────────────────
        cleanupExpiredLimits();
        const QString peerIp = conn->peerAddress();
        const qint64 now = OkidiaUtils::nowSecs();

        // 1. IP Bloklama Kontrolü
        if (m_failedIpAttempts.contains(peerIp) && m_failedIpAttempts[peerIp].blockedUntil > now) {
            const qint64 remainingSecs = m_failedIpAttempts[peerIp].blockedUntil - now;
            resp.set_success(false);
            resp.set_error_msg(QObject::tr("Çok sayıda başarısız deneme yapıldı. Güvenlik nedeniyle IP adresiniz %1 saniye süreyle engellendi.")
                                  .arg(remainingSecs).toStdString());
            okidia::Envelope re;
            re.set_type(okidia::MessageType::LOGIN_RESPONSE);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
            Logger::warning(QObject::tr("Broker: Engelli IP'den login isteği reddedildi → ") + peerIp + " (" + QString::number(remainingSecs) + " sn kaldı)");
            return true;
        }

        // 2. Hesap Kilitleme Kontrolü
        if (!username.isEmpty() && m_failedUserAttempts.contains(username) && m_failedUserAttempts[username].lockedUntil > now) {
            const qint64 remainingMins = qMax<qint64>(1, (m_failedUserAttempts[username].lockedUntil - now + 59) / 60);
            resp.set_success(false);
            resp.set_error_msg(QObject::tr("Bu hesap çok sayıda hatalı deneme nedeniyle geçici olarak kilitlendi. Lütfen %1 dakika sonra tekrar deneyin.")
                                  .arg(remainingMins).toStdString());
            okidia::Envelope re;
            re.set_type(okidia::MessageType::LOGIN_RESPONSE);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
            Logger::warning(QObject::tr("Broker: Kilitli hesaba login isteği reddedildi → ") + username + " (IP: " + peerIp + ", " + QString::number(remainingMins) + " dk kaldı)");
            return true;
        }

        if (u.id.isEmpty() || !u.isActive || u.passwordHash != password) {
            // Başarısız giriş denemesi: sayaçları artır
            auto& ipInfo = m_failedIpAttempts[peerIp];
            ipInfo.attempts++;

            if (!username.isEmpty()) {
                auto& userInfo = m_failedUserAttempts[username];
                userInfo.attempts++;

                if (userInfo.attempts >= MAX_USER_ATTEMPTS) {
                    userInfo.lockedUntil = now + USER_LOCK_DURATION_SECS;
                    Logger::warning(QObject::tr("Broker: GÜVENLİK ALARMI! %1 hesabı art arda %2 hatalı deneme nedeniyle %3 dakika kilitlendi!")
                                    .arg(username).arg(userInfo.attempts).arg(USER_LOCK_DURATION_SECS / 60));
                    OkidiaAuditDatabase::instance().logCommand(
                        peerIp,
                        username,
                        QObject::tr("Hesap brute-force şüphesiyle %1 dakika süreyle kilitlendi (Hatalı deneme: %2).")
                            .arg(USER_LOCK_DURATION_SECS / 60).arg(userInfo.attempts),
                        QStringLiteral("Security")
                    );
                }
            }

            if (ipInfo.attempts >= MAX_IP_ATTEMPTS) {
                ipInfo.blockedUntil = now + IP_BLOCK_DURATION_SECS;
                Logger::warning(QObject::tr("Broker: GÜVENLİK ALARMI! %1 IP adresi art arda %2 hatalı deneme nedeniyle %3 saniye engellendi!")
                                .arg(peerIp).arg(ipInfo.attempts).arg(IP_BLOCK_DURATION_SECS));
                OkidiaAuditDatabase::instance().logCommand(
                    peerIp,
                    username.isEmpty() ? QStringLiteral("Unknown") : username,
                    QObject::tr("IP adresi brute-force şüphesiyle %1 saniye süreyle engellendi (Hatalı deneme: %2).")
                        .arg(IP_BLOCK_DURATION_SECS).arg(ipInfo.attempts),
                    QStringLiteral("Security")
                );
            }

            resp.set_success(false);
            if (ipInfo.blockedUntil > now) {
                const qint64 remainingSecs = ipInfo.blockedUntil - now;
                resp.set_error_msg(QObject::tr("Çok sayıda başarısız deneme yapıldı. Güvenlik nedeniyle IP adresiniz %1 saniye süreyle engellendi.")
                                      .arg(remainingSecs).toStdString());
            } else if (!username.isEmpty() && m_failedUserAttempts.value(username).lockedUntil > now) {
                const qint64 remainingMins = qMax<qint64>(1, (m_failedUserAttempts[username].lockedUntil - now + 59) / 60);
                resp.set_error_msg(QObject::tr("Bu hesap çok sayıda hatalı deneme nedeniyle geçici olarak kilitlendi. Lütfen %1 dakika sonra tekrar deneyin.")
                                      .arg(remainingMins).toStdString());
            } else {
                resp.set_error_msg(QObject::tr("Kullanıcı adı veya şifre hatalı.").toStdString());
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::LOGIN_RESPONSE);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
            Logger::warning(QObject::tr("Broker: Hatalı login denemesi → ") + username + " (" + peerIp + ") [IP deneme: " + QString::number(ipInfo.attempts) + "]");
            return true;
        }

        // Başarılı giriş: Bu IP ve kullanıcı için sayaçları sıfırla
        resetIpRateLimit(peerIp);
        resetAccountLockout(username);

        // ── SÜRÜM DENETİMİ (VERSION GATE) ───────────────────────────
        if (!masterVer.isEmpty()) {
            int cmp = OkidiaUtils::compareVersions(masterVer, brokerVer);
            if (cmp > 0 && u.role != "YM") {
                okidia::LoginResponse vResp;
                vResp.set_success(false);
                vResp.set_broker_version(brokerVer.toStdString());
                vResp.set_broker_os(brokerOsPretty.toStdString());
                vResp.set_broker_os_tag(brokerOsTag.toStdString());
                vResp.set_broker_arch(brokerArch.toStdString());
                for (const QString& bip : brokerIpList) {
                    vResp.add_broker_ips(bip.toStdString());
                }
                vResp.set_error_msg(QObject::tr("Sunucu (Broker) sürümünüz (%1) bu Master sürümünden (%2) daha eski.\n\n"
                                               "• Yöneticinizden (YM) sunucuyu güncellemesini isteyebilir,\n"
                                               "• Veya sunucuyla uyumlu v%1 sürümünü 'https://okidia.net/download' adresinden indirip kurabilirsiniz.")
                                      .arg(brokerVer, masterVer).toStdString());
                
                okidia::Envelope re;
                re.set_type(okidia::MessageType::LOGIN_RESPONSE);
                re.set_payload(vResp.SerializeAsString());
                conn->sendEnvelope(re);
                Logger::warning(QObject::tr("Broker: Master sürüm uyuşmazlığı nedeniyle giriş reddedildi (OM/Gözlemci Master v") + masterVer + QObject::tr(" > Broker v") + brokerVer + ")");
                return true;
            }
        }

        QList<Connection*> toDisconnect;
        for (Connection* c : m_ctx->registry->masterConnections()) {
            const QString authedId = m_ctx->registry->authenticatedUserId(c);
            if (c != conn && !u.id.isEmpty() && !authedId.isEmpty() && authedId == u.id) {
                toDisconnect.append(c);
            }
        }
        for (Connection* c : toDisconnect) {
            const QString oldUser = m_ctx->registry->authenticatedUsername(c);
            const QString oldId = m_ctx->registry->authenticatedUserId(c);
            Logger::warning(QObject::tr("Broker: Aynı hesaptan yeni giriş yapıldı (Giriş yapan: %1 [ID: %2], Önceki: %3 [ID: %4]). Önceki bağlantı kısıtlı moda düşürülüyor: hedef_soket=%5, yeni_soket=%6")
                            .arg(u.username, u.id, oldUser, oldId, c->peerAddress(), conn->peerAddress()));
            okidia::Envelope forceEnv;
            forceEnv.set_type(okidia::MessageType::SESSION_RESTRICT_MODE);
            c->sendEnvelope(forceEnv);
            c->disconnect();
        }

        resp.set_success(true);
        resp.set_user_id(u.id.toStdString());
        resp.set_role(u.role.toStdString());
        resp.set_display_name(u.displayName.toStdString());
        resp.set_must_change_pw(u.mustChangePw);
        const QString schoolName = OkidiaDatabase::instance().metaValue("school_name").trimmed();
        if (!schoolName.isEmpty()) {
            resp.set_school_name(schoolName.toStdString());
        }
        // Fiziksel Master Cihazı kaydı (master_devices)
        QString masterHost = QString::fromStdString(req.hostname()).trimmed();
        QString masterMachineId = QString::fromStdString(req.machine_id()).trimmed();
        if (!masterHost.isEmpty()) {
            masterMachineId = QUuid::createUuidV5(QUuid(), "master-host:" + masterHost.toLower()).toString(QUuid::WithoutBraces);
        } else if (masterMachineId.isEmpty()) {
            masterMachineId = QUuid::createUuidV5(QUuid(), "master:" + conn->peerAddress() + ":" + masterHost).toString(QUuid::WithoutBraces);
        }
        if (masterHost.isEmpty()) {
            masterHost = conn->peerAddress();
        }

        m_ctx->registry->setAuthenticatedUser(conn, u.id, u.username, u.displayName, u.role, masterMachineId, masterHost);
        OkidiaDatabase::instance().updateLastLogin(u.id, masterVer, masterOsDisplay, masterOsTag, masterArch);

        // Eğer YM veya Replika OM kullanıcısının henüz atanmış bir yedek bilgisayarı yoksa ve
        // giriş yapılan bilgisayar başka bir kullanıcının yedek kasası değilse, ilk girişte bu cihaza bağla
        if ((u.role == "YM" || u.isBackupReplica) && u.replicaHostname.trimmed().isEmpty() && u.replicaMachineId.trimmed().isEmpty()) {
            if (OkidiaDatabase::instance().isReplicaDeviceAuthorized(u.id, masterMachineId, masterHost)) {
                OkidiaDatabase::instance().setUserBackupReplica(u.id, true, masterMachineId, masterHost);
                u.isBackupReplica = true;
                u.replicaMachineId = masterMachineId;
                u.replicaHostname = masterHost;
                Logger::info(QObject::tr("Broker: %1 (%2) kullanıcısının yedek replikasyon cihazı '%3' olarak kaydedildi.")
                             .arg(u.username, u.role, masterHost));
            }
        }

        MasterDeviceRecord mdev;
        mdev.machineId = masterMachineId;
        mdev.hostname = masterHost;
        mdev.ip = conn->peerAddress();
        mdev.osPrettyName = masterOsDisplay;
        mdev.osTag = masterOsTag;
        mdev.osArch = masterArch;
        mdev.masterVer = masterVer;
        mdev.lastUserId = u.id;
        mdev.lastUsername = u.username;
        mdev.lastSeen = OkidiaUtils::nowSecs();
        OkidiaDatabase::instance().upsertMasterDevice(mdev);

        if (m_ctx->broadcastDbSyncSignal) {
            m_ctx->broadcastDbSyncSignal();
        }
        Logger::info(QObject::tr("Broker: Ağ login başarılı → ") + username + " (" + conn->peerAddress() + ") [v" + masterVer + ", " + masterOsDisplay + " (" + masterOsTag + ")/" + masterArch + "]");

        // 1. Önce LOGIN_RESPONSE gönder
        okidia::Envelope re;
        re.set_type(okidia::MessageType::LOGIN_RESPONSE);
        re.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(re);

        // 1.01 Canlı istemci durumlarını Master'a tazele (Reconciliation)
        m_ctx->registry->sendAllClientsToMaster(conn);

        // 1.0 Broker zaman desync kontrolü (Grace period bildirimi)
        if (m_ctx->enrollmentManager) {
            m_ctx->enrollmentManager->onMasterLoginSuccess(conn);
        }

        // 1.1 Master(YM)'a güncel kaldırma parolası hash'ini bildir
        if (u.role == "YM") {
            okidia::AgentUninstallPassSet passMsg;
            passMsg.set_password_hash(OkidiaDatabase::instance().uninstallPasswordHash().toStdString());
            okidia::Envelope passEnv;
            passEnv.set_type(okidia::MessageType::AGENT_UNINSTALL_PASS_SET);
            passEnv.set_payload(passMsg.SerializeAsString());
            conn->sendEnvelope(passEnv);
        }

        // 1.2 YM veya İkincil Yedek Alıcısına (yalnızca tanımlı yedek bilgisayarından giriş yaptıysa) son hazır yedeği otomatik gönder
        if ((u.role == "YM" || u.isBackupReplica) && m_ctx->pushLatestBackupToUser) {
            if (OkidiaDatabase::instance().isReplicaDeviceAuthorized(u.id, masterMachineId, masterHost)) {
                QString uid = u.id;
                QTimer::singleShot(2000, [this, uid]() {
                    if (m_ctx->pushLatestBackupToUser) {
                        m_ctx->pushLatestBackupToUser(uid);
                    }
                });
            } else {
                Logger::info(QObject::tr("Broker: %1 (%2) tanımlı yedek bilgisayarı (%3) yerine farklı bir bilgisayardan (%4) giriş yaptı; .okbak yedeği bu bilgisayara aktarılmadı.")
                             .arg(u.username, u.role, u.replicaHostname, masterHost));
            }
        }

        // 1.3 Master için bekleyen/mevcut güncellemeyi kontrol et ve bildir
        if (m_ctx->transferManager) {
            m_ctx->transferManager->checkAndNotifyMasterUpdate(conn, masterVer, masterOsTag, masterArch);
        }

        // 2. Yeni bağlanan Master'a şu anda online olan diğer Master'ları bildir
        for (Connection* mConn : m_ctx->registry->masterConnections()) {
            if (mConn != conn && !m_ctx->registry->authenticatedUserId(mConn).isEmpty()) {
                const QString existingUser = m_ctx->registry->authenticatedUsername(mConn);
                const QString existingDisp = m_ctx->registry->authenticatedDisplayName(mConn);
                const QString existingRole = m_ctx->registry->authenticatedRole(mConn);
                okidia::ChatMemberEvent existingEvt;
                existingEvt.set_room_id("room_admin");
                existingEvt.set_client_id(("master:" + existingUser).toStdString());
                existingEvt.set_hostname(existingDisp.isEmpty() ? existingUser.toStdString() : existingDisp.toStdString());
                existingEvt.set_username(existingRole.toStdString());
                okidia::Envelope existEnv;
                existEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
                existEnv.set_payload(existingEvt.SerializeAsString());
                conn->sendEnvelope(existEnv);
            }
        }

        // 3. İlgili tüm odalardaki aktif üyeleri de yeni bağlanan Master'a bildir
        QSet<QString> allowedRoomIds;
        allowedRoomIds.insert("room_admin");
        if (u.role == "YM") {
            for (const auto& cls : OkidiaDatabase::instance().allClasses()) {
                allowedRoomIds.insert("room_" + cls.id);
            }
        } else if (u.role == "OM") {
            for (const QString& clsId : OkidiaDatabase::instance().classIdsForUser(u.id)) {
                allowedRoomIds.insert("room_" + clsId);
            }
        }

        if (m_ctx->activeRoomMembers) {
            for (auto it = m_ctx->activeRoomMembers->begin(); it != m_ctx->activeRoomMembers->end(); ++it) {
                if (it.key() == "room_admin") continue;
                if (!allowedRoomIds.contains(it.key())) continue;
                for (const okidia::ChatMemberEvent& evt : it.value()) {
                    okidia::Envelope roomMemberEnv;
                    roomMemberEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
                    roomMemberEnv.set_payload(evt.SerializeAsString());
                    conn->sendEnvelope(roomMemberEnv);
                }
            }
        }

        // 4. Sohbet penceresi açık olan İstemcilerin durumunu bildir
        if (m_ctx->agentWindowOpenMap) {
            for (auto it = m_ctx->agentWindowOpenMap->begin(); it != m_ctx->agentWindowOpenMap->end(); ++it) {
                if (it.value()) {
                    okidia::ChatWindowState cws;
                    cws.set_open(true);
                    okidia::Envelope cwsEnv;
                    cwsEnv.set_type(okidia::MessageType::CHAT_WINDOW_STATE);
                    cwsEnv.set_target_id(it.key().toStdString());
                    cwsEnv.set_sequence_id(qHash(it.key()));
                    cwsEnv.set_payload(cws.SerializeAsString());
                    conn->sendEnvelope(cwsEnv);
                }
            }
        }

        // 5. Diğer tüm bağlı Master'lara ve ilgili İstemcilere bu yeni Master'ın katıldığını bildir
        const QString masterClientId = "master:" + u.username;
        const QString masterDisp = u.displayName.isEmpty() ? u.username : u.displayName;

        QStringList targetRoomIds;
        targetRoomIds.append("room_admin");
        if (u.role == "YM") {
            for (const auto& cls : OkidiaDatabase::instance().allClasses()) {
                targetRoomIds.append("room_" + cls.id);
            }
        } else if (u.role == "OM") {
            for (const QString& clsId : OkidiaDatabase::instance().classIdsForUser(u.id)) {
                targetRoomIds.append("room_" + clsId);
            }
        }

        for (const QString& rId : targetRoomIds) {
            okidia::ChatMemberEvent joinEvt;
            joinEvt.set_room_id(rId.toStdString());
            joinEvt.set_client_id(masterClientId.toStdString());
            joinEvt.set_hostname(masterDisp.toStdString());
            joinEvt.set_username(u.role.toStdString());

            if (m_ctx->activeRoomMembers) {
                (*m_ctx->activeRoomMembers)[rId][masterClientId] = joinEvt;
            }
            if (m_ctx->connToRoomMembers) {
                (*m_ctx->connToRoomMembers)[conn].insert({rId, masterClientId});
            }

            okidia::Envelope joinEnv;
            joinEnv.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
            joinEnv.set_payload(joinEvt.SerializeAsString());

            for (Connection* mConn : m_ctx->registry->masterConnections()) {
                if (mConn != conn) {
                    mConn->sendEnvelope(joinEnv);
                }
            }

            if (rId != "room_admin") {
                QString targetClassId = rId.mid(5);
                for (const auto& c : m_ctx->registry->allClients()) {
                    const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                    if (rec.classId == targetClassId) {
                        m_ctx->registry->forwardToClient(c.clientId, joinEnv);
                    }
                }
            }
        }

        // 6. Bekleyen (offline) mesajları Master'a gönder
        auto pendingCommandsByUsername = OkidiaDatabase::instance().pendingCommands("master:" + u.username);
        auto pendingCommandsById = OkidiaDatabase::instance().pendingCommands("master:" + u.id);
        auto allPending = pendingCommandsByUsername + pendingCommandsById;
        if (!allPending.isEmpty()) {
            Logger::info(QObject::tr("Broker: Master için %1 bekleyen mesaj gönderiliyor → %2").arg(allPending.size()).arg(u.username));
            for (const auto& pcmd : allPending) {
                okidia::Envelope penv;
                if (penv.ParseFromArray(pcmd.payload.constData(), pcmd.payload.size())) {
                    conn->sendEnvelope(penv);
                    OkidiaDatabase::instance().deletePendingCommand(pcmd.id);
                }
            }
        }

        return true;
    }

    // ── SET_RECOVERY_KEY_REQ ───────────────────────────────────────────────
    if (env.type() == okidia::MessageType::SET_RECOVERY_KEY_REQ) {
        const QString userId = m_ctx->registry->authenticatedUserId(conn);
        if (userId.isEmpty()) return true;
        UserRecord u = OkidiaDatabase::instance().userById(userId);
        if (u.role != "YM") return true;

        okidia::SetRecoveryKeyRequest req;
        req.ParseFromString(env.payload());
        OkidiaDatabase::instance().setMetaValue("recovery_key_hash", QString::fromStdString(req.key_hash()));
        Logger::info(QObject::tr("Broker: Sistem kurtarma kodu güncellendi."));
        return true;
    }

    // ── PASSWORD_RESET_REQ ─────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::PASSWORD_RESET_REQ) {
        okidia::PasswordResetRequest req;
        req.ParseFromString(env.payload());
        const QString savedHash = OkidiaDatabase::instance().metaValue("recovery_key_hash");
        const QString savedSetupCode = OkidiaDatabase::instance().metaValue("setup_code");
        const QString sentKey = QString::fromStdString(req.recovery_key());
        
        const QString cleanKey = OkidiaUtils::cleanRecoveryKey(sentKey);
        const QString cleanHash = OkidiaUtils::sha256(cleanKey.toUtf8());
        
        const QString legacyKey = sentKey.trimmed().toUpper();
        const QString legacyHash = OkidiaUtils::sha256(legacyKey.toUtf8());

        bool isRecoveryKeyValid = (!savedHash.isEmpty() && (savedHash == cleanHash || savedHash == legacyHash));
        bool isSetupCodeValid = false;
        if (!savedSetupCode.isEmpty()) {
            QString cleanSavedCode = OkidiaUtils::cleanRecoveryKey(savedSetupCode);
            QString cleanSentCode = OkidiaUtils::cleanRecoveryKey(sentKey);
            if (cleanSavedCode == cleanSentCode ||
                cleanSavedCode.remove("OKIDIA") == cleanSentCode.remove("OKIDIA") ||
                savedSetupCode.trimmed().toUpper() == sentKey.trimmed().toUpper()) {
                isSetupCodeValid = true;
            }
        }
        
        okidia::PasswordResetResponse resp;
        if (isRecoveryKeyValid || isSetupCodeValid) {
            QList<UserRecord> users = OkidiaDatabase::instance().allUsers();
            QString ymId;
            for (const auto& u : users) if (u.role == "YM") { ymId = u.id; break; }
            if (!ymId.isEmpty()) {
                const QString newPwHash = QString::fromStdString(req.new_password_hash());
                if (newPwHash.isEmpty()) {
                    resp.set_success(true);
                    Logger::info(QObject::tr("Broker: Şifre sıfırlama kodu başarıyla doğrulandı (%1).")
                                 .arg(isSetupCodeValid ? QObject::tr("Kurulum Kodu") : QObject::tr("Kurtarma Kodu")));
                } else {
                    OkidiaDatabase::instance().setUserPassword(ymId, newPwHash, false);
                    UserRecord ymUser = OkidiaDatabase::instance().userById(ymId);
                    resetAccountLockout(ymUser.username);
                    resetIpRateLimit(conn->peerAddress());
                    resp.set_success(true);
                    Logger::info(QObject::tr("Broker: YM şifresi başarıyla sıfırlandı (%1).")
                                 .arg(isSetupCodeValid ? QObject::tr("Kurulum Kodu ile") : QObject::tr("Kurtarma Kodu ile")));
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("YM kullanıcısı bulunamadı.");
            }
        } else {
            resp.set_success(false);
            resp.set_error_msg("Geçersiz kurtarma veya kurulum kodu.");
        }
        okidia::Envelope re;
        re.set_type(okidia::MessageType::PASSWORD_RESET_RES);
        re.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(re);
        return true;
    }

    // ── JOIN_REQUEST ───────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::JOIN_REQUEST) {
        Logger::info(QObject::tr("Broker: JOIN_REQUEST ← ") + conn->peerAddress());
        okidia::JoinRequest req;
        if (!req.ParseFromString(env.payload())) {
            Logger::error(QObject::tr("Broker: JOIN_REQUEST parse hatası"));
            return true;
        }
        
        okidia::JoinResponse resp;
        JoinCodeRecord codeRec = OkidiaDatabase::instance().validateJoinCode(
            QString::fromStdString(req.join_code()));
        
        qint64 now = OkidiaUtils::nowSecs();
        
        if (codeRec.code.isEmpty() || codeRec.usedAt > 0 || codeRec.expiresAt < now) {
            resp.set_success(false);
            resp.set_error_msg(codeRec.code.isEmpty() ? "Geçersiz katılım kodu." :
                               (codeRec.usedAt > 0 ? "Bu kod zaten kullanılmış." : "Katılım kodunun süresi dolmuş."));
        } else {
            QString err;
            const QByteArray certPem = CertSigner::signCsr(
                QByteArray::fromStdString(req.csr_pem()), err, codeRec.userId);
            
            if (certPem.isEmpty()) {
                resp.set_success(false);
                resp.set_error_msg("Sertifika imzalama başarısız: " + err.toStdString());
            } else {
                QFile caFile(OkidiaConfig::brokerCertDir() + "/ca.crt");
                if (caFile.open(QIODevice::ReadOnly)) {
                    const QByteArray caPem = caFile.readAll();
                    resp.set_success(true);
                    resp.set_master_cert(certPem.toStdString());
                    resp.set_ca_cert(caPem.toStdString());

                    // Kullanıcı bilgilerini DB'den al ve yanıta ekle
                    UserRecord omUser = OkidiaDatabase::instance().userById(codeRec.userId);
                    if (!omUser.id.isEmpty()) {
                        resp.set_username(omUser.username.toStdString());

                        // Eğer kullanıcı kurulum wizard'ında şifre belirlediyse kaydet
                        const QString newPwHash = QString::fromStdString(req.password_hash()).trimmed();
                        if (!newPwHash.isEmpty()) {
                            omUser.passwordHash = newPwHash;
                            omUser.mustChangePw = false;
                            OkidiaDatabase::instance().upsertUser(omUser);
                            resp.set_must_change_pw(false);
                            Logger::info(QObject::tr("Broker: Kullanıcı şifresi JOIN sırasında güncellendi → ") + omUser.username);
                        } else {
                            // Şifre gönderilmemişse ilk girişte değiştirme zorunlu
                            omUser.mustChangePw = true;
                            OkidiaDatabase::instance().upsertUser(omUser);
                            resp.set_must_change_pw(true);
                            Logger::warning(QObject::tr("Broker: JOIN_REQUEST şifre hash'i boş → mustChangePw=true → ") + omUser.username);
                        }
                    } else {
                        Logger::warning(QObject::tr("Broker: JOIN_REQUEST için kullanıcı DB'de bulunamadı! codeRec.userId = ") + codeRec.userId);
                    }

                    const QString schoolName = OkidiaDatabase::instance().metaValue("school_name");
                    if (!schoolName.isEmpty()) {
                        resp.set_school_name(schoolName.toStdString());
                    }

                    OkidiaDatabase::instance().consumeJoinCode(codeRec.code);
                    Logger::info(QObject::tr("Broker: Katılım sertifikası imzalandı → ") + codeRec.userId);
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("CA sertifikası okunamadı.");
                }
            }
        }
        
        okidia::Envelope re;
        re.set_type(okidia::MessageType::JOIN_RESPONSE);
        re.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(re);
        return true;
    }

    // ── JOIN_CODE_REQUEST ──────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::JOIN_CODE_REQUEST) {
        okidia::JoinCodeRequest req;
        if (req.ParseFromString(env.payload())) {
            QString userId = QString::fromStdString(req.user_id());
            const QString requesterId = m_ctx->registry->authenticatedUserId(conn).isEmpty()
                                        ? peerId
                                        : m_ctx->registry->authenticatedUserId(conn);

            Logger::info(QObject::tr("Broker: Davet kodu isteniyor → ") + userId + QObject::tr(" (İsteyen: ") + requesterId + ")");
            UserRecord u = OkidiaDatabase::instance().userById(requesterId);
            UserRecord targetUser = OkidiaDatabase::instance().userById(userId);

            okidia::JoinCodeResponse resp;
            if (u.role != "YM") {
                resp.set_success(false);
                resp.set_error_msg("Bu işlem için yetkiniz yok (Sadece YM).");
                Logger::warning(QObject::tr("Broker: Yetkisiz davet kodu üretme denemesi → ") + requesterId
                                + QObject::tr(" (rol: ") + u.role + ")");
            } else if (targetUser.id.isEmpty()) {
                resp.set_success(false);
                resp.set_error_msg("Hedef kullanıcı veritabanında bulunamadı.");
                Logger::warning(QObject::tr("Broker: Katılım kodu üretilemedi, hedef kullanıcı DB'de yok → ") + userId);
            } else {
                QString code = OkidiaDatabase::instance().createJoinCode(userId);
                if (!code.isEmpty()) {
                    resp.set_success(true);
                    resp.set_code(code.toStdString());
                    Logger::info(QObject::tr("Broker: Davet kodu üretildi → ") + code + QObject::tr(" (hedef: ") + userId + ")");
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Kod üretilemedi.");
                }
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::JOIN_CODE_RESPONSE);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_ADD_REQ ────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_ADD_REQ) {
        okidia::DbUserAddRequest req;
        okidia::DbUserAddResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                UserRecord u;
                const QString reqUid = QString::fromStdString(req.user_id()).trimmed();
                u.id           = reqUid.isEmpty() ? OkidiaUtils::generateUuid() : reqUid;
                u.username     = QString::fromStdString(req.username());
                u.displayName  = QString::fromStdString(req.display_name());
                u.passwordHash = QString::fromStdString(req.password_hash());
                u.role         = QString::fromStdString(req.role());
                u.isBackupReplica = req.is_backup_replica();
                u.replicaMachineId = QString::fromStdString(req.replica_machine_id()).trimmed();
                u.replicaHostname  = QString::fromStdString(req.replica_hostname()).trimmed();
                if (u.replicaHostname == "__AUTO__") {
                    u.replicaMachineId.clear();
                    u.replicaHostname.clear();
                }
                u.mustChangePw = true;
                u.isActive     = true;
                u.createdAt    = OkidiaUtils::nowSecs();

                // Tek YM Kuralı: Yeni kullanıcı YM yapılıyorsa diğer YM'leri GM'ye düşür
                if (u.role == "YM") {
                    OkidiaDatabase::instance().demoteOtherYmUsers(u.id);
                    u.isBackupReplica = true; // YM her zaman yedek replikası alabilir
                }

                if (OkidiaDatabase::instance().upsertUser(u)) {
                    Logger::info(QObject::tr("Broker DB: Kullanıcı eklendi → ") + u.username);
                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();

                    // Eğer yeni YM veya Replika atandıysa ve pushLatestBackupToUser tanımlıysa son yedeği anında gönder
                    if ((u.role == "YM" || u.isBackupReplica) && m_ctx->pushLatestBackupToUser) {
                        m_ctx->pushLatestBackupToUser(u.id);
                    }
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı yazma hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece YM).");
            }
            
            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_ADD_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_UPDATE_REQ ──────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_UPDATE_REQ) {
        okidia::DbUserUpdateRequest req;
        okidia::DbUserUpdateResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                QString uid = QString::fromStdString(req.user_id());
                UserRecord existing = OkidiaDatabase::instance().userById(uid);
                if (existing.id.isEmpty()) {
                    resp.set_success(false);
                    resp.set_error_msg("Kullanıcı bulunamadı.");
                } else {
                    existing.displayName = QString::fromStdString(req.display_name());
                    QString newRole = QString::fromStdString(req.role());
                    if (!newRole.isEmpty()) {
                        existing.role = newRole;
                    }
                    existing.isBackupReplica = req.is_backup_replica();
                    const QString reqRepMid  = QString::fromStdString(req.replica_machine_id()).trimmed();
                    const QString reqRepHost = QString::fromStdString(req.replica_hostname()).trimmed();
                    if (reqRepHost == "__AUTO__") {
                        existing.replicaMachineId.clear();
                        existing.replicaHostname.clear();
                        OkidiaDatabase::instance().setUserBackupReplica(existing.id, existing.isBackupReplica || existing.role == "YM", "__AUTO__", "__AUTO__");
                    } else if (!reqRepHost.isEmpty() || !reqRepMid.isEmpty()) {
                        existing.replicaMachineId = reqRepMid;
                        existing.replicaHostname  = reqRepHost;
                    } else if (!existing.isBackupReplica && existing.role != "YM") {
                        existing.replicaMachineId.clear();
                        existing.replicaHostname.clear();
                    }
                    QString newPwHash = QString::fromStdString(req.password_hash());
                    if (!newPwHash.isEmpty()) {
                        existing.passwordHash = newPwHash;
                        existing.mustChangePw = true;
                    }

                    // Tek YM Kuralı: Güncellenen kullanıcı YM yapılıyorsa diğer YM'leri GM'ye düşür
                    if (existing.role == "YM") {
                        OkidiaDatabase::instance().demoteOtherYmUsers(existing.id);
                        existing.isBackupReplica = true;
                    }

                    if (OkidiaDatabase::instance().upsertUser(existing)) {
                        Logger::info(QObject::tr("Broker DB: Kullanıcı güncellendi → ") + existing.username + " (Rol: " + existing.role + ")");
                        resp.set_success(true);
                        if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();

                        if ((existing.role == "YM" || existing.isBackupReplica) && m_ctx->pushLatestBackupToUser) {
                            m_ctx->pushLatestBackupToUser(existing.id);
                        }

                        // Eğer güncellenen kullanıcı şu anda online ise ve rolü değiştiyse re-login bildirimi gönder
                        for (Connection* mConn : m_ctx->registry->masterConnections()) {
                            if (m_ctx->registry->authenticatedUserId(mConn) == existing.id) {
                                okidia::UserRoleChangedNotify notify;
                                notify.set_user_id(existing.id.toStdString());
                                notify.set_new_role(existing.role.toStdString());
                                notify.set_display_name(existing.displayName.toStdString());
                                notify.set_message("Rolünüz güncellendi. Yeni yetkilerinizin devreye girmesi için oturumunuzun yenilenmesi gerekmektedir.");
                                okidia::Envelope notifyEnv;
                                notifyEnv.set_type(okidia::MessageType::USER_ROLE_CHANGED_NOTIFY);
                                notifyEnv.set_payload(notify.SerializeAsString());
                                mConn->sendEnvelope(notifyEnv);
                                Logger::info(QObject::tr("Broker: Online Master kullanıcısına USER_ROLE_CHANGED_NOTIFY iletildi → ") + existing.username);
                            }
                        }
                    } else {
                        resp.set_success(false);
                        resp.set_error_msg("Veritabanı güncelleme hatası.");
                    }
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece YM).");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_UPDATE_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_SET_REPLICA_REQ ────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_SET_REPLICA_REQ) {
        okidia::DbUserSetReplicaRequest req;
        okidia::DbUserSetReplicaResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                QString uid = QString::fromStdString(req.user_id());
                QString repMid = QString::fromStdString(req.replica_machine_id()).trimmed();
                QString repHost = QString::fromStdString(req.replica_hostname()).trimmed();

                // Eğer replika yetkisi açılıyorsa ve özel bir cihaz gönderilmediyse,
                // kullanıcının daha önce giriş yaptığı kendi Master bilgisayarını bulup bağla
                if (req.is_backup_replica() && repHost.isEmpty() && repMid.isEmpty()) {
                    const QList<UserRecord> allU = OkidiaDatabase::instance().allUsers();
                    QSet<QString> claimedHosts;
                    for (const auto& usr : allU) {
                        if (usr.id != uid && !usr.replicaHostname.trimmed().isEmpty()) {
                            claimedHosts.insert(usr.replicaHostname.trimmed().toLower());
                        }
                    }
                    const QList<MasterDeviceRecord> devs = OkidiaDatabase::instance().allMasterDevices();
                    for (const auto& d : devs) {
                        if (d.lastUserId == uid && !d.hostname.trimmed().isEmpty() &&
                            !claimedHosts.contains(d.hostname.trimmed().toLower())) {
                            repMid = d.machineId;
                            repHost = d.hostname.trimmed();
                            break;
                        }
                    }
                }

                bool ok = OkidiaDatabase::instance().setUserBackupReplica(uid, req.is_backup_replica(), repMid, repHost);
                resp.set_success(ok);
                if (ok) {
                    Logger::info(QObject::tr("Broker DB: Kullanıcı ikincil yedek yetkisi güncellendi → %1 (Cihaz: %2)")
                                 .arg(uid, repHost.isEmpty() ? QObject::tr("İlk girişte bağlanacak") : repHost));
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                    if (req.is_backup_replica() && m_ctx->pushLatestBackupToUser) {
                        m_ctx->pushLatestBackupToUser(uid);
                    }
                } else {
                    resp.set_error_msg("Veritabanı güncelleme hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece YM).");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_SET_REPLICA_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_DEL_REQ ────────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_DEL_REQ) {
        okidia::DbUserDelRequest req;
        okidia::DbUserDelResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                QString uid = QString::fromStdString(req.user_id());
                UserRecord u = OkidiaDatabase::instance().userById(uid);
                if (u.role == "YM") {
                    resp.set_success(false);
                    resp.set_error_msg("Yönetici hesabı silinemez.");
                } else if (OkidiaDatabase::instance().removeUser(uid)) {
                    Logger::info(QObject::tr("Broker DB: Kullanıcı silindi → ") + uid);
                    resp.set_success(true);

                    // Aktif bağlantısı varsa anında kısıtlı moda al ve bağlantısını kes
                    for (Connection* c : m_ctx->registry->masterConnections()) {
                        if (m_ctx->registry->authenticatedUserId(c) == uid) {
                            Logger::warning(QObject::tr("Broker: Silinen kullanıcının oturumu sonlandırılıyor: ") + uid);
                            okidia::Envelope forceEnv;
                            forceEnv.set_type(okidia::MessageType::SESSION_RESTRICT_MODE);
                            c->sendEnvelope(forceEnv);
                            c->disconnect();
                        }
                    }

                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı silme hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece YM).");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_DEL_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_PW_RESET_REQ ───────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_PW_RESET_REQ) {
        okidia::DbUserPwResetRequest req;
        okidia::DbUserPwResetResponse resp;
        if (req.ParseFromString(env.payload())) {
            QString uid = QString::fromStdString(req.user_id());
            QString hash = QString::fromStdString(req.password_hash());
            bool isAdminReset = req.is_admin_reset();

            QString requesterId = m_ctx->registry->authenticatedUserId(conn);
            bool isYM = m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn);

            if (isYM || (requesterId == uid)) {
                if (OkidiaDatabase::instance().setUserPassword(uid, hash, isAdminReset)) {
                    UserRecord u = OkidiaDatabase::instance().userById(uid);
                    resetAccountLockout(u.username);
                    Logger::info(QObject::tr("Broker DB: Kullanıcı şifresi sıfırlandı → ") + uid);
                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı şifre güncelleme hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz şifre sıfırlama denemesi.");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_PW_RESET_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_GRANT_REQ ──────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_GRANT_REQ) {
        okidia::DbUserGrantRequest req;
        okidia::DbUserGrantResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                QString uid = QString::fromStdString(req.user_id());
                QString cid = QString::fromStdString(req.class_id());
                QString level = QString::fromStdString(req.level());
                
                if (OkidiaDatabase::instance().grantPermission(uid, cid, level)) {
                    Logger::info(QObject::tr("Broker DB: Kullanıcıya yetki verildi → user: ") + uid + QObject::tr(" class: ") + cid);
                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı yetkilendirme hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece YM).");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_GRANT_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_USER_REVOKE_REQ ─────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_USER_REVOKE_REQ) {
        okidia::DbUserRevokeRequest req;
        okidia::DbUserRevokeResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                QString uid = QString::fromStdString(req.user_id());
                QString cid = QString::fromStdString(req.class_id());
                
                if (OkidiaDatabase::instance().revokePermission(uid, cid)) {
                    Logger::info(QObject::tr("Broker DB: Kullanıcı yetkisi kaldırıldı → user: ") + uid + QObject::tr(" class: ") + cid);
                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg("Veritabanı yetki kaldırma hatası.");
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg("Yetkisiz işlem (Sadece YM).");
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_USER_REVOKE_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_TEACHER_UPSERT_REQ ──────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_TEACHER_UPSERT_REQ) {
        okidia::DbTeacherUpsertRequest req;
        okidia::DbTeacherUpsertResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                TeacherRecord t;
                t.id          = QString::fromStdString(req.teacher_id());
                if (t.id.isEmpty()) t.id = OkidiaUtils::generateUuid();
                t.username    = QString::fromStdString(req.username());
                t.displayName = QString::fromStdString(req.display_name());
                t.branch      = QString::fromStdString(req.branch());
                t.seedHex     = QString::fromStdString(req.seed_hex());
                t.password    = QString::fromStdString(req.password());
                t.isActive    = req.is_active();
                t.createdBy   = m_ctx->registry->authenticatedUserId(conn);
                t.createdAt   = OkidiaUtils::nowSecs();
                t.updatedAt   = t.createdAt;

                if (OkidiaDatabase::instance().upsertTeacher(t)) {
                    Logger::info(QObject::tr("Broker DB: Öğretmen kaydedildi → ") + t.displayName + " (" + t.username + ")");
                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg(QObject::tr("Öğretmen veritabanına kaydedilemedi.").toStdString());
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg(QObject::tr("Yetkisiz işlem (Sadece YM).").toStdString());
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_TEACHER_UPSERT_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    // ── DB_TEACHER_DEL_REQ ─────────────────────────────────────────────────
    if (env.type() == okidia::MessageType::DB_TEACHER_DEL_REQ) {
        okidia::DbTeacherDeleteRequest req;
        okidia::DbTeacherDeleteResponse resp;
        if (req.ParseFromString(env.payload())) {
            if (m_ctx->isRequesterYM && m_ctx->isRequesterYM(conn)) {
                QString tid = QString::fromStdString(req.teacher_id());
                if (OkidiaDatabase::instance().removeTeacher(tid)) {
                    Logger::info(QObject::tr("Broker DB: Öğretmen silindi → ") + tid);
                    resp.set_success(true);
                    if (m_ctx->broadcastDbSyncSignal) m_ctx->broadcastDbSyncSignal();
                } else {
                    resp.set_success(false);
                    resp.set_error_msg(QObject::tr("Öğretmen silinemedi.").toStdString());
                }
            } else {
                resp.set_success(false);
                resp.set_error_msg(QObject::tr("Yetkisiz işlem (Sadece YM).").toStdString());
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::DB_TEACHER_DEL_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
        }
        return true;
    }

    return false;
}

void AuthHandler::resetAccountLockout(const QString& username) {
    if (!username.isEmpty() && m_failedUserAttempts.contains(username)) {
        m_failedUserAttempts.remove(username);
        Logger::info(QObject::tr("Broker: %1 kullanıcısının hesap kilidi/deneme sayacı sıfırlandı.").arg(username));
    }
}

void AuthHandler::resetIpRateLimit(const QString& peerIp) {
    if (!peerIp.isEmpty() && m_failedIpAttempts.contains(peerIp)) {
        m_failedIpAttempts.remove(peerIp);
        Logger::info(QObject::tr("Broker: %1 IP adresinin engel/deneme sayacı sıfırlandı.").arg(peerIp));
    }
}

void AuthHandler::cleanupExpiredLimits() {
    const qint64 now = OkidiaUtils::nowSecs();

    // Süresi geçmiş IP engellerini veya eski denemeleri temizle (1 saat işlem görmeyenler)
    auto ipIt = m_failedIpAttempts.begin();
    while (ipIt != m_failedIpAttempts.end()) {
        if (ipIt.value().blockedUntil > 0 && ipIt.value().blockedUntil <= now) {
            ipIt = m_failedIpAttempts.erase(ipIt);
        } else {
            ++ipIt;
        }
    }

    // Süresi geçmiş kullanıcı kilitlerini temizle
    auto userIt = m_failedUserAttempts.begin();
    while (userIt != m_failedUserAttempts.end()) {
        if (userIt.value().lockedUntil > 0 && userIt.value().lockedUntil <= now) {
            userIt = m_failedUserAttempts.erase(userIt);
        } else {
            ++userIt;
        }
    }
}

