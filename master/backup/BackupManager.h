// master/backup/BackupManager.h
#pragma once

#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QFileInfo>
#include "../../common/crypto/BackupPackage.h"

/**
 * Dağıtık ve Yerel Yedekleme Yöneticisi.
 *
 * Root CA ve Broker sertifikaları ile SQLite veritabanını
 * AES-256-GCM ile şifreler ve tekil .okbak dosyası olarak saklar.
 */
class BackupManager : public QObject {
    Q_OBJECT

public:
    static BackupManager& instance();

    // Yedek al: yönetici şifresiyle şifrele, targetDir'e .okbak olarak kaydet
    bool backup(const QString& targetDir,
                const QString& adminPassword,
                const BackupContent& content);

    // Yedeği geri yükle (unpack)
    bool restore(const QString& backupPath,
                 const QString& adminPassword,
                 BackupContent& outContent);

    // Recovery phrase üret (ilk kurulumda)
    static QStringList generateRecoveryPhrase();

    // Recovery phrase'den şifre türet
    static QString passwordFromPhrase(const QStringList& phrase);

    // Mevcut yedekleri listele
    QList<QDateTime> listBackups(const QString& dir) const;

signals:
    void backupFinished(bool success, const QString& path);
    void restoreFinished(bool success);
    void backupProgress(int percent);

private:
    explicit BackupManager(QObject* parent = nullptr);
    BackupManager(const BackupManager&)            = delete;
    BackupManager& operator=(const BackupManager&) = delete;

    static const QStringList& wordList();
};
