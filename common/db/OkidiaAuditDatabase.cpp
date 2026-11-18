// common/db/OkidiaAuditDatabase.cpp
#include "OkidiaAuditDatabase.h"
#include "../utils/Logger.h"
#include "../utils/OkidiaUtils.h"
#include "../OkidiaConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>

OkidiaAuditDatabase& OkidiaAuditDatabase::instance() {
    static OkidiaAuditDatabase s_inst;
    return s_inst;
}

OkidiaAuditDatabase::OkidiaAuditDatabase(QObject* parent)
    : QObject(parent)
{
}

bool OkidiaAuditDatabase::open(const QString& path) {
    if (m_open) return true;
    m_path = path;

    if (m_path != ":memory:") {
        QFileInfo fi(m_path);
        QDir().mkpath(fi.absolutePath());
    }

    const QString connName = QString("okidia_audit_conn");
    if (QSqlDatabase::contains(connName)) {
        m_db = QSqlDatabase::database(connName);
    } else {
        m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
    }
    m_db.setDatabaseName(m_path);

    if (!m_db.open()) {
        Logger::error(QObject::tr("OkidiaAuditDatabase: Veritabanı açılamadı: ")
                      + m_path + " - " + m_db.lastError().text());
        return false;
    }

    m_open = true;

    execQuery("PRAGMA journal_mode = WAL");
    execQuery("PRAGMA busy_timeout = 5000");
    execQuery("PRAGMA synchronous = NORMAL");
    execQuery("PRAGMA temp_store = MEMORY");
    execQuery("PRAGMA wal_autocheckpoint = 1000");

    if (!createAuditSchema()) {
        close();
        return false;
    }

    if (!m_flushTimer) {
        m_flushTimer = new QTimer(this);
        m_flushTimer->setInterval(FLUSH_INTERVAL_MS);
        connect(m_flushTimer, &QTimer::timeout, this, [this]() {
            flushQueue();
        });
    }
    m_flushTimer->start();

    Logger::info(QObject::tr("OkidiaAuditDatabase: Başarıyla açıldı → ") + m_path);
    return true;
}

void OkidiaAuditDatabase::close() {
    if (!m_open) return;
    flushQueue();
    if (m_flushTimer) {
        m_flushTimer->stop();
    }
    const QString conn = m_db.connectionName();
    m_db.close();
    m_open = false;
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(conn);
}

void OkidiaAuditDatabase::flushQueue() {
    if (!m_open || m_writeQueue.isEmpty()) return;

    QList<PendingAuditLog> batch;
    batch.swap(m_writeQueue);

    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO audit_log(timestamp, category, actor, user_id, master_id, target, detail)"
        " VALUES(?,?,?,?,?,?,?)");

    for (const auto& item : batch) {
        q.addBindValue(item.timestamp > 0 ? item.timestamp : OkidiaUtils::nowSecs());
        q.addBindValue(item.category.isEmpty() ? QStringLiteral("Command") : item.category);
        q.addBindValue(item.actor);
        q.addBindValue(item.userId.isEmpty() ? QStringLiteral("") : item.userId);
        q.addBindValue(item.masterId.isEmpty() ? QStringLiteral("") : item.masterId);
        q.addBindValue(item.target.isEmpty() ? QStringLiteral("") : item.target);
        q.addBindValue(item.detail.isEmpty() ? QStringLiteral("") : item.detail);
        if (!q.exec()) {
            Logger::error(QObject::tr("OkidiaAuditDatabase: Batch log yazma hatası: ") + q.lastError().text());
        }
    }
    m_db.commit();
}

bool OkidiaAuditDatabase::isOpen() const {
    return m_open && m_db.isOpen();
}

bool OkidiaAuditDatabase::execQuery(const QString& sql) const {
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        Logger::error(QObject::tr("OkidiaAuditDatabase: SQL Hatası: ") + q.lastError().text()
                      + " | SQL: " + sql.left(80));
        return false;
    }
    return true;
}

bool OkidiaAuditDatabase::execQuery(QSqlQuery& q, const QString& context) const {
    if (!q.exec()) {
        Logger::error(QObject::tr("OkidiaAuditDatabase: Sorgu Hatası (%1): %2 | SQL: %3")
                      .arg(context, q.lastError().text(), q.lastQuery().left(80)));
        return false;
    }
    return true;
}

bool OkidiaAuditDatabase::createAuditSchema() {
    const QStringList ddl = {
        // Audit log
        R"(CREATE TABLE IF NOT EXISTS audit_log (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp  INTEGER NOT NULL DEFAULT (unixepoch()),
            category   TEXT NOT NULL DEFAULT 'Command',
            actor      TEXT NOT NULL,
            user_id    TEXT NOT NULL DEFAULT '',
            master_id  TEXT NOT NULL DEFAULT '',
            target     TEXT NOT NULL DEFAULT '',
            detail     TEXT NOT NULL DEFAULT ''
        ))",
        "CREATE INDEX IF NOT EXISTS idx_audit_time   ON audit_log(timestamp)",
        "CREATE INDEX IF NOT EXISTS idx_audit_actor  ON audit_log(actor)",
        "CREATE INDEX IF NOT EXISTS idx_audit_user   ON audit_log(user_id)",
        "CREATE INDEX IF NOT EXISTS idx_audit_cat_time ON audit_log(category, timestamp)",
        "CREATE INDEX IF NOT EXISTS idx_audit_target ON audit_log(target)",

        // WORM Tetikleyicileri (ForensicAudit ve Security kayıtları YM dahil hiç kimse tarafından silinemez/değiştirilemez)
        R"(CREATE TRIGGER IF NOT EXISTS trg_prevent_forensic_audit_delete
        BEFORE DELETE ON audit_log
        WHEN OLD.category IN ('ForensicAudit', 'Security')
        BEGIN
            SELECT RAISE(FAIL, 'Adli denetim kayıtları YM dahil hiçbir kullanıcı veya komut tarafından silinemez!');
        END;)",

        R"(CREATE TRIGGER IF NOT EXISTS trg_prevent_forensic_audit_update
        BEFORE UPDATE ON audit_log
        WHEN OLD.category IN ('ForensicAudit', 'Security')
        BEGIN
            SELECT RAISE(FAIL, 'Adli denetim kayıtları üzerinde sonradan değişiklik yapılamaz!');
        END;)",

        // Özel sohbet kayıt defteri — sadece meta, içerik yok (KVKK)
        R"(CREATE TABLE IF NOT EXISTS private_chat_log (
            id              TEXT PRIMARY KEY,
            from_machine_id TEXT NOT NULL,
            from_host       TEXT NOT NULL,
            from_user       TEXT NOT NULL,
            to_machine_id   TEXT NOT NULL,
            to_host         TEXT NOT NULL,
            to_user         TEXT NOT NULL,
            started_at      INTEGER NOT NULL DEFAULT (unixepoch()),
            ended_at        INTEGER
        ))",
        "CREATE INDEX IF NOT EXISTS idx_pclog_from ON private_chat_log(from_machine_id)",
        "CREATE INDEX IF NOT EXISTS idx_pclog_to   ON private_chat_log(to_machine_id)",

        // Sync log
        R"(CREATE TABLE IF NOT EXISTS sync_log (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            master_id     TEXT NOT NULL,
            synced_at     INTEGER NOT NULL DEFAULT (unixepoch()),
            tables_synced TEXT NOT NULL DEFAULT '',
            delta_count   INTEGER NOT NULL DEFAULT 0
        ))",

        // Backup log
        R"(CREATE TABLE IF NOT EXISTS backup_log (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL DEFAULT (unixepoch()),
            sent_to    TEXT NOT NULL,
            file_size  INTEGER NOT NULL DEFAULT 0,
            checksum   TEXT NOT NULL DEFAULT '',
            success    INTEGER NOT NULL DEFAULT 0
        ))",

        // Meta tablosu (retention ayarları için)
        R"(CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        ))"
    };

    QSqlQuery q(m_db);
    for (const QString& sql : ddl) {
        if (!q.exec(sql)) {
            Logger::error(QObject::tr("OkidiaAuditDatabase: Schema hatası → ") + q.lastError().text());
            return false;
        }
    }
    return true;
}

AuditRecord OkidiaAuditDatabase::rowToAudit(const QSqlQuery& q) const {
    AuditRecord r;
    r.id        = q.value("id").toLongLong();
    r.timestamp = q.value("timestamp").toLongLong();
    r.category  = q.value("category").toString();
    r.actor     = q.value("actor").toString();
    r.userId    = q.value("user_id").toString();
    r.masterId  = q.value("master_id").toString();
    r.target    = q.value("target").toString();
    r.detail    = q.value("detail").toString();
    return r;
}

void OkidiaAuditDatabase::logCommand(const QString& actor,
                                     const QString& target,
                                     const QString& detail,
                                     const QString& category,
                                     const QString& userId,
                                     const QString& masterId,
                                     qint64         timestamp) {
    if (!m_open) return;

    PendingAuditLog item;
    item.actor     = actor;
    item.target    = target.isEmpty() ? QStringLiteral("") : target;
    item.detail    = detail.isEmpty() ? QStringLiteral("") : detail;
    item.category  = category.isEmpty() ? QStringLiteral("Command") : category;
    item.userId    = userId.isEmpty() ? QStringLiteral("") : userId;
    item.masterId  = masterId.isEmpty() ? QStringLiteral("") : masterId;
    item.timestamp = timestamp > 0 ? timestamp : OkidiaUtils::nowSecs();

    m_writeQueue.append(item);

    // Kritik adli kayıtlar veya kuyruk dolduğunda anında diske yaz
    if (m_writeQueue.size() >= MAX_QUEUE_SIZE || item.category == "Security" || item.category == "ForensicAudit") {
        flushQueue();
    }
}

QList<AuditRecord> OkidiaAuditDatabase::queryAuditLogAdvanced(qint64 fromTs,
                                                              qint64 toTs,
                                                              const QString& category,
                                                              const QString& actor,
                                                              const QString& target,
                                                              const QString& searchQuery,
                                                              int limit,
                                                              int offset,
                                                              int* totalCountOut) const {
    QList<AuditRecord> list;
    if (!m_open) return list;
    const_cast<OkidiaAuditDatabase*>(this)->flushQueue();

    QString where = " WHERE 1=1";
    if (fromTs > 0) where += " AND timestamp >= :from";
    if (toTs   > 0) where += " AND timestamp <= :to";
    if (!category.isEmpty() && category != "All" && category != "Tümü") where += " AND category = :cat";
    if (!actor.isEmpty()) where += " AND actor = :actor";
    if (!target.isEmpty()) where += " AND target LIKE :target";
    if (!searchQuery.isEmpty()) {
        where += " AND (actor LIKE :q OR target LIKE :q OR detail LIKE :q)";
    }

    if (totalCountOut) {
        *totalCountOut = 0;
        QSqlQuery countQ(m_db);
        countQ.prepare("SELECT COUNT(*) FROM audit_log" + where);
        if (fromTs > 0) countQ.bindValue(":from", fromTs);
        if (toTs   > 0) countQ.bindValue(":to", toTs);
        if (!category.isEmpty() && category != "All" && category != "Tümü") countQ.bindValue(":cat", category);
        if (!actor.isEmpty()) countQ.bindValue(":actor", actor);
        if (!target.isEmpty()) countQ.bindValue(":target", "%" + target + "%");
        if (!searchQuery.isEmpty()) countQ.bindValue(":q", "%" + searchQuery + "%");
        if (countQ.exec() && countQ.next()) {
            *totalCountOut = countQ.value(0).toInt();
        }
    }

    QString sql = "SELECT * FROM audit_log" + where + " ORDER BY timestamp DESC LIMIT :lim OFFSET :off";
    QSqlQuery q(m_db);
    q.prepare(sql);
    if (fromTs > 0) q.bindValue(":from",  fromTs);
    if (toTs   > 0) q.bindValue(":to",    toTs);
    if (!category.isEmpty() && category != "All" && category != "Tümü") q.bindValue(":cat", category);
    if (!actor.isEmpty()) q.bindValue(":actor", actor);
    if (!target.isEmpty()) q.bindValue(":target", "%" + target + "%");
    if (!searchQuery.isEmpty()) q.bindValue(":q", "%" + searchQuery + "%");
    q.bindValue(":lim", limit > 0 ? limit : 500);
    q.bindValue(":off", offset >= 0 ? offset : 0);

    if (q.exec()) {
        while (q.next()) list << rowToAudit(q);
    }
    return list;
}

QList<AuditRecord> OkidiaAuditDatabase::queryAuditLog(qint64 fromTs,
                                                      qint64 toTs,
                                                      const QString& actor,
                                                      int limit) const {
    return queryAuditLogAdvanced(fromTs, toTs, {}, actor, {}, {}, limit, 0, nullptr);
}

void OkidiaAuditDatabase::purgeAuditLog(int olderThanDays) {
    if (!m_open) return;
    if (olderThanDays <= 0) olderThanDays = auditRetentionDays();
    const qint64 cutoff = OkidiaUtils::nowSecs() - (qint64)olderThanDays * 86400LL;
    QSqlQuery q(m_db);
    // ForensicAudit ve Security kategorileri WORM adli denetim güvencesi altındadır, asla silinmez
    q.prepare("DELETE FROM audit_log WHERE timestamp < ? AND category NOT IN ('ForensicAudit', 'Security')");
    q.addBindValue(cutoff);
    execQuery(q, "purgeAuditLog");
}

bool OkidiaAuditDatabase::logPrivateChatSession(
    const QString& id,
    const QString& fromMachineId, const QString& fromHost, const QString& fromUser,
    const QString& toMachineId,   const QString& toHost,   const QString& toUser) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT OR IGNORE INTO private_chat_log"
        "(id, from_machine_id, from_host, from_user,"
        " to_machine_id, to_host, to_user, started_at)"
        " VALUES(?,?,?,?,?,?,?,?)");
    q.addBindValue(id);
    q.addBindValue(fromMachineId); q.addBindValue(fromHost); q.addBindValue(fromUser);
    q.addBindValue(toMachineId);   q.addBindValue(toHost);   q.addBindValue(toUser);
    q.addBindValue(OkidiaUtils::nowSecs());
    return execQuery(q, "logPrivateChatSession");
}

bool OkidiaAuditDatabase::closePrivateChatSession(const QString& id) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE private_chat_log SET ended_at=? WHERE id=? AND ended_at IS NULL");
    q.addBindValue(OkidiaUtils::nowSecs());
    q.addBindValue(id);
    return execQuery(q, "closePrivateChatSession");
}

int OkidiaAuditDatabase::auditRetentionDays() const {
    if (!m_open) return OkidiaConfig::DEFAULT_AUDIT_RETENTION_DAYS;
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM meta WHERE key = 'retention/audit_days'");
    if (q.exec() && q.next()) {
        int d = q.value(0).toInt();
        if (d > 0) return d;
    }
    return OkidiaConfig::DEFAULT_AUDIT_RETENTION_DAYS;
}

void OkidiaAuditDatabase::setAuditRetentionDays(int days) {
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO meta(key, value) VALUES('retention/audit_days', ?)"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.addBindValue(QString::number(days));
    execQuery(q, "setAuditRetentionDays");
}

int OkidiaAuditDatabase::archiveRetentionDays() const {
    if (!m_open) return OkidiaConfig::DEFAULT_ARCHIVE_RETENTION_DAYS;
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM meta WHERE key = 'retention/archive_days'");
    if (q.exec() && q.next()) {
        int d = q.value(0).toInt();
        if (d > 0) return d;
    }
    return OkidiaConfig::DEFAULT_ARCHIVE_RETENTION_DAYS;
}

void OkidiaAuditDatabase::setArchiveRetentionDays(int days) {
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO meta(key, value) VALUES('retention/archive_days', ?)"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.addBindValue(QString::number(days));
    execQuery(q, "setArchiveRetentionDays");
}

bool OkidiaAuditDatabase::archiveOldAudit(int auditDays) {
    if (!m_open) return false;
    flushQueue();
    if (auditDays <= 0) auditDays = auditRetentionDays();

    QFileInfo fi(m_path);
    QString archiveDir = fi.absoluteDir().filePath("archive");
    QDir().mkpath(archiveDir);

    const qint64 auditCutoff = OkidiaUtils::nowSecs() - (qint64)auditDays * 86400LL;
    const QString weekStamp = QDateTime::currentDateTime().toString("yyyy_w%1").arg(QDateTime::currentDateTime().date().weekNumber(), 2, 10, QChar('0'));

    QSqlQuery qCheck(m_db);
    qCheck.prepare("SELECT COUNT(*) FROM audit_log WHERE timestamp < ? AND category NOT IN ('ForensicAudit', 'Security')");
    qCheck.addBindValue(auditCutoff);
    if (qCheck.exec() && qCheck.next() && qCheck.value(0).toInt() > 0) {
        int auditCount = qCheck.value(0).toInt();
        QString rawArchiveDb = archiveDir + QString("/audit_%1.db").arg(weekStamp);
        QSqlQuery qArch(m_db);
        qArch.exec(QString("ATTACH DATABASE '%1' AS arch_audit").arg(rawArchiveDb));
        qArch.exec("CREATE TABLE IF NOT EXISTS arch_audit.audit_log (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, category TEXT NOT NULL, actor TEXT NOT NULL, user_id TEXT NOT NULL, master_id TEXT NOT NULL, target TEXT NOT NULL, detail TEXT NOT NULL)");
        qArch.prepare("INSERT OR IGNORE INTO arch_audit.audit_log SELECT * FROM audit_log WHERE timestamp < ? AND category NOT IN ('ForensicAudit', 'Security')");
        qArch.addBindValue(auditCutoff);
        qArch.exec();
        qArch.exec("DETACH DATABASE arch_audit");

        // Canlı tablodan sil
        QSqlQuery qDel(m_db);
        qDel.prepare("DELETE FROM audit_log WHERE timestamp < ? AND category NOT IN ('ForensicAudit', 'Security')");
        qDel.addBindValue(auditCutoff);
        qDel.exec();

        // Dosyayı sıkıştır
        QFile rawFile(rawArchiveDb);
        if (rawFile.open(QIODevice::ReadOnly)) {
            QByteArray rawData = rawFile.readAll();
            rawFile.close();
            QFile gzFile(rawArchiveDb + ".gz");
            if (gzFile.open(QIODevice::WriteOnly)) {
                gzFile.write(qCompress(rawData, 9));
                gzFile.close();
                rawFile.remove();
            }
        }
        Logger::info(QObject::tr("OkidiaAuditDatabase: %1 adet eski denetim kaydı arşivlendi → audit_%2.db.gz").arg(auditCount).arg(weekStamp));
    }

    // Çok eski arşivlerin temizlenmesi
    int maxArchDays = archiveRetentionDays();
    if (maxArchDays > 0) {
        QDir aDir(archiveDir);
        QFileInfoList list = aDir.entryInfoList(QStringList() << "audit_*.db.gz", QDir::Files);
        const qint64 archCutoff = OkidiaUtils::nowSecs() - (qint64)maxArchDays * 86400LL;
        for (const auto& fiArch : list) {
            if (fiArch.lastModified().toSecsSinceEpoch() < archCutoff) {
                QFile::remove(fiArch.absoluteFilePath());
                Logger::info(QObject::tr("OkidiaAuditDatabase: Süresi dolan eski arşiv silindi: ") + fiArch.fileName());
            }
        }
    }

    return true;
}

bool OkidiaAuditDatabase::vacuumInto(const QString& targetPath) {
    if (!m_open) return false;
    flushQueue();
    QSqlQuery q(m_db);
    q.prepare("VACUUM INTO ?");
    q.addBindValue(targetPath);
    return execQuery(q, "vacuumInto");
}

bool OkidiaAuditDatabase::restoreFrom(const QString& sourcePath) {
    if (m_path.isEmpty()) return false;
    QString currentPath = m_path;
    close();

    QString oldPath = currentPath + ".bak_old";
    QFile::remove(oldPath);
    QFile::copy(currentPath, oldPath);
    QFile::remove(currentPath);

    if (QFile::copy(sourcePath, currentPath)) {
        QFile::remove(oldPath);
        return open(currentPath);
    } else {
        QFile::copy(oldPath, currentPath);
        QFile::remove(oldPath);
        open(currentPath);
        return false;
    }
}
