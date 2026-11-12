#include "EnrollmentManager.h"
#include "ClientRegistry.h"
#include "CertSigner.h"
#include "../common/crypto/KeyManager.h"
#include "../common/utils/Logger.h"
#include "../common/db/OkidiaDatabase.h"
#include "../common/network/TlsServer.h"
#include <QFile>
#include "../common/OkidiaConfig.h"
#include "../common/utils/OkidiaUtils.h"

EnrollmentManager::EnrollmentManager(ClientRegistry* registry)
    : m_registry(registry)
{
}

void EnrollmentManager::setBroadcastStatusCallback(std::function<void()> callback)
{
    m_broadcastCallback = callback;
}

bool EnrollmentManager::process(Connection* conn, const okidia::Envelope& env, const QString& peerId)
{
    if (env.type() == okidia::MessageType::DISCOVERY_REQUEST) {
        handleDiscoveryRequest(conn, env);
        return true;
    }
    
    if (env.type() == okidia::MessageType::DISCOVERY_APPROVE) {
        handleDiscoveryApprove(conn, env, peerId);
        return true;
    }

    if (env.type() == okidia::MessageType::DISCOVERY_REJECT) {
        handleDiscoveryReject(conn, env, peerId);
        return true;
    }

    if (env.type() == okidia::MessageType::DISCOVERY_IGNORE) {
        handleDiscoveryIgnore(conn, env, peerId);
        return true;
    }
    
    if (env.type() == okidia::MessageType::ENROLL_REQUEST) {
        handleEnrollRequest(conn, env);
        return true;
    }

    if (env.type() == okidia::MessageType::TIME_SYNC_REQUEST) {
        handleTimeSyncRequest(conn, env);
        return true;
    }
    
    return false;
}

void EnrollmentManager::setBrokerTimeStatus(bool isValid, int remainingLogins)
{
    m_brokerTimeValid = isValid;
    m_remainingGraceLogins = remainingLogins;
}

void EnrollmentManager::onMasterLoginSuccess(Connection* conn)
{
    if (m_brokerTimeValid) return;

    if (m_remainingGraceLogins > 0) {
        m_remainingGraceLogins--;
        OkidiaDatabase::instance().setMetaValue("time_desync_grace_logins", QString::number(m_remainingGraceLogins));
    }

    okidia::BrokerTimeWarning warn;
    warn.set_is_desync(true);
    warn.set_remaining_logins(m_remainingGraceLogins);
    warn.set_is_locked(m_remainingGraceLogins <= 0);

    if (m_remainingGraceLogins > 0) {
        warn.set_warning_message(
            QObject::tr("DİKKAT: Broker sunucusunun sistem saati sertifika tarihinden geride! (Kalan giriş toleransı: %1/3). "
                        "Lütfen sunucu saatini düzeltin veya anakart BIOS pilini değiştirin.")
            .arg(m_remainingGraceLogins).toStdString()
        );
    } else {
        warn.set_warning_message(
            QObject::tr("KRİTİK HATA: Broker sunucusu BIOS pili değiştirilmedi veya saati düzeltilmedi. "
                        "Tolerans süresi dolduğu için sistem güvenliği adına yönetim kilitlendi!")
            .toStdString()
        );
    }

    okidia::Envelope warnEnv;
    warnEnv.set_type(okidia::MessageType::BROKER_TIME_WARNING);
    warnEnv.set_payload(warn.SerializeAsString());
    conn->sendEnvelope(warnEnv);

    Logger::warning(QObject::tr("Broker: Master'a saat uyumsuzluğu uyarısı gönderildi (Kalan tolerans: %1)").arg(m_remainingGraceLogins));
}

void EnrollmentManager::handleTimeSyncRequest(Connection* conn, const okidia::Envelope& env)
{
    okidia::TimeSyncRequest req;
    if (!req.ParseFromString(env.payload())) return;

    const qint64 brokerNow = OkidiaUtils::nowSecs();
    const QString senderId = QString::fromStdString(req.sender_id());
    const qint64 clientLocalTime = req.client_local_time();
    const bool rtcFault = req.rtc_fault_suspected();
    const bool firstEnroll = req.is_first_enrollment();

    // Zaman yanıtını hazırla ve anında gönder
    okidia::TimeSyncResponse resp;
    resp.set_server_utc_time(brokerNow);
    resp.set_broker_time_valid(m_brokerTimeValid);
#ifdef OKIDIA_VERSION
    resp.set_broker_version(OKIDIA_VERSION);
#else
    resp.set_broker_version("0.1.0");
#endif

    okidia::Envelope respEnv;
    respEnv.set_type(okidia::MessageType::TIME_SYNC_RESPONSE);
    respEnv.set_payload(resp.SerializeAsString());
    conn->sendEnvelope(respEnv);

    Logger::info(QObject::tr("Broker: TIME_SYNC_REQUEST ← %1 (Client: %2, Broker: %3, RTC Fault: %4, First: %5)")
                 .arg(senderId.isEmpty() ? conn->peerAddress() : senderId)
                 .arg(clientLocalTime)
                 .arg(brokerNow)
                 .arg(rtcFault ? "Evet" : "Hayir")
                 .arg(firstEnroll ? "Evet" : "Hayir"));

    // ── BIOS Pili / RTC Arıza Telemetrisi & Tolerans Kontrolü ────────────────
    if (!senderId.isEmpty()) {
        const qint64 diffSecs = brokerNow - clientLocalTime;

        // Arıza kriteri:
        // 1. Agent kendisi sertifika tarihinden geride olduğunu bildirdiyse (rtcFault)
        // 2. VEYA istemci saati Broker'dan 48 saatten (172800 sn) daha eskiyse
        const bool isSevereDrift = rtcFault || (diffSecs > 172800);

        if (isSevereDrift) {
            int fails = m_rtcBatteryFails.value(senderId, 0) + 1;
            m_rtcBatteryFails[senderId] = fails;
            Logger::warning(QObject::tr("Broker: İstemci %1 saat sapması/BIOS pil arızası tespit edildi! (Ardışık: %2)")
                            .arg(senderId).arg(fails));

            // Eğer arka arkaya 3 kez tolerans dışı uyanmışsa Master'a bildir
            if (fails >= 3) {
                okidia::ClientHardwareAlert alert;
                alert.set_machine_id(senderId.toStdString());
                alert.set_alert_type("BIOS_BATTERY_DEAD");
                alert.set_occurrence_count(fails);
                alert.set_details(
                    QObject::tr("Cihaz saati üst üste %1 açılışta da anakart/BIOS pil arızası nedeniyle geriye düştü. "
                                "Lütfen cihazın BIOS pilini yenileyin.").arg(fails).toStdString()
                );

                // Hostname bilgisi varsa ekle
                AgentRecord ar = OkidiaDatabase::instance().agent(senderId);
                if (!ar.machineId.isEmpty()) {
                    alert.set_hostname(ar.hostname.toStdString());
                }

                okidia::Envelope alertEnv;
                alertEnv.set_type(okidia::MessageType::CLIENT_HARDWARE_ALERT);
                alertEnv.set_payload(alert.SerializeAsString());
                m_registry->forwardToMaster(senderId, alertEnv);
            }
        } else if (diffSecs <= 172800 && diffSecs >= -3600) {
            // Tolerans içi (örneğin pil değişmiş veya normal saat kayması)
            // Eğer önceki bir hata sayacı varsa temizle/sıfırla
            if (m_rtcBatteryFails.contains(senderId)) {
                m_rtcBatteryFails.remove(senderId);
                Logger::info(QObject::tr("Broker: İstemci %1 saati sağlıklı tolerans aralığında, pil arıza sayacı sıfırlandı.")
                             .arg(senderId));
            }
        }
    }
}

void EnrollmentManager::handleDiscoveryRequest(Connection* conn, const okidia::Envelope& env)
{
    Logger::info("Broker: Discovery isteği ← " + conn->peerAddress());
    okidia::DiscoveryRequest req;
    if (req.ParseFromString(env.payload())) {
        const QString machineId = QString::fromStdString(req.machine_id());
        if (machineId.isEmpty()) return;
        
        if (m_registry->isRevoked(machineId)) {
            Logger::warning("Broker: Kara listedeki agent bağlanmaya çalıştı → reddedildi: " + machineId);
            okidia::Envelope rejectEnv;
            rejectEnv.set_type(okidia::MessageType::DISCOVERY_REJECT);
            conn->sendEnvelope(rejectEnv);
            return;
        }

        // Önceden onaylanmış ajan zırhı (Zero-Config dynamic recovery):
        // Eğer ajan daha önce Master tarafından onaylanmış ancak sertifikayı teslim alamadan kopmuşsa,
        // VE gelen CSR'ın açık anahtarı veritabanındaki sertifikanın açık anahtarıyla birebir eşleşiyorsa
        // tekrar Master onayına sunmak yerine veritabanındaki sertifikasını anında teslim et.
        AgentRecord existingAgent = OkidiaDatabase::instance().agent(machineId);
        if (!existingAgent.machineId.isEmpty() && !existingAgent.certPem.isEmpty()) {
            const QByteArray incomingCsr = QByteArray::fromStdString(req.csr_pem());
            const QByteArray existingCert = existingAgent.certPem.toLatin1();

            if (KeyManager::matchCsrWithCertificate(incomingCsr, existingCert)) {
                Logger::info("Broker: Önceden onaylanmış ve açık anahtarı doğrulanan ajan için otomatik CERT_PROVISION teslim ediliyor → " + machineId);
                QFile caFile(OkidiaConfig::brokerCertDir() + "/ca.crt");
                if (caFile.open(QIODevice::ReadOnly)) {
                    okidia::CertProvision prov;
                    prov.set_cert_pem(existingAgent.certPem.toStdString());
                    prov.set_ca_pem(caFile.readAll().toStdString());
                    
                    okidia::Envelope provEnv;
                    provEnv.set_type(okidia::MessageType::CERT_PROVISION);
                    provEnv.set_payload(prov.SerializeAsString());
                    conn->sendEnvelope(provEnv);
                    return;
                }
            } else {
                Logger::warning("Broker: Ajan veritabanında kayıtlı (" + machineId + 
                               ") fakat gelen CSR açık anahtarı mevcut sertifikayla eşleşmiyor! "
                               "(Yeniden kurulum veya anahtar yenilemesi tespit edildi). İstek Master onayına iletiliyor.");
            }
        }
        
        m_registry->storePendingRequest(machineId, req);
        m_registry->forwardToMaster(machineId, env);
        m_registry->addPendingDiscovery(machineId, conn);
    }
    
    if (m_broadcastCallback) {
        m_broadcastCallback();
    }
}

void EnrollmentManager::handleDiscoveryApprove(Connection* conn, const okidia::Envelope& env, const QString& peerId)
{
    const QString requesterId = m_registry->authenticatedUserId(conn).isEmpty()
                                ? peerId
                                : m_registry->authenticatedUserId(conn);
                                
    UserRecord u = OkidiaDatabase::instance().userById(requesterId);
    if (u.id.isEmpty() || (u.role != "YM" && u.role != "OM")) {
        Logger::warning("Broker: Yetkisiz DISCOVERY_APPROVE denemesi → " + requesterId);
        return;
    }

    okidia::DiscoveryApprove appr;
    if (!appr.ParseFromString(env.payload())) return;
    const QString targetId = QString::fromStdString(appr.target_id());
    Logger::info("Broker: DISCOVERY_APPROVE ← master, hedef=" + targetId);

    const okidia::DiscoveryRequest req = m_registry->pendingRequest(targetId);
    const QByteArray csrPem = m_registry->pendingCsr(targetId);

    if (csrPem.isEmpty()) {
        Logger::warning("Broker: Hedef ajan için CSR bulunamadı → " + targetId);
        return;
    }

    if (!m_brokerTimeValid) {
        Logger::error(QObject::tr("Broker: KRİTİK GÜVENLİK ENGELİ! Broker sistem saati geçersiz/bozuk olduğu için CSR imzalanamaz! "
                                  "Lütfen sunucu saatini düzeltin veya BIOS pilini yenileyin."));
        return;
    }

    QString err;
    const QByteArray certPem = CertSigner::signCsr(csrPem, err, QString::fromStdString(req.machine_id()));

    if (certPem.isEmpty()) {
        Logger::warning("Broker: Sertifika imzalanamadı: " + err);
        return;
    }

    QFile caFile(OkidiaConfig::brokerCertDir() + "/ca.crt");
    if (!caFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Broker: CA sertifikası okunamadı!");
        return;
    }

    const QByteArray caData = caFile.readAll();
    
    okidia::CertProvision prov;
    prov.set_cert_pem(certPem.toStdString());
    prov.set_ca_pem(caData.toStdString());

    okidia::Envelope provEnv;
    provEnv.set_type(okidia::MessageType::CERT_PROVISION);
    provEnv.set_payload(prov.SerializeAsString());

    m_registry->forwardToPending(targetId, provEnv);
    Logger::info("Broker: CERT_PROVISION gönderildi → " + targetId);

    QSslCertificate generatedCert(certPem);
    QString realMachineId = generatedCert.subjectInfo(QSslCertificate::CommonName).join("");
    if (realMachineId.isEmpty()) {
        realMachineId = QString::fromStdString(req.machine_id());
    }

    AgentRecord rec;
    rec.machineId = realMachineId;
    rec.hostname = QString::fromStdString(req.hostname());
    rec.ip = QString::fromStdString(req.ip_address());
    rec.mac = QString::fromStdString(req.mac_address());
    rec.osInfo = QString::fromStdString(req.os_info());
    rec.osArch = QString::fromStdString(req.os_arch());
    rec.agentVer = QString::fromStdString(req.agent_version());
    rec.certPem = QString::fromLatin1(certPem);
    rec.agentType = appr.agent_type();

    // Eğer veritabanında bu hostname ile Deploy Panel veya tarayıcı üzerinden 
    // daha önce eklenmiş sahte bir kayıt varsa (machineId == hostname), 
    // yeni onaylanan ajan ile çakışmaması (hayalet kopya olmaması) için eski kaydı sil.
    const auto existingAgents = OkidiaDatabase::instance().allAgents();
    for (const auto& a : existingAgents) {
        if (a.hostname == rec.hostname && a.machineId == a.hostname && a.machineId != rec.machineId) {
            OkidiaDatabase::instance().removeAgent(a.machineId);
            Logger::info("Broker: Deploy panel dummy kaydı (" + a.machineId + ") silindi, yerine gerçek UUID kaydediliyor.");
        }
    }

    OkidiaDatabase::instance().upsertAgent(rec);

    // Bekleyen keşif isteğini Broker hafızasından temizle
    m_registry->removePending(targetId);

    // Tüm bağlı Master'lara (YM, OM vb.) bu istemcinin onaylandığını bildir
    okidia::DiscoveryResolvedBroadcast resBrd;
    resBrd.set_target_id(targetId.toStdString());
    resBrd.set_resolved_by(u.username.toStdString());
    resBrd.set_action("approve");
    resBrd.set_agent_type(rec.agentType);
    resBrd.set_hostname(rec.hostname.toStdString());

    okidia::Envelope resEnv;
    resEnv.set_type(okidia::MessageType::DISCOVERY_RESOLVED_BROADCAST);
    resEnv.set_payload(resBrd.SerializeAsString());

    for (Connection* mConn : m_registry->allMasterConnections()) {
        mConn->sendEnvelope(resEnv);
    }
    Logger::info("Broker: DISCOVERY_RESOLVED_BROADCAST (approve) tüm master'lara iletildi → hedef=" + targetId);

    if (m_broadcastCallback) {
        m_broadcastCallback();
    }
}

void EnrollmentManager::handleDiscoveryReject(Connection* conn, const okidia::Envelope& env, const QString& peerId)
{
    const QString requesterId = m_registry->authenticatedUserId(conn).isEmpty()
                                ? peerId
                                : m_registry->authenticatedUserId(conn);
    UserRecord u = OkidiaDatabase::instance().userById(requesterId);
    if (u.id.isEmpty() || (u.role != "YM" && u.role != "OM")) {
        Logger::warning("Broker: Yetkisiz DISCOVERY_REJECT denemesi → " + requesterId);
        return;
    }

    const QString targetId = QString::fromStdString(env.target_id());
    if (targetId.isEmpty()) return;

    Logger::info("Broker: DISCOVERY_REJECT ← master, hedef=" + targetId);

    // Pending bağlantıyı kopar ve temizle
    m_registry->disconnectPending(targetId);
    m_registry->removePending(targetId);

    // Tüm bağlı Master'lara bu isteğin reddedildiğini yayınla (listelerden kalksın)
    okidia::DiscoveryResolvedBroadcast resBrd;
    resBrd.set_target_id(targetId.toStdString());
    resBrd.set_resolved_by(u.username.toStdString());
    resBrd.set_action("reject");
    resBrd.set_agent_type(0);

    okidia::Envelope resEnv;
    resEnv.set_type(okidia::MessageType::DISCOVERY_RESOLVED_BROADCAST);
    resEnv.set_payload(resBrd.SerializeAsString());

    for (Connection* mConn : m_registry->allMasterConnections()) {
        mConn->sendEnvelope(resEnv);
    }

    if (m_broadcastCallback) {
        m_broadcastCallback();
    }
}

void EnrollmentManager::handleDiscoveryIgnore(Connection* conn, const okidia::Envelope& env, const QString& peerId)
{
    const QString requesterId = m_registry->authenticatedUserId(conn).isEmpty()
                                ? peerId
                                : m_registry->authenticatedUserId(conn);
    UserRecord u = OkidiaDatabase::instance().userById(requesterId);
    if (u.id.isEmpty() || (u.role != "YM" && u.role != "OM")) {
        Logger::warning("Broker: Yetkisiz DISCOVERY_IGNORE denemesi → " + requesterId);
        return;
    }

    const QString targetId = QString::fromStdString(env.target_id());
    if (targetId.isEmpty()) return;

    Logger::info("Broker: DISCOVERY_IGNORE ← master, hedef=" + targetId);

    // Pending bağlantıyı kopar ve temizle
    m_registry->disconnectPending(targetId);
    m_registry->removePending(targetId);

    // Tüm bağlı Master'lara bu isteğin yoksayıldığını (kara liste) yayınla
    okidia::DiscoveryResolvedBroadcast resBrd;
    resBrd.set_target_id(targetId.toStdString());
    resBrd.set_resolved_by(u.username.toStdString());
    resBrd.set_action("ignore");
    resBrd.set_agent_type(0);

    okidia::Envelope resEnv;
    resEnv.set_type(okidia::MessageType::DISCOVERY_RESOLVED_BROADCAST);
    resEnv.set_payload(resBrd.SerializeAsString());

    for (Connection* mConn : m_registry->allMasterConnections()) {
        mConn->sendEnvelope(resEnv);
    }

    if (m_broadcastCallback) {
        m_broadcastCallback();
    }
}

void EnrollmentManager::handleEnrollRequest(Connection* conn, const okidia::Envelope& env)
{
    const QString userId = m_registry->authenticatedUserId(conn);
    okidia::EnrollResponse resp;
    if (userId.isEmpty()) {
        resp.set_success(false);
        resp.set_error_msg("Oturum acilmamis.");
    } else if (!m_brokerTimeValid) {
        resp.set_success(false);
        resp.set_error_msg("Broker sistem saati hatali (BIOS pili/desync), saat duzeltilmeden sertifika uretilemez.");
        Logger::error(QObject::tr("Broker: Saat hatasi nedeniyle ENROLL_REQUEST CSR imzalamasi engellendi!"));
    } else {
        okidia::EnrollRequest req;
        req.ParseFromString(env.payload());
        
        QString err;
        const QByteArray certPem = CertSigner::signCsr(
            QByteArray::fromStdString(req.csr_pem()), err);
        
        if (certPem.isEmpty()) {
            resp.set_success(false);
            resp.set_error_msg("Sertifika imzalama hatasi: " + err.toStdString());
        } else {
            QFile caFile(OkidiaConfig::brokerCertDir() + "/ca.crt");
            if (caFile.open(QIODevice::ReadOnly)) {
                resp.set_success(true);
                resp.set_master_cert(certPem.toStdString());
                resp.set_ca_cert(caFile.readAll().toStdString());
            } else {
                resp.set_success(false);
                resp.set_error_msg("CA sertifikasi okunamadi.");
            }
        }
    }
    
    okidia::Envelope re;
    re.set_type(okidia::MessageType::ENROLL_RESPONSE);
    re.set_payload(resp.SerializeAsString());
    conn->sendEnvelope(re);
}
