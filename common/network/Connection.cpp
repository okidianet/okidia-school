// common/network/Connection.cpp
#include "Connection.h"
#include "../utils/Logger.h"
#include "messages.pb.h"
#include <QDataStream>

Connection::Connection(QSslSocket* socket, QObject* parent, bool ownsSocket)
    : QObject(parent)
    , m_socket(socket)
    , m_ownsSocket(ownsSocket) {

    connect(m_socket, &QSslSocket::readyRead,
            this, &Connection::onReadyRead);
    connect(m_socket, &QSslSocket::disconnected,
            this, &Connection::onDisconnected);
    connect(m_socket, &QSslSocket::sslErrors,
            this, &Connection::onSslErrors);

    // Gecikmeyi azaltmak için Nagle algoritmasını devre dışı bırak (TCP_NODELAY)
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
}

Connection::~Connection() {
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket = nullptr;
    }
}

bool Connection::sendEnvelope(const okidia::Envelope& envelope) {
    if (!m_socket || !m_socket->isEncrypted()) {
        Logger::error(QObject::tr("Connection: Şifreli bağlantı yok, mesaj gönderilemedi"));
        return false;
    }


    // Protobuf → binary
    std::string data;
    if (!envelope.SerializeToString(&data)) {
        Logger::error(QObject::tr("Connection: Envelope serialize edilemedi"));
        return false;
    }

    // Çerçeveleme: [4 byte big-endian uzunluk][data]
    QByteArray frame;
    QDataStream stream(&frame, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(data.size());
    frame.append(QByteArray::fromStdString(data));

    qint64 written = m_socket->write(frame);
    if (written != frame.size()) {
        Logger::error(QObject::tr("Connection: Mesaj tam yazılamadı"));
        return false;
    }
    // TCP_NODELAY zaten aktif (satır 20) — ek flush() gereksiz.
    // flush() kaldırılması, aynı anda gönderilen birden fazla tile'ın
    // tek TCP segmentinde birleştirilmesine izin verir.

    return true;
}

QString Connection::peerAddress() const {
    if (!m_socket) return {};
    QString ip = m_socket->peerAddress().toString();
    if (ip.startsWith("::ffff:")) {
        ip = ip.mid(7); // "::ffff:" kısmını at
    }
    return ip;
}

bool Connection::isConnected() const {
    return m_socket &&
           m_socket->state() == QAbstractSocket::ConnectedState &&
           m_socket->isEncrypted();
}

void Connection::disconnect() {
    if (m_socket) {
        m_socket->disconnectFromHost();
    }
}

void Connection::onReadyRead() {
    if (!m_socket) return;

    // Tampon taşma koruması (Bufferbloat / DoS önleme)
    qint64 available = m_socket->bytesAvailable();
    if (static_cast<qint64>(m_buffer.size()) + available > MAX_BUFFER_SIZE) {
        Logger::error(QObject::tr("Connection: Soket tampon boyutu sınırı aşıldı (%1 MB > %2 MB) ← %3, bağlantı kesiliyor")
            .arg(double(m_buffer.size() + available) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(double(MAX_BUFFER_SIZE) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(peerAddress()));
        m_buffer.clear();
        m_socket->disconnectFromHost();
        return;
    }

    // Gelen veriyi tampona ekle
    if (available > 0) {
        m_buffer.append(m_socket->readAll());
    }

    // Qt GUI Event Loop Bloklanma Önleme (UI Unblocking):
    // Tek bir event loop turunda maksimum kMaxMessagesPerRead paket işlenir.
    // Tamponda daha fazla veri kalmışsa, sonraki event loop turuna singleShot ile devredilir.
    constexpr int kMaxMessagesPerRead = 8;
    int processed = 0;
    while (processed < kMaxMessagesPerRead && tryParseMessage()) {
        ++processed;
    }

    if (m_buffer.size() >= 4 || (m_socket && m_socket->bytesAvailable() > 0)) {
        QPointer<Connection> self(this);
        QTimer::singleShot(0, this, [self]() {
            if (self) self->onReadyRead();
        });
    }
}

bool Connection::tryParseMessage() {
    // En az 4 byte (uzunluk başlığı) geldi mi?
    if (m_buffer.size() < 4) return false;

    // Uzunluğu oku (big-endian)
    QDataStream stream(m_buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 msgLen = 0;
    stream >> msgLen;

    // Maksimum mesaj boyutu kontrolü (10 MB)
    if (msgLen > MAX_MESSAGE_SIZE) {
        Logger::error(QObject::tr("Connection: Mesaj çok büyük (%1 MB > %2 MB) ← %3, bağlantı kesiliyor")
            .arg(double(msgLen) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(double(MAX_MESSAGE_SIZE) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(peerAddress()));
        m_buffer.clear();
        if (m_socket) {
            m_socket->disconnectFromHost();
        }
        return false;
    }

    // Tüm mesaj geldi mi?
    if (m_buffer.size() < static_cast<int>(4 + msgLen)) return false;

    // Mesajı tamponden al
    QByteArray msgData = m_buffer.mid(4, msgLen);
    m_buffer.remove(0, 4 + msgLen);

    // Protobuf parse et
    okidia::Envelope envelope;
    bool parsed = envelope.ParseFromArray(msgData.constData(), msgData.size());
    // KVKK & Bellek Hijyeni: Ham bayt tamponunu bellekten anında düşür
    msgData.clear();

    if (!parsed) {
        Logger::error(QObject::tr("Connection: Envelope parse edilemedi ← ") + peerAddress());
        return false;
    }

    // Mesaj türüne göre katmanlı boyut kontrolü (Protocol anomaly guard)
    if (!validateEnvelope(envelope, msgLen)) {
        if (m_socket) {
            m_socket->disconnectFromHost();
        }
        return false;
    }

    emit envelopeReceived(envelope);
    return true;
}

bool Connection::validateEnvelope(const okidia::Envelope& envelope, quint32 rawSize) {
    // Büyük boyut taşıyabilen meşru mesaj tipleri:
    // - Ekran kareleri / karoları
    // - Dosya transfer parçaları
    // - OTA güncelleme parçaları
    // - Ekran yayını kareleri
    // - Ses yayını paketleri
    okidia::MessageType type = envelope.type();
    bool isBulkData = (type == okidia::SCREEN_FRAME ||
                       type == okidia::SCREEN_TILE_UPDATE ||
                       type == okidia::REMOTE_FRAME ||
                       type == okidia::BROADCAST_FRAME ||
                       type == okidia::FILE_TRANSFER_CHUNK ||
                       type == okidia::OTA_UPDATE_CHUNK ||
                       type == okidia::BROKER_OTA_UPDATE_CHUNK ||
                       type == okidia::AUDIO_BROADCAST_LIVE ||
                       type == okidia::AUDIO_BROADCAST_FILE);

    if (!isBulkData && rawSize > MAX_CONTROL_PAYLOAD_SIZE) {
        Logger::error(QObject::tr("Connection: Kontrol mesajı izin verilen boyutu aştı (%1 KB > %2 KB, tip: %3) ← %4, bağlantı kesiliyor")
            .arg(rawSize / 1024)
            .arg(MAX_CONTROL_PAYLOAD_SIZE / 1024)
            .arg(static_cast<int>(type))
            .arg(peerAddress()));
        return false;
    }

    return true;
}

void Connection::onDisconnected() {
    Logger::info(QObject::tr("Connection: Bağlantı kesildi ← ") + peerAddress());
    emit disconnected();
}

void Connection::onSslErrors(const QList<QSslError>& errors) {
    for (const QSslError& err : errors) {
        Logger::error(QObject::tr("Connection: SSL hatası: ") + err.errorString());
    }
    emit error("SSL hatası");
}