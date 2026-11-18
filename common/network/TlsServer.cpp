// common/network/TlsServer.cpp
#include "TlsServer.h"
#include "../utils/Logger.h"
#include "../utils/OkidiaUtils.h"
#include "../crypto/KeyManager.h"
#include "messages.pb.h"

#include <QSslCertificate>
#include <QSslKey>
#include <QFile>
#include <QTimer>

TlsServer::TlsServer(QObject* parent)
    : QTcpServer(parent) {}

TlsServer::~TlsServer() {
    stopListening();
    for (QTimer* timer : m_handshakeTimers) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }
    m_handshakeTimers.clear();
    m_activeSockets.clear();
}


bool TlsServer::startListening(quint16 port,
                                const QString& certPath,
                                const QString& keyPath,
                                const QString& caCertPath) {
    m_caCertPath = caCertPath;

    // Sertifikayı yükle
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("TlsServer: Sertifika açılamadı: ") + certPath);
        return false;
    }
    QSslCertificate cert(&certFile, QSsl::Pem);
    certFile.close();

    if (cert.isNull()) {
        Logger::error(QObject::tr("TlsServer: Geçersiz sertifika: ") + certPath);
        return false;
    }

    // Private key'i yükle — önce EC (ECDSA), sonra RSA dene
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("TlsServer: Private key açılamadı: ") + keyPath);
        return false;
    }
    QByteArray keyData = keyFile.readAll();
    keyFile.close();

    // EC (ECDSA P-256) dene
    QSslKey key(keyData, QSsl::Ec, QSsl::Pem);

    // EC başarısız → RSA dene
    if (key.isNull())
        key = QSslKey(keyData, QSsl::Rsa, QSsl::Pem);

    // RSA da başarısız → Opaque key (Qt 5.15+ OpenSSL backend)
    if (key.isNull())
        key = QSslKey(keyData, QSsl::Opaque, QSsl::Pem);

    if (key.isNull()) {
        Logger::error(QObject::tr("TlsServer: Geçersiz private key: ") + keyPath);
        return false;
    }

    // CA sertifikasını yükle (mutual TLS için)
    QFile caFile(caCertPath);
    if (!caFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("TlsServer: CA sertifikası açılamadı: ") + caCertPath);
        return false;
    }
    QSslCertificate caCert(&caFile, QSsl::Pem);
    caFile.close();

    if (caCert.isNull()) {
        Logger::error(QObject::tr("TlsServer: Geçersiz CA sertifikası: ") + caCertPath);
        return false;
    }

    // SSL konfigürasyonu
    m_sslConfig = QSslConfiguration::defaultConfiguration();
    m_sslConfig.setProtocol(QSsl::TlsV1_3);
    m_sslConfig.setLocalCertificate(cert);
    m_sslConfig.setPrivateKey(key);

    // CA'yı sistem listesine ekle
    QList<QSslCertificate> caCerts = m_sslConfig.caCertificates();
    caCerts.append(caCert);
    m_sslConfig.setCaCertificates(caCerts);

    // QueryPeer: sertifikalı bağlantıları doğrula, sertifikasızlara izin ver
    // Sertifikasız agent'lar DISCOVERY_REQUEST gönderebilir
    m_sslConfig.setPeerVerifyMode(QSslSocket::QueryPeer);

    // Sunucuyu başlat
    if (!listen(QHostAddress::Any, port)) {
        Logger::error(QObject::tr("TlsServer: Port dinlenemedi: ") +
                      QString::number(port) + " → " + errorString());
        return false;
    }

    Logger::info(QObject::tr("TlsServer: Dinleniyor → port ") + QString::number(port));
    return true;
}

void TlsServer::stopListening() {
    close();

    for (QTimer* timer : m_handshakeTimers) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }
    m_handshakeTimers.clear();

    for (QSslSocket* sock : m_activeSockets) {
        if (sock) {
            sock->disconnect(this);
            sock->abort();
            sock->deleteLater();
        }
    }
    m_activeSockets.clear();

    Logger::info(QObject::tr("TlsServer: Durduruldu"));
}

bool TlsServer::checkIpRateLimit(const QString& peerIp) {
    if (peerIp.isEmpty()) return true;

    const qint64 now = OkidiaUtils::nowSecs();
    auto& info = m_ipConnectionRates[peerIp];

    // 1 dakikalık pencere dolduysa sıfırla
    if (now - info.windowStartSecs >= 60) {
        info.windowStartSecs = now;
        info.count = 1;
        return true;
    }

    info.count++;
    if (info.count > MAX_CONNECTIONS_PER_MINUTE_IP) {
        Logger::warning(QObject::tr("TlsServer: GÜVENLİK UYARISI! %1 IP adresinden dakikada %2 bağlantı açıldı! DoS engeli devrede.")
                        .arg(peerIp).arg(info.count));
        return false;
    }
    return true;
}

void TlsServer::cleanupPendingSocket(QSslSocket* socket) {
    if (!socket) return;
    if (m_handshakeTimers.contains(socket)) {
        QTimer* t = m_handshakeTimers.take(socket);
        if (t) {
            t->stop();
            t->deleteLater();
        }
    }
    m_activeSockets.remove(socket);
}

void TlsServer::onSocketDisconnected() {
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    if (socket) {
        cleanupPendingSocket(socket);
    }
}

void TlsServer::incomingConnection(qintptr socketDescriptor) {
    // 1. Eşzamanlı maksimum bağlantı sınır kontrolü (Max connection exhaustion guard)
    if (m_activeSockets.size() >= MAX_CONCURRENT_CONNECTIONS) {
        Logger::warning(QObject::tr("TlsServer: Maksimum eşzamanlı bağlantı sınırına ulaşıldı (%1/%2)! Yeni bağlantı reddediliyor.")
                        .arg(m_activeSockets.size()).arg(MAX_CONCURRENT_CONNECTIONS));
        // Soketi geçici açıp hemen abort et
        QSslSocket tempSocket;
        if (tempSocket.setSocketDescriptor(socketDescriptor)) {
            tempSocket.abort();
        }
        return;
    }

    QSslSocket* socket = new QSslSocket(this);

    if (!socket->setSocketDescriptor(socketDescriptor)) {
        Logger::error(QObject::tr("TlsServer: Socket descriptor atanamadı"));
        delete socket;
        return;
    }

    // IP temizleme (::ffff: IPv4-mapped IPv6 ön ekini kaldır)
    QString peerIp = socket->peerAddress().toString();
    if (peerIp.startsWith("::ffff:")) {
        peerIp = peerIp.mid(7);
    }

    // 2. IP bazlı bağlantı hızı kontrolü (Connection flood / SYN churn guard)
    if (!checkIpRateLimit(peerIp)) {
        socket->abort();
        socket->deleteLater();
        return;
    }

    // Aktif soket listesine ekle
    m_activeSockets.insert(socket);

    // SSL konfigürasyonunu uygula
    socket->setSslConfiguration(m_sslConfig);

    // Kendi CA'mızla imzalı sertifikalar ve anonim keşif (sertifikasız) için hataları önceden ignore et.
    // Güvenlik sertleştirmesi: CertificateExpired ve CertificateNotYetValid yoksayılmaz!
    socket->ignoreSslErrors({
        QSslError(QSslError::SelfSignedCertificate),
        QSslError(QSslError::SelfSignedCertificateInChain),
        QSslError(QSslError::CertificateUntrusted),
        QSslError(QSslError::HostNameMismatch),
        QSslError(QSslError::NoPeerCertificate)
    });

    // 3. Sunucu tarafı TLS Handshake Timeout Bekçisi (Slowloris / Handshake stall guard)
    QTimer* handshakeTimer = new QTimer(this);
    handshakeTimer->setSingleShot(true);
    handshakeTimer->setInterval(HANDSHAKE_TIMEOUT_MS);
    m_handshakeTimers[socket] = handshakeTimer;

    connect(handshakeTimer, &QTimer::timeout, this, [this, socket, peerIp]() {
        if (socket && m_activeSockets.contains(socket)) {
            Logger::warning(QObject::tr("TlsServer: TLS el sıkışması zaman aşımına uğradı (10sn) ← ") + peerIp + QObject::tr(" Soket kapatılıyor."));
            cleanupPendingSocket(socket);
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
        }
    });
    handshakeTimer->start();

    // Sinyalleri bağla
    connect(socket, &QSslSocket::encrypted,
            this, &TlsServer::onSslHandshakeComplete);
    connect(socket, &QSslSocket::sslErrors,
            this, &TlsServer::onSslErrors);
    connect(socket, &QSslSocket::disconnected,
            this, &TlsServer::onSocketDisconnected);

    // TLS el sıkışmasını başlat (sunucu modu)
    socket->startServerEncryption();

    Logger::info(QObject::tr("TlsServer: Yeni bağlantı kabul edildi ← ") + peerIp
                 + " (" + QString::number(m_activeSockets.size()) + "/" + QString::number(MAX_CONCURRENT_CONNECTIONS) + ")");
}

void TlsServer::onSslHandshakeComplete() {
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    if (!socket) return;

    // Handshake tamamlandı, handshake timer'ını kaldır
    if (m_handshakeTimers.contains(socket)) {
        QTimer* t = m_handshakeTimers.take(socket);
        if (t) {
            t->stop();
            t->deleteLater();
        }
    }

    Logger::info(QObject::tr("TlsServer: TLS el sıkışması tamamlandı ← ") +
                 socket->peerAddress().toString());

    // İstemci sertifikasını kontrol et
    QSslCertificate peerCert = socket->peerCertificate();
    if (peerCert.isNull()) {
        // Sertifikasız bağlantı → discovery modu, kabul et
        Logger::info(QObject::tr("TlsServer: Sertifikasiz baglanti (discovery modu) ← ") +
                     socket->peerAddress().toString());
    } else {
        Logger::info(QObject::tr("TlsServer: Istemci sertifikasi → ") +
                     peerCert.subjectDisplayName());

        // Mutual TLS CA Doğrulaması: Sertifikalı bağlanan istemcinin bu Broker'ın CA'sı tarafından imzalandığını doğrula
        if (!m_caCertPath.isEmpty() && !KeyManager::verifyCertificateChain(peerCert.toPem(), m_caCertPath)) {
            Logger::warning(QObject::tr("TlsServer: GÜVENLİK UYARISI! İstemci sertifikası bu Broker'ın CA'sı tarafından onaylanmamış! Yabancı istemci bağlantısı reddediliyor ← ") +
                            socket->peerAddress().toString());
            cleanupPendingSocket(socket);
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
            return;
        }
    }

    // Connection nesnesi oluştur ve sinyal gönder
    Connection* conn = new Connection(socket, this, true);
    emit newConnection(conn);
}

void TlsServer::onSslErrors(const QList<QSslError>& errors) {
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    QList<QSslError> ignored;
    bool hasFatal = false;

    for (const QSslError& err : errors) {
        switch (err.error()) {
        case QSslError::SelfSignedCertificate:
        case QSslError::SelfSignedCertificateInChain:
        case QSslError::CertificateUntrusted:
        case QSslError::HostNameMismatch:
        case QSslError::NoPeerCertificate:
            ignored << err;
            break;
        case QSslError::CertificateExpired:
            // OTA/restart sonrası kısa süreli saat sapması toleransı (≤300 sn)
            if (!err.certificate().isNull()) {
                qint64 secsOverdue = err.certificate().expiryDate().secsTo(QDateTime::currentDateTimeUtc());
                if (secsOverdue >= 0 && secsOverdue <= 300) {
                    Logger::warning(QObject::tr("TlsServer: İstemci sertifikası yakın zamanda dolmuş (%1 sn), OTA drift toleransı (≤300 sn) ← %2")
                                    .arg(secsOverdue)
                                    .arg(socket ? socket->peerAddress().toString() : QString()));
                    ignored << err;
                    break;
                }
            }
            hasFatal = true;
            Logger::error(QObject::tr("TlsServer: İstemci sertifikasının geçerlilik süresi dolmuş (CertificateExpired) ← ") +
                (socket ? socket->peerAddress().toString() : QString()));
            break;
        case QSslError::CertificateNotYetValid:
            // OTA/restart sonrası kısa süreli saat sapması toleransı (≤300 sn)
            if (!err.certificate().isNull()) {
                qint64 secsUntilValid = QDateTime::currentDateTimeUtc().secsTo(err.certificate().effectiveDate());
                if (secsUntilValid >= 0 && secsUntilValid <= 300) {
                    Logger::warning(QObject::tr("TlsServer: İstemci sertifikası %1 sn sonra geçerli olacak, OTA drift toleransı (≤300 sn) ← %2")
                                    .arg(secsUntilValid)
                                    .arg(socket ? socket->peerAddress().toString() : QString()));
                    ignored << err;
                    break;
                }
            }
            hasFatal = true;
            Logger::error(QObject::tr("TlsServer: İstemci sertifikası henüz geçerli değil (CertificateNotYetValid) ← ") +
                (socket ? socket->peerAddress().toString() : QString()));
            break;
        default:
            hasFatal = true;
            Logger::error(QObject::tr("TlsServer: SSL hatası") +
                (socket ? " ← " + socket->peerAddress().toString() : "") +
                ": " + err.errorString());
            break;
        }
    }

    if (!ignored.isEmpty() && socket)
        socket->ignoreSslErrors(ignored);

    if (hasFatal && socket) {
        cleanupPendingSocket(socket);
        socket->disconnectFromHost();
    }
}