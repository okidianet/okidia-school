// common/network/TlsClient.h
#pragma once

#include <QObject>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QTimer>
#include "Connection.h"

#if defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

class TlsClient : public QObject {
    Q_OBJECT
public:
    explicit TlsClient(QObject* parent = nullptr);
    ~TlsClient();

    bool connectToServer(const QString& host, quint16 port,
                         const QString& certPath,
                         const QString& keyPath,
                         const QString& caCertPath);

    // Sertifikasız bağlantı — discovery modu
    bool connectToServerAnonymous(const QString& host, quint16 port);

    // Sertifika alındıktan sonra yeniden bağlan — event loop'tan çağrılır
    void reconnectWithCert(const QString& certPath,
                            const QString& keyPath,
                            const QString& caCertPath);

    // İptal/revoke sonrası — sertifikaları sil, anonymous moda geç, yeniden bağlan
    // disconnectFromServer()'dan farklı: autoReconnect KAPATILMAZ
    void reconnectAnonymous();

    void disconnectFromServer();

    // autoReconnect açık kalarak mevcut soketi zorla kapatır ve yeniden bağlanır.
    // Ağ arayüzü değişiminde kullanılır; disconnectFromServer()'dan farklı olarak
    // autoReconnect bayrağını kapatmaz.
    void forceReconnect();

    bool isConnected() const;
    bool isConnecting() const { return m_socket && (m_socket->state() == QAbstractSocket::ConnectingState || m_socket->state() == QAbstractSocket::HostLookupState); }
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    Connection* connection() const { return m_connection; }
    void setAutoReconnect(bool enabled, int intervalMs = 5000);
    bool isAnonymous() const { return m_anonymous; }

signals:
    void connected(Connection* connection);
    void disconnected();
    void error(const QString& message);

private slots:
    void onEncrypted();
    void onDisconnected();
    void onSslErrors(const QList<QSslError>& errors);
    void onReconnectTimer();
    void onHandshakeTimeout();

private:
    void setupSocket();
    void doConnect();       // host/port/certler ayarlandıktan sonra çağrılır
    void applyKeepalive();  // TCP SO_KEEPALIVE + KEEPIDLE/INTVL/CNT uygular

    QSslSocket*       m_socket            = nullptr;
    Connection*       m_connection        = nullptr;
    QSslConfiguration m_sslConfig;
    QTimer*           m_reconnectTimer    = nullptr;
    QTimer*           m_handshakeTimer    = nullptr;  // TLS handshake timeout

    QString  m_host;
    quint16  m_port         = 0;
    QString  m_certPath;
    QString  m_keyPath;
    QString  m_caCertPath;
    bool     m_autoReconnect = false;
    bool     m_anonymous     = false;
};
