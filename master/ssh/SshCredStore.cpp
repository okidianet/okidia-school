#include "SshCredStore.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <QDataStream>

SshCredStore& SshCredStore::instance() {
    static SshCredStore s;
    return s;
}

void SshCredStore::saveCredentials(const QString& target, const QString& user, const QString& pass) {
    if (target.isEmpty()) return;

    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << user << pass;

    QByteArray encrypted = encrypt(payload);
    OkidiaUtils::secureWipe(payload);
    if (encrypted.isEmpty()) {
        Logger::error(QObject::tr("SshCredStore: Sifreleme basarisiz: ") + target);
        return;
    }

    QString base64 = encrypted.toBase64();
    OkidiaDatabase::instance().setMetaValue("ssh_cred_" + target, base64);
}

bool SshCredStore::getCredentials(const QString& target, QString& user, QString& pass) {
    if (target.isEmpty()) return false;

    QString base64 = OkidiaDatabase::instance().metaValue("ssh_cred_" + target);
    if (base64.isEmpty()) return false;

    QByteArray encrypted = QByteArray::fromBase64(base64.toUtf8());
    QByteArray payload = decrypt(encrypted);
    if (payload.isEmpty()) {
        return false;
    }

    QDataStream in(&payload, QIODevice::ReadOnly);
    in.setVersion(QDataStream::Qt_6_0);
    in >> user >> pass;
    OkidiaUtils::secureWipe(payload);
    return true;
}

QByteArray SshCredStore::deriveKey(const QByteArray& salt) {
    QByteArray key(32, 0);
    PKCS5_PBKDF2_HMAC(
        kSecretPass, -1,
        reinterpret_cast<const unsigned char*>(salt.constData()),
        salt.size(),
        10000, // 10k iterasyon yeterli ve hizlidir
        EVP_sha256(),
        32,
        reinterpret_cast<unsigned char*>(key.data()));
    return key;
}

QByteArray SshCredStore::encrypt(const QByteArray& data) {
    QByteArray iv(16, 0);
    QByteArray salt(32, 0);
    RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), 16);
    RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), 32);

    QByteArray key = deriveKey(salt);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()),
        reinterpret_cast<const unsigned char*>(iv.constData()));

    QByteArray out(data.size() + 16, 0);
    int outLen = 0, finalLen = 0;

    EVP_EncryptUpdate(ctx,
        reinterpret_cast<unsigned char*>(out.data()), &outLen,
        reinterpret_cast<const unsigned char*>(data.constData()),
        data.size());

    EVP_EncryptFinal_ex(ctx,
        reinterpret_cast<unsigned char*>(out.data()) + outLen,
        &finalLen);

    EVP_CIPHER_CTX_free(ctx);
    OkidiaUtils::secureWipe(key);
    out.resize(outLen + finalLen);

    // Paketliyoruz: [IV] [SALT] [PAYLOAD]
    QByteArray packed;
    QDataStream pStream(&packed, QIODevice::WriteOnly);
    pStream.setVersion(QDataStream::Qt_6_0);
    pStream << iv << salt << out;
    return packed;
}

QByteArray SshCredStore::decrypt(const QByteArray& data) {
    QDataStream pStream(data);
    pStream.setVersion(QDataStream::Qt_6_0);
    QByteArray iv, salt, cipher;
    pStream >> iv >> salt >> cipher;

    if (iv.size() != 16 || salt.size() != 32 || cipher.isEmpty()) {
        return {};
    }

    QByteArray key = deriveKey(salt);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()),
        reinterpret_cast<const unsigned char*>(iv.constData()));

    QByteArray out(cipher.size(), 0);
    int outLen = 0, finalLen = 0;

    if (!EVP_DecryptUpdate(ctx,
            reinterpret_cast<unsigned char*>(out.data()), &outLen,
            reinterpret_cast<const unsigned char*>(cipher.constData()),
            cipher.size())) {
        EVP_CIPHER_CTX_free(ctx);
        OkidiaUtils::secureWipe(key);
        return {};
    }

    if (!EVP_DecryptFinal_ex(ctx,
            reinterpret_cast<unsigned char*>(out.data()) + outLen,
            &finalLen)) {
        EVP_CIPHER_CTX_free(ctx);
        OkidiaUtils::secureWipe(key);
        return {};
    }

    EVP_CIPHER_CTX_free(ctx);
    OkidiaUtils::secureWipe(key);
    out.resize(outLen + finalLen);
    return out;
}
