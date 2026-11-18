// common/crypto/AesGcm.cpp
#include "AesGcm.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

QByteArray AesGcm::encrypt(const QByteArray& data, const QByteArray& key) {
    if (key.size() != 32) return {};

    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, 
                           reinterpret_cast<const unsigned char*>(key.constData()), iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    QByteArray ciphertext(data.size(), 0);
    int outlen = 0;
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &outlen,
                          reinterpret_cast<const unsigned char*>(data.constData()), data.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    int finalLen = 0;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + outlen, &finalLen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    EVP_CIPHER_CTX_free(ctx);

    QByteArray result;
    result.append(reinterpret_cast<const char*>(iv), 12);
    result.append(reinterpret_cast<const char*>(tag), 16);
    result.append(ciphertext);
    return result;
}

QByteArray AesGcm::decrypt(const QByteArray& encryptedData, const QByteArray& key) {
    if (key.size() != 32 || encryptedData.size() < 28) return {};

    const unsigned char* iv = reinterpret_cast<const unsigned char*>(encryptedData.constData());
    const unsigned char* tag = reinterpret_cast<const unsigned char*>(encryptedData.constData()) + 12;
    const unsigned char* ciphertext = reinterpret_cast<const unsigned char*>(encryptedData.constData()) + 28;
    int ciphertextLen = encryptedData.size() - 28;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, 
                           reinterpret_cast<const unsigned char*>(key.constData()), iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    QByteArray plaintext(ciphertextLen, 0);
    int outlen = 0;
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &outlen,
                          ciphertext, ciphertextLen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    int finalLen = 0;
    int ret = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + outlen, &finalLen);
    
    EVP_CIPHER_CTX_free(ctx);

    if (ret > 0) {
        plaintext.resize(outlen + finalLen);
        return plaintext;
    } else {
        return {};
    }
}

QByteArray AesGcm::deriveKey(const QString& password, const QByteArray& salt) {
    QByteArray key(32, 0);
    QByteArray saltUsed = salt;
    if (saltUsed.isEmpty()) {
        saltUsed = "OkidiaDefaultSalt2026!";
    }

    PKCS5_PBKDF2_HMAC(
        password.toUtf8().constData(), password.toUtf8().size(),
        reinterpret_cast<const unsigned char*>(saltUsed.constData()), saltUsed.size(),
        10000, EVP_sha256(), 32,
        reinterpret_cast<unsigned char*>(key.data()));
    
    return key;
}

QByteArray AesGcm::defaultSystemKey() {
    // Statik anahtar türetme (basitlik için)
    return deriveKey("OkidiaSshSecret2026!", "SystemGlobalSalt");
}
