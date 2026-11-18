// agent/system/SystemInfo.h
#pragma once

#include <QString>
#include <QMap>

struct SystemStats {
    float    cpuUsage    = 0.0f;
    float    memUsage    = 0.0f;
    uint64_t memTotal    = 0;
    uint64_t memUsed     = 0;
    
    // Disk metrics (Root /)
    uint64_t diskTotal   = 0; // bytes
    uint64_t diskUsed    = 0; // bytes
    float    diskUsage   = 0.0f; // percentage

    QString  hostname;
    QString  username;
    QString  osVersion;
    QString  osTag;
    QString  osArch;
    QString  agentVersion;
    bool     secureBoot    = false; // Secure Boot / UEFI durumu
    bool     diskEncrypted = false; // LUKS / BitLocker tam disk şifreleme durumu
};

class SystemInfo {
public:
    static float       readCpuUsage();
    static float       readMemUsage();
    static SystemStats readStats();

    static bool        isSecureBootActive();
    static bool        isStorageEncrypted();

    static QString hostname();
    static QString username();   // Tray tarafı için — mevcut sistem kullanıcısı
    static QString osVersion();
    static QString machineId();
    static QString ipAddress();

    static QString macAddress(const QString& ifaceName = "");
    static QMap<QString, QString> allMacAddresses();

    // ── Oturum izleme (agent/main.cpp için) ─────────────────────────────
    // :0 ekranındaki aktif kullanıcıyı döndürür.
    // Login ekranındaysa "lightdm"/"sddm", gerçek oturumdaysa username,
    // belirsiz/geçiş durumundaysa boş QString döner.
    static QString activeDisplayUser();

    // Kullanıcının sistemdeki tam adını (Ad Soyad / GECOS / Display Name) döndürür.
    // Tanımlı değilse veya username ile aynıysa boş QString döner.
    static QString userDisplayName(const QString& username);

    // Oturum yöneticisinin (lightdm/sddm) XAUTHORITY dosya yolunu döndürür.
    // Dosya yoksa (= DM ekranda değil) boş QString döner.
    static QString activeDmDisplay();  // Greeter'in gerçek display'i (:0, :1...)
    static QString dmXauthority();

    // Verilen kullanıcı adı bir display manager kullanıcısı mı?
    // (lightdm, sddm, gdm, boş string → true)
    static bool isDmUser(const QString& username);

    // Aktif ses oturumu için (PulseAudio/PipeWire) kullanıcı adı ve UID döndürür.
    static bool getAudioSession(QString& outUsername, QString& outUid);

    // Aktif konsol oturumunda (Session 1 / Winlogon) bağımsız süreç başlatır (Windows)
    static bool launchProcessInConsoleSession(const QString& appPath, const QStringList& args, void** outProcessHandle = nullptr);

    // Aktif kullanıcı oturumunda GUI süreci başlatır (Linux / Windows)
    static bool launchProcessInUserSession(const QString& username, const QString& appPath, const QStringList& args);

    // Windows iş istasyonunun kilitli olup olmadığını (Lock Screen / LogonUI aktif) kontrol eder
    static bool isWorkstationLocked();

    // İşletim sistemi seviyesinde "Kullanıcı Değiştir" (Switch User) özelliğini açar/kapatır
    static void setSwitchUserDisabled(bool disabled);

    // İşletim sistemi seviyesinde USB koruma (otomatik sync ve kirli dosya sistemi onarımı) kuralını yönetir
    static void setUsbProtectionEnabled(bool enabled);

    // Takılı çıkarılabilir USB belleklerin listesini döndürür
    struct UsbDriveInfo {
        QString rootPath;
        QString device;
        QString displayName;
    };
    static QList<UsbDriveInfo> mountedUsbDrives();

    // Takılı USB bellekleri güvenle ayırır (sync + unmount)
    static bool safeEjectUsbDrives();

    // Donanım saati (RTC) ve BIOS pili durumunu denetler
    static bool isRtcBatteryFaulty();

private:
    struct CpuTimes {
        uint64_t user = 0, nice = 0, system = 0,
                 idle = 0, iowait = 0, irq = 0,
                 softirq = 0, steal = 0;
        uint64_t total() const {
            return user + nice + system + idle +
                   iowait + irq + softirq + steal;
        }
        uint64_t active() const {
            return user + nice + system + irq + softirq + steal;
        }
    };

    static CpuTimes readCpuTimes();
    static CpuTimes s_prevCpuTimes;
};
