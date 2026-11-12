// broker/db/DbRecords.h
// Veritabanı satır yapıları — tüm bileşenler tarafından kullanılır
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QDateTime>

// ── Agent (onaylı istemci) ────────────────────────────────────────────────
struct AgentRecord {
    QString machineId;    // PRIMARY KEY
    QString hostname;
    QString mac;
    QString ip;
    QString osInfo;
    QString osPrettyName;
    QString osArch;
    QString agentVer;
    QString classId;      // FK → classes.id (boş = atanmamış)
    QString certPem;
    int     agentType   = 0;  // 0=Standard, 1, 2, 3
    QString alias;
    
    int     status      = 1;  // 0=Pending, 1=Approved, 2=Blacklisted
    QString statusReason;
    
    QString approvedBy;   // user_id
    qint64  approvedAt  = 0;
    qint64  lastSeen    = 0;
};



// ── User (YM / OM / Gözlemci) ────────────────────────────────────────────
struct UserRecord {
    QString id;           // uuid, PRIMARY KEY
    QString username;
    QString passwordHash; // Argon2id
    QString role;         // "YM" | "OM" | "OBSERVER"
    QString displayName;
    bool    mustChangePw = true;
    bool    isActive     = true;
    QString createdBy;    // user_id
    qint64  createdAt    = 0;
    qint64  lastLogin    = 0;
    bool    isBackupReplica = false; // İkincil yedek deposu olarak yetkili mi?
    QString replicaMachineId; // Yedek replikasyonu için yetkilendirilmiş fiziksel Master cihaz kimliği
    QString replicaHostname;  // Yedek replikasyonu için yetkilendirilmiş Master bilgisayar adı (hostname)
    QString lastVersion;  // En son bağlandığı Master sürümü (Örn: "0.1.468")
    QString lastOs;       // Görünen OS Adı / Pretty Name (Örn: "Linux Mint 22.1", "Ubuntu 24.04 LTS")
    QString lastOsTag;    // Paket eşleme etiketi (Örn: "ubuntu24", "debian12")
    QString lastArch;     // CPU mimarisi (Örn: "amd64", "arm64")
};

// ── Teacher (Akıllı Tahta Branş Öğretmeni) ────────────────────────────────
struct TeacherRecord {
    QString id;           // uuid, PRIMARY KEY
    QString username;     // Sistem OS kullanıcı adı (örn: ahmet.yilmaz)
    QString displayName;  // Ad Soyad (örn: Ahmet Yılmaz)
    QString branch;       // Branş (örn: Fizik, Matematik, Bilişim)
    QString seedHex;      // 32-bayt HMAC tohumu (Hex string)
    QString password;     // İsteğe bağlı ilk oluşturma parolası
    bool    isActive  = true;
    QString createdBy;    // user_id
    qint64  createdAt = 0;
    qint64  updatedAt = 0;
};


// ── Class (sınıf / grup) ─────────────────────────────────────────────────
struct ClassRecord {
    QString id;           // uuid, PRIMARY KEY (TreeManager stableId)
    QString name;
    QString icon          = "🏫";
    QString type          = "class";  // "class" | "group"
    QString parentId;     // boş = top-level
    QString subnetHint;   // opsiyonel: "192.168.1.0/24"
    QString createdBy;    // user_id
    qint64  createdAt     = 0;
    qint64  updatedAt     = 0;
};

// ── UserClass (kullanıcı-sınıf yetkisi) ──────────────────────────────────
struct UserClassRecord {
    QString userId;
    QString classId;
    QString permission    = "full";  // "full" | "view"
    QString grantedBy;    // user_id
    qint64  grantedAt     = 0;
};

// ── Delegation (geçici yetki devri) ──────────────────────────────────────
struct DelegationRecord {
    QString id;           // uuid
    QString fromUser;     // user_id
    QString toUser;       // user_id
    QString classId;      // FK → classes.id
    QString type;         // "full" | "shared" | "view"
    qint64  startsAt      = 0;
    qint64  endsAt        = 0;
    QString createdBy;    // user_id
    qint64  createdAt     = 0;
};

// ── AuditEvent (komut kaydı) ──────────────────────────────────────────────
struct AuditRecord {
    qint64  id            = 0;
    qint64  timestamp     = 0;
    QString category      = "Command";
    QString actor;        // username
    QString userId;       // OKIDIA users.id UUID
    QString masterId;     // hangi master'dan geldi
    QString target;       // clientId / machineId
    QString detail;
};

// ── Chat mesaj kaydı ─────────────────────────────────────────────────────
struct ChatMessageRecord {
    QString id;
    QString roomId;
    QString fromHost;
    int     senderRole  = 0;
    QString content;
    qint64  timestamp   = 0;
    bool    isSystem    = false;
};

// ── Özel sohbet oturum kaydı (içerik yok, sadece meta) ───────────────────
struct PrivateChatLogRecord {
    QString id;
    QString fromMachineId;
    QString fromHost;
    QString fromUser;
    QString toMachineId;
    QString toHost;
    QString toUser;
    qint64  startedAt   = 0;
    qint64  endedAt     = 0;  // 0 = devam ediyor
};

// ── Sohbet kilit durumu ───────────────────────────────────────────────────
struct ChatLockRecord {
    QString classId;
    bool    chatLocked    = false;
    bool    privateLocked = false;
    qint64  updatedAt     = 0;
};

// ── Bekleyen komut (offline agent) ───────────────────────────────────────
struct PendingCommandRecord {
    qint64     id         = 0;
    QString    machineId;
    int        msgType    = 0;
    QByteArray payload;
    qint64     createdAt  = 0;
    qint64     expiresAt  = 0;
};

// ── Broker (SB/MB kaydı) ─────────────────────────────────────────────────
struct BrokerRecord {
    QString id;           // uuid
    QString hostname;
    QString ip;
    QString role          = "SB";  // "MB" | "SB"
    QString certPem;
    qint64  lastSeen      = 0;
    qint64  createdAt     = 0;
};

// ── SSH Kimlikleri ───────────────────────────────────────────────────────
struct SshCredentialRecord {
    QString id;           // uuid, PRIMARY KEY
    QString name;         // "Öğretmen PC", "Laboratuvar"
    QString username;
    QByteArray secret;    // Şifrelenmiş parola/anahtar
    bool    isKey       = false;
    QString ownerUserId;  // Boşsa ortak, doluysa o kullanıcıya özel
    QString targetId;     // Agent IP veya Class UUID
    QString targetType;   // "agent" veya "class"
    QString machineId;    // Okidia UUID (İstemci bağlandığında eşleşir)
    qint64  createdAt   = 0;
};

// ── Katılım Kodları (OM davetleri) ────────────────────────────────────────
struct JoinCodeRecord {
    QString code;         // PRIMARY KEY (JOIN-XXXX-YYYY)
    QString userId;       // Hangi kullanıcı için üretildi (users.id)
    qint64  expiresAt   = 0;
    qint64  usedAt      = 0;  // 0 = kullanılmadı
};

// ── Master Cihazı (Fiziksel Bilgisayar Kimliği) ───────────────────────────
struct MasterDeviceRecord {
    QString machineId;    // Fiziksel makine kimliği (PRIMARY KEY)
    QString hostname;     // Bilgisayar adı (örn: lab-pc1, yonetici-pc)
    QString ip;           // En son bağlandığı IP adresi
    QString osPrettyName; // Görünen OS (örn: Ubuntu 24.04 LTS, Debian 13)
    QString osTag;        // Paket etiketi (ubuntu24, debian12 vb.)
    QString osArch;       // CPU mimarisi (amd64, arm64)
    QString masterVer;    // Yüklü Master sürümü (örn: 0.1.885)
    QString lastUserId;   // Son giriş yapan kullanıcı UUID
    QString lastUsername; // Son giriş yapan kullanıcı adı
    qint64  lastSeen = 0; // Son görülme / login zamanı
};

// ── Kullanıcı Tanımlı Özel Ajan Profili (Maskeli Öğrenci PC) ────────────
struct CustomProfileRecord {
    int     id          = 0;                // 101'den başlar (101, 102...)
    QString name;                           // "Zil & Amfi PC", "Özel Eğitim"
    bool    playBell    = true;             // 1: Zil Çalar, 0: Sessiz
    bool    recessLock  = false;            // 1: Teneffüste Kilitlenir, 0: Asla Kilitlenmez
    bool    chatEnabled = true;             // 1: Sohbete Katılabilir, 0: Kapalı
    QString iconName    = "custom_profile"; // Arayüz SVG ikon adı
    qint64  createdAt   = 0;
};
