// common/crypto/KeyManager.h
#pragma once

#include <QByteArray>
#include <QString>
#include <openssl/evp.h>

class KeyManager {
public:
    // Ed25519/RSA keypair üret
    static bool generateKeyPair(const QString& privatePath,
                                const QString& publicPath);

    // Kök CA Sertifikası Üret (Self-signed Root CA)
    static bool generateCaCertificate(const QString& caKeyPath,
                                      const QString& caCrtPath,
                                      const QString& commonName = "OKIDIA Root CA");

    // Kök CA tarafından imzalanmış Sunucu/Broker Sertifikası Üret
    static bool generateSignedCertificate(const QString& keyPath,
                                          const QString& crtPath,
                                          const QString& caKeyPath,
                                          const QString& caCrtPath,
                                          const QString& commonName = "OKIDIA Broker");

    // Bellekte CSR İmzala (C++ OpenSSL, dosyasız ve platform bağımsız)
    static QByteArray signCsr(const QByteArray& csrPem,
                              const QString& caKeyPath,
                              const QString& caCrtPath,
                              QString* outError = nullptr);

    // CSR (Certificate Signing Request) üret
    static bool generateCsr(const QString& privatePath,
                            const QString& csrPath,
                            const QString& commonName);

    // Private key ile challenge imzala
    static QByteArray signChallenge(const QByteArray& challenge,
                                    const QString& privatePath);

    // Public key ile imzayı doğrula
    static bool verifySignature(const QByteArray& challenge,
                                const QByteArray& signature,
                                const QString& publicPath);

    // Sertifika zincir doğrulaması
    static bool verifyCertificateChain(const QByteArray& certPem, const QString& caCertPath);

    // CSR açık anahtarı ile X.509 sertifikasının açık anahtarını karşılaştırır (EVP_PKEY_eq)
    static bool matchCsrWithCertificate(const QByteArray& csrPem, const QByteArray& certPem);

    // Sertifikanın geçerlilik başlangıç (notBefore) UTC zamanını döndür
    static qint64 getCertificateNotBefore(const QString& certPath);

    // Sertifikanın geçerlilik bitiş (notAfter) UTC zamanını döndür
    static qint64 getCertificateNotAfter(const QString& certPath);

    // Sertifikanın kalan gün sayısını döndür (geçersiz/süresi dolmuş ise 0 veya negatif)
    static int getCertificateDaysRemaining(const QString& certPath);

    // Sertifika ile imzayı doğrula
    static bool verifySignatureWithCert(const QByteArray& challenge,
                                        const QByteArray& signature,
                                        const QByteArray& certPem);

    // Public key fingerprint (UI'da göstermek için)
    static QString getFingerprint(const QString& publicPath);

    // Rastgele challenge üret (32 byte nonce)
    static QByteArray generateChallenge();

    // Ephemeral X25519 keypair üret ve public key'i ham byte dizisi olarak döndür
    static bool generateEcdhKeyPair(EVP_PKEY** outPrivateKey, QByteArray& outPublicKey);

    // Shared secret hesapla ve SHA-256 ile 256-bit symmetric key üret
    static QByteArray deriveSharedSecret(EVP_PKEY* privateKey, const QByteArray& peerPublicKeyBytes);

    // AES-256-GCM Şifreleme
    static bool encryptAesGcm(const QByteArray& plaintext, const QByteArray& key, const QByteArray& iv,
                              QByteArray& ciphertext, QByteArray& tag);

    // AES-256-GCM Şifre Çözme
    static bool decryptAesGcm(const QByteArray& ciphertext, const QByteArray& key, const QByteArray& iv,
                              const QByteArray& tag, QByteArray& plaintext);

    // HMAC-SHA256 Hesaplama (Air-Gapped QR-PIN için)
    static QByteArray computeHmacSha256(const QByteArray& key, const QByteArray& data);

    // 6 Haneli Nümerik PIN Üret (HMAC tabanlı TOTP/HOTP)
    static QString generatePinFromSeed(const QByteArray& seedBytes, const QString& nonce, qint64 timeWindow);

    // Windows DPAPI (CryptProtectData / CryptUnprotectData) ile veri koruma
    static QByteArray protectDataDpapi(const QByteArray& data, const QString& description = QString());
    static QByteArray unprotectDataDpapi(const QByteArray& encryptedData);

private:
    // EVP_PKEY yükle (private veya public)
    static EVP_PKEY* loadPrivateKey(const QString& path);
    static EVP_PKEY* loadPublicKey(const QString& path);
};