// common/network/TlsServer.h
#pragma once

#include <QTcpServer>
#include <QSslConfiguration>
#include "Connection.h"

#include <QSet>
#include <QMap>
#include <QPointer>

class QTimer;

class TlsServer : public QTcpServer {
    Q_OBJECT

public:
    explicit TlsServer(QObject* parent = nullptr);
    ~TlsServer() override;

    // Sunucuyu başlat
    bool startListening(quint16 port,
                        const QString& certPath,
                        const QString& keyPath,
                        const QString& caCertPath);

    void stopListening();

    int activeConnectionCount() const { return m_activeSockets.size(); }

signals:
    // Yeni istemci bağlandı ve TLS el sıkışması tamamlandı
    void newConnection(Connection* connection);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private slots:
    void onSslHandshakeComplete();
    void onSslErrors(const QList<QSslError>& errors);
    void onSocketDisconnected();

private:
    QSslConfiguration m_sslConfig;
    QString           m_caCertPath;

    // DoS / Bağlantı Taşma Koruması
    static constexpr int MAX_CONCURRENT_CONNECTIONS = 500;
    static constexpr int HANDSHAKE_TIMEOUT_MS = 10000;      // 10 saniye TLS handshake timeout
    static constexpr int MAX_CONNECTIONS_PER_MINUTE_IP = 500; // Okul NAT ağları (20-40 PC) için 30 çok düşük. 500 güvenli bir eşik.

    struct IpRateInfo {
        int count = 0;
        qint64 windowStartSecs = 0;
    };

    QSet<QSslSocket*>              m_activeSockets;
    QMap<QSslSocket*, QTimer*>     m_handshakeTimers;
    QMap<QString, IpRateInfo>      m_ipConnectionRates;

    bool checkIpRateLimit(const QString& peerIp);
    void cleanupPendingSocket(QSslSocket* socket);
};