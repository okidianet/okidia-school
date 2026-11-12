// common/network/LocalConnection.h
#pragma once

#include <QObject>
#include <QMap>
#include <QLocalSocket>
#include <QLocalServer>
#include <QByteArray>
#include "messages.pb.h"

/**
 * Unix socket üzerinden proto Envelope iletişimi.
 * Connection sınıfıyla aynı length-prefix framing kullanır.
 *
 * Servis: LocalServer açar → her bağlanan tray için LocalConnection
 * Tray:   LocalConnection ile servise bağlanır
 */
class LocalConnection : public QObject {
    Q_OBJECT

public:
    // Tray tarafı — servise bağlan
    explicit LocalConnection(const QString& socketPath,
                             QObject* parent = nullptr);

    // Servis tarafı — kabul edilen socket
    explicit LocalConnection(QLocalSocket* socket,
                             QObject* parent = nullptr);

    ~LocalConnection();

    bool sendEnvelope(const okidia::Envelope& envelope);
    bool isConnected() const;
    void connectToServer();
    void disconnectFromServer();
    qint64 bytesToWrite() const;
    void setReadEnabled(bool enabled);

signals:
    void envelopeReceived(const okidia::Envelope& envelope);
    void connected();
    void disconnected();

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();

private:
    bool tryParseMessage();

    QLocalSocket* m_socket    = nullptr;
    QString       m_path;
    QByteArray    m_buffer;
    bool          m_ownsSocket;
};

// ─────────────────────────────────────────────────────────────
/**
 * Servis tarafında Unix socket server.
 *
 * Her bağlanan tray bir kullanıcıya eşlenebilir (setUserForConnection).
 * Aktif kullanıcı setActiveUser ile belirlenir. sendToActive yalnızca
 * aktif kullanıcının bağlantısına gönderir; aktif bağlantı yoksa tüm
 * bağlantılara broadcast eder (güvenli fallback).
 */
class LocalServer : public QObject {
    Q_OBJECT

public:
    explicit LocalServer(const QString& socketPath,
                         QObject* parent = nullptr);
    ~LocalServer();

    bool listen();

    // Tüm bağlı tray'lere gönder
    void broadcast(const okidia::Envelope& envelope);

    // Sadece aktif kullanıcının tray'ine gönder
    // Aktif bağlantı bulunamazsa broadcast'e düşer
    void sendToActive(const okidia::Envelope& envelope);

    int  clientCount() const { return m_clients.size(); }

    // ── Kullanıcı – bağlantı eşlemesi ────────────────────────────────────
    // Tray CLIENT_INFO gönderince çağrılır; hangi bağlantının hangi
    // kullanıcıya ait olduğunu kaydeder.
    void    setUserForConnection(LocalConnection* conn,
                                 const QString& username);
    QString userForConnection(LocalConnection* conn) const;
    bool    hasUserConnection(const QString& username) const;

    // ── Aktif kullanıcı yönetimi ──────────────────────────────────────────
    // sessionTimer aktif display kullanıcısını tespit edince çağrılır.
    void             setActiveUser(const QString& username);
    QString          activeUser()       const { return m_activeUser; }

    // Aktif kullanıcının LocalConnection'ı (eşleşme yoksa nullptr)
    LocalConnection* activeConnection() const;

signals:
    void clientConnected(LocalConnection* conn);
    void clientDisconnected(LocalConnection* conn);
    void envelopeReceived(LocalConnection* sender,
                          const okidia::Envelope& envelope);

private slots:
    void onNewConnection();

private:
    QLocalServer*                   m_server;
    QString                         m_path;
    QList<LocalConnection*>         m_clients;
    QMap<LocalConnection*, QString> m_userMap;   // conn → username
    QString                         m_activeUser; // şu an aktif olan kullanıcı
};
