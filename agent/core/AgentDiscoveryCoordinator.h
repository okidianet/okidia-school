#pragma once

#include <QObject>
#include <QString>

class MdnsManager;
class TlsClient;
class LocalServer;

class AgentDiscoveryCoordinator : public QObject {
    Q_OBJECT
public:
    explicit AgentDiscoveryCoordinator(TlsClient* tls, LocalServer* localServer, QObject* parent = nullptr);

    void start();
    void connectToBroker(const QString& host, quint16 port);

private:
    void setupNetworkMonitoring();
    void tryDnsFallback();
    void startFallbackTimer();
    void stopFallbackTimer();

    TlsClient*    m_tls = nullptr;
    LocalServer*  m_localServer = nullptr;
    MdnsManager*  m_mdns = nullptr;
    QString       m_configuredBrokerIp;
    int           m_configuredBrokerPort = 0;
    class QTimer* m_dnsFallbackTimer = nullptr;
};
