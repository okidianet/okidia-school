// common/network/Connection.h
#pragma once

#include <QObject>
#include <QSslSocket>
#include <QByteArray>
#include <QTimer>
#include <QPointer>

// Protobuf - CMake tarafından build dizinine üretilir
#ifdef KEY_EVENT
#  undef KEY_EVENT
#endif
#ifdef MOUSE_EVENT
#  undef MOUSE_EVENT
#endif
#include "messages.pb.h"

class Connection : public QObject {
    Q_OBJECT

public:
    explicit Connection(QSslSocket* socket,
                    QObject* parent = nullptr,
                    bool ownsSocket = true);
    ~Connection();

    bool sendEnvelope(const okidia::Envelope& envelope);

    qint64 bytesToWrite() const { return m_socket ? m_socket->bytesToWrite() : 0; }

    QString peerAddress() const;
    bool isConnected() const;
    void disconnect();
    QSslSocket* sslSocket() const { return m_socket; }

    QString connectionId() const { return m_connectionId; }
    void setConnectionId(const QString& id) { m_connectionId = id; }

    static constexpr quint32 MAX_MESSAGE_SIZE = 64 * 1024 * 1024;         // 64 MB azami tekil paket boyutu (DB senkronizasyonu ve medya)
    static constexpr qint64  MAX_BUFFER_SIZE  = 128 * 1024 * 1024;        // 128 MB soket tampon birikme üst sınırı
    static constexpr quint32 MAX_CONTROL_PAYLOAD_SIZE = 2 * 1024 * 1024; // 2 MB kontrol/komut payload sınırı

signals:
    void envelopeReceived(const okidia::Envelope& envelope);
    void disconnected();
    void error(const QString& message);

private slots:
    void onReadyRead();
    void onDisconnected();
    void onSslErrors(const QList<QSslError>& errors);

private:
    QPointer<QSslSocket> m_socket;
    QByteArray  m_buffer;
    QString     m_connectionId;
    bool        m_ownsSocket;

    bool tryParseMessage();
    bool validateEnvelope(const okidia::Envelope& envelope, quint32 rawSize);
};