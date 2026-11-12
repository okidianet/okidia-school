#include "DiscoveryService.h"
#include "../common/network/MdnsManager.h"
#include "../common/network/TlsServer.h"
#include "../common/utils/Logger.h"
#include "../system/SystemInfo.h"
#include "../common/crypto/KeyManager.h"
#include "messages.pb.h"
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QNetworkInterface>
#include <QSysInfo>
#include "../../common/OkidiaConfig.h"
#include "../../common/utils/OkidiaUtils.h"

DiscoveryService::DiscoveryService(MdnsManager* mdns, QObject* parent)
    : QObject(parent), m_mdns(mdns)
{
}

void DiscoveryService::startDiscovery(const QString& configuredBrokerIp)
{
    if (configuredBrokerIp.isEmpty()) {
        m_mdns->startBrowsing("_okidia._tcp");
    }
}

void DiscoveryService::restartDiscovery()
{
    m_mdns->restartBrowsing("_okidia._tcp");
}

// ── Yardımcı: keypair + CSR üret ────────────────────────────────────────
QByteArray DiscoveryService::generateOrGetCsr()
{
    const QString certDir = OkidiaConfig::agentCertDir();
    const QString keyPath = certDir + "/agent.key";
    const QString pubPath = certDir + "/agent.pub";
    const QString csrPath = certDir + "/agent.csr";

    QDir().mkpath(certDir);

    if (!QFile::exists(keyPath)) {
        Logger::info("Agent: Keypair üretiliyor...");
        if (!KeyManager::generateKeyPair(keyPath, pubPath)) {
            Logger::error("Agent: Keypair üretilemedi");
            return {};
        }
    }

    if (!QFile::exists(csrPath)) {
        if (!KeyManager::generateCsr(keyPath, csrPath, SystemInfo::machineId())) {
            Logger::error("Agent: CSR üretilemedi");
            return {};
        }
    }

    QFile f(csrPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// ── Yardımcı: DISCOVERY_REQUEST gönder ──────────────────────────────────
void DiscoveryService::sendDiscoveryRequest(Connection* conn)
{
    const QByteArray csrPem = generateOrGetCsr();
    if (csrPem.isEmpty()) {
        Logger::error("Agent: CSR alinamadi, discovery gönderilemedi");
        return;
    }

    QString ipAddress;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                && !entry.ip().isLoopback()) {
                ipAddress = entry.ip().toString();
                break;
            }
        }
        if (!ipAddress.isEmpty()) break;
    }

    okidia::DiscoveryRequest req;
    req.set_hostname(SystemInfo::hostname().toStdString());
    req.set_username(SystemInfo::activeDisplayUser().toStdString());
    req.set_machine_id(SystemInfo::machineId().toStdString());
    req.set_mac_address(SystemInfo::macAddress().toStdString());
    req.set_ip_address(ipAddress.toStdString());
#ifdef OKIDIA_VERSION
    req.set_agent_version(OKIDIA_VERSION);
#else
    req.set_agent_version("0.0.0");
#endif
    req.set_os_info(SystemInfo::osVersion().toStdString());
    req.set_os_arch(QSysInfo::currentCpuArchitecture().toStdString());
    req.set_csr_pem(csrPem.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DISCOVERY_REQUEST);
    env.set_payload(req.SerializeAsString());
    conn->sendEnvelope(env);

    Logger::info("Agent: Discovery isteği gönderildi → " + ipAddress);
}

void DiscoveryService::sendTimeSyncRequest(Connection* conn, bool rtcFaultSuspected, bool isFirstEnrollment)
{
    if (!conn) return;

    okidia::TimeSyncRequest req;
    req.set_sender_id(SystemInfo::machineId().toStdString());
    req.set_client_local_time(OkidiaUtils::nowSecs());
    req.set_rtc_fault_suspected(rtcFaultSuspected);
    req.set_is_first_enrollment(isFirstEnrollment);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::TIME_SYNC_REQUEST);
    env.set_payload(req.SerializeAsString());
    conn->sendEnvelope(env);

    Logger::info(QObject::tr("Agent: TIME_SYNC_REQUEST gönderildi (RTC Şüpheli: %1, İlk Kayıt: %2)")
                 .arg(rtcFaultSuspected ? "Evet" : "Hayır")
                 .arg(isFirstEnrollment ? "Evet" : "Hayır"));
}
