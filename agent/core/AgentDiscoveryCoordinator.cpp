#include "AgentDiscoveryCoordinator.h"
#include "../network/DiscoveryService.h"
#include "../../common/network/TlsClient.h"
#include "../../common/network/LocalConnection.h"
#include "../../common/network/MdnsManager.h"
#include "../../common/OkidiaConfig.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/PlatformService.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/crypto/KeyManager.h"

#include <QFile>
#include <QTimer>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QMetaEnum>
#include <QHostInfo>

AgentDiscoveryCoordinator::AgentDiscoveryCoordinator(TlsClient* tls, LocalServer* localServer, QObject* parent)
    : QObject(parent), m_tls(tls), m_localServer(localServer) {
    m_mdns = new MdnsManager(this);
    m_configuredBrokerIp = OKIDIA_CONFIG.brokerIp();
    m_configuredBrokerPort = OKIDIA_CONFIG.brokerPort();
}

void AgentDiscoveryCoordinator::connectToBroker(const QString& host, quint16 port) {
    if ((m_tls->isConnected() || m_tls->isConnecting()) && m_tls->host() == host && m_tls->port() == port) return;
    if (m_tls->isConnected()) {
        Logger::info(QObject::tr("Agent: Yeni mDNS IP adresi tespit edildi, mevcut zombi baglanti dusuruluyor..."));
    }

    Logger::info(QObject::tr("Agent: Broker'a bağlanılıyor → ") + host + ":" + QString::number(port));
    PlatformService::notifyStatus(QObject::tr("Broker'a bağlanılıyor..."));

    const QString certPath = OkidiaConfig::agentCertDir() + "/agent.crt";
    const QString keyPath  = OkidiaConfig::agentCertDir() + "/agent.key";
    const QString caPath   = OkidiaConfig::agentCertDir() + "/ca.crt";

    const bool hasCert = QFile::exists(certPath) && QFile::exists(keyPath);
    const bool hasCa   = QFile::exists(caPath);

    if (hasCert && hasCa) {
        // Pre-mTLS RTC & Saat Sapması Denetimi:
        // Eğer yerel saat sertifikanın başlangıç tarihinden gerideyse (örn. BIOS pili bitmiş, saat 2010),
        // mTLS başlatılırsa CertificateNotYetValid hatasıyla el sıkışma çöker.
        // Bu durumda önce anonymous bağlanıp saati Broker'a eşitleyeceğiz, ardından mTLS'e geçeceğiz.
        const qint64 notBefore = KeyManager::getCertificateNotBefore(certPath);
        const qint64 localNow  = OkidiaUtils::nowSecs();

        if (notBefore > 0 && localNow < notBefore) {
            Logger::warning(QObject::tr("Agent: DİKKAT! Sistem saati (%1) yerel sertifika başlangıç tarihinden (%2) geride! "
                                        "BIOS pili bitmiş olabilir. mTLS öncesi anonymous zaman senkronizasyonuna geçiliyor...")
                            .arg(localNow).arg(notBefore));
            m_tls->connectToServerAnonymous(host, port);
            return;
        }

        // Proaktif sertifika süresi denetimi
        int daysRemaining = KeyManager::getCertificateDaysRemaining(certPath);
        if (daysRemaining <= 30) {
            Logger::warning(QObject::tr("Agent: Sertifika geçerlilik süresinin dolmasına %1 gün kaldı!").arg(daysRemaining));
        } else {
            Logger::info(QObject::tr("Agent: Sertifika geçerli (kalan süre: %1 gün).").arg(daysRemaining));
        }

        m_tls->connectToServer(host, port, certPath, keyPath, caPath);
    } else if (QFile::exists("../certs/test/client.crt")) {
        m_tls->connectToServer(host, port,
            "../certs/test/client.crt",
            "../certs/test/client.key",
            "../certs/test/ca.crt");
    } else {
        Logger::info(QObject::tr("Agent: Sertifika yok → discovery modu"));
        m_tls->connectToServerAnonymous(host, port);
    }
}

void AgentDiscoveryCoordinator::start() {
    auto failedReconnects = std::make_shared<int>(0);
    connect(m_tls, &TlsClient::connected, this, [this, failedReconnects](Connection*) {
        *failedReconnects = 0;
        stopFallbackTimer();
    });

    connect(m_tls, &TlsClient::error, this, [this](const QString& errMsg) {
        Logger::warning(QObject::tr("Agent: TLS hatası: ") + errMsg);
        if (m_configuredBrokerIp.isEmpty()) {
            Logger::info(QObject::tr("Agent: mDNS taraması tazeleniyor..."));
            DiscoveryService(m_mdns).restartDiscovery();
            startFallbackTimer();
        }
    });

    connect(m_tls, &TlsClient::disconnected, this, [this, failedReconnects]() {
        Logger::warning(QObject::tr("Agent: Bağlantı kesildi, yeniden bağlanılacak..."));
        
        (*failedReconnects)++;
        if (m_configuredBrokerIp.isEmpty()) {
            Logger::info(QObject::tr("Agent: Bağlantı koptu, mDNS ağ taraması anında yenileniyor..."));
            DiscoveryService(m_mdns).restartDiscovery();
            startFallbackTimer();
        }

        PlatformService::notifyStatus(QObject::tr("Bağlantı kesildi..."));

        // Tray'e "Broker Bağlantısı Koptu" bilgisini gönder
        if (m_localServer) {
            okidia::AgentStatus st;
            st.set_broker_connected(false);
            okidia::Envelope stEnv;
            stEnv.set_type(okidia::MessageType::AGENT_STATUS);
            stEnv.set_payload(st.SerializeAsString());
            m_localServer->broadcast(stEnv);
        }
    });

    connect(m_mdns, &MdnsManager::serviceFound, this,
            [this](const QString& name, const QString& host, quint16 port, const QString& /*role*/, const QString& cafp) {
        // Çoklu Broker ortamı ve CA eşleştirme denetimi:
        // Eğer cihazda yerel ca.crt varsa ve mDNS'te cafp yayınlanmışsa, uyuşmayan broker'ları ele
        const QString caPath = OkidiaConfig::agentCertDir() + "/ca.crt";
        if (!cafp.isEmpty() && QFile::exists(caPath)) {
            QString localFp = KeyManager::getFingerprint(caPath);
            if (!localFp.isEmpty() && localFp.compare(cafp, Qt::CaseInsensitive) != 0) {
                Logger::info(QObject::tr("Agent: Farklı CA imzasına sahip yabancı Broker anonsu yok sayıldı → ") + name + " (" + host + ")");
                return;
            }
        }
        stopFallbackTimer();
        connectToBroker(host, port);
    });

    const bool hasCert = QFile::exists(OkidiaConfig::agentCertDir() + "/agent.crt") &&
                         QFile::exists(OkidiaConfig::agentCertDir() + "/agent.key");
    if (!hasCert) {
        DiscoveryService::generateOrGetCsr();
    }

    if (!m_configuredBrokerIp.isEmpty()) {
        Logger::info(QObject::tr("Agent: Statik broker yapılandırması bulundu → ") + m_configuredBrokerIp);
        connectToBroker(m_configuredBrokerIp, m_configuredBrokerPort);
    } else {
        DiscoveryService(m_mdns).startDiscovery(m_configuredBrokerIp);
        startFallbackTimer();
        PlatformService::notifyStatus(QObject::tr("Broker aranıyor..."));
        Logger::info(QObject::tr("Agent: Hazır, broker bekleniyor (mDNS)..."));
    }

    setupNetworkMonitoring();
}

void AgentDiscoveryCoordinator::startFallbackTimer() {
    if (!m_configuredBrokerIp.isEmpty()) return;
    if (!m_dnsFallbackTimer) {
        m_dnsFallbackTimer = new QTimer(this);
        m_dnsFallbackTimer->setSingleShot(true);
        connect(m_dnsFallbackTimer, &QTimer::timeout, this, &AgentDiscoveryCoordinator::tryDnsFallback);
    }
    // 5 saniye mDNS yanıtı gelmezse DNS fallback dene
    m_dnsFallbackTimer->start(5000);
}

void AgentDiscoveryCoordinator::stopFallbackTimer() {
    if (m_dnsFallbackTimer) {
        m_dnsFallbackTimer->stop();
    }
}

void AgentDiscoveryCoordinator::tryDnsFallback() {
    if (m_tls->isConnected() || m_tls->isConnecting()) return;

    Logger::info(QObject::tr("Agent: mDNS 5 saniyede yanıt vermedi (L3 / VLAN izolasyonu olabilir). DNS Fallback sorgusu başlatılıyor..."));
    
    // 1. okidia-broker dene
    QHostInfo::lookupHost(QString::fromLatin1(OkidiaConfig::DEFAULT_BROKER_HOSTNAME), this, [this](const QHostInfo& info) {
        if (m_tls->isConnected() || m_tls->isConnecting()) return;

        if (info.error() == QHostInfo::NoError && !info.addresses().isEmpty()) {
            QString resolvedIp = info.addresses().first().toString();
            Logger::info(QObject::tr("Agent: DNS fallback başarılı (okidia-broker) → ") + resolvedIp);
            connectToBroker(resolvedIp, OkidiaConfig::DEFAULT_PORT);
            return;
        }

        // 2. broker.okidia.local dene
        QHostInfo::lookupHost(QString::fromLatin1(OkidiaConfig::DEFAULT_BROKER_FQDN), this, [this](const QHostInfo& info2) {
            if (m_tls->isConnected() || m_tls->isConnecting()) return;

            if (info2.error() == QHostInfo::NoError && !info2.addresses().isEmpty()) {
                QString resolvedIp = info2.addresses().first().toString();
                Logger::info(QObject::tr("Agent: DNS fallback başarılı (broker.okidia.local) → ") + resolvedIp);
                connectToBroker(resolvedIp, OkidiaConfig::DEFAULT_PORT);
            } else {
                Logger::info(QObject::tr("Agent: DNS fallback ile broker bulunamadı. mDNS araması devam ediyor..."));
                startFallbackTimer();
            }
        });
    });
}

void AgentDiscoveryCoordinator::setupNetworkMonitoring() {
    if (QNetworkInformation::loadDefaultBackend()) {
        QNetworkInformation* netInfo = QNetworkInformation::instance();
        if (netInfo) {
            connect(netInfo, &QNetworkInformation::reachabilityChanged, this,
                    [this](QNetworkInformation::Reachability reachability) {
                QString reachStr = QMetaEnum::fromType<QNetworkInformation::Reachability>()
                                             .valueToKey(static_cast<int>(reachability));
                if (reachStr.isEmpty()) reachStr = "Unknown";

                Logger::info(QObject::tr("Agent: Ağ erişilebilirliği değişti → ") + reachStr);

                if (reachability == QNetworkInformation::Reachability::Online) {
                    Logger::info(QObject::tr("Agent: Ağ bağlantısı yeniden kuruldu, reconnect tetikleniyor"));
                    if (m_tls->isConnected()) {
                        m_tls->forceReconnect();
                    }
                    QTimer::singleShot(1000, this, [this]() {
                        if (!m_configuredBrokerIp.isEmpty()) {
                            connectToBroker(m_configuredBrokerIp, m_configuredBrokerPort);
                        } else {
                            DiscoveryService(m_mdns).restartDiscovery();
                            startFallbackTimer();
                        }
                    });
                } else {
                    Logger::warning(QObject::tr("Agent: Ağ bağlantısı kesildi (reachability: ") + reachStr + ")");
                    if (m_tls->isConnected()) {
                        m_tls->forceReconnect();
                    }
                }
            });
            Logger::info(QObject::tr("Agent: QNetworkInformation izleyici başlatıldı (backend: ")
                         + netInfo->backendName() + ")");
        }
    } else {
        Logger::warning(QObject::tr("Agent: QNetworkInformation backend yüklenemedi, polling fallback aktif"));
        QTimer* networkWatcher = new QTimer(this);
        networkWatcher->setInterval(3000);
        connect(networkWatcher, &QTimer::timeout, this, [this]() {
            static QString lastIp;

            QString currentIp;
            for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
                if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
                if (!iface.flags().testFlag(QNetworkInterface::IsUp))      continue;
                if (!iface.flags().testFlag(QNetworkInterface::IsRunning)) continue;
                for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                    if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                        && !entry.ip().isLoopback()) {
                        currentIp = entry.ip().toString();
                        break;
                    }
                }
                if (!currentIp.isEmpty()) break;
            }

            if (lastIp.isEmpty()) { lastIp = currentIp; return; }
            if (currentIp.isEmpty() || currentIp == lastIp) return;

            Logger::info(QObject::tr("Agent: IP değişimi tespit edildi → ") + lastIp + " → " + currentIp);
            lastIp = currentIp;

            if (m_tls->isConnected()) m_tls->forceReconnect();
            QTimer::singleShot(1000, this, [this]() {
                if (!m_configuredBrokerIp.isEmpty()) {
                    connectToBroker(m_configuredBrokerIp, m_configuredBrokerPort);
                } else {
                    DiscoveryService(m_mdns).restartDiscovery();
                    startFallbackTimer();
                }
            });
        });
        networkWatcher->start();
    }
}
