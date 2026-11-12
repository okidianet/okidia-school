#ifndef DISCOVERYSERVICE_H
#define DISCOVERYSERVICE_H

#include <QString>
#include <QByteArray>
#include <QObject>

class Connection;
class MdnsManager;

class DiscoveryService : public QObject {
    Q_OBJECT
public:
    explicit DiscoveryService(MdnsManager* mdns, QObject* parent = nullptr);

    // Starts mDNS browsing if static IP is not configured
    void startDiscovery(const QString& configuredBrokerIp);
    
    // Restarts mDNS browsing
    void restartDiscovery();

    // Generates CSR and Private Key if they don't exist, returns CSR in PEM format
    static QByteArray generateOrGetCsr();

    // Sends the DISCOVERY_REQUEST envelope over the anonymous connection
    static void sendDiscoveryRequest(Connection* conn);

    // Sends TIME_SYNC_REQUEST over the connection
    static void sendTimeSyncRequest(Connection* conn, bool rtcFaultSuspected = false, bool isFirstEnrollment = false);

private:
    MdnsManager* m_mdns;
};

#endif // DISCOVERYSERVICE_H
