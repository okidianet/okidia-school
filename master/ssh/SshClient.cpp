#include "SshClient.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryFile>
#include <QDir>
#include <QStandardPaths>
#include <QThread>
#include <QNetworkProxy>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

// known_hosts dosyasına paralel erişim koruması
static QMutex s_knownHostsMutex;

// libssh2 global init/exit — tek seferlik
static struct Libssh2GlobalInit {
    Libssh2GlobalInit()  { libssh2_init(0); }
    ~Libssh2GlobalInit() { libssh2_exit(); }
} s_libssh2Init;

void SshClient::Config::wipe() {
    OkidiaUtils::secureWipe(password);
    OkidiaUtils::secureWipe(passphrase);
    if (privateKeyPath.trimmed().startsWith("-----BEGIN")) {
        OkidiaUtils::secureWipe(privateKeyPath);
    }
}

// ---------------------------------------------------------------------------
SshClient::SshClient(QObject* parent) : QObject(parent) {}

SshClient::~SshClient() {
    disconnectFromHost();
    m_config.wipe();
}

// ---------------------------------------------------------------------------
void SshClient::connectToHost(const Config& config) {
    if (m_state != State::Disconnected) disconnectFromHost();

    m_config = config;
    if (m_config.host.startsWith("::ffff:")) {
        m_config.host = m_config.host.mid(7);
    }
    m_state  = State::Connecting;

    Logger::info(QObject::tr("SshClient: Bağlanıyor → ") +
                 m_config.host + ":" + QString::number(m_config.port));

#ifdef Q_OS_WIN
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        setError(tr("TCP soket oluşturulamadı: ") + QString::number(WSAGetLastError()));
        return;
    }

    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(m_config.port);
    sin.sin_addr.s_addr = inet_addr(m_config.host.toUtf8().constData());
    if (sin.sin_addr.s_addr == INADDR_NONE) {
        hostent* hp = gethostbyname(m_config.host.toUtf8().constData());
        if (!hp) {
            ::closesocket(s);
            setError(tr("Sunucu adresi çözümlenemedi: ") + m_config.host);
            return;
        }
        sin.sin_addr = *reinterpret_cast<in_addr*>(hp->h_addr);
    }

    // Non-blocking bağlantı zaman aşımı kontrolü
    u_long nonblock = 1;
    ::ioctlsocket(s, FIONBIO, &nonblock);
    int res = ::connect(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin));
    if (res != 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set writeFds, errFds;
            FD_ZERO(&writeFds);
            FD_ZERO(&errFds);
            FD_SET(s, &writeFds);
            FD_SET(s, &errFds);
            timeval tv{};
            int timeoutMs = m_config.connectTimeoutMs > 0 ? m_config.connectTimeoutMs : 4500;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = ::select(0, nullptr, &writeFds, &errFds, &tv);
            if (sel <= 0 || FD_ISSET(s, &errFds)) {
                ::closesocket(s);
                setError(tr("TCP bağlantısı zaman aşımına uğradı (%1:%2)").arg(m_config.host).arg(m_config.port));
                return;
            }
        } else {
            ::closesocket(s);
            setError(tr("TCP bağlantı hatası (%1): %2").arg(m_config.host).arg(err));
            return;
        }
    }

    nonblock = 0; // Blocking moda geri al
    ::ioctlsocket(s, FIONBIO, &nonblock);

    m_sockFd = static_cast<int>(s);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        setError(tr("TCP soket oluşturulamadı: ") + QString::number(errno));
        return;
    }

    int nodelay = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(m_config.port);
    sin.sin_addr.s_addr = inet_addr(m_config.host.toUtf8().constData());
    if (sin.sin_addr.s_addr == INADDR_NONE) {
        hostent* hp = gethostbyname(m_config.host.toUtf8().constData());
        if (!hp) {
            ::close(s);
            setError(tr("Sunucu adresi çözümlenemedi: ") + m_config.host);
            return;
        }
        sin.sin_addr = *reinterpret_cast<in_addr*>(hp->h_addr);
    }

    // Non-blocking bağlantı zaman aşımı kontrolü
    int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int res = ::connect(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin));
    if (res != 0) {
        if (errno == EINPROGRESS) {
            fd_set writeFds, errFds;
            FD_ZERO(&writeFds);
            FD_ZERO(&errFds);
            FD_SET(s, &writeFds);
            FD_SET(s, &errFds);
            timeval tv{};
            int timeoutMs = m_config.connectTimeoutMs > 0 ? m_config.connectTimeoutMs : 4500;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = ::select(s + 1, nullptr, &writeFds, &errFds, &tv);
            if (sel <= 0 || FD_ISSET(s, &errFds)) {
                ::close(s);
                setError(tr("TCP bağlantısı zaman aşımına uğradı (%1:%2)").arg(m_config.host).arg(m_config.port));
                return;
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error != 0) {
                ::close(s);
                setError(tr("TCP bağlantı hatası (%1): %2").arg(m_config.host).arg(so_error));
                return;
            }
        } else {
            ::close(s);
            setError(tr("TCP bağlantı hatası (%1): %2").arg(m_config.host).arg(errno));
            return;
        }
    }

    ::fcntl(s, F_SETFL, flags); // Blocking moda geri al
    m_sockFd = s;
#endif

    Logger::debug(QObject::tr("SshClient: Soket bağlandı (FD: ") + QString::number(m_sockFd) + QObject::tr("), handshake başlıyor..."));

    m_session = libssh2_session_init();
    if (!m_session) {
        setError(tr("libssh2 oturumu oluşturulamadı"));
        return;
    }
    libssh2_session_set_timeout(m_session, 10000); // 10 sn zaman aşımı
    doHandshake();
}

void SshClient::disconnectFromHost() {
    // Kanalları kapat
    for (auto* ch : m_channels)
        libssh2_channel_free(ch);
    m_channels.clear();

    if (m_knownHosts) {
        libssh2_knownhost_free(m_knownHosts);
        m_knownHosts = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect(m_session, "Normal Shutdown");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }

    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }

    if (m_sockFd != -1) {
#ifdef Q_OS_WIN
        ::closesocket(m_sockFd);
#else
        ::close(m_sockFd);
#endif
        m_sockFd = -1;
    }
    
    if (m_state != State::Disconnected) {
        m_state = State::Disconnected;
        emit disconnected();
    }
}

// ---------------------------------------------------------------------------
void SshClient::doHandshake() {
    // İlk el sıkışması (handshake) için geçici olarak BLOKLAYAN mod kullanıyoruz.
    // Bu, yerel ağdaki paket kaçırma veya gecikme sorunlarını çözer.
    libssh2_session_set_blocking(m_session, 1);
 
    int rc = libssh2_session_handshake(m_session, m_sockFd);
    
    if (rc != 0) {
        setError(tr("SSH el sıkışması başarısız (rc=") + QString::number(rc) + "): " + sshError());
        return;
    }

    // El sıkışması bitti, artık non-blocking moda geçebiliriz (Terminal widget için gerekli)
    libssh2_session_set_blocking(m_session, 0);
    
    Logger::info(QObject::tr("SshClient: El sıkışması tamamlandı → ") + m_config.host);

    // Host key kontrolü
    if (m_config.hostKeyCheck) {
        if (!checkHostKey()) return;
    } else {
        autoAcceptHostKeySilently();
    }

    m_state = State::Authenticating;
    Logger::info(QObject::tr("SshClient: Kimlik doğrulama başlatılıyor → ") + m_config.host);
    doAuthenticate();
}

// Helper to map libssh2 hostkey type to knownhost type flags
static int getKnownHostKeyType(int hostkeyType) {
    switch (hostkeyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:
            return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS:
            return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
            return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_384
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
            return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_521
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
            return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
        case LIBSSH2_HOSTKEY_TYPE_ED25519:
            return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
        default:
            return LIBSSH2_KNOWNHOST_KEY_SSHRSA; // fallback
    }
}

// ---------------------------------------------------------------------------
bool SshClient::checkHostKey() {
    QMutexLocker locker(&s_knownHostsMutex);

    size_t len; int type;
    const char* raw = libssh2_session_hostkey(
        m_session, &len, &type);
    if (!raw) return true; // devam et

    // SHA256 parmak izi
    const char* fp = libssh2_hostkey_hash(
        m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    QByteArray fpBytes(fp, 32);
    m_fingerprint = "SHA256:" + fpBytes.toBase64();

    // known_hosts kontrolü
    m_knownHosts = libssh2_knownhost_init(m_session);

    QString khPath = m_config.knownHostsPath;
    if (khPath.isEmpty())
        khPath = QDir::homePath() + "/.ssh/known_hosts";

    libssh2_knownhost_readfile(
        m_knownHosts, khPath.toLocal8Bit(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    struct libssh2_knownhost* kh = nullptr;
    int check = libssh2_knownhost_checkp(
        m_knownHosts,
        m_config.host.toLocal8Bit(),
        m_config.port,
        raw, len,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW,
        &kh);

    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) return true;

    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        // Bilinmeyen host — kullanıcıya sor
        QString keyTypeStr = (type == LIBSSH2_HOSTKEY_TYPE_RSA)
            ? "RSA" : (type == LIBSSH2_HOSTKEY_TYPE_DSS)
            ? "DSS" : "Ed25519/ECDSA";
        emit hostKeyUnknown(m_fingerprint, keyTypeStr);
        // acceptHostKey() çağrılana kadar bekle
        return false;
    }

    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        emit hostKeyMismatch(m_fingerprint);
        return false;
    }

    return true;
}

void SshClient::autoAcceptHostKeySilently() {
    QMutexLocker locker(&s_knownHostsMutex);

    size_t len; int type;
    const char* raw = libssh2_session_hostkey(m_session, &len, &type);
    if (!raw) return;

    LIBSSH2_KNOWNHOSTS* khs = libssh2_knownhost_init(m_session);
    if (!khs) return;

    QString khPath = m_config.knownHostsPath;
    if (khPath.isEmpty())
        khPath = QDir::homePath() + "/.ssh/known_hosts";

    // Eger .ssh klasörü yoksa olustur
    QDir().mkpath(QFileInfo(khPath).absolutePath());

    libssh2_knownhost_readfile(
        khs, khPath.toLocal8Bit(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    struct libssh2_knownhost* kh = nullptr;
    int check = libssh2_knownhost_checkp(
        khs,
        m_config.host.toLocal8Bit(),
        m_config.port,
        raw, len,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW,
        &kh);

    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND || check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH && kh) {
            libssh2_knownhost_del(khs, kh);
        }

        int typeMask = getKnownHostKeyType(type);

        libssh2_knownhost_addc(
            khs,
            m_config.host.toLocal8Bit(), nullptr,
            raw, len, nullptr, 0,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN |
            LIBSSH2_KNOWNHOST_KEYENC_RAW |
            typeMask,
            nullptr);

        libssh2_knownhost_writefile(
            khs, khPath.toLocal8Bit(),
            LIBSSH2_KNOWNHOST_FILE_OPENSSH);

        Logger::info(QObject::tr("SshClient: ") + m_config.host + QObject::tr(" icin host key güncellendi / known_hosts dosyasina eklendi."));
    }

    libssh2_knownhost_free(khs);
}

bool SshClient::acceptHostKey() {
    QMutexLocker locker(&s_knownHostsMutex);

    // known_hosts'a ekle
    if (!m_knownHosts || !m_session) return false;

    size_t len; int type;
    const char* raw = libssh2_session_hostkey(m_session, &len, &type);
    if (!raw) return false;

    int typeMask = getKnownHostKeyType(type);

    libssh2_knownhost_addc(
        m_knownHosts,
        m_config.host.toLocal8Bit(), nullptr,
        raw, len, nullptr, 0,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN |
        LIBSSH2_KNOWNHOST_KEYENC_RAW |
        typeMask,
        nullptr);

    QString khPath = m_config.knownHostsPath;
    if (khPath.isEmpty())
        khPath = QDir::homePath() + "/.ssh/known_hosts";

    // Eger .ssh klasörü yoksa olustur
    QDir().mkpath(QFileInfo(khPath).absolutePath());

    libssh2_knownhost_writefile(
        m_knownHosts, khPath.toLocal8Bit(),
        LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    // Devam et
    m_state = State::Authenticating;
    doAuthenticate();
    return true;
}

bool SshClient::rejectHostKey() {
    setError("Host key reddedildi: " + m_fingerprint);
    return false;
}

// ---------------------------------------------------------------------------
void SshClient::doAuthenticate() {
    int rc = LIBSSH2_ERROR_EAGAIN;
    
    if (m_state != State::Authenticating) return;

    // Kimlik doğrulama için de geçici olarak BLOKLAYAN mod kullanıyoruz.
    libssh2_session_set_blocking(m_session, 1);

    switch (m_config.authMethod) {
    case AuthMethod::Password: {
        QByteArray passBytes = m_config.password.toUtf8();
        OkidiaUtils::secureLockMemory(passBytes.data(), static_cast<size_t>(passBytes.size()));

        rc = libssh2_userauth_password(
            m_session,
            m_config.username.toUtf8(),
            passBytes.constData());

        if (rc != 0) {
            char* userauthlist = libssh2_userauth_list(m_session, m_config.username.toUtf8().constData(), m_config.username.toUtf8().length());
            if (userauthlist && strstr(userauthlist, "keyboard-interactive")) {
                Logger::info(QObject::tr("SshClient: Parola yetkilendirmesi yanıt vermedi, keyboard-interactive deneniyor..."));
                thread_local QByteArray t_cachedPasswordBytes;
                t_cachedPasswordBytes = passBytes;
                rc = libssh2_userauth_keyboard_interactive(
                    m_session,
                    m_config.username.toUtf8().constData(),
                    [](const char* /*name*/, int /*name_len*/, const char* /*instruction*/, int /*instruction_len*/,
                       int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT* /*prompts*/,
                       LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** /*abstract*/) {
                        for (int i = 0; i < num_prompts; ++i) {
                            responses[i].text = strdup(t_cachedPasswordBytes.constData());
                            responses[i].length = t_cachedPasswordBytes.length();
                        }
                    });
                OkidiaUtils::secureWipe(t_cachedPasswordBytes);
            }
        }
        OkidiaUtils::secureUnlockMemory(passBytes.data(), static_cast<size_t>(passBytes.size()));
        OkidiaUtils::secureWipe(passBytes);
        break;
    }

    case AuthMethod::PrivateKey: {
        QString actualKeyPath = m_config.privateKeyPath;
        QTemporaryFile* tempKeyFile = nullptr;

        if (m_config.privateKeyPath.trimmed().startsWith("-----BEGIN")) {
            tempKeyFile = new QTemporaryFile(this);
            tempKeyFile->setAutoRemove(false);
            if (tempKeyFile->open()) {
                // Key dosyasının izinleri libssh2 için 0600 (owner only) olmalı.
                tempKeyFile->write(m_config.privateKeyPath.toUtf8());
                tempKeyFile->flush();
                actualKeyPath = tempKeyFile->fileName();
                OkidiaUtils::securePrivateKeyFile(actualKeyPath, false);
                Logger::info(QObject::tr("SshClient: Geçici özel anahtar dosyası oluşturuldu ve kilitlendi: ") + actualKeyPath);
            } else {
                Logger::error(QObject::tr("SshClient: Geçici özel anahtar dosyası oluşturulamadı!"));
            }
        }

        QString pubPath = m_config.publicKeyPath;
        if (pubPath.isEmpty() && !tempKeyFile)
            pubPath = actualKeyPath + ".pub";

        // .pub yoksa null gönder (libssh2 otomatik çıkarır)
        const char* pub = (!pubPath.isEmpty() && QFile::exists(pubPath))
            ? pubPath.toLocal8Bit().constData() : nullptr;

        QByteArray passphrBytes = m_config.passphrase.toUtf8();
        if (!passphrBytes.isEmpty()) {
            OkidiaUtils::secureLockMemory(passphrBytes.data(), static_cast<size_t>(passphrBytes.size()));
        }

        rc = libssh2_userauth_publickey_fromfile(
            m_session,
            m_config.username.toUtf8(),
            pub,
            actualKeyPath.toLocal8Bit().constData(),
            passphrBytes.constData());

        if (!passphrBytes.isEmpty()) {
            OkidiaUtils::secureUnlockMemory(passphrBytes.data(), static_cast<size_t>(passphrBytes.size()));
            OkidiaUtils::secureWipe(passphrBytes);
        }

        if (tempKeyFile) {
            QString tmpPath = tempKeyFile->fileName();
            tempKeyFile->close();
            delete tempKeyFile;
            tempKeyFile = nullptr;
            OkidiaUtils::secureRemoveFile(tmpPath);
            Logger::info(QObject::tr("SshClient: Geçici özel anahtar dosyası güvenli şekilde silindi."));
        }
        break;
    }

    case AuthMethod::Agent: {
        LIBSSH2_AGENT* agent = libssh2_agent_init(m_session);
        if (!agent) { setError(tr("SSH agent başlatılamadı")); return; }

        libssh2_agent_connect(agent);
        libssh2_agent_list_identities(agent);

        struct libssh2_agent_publickey* identity = nullptr;
        struct libssh2_agent_publickey* prev     = nullptr;

        while (libssh2_agent_get_identity(agent, &identity, prev) == 1) {
            rc = libssh2_agent_userauth(
                agent, m_config.username.toUtf8(), identity);
            if (rc == 0) break;
            prev = identity;
        }
        libssh2_agent_free(agent);
        break;
    }
    }

    // İşlem bitti, non-blocking moda geri dön (Terminal widget için gerekli)
    libssh2_session_set_blocking(m_session, 0);

    if (rc != 0) {
        // Hata durumunda da kimlik bilgilerini RAM'den derhal temizle
        m_config.wipe();
        emit authenticationFailed(sshError());
        setError(tr("Kimlik doğrulama başarısız: ") + sshError());
        return;
    }

    // Bağlantı başarılı oldu: Parola ve anahtar ömrü bitti, RAM'den güvenle sıfırla
    m_config.wipe();

    // Exec kanallarındaki komut çıktılarını QSocketNotifier ile dinliyoruz (varsayılan kapalı).
    m_notifier = new QSocketNotifier(m_sockFd, QSocketNotifier::Read, this);
    m_notifier->setEnabled(false);
    connect(m_notifier, &QSocketNotifier::activated, this, &SshClient::pollChannels);

    m_state = State::Connected;
    Logger::info(QObject::tr("SshClient: Bağlandı → ") + m_config.host +
                 QObject::tr(" olarak ") + m_config.username);
    emit connected();
}

// ---------------------------------------------------------------------------
int SshClient::execute(const QString& command) {
    if (!isConnected()) return -1;

    LIBSSH2_CHANNEL* channel = nullptr;
    do {
        channel = libssh2_channel_open_session(m_session);
        if (!channel && libssh2_session_last_error(
                m_session, nullptr, nullptr, 0) == LIBSSH2_ERROR_EAGAIN) {
            QThread::msleep(10);
        }
    } while (!channel);

    // Komutu gönder
    int rc;
    do {
        rc = libssh2_channel_exec(
            channel, command.toUtf8());
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    if (rc != 0) {
        libssh2_channel_free(channel);
        return -1;
    }

    int id = m_nextChannelId++;
    m_channels[id] = channel;
    if (m_notifier) m_notifier->setEnabled(true);

    // Non-blocking polling için timer
    QTimer::singleShot(50, this, [this]() { pollChannels(); });

    return id;
}

LIBSSH2_CHANNEL* SshClient::openPtyChannel(int cols, int rows) {
    if (!isConnected()) return nullptr;

    libssh2_session_set_blocking(m_session, 1);

    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(m_session);
    if (!ch) {
        libssh2_session_set_blocking(m_session, 0);
        return nullptr;
    }

    int rc = libssh2_channel_request_pty_ex(
        ch, "xterm-256color", strlen("xterm-256color"),
        nullptr, 0,
        cols, rows, 0, 0);

    if (rc != 0) {
        libssh2_channel_free(ch);
        libssh2_session_set_blocking(m_session, 0);
        return nullptr;
    }

    rc = libssh2_channel_shell(ch);
    if (rc != 0) {
        libssh2_channel_free(ch);
        libssh2_session_set_blocking(m_session, 0);
        return nullptr;
    }

    libssh2_session_set_blocking(m_session, 0);
    return ch;
}

void SshClient::resizePty(LIBSSH2_CHANNEL* ch, int cols, int rows) {
    if (!ch || !m_session) return;

    libssh2_session_set_blocking(m_session, 1);
    
    int rc;
    do {
        rc = libssh2_channel_request_pty_size_ex(ch, cols, rows, 0, 0);
        if (rc == LIBSSH2_ERROR_EAGAIN) QThread::msleep(5);
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    libssh2_session_set_blocking(m_session, 0);
}

// ---------------------------------------------------------------------------
QString SshClient::executeSync(const QString& command, int timeoutMs, int* exitCode, std::function<void(const QByteArray&)> onOutput, std::function<bool()> isCancelled) {
    if (!isConnected()) return {};

    // Oturumun tamamen BLOKLAYAN (blocking) modda çalıştığından emin olalım.
    libssh2_session_set_blocking(m_session, 1);
    
    // Geçici olarak bu komut için timeout ayarlayalım
    libssh2_session_set_timeout(m_session, timeoutMs > 0 ? timeoutMs : 10000);

    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(m_session);
    if (!ch) {
        Logger::error(QObject::tr("executeSync: libssh2_channel_open_session failed: ") + sshError());
        if (exitCode) *exitCode = -1;
        return {};
    }

    int execRc = libssh2_channel_exec(ch, command.toUtf8());
    if (execRc != 0) {
        Logger::error(QObject::tr("executeSync: libssh2_channel_exec failed: ") + sshError());
        libssh2_channel_free(ch);
        if (exitCode) *exitCode = execRc;
        return {};
    }

    // Okuma işlemi için non-blocking moda geç
    libssh2_session_set_blocking(m_session, 0);

    QByteArray output;
    char buf[4096];
    QElapsedTimer idleTimer;
    idleTimer.start();
    QElapsedTimer totalTimer;
    totalTimer.start();
    
    // Non-blocking okuma döngüsü
    while (true) {
        if (isCancelled && isCancelled()) {
            output.append(QString(tr("\n[İşlem kullanıcı veya yeni görev tarafından iptal edildi]")).toUtf8());
            break;
        }

        // Toplam çalışma süresi (hard timeout: timeoutMs veya en az 1 saat) kontrolü
        int hardTimeoutMs = qMax(timeoutMs, 3600000);
        if (totalTimer.hasExpired(hardTimeoutMs)) {
            output.append(QString(tr("\n[Toplam zaman aşımı (%1 sn aşıldı, komut sonlandırıldı)]")).arg(hardTimeoutMs / 1000).toUtf8());
            break;
        }

        // Hareketsizlik zaman aşımı (idle timeout)
        if (timeoutMs > 0 && idleTimer.hasExpired(timeoutMs)) {
            output.append(QString(tr("\n[Zaman aşımı (%1 sn hareketsizlik)]")).arg(timeoutMs / 1000).toUtf8());
            break;
        }

        bool activity = false;
        
        int rcOut = libssh2_channel_read(ch, buf, sizeof(buf));
        if (rcOut > 0) {
            output.append(buf, rcOut);
            if (onOutput) onOutput(QByteArray(buf, rcOut));
            activity = true;
        } else if (rcOut < 0 && rcOut != LIBSSH2_ERROR_EAGAIN) {
            Logger::warning(QObject::tr("executeSync: kanal okuma hatası veya soket koptu: ") + QString::number(rcOut));
            break;
        }

        int rcErr = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
        if (rcErr > 0) {
            output.append(buf, rcErr);
            if (onOutput) onOutput(QByteArray(buf, rcErr));
            activity = true;
        } else if (rcErr < 0 && rcErr != LIBSSH2_ERROR_EAGAIN) {
            Logger::warning(QObject::tr("executeSync: stderr okuma hatası veya soket koptu: ") + QString::number(rcErr));
            break;
        }

        if (activity) {
            idleTimer.restart(); // Idle timeout mantığı: aktivite oldukça sıfırla
        }

        if (libssh2_channel_eof(ch)) {
            // EOF oldu, tamponda kalanları son bir kez okuyalım
            while ((rcOut = libssh2_channel_read(ch, buf, sizeof(buf))) > 0) {
                output.append(buf, rcOut);
            }
            while ((rcErr = libssh2_channel_read_stderr(ch, buf, sizeof(buf))) > 0) {
                output.append(buf, rcErr);
            }
            break;
        }
        
        if (!activity) {
            QThread::msleep(10);
        }
    }

    // Kapatma işlemleri için tekrar blocking moda geç
    libssh2_session_set_blocking(m_session, 1);

    libssh2_channel_close(ch);
    if (exitCode) {
        *exitCode = libssh2_channel_get_exit_status(ch);
    }
    libssh2_channel_free(ch);

    return QString::fromUtf8(output);
}

// ---------------------------------------------------------------------------
bool SshClient::uploadFile(const QString& localPath,
                            const QString& remotePath,
                            std::function<void(qint64,qint64)> progress) {
    if (!isConnected()) return false;

    m_lastError.clear();
    libssh2_session_set_blocking(m_session, 1);

    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_lastError = tr("Yerel dosya okuma için açılamadı.");
        return false;
    }

    qint64 total = f.size();

    LIBSSH2_SFTP* sftp = nullptr;
    libssh2_session_set_timeout(m_session, 45000); // Zayıf Wi-Fi ağları için 45 sn tolerans
    for (int retry = 0; retry < 5; ++retry) {
        sftp = libssh2_sftp_init(m_session);
        if (sftp) break;
        int err = libssh2_session_last_errno(m_session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            QThread::msleep(50);
        } else {
            QThread::msleep(200);
        }
    }

    if (!sftp) {
        m_lastError = sshError();
        f.close();
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(
        sftp, remotePath.toUtf8(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);

    if (!fh) { 
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        if (sftp_err == LIBSSH2_FX_PERMISSION_DENIED) {
            m_lastError = "Permission denied";
        } else {
            m_lastError = QString("SFTP Error: %1").arg(sftp_err);
        }
        libssh2_sftp_shutdown(sftp); 
        libssh2_session_set_blocking(m_session, 0);
        return false; 
    }

    qint64 sent = 0;
    // Zayıf Wi-Fi ve küçük MTU ağları için 16 KB tampon boyutu (CSMA/CA collision önleyici)
    char buf[16384];
    while (!f.atEnd()) {
        qint64 nr = f.read(buf, sizeof(buf));
        if (nr <= 0) break;

        char* ptr = buf;
        while (nr > 0) {
            int nw = libssh2_sftp_write(fh, ptr, static_cast<size_t>(nr));
            if (nw == LIBSSH2_ERROR_EAGAIN) {
                QThread::msleep(10);
                continue;
            }
            if (nw < 0) {
                // Wi-Fi paket kaybı/jitter anında hemen vazgeçmeyip 3 kez tolerans tanı
                int lastErr = libssh2_session_last_errno(m_session);
                bool retried = false;
                for (int writeRetry = 0; writeRetry < 3; ++writeRetry) {
                    QThread::msleep(150);
                    nw = libssh2_sftp_write(fh, ptr, static_cast<size_t>(nr));
                    if (nw > 0) {
                        retried = true;
                        break;
                    }
                    if (nw == LIBSSH2_ERROR_EAGAIN) {
                        QThread::msleep(50);
                    }
                }
                if (!retried && nw < 0) {
                    m_lastError = tr("SFTP veri yazma hatası (rc=%1, err=%2)").arg(nw).arg(lastErr);
                    goto done;
                }
            }
            ptr += nw;
            nr  -= nw;
            sent += nw;
        }
        if (progress) progress(sent, total);
        emit transferProgress(sent, total);
    }

done:
    libssh2_sftp_close(fh);
    libssh2_sftp_shutdown(sftp);
    f.close();
    libssh2_session_set_blocking(m_session, 0);
    Logger::info(QObject::tr("SshClient: Upload tamamlandı → ") + remotePath);
    return sent == total;
}

// ---------------------------------------------------------------------------
bool SshClient::downloadFile(const QString& remotePath,
                            const QString& localPath,
                            std::function<void(qint64,qint64)> progress) {
    if (!isConnected()) return false;

    m_lastError.clear();
    QFile f(localPath);
    if (!f.open(QIODevice::WriteOnly)) {
        m_lastError = tr("Yerel dosya yazma için açılamadı.");
        return false;
    }

    LIBSSH2_SFTP* sftp = nullptr;
    do {
        sftp = libssh2_sftp_init(m_session);
        if (!sftp) {
            int err = libssh2_session_last_errno(m_session);
            if (err == LIBSSH2_ERROR_EAGAIN) { QThread::msleep(5); continue; }
            m_lastError = sshError();
            f.close();
            return false;
        }
    } while (!sftp);

    LIBSSH2_SFTP_HANDLE* fh = nullptr;
    do {
        fh = libssh2_sftp_open(
            sftp, remotePath.toUtf8().constData(),
            LIBSSH2_FXF_READ, 0);
        if (!fh) {
            int err = libssh2_session_last_errno(m_session);
            if (err == LIBSSH2_ERROR_EAGAIN) { QThread::msleep(5); continue; }
            unsigned long sftp_err = libssh2_sftp_last_error(sftp);
            if (sftp_err == LIBSSH2_FX_PERMISSION_DENIED) {
                m_lastError = "Permission denied";
            } else {
                m_lastError = QString("SFTP Error: %1").arg(sftp_err);
            }
            libssh2_sftp_shutdown(sftp);
            f.close();
            return false;
        }
    } while (!fh);

    // Boyutu al
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    qint64 total = 0;
    while (true) {
        int rc = libssh2_sftp_fstat(fh, &attrs);
        if (rc == LIBSSH2_ERROR_EAGAIN) { QThread::msleep(5); continue; }
        if (rc == 0) {
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
                total = attrs.filesize;
            }
        }
        break;
    }

    qint64 received = 0;
    char buf[32768];
    while (true) {
        int rc = libssh2_sftp_read(fh, buf, sizeof(buf));
        if (rc == LIBSSH2_ERROR_EAGAIN) { QThread::msleep(5); continue; }
        if (rc < 0) {
            break; // Hata
        }
        if (rc == 0) {
            break; // EOF
        }

        f.write(buf, rc);
        received += rc;
        if (progress) progress(received, total);
        emit transferProgress(received, total);
    }

    libssh2_sftp_close(fh);
    libssh2_sftp_shutdown(sftp);
    f.close();
    libssh2_session_set_blocking(m_session, 0);
    Logger::info(QObject::tr("SshClient: Download tamamlandı ← ") + remotePath);
    return (total == 0) || (received == total);
}

// ---------------------------------------------------------------------------
bool SshClient::listDirectory(const QString& remotePath,
                              QList<RemoteFileInfo>& outFiles) {
    if (!isConnected()) return false;
    m_lastError.clear();
    libssh2_session_set_blocking(m_session, 1);

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(m_session);
    if (!sftp) {
        m_lastError = sshError();
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    LIBSSH2_SFTP_HANDLE* dh = libssh2_sftp_opendir(sftp, remotePath.toUtf8().constData());
    if (!dh) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        m_lastError = QString("SFTP Error: %1").arg(sftp_err);
        libssh2_sftp_shutdown(sftp);
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    char mem[512];
    char longentry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    outFiles.clear();

    while (true) {
        int rc = libssh2_sftp_readdir_ex(dh, mem, sizeof(mem), longentry, sizeof(longentry), &attrs);
        if (rc <= 0) break;

        QString name = QString::fromUtf8(mem, rc);
        if (name == "." || name == "..") continue;

        RemoteFileInfo fi;
        fi.name = name;
        fi.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? static_cast<qint64>(attrs.filesize) : 0;
        fi.isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? LIBSSH2_SFTP_S_ISDIR(attrs.permissions) : false;
        
        if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            uint perms = attrs.permissions;
            fi.permissions = QString::number((perms & 0777), 8);
        } else {
            fi.permissions = "-";
        }

        if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
            fi.mtime = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attrs.mtime));
        } else {
            fi.mtime = QDateTime::currentDateTime();
        }

        outFiles.append(fi);
    }

    libssh2_sftp_closedir(dh);
    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(m_session, 0);
    return true;
}

// ---------------------------------------------------------------------------
bool SshClient::createDirectory(const QString& remotePath) {
    if (!isConnected()) return false;
    m_lastError.clear();
    libssh2_session_set_blocking(m_session, 1);

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(m_session);
    if (!sftp) {
        m_lastError = sshError();
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    int rc = libssh2_sftp_mkdir(sftp, remotePath.toUtf8().constData(),
                                LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH);
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        m_lastError = QString("SFTP mkdir Error: %1").arg(sftp_err);
    }

    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(m_session, 0);
    return rc == 0;
}

// ---------------------------------------------------------------------------
bool SshClient::renameRemoteFile(const QString& oldPath, const QString& newPath) {
    if (!isConnected()) return false;
    m_lastError.clear();
    libssh2_session_set_blocking(m_session, 1);

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(m_session);
    if (!sftp) {
        m_lastError = sshError();
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    int rc = libssh2_sftp_rename(sftp, oldPath.toUtf8().constData(), newPath.toUtf8().constData());
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        m_lastError = QString("SFTP rename Error: %1").arg(sftp_err);
    }

    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(m_session, 0);
    return rc == 0;
}

// ---------------------------------------------------------------------------
bool SshClient::removeRemoteFile(const QString& remotePath) {
    if (!isConnected()) return false;
    m_lastError.clear();
    libssh2_session_set_blocking(m_session, 1);

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(m_session);
    if (!sftp) {
        m_lastError = sshError();
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    int rc = libssh2_sftp_unlink(sftp, remotePath.toUtf8().constData());
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        m_lastError = QString("SFTP unlink Error: %1").arg(sftp_err);
    }

    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(m_session, 0);
    return rc == 0;
}

// ---------------------------------------------------------------------------
bool SshClient::removeRemoteDirectory(const QString& remotePath) {
    if (!isConnected()) return false;
    m_lastError.clear();
    libssh2_session_set_blocking(m_session, 1);

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(m_session);
    if (!sftp) {
        m_lastError = sshError();
        libssh2_session_set_blocking(m_session, 0);
        return false;
    }

    int rc = libssh2_sftp_rmdir(sftp, remotePath.toUtf8().constData());
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp);
        m_lastError = QString("SFTP rmdir Error: %1").arg(sftp_err);
    }

    libssh2_sftp_shutdown(sftp);
    libssh2_session_set_blocking(m_session, 0);
    return rc == 0;
}

// ---------------------------------------------------------------------------
void SshClient::pollChannels() {
    if (!isConnected() || m_channels.isEmpty()) return;

    char buf[8192];
    const QList<int> ids = m_channels.keys();

    for (int id : ids) {
        if (!m_channels.contains(id)) continue;
        LIBSSH2_CHANNEL* ch = m_channels[id];

        // stdout
        int rc = libssh2_channel_read(ch, buf, sizeof(buf));
        if (rc > 0)
            emit commandOutput(id, QByteArray(buf, rc));

        if (!m_channels.contains(id)) continue;

        // stderr
        rc = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
        if (rc > 0)
            emit commandError(id, QByteArray(buf, rc));

        if (!m_channels.contains(id)) continue;

        // Bitti mi?
        if (libssh2_channel_eof(ch)) {
            int exitCode = libssh2_channel_get_exit_status(ch);
            libssh2_channel_close(ch);
            
            // Remove from map FIRST so disconnectFromHost() called in slot won't double-free it
            m_channels.remove(id);
            
            libssh2_channel_free(ch);
            emit commandFinished(id, exitCode);
        }
    }

    if (!m_channels.isEmpty()) {
        QTimer::singleShot(50, this, [this]() { pollChannels(); });
    } else {
        if (m_notifier) m_notifier->setEnabled(false);
    }
}

// ---------------------------------------------------------------------------
void SshClient::setError(const QString& msg) {
    m_lastError = msg;
    m_state = State::Error;
    Logger::error(QObject::tr("SshClient: ") + msg);
    emit errorOccurred(msg);
    disconnectFromHost();
}

QString SshClient::sshError() const {
    if (!m_session) return tr("Oturum yok");
    char* msg = nullptr;
    int code = libssh2_session_last_error(m_session, &msg, nullptr, 0);
    QString msgStr = msg ? QString::fromUtf8(msg).trimmed() : QString();
    if (code == 0 && msgStr.isEmpty()) {
        return tr("Kullanıcı adı veya parola hatalı.");
    }
    if (msgStr.isEmpty()) {
        msgStr = tr("bilinmeyen hata");
    }
    return QString("(%1) %2").arg(code).arg(msgStr);
}

// ---------------------------------------------------------------------------
void SshClient::enableKeepalive(int seconds) {
    if (m_session && seconds > 0) {
        libssh2_keepalive_config(m_session, 1, seconds);
    }
}

bool SshClient::sendKeepalive() {
    if (!m_session) return false;
    int seconds_to_next = 0;
    int rc = libssh2_keepalive_send(m_session, &seconds_to_next);
    // If it returns > 0, it means it's not time to send keepalive yet (which is fine).
    // If it returns 0, it sent a keepalive successfully.
    // If it returns < 0, an error occurred (e.g., connection lost).
    if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN) {
        return false;
    }
    return true;
}
