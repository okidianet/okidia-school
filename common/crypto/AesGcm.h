// common/crypto/AesGcm.h
#pragma once

#include <QByteArray>
#include <QString>

/**
 * AES-256-GCM (Authenticated Encryption) yardımcı sınıfı.
 * Hem Broker hem de Master tarafından kullanılır.
 */
class AesGcm {
public:
    /**
     * Veriyi şifreler.
     * Çıktı formatı: [IV (12 byte)] [Tag (16 byte)] [Ciphertext]
     */
    static QByteArray encrypt(const QByteArray& data, const QByteArray& key);

    /**
     * Şifrelenmiş veriyi çözer.
     * Beklenen format: [IV] [Tag] [Ciphertext]
     */
    static QByteArray decrypt(const QByteArray& encryptedData, const QByteArray& key);

    /**
     * Bir paroladan (ve opsiyonel salt'tan) 32-byte (256-bit) anahtar türetir.
     */
    static QByteArray deriveKey(const QString& password, const QByteArray& salt = QByteArray());

    /**
     * Sistem genelinde kullanılan varsayılan anahtar.
     * Not: Gerçek bir üretim ortamında bu anahtar master.key gibi bir dosyadan okunmalıdır.
     */
    static QByteArray defaultSystemKey();
};
