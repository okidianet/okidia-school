// common/crypto/BackupPackage.cpp
#include "BackupPackage.h"
#include "AesGcm.h"
#include "../utils/Logger.h"
#include "../utils/OkidiaUtils.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDataStream>
#include <QCryptographicHash>
#include <openssl/rand.h>

QByteArray BackupPackage::pack(const QString& password, const BackupContent& content) {
    if (password.isEmpty()) {
        Logger::error(QObject::tr("BackupPackage::pack: Parola boş olamaz."));
        return {};
    }

    // 1. İçeriği serialize et
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_6_0);

    ds << QString("OKBK_PAYLOAD");
    ds << content.meta.version;
    ds << content.meta.timestamp;
    ds << content.meta.creator;
    ds << content.meta.description;
    ds << content.databaseData;
    ds << content.auditData;
    ds << content.chatData;

    // Sertifika dosyaları
    ds << static_cast<quint32>(content.certFiles.size());
    for (auto it = content.certFiles.cbegin(); it != content.certFiles.cend(); ++it) {
        ds << it.key() << it.value();
    }

    // 2. Rastgele 32 byte Salt üret
    QByteArray salt(32, 0);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), 32) != 1) {
        Logger::error(QObject::tr("BackupPackage::pack: OpenSSL RAND_bytes başarısız."));
        return {};
    }

    // 3. PBKDF2 ile anahtar türet ve AES-256-GCM ile şifrele
    QByteArray key = AesGcm::deriveKey(password, salt);
    QByteArray encrypted = AesGcm::encrypt(payload, key);
    if (encrypted.isEmpty()) {
        Logger::error(QObject::tr("BackupPackage::pack: AES-256-GCM şifreleme başarısız."));
        return {};
    }

    // 4. Şifreli veri üzerinden SHA-256 Checksum
    QByteArray checksum = QCryptographicHash::hash(encrypted, QCryptographicHash::Sha256);

    // 5. Nihai .okbak paketini oluştur
    QByteArray package;
    QDataStream writer(&package, QIODevice::WriteOnly);
    writer.setVersion(QDataStream::Qt_6_0);

    writer.writeRawData("OKBK", 4);
    writer << static_cast<quint32>(CURRENT_VERSION);
    writer << salt;
    writer << OkidiaUtils::nowSecs();
    writer << encrypted;
    writer << checksum;

    return package;
}

bool BackupPackage::packToFile(const QString& filePath, const QString& password, const BackupContent& content, QString* outError) {
    QFileInfo fi(filePath);
    QDir().mkpath(fi.absolutePath());

    QByteArray package = pack(password, content);
    if (package.isEmpty()) {
        if (outError) *outError = QObject::tr("Yedek paketi şifrelenemedi veya oluşturulamadı.");
        return false;
    }

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        QString err = QObject::tr("Yedek dosyası yazılamadı: %1").arg(filePath);
        Logger::error(QObject::tr("BackupPackage::packToFile: ") + err);
        if (outError) *outError = err;
        return false;
    }

    if (f.write(package) != package.size()) {
        f.close();
        QString err = QObject::tr("Dosyaya tam veri yazılamadı.");
        Logger::error(QObject::tr("BackupPackage::packToFile: ") + err);
        if (outError) *outError = err;
        return false;
    }
    f.close();

    Logger::info(QObject::tr("BackupPackage::packToFile: Tek dosya yedek paketi oluşturuldu → ") + filePath +
                 " (" + QString::number(package.size() / 1024) + QObject::tr(" KB)"));
    return true;
}

bool BackupPackage::unpack(const QByteArray& packageData, const QString& password, BackupContent& outContent, QString* outError) {
    if (packageData.size() < 4 + 4 + 32 + 8) {
        QString err = QObject::tr("Geçersiz veya eksik yedek dosyası boyutu.");
        if (outError) *outError = err;
        return false;
    }

    QDataStream reader(packageData);
    reader.setVersion(QDataStream::Qt_6_0);

    // Magic Header Kontrolü
    char magic[4];
    if (reader.readRawData(magic, 4) != 4 || QByteArray(magic, 4) != "OKBK") {
        QString err = QObject::tr("Geçersiz yedek dosya formatı (Magic 'OKBK' eşleşmedi).");
        Logger::error(QObject::tr("BackupPackage::unpack: ") + err);
        if (outError) *outError = err;
        return false;
    }

    quint32 version = 0;
    QByteArray salt, encrypted, checksum;
    qint64 timestamp = 0;

    reader >> version;
    if (version == 3 || version == 2) {
        reader >> salt >> timestamp >> encrypted >> checksum;
    } else if (version == 1) {
        // Legacy v1 formatı: [IV (16)] [Salt (32)] [Timestamp (8)] [Encrypted] [Checksum]
        QByteArray iv;
        reader >> iv >> salt >> timestamp >> encrypted >> checksum;
    } else {
        QString err = QObject::tr("Desteklenmeyen yedek format versiyonu: %1").arg(version);
        Logger::error(QObject::tr("BackupPackage::unpack: ") + err);
        if (outError) *outError = err;
        return false;
    }

    // Checksum Doğrulama
    QByteArray expectedChecksum = QCryptographicHash::hash(encrypted, QCryptographicHash::Sha256);
    if (expectedChecksum != checksum) {
        QString err = QObject::tr("Yedek dosyası bütünlük doğrulaması (SHA-256 Checksum) başarısız! Dosya bozulmuş olabilir.");
        Logger::error(QObject::tr("BackupPackage::unpack: ") + err);
        if (outError) *outError = err;
        return false;
    }

    // PBKDF2 ile Anahtar Türet ve Deşifre Et
    QByteArray key = AesGcm::deriveKey(password, salt);
    QByteArray payload = AesGcm::decrypt(encrypted, key);

    if (payload.isEmpty()) {
        QString err = QObject::tr("Yedek dosyası çözülemedi. Parola hatalı veya veri şifresi bozuk.");
        Logger::error(QObject::tr("BackupPackage::unpack: ") + err);
        if (outError) *outError = err;
        return false;
    }

    // Payload'ı ayrıştır
    QDataStream ds(payload);
    ds.setVersion(QDataStream::Qt_6_0);

    QString marker;
    ds >> marker;
    if (marker == "OKBK_PAYLOAD") {
        ds >> outContent.meta.version;
        ds >> outContent.meta.timestamp;
        ds >> outContent.meta.creator;
        ds >> outContent.meta.description;
        ds >> outContent.databaseData;
        if (version >= 3) {
            ds >> outContent.auditData;
            ds >> outContent.chatData;
        } else {
            outContent.auditData.clear();
            outContent.chatData.clear();
        }

        quint32 certCount = 0;
        ds >> certCount;
        outContent.certFiles.clear();
        for (quint32 i = 0; i < certCount; ++i) {
            QString name;
            QByteArray data;
            ds >> name >> data;
            outContent.certFiles[name] = data;
        }
    } else {
        // Eski tekil QDataStream dosyaları (Legacy)
        // İlk alan dosya sayısı varsayılır
        QDataStream legacyDs(payload);
        legacyDs.setVersion(QDataStream::Qt_6_0);
        quint32 fileCount = 0;
        legacyDs >> fileCount;
        outContent.certFiles.clear();
        for (quint32 i = 0; i < fileCount; ++i) {
            QString name;
            QByteArray data;
            legacyDs >> name >> data;
            if (name == "okidia.db") {
                outContent.databaseData = data;
            } else {
                outContent.certFiles[name] = data;
            }
        }
        outContent.meta.version = "1.0-legacy";
        outContent.meta.timestamp = timestamp;
    }

    return true;
}

bool BackupPackage::unpackFromFile(const QString& filePath, const QString& password, BackupContent& outContent, QString* outError) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        QString err = QObject::tr("Yedek dosyası açılamadı: %1").arg(filePath);
        Logger::error(QObject::tr("BackupPackage::unpackFromFile: ") + err);
        if (outError) *outError = err;
        return false;
    }

    QByteArray data = f.readAll();
    f.close();

    return unpack(data, password, outContent, outError);
}

bool BackupPackage::verifyHeader(const QByteArray& packageData) {
    if (packageData.size() < 4) return false;
    return packageData.startsWith("OKBK");
}

bool BackupPackage::verifyHeaderFile(const QString& filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    char magic[4];
    qint64 read = f.read(magic, 4);
    f.close();
    return (read == 4 && QByteArray(magic, 4) == "OKBK");
}
