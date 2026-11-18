// common/crypto/KeyManager.cpp
#include "KeyManager.h"
#include "../utils/Logger.h"
#include "../utils/OkidiaUtils.h"

#include "AesGcm.h"

#include <QFile>
#include <QCryptographicHash>
#include <QtEndian>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#  include <dpapi.h>
#endif

#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <openssl/hmac.h>

// OpenSSL hata mesajını QString'e çevir
static QString opensslError() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

bool KeyManager::generateKeyPair(const QString& privatePath,
                                  const QString& publicPath) {
    // RSA context oluştur (Eski sistemlerde Ed25519 desteği olmadığı için RSA 2048 kullanıyoruz)
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) {
        Logger::error(QObject::tr("KeyManager: Context oluşturulamadı: ") + opensslError());
        return false;
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        Logger::error(QObject::tr("KeyManager: keygen_init hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
        Logger::error(QObject::tr("KeyManager: rsa_keygen_bits hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        Logger::error(QObject::tr("KeyManager: keygen hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    // Private key kaydet (OpenSSL BIO ile CRT uyumlu)
    BIO* privBio = BIO_new_file(privatePath.toLocal8Bit().constData(), "wb");
    if (!privBio) {
        Logger::error(QObject::tr("KeyManager: Private key dosyası açılamadı: ") + privatePath);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_bio_PrivateKey(privBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free_all(privBio);

    // Özel anahtarın dosya izinlerini sistem seviyesinde sertleştir (chmod 0600 / NTFS ACL)
    OkidiaUtils::securePrivateKeyFile(privatePath);

    // Public key kaydet (OpenSSL BIO ile CRT uyumlu)
    BIO* pubBio = BIO_new_file(publicPath.toLocal8Bit().constData(), "wb");
    if (!pubBio) {
        Logger::error(QObject::tr("KeyManager: Public key dosyası açılamadı: ") + publicPath);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_bio_PUBKEY(pubBio, pkey);
    BIO_free_all(pubBio);

    EVP_PKEY_free(pkey);
    Logger::info(QObject::tr("KeyManager: Keypair oluşturuldu → ") + privatePath);
    return true;
}

bool KeyManager::generateCaCertificate(const QString& caKeyPath,
                                      const QString& caCrtPath,
                                      const QString& commonName) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) {
        Logger::error(QObject::tr("KeyManager: CA Context oluşturulamadı: ") + opensslError());
        return false;
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 4096) <= 0) {
        Logger::error(QObject::tr("KeyManager: CA keygen_init hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        Logger::error(QObject::tr("KeyManager: CA keygen hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    X509* x509 = X509_new();
    X509_set_version(x509, 2); // X509 v3
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 3650L * 24L * 3600L); // 10 yıl
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char*)"TR", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"OKIDIA", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)commonName.toUtf8().constData(), -1, -1, 0);
    X509_set_subject_name(x509, name);
    X509_set_issuer_name(x509, name);
    X509_NAME_free(name);

    X509V3_CTX v3ctx;
    X509V3_set_ctx_nodb(&v3ctx);
    X509V3_set_ctx(&v3ctx, x509, x509, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints, const_cast<char*>("critical,CA:TRUE"));
    if (ext) {
        X509_add_ext(x509, ext, -1);
        X509_EXTENSION_free(ext);
    }

    if (X509_sign(x509, pkey, EVP_sha256()) <= 0) {
        Logger::error(QObject::tr("KeyManager: CA imzalanamadı: ") + opensslError());
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }

    BIO* keyBio = BIO_new_file(caKeyPath.toLocal8Bit().constData(), "wb");
    if (!keyBio) {
        Logger::error(QObject::tr("KeyManager: CA key dosyası açılamadı: ") + caKeyPath);
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free_all(keyBio);
    OkidiaUtils::securePrivateKeyFile(caKeyPath, true);

    BIO* crtBio = BIO_new_file(caCrtPath.toLocal8Bit().constData(), "wb");
    if (!crtBio) {
        Logger::error(QObject::tr("KeyManager: CA cert dosyası açılamadı: ") + caCrtPath);
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_bio_X509(crtBio, x509);
    BIO_free_all(crtBio);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    Logger::info(QObject::tr("KeyManager: Kök CA başarıyla üretildi → ") + caCrtPath);
    return true;
}

bool KeyManager::generateSignedCertificate(const QString& keyPath,
                                          const QString& crtPath,
                                          const QString& caKeyPath,
                                          const QString& caCrtPath,
                                          const QString& commonName) {
    EVP_PKEY* caKey = loadPrivateKey(caKeyPath);
    if (!caKey) {
        Logger::error(QObject::tr("KeyManager: CA private key açılamadı: ") + caKeyPath);
        return false;
    }

    BIO* caBio = BIO_new_file(caCrtPath.toLocal8Bit().constData(), "rb");
    if (!caBio) {
        Logger::error(QObject::tr("KeyManager: CA cert dosyası açılamadı: ") + caCrtPath);
        EVP_PKEY_free(caKey);
        return false;
    }
    X509* caCert = PEM_read_bio_X509(caBio, nullptr, nullptr, nullptr);
    BIO_free_all(caBio);
    if (!caCert) {
        Logger::error(QObject::tr("KeyManager: CA cert okunamadı: ") + caCrtPath);
        EVP_PKEY_free(caKey);
        return false;
    }

    // Sunucu anahtarını üret
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) {
        X509_free(caCert);
        EVP_PKEY_free(caKey);
        return false;
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        X509_free(caCert);
        EVP_PKEY_free(caKey);
        return false;
    }
    EVP_PKEY* serverKey = nullptr;
    if (EVP_PKEY_keygen(ctx, &serverKey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        X509_free(caCert);
        EVP_PKEY_free(caKey);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    X509* serverCert = X509_new();
    X509_set_version(serverCert, 2); // v3
    quint64 serialNum = 0;
    RAND_bytes((unsigned char*)&serialNum, sizeof(serialNum));
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(serverCert), serialNum & 0x7FFFFFFFFFFFFFFF);

    X509_gmtime_adj(X509_get_notBefore(serverCert), 0);
    X509_gmtime_adj(X509_get_notAfter(serverCert), 3650L * 24L * 3600L); // 10 yıl
    X509_set_pubkey(serverCert, serverKey);

    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char*)"TR", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"OKIDIA", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)commonName.toUtf8().constData(), -1, -1, 0);
    X509_set_subject_name(serverCert, name);
    X509_NAME_free(name);

    X509_set_issuer_name(serverCert, X509_get_subject_name(caCert));

    if (X509_sign(serverCert, caKey, EVP_sha256()) <= 0) {
        Logger::error(QObject::tr("KeyManager: Sunucu sertifikası imzalanamadı: ") + opensslError());
        X509_free(serverCert);
        EVP_PKEY_free(serverKey);
        X509_free(caCert);
        EVP_PKEY_free(caKey);
        return false;
    }

    BIO* keyBio = BIO_new_file(keyPath.toLocal8Bit().constData(), "wb");
    if (keyBio) {
        PEM_write_bio_PrivateKey(keyBio, serverKey, nullptr, nullptr, 0, nullptr, nullptr);
        BIO_free_all(keyBio);
        OkidiaUtils::securePrivateKeyFile(keyPath, true);
    }

    BIO* crtBio = BIO_new_file(crtPath.toLocal8Bit().constData(), "wb");
    if (crtBio) {
        PEM_write_bio_X509(crtBio, serverCert);
        BIO_free_all(crtBio);
    }

    X509_free(serverCert);
    EVP_PKEY_free(serverKey);
    X509_free(caCert);
    EVP_PKEY_free(caKey);
    Logger::info(QObject::tr("KeyManager: İmzalı sertifika oluşturuldu → ") + crtPath);
    return true;
}

QByteArray KeyManager::signCsr(const QByteArray& csrPem,
                              const QString& caKeyPath,
                              const QString& caCrtPath,
                              QString* outError) {
    BIO* csrBio = BIO_new_mem_buf(csrPem.constData(), csrPem.size());
    if (!csrBio) {
        if (outError) *outError = "CSR memory buffer oluşturulamadı";
        return {};
    }
    X509_REQ* req = PEM_read_bio_X509_REQ(csrBio, nullptr, nullptr, nullptr);
    BIO_free_all(csrBio);
    if (!req) {
        if (outError) *outError = "CSR okunamadı: " + opensslError();
        return {};
    }

    EVP_PKEY* caKey = loadPrivateKey(caKeyPath);
    if (!caKey) {
        X509_REQ_free(req);
        if (outError) *outError = "CA private key okunamadı: " + caKeyPath;
        return {};
    }

    BIO* caBio = BIO_new_file(caCrtPath.toLocal8Bit().constData(), "rb");
    if (!caBio) {
        X509_REQ_free(req);
        EVP_PKEY_free(caKey);
        if (outError) *outError = "CA cert okunamadı: " + caCrtPath;
        return {};
    }
    X509* caCert = PEM_read_bio_X509(caBio, nullptr, nullptr, nullptr);
    BIO_free_all(caBio);
    if (!caCert) {
        X509_REQ_free(req);
        EVP_PKEY_free(caKey);
        if (outError) *outError = "CA cert ayrıştırılamadı";
        return {};
    }

    EVP_PKEY* reqPubkey = X509_REQ_get_pubkey(req);
    if (!reqPubkey) {
        X509_REQ_free(req);
        X509_free(caCert);
        EVP_PKEY_free(caKey);
        if (outError) *outError = "CSR public key okunamadı";
        return {};
    }

    X509* cert = X509_new();
    X509_set_version(cert, 2); // v3

    quint64 serialNum = 0;
    RAND_bytes((unsigned char*)&serialNum, sizeof(serialNum));
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert), serialNum & 0x7FFFFFFFFFFFFFFF);

    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 3650L * 24L * 3600L); // 10 yıl

    X509_set_subject_name(cert, X509_REQ_get_subject_name(req));
    X509_set_issuer_name(cert, X509_get_subject_name(caCert));
    X509_set_pubkey(cert, reqPubkey);

    if (X509_sign(cert, caKey, EVP_sha256()) <= 0) {
        if (outError) *outError = "Sertifika imzalanamadı: " + opensslError();
        EVP_PKEY_free(reqPubkey);
        X509_free(cert);
        X509_free(caCert);
        EVP_PKEY_free(caKey);
        X509_REQ_free(req);
        return {};
    }

    BIO* outBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(outBio, cert);

    char* buf = nullptr;
    long len = BIO_get_mem_data(outBio, &buf);
    QByteArray result(buf, len);

    BIO_free_all(outBio);
    EVP_PKEY_free(reqPubkey);
    X509_free(cert);
    X509_free(caCert);
    EVP_PKEY_free(caKey);
    X509_REQ_free(req);

    return result;
}

bool KeyManager::generateCsr(const QString& privatePath,
                             const QString& csrPath,
                             const QString& commonName) {
    EVP_PKEY* pkey = loadPrivateKey(privatePath);
    if (!pkey) {
        Logger::error(QObject::tr("KeyManager: CSR için private key yüklenemedi: ") + privatePath);
        return false;
    }

    X509_REQ* req = X509_REQ_new();
    if (!req) {
        Logger::error(QObject::tr("KeyManager: X509_REQ oluşturulamadı: ") + opensslError());
        EVP_PKEY_free(pkey);
        return false;
    }

    X509_REQ_set_version(req, 0); // version 1
    X509_REQ_set_pubkey(req, pkey);

    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)commonName.toUtf8().constData(), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"OKIDIA", -1, -1, 0);
    X509_REQ_set_subject_name(req, name);
    X509_NAME_free(name);

    if (X509_REQ_sign(req, pkey, EVP_sha256()) <= 0) {
        Logger::error(QObject::tr("KeyManager: CSR imzalanamadı: ") + opensslError());
        X509_REQ_free(req);
        EVP_PKEY_free(pkey);
        return false;
    }

    BIO* csrBio = BIO_new_file(csrPath.toLocal8Bit().constData(), "wb");
    if (!csrBio) {
        Logger::error(QObject::tr("KeyManager: CSR dosyası açılamadı: ") + csrPath);
        X509_REQ_free(req);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_bio_X509_REQ(csrBio, req);
    BIO_free_all(csrBio);

    X509_REQ_free(req);
    EVP_PKEY_free(pkey);
    Logger::info(QObject::tr("KeyManager: CSR başarıyla oluşturuldu → ") + csrPath);
    return true;
}

EVP_PKEY* KeyManager::loadPrivateKey(const QString& path) {
    BIO* bio = BIO_new_file(path.toLocal8Bit().constData(), "rb");
    if (!bio) {
        Logger::error(QObject::tr("KeyManager: Private key dosyası açılamadı: ") + path);
        return nullptr;
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);
    if (!pkey) {
        Logger::error(QObject::tr("KeyManager: Private key okunamadı: ") + opensslError());
    }
    return pkey;
}

EVP_PKEY* KeyManager::loadPublicKey(const QString& path) {
    BIO* bio = BIO_new_file(path.toLocal8Bit().constData(), "rb");
    if (!bio) {
        Logger::error(QObject::tr("KeyManager: Public key dosyası açılamadı: ") + path);
        return nullptr;
    }
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);
    if (!pkey) {
        Logger::error(QObject::tr("KeyManager: Public key okunamadı: ") + opensslError());
    }
    return pkey;
}

QByteArray KeyManager::signChallenge(const QByteArray& challenge,
                                      const QString& privatePath) {
    EVP_PKEY* pkey = loadPrivateKey(privatePath);
    if (!pkey) return {};

    EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();
    if (!mdCtx) {
        EVP_PKEY_free(pkey);
        return {};
    }

    if (EVP_DigestSignInit(mdCtx, nullptr, nullptr, nullptr, pkey) <= 0) {
        Logger::error(QObject::tr("KeyManager: DigestSignInit hatası: ") + opensslError());
        EVP_MD_CTX_free(mdCtx);
        EVP_PKEY_free(pkey);
        return {};
    }

    // İmza boyutunu öğren
    size_t sigLen = 0;
    if (EVP_DigestSign(mdCtx, nullptr, &sigLen,
                       reinterpret_cast<const unsigned char*>(challenge.constData()),
                       challenge.size()) <= 0) {
        Logger::error(QObject::tr("KeyManager: DigestSign (boyut) hatası: ") + opensslError());
        EVP_MD_CTX_free(mdCtx);
        EVP_PKEY_free(pkey);
        return {};
    }

    QByteArray signature(static_cast<int>(sigLen), 0);
    if (EVP_DigestSign(mdCtx,
                       reinterpret_cast<unsigned char*>(signature.data()),
                       &sigLen,
                       reinterpret_cast<const unsigned char*>(challenge.constData()),
                       challenge.size()) <= 0) {
        Logger::error(QObject::tr("KeyManager: DigestSign hatası: ") + opensslError());
        EVP_MD_CTX_free(mdCtx);
        EVP_PKEY_free(pkey);
        return {};
    }

    EVP_MD_CTX_free(mdCtx);
    EVP_PKEY_free(pkey);
    return signature;
}

bool KeyManager::verifySignature(const QByteArray& challenge,
                                  const QByteArray& signature,
                                  const QString& publicPath) {
    EVP_PKEY* pkey = loadPublicKey(publicPath);
    if (!pkey) return false;

    EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();
    if (!mdCtx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    if (EVP_DigestVerifyInit(mdCtx, nullptr, nullptr, nullptr, pkey) <= 0) {
        Logger::error(QObject::tr("KeyManager: DigestVerifyInit hatası: ") + opensslError());
        EVP_MD_CTX_free(mdCtx);
        EVP_PKEY_free(pkey);
        return false;
    }

    int result = EVP_DigestVerify(
        mdCtx,
        reinterpret_cast<const unsigned char*>(signature.constData()),
        signature.size(),
        reinterpret_cast<const unsigned char*>(challenge.constData()),
        challenge.size()
    );

    EVP_MD_CTX_free(mdCtx);
    EVP_PKEY_free(pkey);

    if (result != 1) {
        Logger::warning(QObject::tr("KeyManager: İmza doğrulama başarısız"));
        return false;
    }
    return true;
}

bool KeyManager::verifyCertificateChain(const QByteArray& certPem, const QString& caCertPath) {
    QFile caFile(caCertPath);
    if (!caFile.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("KeyManager: CA sertifikası okunamadı: ") + caCertPath);
        return false;
    }
    QByteArray caPem = caFile.readAll();
    caFile.close();

    BIO* caBio = BIO_new_mem_buf(caPem.constData(), caPem.size());
    X509* caCert = PEM_read_bio_X509(caBio, nullptr, nullptr, nullptr);
    BIO_free(caBio);
    if (!caCert) {
        Logger::error(QObject::tr("KeyManager: CA sertifikası ayrıştırılamadı."));
        return false;
    }

    BIO* certBio = BIO_new_mem_buf(certPem.constData(), certPem.size());
    X509* childCert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO_free(certBio);
    if (!childCert) {
        Logger::error(QObject::tr("KeyManager: Uç sertifika ayrıştırılamadı."));
        X509_free(caCert);
        return false;
    }

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, caCert);

    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, childCert, nullptr);
    int result = X509_verify_cert(ctx);
    
    if (result != 1) {
        int err = X509_STORE_CTX_get_error(ctx);
        if (err == X509_V_ERR_CERT_NOT_YET_VALID || err == X509_V_ERR_CERT_HAS_EXPIRED) {
            EVP_PKEY* caPubKey = X509_get_pubkey(caCert);
            if (caPubKey) {
                if (X509_verify(childCert, caPubKey) == 1) {
                    result = 1;
                    Logger::info(QObject::tr("KeyManager: Sertifika tarihi uyumsuz fakat CA imzası geçerli (saat kayması tolere edildi)."));
                }
                EVP_PKEY_free(caPubKey);
            }
        }
        if (result != 1) {
            Logger::warning(QObject::tr("KeyManager: Sertifika doğrulama başarısız: %1")
                            .arg(X509_verify_cert_error_string(err)));
        }
    }

    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(childCert);
    X509_free(caCert);
    
    return (result == 1);
}

bool KeyManager::matchCsrWithCertificate(const QByteArray& csrPem, const QByteArray& certPem) {
    if (csrPem.isEmpty() || certPem.isEmpty()) return false;

    BIO* csrBio = BIO_new_mem_buf(csrPem.constData(), csrPem.size());
    if (!csrBio) return false;
    X509_REQ* req = PEM_read_bio_X509_REQ(csrBio, nullptr, nullptr, nullptr);
    BIO_free_all(csrBio);
    if (!req) return false;

    BIO* certBio = BIO_new_mem_buf(certPem.constData(), certPem.size());
    if (!certBio) {
        X509_REQ_free(req);
        return false;
    }
    X509* cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO_free_all(certBio);
    if (!cert) {
        X509_REQ_free(req);
        return false;
    }

    EVP_PKEY* reqPub = X509_REQ_get_pubkey(req);
    EVP_PKEY* certPub = X509_get_pubkey(cert);

    bool match = false;
    if (reqPub && certPub) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        match = (EVP_PKEY_eq(reqPub, certPub) == 1);
#else
        match = (EVP_PKEY_cmp(reqPub, certPub) == 1);
#endif
    }

    if (reqPub)  EVP_PKEY_free(reqPub);
    if (certPub) EVP_PKEY_free(certPub);
    X509_REQ_free(req);
    X509_free(cert);

    return match;
}

qint64 KeyManager::getCertificateNotBefore(const QString& certPath) {
    BIO* bio = BIO_new_file(certPath.toLocal8Bit().constData(), "rb");
    if (!bio) return 0;

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);
    if (!cert) return 0;

    const ASN1_TIME* notBefore = X509_get0_notBefore(cert);
    if (!notBefore) {
        X509_free(cert);
        return 0;
    }

    struct tm tmTime = {};
    if (ASN1_TIME_to_tm(notBefore, &tmTime) != 1) {
        X509_free(cert);
        return 0;
    }
    X509_free(cert);

#if defined(Q_OS_WIN)
    time_t t = _mkgmtime(&tmTime);
#else
    time_t t = timegm(&tmTime);
#endif
    return (t > 0) ? static_cast<qint64>(t) : 0;
}

qint64 KeyManager::getCertificateNotAfter(const QString& certPath) {
    BIO* bio = BIO_new_file(certPath.toLocal8Bit().constData(), "rb");
    if (!bio) return 0;

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);
    if (!cert) return 0;

    const ASN1_TIME* notAfter = X509_get0_notAfter(cert);
    if (!notAfter) {
        X509_free(cert);
        return 0;
    }

    struct tm tmTime = {};
    if (ASN1_TIME_to_tm(notAfter, &tmTime) != 1) {
        X509_free(cert);
        return 0;
    }
    X509_free(cert);

#if defined(Q_OS_WIN)
    time_t t = _mkgmtime(&tmTime);
#else
    time_t t = timegm(&tmTime);
#endif
    return (t > 0) ? static_cast<qint64>(t) : 0;
}

int KeyManager::getCertificateDaysRemaining(const QString& certPath) {
    qint64 notAfter = getCertificateNotAfter(certPath);
    if (notAfter <= 0) return 0;

    qint64 now = OkidiaUtils::nowSecs();
    if (now >= notAfter) return 0;

    return static_cast<int>((notAfter - now) / 86400);
}

bool KeyManager::verifySignatureWithCert(const QByteArray& challenge,
                                         const QByteArray& signature,
                                         const QByteArray& certPem) {
    BIO* certBio = BIO_new_mem_buf(certPem.constData(), certPem.size());
    X509* cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO_free(certBio);
    if (!cert) {
        Logger::error(QObject::tr("KeyManager: İmza için sertifika ayrıştırılamadı."));
        return false;
    }

    EVP_PKEY* pkey = X509_get_pubkey(cert);
    X509_free(cert);
    if (!pkey) {
        Logger::error(QObject::tr("KeyManager: Sertifikadan Public Key çıkarılamadı."));
        return false;
    }

    EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();
    if (!mdCtx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    if (EVP_DigestVerifyInit(mdCtx, nullptr, nullptr, nullptr, pkey) <= 0) {
        Logger::error(QObject::tr("KeyManager: DigestVerifyInit hatası: ") + opensslError());
        EVP_MD_CTX_free(mdCtx);
        EVP_PKEY_free(pkey);
        return false;
    }

    int result = EVP_DigestVerify(
        mdCtx,
        reinterpret_cast<const unsigned char*>(signature.constData()),
        signature.size(),
        reinterpret_cast<const unsigned char*>(challenge.constData()),
        challenge.size()
    );

    EVP_MD_CTX_free(mdCtx);
    EVP_PKEY_free(pkey);

    if (result != 1) {
        Logger::warning(QObject::tr("KeyManager: Sertifika ile imza doğrulama başarısız"));
        return false;
    }
    return true;
}

QString KeyManager::getFingerprint(const QString& publicPath) {
    QFile file(publicPath);
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("KeyManager: Public key okunamadı: ") + publicPath);
        return {};
    }
    QByteArray data = file.readAll();
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    // AA:BB:CC:... formatında döndür
    QString fp;
    for (int i = 0; i < hash.size(); ++i) {
        if (i > 0) fp += ':';
        fp += QString("%1").arg(
            static_cast<unsigned char>(hash[i]), 2, 16, QChar('0')).toUpper();
    }
    return fp;
}

QByteArray KeyManager::generateChallenge() {
    QByteArray nonce(32, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), 32) != 1) {
        Logger::error(QObject::tr("KeyManager: Rastgele nonce üretilemedi"));
        return {};
    }
    return nonce;
}

bool KeyManager::generateEcdhKeyPair(EVP_PKEY** outPrivateKey, QByteArray& outPublicKey) {
    if (!outPrivateKey) return false;
    
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) {
        Logger::error(QObject::tr("KeyManager: ECDH Context oluşturulamadı: ") + opensslError());
        return false;
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        Logger::error(QObject::tr("KeyManager: ECDH keygen_init hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        Logger::error(QObject::tr("KeyManager: ECDH keygen hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    unsigned char pubKey[32];
    size_t len = 32;
    if (EVP_PKEY_get_raw_public_key(pkey, pubKey, &len) <= 0) {
        Logger::error(QObject::tr("KeyManager: ECDH get_raw_public_key hatası: ") + opensslError());
        EVP_PKEY_free(pkey);
        return false;
    }

    outPublicKey = QByteArray(reinterpret_cast<char*>(pubKey), 32);
    *outPrivateKey = pkey;
    return true;
}

QByteArray KeyManager::deriveSharedSecret(EVP_PKEY* privateKey, const QByteArray& peerPublicKeyBytes) {
    if (!privateKey || peerPublicKeyBytes.size() != 32) return {};

    EVP_PKEY* peerPubKey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr,
        reinterpret_cast<const unsigned char*>(peerPublicKeyBytes.constData()),
        peerPublicKeyBytes.size()
    );
    if (!peerPubKey) {
        Logger::error(QObject::tr("KeyManager: Peer public key yüklenemedi: ") + opensslError());
        return {};
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(privateKey, nullptr);
    if (!ctx) {
        EVP_PKEY_free(peerPubKey);
        return {};
    }

    if (EVP_PKEY_derive_init(ctx) <= 0) {
        Logger::error(QObject::tr("KeyManager: derive_init hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peerPubKey);
        return {};
    }

    if (EVP_PKEY_derive_set_peer(ctx, peerPubKey) <= 0) {
        Logger::error(QObject::tr("KeyManager: derive_set_peer hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peerPubKey);
        return {};
    }

    size_t secretLen = 0;
    if (EVP_PKEY_derive(ctx, nullptr, &secretLen) <= 0) {
        Logger::error(QObject::tr("KeyManager: derive size hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peerPubKey);
        return {};
    }

    QByteArray secretBytes(static_cast<int>(secretLen), 0);
    if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(secretBytes.data()), &secretLen) <= 0) {
        Logger::error(QObject::tr("KeyManager: derive hatası: ") + opensslError());
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peerPubKey);
        return {};
    }

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peerPubKey);

    return QCryptographicHash::hash(secretBytes, QCryptographicHash::Sha256);
}

bool KeyManager::encryptAesGcm(const QByteArray& plaintext, const QByteArray& key, const QByteArray& iv,
                              QByteArray& ciphertext, QByteArray& tag) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, 
                           reinterpret_cast<const unsigned char*>(key.constData()), 
                           reinterpret_cast<const unsigned char*>(iv.constData())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    ciphertext.resize(plaintext.size());
    int len = 0;
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &len,
                           reinterpret_cast<const unsigned char*>(plaintext.constData()), plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int ciphertextLen = len;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    ciphertextLen += len;
    ciphertext.resize(ciphertextLen);

    tag.resize(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

bool KeyManager::decryptAesGcm(const QByteArray& ciphertext, const QByteArray& key, const QByteArray& iv,
                              const QByteArray& tag, QByteArray& plaintext) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, 
                           reinterpret_cast<const unsigned char*>(key.constData()), 
                           reinterpret_cast<const unsigned char*>(iv.constData())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    plaintext.resize(ciphertext.size());
    int len = 0;
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &len,
                           reinterpret_cast<const unsigned char*>(ciphertext.constData()), ciphertext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int plaintextLen = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<char*>(tag.constData())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int ret = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret > 0) {
        plaintextLen += len;
        plaintext.resize(plaintextLen);
        return true;
    }
    return false;
}

QByteArray KeyManager::computeHmacSha256(const QByteArray& key, const QByteArray& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int resultLen = 0;

    HMAC(EVP_sha256(),
         key.constData(), key.size(),
         reinterpret_cast<const unsigned char*>(data.constData()), data.size(),
         result, &resultLen);

    return QByteArray(reinterpret_cast<const char*>(result), resultLen);
}

QString KeyManager::generatePinFromSeed(const QByteArray& seedBytes, const QString& nonce, qint64 timeWindow) {
    if (seedBytes.isEmpty()) return QStringLiteral("000000");

    // data = Nonce (UTF-8) + Big-Endian 64-bit timeWindow
    QByteArray data = nonce.toUtf8();
    quint64 beWindow = qToBigEndian<quint64>(static_cast<quint64>(timeWindow));
    data.append(reinterpret_cast<const char*>(&beWindow), sizeof(beWindow));

    QByteArray hmac = computeHmacSha256(seedBytes, data);
    if (hmac.size() < 4) return QStringLiteral("000000");

    // Dynamic truncation (RFC 4226 / HOTP standardı)
    int offset = hmac.at(hmac.size() - 1) & 0x0F;
    if (offset + 4 > hmac.size()) offset = hmac.size() - 4;

    quint32 binaryCode =
        ((static_cast<quint8>(hmac.at(offset))     & 0x7F) << 24) |
        ((static_cast<quint8>(hmac.at(offset + 1)) & 0xFF) << 16) |
        ((static_cast<quint8>(hmac.at(offset + 2)) & 0xFF) << 8)  |
        ( static_cast<quint8>(hmac.at(offset + 3)) & 0xFF);

    quint32 pin = binaryCode % 1000000;
    return QString("%1").arg(pin, 6, 10, QChar('0'));
}

QByteArray KeyManager::protectDataDpapi(const QByteArray& data, const QString& description) {
    if (data.isEmpty()) return QByteArray();
#if defined(Q_OS_WIN)
    DATA_BLOB dataIn;
    dataIn.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(data.constData()));
    dataIn.cbData = static_cast<DWORD>(data.size());

    DATA_BLOB dataOut = {};
    LPCWSTR descW = description.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(description.utf16());

    if (CryptProtectData(&dataIn, descW, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        QByteArray encrypted(reinterpret_cast<const char*>(dataOut.pbData), static_cast<int>(dataOut.cbData));
        LocalFree(dataOut.pbData);
        return encrypted;
    } else {
        Logger::error(QObject::tr("KeyManager: CryptProtectData başarısız (Win32 Hata: %1)").arg(GetLastError()));
        return QByteArray();
    }
#else
    Q_UNUSED(description);
    // Linux/Unix fallback: AES-GCM ile sistem düzeyinde koruma
    return AesGcm::encrypt(data, AesGcm::defaultSystemKey());
#endif
}

QByteArray KeyManager::unprotectDataDpapi(const QByteArray& encryptedData) {
    if (encryptedData.isEmpty()) return QByteArray();
#if defined(Q_OS_WIN)
    DATA_BLOB dataIn;
    dataIn.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(encryptedData.constData()));
    dataIn.cbData = static_cast<DWORD>(encryptedData.size());

    DATA_BLOB dataOut = {};
    LPWSTR pDescr = nullptr;

    if (CryptUnprotectData(&dataIn, &pDescr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        QByteArray decrypted(reinterpret_cast<const char*>(dataOut.pbData), static_cast<int>(dataOut.cbData));
        LocalFree(dataOut.pbData);
        if (pDescr) LocalFree(pDescr);
        return decrypted;
    } else {
        Logger::error(QObject::tr("KeyManager: CryptUnprotectData başarısız (Win32 Hata: %1)").arg(GetLastError()));
        return QByteArray();
    }
#else
    // Linux/Unix fallback: AES-GCM decrypt
    return AesGcm::decrypt(encryptedData, AesGcm::defaultSystemKey());
#endif
}