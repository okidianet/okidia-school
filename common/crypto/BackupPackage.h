// common/crypto/BackupPackage.h
#pragma once

#include <QString>
#include <QByteArray>
#include <QMap>
#include <QDateTime>

struct BackupMetadata {
    QString version;
    qint64 timestamp = 0;
    QString creator;
    QString description;
};

struct BackupContent {
    BackupMetadata meta;
    QByteArray databaseData;               // raw SQLite okidia.db bytes
    QByteArray auditData;                  // raw SQLite okidia_audit.db bytes (opsiyonel)
    QByteArray chatData;                   // raw SQLite okidia_chat.db bytes (opsiyonel)
    QMap<QString, QByteArray> certFiles;   // "ca.crt", "ca.key", "ca.srl", "broker.crt", "broker.key", etc.
};

class BackupPackage {
public:
    static constexpr quint32 CURRENT_VERSION = 3;

    // Packs content, derives key from password + random salt via PBKDF2, encrypts via AES-256-GCM
    static QByteArray pack(const QString& password, const BackupContent& content);
    static bool packToFile(const QString& filePath, const QString& password, const BackupContent& content, QString* outError = nullptr);

    // Unpacks raw bytes or file, checks magic/version/checksum, decrypts via AES-256-GCM
    static bool unpack(const QByteArray& packageData, const QString& password, BackupContent& outContent, QString* outError = nullptr);
    static bool unpackFromFile(const QString& filePath, const QString& password, BackupContent& outContent, QString* outError = nullptr);

    // Verifies whether header magic is "OKBK"
    static bool verifyHeader(const QByteArray& packageData);
    static bool verifyHeaderFile(const QString& filePath);
};
