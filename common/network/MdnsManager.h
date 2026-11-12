// common/network/MdnsManager.h
#pragma once

#include <QObject>
#include <QString>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QHostAddress>

#ifdef HAVE_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#endif

class MdnsManager : public QObject {
    Q_OBJECT

public:
    explicit MdnsManager(QObject* parent = nullptr);
    ~MdnsManager();

    // Broker: Servisi ağa kaydet
    // role: "MB" veya "SB" — TXT record olarak yayınlanır
    // caFingerprint: SHA-256 Kök CA özeti (cafp=... TXT kaydı)
    bool registerService(const QString& serviceName,
                         const QString& serviceType,
                         quint16 port,
                         const QString& role = "SB",
                         const QString& caFingerprint = QString());
    void unregisterService();

    // Agent/Master: Broker'ı bul
    bool startBrowsing(const QString& serviceType);
    void stopBrowsing();
    // Ağ arayüzü değişiminde: browser + Avahi client'i sıfırlayarak yeniden başlat
    void restartBrowsing(const QString& serviceType);

    // Ağ arayüzü eklendiğinde/çıkarıldığında (USB hotplug vb.) multicast gruplarını ve anonsları yenile
    void refreshInterfaces();

signals:
    // Broker bulundu
    void serviceFound(const QString& name,
                      const QString& host,
                      quint16 port,
                      const QString& role = "SB",
                      const QString& caFingerprint = QString());
    // Broker kayboldu
    void serviceLost(const QString& name);

private slots:
    void onPollTimer();
    void onMdnsDatagramReceived();
    void sendMdnsQuery();
    void onMdnsResponderDatagramReceived();
    void sendMdnsAnnouncement();

private:
#ifdef HAVE_AVAHI
    AvahiSimplePoll*    m_poll         = nullptr;
    AvahiClient*        m_client       = nullptr;
    AvahiEntryGroup*    m_group        = nullptr;
    AvahiServiceBrowser*m_browser      = nullptr;

    // Avahi callback'leri (static, C API)
    static void clientCallback(AvahiClient* client,
                                AvahiClientState state,
                                void* userdata);
    static void groupCallback(AvahiEntryGroup* group,
                               AvahiEntryGroupState state,
                               void* userdata);
    static void browseCallback(AvahiServiceBrowser* browser,
                                AvahiIfIndex interface,
                                AvahiProtocol protocol,
                                AvahiBrowserEvent event,
                                const char* name,
                                const char* type,
                                const char* domain,
                                AvahiLookupResultFlags flags,
                                void* userdata);
    static void resolveCallback(AvahiServiceResolver* resolver,
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
                                 void* userdata);
#endif

    QTimer*        m_pollTimer = nullptr;
    QUdpSocket*    m_udpSocket = nullptr;
    QTimer*        m_queryTimer = nullptr;
    QUdpSocket*    m_responderSocket = nullptr;
    QTimer*        m_announceTimer = nullptr;
    QString        m_serviceName;
    QString        m_originalServiceName;
    QString        m_serviceType;
    quint16        m_port = 0;
    QString        m_role = "SB";  // TXT record: role=MB|SB
    QString        m_caFingerprint;
    int            m_collisionCount = 0;
    QSet<QString>  m_loggedBrowse;   // tekrar log önleme
};