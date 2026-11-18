// master/ssh/SshClient.h
#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QByteArray>
#include <QMap>
#include <functional>

#include <libssh2.h>
#include <libssh2_sftp.h>

/**
 * libssh2 tabanlı asenkron SSH istemcisi.
 *
 * Desteklenen kimlik doğrulama:
 *   - Kullanıcı adı + parola
 *   - Kullanıcı adı + özel anahtar (RSA/Ed25519)
 *   - Kullanıcı adı + SSH agent (ssh-agent)
 *
 * Desteklenen işlemler:
 *   - Komut çalıştırma (exec) — çıktı sinyal ile gelir
 *   - Etkileşimli terminal (PTY) — SshTerminalWidget ile kullanılır
 *   - SFTP dosya transfer (upload/download)
 *   - Port forwarding (ileride)
 */
#include <QDateTime>

class SshClient : public QObject {
    Q_OBJECT

public:
    enum class AuthMethod { Password, PrivateKey, Agent };
    enum class State {
        Disconnected, Connecting, Authenticating,
        Connected, Error
    };

    struct RemoteFileInfo {
        QString   name;
        qint64    size = 0;
        bool      isDir = false;
        QString   permissions;
        QDateTime mtime;
    };

    struct Config {
        QString  host;
        quint16  port         = 22;
        QString  username;
        AuthMethod authMethod = AuthMethod::Password;
        QString  password;
        QString  privateKeyPath;   // ~/.ssh/id_ed25519
        QString  publicKeyPath;    // ~/.ssh/id_ed25519.pub (boş → otomatik)
        QString  passphrase;       // private key parolası
        int      connectTimeoutMs = 4000;
        bool     hostKeyCheck     = true;
        QString  knownHostsPath;   // boş → ~/.ssh/known_hosts

        // Hassas parola ve anahtar içeriklerini RAM'den güvenle temizle
        void wipe();
    };

    explicit SshClient(QObject* parent = nullptr);
    ~SshClient();

    // Bağlan / kapat
    void connectToHost(const Config& config);
    void disconnectFromHost();

    State state() const { return m_state; }
    bool  isConnected() const { return m_state == State::Connected; }
    QString lastError() const { return m_lastError; }
    QString hostFingerprint() const { return m_fingerprint; }

    // Komut çalıştır — non-blocking, sonuç sinyalle gelir
    // Dönen int: channel ID (birden fazla paralel komut için)
    int execute(const QString& command);

    // Etkileşimli PTY kanalı aç (terminal için)
    // Dönen LIBSSH2_CHANNEL* — SshTerminalWidget doğrudan kullanır
    LIBSSH2_CHANNEL* openPtyChannel(int cols = 80, int rows = 24);
    void resizePty(LIBSSH2_CHANNEL* ch, int cols, int rows);

    // SFTP
    bool uploadFile(const QString& localPath,
                    const QString& remotePath,
                    std::function<void(qint64,qint64)> progress = nullptr);
    bool downloadFile(const QString& remotePath,
                      const QString& localPath,
                      std::function<void(qint64,qint64)> progress = nullptr);
    bool listDirectory(const QString& remotePath,
                       QList<RemoteFileInfo>& outFiles);
    bool createDirectory(const QString& remotePath);
    bool renameRemoteFile(const QString& oldPath, const QString& newPath);
    bool removeRemoteFile(const QString& remotePath);
    bool removeRemoteDirectory(const QString& remotePath);

    // SFTP komut çalıştır ve tüm çıktıyı döndür (senkron, timeout'lu)
    QString executeSync(const QString& command,
                        int timeoutMs = 5000, int* exitCode = nullptr,
                        std::function<void(const QByteArray&)> onOutput = nullptr,
                        std::function<bool()> isCancelled = nullptr);

    // Keepalive (Terminal bağlantı kopmasını anında algılamak için)
    void enableKeepalive(int seconds);
    bool sendKeepalive();


    // Host key parmak izi doğrulama
    bool acceptHostKey();   // once / always
    bool rejectHostKey();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    void authenticationFailed(const QString& message);
    void hostKeyUnknown(const QString& fingerprint,
                         const QString& keyType);
    void hostKeyMismatch(const QString& fingerprint);

    // execute() sonuçları
    void commandOutput(int channelId, const QByteArray& data);
    void commandError(int channelId, const QByteArray& data);
    void commandFinished(int channelId, int exitCode);

    // SFTP ilerleme
    void transferProgress(qint64 sent, qint64 total);


private:
    void      doHandshake();
    void      doAuthenticate();
    bool      checkHostKey();
    void      autoAcceptHostKeySilently();
    void      pollChannels();
    void      setError(const QString& msg);
    QString   sshError() const;

    Config             m_config;
    State              m_state   = State::Disconnected;
    QString            m_lastError;
    QString            m_fingerprint;

    LIBSSH2_SESSION*   m_session = nullptr;
    LIBSSH2_KNOWNHOSTS*m_knownHosts = nullptr;

    // Aktif exec kanalları: id → channel
    int m_nextChannelId = 0;
    QMap<int, LIBSSH2_CHANNEL*> m_channels;

    // Socket FD (libssh2 için)
    int m_sockFd = -1;
    QSocketNotifier* m_notifier = nullptr;
};
