// common/network/LocalConnection.cpp
#include "LocalConnection.h"
#include "../utils/Logger.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDataStream>
#include <QDateTime>

// ── Framing: 4 byte big-endian length + payload (Connection ile aynı) ─────

static bool writeEnvelope(QLocalSocket* sock,
                          const okidia::Envelope& env) {

    std::string data = env.SerializeAsString();
    quint32 len = static_cast<quint32>(data.size());

    QByteArray frame(4 + static_cast<int>(len), Qt::Uninitialized);
    frame[0] = (len >> 24) & 0xFF;
    frame[1] = (len >> 16) & 0xFF;
    frame[2] = (len >>  8) & 0xFF;
    frame[3] =  len        & 0xFF;
    memcpy(frame.data() + 4, data.data(), len);

    return sock->write(frame) == frame.size();
}

// ── LocalConnection ───────────────────────────────────────────────────────

LocalConnection::LocalConnection(const QString& socketPath, QObject* parent)
    : QObject(parent)
    , m_path(socketPath)
    , m_ownsSocket(true)
{
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::readyRead,
            this, &LocalConnection::onReadyRead);
    connect(m_socket, &QLocalSocket::connected,
            this, &LocalConnection::onConnected);
    connect(m_socket, &QLocalSocket::disconnected,
            this, &LocalConnection::onDisconnected);
}

LocalConnection::LocalConnection(QLocalSocket* socket, QObject* parent)
    : QObject(parent)
    , m_socket(socket)
    , m_ownsSocket(false)
{
    m_socket->setParent(this);
    connect(m_socket, &QLocalSocket::readyRead,
            this, &LocalConnection::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected,
            this, &LocalConnection::onDisconnected);
}

LocalConnection::~LocalConnection() {
    if (m_socket && m_ownsSocket)
        m_socket->disconnectFromServer();
}

void LocalConnection::connectToServer() {
    m_socket->connectToServer(m_path);
}

void LocalConnection::disconnectFromServer() {
    if (m_socket) {
        m_socket->disconnectFromServer();
    }
}

bool LocalConnection::isConnected() const {
    return m_socket &&
           m_socket->state() == QLocalSocket::ConnectedState;
}

bool LocalConnection::sendEnvelope(const okidia::Envelope& env) {
    if (!isConnected()) return false;
    return writeEnvelope(m_socket, env);
}

void LocalConnection::onConnected() {
    Logger::info(QObject::tr("LocalConnection: Bağlandı → ") + m_path);
    emit connected();
}

void LocalConnection::onDisconnected() {
    Logger::info(QObject::tr("LocalConnection: Bağlantı kesildi"));
    emit disconnected();
}

qint64 LocalConnection::bytesToWrite() const {
    return m_socket ? m_socket->bytesToWrite() : 0;
}

void LocalConnection::setReadEnabled(bool enabled) {
    if (!m_socket) return;
    if (enabled) {
        connect(m_socket, &QLocalSocket::readyRead,
                this, &LocalConnection::onReadyRead, Qt::UniqueConnection);
        if (m_socket->bytesAvailable() > 0) {
            onReadyRead();
        }
    } else {
        disconnect(m_socket, &QLocalSocket::readyRead,
                   this, &LocalConnection::onReadyRead);
    }
}

void LocalConnection::onReadyRead() {
    m_buffer.append(m_socket->readAll());
    while (tryParseMessage()) {}
}

bool LocalConnection::tryParseMessage() {
    if (m_buffer.size() < 4) return false;

    quint32 len = ((quint8)m_buffer[0] << 24) |
                  ((quint8)m_buffer[1] << 16) |
                  ((quint8)m_buffer[2] <<  8) |
                   (quint8)m_buffer[3];

    if (m_buffer.size() < 4 + (int)len) return false;

    okidia::Envelope env;
    if (env.ParseFromArray(m_buffer.constData() + 4, static_cast<int>(len)))
        emit envelopeReceived(env);

    m_buffer.remove(0, 4 + static_cast<int>(len));
    return true;
}

// ── LocalServer ───────────────────────────────────────────────────────────

LocalServer::LocalServer(const QString& socketPath, QObject* parent)
    : QObject(parent)
    , m_server(new QLocalServer(this))
    , m_path(socketPath)
{
    connect(m_server, &QLocalServer::newConnection,
            this, &LocalServer::onNewConnection);
}

LocalServer::~LocalServer() {
    m_server->close();
    QFile::remove(m_path);
}

bool LocalServer::listen() {
    QFile::remove(m_path);
    QDir().mkpath(QFileInfo(m_path).absolutePath());

    m_server->setSocketOptions(QLocalServer::WorldAccessOption);

    if (!m_server->listen(m_path)) {
        Logger::error(QObject::tr("LocalServer: Dinlenemedi → ") + m_path
                      + " : " + m_server->errorString());
        return false;
    }

    // Herkes okuyabilsin — tray root olmadan bağlanabilir
    QFile::setPermissions(m_path,
        QFile::ReadOwner | QFile::WriteOwner |
        QFile::ReadGroup | QFile::WriteGroup |
        QFile::ReadOther | QFile::WriteOther);

    Logger::info(QObject::tr("LocalServer: Dinliyor → ") + m_path);
    return true;
}

void LocalServer::broadcast(const okidia::Envelope& envelope) {
    for (auto* c : m_clients)
        c->sendEnvelope(envelope);
}

void LocalServer::sendToActive(const okidia::Envelope& envelope)
{
    if (auto* active = activeConnection()) {
        active->sendEnvelope(envelope);
    } else {
        // Aktif bağlantı henüz eşleşmedi (geçiş durumu) → tümüne gönder
        if (!m_clients.isEmpty()) {
            static qint64 s_lastLogTime = 0;
            qint64 now = QDateTime::currentSecsSinceEpoch();
            if (now - s_lastLogTime >= 5) {
                s_lastLogTime = now;
                Logger::info(QObject::tr("LocalServer: Aktif kullanıcı henüz belirlenmedi, tum tray'lere broadcast"));
            }
            broadcast(envelope);
        }
    }
}

// ── Kullanıcı – bağlantı eşlemesi ────────────────────────────────────────

void LocalServer::setUserForConnection(LocalConnection* conn,
                                       const QString& username)
{
    const QString prev = m_userMap.value(conn);
    m_userMap[conn] = username;
    if (prev != username) {
        Logger::info(QObject::tr("LocalServer: Bağlantı → kullanıcı eşlendi: ") + username);
    }
}

QString LocalServer::userForConnection(LocalConnection* conn) const
{
    return m_userMap.value(conn);
}

bool LocalServer::hasUserConnection(const QString& username) const
{
    for (auto it = m_userMap.constBegin(); it != m_userMap.constEnd(); ++it) {
        if (it.value().compare(username, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

void LocalServer::setActiveUser(const QString& username)
{
    if (m_activeUser != username) {
        Logger::info(QObject::tr("LocalServer: Aktif kullanıcı güncellendi → '") + username + "'");
        m_activeUser = username;
    }
}

LocalConnection* LocalServer::activeConnection() const
{
    if (m_clients.isEmpty()) return nullptr;
    if (m_clients.size() == 1) return m_clients.first();

    if (!m_activeUser.isEmpty()) {
        for (auto* c : m_clients) {
            if (m_userMap.value(c).compare(m_activeUser, Qt::CaseInsensitive) == 0)
                return c;
        }
    }
    return m_clients.last();
}

// ── Yeni bağlantı ─────────────────────────────────────────────────────────

void LocalServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket* sock = m_server->nextPendingConnection();
        auto* conn = new LocalConnection(sock, this);

        connect(conn, &LocalConnection::envelopeReceived,
                [this, conn](const okidia::Envelope& env) {
            emit envelopeReceived(conn, env);
        });

        connect(conn, &LocalConnection::disconnected,
                [this, conn]() {
            const QString user = m_userMap.value(conn);
            m_clients.removeOne(conn);
            m_userMap.remove(conn);          // kullanıcı eşlemesini temizle
            emit clientDisconnected(conn);
            conn->deleteLater();
            Logger::info(QObject::tr("LocalServer: Tray bağlantısı kesildi")
                         + (user.isEmpty() ? QString() : " [" + user + "]")
                         + ", " + QString::number(m_clients.size()) + QObject::tr(" istemci kaldı"));
        });

        m_clients.append(conn);
        Logger::info(QObject::tr("LocalServer: Yeni tray bağlandı, ")
                     + QString::number(m_clients.size()) + QObject::tr(" istemci"));
        emit clientConnected(conn);
    }
}
