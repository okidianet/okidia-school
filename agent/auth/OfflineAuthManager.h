// agent/auth/OfflineAuthManager.h
#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include <QDateTime>
#include <QMutex>

struct TeacherInfo {
    QString teacherId;
    QString displayName;
    QString username;
    QByteArray seedBytes;
    QString password;
};

/**
 * OfflineAuthManager — Akıllı Tahta (Tip 3) Çevrimdışı Kimlik Doğrulama Motoru
 *
 * Sıfır ağ teması ile (Air-Gapped) öğretmenlerin kayıtlı tohumları üzerinden
 * tahta dinamik QR kodu ve 6 haneli HMAC PIN doğrulamasını yönetir.
 */
class OfflineAuthManager : public QObject {
    Q_OBJECT

public:
    static OfflineAuthManager& instance();

    // Yerel tohum deposunu yükle / kaydet (/etc/okidia/teachers.dat)
    void loadStore();
    void saveStore();

    // Öğretmen ekle veya güncelle
    void upsertTeacher(const QString& teacherId, const QString& displayName,
                       const QString& username, const QString& seedHex,
                       const QString& password = QString());
    void removeTeacher(const QString& teacherId);
    QList<TeacherInfo> allTeachers() const;

    // Aktif kilit oturumu için Nonce ve dinamik QR içerik üret
    QString startLockSession(const QString& agentUuid);
    void endLockSession();

    // Geçerli aktif kilit oturumunun verileri
    QString currentNonce() const { return m_currentNonce; }
    qint64 currentWindow() const { return m_currentWindow; }
    QString currentQrData() const { return m_currentQrData; }

    // Test ve geliştirme aşaması için aktif ilk öğretmenin geçerli PIN'ini döner
    QString currentTestPin() const;
    int remainingSeconds() const;

    // Tahtaya girilen 6 haneli PIN'i kayıtlı öğretmenlerle doğrula
    // Eşleşirse eşleşen öğretmenin kullanıcı adı, display adı ve şifresi döner
    bool verifyPin(const QString& enteredPin, QString* outUsername = nullptr,
                   QString* outDisplayName = nullptr, QString* outPassword = nullptr);


signals:
    void lockSessionRefreshed(const QString& qrData, const QString& testPin, int remainingSeconds);
    void authSucceeded(const QString& username, const QString& displayName);
    void authFailed(int remainingAttempts);

private:
    explicit OfflineAuthManager(QObject* parent = nullptr);
    ~OfflineAuthManager() override = default;

    OfflineAuthManager(const OfflineAuthManager&) = delete;
    OfflineAuthManager& operator=(const OfflineAuthManager&) = delete;

    mutable QMutex m_mutex;
    QMap<QString, TeacherInfo> m_teachers; // teacherId -> TeacherInfo

    QString m_agentUuid;
    QString m_currentNonce;
    qint64  m_currentWindow = 0;
    QString m_currentQrData;
    int     m_failedAttempts = 0;
    QDateTime m_sessionStartTime;
};
