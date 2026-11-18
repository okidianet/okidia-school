// master/license/LicenseManager.h
#pragma once

#include <QObject>
#include <QDateTime>

enum class LicenseType {
    Free,       // Sınıf versiyonu — max 1 öğretmen, 50 istemci
    Campus      // Kampüs versiyonu — limit yok
};

struct LicenseInfo {
    LicenseType type        = LicenseType::Free;
    QString     schoolName;
    int         maxClients  = 50;   // Free: 50, Campus: INT_MAX
    int         maxTeachers = 1;
    QDateTime   expiresAt;          // Geçersiz QDateTime = sonsuz (Free)
    QString     serialNumber;
    bool        valid       = false;
};

/**
 * Sertifika tabanlı lisans yöneticisi.
 *
 * - Lisans bilgisi X.509 sertifikasındaki extension alanlarından okunur.
 * - Doğrulama önce lokal (offline) yapılır.
 * - Ayda bir lisans sunucusuyla senkronize edilir (toleranslı mod).
 * - Sunucuya ulaşılamazsa 30 gün grace period tanınır.
 */
class LicenseManager : public QObject {
    Q_OBJECT

public:
    static LicenseManager& instance();

    // Sertifikadan lisansı yükle
    bool load(const QString& certPath);

    // Lisans bilgisi
    const LicenseInfo& info() const { return m_info; }
    bool isValid()    const;
    bool isExpired()  const;
    bool isFree()     const { return m_info.type == LicenseType::Free; }

    // Limit kontrolleri
    bool canAddClient()  const;
    bool canAddTeacher() const;

    // Sunucu senkronizasyonu (arka planda çalışır)
    void startPeriodicSync(const QString& serverUrl);

signals:
    void licenseExpiringSoon(int daysLeft);
    void licenseExpired();
    void licenseUpdated();

private:
    explicit LicenseManager(QObject* parent = nullptr);
    LicenseManager(const LicenseManager&)            = delete;
    LicenseManager& operator=(const LicenseManager&) = delete;

    LicenseInfo m_info;
    QString     m_certPath;
    QString     m_serverUrl;

    // Son başarılı sunucu sync zamanı
    QDateTime   m_lastSync;
    // Grace period: sunucu erişilemezse bu kadar gün geçerli
    static constexpr int GRACE_DAYS = 30;

    void parseFromCert(const QString& certPath);
    void checkExpiry();
    void syncWithServer();
};
