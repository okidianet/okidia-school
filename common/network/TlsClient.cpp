// common/network/TlsClient.cpp
#include "TlsClient.h"
#include "../utils/Logger.h"
#include "../crypto/KeyManager.h"

#include <QSslCertificate>
#include <QSslKey>
#include <QFile>
#include <QTimer>

TlsClient::TlsClient(QObject* parent) : QObject(parent) {
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &TlsClient::onReconnectTimer);

    m_handshakeTimer = new QTimer(this);
    m_handshakeTimer->setSingleShot(true);
    m_handshakeTimer->setInterval(10000);  // 10sn handshake timeout
    connect(m_handshakeTimer, &QTimer::timeout,
            this, &TlsClient::onHandshakeTimeout);
}

TlsClient::~TlsClient() {
    m_autoReconnect = false;
    m_reconnectTimer->stop();
    if (m_connection) { m_connection->deleteLater(); m_connection = nullptr; }
    if (m_socket) {
        m_socket->disconnect();
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

// ── Ortak: socket oluştur ─────────────────────────────────────────────────────
void TlsClient::setupSocket() {
    m_handshakeTimer->stop();  // Önceki handshake timer'ı iptal et
    if (m_socket) {
        m_socket->disconnect();  // sinyalleri ayır
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }
    m_socket = new QSslSocket(this);
    connect(m_socket, &QSslSocket::encrypted,
            this, &TlsClient::onEncrypted);
    connect(m_socket, &QSslSocket::disconnected,
            this, &TlsClient::onDisconnected);
    connect(m_socket,
            QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
            this, &TlsClient::onSslErrors);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, [this](QAbstractSocket::SocketError err) {
        if (m_socket) {
            Logger::warning(QObject::tr("TlsClient: Soket hatası (kod=%1): %2").arg(err).arg(m_socket->errorString()));
        }
    });

    // TCP Keepalive: Soket bağlanma state'ine geçince keepalive uygula.
    // connectToHostEncrypted çağrılmadan önce descriptor oluşmuyor,
    // bu yüzden stateChanged sinyali ile bekliyoruz.
    connect(m_socket, &QAbstractSocket::stateChanged,
            this, [this](QAbstractSocket::SocketState state) {
        if (state == QAbstractSocket::ConnectingState ||
            state == QAbstractSocket::ConnectedState) {
            applyKeepalive();
        }
    });
}

// ── Sertifikalı bağlantı ──────────────────────────────────────────────────────
bool TlsClient::connectToServer(const QString& host, quint16 port,
                                 const QString& certPath,
                                 const QString& keyPath,
                                 const QString& caCertPath) {
    m_host      = host;
    m_port      = port;
    m_certPath  = certPath;
    m_keyPath   = keyPath;
    m_caCertPath= caCertPath;
    m_anonymous = false;

    // Sertifika yükle
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("TlsClient: Sertifika acilamadi: ") + certPath);
        return false;
    }
    QSslCertificate cert(&certFile, QSsl::Pem);
    certFile.close();
    if (cert.isNull()) {
        Logger::error(QObject::tr("TlsClient: Gecersiz sertifika: ") + certPath);
        return false;
    }

    // Key yükle
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("TlsClient: Private key acilamadi: ") + keyPath);
        return false;
    }
    QByteArray keyData = keyFile.readAll();
    keyFile.close();
    QSslKey key(keyData, QSsl::Ec, QSsl::Pem);
    if (key.isNull()) key = QSslKey(keyData, QSsl::Rsa, QSsl::Pem);
    if (key.isNull()) key = QSslKey(keyData, QSsl::Opaque, QSsl::Pem);
    if (key.isNull()) {
        Logger::error(QObject::tr("TlsClient: Gecersiz private key: ") + keyPath);
        return false;
    }

    // CA yükle
    QFile caFile(caCertPath);
    if (!caFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("TlsClient: CA acilamadi: ") + caCertPath);
        return false;
    }
    QSslCertificate caCert(&caFile, QSsl::Pem);
    caFile.close();
    if (caCert.isNull()) {
        Logger::error(QObject::tr("TlsClient: Gecersiz CA: ") + caCertPath);
        return false;
    }

    m_sslConfig = QSslConfiguration::defaultConfiguration();
    m_sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    m_sslConfig.setLocalCertificate(cert);
    m_sslConfig.setPrivateKey(key);
    QList<QSslCertificate> caCerts = m_sslConfig.caCertificates();
    caCerts.append(caCert);
    m_sslConfig.setCaCertificates(caCerts);
    m_sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);

    setupSocket();
    m_socket->setSslConfiguration(m_sslConfig);
    // Güvenlik sertleştirmesi: CertificateExpired ve CertificateNotYetValid yoksayılmaz!
    m_socket->ignoreSslErrors({
        QSslError(QSslError::SelfSignedCertificate),
        QSslError(QSslError::SelfSignedCertificateInChain),
        QSslError(QSslError::CertificateUntrusted),
        QSslError(QSslError::HostNameMismatch)
    });

    Logger::info(QObject::tr("TlsClient: Baglanıyor → ") + host + ":" + QString::number(port));
    m_socket->connectToHostEncrypted(host, port);
    m_handshakeTimer->start();  // TLS handshake timeout başlat
    return true;
}

// ── Sertifikasız (discovery) bağlantı ────────────────────────────────────────
bool TlsClient::connectToServerAnonymous(const QString& host, quint16 port) {
    m_host      = host;
    m_port      = port;
    m_certPath  = QString();
    m_keyPath   = QString();
    m_caCertPath= QString();
    m_anonymous = true;

    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    cfg.setProtocol(QSsl::SecureProtocols);

    setupSocket();
    m_socket->setSslConfiguration(cfg);
    m_socket->ignoreSslErrors(); // Anonymous keşifte sertifika uyarılarını baştan yoksay

    Logger::info(QObject::tr("TlsClient: Anonymous baglanıyor → ") + host + ":" + QString::number(port));
    m_socket->connectToHostEncrypted(host, port);
    m_handshakeTimer->start();  // TLS handshake timeout başlat
    return true;
}

// ── Sertifika alındıktan sonra yeniden bağlan ─────────────────────────────────
// ÖNEMLI: Bu metot event loop'tan (QTimer::singleShot) çağrılmalı,
//         asla bir sinyal lambda'sının içinden direkt çağrılmamalı.
void TlsClient::reconnectWithCert(const QString& certPath,
                                   const QString& keyPath,
                                   const QString& caCertPath) {
    m_certPath   = certPath;
    m_keyPath    = keyPath;
    m_caCertPath = caCertPath;
    m_anonymous  = false;

    if (m_host.isEmpty() || m_port == 0) {
        return;
    }

    Logger::info(QObject::tr("TlsClient: Sertifikalı bağlantıya geciliyor"));

    // autoReconnect'i geçici kapat — socket abort'u onDisconnected'ı tetikler
    // onDisconnected içinde timer başlamasın
    const bool savedAuto = m_autoReconnect;
    m_autoReconnect = false;
    m_reconnectTimer->stop();

    // Eski soketi yok et
    if (m_socket) {
        m_socket->disconnect();  // tüm Qt signal bağlantılarını kes
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }

    // autoReconnect'i geri al
    m_autoReconnect = savedAuto;

    // Sertifikalı bağlantı başlat
    connectToServer(m_host, m_port, certPath, keyPath, caCertPath);
}

// ── Revoke sonrası yeniden bağlan (anonymous) ────────────────────────────────
// disconnectFromServer'dan farkı: m_autoReconnect değiştirilmez.
// Agent sertifikasız kalınca bu metot çağrılır:
//   sertifikaları sil → bu metodu çağır → anonymous bağlantı → DISCOVERY_REQUEST
void TlsClient::reconnectAnonymous() {
    Logger::info(QObject::tr("TlsClient: Anonymous moda geciliyor (revoke/cert yok)"));
    m_anonymous  = true;
    m_certPath   = QString();
    m_keyPath    = QString();
    m_caCertPath = QString();

    // Mevcut bağlantıyı kapat — ama autoReconnect'e dokunma
    const bool savedAuto = m_autoReconnect;
    m_autoReconnect = false;   // onDisconnected'ın timer'ı başlatmasını engelle
    m_reconnectTimer->stop();

    if (m_socket) {
        m_socket->disconnect();
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }

    m_autoReconnect = savedAuto;

    // Hemen anonymous bağlantı dene
    if (!m_host.isEmpty())
        connectToServerAnonymous(m_host, m_port);
}

void TlsClient::disconnectFromServer() {
    m_autoReconnect = false;
    m_reconnectTimer->stop();
    if (m_connection) { m_connection->deleteLater(); m_connection = nullptr; }
    if (m_socket) { m_socket->disconnect(); m_socket->abort(); }
}

bool TlsClient::isConnected() const {
    return m_connection && m_connection->isConnected();
}

void TlsClient::setAutoReconnect(bool enabled, int intervalMs) {
    m_autoReconnect = enabled;
    m_reconnectTimer->setInterval(intervalMs);
}

// ── TCP Keepalive ────────────────────────────────────────────────────────────
void TlsClient::applyKeepalive() {
#if defined(Q_OS_LINUX)
    if (!m_socket) return;
    int fd = static_cast<int>(m_socket->socketDescriptor());
    if (fd < 0) return;

    int on    = 1;
    int idle  = 15;  // 15 sn sonra probe başlat
    int intvl = 5;   // Probler arası 5 sn
    int cnt   = 3;   // 3 başarısız probe → bağlantı kapatılır (~30 sn toplam)

    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &on,    sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    Logger::info(QObject::tr("TlsClient: TCP Keepalive etkinleştirildi (idle=") +
                 QString::number(idle) + QObject::tr("s, intvl=") +
                 QString::number(intvl) + QObject::tr("s, cnt=") +
                 QString::number(cnt) + ")");
#endif
}

// ── Zorla yeniden bağlanma ───────────────────────────────────────────────────
// disconnectFromServer()'dan farkı: autoReconnect bayrağına dokunmaz.
// Ağ arayüzü değişiminde NetworkWatcher tarafından çağrılır.
void TlsClient::forceReconnect() {
    Logger::info(QObject::tr("TlsClient: Zorla yeniden bağlanma tetiklendi (ağ değişimi)"));
    m_reconnectTimer->stop();

    if (m_socket) {
        m_socket->disconnect();  // Qt sinyallerini ayır
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }

    // disconnected sinyalini manuel emit et (tray'e bildir)
    emit disconnected();

    // autoReconnect açıksa hemen timer başlat
    if (m_autoReconnect) {
        Logger::info(QObject::tr("TlsClient: Yeniden bağlanma zamanlayıcısı başlatılıyor"));
        m_reconnectTimer->start();
    }
}

void TlsClient::onEncrypted() {
    m_handshakeTimer->stop();  // Handshake başarılı → timer'ı durdur
    Logger::info(QObject::tr("TlsClient: TLS bağlantısı kuruldu → ") + m_host);
    QSslCertificate peerCert = m_socket->peerCertificate();
    if (!peerCert.isNull())
        Logger::info(QObject::tr("TlsClient: Sunucu sertifikası → ") + peerCert.subjectDisplayName());

    // Strict CA Pinning: Sertifikalı moddaysak, bağlandığımız sunucu (Broker) sertifikasının
    // bu ajanın kayıtlı olduğu Kök CA (ca.crt) tarafından imzalandığını doğrula.
    if (!m_anonymous && !m_caCertPath.isEmpty()) {
        if (!KeyManager::verifyCertificateChain(peerCert.toPem(), m_caCertPath)) {
            Logger::error(QObject::tr("TlsClient: GÜVENLİK İHLALİ! Sunucu sertifikası bu ajanın kayıtlı olduğu CA tarafından imzalanmamış! Yabancı/Sahte Broker reddedildi: ") + m_host);
            m_reconnectTimer->stop();
            m_host = ""; // Yabancı Broker IP'sine otomatik reconnect döngüsüne girmesin
            if (m_socket) {
                m_socket->disconnect();
                m_socket->abort();
            }
            emit error(tr("Güvenilmeyen yabancı Broker reddedildi"));
            emit disconnected();
            return;
        }
    }

    if (m_connection) { m_connection->deleteLater(); m_connection = nullptr; }
    m_connection = new Connection(m_socket, this, false);
    emit connected(m_connection);
}

void TlsClient::onDisconnected() {
    Logger::info(QObject::tr("TlsClient: Bağlantı kesildi"));
    // NOT: m_socket ve m_connection burada silinmez — setupSocket() halleder
    // Sadece m_connection pointer'ını null yap
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }
    emit disconnected();
    if (m_autoReconnect) {
        Logger::info(QObject::tr("TlsClient: Yeniden bağlanma zamanlayıcısı başlatılıyor"));
        m_reconnectTimer->start();
    }
}

void TlsClient::onSslErrors(const QList<QSslError>& errors) {
    if (m_anonymous) {
        // Discovery modunda tüm SSL hatalarını yoksay
        if (m_socket) m_socket->ignoreSslErrors(errors);
        return;
    }
    QList<QSslError> ignored;
    for (const QSslError& err : errors) {
        switch (err.error()) {
        case QSslError::SelfSignedCertificate:
        case QSslError::SelfSignedCertificateInChain:
        case QSslError::CertificateUntrusted:
        case QSslError::HostNameMismatch:
            ignored << err;
            break;
        case QSslError::CertificateExpired:
            // OTA/restart sonrası kısa süreli saat sapması toleransı (≤300 sn)
            if (!err.certificate().isNull()) {
                qint64 secsOverdue = err.certificate().expiryDate().secsTo(QDateTime::currentDateTimeUtc());
                if (secsOverdue >= 0 && secsOverdue <= 300) {
                    Logger::warning(QObject::tr("TlsClient: Sertifika yakın zamanda dolmuş (%1 sn), geçici OTA drift olarak kabul edildi (≤300 sn).")
                                    .arg(secsOverdue));
                    ignored << err;
                    break;
                }
            }
            Logger::error(QObject::tr("TlsClient: Sertifikanın geçerlilik süresi dolmuş (CertificateExpired)!"));
            emit error(QObject::tr("Sertifikanın süresi dolmuş"));
            if (m_socket) m_socket->disconnectFromHost();
            return;
        case QSslError::CertificateNotYetValid:
            // OTA/restart sonrası kısa süreli saat sapması toleransı (≤300 sn)
            // Okidia self-signed CA kullandığından, 5 dk içindeki notBefore drift güvenlidir.
            if (!err.certificate().isNull()) {
                qint64 secsUntilValid = QDateTime::currentDateTimeUtc().secsTo(err.certificate().effectiveDate());
                if (secsUntilValid >= 0 && secsUntilValid <= 300) {
                    Logger::warning(QObject::tr("TlsClient: Sertifika %1 sn sonra geçerli olacak, OTA/restart clock drift olarak kabul edildi (≤300 sn).")
                                    .arg(secsUntilValid));
                    ignored << err;
                    break;
                }
            }
            Logger::error(QObject::tr("TlsClient: Sertifika henüz geçerli değil (CertificateNotYetValid) — Sistem saatini kontrol edin!"));
            emit error(QObject::tr("Sistem saati geride / Sertifika henüz geçerli değil"));
            if (m_socket) m_socket->disconnectFromHost();
            return;
        default:
            Logger::error(QObject::tr("TlsClient: SSL hatası: ") + err.errorString());
            emit error("SSL hatası");
            if (m_socket) m_socket->disconnectFromHost();
            return;
        }
    }
    if (!ignored.isEmpty() && m_socket)
        m_socket->ignoreSslErrors(ignored);
}

void TlsClient::onReconnectTimer() {
    if (isConnected()) return;
    if (!m_autoReconnect || m_host.isEmpty()) return;

    // Sertifika dosyası silinmişse (revoke, manuel silme) → anonymous moda geç
    if (!m_anonymous && !m_certPath.isEmpty() && !QFile::exists(m_certPath)) {
        Logger::info(QObject::tr("TlsClient: Sertifika bulunamadi, anonymous moda geciliyor → ") + m_certPath);
        m_anonymous  = true;
        m_certPath   = QString();
        m_keyPath    = QString();
        m_caCertPath = QString();
    }

    Logger::info(QObject::tr("TlsClient: Yeniden baglaniliyor → ") + m_host +
                 (m_anonymous ? QObject::tr(" (anonymous)") : QObject::tr(" (sertifikali)")));

    if (m_anonymous)
        connectToServerAnonymous(m_host, m_port);
    else
        connectToServer(m_host, m_port, m_certPath, m_keyPath, m_caCertPath);
}

// ── TLS Handshake Timeout ────────────────────────────────────────────────────
void TlsClient::onHandshakeTimeout() {
    Logger::warning(QObject::tr("TlsClient: TLS handshake timeout → ") + m_host +
                    ":" + QString::number(m_port));
    if (m_socket) {
        m_socket->disconnect();  // sinyalleri ayır
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connection) {
        m_connection->deleteLater();
        m_connection = nullptr;
    }
    // disconnected sinyali manuel emit et — FinishPage bir sonraki host'a geçer
    emit disconnected();

    if (m_autoReconnect) {
        Logger::info(QObject::tr("TlsClient: TLS handshake timeout sonrası yeniden bağlanma zamanlayıcısı başlatılıyor"));
        m_reconnectTimer->start();
    }
}
