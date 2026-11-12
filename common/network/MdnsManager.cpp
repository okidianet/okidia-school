#include "MdnsManager.h"
#include "../utils/Logger.h"
#include <QUdpSocket>
#include <QDataStream>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QVariant>

#ifdef HAVE_AVAHI
#include <avahi-common/address.h>
#endif

MdnsManager::MdnsManager(QObject* parent)
    : QObject(parent) {

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(50); // 50ms'de bir Avahi event loop'u işle
    connect(m_pollTimer, &QTimer::timeout,
            this, &MdnsManager::onPollTimer);
}

MdnsManager::~MdnsManager() {
    unregisterService();
    stopBrowsing();

#ifdef HAVE_AVAHI
    if (m_client) {
        avahi_client_free(m_client);
        m_client = nullptr;
    }
    if (m_poll) {
        avahi_simple_poll_free(m_poll);
        m_poll = nullptr;
    }
#endif
}

static void writeDnsDomain(QDataStream& out, const QString& domain) {
    QString d = domain;
    if (d.startsWith('.')) d = d.mid(1);
    if (d.endsWith('.')) d.chop(1);
    const QStringList parts = d.split('.', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        QByteArray b = part.toUtf8();
        out << quint8(b.size());
        for (int i = 0; i < b.size(); ++i) {
            out << quint8(b.at(i));
        }
    }
    out << quint8(0);
}

static void writeDnsDomainToByteStream(QByteArray& buf, const QString& domain) {
    QDataStream ds(&buf, QIODevice::WriteOnly | QIODevice::Append);
    ds.setByteOrder(QDataStream::BigEndian);
    writeDnsDomain(ds, domain);
}

static int interfacePriority(const QNetworkInterface& iface) {
    if (!iface.flags().testFlag(QNetworkInterface::IsUp) || !iface.flags().testFlag(QNetworkInterface::IsRunning)) {
        return 0; // Kapalı
    }
    if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
        return 1; // Loopback
    }

    QString name = (iface.humanReadableName() + " " + iface.name()).toLower();

    // Sanal / Host-Only / Hyper-V / WSL / Docker / VirtualBox / VMware
    if (name.contains("virtualbox") || name.contains("vmware") || 
        name.contains("hyper-v") || name.contains("vethernet") || 
        name.contains("wsl") || name.contains("docker") ||
        name.contains("host-only") || name.contains("tap-") || 
        name.contains("tun-") || name.contains("sandbox") ||
        name.contains("npcap")) {
        return 2; // Sanal arayüz
    }

    // VPN / Tailscale / ZeroTier
    if (name.contains("tailscale") || name.contains("zerotier") || name.contains("vpn")) {
        return 5; // VPN
    }

    // Fiziksel Wi-Fi veya Ethernet (En Yüksek Öncelik)
    return 10;
}

static int ipPriority(const QString& ipStr) {
    if (ipStr.isEmpty() || ipStr.startsWith("127.")) return 1;

    // VirtualBox Host-Only varsayılan ağı (192.168.56.x)
    if (ipStr.startsWith("192.168.56.")) return 2;

    // Docker / WSL varsayılanları (172.17.x, 172.18.x)
    if (ipStr.startsWith("172.17.") || ipStr.startsWith("172.18.") || ipStr.startsWith("172.19.") || ipStr.startsWith("172.20.")) return 2;

    // VirtualBox / QEMU NAT (10.0.2.x)
    if (ipStr.startsWith("10.0.2.")) return 2;

    // Standart Özel LAN aralıkları (Fiziksel Ev/Okul Ağları)
    if (ipStr.startsWith("192.168.") || ipStr.startsWith("10.") || ipStr.startsWith("172.")) return 10;

    // Diğer tüm geçerli IP'ler
    return 8;
}

static QList<QNetworkInterface> sortedInterfaces() {
    auto ifaces = QNetworkInterface::allInterfaces();
    std::sort(ifaces.begin(), ifaces.end(), [](const QNetworkInterface& a, const QNetworkInterface& b) {
        return interfacePriority(a) > interfacePriority(b);
    });
    return ifaces;
}

static QList<QHostAddress> sortedLocalIpv4Addresses() {
    QList<QHostAddress> result;
    const auto ifaces = sortedInterfaces();
    for (const QNetworkInterface& iface : ifaces) {
        if (interfacePriority(iface) < 5) continue; // VirtualBox, VMware, Hyper-V, Docker vb. sanal adaptörleri kesinlikle hariç tut
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                if (ipPriority(entry.ip().toString()) > 2) {
                    if (!result.contains(entry.ip())) {
                        result.append(entry.ip());
                    }
                }
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const QHostAddress& a, const QHostAddress& b) {
        return ipPriority(a.toString()) > ipPriority(b.toString());
    });
    // Fallback: Eğer hiçbir fiziksel LAN arayüzü bulunamadıysa loopback veya ilk arayüz
    if (result.isEmpty()) {
        for (const QNetworkInterface& iface : ifaces) {
            if (iface.flags().testFlag(QNetworkInterface::IsUp) && !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
                for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                    if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                        result.append(entry.ip());
                        break;
                    }
                }
            }
            if (!result.isEmpty()) break;
        }
    }
    if (result.isEmpty()) {
        result.append(QHostAddress("127.0.0.1"));
    }
    return result;
}

bool MdnsManager::registerService(const QString& serviceName,
                                   const QString& serviceType,
                                   quint16 port,
                                   const QString& role,
                                   const QString& caFingerprint) {
#ifdef HAVE_AVAHI
    m_role = role;
    m_serviceName = serviceName;
    m_originalServiceName = serviceName;
    m_serviceType = serviceType;
    m_port        = port;
    m_caFingerprint = caFingerprint;
    m_collisionCount = 0;

    // Avahi simple poll oluştur
    m_poll = avahi_simple_poll_new();
    if (!m_poll) {
        Logger::error(QObject::tr("MdnsManager: avahi_simple_poll_new başarısız"));
        return false;
    }

    // Avahi client oluştur
    int error = 0;
    m_client = avahi_client_new(
        avahi_simple_poll_get(m_poll),
        AVAHI_CLIENT_NO_FAIL,
        clientCallback,
        this,
        &error
    );

    if (!m_client) {
        Logger::error(QObject::tr("MdnsManager: avahi_client_new başarısız: ") +
                      QString::fromLatin1(avahi_strerror(error)));
        return false;
    }

    m_pollTimer->start();
    Logger::info(QObject::tr("MdnsManager: Servis kaydı başlatılıyor → ") +
                 serviceName + " " + serviceType +
                 ":" + QString::number(port) +
                 (caFingerprint.isEmpty() ? "" : (" (cafp: " + caFingerprint.left(8) + "...)")));
    return true;
#else
    m_serviceName = serviceName;
    m_originalServiceName = serviceName;
    m_serviceType = serviceType;
    m_port        = port;
    m_role        = role;
    m_caFingerprint = caFingerprint;
    m_collisionCount = 0;

    if (!m_responderSocket) {
        m_responderSocket = new QUdpSocket(this);
        m_responderSocket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, QVariant(1));
        bool bound = m_responderSocket->bind(QHostAddress::AnyIPv4, 5353, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        if (!bound) {
            Logger::warning(QObject::tr("MdnsManager (Native UDP Responder): Port 5353 bağlanamadı, rastgele porta geçiliyor"));
            m_responderSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        }

        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& iface : interfaces) {
            if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
                iface.flags().testFlag(QNetworkInterface::IsRunning) &&
                iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
                m_responderSocket->joinMulticastGroup(QHostAddress("224.0.0.251"), iface);
            }
        }
        m_responderSocket->joinMulticastGroup(QHostAddress("224.0.0.251"));
        connect(m_responderSocket, &QUdpSocket::readyRead, this, &MdnsManager::onMdnsResponderDatagramReceived);
    }

    if (!m_announceTimer) {
        m_announceTimer = new QTimer(this);
        m_announceTimer->setInterval(3000); // 3 saniyede bir periyodik duyuru
        connect(m_announceTimer, &QTimer::timeout, this, &MdnsManager::sendMdnsAnnouncement);
    }
    m_announceTimer->start();
    sendMdnsAnnouncement();
    Logger::info(QObject::tr("MdnsManager (Native UDP Responder): Servis kaydedildi ve yayınlanıyor → %1 (%2:%3, Rol: %4)")
                 .arg(m_serviceName, m_serviceType).arg(m_port).arg(m_role));
    return true;
#endif
}

void MdnsManager::unregisterService() {
#ifdef HAVE_AVAHI
    m_pollTimer->stop();

    if (m_group) {
        avahi_entry_group_free(m_group);
        m_group = nullptr;
    }
    Logger::info(QObject::tr("MdnsManager: Servis kaydı kaldırıldı"));
#else
    if (m_announceTimer) {
        m_announceTimer->stop();
    }
    if (m_responderSocket) {
        m_responderSocket->close();
        delete m_responderSocket;
        m_responderSocket = nullptr;
    }
    Logger::info(QObject::tr("MdnsManager (Native UDP Responder): Servis kaydı kaldırıldı"));
#endif
}

void MdnsManager::refreshInterfaces() {
#ifdef HAVE_AVAHI
    // Avahi arka planda netlink dinler; grup kurulmuşsa commit durumunu doğrula
    if (m_client && avahi_client_get_state(m_client) == AVAHI_CLIENT_S_RUNNING) {
        Logger::info(QObject::tr("MdnsManager: Avahi ağ arayüzleri güncellendi."));
    }
#else
    if (!m_responderSocket) return;

    Logger::info(QObject::tr("MdnsManager (Native UDP Responder): Ağ arayüzleri yenileniyor (USB hotplug / link change)..."));
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            iface.flags().testFlag(QNetworkInterface::IsRunning) &&
            iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
            m_responderSocket->joinMulticastGroup(QHostAddress("224.0.0.251"), iface);
        }
    }
    sendMdnsAnnouncement();
#endif
}

void MdnsManager::sendMdnsAnnouncement() {
#ifndef HAVE_AVAHI
    if (!m_responderSocket) return;

    QString serviceDomain = m_serviceType;
    if (!serviceDomain.endsWith(".local", Qt::CaseInsensitive)) {
        serviceDomain += ".local";
    }
    QString instanceDomain = m_serviceName + "." + serviceDomain;
    QString hostDomain = QHostInfo::localHostName();
    if (!hostDomain.endsWith(".local", Qt::CaseInsensitive)) {
        hostDomain += ".local";
    }

    // RFC 6762 Multihomed Hosts standardı:
    // Her aktif ağ arayüzü (örneğin Okul İdare Ağı 192.168.1.x, Laboratuvar Ağı 10.0.0.x, Akıllı Tahta Ağı 172.16.x)
    // için ayrı bir mDNS anons paketi oluşturulur.
    // Her ağ arayüzü paketinin A kaydına YALNIZCA o arayüzün kendi yerel IP adresi yazılır.
    // Böylece izole ağlardaki istemciler yalnızca kendilerine bakan bacağın IP'sini öğrenir;
    // bir ağın IP'si başka bir ağa asla sızmaz ve Broker tüm izole ağları tek çatı altında bağlar.
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) ||
            !iface.flags().testFlag(QNetworkInterface::IsRunning) ||
            iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        // Sanal adaptörleri (VirtualBox Host-Only vb.) filtrele (ancak gerçek ağ adaptörleri 10.x, 172.x, 192.168.x tam desteklenir)
        if (interfacePriority(iface) < 5) continue;

        QList<QHostAddress> ifaceIps;
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                if (ipPriority(entry.ip().toString()) > 2) {
                    ifaceIps.append(entry.ip());
                }
            }
        }
        if (ifaceIps.isEmpty()) continue;

        // Bu arayüze özel DNS Response paketi
        QByteArray packet;
        QDataStream out(&packet, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::BigEndian);

        out << quint16(0x0000); // ID
        out << quint16(0x8400); // Flags: Standard response, Authoritative Answer
        out << quint16(0);      // QDCOUNT (0 questions)
        quint16 ancount = 3 + ifaceIps.size(); // PTR + SRV + TXT + Bu arayüzün A kayıtları
        out << quint16(ancount);
        out << quint16(0);
        out << quint16(0);

        // 1. PTR Record
        writeDnsDomain(out, serviceDomain);
        out << quint16(12) << quint16(0x8001) << quint32(120);
        QByteArray ptrRdata;
        writeDnsDomainToByteStream(ptrRdata, instanceDomain);
        out << quint16(ptrRdata.size());
        out.writeRawData(ptrRdata.constData(), ptrRdata.size());

        // 2. SRV Record
        writeDnsDomain(out, instanceDomain);
        out << quint16(33) << quint16(0x8001) << quint32(120);
        QByteArray srvRdata;
        QDataStream srvOut(&srvRdata, QIODevice::WriteOnly);
        srvOut.setByteOrder(QDataStream::BigEndian);
        srvOut << quint16(0) << quint16(0) << quint16(m_port);
        writeDnsDomain(srvOut, hostDomain);
        out << quint16(srvRdata.size());
        out.writeRawData(srvRdata.constData(), srvRdata.size());

        // 3. TXT Record
        writeDnsDomain(out, instanceDomain);
        out << quint16(16) << quint16(0x8001) << quint32(120);
        QByteArray txtRdata;
        QByteArray roleTxt = ("role=" + m_role).toUtf8();
        txtRdata.append(static_cast<char>(roleTxt.size()));
        txtRdata.append(roleTxt);
        QByteArray nameTxt = ("name=" + m_serviceName).toUtf8();
        txtRdata.append(static_cast<char>(nameTxt.size()));
        txtRdata.append(nameTxt);
        if (!m_caFingerprint.isEmpty()) {
            QByteArray cafpTxt = ("cafp=" + m_caFingerprint).toUtf8();
            txtRdata.append(static_cast<char>(cafpTxt.size()));
            txtRdata.append(cafpTxt);
        }
        out << quint16(txtRdata.size());
        out.writeRawData(txtRdata.constData(), txtRdata.size());

        // 4. A Records — SADECE BU ARAYÜZÜN IP'Sİ
        for (const QHostAddress& ip : ifaceIps) {
            writeDnsDomain(out, hostDomain);
            out << quint16(1) << quint16(0x8001) << quint32(120) << quint16(4);
            quint32 ipv4 = ip.toIPv4Address();
            out << quint8((ipv4 >> 24) & 0xFF);
            out << quint8((ipv4 >> 16) & 0xFF);
            out << quint8((ipv4 >> 8) & 0xFF);
            out << quint8(ipv4 & 0xFF);
        }

        // Paketi YALNIZCA bu arayüz üzerinden multicast (224.0.0.251) ve subnet broadcast adresine gönder
        m_responderSocket->setMulticastInterface(iface);
        m_responderSocket->writeDatagram(packet, QHostAddress("224.0.0.251"), 5353);

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull()) {
                m_responderSocket->writeDatagram(packet, entry.broadcast(), 5353);
            }
        }
    }

    // Localhost döngüsü
    const auto localIps = sortedLocalIpv4Addresses();
    if (!localIps.isEmpty()) {
        QByteArray loopbackPacket;
        QDataStream loopOut(&loopbackPacket, QIODevice::WriteOnly);
        loopOut.setByteOrder(QDataStream::BigEndian);
        loopOut << quint16(0x0000) << quint16(0x8400) << quint16(0) << quint16(4) << quint16(0) << quint16(0);

        writeDnsDomain(loopOut, serviceDomain);
        loopOut << quint16(12) << quint16(0x8001) << quint32(120);
        QByteArray pData; writeDnsDomainToByteStream(pData, instanceDomain);
        loopOut << quint16(pData.size());
        loopOut.writeRawData(pData.constData(), pData.size());

        writeDnsDomain(loopOut, instanceDomain);
        loopOut << quint16(33) << quint16(0x8001) << quint32(120);
        QByteArray sData; QDataStream sDs(&sData, QIODevice::WriteOnly); sDs.setByteOrder(QDataStream::BigEndian);
        sDs << quint16(0) << quint16(0) << quint16(m_port);
        writeDnsDomain(sDs, hostDomain);
        loopOut << quint16(sData.size());
        loopOut.writeRawData(sData.constData(), sData.size());

        writeDnsDomain(loopOut, instanceDomain);
        loopOut << quint16(16) << quint16(0x8001) << quint32(120);
        QByteArray tData;
        QByteArray rTxt = ("role=" + m_role).toUtf8(); tData.append(static_cast<char>(rTxt.size())); tData.append(rTxt);
        QByteArray nTxt = ("name=" + m_serviceName).toUtf8(); tData.append(static_cast<char>(nTxt.size())); tData.append(nTxt);
        if (!m_caFingerprint.isEmpty()) {
            QByteArray cTxt = ("cafp=" + m_caFingerprint).toUtf8(); tData.append(static_cast<char>(cTxt.size())); tData.append(cTxt);
        }
        loopOut << quint16(tData.size());
        loopOut.writeRawData(tData.constData(), tData.size());

        writeDnsDomain(loopOut, hostDomain);
        loopOut << quint16(1) << quint16(0x8001) << quint32(120) << quint16(4);
        quint32 lIpv4 = localIps.first().toIPv4Address();
        loopOut << quint8((lIpv4 >> 24) & 0xFF) << quint8((lIpv4 >> 16) & 0xFF) << quint8((lIpv4 >> 8) & 0xFF) << quint8(lIpv4 & 0xFF);

        m_responderSocket->writeDatagram(loopbackPacket, QHostAddress::LocalHost, 5353);
    }
#endif
}

void MdnsManager::onMdnsResponderDatagramReceived() {
#ifndef HAVE_AVAHI
    while (m_responderSocket && m_responderSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_responderSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort = 0;
        m_responderSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (datagram.size() < 12) continue;

        // QR bayrağı (Byte 2, Bit 7): 0 = Query (Sorgu)
        quint8 flagsHigh = static_cast<quint8>(datagram[2]);
        if ((flagsHigh & 0x80) != 0) {
            continue; // Response'lara cevap verme
        }

        // Sorgu _okidia ile ilgili mi?
        QByteArray lower = datagram.toLower();
        if (lower.contains("_okidia") || lower.contains("okidia")) {
            sendMdnsAnnouncement();
        }
    }
#endif
}

bool MdnsManager::startBrowsing(const QString& serviceType) {
#ifdef HAVE_AVAHI
    m_serviceType = serviceType;

    if (!m_poll) {
        m_poll = avahi_simple_poll_new();
        if (!m_poll) {
            Logger::error(QObject::tr("MdnsManager: Poll oluşturulamadı"));
            return false;
        }
    }

    if (!m_client) {
        int error = 0;
        m_client = avahi_client_new(
            avahi_simple_poll_get(m_poll),
            AVAHI_CLIENT_NO_FAIL,
            clientCallback,
            this,
            &error
        );
        if (!m_client) {
            Logger::error(QObject::tr("MdnsManager: Client oluşturulamadı: ") +
                          QString::fromLatin1(avahi_strerror(error)));
            return false;
        }
    }

    m_browser = avahi_service_browser_new(
        m_client,
        AVAHI_IF_UNSPEC,
        AVAHI_PROTO_UNSPEC,
        serviceType.toUtf8().constData(),
        nullptr,
        (AvahiLookupFlags)0,
        browseCallback,
        this
    );

    if (!m_browser) {
        Logger::error(QObject::tr("MdnsManager: Browser oluşturulamadı: ") +
                      QString::fromLatin1(
                          avahi_strerror(avahi_client_errno(m_client))));
        return false;
    }

    m_pollTimer->start();
    Logger::info(QObject::tr("MdnsManager: Servis aranıyor → ") + serviceType);
    return true;
#else
    m_serviceType = serviceType;
    if (!m_udpSocket) {
        m_udpSocket = new QUdpSocket(this);
        m_udpSocket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, QVariant(1));
        bool bound = m_udpSocket->bind(QHostAddress::AnyIPv4, 5353, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        if (!bound) {
            m_udpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        }
        
        // Tüm aktif ağ arayüzlerinde çoklu yayın grubuna katıl
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& iface : interfaces) {
            if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
                iface.flags().testFlag(QNetworkInterface::IsRunning) &&
                iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
                m_udpSocket->joinMulticastGroup(QHostAddress("224.0.0.251"), iface);
            }
        }
        m_udpSocket->joinMulticastGroup(QHostAddress("224.0.0.251"));
        connect(m_udpSocket, &QUdpSocket::readyRead, this, &MdnsManager::onMdnsDatagramReceived);
    }
    if (!m_queryTimer) {
        m_queryTimer = new QTimer(this);
        m_queryTimer->setInterval(2000);
        connect(m_queryTimer, &QTimer::timeout, this, &MdnsManager::sendMdnsQuery);
    }
    m_queryTimer->start();
    sendMdnsQuery();
    Logger::info(QObject::tr("MdnsManager (Native UDP): Servis aranıyor (224.0.0.251:5353) → ") + serviceType);
    return true;
#endif
}

void MdnsManager::stopBrowsing() {
#ifdef HAVE_AVAHI
    m_pollTimer->stop();

    if (m_browser) {
        avahi_service_browser_free(m_browser);
        m_browser = nullptr;
    }
#else
    if (m_queryTimer) {
        m_queryTimer->stop();
    }
    if (m_udpSocket) {
        m_udpSocket->close();
        delete m_udpSocket;
        m_udpSocket = nullptr;
    }
#endif
}

void MdnsManager::sendMdnsQuery() {
    if (!m_udpSocket) return;

    auto buildPacket = [this](quint16 qclass) -> QByteArray {
        QByteArray packet;
        QDataStream out(&packet, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::BigEndian);

        // DNS Header
        out << quint16(0x0000);  // Transaction ID
        out << quint16(0x0000);  // Flags: Standard query
        out << quint16(1);       // Questions: 1
        out << quint16(0);       // Answer RRs: 0
        out << quint16(0);       // Authority RRs: 0
        out << quint16(0);       // Additional RRs: 0

        // Question: e.g. "_okidia._tcp.local"
        QString target = m_serviceType;
        if (!target.endsWith(".local", Qt::CaseInsensitive)) {
            target += ".local";
        }
        const QStringList labels = target.split('.', Qt::SkipEmptyParts);
        for (const QString& label : labels) {
            QByteArray lBytes = label.toUtf8();
            out << quint8(lBytes.size());
            for (int i = 0; i < lBytes.size(); ++i) {
                out << quint8(lBytes.at(i));
            }
        }
        out << quint8(0);        // Terminating zero length

        // QTYPE: PTR (12), QCLASS: 0x8001 (QU: Unicast Response) veya 0x0001 (QM: Multicast Response)
        out << quint16(12);
        out << quint16(qclass);
        return packet;
    };

    QByteArray packetQU = buildPacket(0x8001); // QU: Unicast-Response (Windows 10 rastgele port için zorunlu)
    QByteArray packetQM = buildPacket(0x0001); // QM: Standart Multicast-Response

    // Tüm ağ arayüzlerine gönder
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (interfacePriority(iface) < 5) continue; // Sanal arayüzleri atla
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            iface.flags().testFlag(QNetworkInterface::IsRunning)) {

            m_udpSocket->setMulticastInterface(iface);
            m_udpSocket->writeDatagram(packetQU, QHostAddress("224.0.0.251"), 5353);
            m_udpSocket->writeDatagram(packetQM, QHostAddress("224.0.0.251"), 5353);

            for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull()) {
                    m_udpSocket->writeDatagram(packetQU, entry.broadcast(), 5353);
                    m_udpSocket->writeDatagram(packetQM, entry.broadcast(), 5353);
                }
            }
        }
    }
    m_udpSocket->writeDatagram(packetQU, QHostAddress::LocalHost, 5353);
    m_udpSocket->writeDatagram(packetQM, QHostAddress::LocalHost, 5353);
}

void MdnsManager::onMdnsDatagramReceived() {
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (datagram.size() < 12) continue;

        // DNS Header QR bayrağı (Byte 2, Bit 7): 0 = Query (Sorgu), 1 = Response (Cevap)
        quint8 flagsHigh = static_cast<quint8>(datagram[2]);
        if ((flagsHigh & 0x80) == 0) {
            continue;
        }

        // Okidia servisiyle ilgili mi?
        QByteArray lower = datagram.toLower();
        if (!lower.contains("_okidia") && !lower.contains("okidia")) {
            continue;
        }

        quint16 port = 11200; // Varsayılan Okidia portu
        QString foundHost = sender.toString();
        if (foundHost.startsWith("::ffff:")) {
            foundHost = foundHost.mid(7);
        }
        QString foundName = "OKIDIA Broker";
        QString foundRole = "SB";
        QString foundCafp;

        if (datagram.contains("role=MB")) {
            foundRole = "MB";
        } else if (datagram.contains("role=SB")) {
            foundRole = "SB";
        }

        // 1. Standart DNS TXT kaydını ara (Type 16 = 0x0010): Type(2B), Class(2B), TTL(4B), Len(2B), RDATA(Len B)
        // RFC 1035 §3.3.14: TXT RDATA bir veya daha fazla length-prefixed (<1B len><veri>) dizgiden oluşur.
        for (int i = 12; i < datagram.size() - 10; ++i) {
            if (static_cast<quint8>(datagram[i]) == 0x00 && static_cast<quint8>(datagram[i+1]) == 0x10) {
                quint16 rdLen = (static_cast<quint8>(datagram[i+8]) << 8) | static_cast<quint8>(datagram[i+9]);
                int rdataPos = i + 10;
                if (rdataPos + rdLen <= datagram.size()) {
                    int pos = rdataPos;
                    int end = rdataPos + rdLen;
                    while (pos < end) {
                        quint8 strLen = static_cast<quint8>(datagram[pos++]);
                        if (pos + strLen > end) break;
                        QByteArray entry = datagram.mid(pos, strLen);
                        pos += strLen;

                        if (entry.startsWith("name=")) {
                            QString n = QString::fromUtf8(entry.mid(5)).trimmed();
                            if (!n.isEmpty()) foundName = n;
                        } else if (entry.startsWith("role=")) {
                            QString r = QString::fromUtf8(entry.mid(5)).trimmed();
                            if (!r.isEmpty()) foundRole = r;
                        } else if (entry.startsWith("cafp=")) {
                            foundCafp = QString::fromUtf8(entry.mid(5)).trimmed();
                        }
                    }
                }
                break;
            }
        }

        // 2. Fallback: Eğer TXT Type 16 bloğu ayrıştırılamadıysa, doğrudan uzunluk önekli (length-prefixed) arama yap
        if (foundName == "OKIDIA Broker") {
            int nameIdx = datagram.indexOf("name=");
            if (nameIdx > 0) {
                quint8 len = static_cast<quint8>(datagram[nameIdx - 1]);
                if (len >= 5 && nameIdx + len <= datagram.size()) {
                    QString n = QString::fromUtf8(datagram.mid(nameIdx + 5, len - 5)).trimmed();
                    if (!n.isEmpty()) foundName = n;
                }
            }
        }

        if (foundCafp.isEmpty()) {
            int cafpIdx = datagram.indexOf("cafp=");
            if (cafpIdx > 0) {
                quint8 len = static_cast<quint8>(datagram[cafpIdx - 1]);
                if (len >= 5 && cafpIdx + len <= datagram.size()) {
                    foundCafp = QString::fromUtf8(datagram.mid(cafpIdx + 5, len - 5)).trimmed();
                }
            }
        }

        // 3. Savunmacı Temizleme (Sanitization):
        // Her koşulda isim alanından cafp parmak izi sızıntısını temizle
        int cafpInName = foundName.indexOf("cafp=", 0, Qt::CaseInsensitive);
        if (cafpInName >= 0) {
            if (cafpInName > 0 && (foundName[cafpInName - 1] == 'd' || foundName[cafpInName - 1] == 'D' || foundName[cafpInName - 1].isSpace())) {
                cafpInName--;
            }
            foundName = foundName.left(cafpInName).trimmed();
        }

        // cafp temizliği: Standart SHA-256 özeti (en fazla 95 karakter hex ve ':')
        if (!foundCafp.isEmpty()) {
            QString cleanCafp;
            for (const QChar& ch : foundCafp) {
                if (ch.isLetterOrNumber() || ch == ':') {
                    cleanCafp.append(ch);
                } else {
                    break;
                }
            }
            if (cleanCafp.length() > 95) {
                cleanCafp = cleanCafp.left(95);
            }
            foundCafp = cleanCafp.trimmed();
        }

        // SRV kaydını ara (Type 33 = 0x0021): Type(2B), Class(2B), TTL(4B), Len(2B), Priority(2B), Weight(2B), Port(2B)
        for (int i = 12; i < datagram.size() - 16; ++i) {
            if ((quint8)datagram[i] == 0x00 && (quint8)datagram[i+1] == 0x21) {
                int portPos = i + 14;
                if (portPos + 2 <= datagram.size()) {
                    quint16 srvPort = (quint8(datagram[portPos]) << 8) | quint8(datagram[portPos + 1]);
                    if (srvPort > 0 && srvPort < 65535) {
                        port = srvPort;
                    }
                }
                break;
            }
        }

        // A kaydını ara (Type 1 = 0x0001): Type(2B), Class(2B), TTL(4B), Len=4(2B), IP(4B)
        QString discoveredIp;
        // A kaydını ara (Type 1 = 0x0001): Type(2B), Class(2B), TTL(4B), Len=4(2B), IP(4B)
        QStringList discoveredIps;
        for (int i = 12; i < datagram.size() - 14; ++i) {
            if ((quint8)datagram[i] == 0x00 && (quint8)datagram[i+1] == 0x01 &&
                (quint8)datagram[i+8] == 0x00 && (quint8)datagram[i+9] == 0x04) {
                quint8 a = (quint8)datagram[i+10];
                quint8 b = (quint8)datagram[i+11];
                quint8 c = (quint8)datagram[i+12];
                quint8 d = (quint8)datagram[i+13];
                if (a != 0) {
                    QString ipCandidate = QString("%1.%2.%3.%4").arg(a).arg(b).arg(c).arg(d);
                    if (!discoveredIps.contains(ipCandidate)) {
                        discoveredIps.append(ipCandidate);
                    }
                }
            }
        }

        // Önceliğe göre sırala (Fiziksel LAN IP'leri en başta)
        std::sort(discoveredIps.begin(), discoveredIps.end(), [](const QString& a, const QString& b) {
            return ipPriority(a) > ipPriority(b);
        });

        if (!discoveredIps.isEmpty()) {
            foundHost = discoveredIps.first();
        } else if (foundHost.isEmpty() || foundHost.startsWith("127.")) {
            const auto localSorted = sortedLocalIpv4Addresses();
            if (!localSorted.isEmpty()) {
                foundHost = localSorted.first().toString();
            } else {
                foundHost = "127.0.0.1";
            }
        }

        QString key = foundHost + ":" + QString::number(port);
        if (!m_loggedBrowse.contains(key)) {
            m_loggedBrowse.insert(key);
            Logger::info(QObject::tr("MdnsManager (Native UDP): Broker keşfedildi → %1:%2 (Rol: %3, İsim: %4, CA: %5)")
                         .arg(foundHost).arg(port).arg(foundRole, foundName, foundCafp.isEmpty() ? "-" : foundCafp.left(8) + "..."));
            emit serviceFound(foundName, foundHost, port, foundRole, foundCafp);
        }
    }
}

// ── Ağ değişimi sonrası yeniden keşif ────────────────────────────────────────
// Avahi client'ı da yeniden oluşturur; eski client yeni ağ arayüzünü
// göremeyebilir.  Browser + loggedBrowse temizlenerek taze bir keşif başlar.
void MdnsManager::restartBrowsing(const QString& serviceType) {
    Logger::info(QObject::tr("MdnsManager: Browser yeniden başlatılıyor (ağ değişimi)..."));
    stopBrowsing();

#ifdef HAVE_AVAHI
    // Eski client'ı serbest bırak — yeni ağ arayüzü için yeniden oluşturulacak
    if (m_client) {
        avahi_client_free(m_client);
        m_client = nullptr;
    }
    if (m_poll) {
        avahi_simple_poll_free(m_poll);
        m_poll = nullptr;
    }
#endif

    m_loggedBrowse.clear();
    startBrowsing(serviceType);
}

void MdnsManager::onPollTimer() {
#ifdef HAVE_AVAHI
    if (m_poll) {
        avahi_simple_poll_iterate(m_poll, 0);
    }
#endif
}

// ─── Avahi Callback'leri ──────────────────────────────────────

#ifdef HAVE_AVAHI

void MdnsManager::clientCallback(AvahiClient* client,
                                   AvahiClientState state,
                                   void* userdata) {
    MdnsManager* self = static_cast<MdnsManager*>(userdata);

    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        Logger::info(QObject::tr("MdnsManager: Avahi client hazır"));
        // Servis kaydı isteniyorsa şimdi yap
        if (!self->m_serviceName.isEmpty() && !self->m_group) {
            self->m_group = avahi_entry_group_new(
                client, groupCallback, self);

            if (self->m_group) {
                AvahiStringList* txtList = nullptr;
                txtList = avahi_string_list_add_printf(txtList, "role=%s", self->m_role.toUtf8().constData());
                txtList = avahi_string_list_add_printf(txtList, "name=%s", self->m_serviceName.toUtf8().constData());
                if (!self->m_caFingerprint.isEmpty()) {
                    txtList = avahi_string_list_add_printf(txtList, "cafp=%s", self->m_caFingerprint.toUtf8().constData());
                }

                avahi_entry_group_add_service_strlst(
                    self->m_group,
                    AVAHI_IF_UNSPEC,
                    AVAHI_PROTO_UNSPEC,
                    (AvahiPublishFlags)0,
                    self->m_serviceName.toUtf8().constData(),
                    self->m_serviceType.toUtf8().constData(),
                    nullptr, nullptr,
                    self->m_port,
                    txtList
                );
                avahi_string_list_free(txtList);
                avahi_entry_group_commit(self->m_group);
            }
        }
        break;

    case AVAHI_CLIENT_FAILURE:
        Logger::error(QObject::tr("MdnsManager: Avahi client hatası: ") +
                      QString::fromLatin1(
                          avahi_strerror(avahi_client_errno(client))));
        break;

    default:
        break;
    }
}

void MdnsManager::groupCallback(AvahiEntryGroup* group,
                                  AvahiEntryGroupState state,
                                  void* userdata) {
    Q_UNUSED(group)
    MdnsManager* self = static_cast<MdnsManager*>(userdata);

    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        Logger::info(QObject::tr("MdnsManager: Servis başarıyla kaydedildi → ") +
                     self->m_serviceName);
        break;

    case AVAHI_ENTRY_GROUP_COLLISION: {
        self->m_collisionCount++;
        QString newName = QString("%1-%2").arg(self->m_originalServiceName).arg(self->m_collisionCount + 1);
        Logger::warning(QObject::tr("MdnsManager: Servis adı çakışması! Yeni ad deneniyor → ") + newName);
        self->m_serviceName = newName;

        if (self->m_group) {
            avahi_entry_group_reset(self->m_group);
            AvahiStringList* txtList = nullptr;
            txtList = avahi_string_list_add_printf(txtList, "role=%s", self->m_role.toUtf8().constData());
            txtList = avahi_string_list_add_printf(txtList, "name=%s", self->m_serviceName.toUtf8().constData());
            if (!self->m_caFingerprint.isEmpty()) {
                txtList = avahi_string_list_add_printf(txtList, "cafp=%s", self->m_caFingerprint.toUtf8().constData());
            }

            avahi_entry_group_add_service_strlst(
                self->m_group,
                AVAHI_IF_UNSPEC,
                AVAHI_PROTO_UNSPEC,
                (AvahiPublishFlags)0,
                self->m_serviceName.toUtf8().constData(),
                self->m_serviceType.toUtf8().constData(),
                nullptr, nullptr,
                self->m_port,
                txtList
            );
            avahi_string_list_free(txtList);
            avahi_entry_group_commit(self->m_group);
        }
        break;
    }

    case AVAHI_ENTRY_GROUP_FAILURE:
        Logger::error(QObject::tr("MdnsManager: Grup hatası"));
        break;

    default:
        break;
    }
}

void MdnsManager::browseCallback(AvahiServiceBrowser* browser,
                                   AvahiIfIndex interface,
                                   AvahiProtocol protocol,
                                   AvahiBrowserEvent event,
                                   const char* name,
                                   const char* type,
                                   const char* domain,
                                   AvahiLookupResultFlags flags,
                                   void* userdata) {
    Q_UNUSED(browser) Q_UNUSED(flags)
    MdnsManager* self = static_cast<MdnsManager*>(userdata);
    AvahiClient* client = avahi_service_browser_get_client(browser);

    switch (event) {
    case AVAHI_BROWSER_NEW: {
        // Aynı servis birden fazla arayüz/protokol üzerinden duyurulabilir
        // (AVAHI_IF_UNSPEC + AVAHI_PROTO_UNSPEC). Her kombinasyon için
        // BROWSER_NEW gelir — sadece ilk kez logla.
        const QString svcName = QString::fromUtf8(name);
        if (!self->m_loggedBrowse.contains(svcName)) {
            self->m_loggedBrowse.insert(svcName);
            Logger::info(QObject::tr("MdnsManager: Servis bulundu → ") + svcName);
        }

        // Resolve et: IP ve port bilgisini al
        avahi_service_resolver_new(
            client,
            interface, protocol,
            name, type, domain,
            AVAHI_PROTO_INET,
            (AvahiLookupFlags)0,
            resolveCallback,
            self
        );
        break;
    }

    case AVAHI_BROWSER_REMOVE:
        Logger::info(QObject::tr("MdnsManager: Servis kayboldu → ") +
                     QString::fromUtf8(name));
        self->m_loggedBrowse.remove(QString::fromUtf8(name));
        emit self->serviceLost(QString::fromUtf8(name));
        break;

    case AVAHI_BROWSER_FAILURE:
        Logger::error(QObject::tr("MdnsManager: Browse hatası"));
        break;

    default:
        break;
    }
}

void MdnsManager::resolveCallback(AvahiServiceResolver* resolver,
                                   AvahiIfIndex interface,
                                   AvahiProtocol protocol,
                                   AvahiResolverEvent event,
                                   const char* name,
                                   const char* type,
                                   const char* domain,
                                   const char* host_name,
                                   const AvahiAddress* address,
                                   uint16_t port,
                                   AvahiStringList* txt,
                                   AvahiLookupResultFlags flags,
                                   void* userdata) {
    Q_UNUSED(interface) Q_UNUSED(protocol) Q_UNUSED(type)
    Q_UNUSED(domain) Q_UNUSED(txt) Q_UNUSED(flags)

    MdnsManager* self = static_cast<MdnsManager*>(userdata);

    if (event == AVAHI_RESOLVER_FOUND && address) {
        char addrStr[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(addrStr, sizeof(addrStr), address);

        QString host = QString::fromUtf8(host_name);
        QString addr = QString::fromUtf8(addrStr);

        Logger::info(QObject::tr("MdnsManager: Çözümlendi → ") +
                     QString::fromUtf8(name) +
                     " @ " + addr + ":" + QString::number(port));

        // TXT record'dan role oku (role=MB veya role=SB)
        QString role = "SB";
        QString cafp;
        if (txt) {
            char* val = nullptr;
            size_t valLen = 0;
            if (avahi_string_list_get_pair(
                    avahi_string_list_find(
                        const_cast<AvahiStringList*>(txt), "role"),
                    nullptr, &val, &valLen) == 0 && val) {
                role = QString::fromUtf8(val, static_cast<int>(valLen));
                avahi_free(val);
                val = nullptr;
            }
            if (avahi_string_list_get_pair(
                    avahi_string_list_find(
                        const_cast<AvahiStringList*>(txt), "cafp"),
                    nullptr, &val, &valLen) == 0 && val) {
                cafp = QString::fromUtf8(val, static_cast<int>(valLen));
                avahi_free(val);
            }
        }
        emit self->serviceFound(
            QString::fromUtf8(name),
            addr,
            port,
            role,
            cafp
        );
    } else {
        Logger::warning(QObject::tr("MdnsManager: Resolve başarısız → ") +
                        QString::fromUtf8(name));
        self->m_loggedBrowse.remove(QString::fromUtf8(name));
    }

    avahi_service_resolver_free(resolver);
}

#endif