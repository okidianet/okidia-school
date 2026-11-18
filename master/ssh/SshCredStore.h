// master/ssh/SshCredStore.h
#pragma once

#include <QString>
#include <QByteArray>

class SshCredStore {
public:
    static SshCredStore& instance();

    // Kimlik bilgilerini kaydet (target: IP veya MachineID)
    void saveCredentials(const QString& target, const QString& user, const QString& pass);

    // Kimlik bilgilerini çek
    bool getCredentials(const QString& target, QString& user, QString& pass);

private:
    SshCredStore() = default;
    ~SshCredStore() = default;
    SshCredStore(const SshCredStore&) = delete;
    SshCredStore& operator=(const SshCredStore&) = delete;

    // AES-256-CBC Şifreleme / Çözme yardımcıları
    QByteArray encrypt(const QByteArray& data);
    QByteArray decrypt(const QByteArray& data);

    QByteArray deriveKey(const QByteArray& salt);

    static constexpr const char* kSecretPass = "OkidiaSshSecret2026!";
};
