#include "OkidiaDatabase.h"
#include "../OkidiaConfig.h"
#include "../utils/Logger.h"
#include "../utils/OkidiaUtils.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlRecord>
#include <QRandomGenerator>

// ── Singleton ─────────────────────────────────────────────────────────────

OkidiaDatabase& OkidiaDatabase::instance() {
    static OkidiaDatabase inst;
    return inst;
}

OkidiaDatabase::OkidiaDatabase(QObject* parent) : QObject(parent) {}

// ── Yaşam döngüsü ─────────────────────────────────────────────────────────

bool OkidiaDatabase::open(const QString& path) {
    if (path.isEmpty()) {
        m_path = OkidiaConfig::dbDir() + "/okidia.db";
    } else {
        m_path = path;
    }

    // Klasörü oluştur
    if (m_path != ":memory:") {
        QDir dir = QFileInfo(m_path).absoluteDir();
        if (!dir.exists()) {
            if (!dir.mkpath(".")) {
                Logger::error(QObject::tr("OkidiaDatabase: Klasör oluşturulamadı → ") + m_path);
                return false;
            }
        }
    }

    // Bağlantıyı aç
    // Birden fazla process aynı DB'ye bağlanabilir — her biri farklı connection name
    const QString appName = QCoreApplication::applicationName();
    const QString connName = QString("okidia_%1").arg(appName);
    if (QSqlDatabase::contains(connName))
        m_db = QSqlDatabase::database(connName);
    else
        m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
    m_db.setDatabaseName(m_path);

    if (!m_db.open()) {
        Logger::error(QObject::tr("OkidiaDatabase: Açılamadı → ") + m_path
                      + " : " + m_db.lastError().text());
        return false;
    }

    // Performans ve güvenlik ayarları (WAL, concurrency, autocheckpoint)
    execQuery("PRAGMA journal_mode = WAL");
    execQuery("PRAGMA busy_timeout = 5000");
    execQuery("PRAGMA synchronous = NORMAL");
    execQuery("PRAGMA temp_store = MEMORY");
    execQuery("PRAGMA foreign_keys = ON");
    execQuery("PRAGMA wal_autocheckpoint = 1000");

    // Sürüm Güvenlik Kapısı (Schema Version Gate)
    const int dbVer = schemaVersion();
    if (dbVer > kCurrentSchemaVersion) {
        Logger::error(QObject::tr("OkidiaDatabase: Veritabanı şema sürümü (v%1), mevcut Okidia ikilisinin desteklediği sürümden (v%2) daha yeni! Eski ikilinin yeni veritabanını bozmaması için açılış durduruldu.")
                      .arg(dbVer).arg(kCurrentSchemaVersion));
        close();
        return false;
    }

    if (!createSchema()) {
        Logger::error(QObject::tr("OkidiaDatabase: Schema oluşturulamadı"));
        return false;
    }

    if (!runMigrations()) {
        Logger::error(QObject::tr("OkidiaDatabase: Migration başarısız"));
        return false;
    }

    m_open = true;
    Logger::info(QObject::tr("OkidiaDatabase: Açıldı → ") + m_path
                 + QObject::tr(" (schema v") + QString::number(kCurrentSchemaVersion) + ")");
    return true;
}

void OkidiaDatabase::close() {
    const QString appName = QCoreApplication::applicationName();
    if (m_db.isOpen()) m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(QString("okidia_%1").arg(appName));

    m_open = false;
}

bool OkidiaDatabase::switchDatabase(const QString& newPath) {
    if (newPath.isEmpty()) return false;
    if (m_open && m_path == newPath) return true; // Zaten bu veritabanı açık

    Logger::info(QObject::tr("OkidiaDatabase: Veritabanı profili değiştiriliyor → ") + newPath);
    close();
    return open(newPath);
}

bool OkidiaDatabase::isOpen() const { return m_open && m_db.isOpen(); }

// ── Schema ────────────────────────────────────────────────────────────────

bool OkidiaDatabase::createSchema() {
    const QStringList ddl = {
        // Meta
        R"(CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        ))",

        // Brokers
        R"(CREATE TABLE IF NOT EXISTS brokers (
            id         TEXT PRIMARY KEY,
            hostname   TEXT NOT NULL,
            ip         TEXT NOT NULL,
            role       TEXT NOT NULL DEFAULT 'SB',
            cert_pem   TEXT,
            last_seen  INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT (unixepoch())
        ))",

        // Users
        R"(CREATE TABLE IF NOT EXISTS users (
            id             TEXT PRIMARY KEY,
            username       TEXT NOT NULL UNIQUE,
            password_hash  TEXT NOT NULL,
            role           TEXT NOT NULL,
            display_name   TEXT NOT NULL DEFAULT '',
            must_change_pw INTEGER NOT NULL DEFAULT 1,
            is_active      INTEGER NOT NULL DEFAULT 1,
            created_by     TEXT,
            created_at     INTEGER NOT NULL DEFAULT (unixepoch()),
            last_login     INTEGER,
            is_backup_replica INTEGER NOT NULL DEFAULT 0,
            replica_machine_id TEXT NOT NULL DEFAULT '',
            replica_hostname   TEXT NOT NULL DEFAULT '',
            last_version   TEXT NOT NULL DEFAULT '',
            last_os        TEXT NOT NULL DEFAULT '',
            last_os_tag    TEXT NOT NULL DEFAULT '',
            last_arch      TEXT NOT NULL DEFAULT ''
        ))",

        // Master Devices (Fiziksel Master Bilgisayarları)
        R"(CREATE TABLE IF NOT EXISTS master_devices (
            machine_id     TEXT PRIMARY KEY,
            hostname       TEXT NOT NULL,
            ip             TEXT NOT NULL DEFAULT '',
            os_pretty_name TEXT NOT NULL DEFAULT '',
            os_tag         TEXT NOT NULL DEFAULT '',
            os_arch        TEXT NOT NULL DEFAULT '',
            master_ver     TEXT NOT NULL DEFAULT '',
            last_user_id   TEXT NOT NULL DEFAULT '',
            last_username  TEXT NOT NULL DEFAULT '',
            last_seen      INTEGER NOT NULL DEFAULT 0
        ))",

        // Teachers (Akıllı Tahta Branş Öğretmenleri)
        R"(CREATE TABLE IF NOT EXISTS teachers (
            id           TEXT PRIMARY KEY,
            username     TEXT NOT NULL UNIQUE,
            display_name TEXT NOT NULL,
            branch       TEXT NOT NULL DEFAULT '',
            seed_hex     TEXT NOT NULL,
            password     TEXT NOT NULL DEFAULT '',
            is_active    INTEGER NOT NULL DEFAULT 1,
            created_by   TEXT,
            created_at   INTEGER NOT NULL DEFAULT (unixepoch()),
            updated_at   INTEGER NOT NULL DEFAULT (unixepoch())
        ))",
        "CREATE INDEX IF NOT EXISTS idx_teachers_username ON teachers(username)",

        // Classes
        R"(CREATE TABLE IF NOT EXISTS classes (
            id          TEXT PRIMARY KEY,
            name        TEXT NOT NULL,
            icon        TEXT NOT NULL DEFAULT '🏫',
            type        TEXT NOT NULL DEFAULT 'class',
            parent_id   TEXT,
            subnet_hint TEXT,
            created_by  TEXT,
            created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
            updated_at  INTEGER NOT NULL DEFAULT (unixepoch()),
            FOREIGN KEY (parent_id) REFERENCES classes(id) ON DELETE SET NULL
        ))",
        "CREATE INDEX IF NOT EXISTS idx_classes_parent ON classes(parent_id)",

        // User-Class permissions
        R"(CREATE TABLE IF NOT EXISTS user_classes (
            user_id    TEXT NOT NULL,
            class_id   TEXT NOT NULL,
            permission TEXT NOT NULL DEFAULT 'full',
            granted_by TEXT,
            granted_at INTEGER NOT NULL DEFAULT (unixepoch()),
            PRIMARY KEY (user_id, class_id),
            FOREIGN KEY (user_id)  REFERENCES users(id)   ON DELETE CASCADE,
            FOREIGN KEY (class_id) REFERENCES classes(id) ON DELETE CASCADE
        ))",

        // Delegations
        R"(CREATE TABLE IF NOT EXISTS delegations (
            id         TEXT PRIMARY KEY,
            from_user  TEXT NOT NULL,
            to_user    TEXT NOT NULL,
            class_id   TEXT NOT NULL,
            type       TEXT NOT NULL,
            starts_at  INTEGER NOT NULL,
            ends_at    INTEGER NOT NULL,
            created_by TEXT NOT NULL,
            created_at INTEGER NOT NULL DEFAULT (unixepoch()),
            FOREIGN KEY (from_user) REFERENCES users(id)   ON DELETE CASCADE,
            FOREIGN KEY (to_user)   REFERENCES users(id)   ON DELETE CASCADE,
            FOREIGN KEY (class_id)  REFERENCES classes(id) ON DELETE CASCADE
        ))",
        "CREATE INDEX IF NOT EXISTS idx_delegations ON delegations(to_user, ends_at)",

        // Agents
        R"(CREATE TABLE IF NOT EXISTS agents (
            id          TEXT PRIMARY KEY,
            hostname    TEXT NOT NULL,
            mac         TEXT NOT NULL DEFAULT '',
            ip          TEXT NOT NULL DEFAULT '',
            os_info     TEXT NOT NULL DEFAULT '',
            os_pretty_name TEXT NOT NULL DEFAULT '',
            os_arch     TEXT NOT NULL DEFAULT '',
            agent_ver   TEXT NOT NULL DEFAULT '',
            class_id    TEXT,
            cert_pem    TEXT,
            agent_type  INTEGER NOT NULL DEFAULT 1,
            alias       TEXT NOT NULL DEFAULT '',
            status      INTEGER NOT NULL DEFAULT 1,
            status_reason TEXT NOT NULL DEFAULT '',
            approved_by TEXT,
            approved_at INTEGER NOT NULL DEFAULT (unixepoch()),
            last_seen   INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (class_id) REFERENCES classes(id) ON DELETE SET NULL
        ))",
        "CREATE INDEX IF NOT EXISTS idx_agents_class ON agents(class_id)",

        // Offline agent için bekleyen komutlar
        R"(CREATE TABLE IF NOT EXISTS pending_commands (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            machine_id  TEXT NOT NULL,
            msg_type    INTEGER NOT NULL,
            payload     BLOB NOT NULL,
            created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
            expires_at  INTEGER NOT NULL DEFAULT 0
        ))",
        "CREATE INDEX IF NOT EXISTS idx_pending_machine ON pending_commands(machine_id)",

        // Sınıf/grup sohbet kilit durumu
        R"(CREATE TABLE IF NOT EXISTS chat_room_state (
            class_id       TEXT PRIMARY KEY,
            chat_locked    INTEGER NOT NULL DEFAULT 0,
            private_locked INTEGER NOT NULL DEFAULT 0,
            updated_at     INTEGER NOT NULL DEFAULT (unixepoch()),
            FOREIGN KEY (class_id) REFERENCES classes(id) ON DELETE CASCADE
        ))",

        // SSH Kimlikleri
        R"(CREATE TABLE IF NOT EXISTS ssh_credentials (
            id          TEXT PRIMARY KEY,
            name        TEXT NOT NULL,
            username    TEXT NOT NULL,
            secret      BLOB NOT NULL,
            is_key      INTEGER NOT NULL DEFAULT 0,
            owner_id    TEXT NOT NULL DEFAULT '',
            target_id   TEXT NOT NULL DEFAULT '',
            target_type TEXT NOT NULL DEFAULT '',
            machine_id  TEXT NOT NULL DEFAULT '',
            created_at  INTEGER NOT NULL DEFAULT (unixepoch())
        ))",
        "CREATE INDEX IF NOT EXISTS idx_ssh_owner  ON ssh_credentials(owner_id)",
        "CREATE INDEX IF NOT EXISTS idx_ssh_target ON ssh_credentials(target_id)",
        R"(CREATE TABLE IF NOT EXISTS join_codes (
            code       TEXT PRIMARY KEY,
            user_id    TEXT NOT NULL,
            expires_at INTEGER NOT NULL,
            used_at    INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
        ))",
        R"(CREATE TABLE IF NOT EXISTS custom_profiles (
            id           INTEGER PRIMARY KEY,
            name         TEXT NOT NULL,
            play_bell    INTEGER NOT NULL DEFAULT 1,
            recess_lock  INTEGER NOT NULL DEFAULT 0,
            chat_enabled INTEGER NOT NULL DEFAULT 1,
            icon_name    TEXT NOT NULL DEFAULT 'custom_profile',
            created_at   INTEGER NOT NULL DEFAULT (unixepoch())
        ))",
    };

    for (const QString& sql : ddl) {
        if (!execQuery(sql)) return false;
    }

    // Seed: 101 ID'li varsayılan "Zil & Amfi PC" profilini oluştur
    {
        QSqlQuery qSeed(m_db);
        qSeed.exec("INSERT OR IGNORE INTO custom_profiles(id, name, play_bell, recess_lock, chat_enabled, icon_name, created_at) "
                   "VALUES(101, 'Zil & Amfi PC', 1, 0, 1, 'custom_profile', unixepoch())");
    }

    // Başlangıç meta değerleri (yoksa ekle)
    QSqlQuery q(m_db);
    q.prepare("INSERT OR IGNORE INTO meta(key, value) VALUES(?, ?)");
    const QList<QPair<QString,QString>> defaults = {
        {"db_version",   QString::number(kCurrentSchemaVersion)},
        {"created_at",   QString::number(OkidiaUtils::nowSecs())},
        {"last_modified",QString::number(OkidiaUtils::nowSecs())},
        {"instance_id",  OkidiaUtils::generateUuid()},
    };
    for (const auto& kv : defaults) {
        q.bindValue(0, kv.first);
        q.bindValue(1, kv.second);
        execQuery(q, "meta init");
    }

    return true;
}

// ── Migration ─────────────────────────────────────────────────────────────

bool OkidiaDatabase::runMigrations() {
    // Mevcut veritabanlarında users tablosunda replica_machine_id ve replica_hostname sütunlarını kontrol et / ekle
    {
        QSqlQuery colCheck(m_db);
        bool hasReplicaMachineId = false;
        bool hasReplicaHostname = false;
        if (colCheck.exec("PRAGMA table_info(users)")) {
            while (colCheck.next()) {
                const QString colName = colCheck.value(1).toString();
                if (colName == "replica_machine_id") hasReplicaMachineId = true;
                else if (colName == "replica_hostname") hasReplicaHostname = true;
            }
        }
        if (!hasReplicaMachineId) {
            execQuery("ALTER TABLE users ADD COLUMN replica_machine_id TEXT NOT NULL DEFAULT ''");
        }
        if (!hasReplicaHostname) {
            execQuery("ALTER TABLE users ADD COLUMN replica_hostname TEXT NOT NULL DEFAULT ''");
        }

        // Eğer YM veya replika yetkili OM kullanıcılarının henüz atanmış bir yedek cihazı yoksa,
        // master_devices tablosundan son giriş yaptıkları kendi bilgisayarlarını otomatik eşleştir
        QSqlQuery qUsers(m_db);
        if (qUsers.exec("SELECT id, role FROM users WHERE (role = 'YM' OR is_backup_replica = 1) AND replica_hostname = ''")) {
            QSet<QString> claimedHosts;
            QSqlQuery qClaimed(m_db);
            if (qClaimed.exec("SELECT LOWER(replica_hostname) FROM users WHERE replica_hostname != ''")) {
                while (qClaimed.next()) claimedHosts.insert(qClaimed.value(0).toString());
            }
            while (qUsers.next()) {
                const QString uid = qUsers.value(0).toString();
                QSqlQuery qDev(m_db);
                qDev.prepare("SELECT machine_id, hostname FROM master_devices WHERE last_user_id = ? ORDER BY last_seen DESC");
                qDev.addBindValue(uid);
                if (qDev.exec()) {
                    while (qDev.next()) {
                        const QString devMid = qDev.value(0).toString().trimmed();
                        const QString devHost = qDev.value(1).toString().trimmed();
                        if (!devHost.isEmpty() && !claimedHosts.contains(devHost.toLower())) {
                            QSqlQuery qUpd(m_db);
                            qUpd.prepare("UPDATE users SET is_backup_replica = 1, replica_machine_id = ?, replica_hostname = ? WHERE id = ?");
                            qUpd.addBindValue(devMid);
                            qUpd.addBindValue(devHost);
                            qUpd.addBindValue(uid);
                            qUpd.exec();
                            claimedHosts.insert(devHost.toLower());
                            break;
                        }
                    }
                }
            }
        }
    }

    // custom_profiles tablosunu kontrol et / oluştur ve 101 tohum kaydını ekle
    {
        execQuery(R"(CREATE TABLE IF NOT EXISTS custom_profiles (
            id           INTEGER PRIMARY KEY,
            name         TEXT NOT NULL,
            play_bell    INTEGER NOT NULL DEFAULT 1,
            recess_lock  INTEGER NOT NULL DEFAULT 0,
            chat_enabled INTEGER NOT NULL DEFAULT 1,
            icon_name    TEXT NOT NULL DEFAULT 'custom_profile',
            created_at   INTEGER NOT NULL DEFAULT (unixepoch())
        ))");

        QSqlQuery qSeed(m_db);
        qSeed.exec("INSERT OR IGNORE INTO custom_profiles(id, name, play_bell, recess_lock, chat_enabled, icon_name, created_at) "
                   "VALUES(101, 'Zil & Amfi PC', 1, 0, 1, 'custom_profile', unixepoch())");
    }

    const int ver = schemaVersion();
    if (ver == kCurrentSchemaVersion) return true;

    Logger::info(QObject::tr("OkidiaDatabase: Şema versiyonu güncelleniyor: ") + QString::number(ver)
                 + " → " + QString::number(kCurrentSchemaVersion));

    setMetaValue("db_version", QString::number(kCurrentSchemaVersion));
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    return true;
}

// ── Meta ──────────────────────────────────────────────────────────────────

int OkidiaDatabase::schemaVersion() const {
    const QString v = metaValue("db_version");
    return v.isEmpty() ? 0 : v.toInt();
}

QString OkidiaDatabase::metaValue(const QString& key) const {
    if (!m_open) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM meta WHERE key = ?");
    q.addBindValue(key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return {};
}

bool OkidiaDatabase::setMetaValue(const QString& key, const QString& value) {
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    q.addBindValue(key);
    q.addBindValue(value);
    return execQuery(q, "setMetaValue");
}

// ── Windows İstemci Kaldırma Koruması ─────────────────────────────────────

const QString OkidiaDatabase::DEFAULT_UNINSTALL_PASS = QStringLiteral("www.okidia.net");
const QString OkidiaDatabase::DEFAULT_UNINSTALL_PASS_HASH = QStringLiteral("f078695dd90be300e7d6af8f96fb389fefb93cdee5d6ae91a9d730e11150b0e9");

QString OkidiaDatabase::uninstallPasswordHash() const {
    QString val = metaValue("sec/agent_uninstall_hash").trimmed().toLower();
    if (val.isEmpty()) {
        return DEFAULT_UNINSTALL_PASS_HASH;
    }
    return val;
}

bool OkidiaDatabase::isDefaultUninstallPassword() const {
    QString val = uninstallPasswordHash();
    return (val.isEmpty() || val == DEFAULT_UNINSTALL_PASS_HASH);
}

bool OkidiaDatabase::setUninstallPasswordHash(const QString& hash) {
    QString h = hash.trimmed().toLower();
    if (h.isEmpty()) {
        h = DEFAULT_UNINSTALL_PASS_HASH;
    }
    bool ok = setMetaValue("sec/agent_uninstall_hash", h);
    if (ok) {
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    }
    return ok;
}

// ── Agents ────────────────────────────────────────────────────────────────

bool OkidiaDatabase::upsertAgent(const AgentRecord& a) {
    // machine_id (UUID) tek ve birincil tanımlayıcıdır.
    // IP veya hostname bazlı deduplication yapılmaz:
    //   - Aynı cihaz farklı ağda farklı IP ile bağlanabilir (mDNS, çok-ağ senaryosu)
    //   - Hostname değişebilir, IP değişebilir; UUID değişmez
    // MAC bazlı deduplication artık burada değil — sadece ilk kayıt anında (approveDiscovery) yapılır.
    // ON CONFLICT(id) DO UPDATE zaten aynı UUID geldiyse kaydı günceller.

    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO agents(id, hostname, mac, ip, os_info, os_pretty_name, os_arch, agent_ver,"
        " class_id, cert_pem, agent_type, alias, status, status_reason, approved_by, approved_at, last_seen)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   hostname    = excluded.hostname,"
        "   mac         = excluded.mac,"
        "   ip          = excluded.ip,"
        "   os_info     = excluded.os_info,"
        "   os_pretty_name = excluded.os_pretty_name,"
        "   os_arch     = excluded.os_arch,"
        "   agent_ver   = excluded.agent_ver,"
        "   class_id    = excluded.class_id,"
        "   cert_pem    = excluded.cert_pem,"
        "   agent_type  = excluded.agent_type,"
        "   alias       = CASE WHEN excluded.alias != '' THEN excluded.alias ELSE agents.alias END,"
        "   status      = excluded.status,"
        "   status_reason = excluded.status_reason,"
        "   approved_by = excluded.approved_by,"
        "   approved_at = excluded.approved_at,"
        "   last_seen   = excluded.last_seen");
    q.addBindValue(a.machineId);
    q.addBindValue(a.hostname.isNull() ? "" : a.hostname);
    q.addBindValue(a.mac.isNull() ? "" : a.mac);
    q.addBindValue(a.ip.isNull() ? "" : a.ip);
    q.addBindValue(a.osInfo.isNull() ? "" : a.osInfo);
    q.addBindValue(a.osPrettyName.isNull() ? "" : a.osPrettyName);
    q.addBindValue(a.osArch.isNull() ? "" : a.osArch);
    q.addBindValue(a.agentVer.isNull() ? "" : a.agentVer);
    q.addBindValue(a.classId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : a.classId);
    q.addBindValue(a.certPem);
    q.addBindValue(a.agentType);
    q.addBindValue(a.alias.isNull() ? "" : a.alias);
    q.addBindValue(a.status);
    q.addBindValue(a.statusReason.isNull() ? "" : a.statusReason);
    q.addBindValue(a.approvedBy.isNull() ? QVariant(QMetaType(QMetaType::QString)) : a.approvedBy);
    q.addBindValue(a.approvedAt);
    q.addBindValue(a.lastSeen);

    if (!execQuery(q, "upsertAgent")) return false;
    
    // SSH Kimlik Eşleştirme (Dinamik IP Uyumu)
    QSqlQuery qSsh(m_db);
    // 1. İstemcinin IP'si değiştiyse, eski machine_id'li kimliklerin target_id'sini yeni IP yap
    qSsh.prepare("UPDATE ssh_credentials SET target_id = ? WHERE machine_id = ? AND target_type = 'agent'");
    qSsh.addBindValue(a.ip);
    qSsh.addBindValue(a.machineId);
    qSsh.exec();

    // 2. Ağ taramasıyla sadece IP üzerinden eklenmiş (machine_id boş) kimliklere bu istemcinin UUID'sini ata
    qSsh.prepare("UPDATE ssh_credentials SET machine_id = ? WHERE target_id = ? AND target_type = 'agent' AND machine_id = ''");
    qSsh.addBindValue(a.machineId);
    qSsh.addBindValue(a.ip);
    qSsh.exec();

    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit agentAdded(a.machineId);
    return true;
}

bool OkidiaDatabase::removeAgent(const QString& machineId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM agents WHERE id = ?");
    q.addBindValue(machineId);
    if (!execQuery(q, "removeAgent")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit agentRemoved(machineId);
    return true;
}

AgentRecord OkidiaDatabase::agent(const QString& machineId) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM agents WHERE id = ?");
    q.addBindValue(machineId);
    if (q.exec() && q.next()) return rowToAgent(q);
    return {};
}

QList<AgentRecord> OkidiaDatabase::allAgents() const {
    QList<AgentRecord> list;
    QSqlQuery q("SELECT * FROM agents WHERE status = 1 ORDER BY hostname", m_db);
    while (q.next()) list << rowToAgent(q);
    return list;
}

bool OkidiaDatabase::updateAgentLastSeen(const QString& machineId,
                                         qint64 timestamp) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE agents SET last_seen = ? WHERE id = ?");
    q.addBindValue(timestamp > 0 ? timestamp : OkidiaUtils::nowSecs());
    q.addBindValue(machineId);
    return execQuery(q, "updateAgentLastSeen");
}

bool OkidiaDatabase::updateAgentClass(const QString& machineId,
                                      const QString& classId) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE agents SET class_id = ? WHERE id = ?");
    q.addBindValue(classId.isEmpty()
                   ? QVariant(QMetaType(QMetaType::QString)) : classId);
    q.addBindValue(machineId);
    if (!execQuery(q, "updateAgentClass")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit agentUpdated(machineId);
    return true;
}

bool OkidiaDatabase::updateAgentAlias(const QString& machineId,
                                      const QString& alias) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE agents SET alias = ? WHERE id = ?");
    q.addBindValue(alias);
    q.addBindValue(machineId);
    if (!execQuery(q, "updateAgentAlias")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit agentUpdated(machineId);
    return true;
}

bool OkidiaDatabase::updateAgentType(const QString& machineId, int type) {
    if (!m_open || machineId.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE agents SET agent_type = ? WHERE id = ?");
    q.addBindValue(type);
    q.addBindValue(machineId);
    if (!execQuery(q, "updateAgentType")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit agentUpdated(machineId);
    return true;
}

// ── Blacklist ─────────────────────────────────────────────────────────────

bool OkidiaDatabase::addToBlacklist(const AgentRecord& rec) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE agents SET status = 2, status_reason = ?, approved_by = ?, approved_at = ? WHERE id = ?");
    q.addBindValue(rec.statusReason);
    q.addBindValue(rec.approvedBy);
    q.addBindValue(rec.approvedAt > 0 ? rec.approvedAt : OkidiaUtils::nowSecs());
    q.addBindValue(rec.machineId);
    
    if (!q.exec()) return false;
    
    if (q.numRowsAffected() == 0) {
        // Agent does not exist in agents table, insert it
        AgentRecord newRec = rec;
        newRec.status = 2;
        if (!upsertAgent(newRec)) return false;
    }
    
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit blacklistChanged();
    return true;
}

bool OkidiaDatabase::removeFromBlacklist(const QString& machineId) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE agents SET status = 0, status_reason = '' WHERE id = ? AND status = 2");
    q.addBindValue(machineId);
    if (!execQuery(q, "removeFromBlacklist")) return false;
    emit blacklistChanged();
    return true;
}

bool OkidiaDatabase::isBlacklisted(const QString& machineId) const {
    if (machineId.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM agents WHERE id = ? AND status = 2 LIMIT 1");
    q.addBindValue(machineId);
    return q.exec() && q.next();
}

AgentRecord OkidiaDatabase::blacklistedAgent(const QString& machineId) const {
    if (machineId.isEmpty()) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM agents WHERE id = ? AND status = 2 LIMIT 1");
    q.addBindValue(machineId);
    if (q.exec() && q.next()) return rowToAgent(q);
    return {};
}

QList<AgentRecord> OkidiaDatabase::blacklist() const {
    QList<AgentRecord> list;
    QSqlQuery q("SELECT * FROM agents WHERE status = 2 ORDER BY approved_at DESC", m_db);
    while (q.next()) list << rowToAgent(q);
    return list;
}

// ── Users ─────────────────────────────────────────────────────────────────

bool OkidiaDatabase::upsertUser(const UserRecord& u) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO users(id, username, password_hash, role, display_name,"
        " must_change_pw, is_active, created_by, created_at, last_login, is_backup_replica,"
        " replica_machine_id, replica_hostname,"
        " last_version, last_os, last_os_tag, last_arch)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   username           = excluded.username,"
        "   password_hash      = excluded.password_hash,"
        "   role               = excluded.role,"
        "   display_name       = excluded.display_name,"
        "   must_change_pw     = excluded.must_change_pw,"
        "   is_active          = excluded.is_active,"
        "   is_backup_replica  = excluded.is_backup_replica,"
        "   replica_machine_id = CASE WHEN excluded.is_backup_replica = 0 AND excluded.role != 'YM' THEN '' "
        "                             WHEN excluded.replica_machine_id != '' THEN excluded.replica_machine_id "
        "                             ELSE users.replica_machine_id END,"
        "   replica_hostname   = CASE WHEN excluded.is_backup_replica = 0 AND excluded.role != 'YM' THEN '' "
        "                             WHEN excluded.replica_hostname != '' THEN excluded.replica_hostname "
        "                             ELSE users.replica_hostname END,"
        "   last_version       = excluded.last_version,"
        "   last_os            = excluded.last_os,"
        "   last_os_tag        = excluded.last_os_tag,"
        "   last_arch          = excluded.last_arch");
    q.addBindValue(u.id.isEmpty() ? OkidiaUtils::generateUuid() : u.id);
    q.addBindValue(u.username);
    q.addBindValue(u.passwordHash);
    q.addBindValue(u.role);
    q.addBindValue(u.displayName);
    q.addBindValue(u.mustChangePw ? 1 : 0);
    q.addBindValue(u.isActive ? 1 : 0);
    q.addBindValue(u.createdBy);
    q.addBindValue(u.createdAt > 0 ? u.createdAt : OkidiaUtils::nowSecs());
    q.addBindValue(u.lastLogin > 0 ? QVariant(u.lastLogin) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(u.isBackupReplica ? 1 : 0);
    q.addBindValue(u.replicaMachineId.isNull() ? QString("") : u.replicaMachineId.trimmed());
    q.addBindValue(u.replicaHostname.isNull() ? QString("") : u.replicaHostname.trimmed());
    q.addBindValue(u.lastVersion.isNull() ? QString("") : u.lastVersion);
    q.addBindValue(u.lastOs.isNull() ? QString("") : u.lastOs);
    q.addBindValue(u.lastOsTag.isNull() ? QString("") : u.lastOsTag);
    q.addBindValue(u.lastArch.isNull() ? QString("") : u.lastArch);
    if (!execQuery(q, "upsertUser")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit userChanged(u.id);
    return true;
}

bool OkidiaDatabase::removeUser(const QString& userId) {
    if (userId.isEmpty()) return false;
    
    // İşlem atomik olsun diye bir transaction başlatabiliriz (SQLite için opsiyonel ama iyi uygulama)
    m_db.transaction();

    QSqlQuery q(m_db);

    // 1. Yetkileri kaldır
    revokeAllPermissions(userId);

    // 2. Özel SSH kimliklerini sil
    q.prepare("DELETE FROM ssh_credentials WHERE owner_id = ?");
    q.addBindValue(userId);
    q.exec();

    // 3. Davet kodlarını sil
    q.prepare("DELETE FROM join_codes WHERE user_id = ?");
    q.addBindValue(userId);
    q.exec();

    // 4. Delegasyonları sil (Cascade olması lazım ama garantiye alalım)
    q.prepare("DELETE FROM delegations WHERE from_user = ? OR to_user = ?");
    q.addBindValue(userId);
    q.addBindValue(userId);
    q.exec();

    // 5. Oluşturduğu sınıfları ve odaları sahipsiz bırak
    q.prepare("UPDATE classes SET created_by = NULL WHERE created_by = ?");
    q.addBindValue(userId);
    q.exec();

    q.prepare("UPDATE chat_rooms SET created_by = NULL WHERE created_by = ?");
    q.addBindValue(userId);
    q.exec();

    // 6. Onayladığı agent'ları sahipsiz bırak
    q.prepare("UPDATE agents SET approved_by = NULL WHERE approved_by = ?");
    q.addBindValue(userId);
    q.exec();

    // 7. Kara listeye ekle (Sertifika iptali için)
    UserRecord u = userById(userId);
    AgentRecord br;
    br.machineId = userId;
    br.hostname  = u.username;
    br.statusReason = "USER_DELETED";
    br.approvedBy   = "System";
    br.approvedAt   = OkidiaUtils::nowSecs();
    addToBlacklist(br);

    // 8. Kullanıcıyı sil
    q.prepare("DELETE FROM users WHERE id = ?");
    q.addBindValue(userId);
    bool ok = execQuery(q, "removeUser");

    if (ok) {
        m_db.commit();
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
        emit userChanged(userId);
        return true;
    } else {
        m_db.rollback();
        return false;
    }
}

bool OkidiaDatabase::setUserPassword(const QString& userId,
                                     const QString& hash,
                                     bool mustChange) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE users SET password_hash = ?, must_change_pw = ? WHERE id = ?");
    q.addBindValue(hash);
    q.addBindValue(mustChange ? 1 : 0);
    q.addBindValue(userId);
    return execQuery(q, "setUserPassword");
}

bool OkidiaDatabase::updateLastLogin(const QString& userId,
                                    const QString& version,
                                    const QString& osPrettyName,
                                    const QString& osTag,
                                    const QString& arch) {
    QSqlQuery q(m_db);
    if (!version.isEmpty() || !osPrettyName.isEmpty() || !osTag.isEmpty() || !arch.isEmpty()) {
        q.prepare("UPDATE users SET last_login = ?, "
                  "last_version = CASE WHEN ? != '' THEN ? ELSE last_version END, "
                  "last_os = CASE WHEN ? != '' THEN ? ELSE last_os END, "
                  "last_os_tag = CASE WHEN ? != '' THEN ? ELSE last_os_tag END, "
                  "last_arch = CASE WHEN ? != '' THEN ? ELSE last_arch END WHERE id = ?");
        q.addBindValue(OkidiaUtils::nowSecs());
        q.addBindValue(version);
        q.addBindValue(version);
        q.addBindValue(osPrettyName);
        q.addBindValue(osPrettyName);
        q.addBindValue(osTag);
        q.addBindValue(osTag);
        q.addBindValue(arch);
        q.addBindValue(arch);
        q.addBindValue(userId);
    } else {
        q.prepare("UPDATE users SET last_login = ? WHERE id = ?");
        q.addBindValue(OkidiaUtils::nowSecs());
        q.addBindValue(userId);
    }
    bool ok = execQuery(q, "updateLastLogin");
    if (ok) {
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
        emit userChanged(userId);
    }
    return ok;
}

UserRecord OkidiaDatabase::userById(const QString& userId) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM users WHERE id = ?");
    q.addBindValue(userId);
    if (q.exec() && q.next()) return rowToUser(q);
    return {};
}

UserRecord OkidiaDatabase::userByUsername(const QString& username) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM users WHERE username = ? COLLATE NOCASE");
    q.addBindValue(username);
    if (q.exec() && q.next()) return rowToUser(q);
    return {};
}

QList<UserRecord> OkidiaDatabase::allUsers() const {
    QList<UserRecord> list;
    QSqlQuery q("SELECT * FROM users ORDER BY username", m_db);
    while (q.next()) list << rowToUser(q);
    return list;
}

bool OkidiaDatabase::setUserBackupReplica(const QString& userId,
                                          bool isReplica,
                                          const QString& replicaMachineId,
                                          const QString& replicaHostname) {
    QSqlQuery q(m_db);
    if (!isReplica) {
        q.prepare("UPDATE users SET is_backup_replica = 0, replica_machine_id = '', replica_hostname = '' WHERE id = ?");
        q.addBindValue(userId);
    } else if (replicaHostname.trimmed() == "__AUTO__") {
        q.prepare("UPDATE users SET is_backup_replica = 1, replica_machine_id = '', replica_hostname = '' WHERE id = ?");
        q.addBindValue(userId);
    } else if (!replicaHostname.trimmed().isEmpty() || !replicaMachineId.trimmed().isEmpty()) {
        q.prepare("UPDATE users SET is_backup_replica = 1, replica_machine_id = ?, replica_hostname = ? WHERE id = ?");
        q.addBindValue(replicaMachineId.trimmed());
        q.addBindValue(replicaHostname.trimmed());
        q.addBindValue(userId);
    } else {
        q.prepare("UPDATE users SET is_backup_replica = 1 WHERE id = ?");
        q.addBindValue(userId);
    }
    bool ok = execQuery(q, "setUserBackupReplica");
    if (ok) {
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
        emit userChanged(userId);
    }
    return ok;
}

bool OkidiaDatabase::isUserBackupReplica(const QString& userId) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT is_backup_replica FROM users WHERE id = ?");
    q.addBindValue(userId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() != 0;
    }
    return false;
}

bool OkidiaDatabase::isReplicaDeviceAuthorized(const QString& userId,
                                               const QString& machineId,
                                               const QString& hostname) const {
    if (userId.isEmpty()) return false;
    const UserRecord u = userById(userId);
    if (u.id.isEmpty() || !u.isActive) return false;
    if (u.role != "YM" && !u.isBackupReplica) return false;

    const QString boundHost = u.replicaHostname.trimmed();
    const QString boundMid  = u.replicaMachineId.trimmed();
    const QString connHost  = hostname.trimmed();
    const QString connMid   = machineId.trimmed();

    // Kullanıcıya atanmış belirli bir yedek cihazı (hostname veya machine_id) varsa birebir eşleşme şarttır
    if (!boundHost.isEmpty()) {
        if (!connHost.isEmpty() && connHost.compare(boundHost, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (!boundMid.isEmpty() && !connMid.isEmpty() && connMid == boundMid) {
            return true;
        }
        return false;
    }
    if (!boundMid.isEmpty()) {
        return (!connMid.isEmpty() && connMid == boundMid);
    }

    // Henüz hiçbir cihaz bağlanmamışsa, giriş yapılan bilgisayarın başka bir kullanıcının
    // tanımlı yedek bilgisayarı olup olmadığını kontrol et
    if (!connHost.isEmpty()) {
        QSqlQuery q(m_db);
        q.prepare("SELECT 1 FROM users WHERE id != ? AND LOWER(replica_hostname) = LOWER(?) LIMIT 1");
        q.addBindValue(userId);
        q.addBindValue(connHost);
        if (q.exec() && q.next()) {
            return false;
        }
    }
    return true;
}

QList<QString> OkidiaDatabase::backupReplicaUserIds() const {
    QList<QString> res;
    QSqlQuery q("SELECT id FROM users WHERE (role = 'YM' OR is_backup_replica = 1) AND is_active = 1", m_db);
    while (q.next()) {
        res << q.value(0).toString();
    }
    return res;
}

bool OkidiaDatabase::demoteOtherYmUsers(const QString& exceptUserId) {
    QSqlQuery q(m_db);
    if (!exceptUserId.isEmpty()) {
        q.prepare("UPDATE users SET role = 'OBSERVER', is_backup_replica = 0, replica_machine_id = '', replica_hostname = '' WHERE role = 'YM' AND id != ?");
        q.addBindValue(exceptUserId);
    } else {
        q.prepare("UPDATE users SET role = 'OBSERVER', is_backup_replica = 0, replica_machine_id = '', replica_hostname = '' WHERE role = 'YM'");
    }
    bool ok = execQuery(q, "demoteOtherYmUsers");
    if (ok) {
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    }
    return ok;
}

// ── Master Devices (Fiziksel Master Bilgisayarları) ───────────────────────

bool OkidiaDatabase::upsertMasterDevice(const MasterDeviceRecord& dev) {
    QString hostClean = dev.hostname.trimmed();
    QString effectiveId = dev.machineId.trimmed();
    if (!hostClean.isEmpty()) {
        // Klonlanmış VM'lerde /etc/machine-id aynı olabileceği için her Master bilgisayarını
        // hostname bazında benzersiz ve kararlı şekilde takip et; aynı hostname'e ait eski kayıtları birleştir.
        effectiveId = QUuid::createUuidV5(QUuid(), "master-host:" + hostClean.toLower()).toString(QUuid::WithoutBraces);
        QSqlQuery qCleanup(m_db);
        qCleanup.prepare("DELETE FROM master_devices WHERE LOWER(hostname) = LOWER(?) AND machine_id != ?");
        qCleanup.addBindValue(hostClean);
        qCleanup.addBindValue(effectiveId);
        qCleanup.exec();
    }
    if (effectiveId.isEmpty()) return false;

    QSqlQuery q(m_db);
    q.prepare(R"(
        INSERT INTO master_devices (machine_id, hostname, ip, os_pretty_name, os_tag, os_arch, master_ver, last_user_id, last_username, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(machine_id) DO UPDATE SET
            hostname = excluded.hostname,
            ip = CASE WHEN excluded.ip != '' THEN excluded.ip ELSE master_devices.ip END,
            os_pretty_name = CASE WHEN excluded.os_pretty_name != '' THEN excluded.os_pretty_name ELSE master_devices.os_pretty_name END,
            os_tag = CASE WHEN excluded.os_tag != '' THEN excluded.os_tag ELSE master_devices.os_tag END,
            os_arch = CASE WHEN excluded.os_arch != '' THEN excluded.os_arch ELSE master_devices.os_arch END,
            master_ver = CASE WHEN excluded.master_ver != '' THEN excluded.master_ver ELSE master_devices.master_ver END,
            last_user_id = CASE WHEN excluded.last_user_id != '' THEN excluded.last_user_id ELSE master_devices.last_user_id END,
            last_username = CASE WHEN excluded.last_username != '' THEN excluded.last_username ELSE master_devices.last_username END,
            last_seen = excluded.last_seen
    )");
    q.addBindValue(effectiveId);
    q.addBindValue(hostClean);
    q.addBindValue(dev.ip);
    q.addBindValue(dev.osPrettyName);
    q.addBindValue(OkidiaUtils::normalizeOsTag(dev.osTag));
    q.addBindValue(OkidiaUtils::normalizeArch(dev.osArch));
    q.addBindValue(dev.masterVer);
    q.addBindValue(dev.lastUserId);
    q.addBindValue(dev.lastUsername);
    q.addBindValue(dev.lastSeen > 0 ? dev.lastSeen : OkidiaUtils::nowSecs());
    bool ok = execQuery(q, "upsertMasterDevice");
    if (ok) {
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    }
    return ok;
}

MasterDeviceRecord OkidiaDatabase::masterDeviceById(const QString& machineId) const {
    if (machineId.isEmpty()) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM master_devices WHERE machine_id = ? OR LOWER(hostname) = LOWER(?)");
    q.addBindValue(machineId);
    q.addBindValue(machineId);
    if (q.exec() && q.next()) return rowToMasterDevice(q);
    return {};
}

QList<MasterDeviceRecord> OkidiaDatabase::allMasterDevices() const {
    QList<MasterDeviceRecord> list;
    QSet<QString> seenHosts;
    // En son görülen kaydı önceliklendirerek aynı hostname tekrarlarını filtrele
    QSqlQuery q("SELECT * FROM master_devices ORDER BY last_seen DESC, hostname ASC", m_db);
    while (q.next()) {
        MasterDeviceRecord rec = rowToMasterDevice(q);
        QString key = rec.hostname.trimmed().toLower();
        if (key.isEmpty()) key = rec.machineId;
        if (seenHosts.contains(key)) continue;
        seenHosts.insert(key);
        list << rec;
    }
    std::sort(list.begin(), list.end(), [](const MasterDeviceRecord& a, const MasterDeviceRecord& b) {
        return OkidiaUtils::naturalCompare(a.hostname, b.hostname) < 0;
    });
    return list;
}

// ── Teachers (Akıllı Tahta Branş Öğretmenleri) ────────────────────────────

bool OkidiaDatabase::upsertTeacher(const TeacherRecord& t) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO teachers(id, username, display_name, branch, seed_hex, password,"
        " is_active, created_by, created_at, updated_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   username     = excluded.username,"
        "   display_name = excluded.display_name,"
        "   branch       = excluded.branch,"
        "   seed_hex     = excluded.seed_hex,"
        "   password     = CASE WHEN excluded.password != '' THEN excluded.password ELSE teachers.password END,"
        "   is_active    = excluded.is_active,"
        "   updated_at   = excluded.updated_at");
    const qint64 now = OkidiaUtils::nowSecs();
    q.addBindValue(t.id.isEmpty() ? OkidiaUtils::generateUuid() : t.id);
    q.addBindValue(t.username);
    q.addBindValue(t.displayName);
    q.addBindValue(t.branch);
    q.addBindValue(t.seedHex);
    q.addBindValue(t.password);
    q.addBindValue(t.isActive ? 1 : 0);
    q.addBindValue(t.createdBy);
    q.addBindValue(t.createdAt > 0 ? t.createdAt : now);
    q.addBindValue(now);
    if (!execQuery(q, "upsertTeacher")) return false;
    setMetaValue("last_modified", QString::number(now));
    emit teacherChanged(t.id);
    return true;
}

bool OkidiaDatabase::removeTeacher(const QString& teacherId) {
    if (teacherId.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM teachers WHERE id = ?");
    q.addBindValue(teacherId);
    bool ok = execQuery(q, "removeTeacher");
    if (ok) {
        setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
        emit teacherChanged(teacherId);
    }
    return ok;
}

TeacherRecord OkidiaDatabase::teacherById(const QString& teacherId) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM teachers WHERE id = ?");
    q.addBindValue(teacherId);
    if (q.exec() && q.next()) return rowToTeacher(q);
    return {};
}

TeacherRecord OkidiaDatabase::teacherByUsername(const QString& username) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM teachers WHERE username = ?");
    q.addBindValue(username);
    if (q.exec() && q.next()) return rowToTeacher(q);
    return {};
}

QList<TeacherRecord> OkidiaDatabase::allTeachers() const {
    QList<TeacherRecord> list;
    QSqlQuery q("SELECT * FROM teachers ORDER BY display_name", m_db);
    while (q.next()) list << rowToTeacher(q);
    return list;
}

// ── Classes ───────────────────────────────────────────────────────────────

bool OkidiaDatabase::upsertClass(const ClassRecord& c) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO classes(id, name, icon, type, parent_id,"
        " subnet_hint, created_by, created_at, updated_at)"
        " VALUES(?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   name        = excluded.name,"
        "   icon        = excluded.icon,"
        "   type        = excluded.type,"
        "   parent_id   = excluded.parent_id,"
        "   subnet_hint = excluded.subnet_hint,"
        "   updated_at  = excluded.updated_at");
    const qint64 now = OkidiaUtils::nowSecs();
    q.addBindValue(c.id.isEmpty() ? OkidiaUtils::generateUuid() : c.id);
    q.addBindValue(c.name);
    q.addBindValue(c.icon);
    q.addBindValue(c.type);
    q.addBindValue(c.parentId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : c.parentId);
    q.addBindValue(c.subnetHint.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : c.subnetHint);
    q.addBindValue(c.createdBy);
    q.addBindValue(c.createdAt > 0 ? c.createdAt : now);
    q.addBindValue(now);
    if (!execQuery(q, "upsertClass")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit classChanged(c.id);
    return true;
}

bool OkidiaDatabase::removeClass(const QString& classId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM classes WHERE id = ?");
    q.addBindValue(classId);
    if (!execQuery(q, "removeClass")) return false;

    // Bağlı sohbet odası kaydını da temizle
    QSqlQuery qRoom(m_db);
    qRoom.prepare("DELETE FROM chat_rooms WHERE class_id = ? OR id = ?");
    qRoom.addBindValue(classId);
    qRoom.addBindValue("room_" + classId);
    execQuery(qRoom, "removeClass_chatRoom");

    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit classChanged(classId);
    return true;
}

ClassRecord OkidiaDatabase::classById(const QString& classId) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM classes WHERE id = ?");
    q.addBindValue(classId);
    if (q.exec() && q.next()) return rowToClass(q);
    return {};
}

QList<ClassRecord> OkidiaDatabase::allClasses() const {
    QList<ClassRecord> list;
    QSqlQuery q("SELECT * FROM classes ORDER BY type, name", m_db);
    while (q.next()) list << rowToClass(q);
    return list;
}

QList<ClassRecord> OkidiaDatabase::childClasses(const QString& parentId) const {
    QList<ClassRecord> list;
    QSqlQuery q(m_db);
    if (parentId.isEmpty()) {
        q.prepare("SELECT * FROM classes WHERE parent_id IS NULL ORDER BY name");
    } else {
        q.prepare("SELECT * FROM classes WHERE parent_id = ? ORDER BY name");
        q.addBindValue(parentId);
    }
    if (q.exec()) while (q.next()) list << rowToClass(q);
    return list;
}

// ── User-Class permissions ────────────────────────────────────────────────

bool OkidiaDatabase::grantPermission(const QString& userId,
                                     const QString& classId,
                                     const QString& permission,
                                     const QString& grantedBy) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT OR REPLACE INTO user_classes"
        "(user_id, class_id, permission, granted_by, granted_at)"
        " VALUES(?,?,?,?,?)");
    q.addBindValue(userId);
    q.addBindValue(classId);
    q.addBindValue(permission);
    q.addBindValue(grantedBy);
    q.addBindValue(OkidiaUtils::nowSecs());
    if (!execQuery(q, "grantPermission")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    return true;
}

bool OkidiaDatabase::revokePermission(const QString& userId,
                                      const QString& classId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM user_classes WHERE user_id = ? AND class_id = ?");
    q.addBindValue(userId);
    q.addBindValue(classId);
    return execQuery(q, "revokePermission");
}

bool OkidiaDatabase::revokeAllPermissions(const QString& userId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM user_classes WHERE user_id = ?");
    q.addBindValue(userId);
    return execQuery(q, "revokeAllPermissions");
}

QList<UserClassRecord> OkidiaDatabase::permissionsForUser(
    const QString& userId) const {
    QList<UserClassRecord> list;
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM user_classes WHERE user_id = ?");
    q.addBindValue(userId);
    if (q.exec()) while (q.next()) list << rowToUserClass(q);

    // Hiyerarşik Miras: Ana sınıfa "full" yetki verilmişse alt gruplara da türetilmiş yetki üret
    if (!list.isEmpty()) {
        const QList<ClassRecord> all = allClasses();
        QSet<QString> existingClassIds;
        for (const auto& rec : list) existingClassIds.insert(rec.classId);

        bool added = true;
        while (added) {
            added = false;
            for (const auto& c : all) {
                if (!c.parentId.isEmpty() && existingClassIds.contains(c.parentId) && !existingClassIds.contains(c.id)) {
                    UserClassRecord derived;
                    derived.userId = userId;
                    derived.classId = c.id;
                    derived.permission = "full";
                    derived.grantedBy = "inheritance";
                    derived.grantedAt = OkidiaUtils::nowSecs();
                    list << derived;
                    existingClassIds.insert(c.id);
                    added = true;
                }
            }
        }
    }
    return list;
}

QStringList OkidiaDatabase::classIdsForUser(const QString& userId) const {
    QStringList ids;
    QSqlQuery q(m_db);
    q.prepare("SELECT class_id FROM user_classes WHERE user_id = ?");
    q.addBindValue(userId);
    if (q.exec()) while (q.next()) ids << q.value(0).toString();

    // Hiyerarşik Miras: Üst sınıfa yetki verilmişse altındaki tüm grup ve sınıflar yetkilidir
    if (!ids.isEmpty()) {
        const QList<ClassRecord> all = allClasses();
        bool added = true;
        while (added) {
            added = false;
            for (const auto& c : all) {
                if (!c.parentId.isEmpty() && ids.contains(c.parentId) && !ids.contains(c.id)) {
                    ids << c.id;
                    added = true;
                }
            }
        }
    }
    return ids;
}

QStringList OkidiaDatabase::userIdsForClass(const QString& classId) const {
    QSet<QString> searchClassIds;
    searchClassIds.insert(classId);

    // Üst sınıfları bul (Parent hierarchy mirası)
    QString cur = classId;
    QSet<QString> visited;
    while (!cur.isEmpty() && !visited.contains(cur)) {
        visited.insert(cur);
        ClassRecord cr = classById(cur);
        if (cr.parentId.isEmpty()) break;
        searchClassIds.insert(cr.parentId);
        cur = cr.parentId;
    }

    QStringList ids;
    for (const QString& cid : searchClassIds) {
        QSqlQuery q(m_db);
        q.prepare("SELECT user_id FROM user_classes WHERE class_id = ?");
        q.addBindValue(cid);
        if (q.exec()) {
            while (q.next()) {
                QString uid = q.value(0).toString();
                if (!ids.contains(uid)) ids << uid;
            }
        }
    }
    return ids;
}

bool OkidiaDatabase::isUserAuthorizedForClass(const QString& userId, const QString& classId) const {
    if (userId.isEmpty() || classId.isEmpty() || classId == "__root__") return false;
    UserRecord u = userById(userId);
    if (u.id.isEmpty()) return false;
    if (u.role == "YM") return true;
    if (u.role == "OBSERVER" || u.role == "GM") return false;

    // OM rolü: Doğrudan veya alt grup hiyerarşisiyle atanmış sınıflar
    const QStringList allowed = classIdsForUser(userId);
    if (allowed.contains(classId)) return true;

    // Aktif delegasyonlar kontrolü (üst sınıfa delegasyon alt sınıflara da geçer)
    const QList<DelegationRecord> delegations = activeDelegationsForUser(userId);
    for (const auto& d : delegations) {
        if (d.classId == classId) return true;
        QString cur = classId;
        QSet<QString> visited;
        while (!cur.isEmpty() && !visited.contains(cur)) {
            visited.insert(cur);
            ClassRecord cr = classById(cur);
            if (cr.parentId.isEmpty()) break;
            if (cr.parentId == d.classId) return true;
            cur = cr.parentId;
        }
    }
    return false;
}

bool OkidiaDatabase::isUserAuthorizedForClient(const QString& userId, const QString& clientId) const {
    if (userId.isEmpty() || clientId.isEmpty()) return false;
    UserRecord u = userById(userId);
    if (u.id.isEmpty()) return false;
    if (u.role == "YM") return true;
    if (u.role == "OBSERVER" || u.role == "GM") return false;

    AgentRecord ag = agent(clientId);
    if (ag.classId.isEmpty() || ag.classId == "__root__") {
        return false; // OM atanmamış istemcileri yönetemez; atanmamışlar sadece YM'ye aittir
    }
    return isUserAuthorizedForClass(userId, ag.classId);
}

// ── Delegations ───────────────────────────────────────────────────────────

bool OkidiaDatabase::addDelegation(const DelegationRecord& d) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO delegations"
        "(id, from_user, to_user, class_id, type, starts_at, ends_at, created_by, created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?)");
    q.addBindValue(d.id.isEmpty() ? OkidiaUtils::generateUuid() : d.id);
    q.addBindValue(d.fromUser);
    q.addBindValue(d.toUser);
    q.addBindValue(d.classId);
    q.addBindValue(d.type);
    q.addBindValue(d.startsAt);
    q.addBindValue(d.endsAt);
    q.addBindValue(d.createdBy);
    q.addBindValue(d.createdAt > 0 ? d.createdAt : OkidiaUtils::nowSecs());
    return execQuery(q, "addDelegation");
}

bool OkidiaDatabase::removeDelegation(const QString& delegationId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM delegations WHERE id = ?");
    q.addBindValue(delegationId);
    return execQuery(q, "removeDelegation");
}

QList<DelegationRecord> OkidiaDatabase::activeDelegationsForUser(
    const QString& toUserId) const {
    QList<DelegationRecord> list;
    const qint64 now = OkidiaUtils::nowSecs();
    QSqlQuery q(m_db);
    q.prepare(
        "SELECT * FROM delegations"
        " WHERE to_user = ? AND starts_at <= ? AND ends_at > ?"
        " ORDER BY ends_at");
    q.addBindValue(toUserId);
    q.addBindValue(now);
    q.addBindValue(now);
    if (q.exec()) while (q.next()) list << rowToDelegation(q);
    return list;
}

// ── Chat lock state ───────────────────────────────────────────────────────────

bool OkidiaDatabase::setChatLockState(const QString& classId,
                                       bool chatLocked, bool privateLocked) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO chat_room_state(class_id, chat_locked, private_locked, updated_at)"
              " VALUES(?,?,?,?)"
              " ON CONFLICT(class_id) DO UPDATE SET"
              "   chat_locked=excluded.chat_locked,"
              "   private_locked=excluded.private_locked,"
              "   updated_at=excluded.updated_at");
    q.addBindValue(classId);
    q.addBindValue(chatLocked    ? 1 : 0);
    q.addBindValue(privateLocked ? 1 : 0);
    q.addBindValue(OkidiaUtils::nowSecs());
    return execQuery(q, "setChatLockState");
}



ChatLockRecord OkidiaDatabase::chatLockState(const QString& classId) const {
    ChatLockRecord r;
    r.classId = classId;
    if (!m_open) return r;
    QSqlQuery q(m_db);
    q.prepare("SELECT chat_locked, private_locked, updated_at"
              " FROM chat_room_state WHERE class_id=?");
    q.addBindValue(classId);
    if (q.exec() && q.next()) {
        r.chatLocked    = q.value(0).toInt() != 0;
        r.privateLocked = q.value(1).toInt() != 0;
        r.updatedAt     = q.value(2).toLongLong();
    }
    return r;
}

QList<ChatLockRecord> OkidiaDatabase::allChatLockStates() const {
    QList<ChatLockRecord> list;
    if (!m_open) return list;
    QSqlQuery q(m_db);
    q.exec("SELECT class_id, chat_locked, private_locked, updated_at FROM chat_room_state");
    while (q.next()) {
        ChatLockRecord r;
        r.classId       = q.value(0).toString();
        r.chatLocked    = q.value(1).toInt() != 0;
        r.privateLocked = q.value(2).toInt() != 0;
        r.updatedAt     = q.value(3).toLongLong();
        list << r;
    }
    return list;
}

// ── Pending commands ──────────────────────────────────────────────────────────

bool OkidiaDatabase::addPendingCommand(const QString& machineId,
                                        int msgType,
                                        const QByteArray& payload,
                                        int ttlSeconds) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO pending_commands(machine_id, msg_type, payload, expires_at)"
              " VALUES(?,?,?,?)");
    q.addBindValue(machineId);
    q.addBindValue(msgType);
    q.addBindValue(payload);
    q.addBindValue(OkidiaUtils::nowSecs() + ttlSeconds);
    return execQuery(q, "addPendingCommand");
}

QList<PendingCommandRecord> OkidiaDatabase::pendingCommands(const QString& machineId) const {
    QList<PendingCommandRecord> list;
    if (!m_open) return list;
    QSqlQuery q(m_db);
    q.prepare("SELECT id, machine_id, msg_type, payload, created_at, expires_at"
              " FROM pending_commands WHERE machine_id=? AND expires_at > ?"
              " ORDER BY id ASC");
    q.addBindValue(machineId);
    q.addBindValue(OkidiaUtils::nowSecs());
    if (!q.exec()) return list;
    while (q.next()) {
        PendingCommandRecord r;
        r.id        = q.value(0).toLongLong();
        r.machineId = q.value(1).toString();
        r.msgType   = q.value(2).toInt();
        r.payload   = q.value(3).toByteArray();
        r.createdAt = q.value(4).toLongLong();
        r.expiresAt = q.value(5).toLongLong();
        list << r;
    }
    return list;
}

bool OkidiaDatabase::deletePendingCommand(qint64 id) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM pending_commands WHERE id=?");
    q.addBindValue(id);
    return execQuery(q, "deletePendingCommand");
}

void OkidiaDatabase::purgePendingCommands() {
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM pending_commands WHERE expires_at <= ?");
    q.addBindValue(OkidiaUtils::nowSecs());
    execQuery(q, "purgePendingCommands");
}

// ── Brokers ───────────────────────────────────────────────────────────────

bool OkidiaDatabase::upsertBroker(const BrokerRecord& b) {
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO brokers(id, hostname, ip, role, cert_pem, last_seen, created_at)"
        " VALUES(?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   hostname  = excluded.hostname,"
        "   ip        = excluded.ip,"
        "   role      = excluded.role,"
        "   cert_pem  = excluded.cert_pem,"
        "   last_seen = excluded.last_seen");
    q.addBindValue(b.id.isEmpty() ? OkidiaUtils::generateUuid() : b.id);
    q.addBindValue(b.hostname);
    q.addBindValue(b.ip);
    q.addBindValue(b.role);
    q.addBindValue(b.certPem);
    q.addBindValue(b.lastSeen > 0 ? b.lastSeen : OkidiaUtils::nowSecs());
    q.addBindValue(b.createdAt > 0 ? b.createdAt : OkidiaUtils::nowSecs());
    return execQuery(q, "upsertBroker");
}

QList<BrokerRecord> OkidiaDatabase::allBrokers() const {
    QList<BrokerRecord> list;
    QSqlQuery q("SELECT * FROM brokers ORDER BY role, hostname", m_db);
    while (q.next()) {
        BrokerRecord b;
        b.id        = q.value("id").toString();
        b.hostname  = q.value("hostname").toString();
        b.ip        = q.value("ip").toString();
        b.role      = q.value("role").toString();
        b.certPem   = q.value("cert_pem").toString();
        b.lastSeen  = q.value("last_seen").toLongLong();
        b.createdAt = q.value("created_at").toLongLong();
        list << b;
    }
    return list;
}

// ── SSH Kimlikleri ────────────────────────────────────────────────────────

bool OkidiaDatabase::upsertSshCredential(const SshCredentialRecord& cred) {
    if (!m_open) return false;
    
    SshCredentialRecord c = cred;
    // Eğer bu bir agent hedefiyse ve machine_id atanmamışsa, o anki IP'den (targetId) UUID'yi bulmaya çalış
    if (c.targetType == "agent" && c.machineId.isEmpty()) {
        QSqlQuery qAgent(m_db);
        qAgent.prepare("SELECT id FROM agents WHERE ip = ? LIMIT 1");
        qAgent.addBindValue(c.targetId);
        if (qAgent.exec() && qAgent.next()) {
            c.machineId = qAgent.value(0).toString();
        }
    }
    
    QSqlQuery q(m_db);
    q.prepare(R"(
        INSERT INTO ssh_credentials (id, name, username, secret, is_key, owner_id, target_id, target_type, machine_id, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            name=excluded.name,
            username=excluded.username,
            secret=excluded.secret,
            is_key=excluded.is_key,
            owner_id=excluded.owner_id,
            target_id=excluded.target_id,
            target_type=excluded.target_type,
            machine_id=excluded.machine_id
    )");
    q.addBindValue(c.id);
    q.addBindValue(c.name);
    q.addBindValue(c.username);
    q.addBindValue(c.secret);
    q.addBindValue(c.isKey ? 1 : 0);
    q.addBindValue(c.ownerUserId.isEmpty() ? QString("") : c.ownerUserId);
    q.addBindValue(c.targetId.isEmpty() ? QString("") : c.targetId);
    q.addBindValue(c.targetType.isEmpty() ? QString("") : c.targetType);
    q.addBindValue(c.machineId.isEmpty() ? QString("") : c.machineId);
    q.addBindValue(c.createdAt > 0 ? c.createdAt : OkidiaUtils::nowSecs());
    return execQuery(q, "upsertSshCredential");
}

bool OkidiaDatabase::deleteSshCredential(const QString& id) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM ssh_credentials WHERE id = ?");
    q.addBindValue(id);
    return execQuery(q, "deleteSshCredential");
}

SshCredentialRecord OkidiaDatabase::sshCredentialById(const QString& id) const {
    SshCredentialRecord r;
    if (!m_open) return r;
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM ssh_credentials WHERE id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        r.id          = q.value("id").toString();
        r.name        = q.value("name").toString();
        r.username    = q.value("username").toString();
        r.secret      = q.value("secret").toByteArray();
        r.isKey       = q.value("is_key").toInt() != 0;
        r.ownerUserId = q.value("owner_id").toString();
        r.targetId    = q.value("target_id").toString();
        r.targetType  = q.value("target_type").toString();
        r.machineId   = q.value("machine_id").toString();
        r.createdAt   = q.value("created_at").toLongLong();
    }
    return r;
}

QList<SshCredentialRecord> OkidiaDatabase::sshCredentials(const QString& ownerUserId) const {
    QList<SshCredentialRecord> list;
    if (!m_open) return list;

    // Kullanıcı rolünü bul
    UserRecord user = userById(ownerUserId);
    bool isYM = (user.role == "YM");

    QSqlQuery q(m_db);
    QString sql;

    if (isYM) {
        // YM her şeyi görebilir
        sql = "SELECT * FROM ssh_credentials ORDER BY name ASC";
        q.prepare(sql);
    } else if (!ownerUserId.isEmpty()) {
        // OM:
        // 1. Kendi özel kimlikleri (owner_id = user.id)
        // 2. Ortak kimlikler (owner_id = '')
        // 3. Sorumlu olduğu sınıflara atanmış kimlikler (target_type='class' AND target_id IN managed_classes)
        // 4. Sorumlu olduğu sınıflardaki agent'lara atanmış kimlikler (target_type='agent' AND target_id IN agents_of_managed_classes)
        
        sql = R"(
            SELECT * FROM ssh_credentials 
            WHERE owner_id = ? 
               OR owner_id = ''
               OR (target_type = 'class' AND target_id IN (SELECT class_id FROM user_classes WHERE user_id = ?))
               OR (target_type = 'agent' AND target_id IN (SELECT id FROM agents WHERE class_id IN (SELECT class_id FROM user_classes WHERE user_id = ?)))
            ORDER BY name ASC
        )";
        q.prepare(sql);
        q.addBindValue(ownerUserId);
        q.addBindValue(ownerUserId);
        q.addBindValue(ownerUserId);
    } else {
        // Anonim/Diğer: Sadece ortak kimlikler
        sql = "SELECT * FROM ssh_credentials WHERE owner_id = '' ORDER BY name ASC";
        q.prepare(sql);
    }

    if (q.exec()) {
        while (q.next()) {
            SshCredentialRecord r;
            r.id          = q.value("id").toString();
            r.name        = q.value("name").toString();
            r.username    = q.value("username").toString();
            r.secret      = q.value("secret").toByteArray();
            r.isKey       = q.value("is_key").toInt() != 0;
            r.ownerUserId = q.value("owner_id").toString();
            r.targetId    = q.value("target_id").toString();
            r.targetType  = q.value("target_type").toString();
            r.machineId   = q.value("machine_id").toString();
            r.createdAt   = q.value("created_at").toLongLong();
            list << r;
        }
    }
    return list;
}

// ── Yardımcı: sorgu çalıştır ──────────────────────────────────────────────

bool OkidiaDatabase::execQuery(const QString& sql) const {
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        Logger::error(QObject::tr("OkidiaDatabase: SQL hatası → ")
                      + q.lastError().text() + " | " + sql.left(80));
        return false;
    }
    return true;
}

bool OkidiaDatabase::execQuery(QSqlQuery& q, const QString& ctx) const {
    if (!q.exec()) {
        Logger::error(QObject::tr("OkidiaDatabase: ") + (ctx.isEmpty() ? QObject::tr("query") : ctx)
                      + " → " + q.lastError().text());
        return false;
    }
    return true;
}

// ── Row → struct dönüşümleri ──────────────────────────────────────────────

AgentRecord OkidiaDatabase::rowToAgent(const QSqlQuery& q) const {
    AgentRecord a;
    a.machineId  = q.value("id").toString();
    a.hostname   = q.value("hostname").toString();
    a.mac        = q.value("mac").toString();
    a.ip         = q.value("ip").toString();
    a.osInfo     = q.value("os_info").toString();
    a.osPrettyName = q.value("os_pretty_name").toString();
    a.osArch     = q.value("os_arch").toString();
    a.agentVer   = q.value("agent_ver").toString();
    a.classId    = q.value("class_id").toString();
    a.certPem    = q.value("cert_pem").toString();
    a.agentType  = q.value("agent_type").toInt();
    a.alias      = q.value("alias").toString();
    a.approvedBy = q.value("approved_by").toString();
    a.approvedAt = q.value("approved_at").toLongLong();
    a.lastSeen   = q.value("last_seen").toLongLong();
    a.status     = q.value("status").toInt();
    a.statusReason = q.value("status_reason").toString();
    return a;
}

UserRecord OkidiaDatabase::rowToUser(const QSqlQuery& q) const {
    UserRecord u;
    u.id           = q.value("id").toString();
    u.username     = q.value("username").toString();
    u.passwordHash = q.value("password_hash").toString();
    u.role         = q.value("role").toString();
    u.displayName  = q.value("display_name").toString();
    u.mustChangePw = q.value("must_change_pw").toInt() != 0;
    u.isActive     = q.value("is_active").toInt() != 0;
    u.createdBy    = q.value("created_by").toString();
    u.createdAt    = q.value("created_at").toLongLong();
    u.lastLogin    = q.value("last_login").toLongLong();
    u.isBackupReplica = q.value("is_backup_replica").toInt() != 0;
    u.replicaMachineId = q.value("replica_machine_id").toString();
    u.replicaHostname  = q.value("replica_hostname").toString();
    u.lastVersion  = q.value("last_version").toString();
    u.lastOs       = q.value("last_os").toString();
    u.lastOsTag    = q.value("last_os_tag").toString();
    u.lastArch     = q.value("last_arch").toString();
    return u;
}

MasterDeviceRecord OkidiaDatabase::rowToMasterDevice(const QSqlQuery& q) const {
    MasterDeviceRecord m;
    m.machineId    = q.value("machine_id").toString();
    m.hostname     = q.value("hostname").toString();
    m.ip           = q.value("ip").toString();
    m.osPrettyName = q.value("os_pretty_name").toString();
    m.osTag        = q.value("os_tag").toString();
    m.osArch       = q.value("os_arch").toString();
    m.masterVer    = q.value("master_ver").toString();
    m.lastUserId   = q.value("last_user_id").toString();
    m.lastUsername = q.value("last_username").toString();
    m.lastSeen     = q.value("last_seen").toLongLong();
    return m;
}

TeacherRecord OkidiaDatabase::rowToTeacher(const QSqlQuery& q) const {
    TeacherRecord t;
    t.id          = q.value("id").toString();
    t.username    = q.value("username").toString();
    t.displayName = q.value("display_name").toString();
    t.branch      = q.value("branch").toString();
    t.seedHex     = q.value("seed_hex").toString();
    t.password    = q.value("password").toString();
    t.isActive    = q.value("is_active").toInt() != 0;
    t.createdBy   = q.value("created_by").toString();
    t.createdAt   = q.value("created_at").toLongLong();
    t.updatedAt   = q.value("updated_at").toLongLong();
    return t;
}


ClassRecord OkidiaDatabase::rowToClass(const QSqlQuery& q) const {
    ClassRecord c;
    c.id         = q.value("id").toString();
    c.name       = q.value("name").toString();
    c.icon       = q.value("icon").toString();
    c.type       = q.value("type").toString();
    c.parentId   = q.value("parent_id").toString();
    c.subnetHint = q.value("subnet_hint").toString();
    c.createdBy  = q.value("created_by").toString();
    c.createdAt  = q.value("created_at").toLongLong();
    c.updatedAt  = q.value("updated_at").toLongLong();
    return c;
}

UserClassRecord OkidiaDatabase::rowToUserClass(const QSqlQuery& q) const {
    UserClassRecord r;
    r.userId     = q.value("user_id").toString();
    r.classId    = q.value("class_id").toString();
    r.permission = q.value("permission").toString();
    r.grantedBy  = q.value("granted_by").toString();
    r.grantedAt  = q.value("granted_at").toLongLong();
    return r;
}

DelegationRecord OkidiaDatabase::rowToDelegation(const QSqlQuery& q) const {
    DelegationRecord d;
    d.id        = q.value("id").toString();
    d.fromUser  = q.value("from_user").toString();
    d.toUser    = q.value("to_user").toString();
    d.classId   = q.value("class_id").toString();
    d.type      = q.value("type").toString();
    d.startsAt  = q.value("starts_at").toLongLong();
    d.endsAt    = q.value("ends_at").toLongLong();
    d.createdBy = q.value("created_by").toString();
    d.createdAt = q.value("created_at").toLongLong();
    return d;
}

// ── Join Codes (OM Davet) ────────────────────────────────────────────────

QString OkidiaDatabase::createJoinCode(const QString& userId, int ttlHours) {
    auto genPart = []() {
        const QString chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; 
        QString res;
        for (int i = 0; i < 4; ++i) res += chars.at(QRandomGenerator::global()->bounded(chars.size()));
        return res;
    };
    
    QString code = QString("JOIN-%1-%2").arg(genPart(), genPart());
    qint64 expiresAt = OkidiaUtils::nowSecs() + (ttlHours * 3600);
    
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO join_codes (code, user_id, expires_at, used_at) VALUES (?, ?, ?, 0)");
    q.addBindValue(code);
    q.addBindValue(userId);
    q.addBindValue(expiresAt);
    
    if (execQuery(q, "createJoinCode")) return code;
    return {};
}

JoinCodeRecord OkidiaDatabase::validateJoinCode(const QString& code) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT code, user_id, expires_at, used_at FROM join_codes WHERE code = ?");
    q.addBindValue(code);
    
    if (execQuery(q, "validateJoinCode") && q.next()) {
        JoinCodeRecord rec;
        rec.code = q.value(0).toString();
        rec.userId = q.value(1).toString();
        rec.expiresAt = q.value(2).toLongLong();
        rec.usedAt = q.value(3).toLongLong();
        return rec;
    }
    return {};
}

bool OkidiaDatabase::consumeJoinCode(const QString& code) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE join_codes SET used_at = ? WHERE code = ?");
    q.addBindValue(OkidiaUtils::nowSecs());
    q.addBindValue(code);
    return execQuery(q, "consumeJoinCode");
}

void OkidiaDatabase::purgeJoinCodes() {
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM join_codes WHERE (expires_at < ? AND used_at = 0) OR (used_at > 0 AND used_at < ?)");
    q.addBindValue(OkidiaUtils::nowSecs());
    q.addBindValue(OkidiaUtils::nowSecs() - (7 * 24 * 3600)); 
    execQuery(q, "purgeJoinCodes");
}

bool OkidiaDatabase::vacuumInto(const QString& targetPath) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("VACUUM INTO ?");
    q.addBindValue(targetPath);
    return execQuery(q, "vacuumInto");
}

bool OkidiaDatabase::restoreFrom(const QString& sourcePath) {
    if (m_path.isEmpty()) return false;
    QString currentPath = m_path;
    close();

    // Mevcut dosyayı geçici olarak yedekle
    QString oldPath = currentPath + ".bak_old";
    QFile::remove(oldPath);
    QFile::copy(currentPath, oldPath);
    QFile::remove(currentPath);

    if (QFile::copy(sourcePath, currentPath)) {
        QFile::remove(oldPath);
        return open(currentPath);
    } else {
        // Başarısız olursa eskiyi geri al
        QFile::copy(oldPath, currentPath);
        QFile::remove(oldPath);
        open(currentPath);
        return false;
    }
}

// ── Özel Ajan Profilleri (Custom Profiles) ──────────────────────────────────

CustomProfileRecord OkidiaDatabase::rowToCustomProfile(const QSqlQuery& q) const {
    CustomProfileRecord r;
    r.id = q.value("id").toInt();
    r.name = q.value("name").toString();
    r.playBell = (q.value("play_bell").toInt() != 0);
    r.recessLock = (q.value("recess_lock").toInt() != 0);
    r.chatEnabled = (q.value("chat_enabled").toInt() != 0);
    r.iconName = q.value("icon_name").toString();
    if (r.iconName.isEmpty()) r.iconName = "custom_profile";
    r.createdAt = q.value("created_at").toLongLong();
    return r;
}

bool OkidiaDatabase::upsertCustomProfile(const CustomProfileRecord& prof) {
    int profId = prof.id;
    if (profId <= 0) {
        // 101'den başlayan ilk müsait ID'yi bul
        QSqlQuery qQuery(m_db);
        if (qQuery.exec("SELECT COALESCE(MAX(id), 100) FROM custom_profiles WHERE id >= 100") && qQuery.next()) {
            int maxId = qQuery.value(0).toInt();
            profId = std::max(101, maxId + 1);
        } else {
            profId = 101;
        }
    }

    QSqlQuery q(m_db);
    q.prepare("INSERT INTO custom_profiles (id, name, play_bell, recess_lock, chat_enabled, icon_name, created_at) "
              "VALUES (?, ?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(id) DO UPDATE SET "
              "  name = excluded.name, "
              "  play_bell = excluded.play_bell, "
              "  recess_lock = excluded.recess_lock, "
              "  chat_enabled = excluded.chat_enabled, "
              "  icon_name = excluded.icon_name");
    q.addBindValue(profId);
    q.addBindValue(prof.name);
    q.addBindValue(prof.playBell ? 1 : 0);
    q.addBindValue(prof.recessLock ? 1 : 0);
    q.addBindValue(prof.chatEnabled ? 1 : 0);
    q.addBindValue(prof.iconName.isEmpty() ? "custom_profile" : prof.iconName);
    q.addBindValue(prof.createdAt > 0 ? prof.createdAt : OkidiaUtils::nowSecs());

    if (!execQuery(q, "upsertCustomProfile")) return false;
    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit customProfilesChanged();
    return true;
}

bool OkidiaDatabase::deleteCustomProfile(int id) {
    if (id <= 0) return false;

    // 1. Bu profile bağlı tüm istemcileri otomatik olarak Öğrenci PC (Tip 1)'e düşür (Orphan Cleanup)
    QSqlQuery qRevert(m_db);
    qRevert.prepare("UPDATE agents SET agent_type = 1 WHERE agent_type = ?");
    qRevert.addBindValue(id);
    qRevert.exec();

    // 2. Profili tablodan sil
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM custom_profiles WHERE id = ?");
    q.addBindValue(id);
    if (!execQuery(q, "deleteCustomProfile")) return false;

    setMetaValue("last_modified", QString::number(OkidiaUtils::nowSecs()));
    emit customProfilesChanged();
    return true;
}

CustomProfileRecord OkidiaDatabase::customProfile(int id) const {
    if (!m_open || id <= 0) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT * FROM custom_profiles WHERE id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next()) return rowToCustomProfile(q);
    return {};
}

QList<CustomProfileRecord> OkidiaDatabase::allCustomProfiles() const {
    QList<CustomProfileRecord> list;
    if (!m_open) return list;
    QSqlQuery q(m_db);
    if (q.exec("SELECT * FROM custom_profiles ORDER BY id ASC")) {
        while (q.next()) {
            list.append(rowToCustomProfile(q));
        }
    }
    return list;
}

int OkidiaDatabase::countAgentsWithCustomProfile(int profileId) const {
    if (!m_open || profileId <= 0) return 0;
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM agents WHERE agent_type = ?");
    q.addBindValue(profileId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}
