// common/db/OkidiaChatDatabase.cpp
#include "OkidiaChatDatabase.h"
#include "../utils/Logger.h"
#include "../utils/OkidiaUtils.h"
#include "../OkidiaConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

OkidiaChatDatabase& OkidiaChatDatabase::instance() {
    static OkidiaChatDatabase s_inst;
    return s_inst;
}

OkidiaChatDatabase::OkidiaChatDatabase(QObject* parent)
    : QObject(parent)
{
}

bool OkidiaChatDatabase::open(const QString& path) {
    if (m_open) return true;
    m_path = path;

    if (m_path != ":memory:") {
        QFileInfo fi(m_path);
        QDir().mkpath(fi.absolutePath());
    }

    const QString connName = QString("okidia_chat_conn");
    if (QSqlDatabase::contains(connName)) {
        m_db = QSqlDatabase::database(connName);
    } else {
        m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
    }
    m_db.setDatabaseName(m_path);

    if (!m_db.open()) {
        Logger::error(QObject::tr("OkidiaChatDatabase: Veritabanı açılamadı: ")
                      + m_path + " - " + m_db.lastError().text());
        return false;
    }

    m_open = true;

    execQuery("PRAGMA journal_mode = WAL");
    execQuery("PRAGMA busy_timeout = 5000");
    execQuery("PRAGMA synchronous = NORMAL");
    execQuery("PRAGMA temp_store = MEMORY");
    execQuery("PRAGMA wal_autocheckpoint = 1000");

    if (!createChatSchema()) {
        close();
        return false;
    }

    Logger::info(QObject::tr("OkidiaChatDatabase: Başarıyla açıldı → ") + m_path);
    return true;
}

void OkidiaChatDatabase::close() {
    if (!m_open) return;
    const QString conn = m_db.connectionName();
    m_db.close();
    m_open = false;
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(conn);
}

bool OkidiaChatDatabase::isOpen() const {
    return m_open && m_db.isOpen();
}

bool OkidiaChatDatabase::execQuery(const QString& sql) const {
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        Logger::error(QObject::tr("OkidiaChatDatabase: SQL Hatası: ") + q.lastError().text()
                      + " | SQL: " + sql.left(80));
        return false;
    }
    return true;
}

bool OkidiaChatDatabase::execQuery(QSqlQuery& q, const QString& context) const {
    if (!q.exec()) {
        Logger::error(QObject::tr("OkidiaChatDatabase: Sorgu Hatası (%1): %2 | SQL: %3")
                      .arg(context, q.lastError().text(), q.lastQuery().left(80)));
        return false;
    }
    return true;
}

bool OkidiaChatDatabase::createChatSchema() {
    const QStringList ddl = {
        // Chat rooms
        R"(CREATE TABLE IF NOT EXISTS chat_rooms (
            id             TEXT PRIMARY KEY,
            name           TEXT NOT NULL,
            class_id       TEXT,
            parent_room_id TEXT,
            permanent      INTEGER NOT NULL DEFAULT 0,
            created_by     TEXT,
            created_at     INTEGER NOT NULL DEFAULT (unixepoch())
        ))",

        // Chat messages
        R"(CREATE TABLE IF NOT EXISTS chat_messages (
            id          TEXT PRIMARY KEY,
            room_id     TEXT NOT NULL,
            from_host   TEXT NOT NULL,
            sender_role INTEGER NOT NULL DEFAULT 0,
            content     TEXT NOT NULL,
            timestamp   INTEGER NOT NULL,
            is_system   INTEGER NOT NULL DEFAULT 0
        ))",
        "CREATE INDEX IF NOT EXISTS idx_msg_room_time ON chat_messages(room_id, timestamp)",

        // Meta tablosu
        R"(CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        ))"
    };

    QSqlQuery q(m_db);
    for (const QString& sql : ddl) {
        if (!q.exec(sql)) {
            Logger::error(QObject::tr("OkidiaChatDatabase: Schema hatası → ") + q.lastError().text());
            return false;
        }
    }
    return true;
}

bool OkidiaChatDatabase::saveChatRoom(const QString& id, const QString& name,
                                      const QString& classId, bool permanent) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT INTO chat_rooms(id, name, class_id, permanent)"
        " VALUES(?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name");
    q.addBindValue(id);
    q.addBindValue(name);
    q.addBindValue(classId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : classId);
    q.addBindValue(permanent ? 1 : 0);
    return execQuery(q, "saveChatRoom");
}

bool OkidiaChatDatabase::deleteChatRoom(const QString& id) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM chat_rooms WHERE id=?");
    q.addBindValue(id);
    return execQuery(q, "deleteChatRoom");
}

bool OkidiaChatDatabase::saveChatMessage(const QString& id,
                                         const QString& roomId,
                                         const QString& fromHost,
                                         int senderRole,
                                         const QString& content,
                                         qint64 timestamp,
                                         bool isSystem) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare(
        "INSERT OR REPLACE INTO chat_messages"
        "(id, room_id, from_host, sender_role, content, timestamp, is_system)"
        " VALUES(?,?,?,?,?,?,?)");
    q.addBindValue(id);
    q.addBindValue(roomId);
    q.addBindValue(fromHost);
    q.addBindValue(senderRole);
    q.addBindValue(content);
    q.addBindValue(timestamp);
    q.addBindValue(isSystem ? 1 : 0);
    return execQuery(q, "saveChatMessage");
}

QList<ChatMessageRecord> OkidiaChatDatabase::getChatMessages(
    const QString& roomId, int limit, qint64 beforeTimestamp) const {
    QList<ChatMessageRecord> list;
    if (!m_open) return list;
    QSqlQuery q(m_db);
    QString sql = "SELECT id, room_id, from_host, sender_role, content, timestamp, is_system"
                  " FROM chat_messages WHERE room_id=?";
    if (beforeTimestamp > 0) sql += " AND timestamp < ?";
    sql += " ORDER BY timestamp DESC LIMIT ?";
    q.prepare(sql);
    q.addBindValue(roomId);
    if (beforeTimestamp > 0) q.addBindValue(beforeTimestamp);
    q.addBindValue(limit);
    if (!q.exec()) return list;
    while (q.next()) {
        ChatMessageRecord r;
        r.id         = q.value(0).toString();
        r.roomId     = q.value(1).toString();
        r.fromHost   = q.value(2).toString();
        r.senderRole = q.value(3).toInt();
        r.content    = q.value(4).toString();
        r.timestamp  = q.value(5).toLongLong();
        r.isSystem   = q.value(6).toInt() != 0;
        list.prepend(r);  // kronolojik sıra
    }
    return list;
}

void OkidiaChatDatabase::purgeChatMessages(int olderThanDays) {
    if (!m_open) return;
    if (olderThanDays <= 0) olderThanDays = chatRetentionDays();
    const qint64 cutoff = OkidiaUtils::nowSecs() - (qint64)olderThanDays * 86400LL;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM chat_messages WHERE timestamp < ?");
    q.addBindValue(cutoff);
    execQuery(q, "purgeChatMessages");
}

int OkidiaChatDatabase::chatRetentionDays() const {
    if (!m_open) return OkidiaConfig::DEFAULT_CHAT_RETENTION_DAYS;
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM meta WHERE key = 'retention/chat_days'");
    if (q.exec() && q.next()) {
        int d = q.value(0).toInt();
        if (d > 0) return d;
    }
    return OkidiaConfig::DEFAULT_CHAT_RETENTION_DAYS;
}

void OkidiaChatDatabase::setChatRetentionDays(int days) {
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO meta(key, value) VALUES('retention/chat_days', ?)"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.addBindValue(QString::number(days));
    execQuery(q, "setChatRetentionDays");
}

int OkidiaChatDatabase::archiveRetentionDays() const {
    if (!m_open) return OkidiaConfig::DEFAULT_ARCHIVE_RETENTION_DAYS;
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM meta WHERE key = 'retention/archive_days'");
    if (q.exec() && q.next()) {
        int d = q.value(0).toInt();
        if (d > 0) return d;
    }
    return OkidiaConfig::DEFAULT_ARCHIVE_RETENTION_DAYS;
}

void OkidiaChatDatabase::setArchiveRetentionDays(int days) {
    if (!m_open) return;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO meta(key, value) VALUES('retention/archive_days', ?)"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    q.addBindValue(QString::number(days));
    execQuery(q, "setArchiveRetentionDays");
}

bool OkidiaChatDatabase::archiveOldChat(int chatDays) {
    if (!m_open) return false;
    if (chatDays <= 0) chatDays = chatRetentionDays();

    QFileInfo fi(m_path);
    QString archiveDir = fi.absoluteDir().filePath("archive");
    QDir().mkpath(archiveDir);

    const qint64 chatCutoff = OkidiaUtils::nowSecs() - (qint64)chatDays * 86400LL;
    const QString weekStamp = QDateTime::currentDateTime().toString("yyyy_w%1").arg(QDateTime::currentDateTime().date().weekNumber(), 2, 10, QChar('0'));

    QSqlQuery qCheck(m_db);
    qCheck.prepare("SELECT COUNT(*) FROM chat_messages WHERE timestamp < ?");
    qCheck.addBindValue(chatCutoff);
    if (qCheck.exec() && qCheck.next() && qCheck.value(0).toInt() > 0) {
        int msgCount = qCheck.value(0).toInt();
        QString rawArchiveDb = archiveDir + QString("/chat_%1.db").arg(weekStamp);
        QSqlQuery qArch(m_db);
        qArch.exec(QString("ATTACH DATABASE '%1' AS arch_chat").arg(rawArchiveDb));
        qArch.exec("CREATE TABLE IF NOT EXISTS arch_chat.chat_messages (id TEXT PRIMARY KEY, room_id TEXT NOT NULL, from_host TEXT NOT NULL, sender_role INTEGER NOT NULL DEFAULT 0, content TEXT NOT NULL, timestamp INTEGER NOT NULL, is_system INTEGER NOT NULL DEFAULT 0)");
        qArch.prepare("INSERT OR IGNORE INTO arch_chat.chat_messages SELECT * FROM chat_messages WHERE timestamp < ?");
        qArch.addBindValue(chatCutoff);
        qArch.exec();
        qArch.exec("DETACH DATABASE arch_chat");

        // Canlı tablodan sil
        QSqlQuery qDel(m_db);
        qDel.prepare("DELETE FROM chat_messages WHERE timestamp < ?");
        qDel.addBindValue(chatCutoff);
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
        Logger::info(QObject::tr("OkidiaChatDatabase: %1 adet eski sohbet mesajı arşivlendi → chat_%2.db.gz").arg(msgCount).arg(weekStamp));
    }

    // Çok eski arşivlerin temizlenmesi
    int maxArchDays = archiveRetentionDays();
    if (maxArchDays > 0) {
        QDir aDir(archiveDir);
        QFileInfoList list = aDir.entryInfoList(QStringList() << "chat_*.db.gz", QDir::Files);
        const qint64 archCutoff = OkidiaUtils::nowSecs() - (qint64)maxArchDays * 86400LL;
        for (const auto& fiArch : list) {
            if (fiArch.lastModified().toSecsSinceEpoch() < archCutoff) {
                QFile::remove(fiArch.absoluteFilePath());
                Logger::info(QObject::tr("OkidiaChatDatabase: Süresi dolan eski arşiv silindi: ") + fiArch.fileName());
            }
        }
    }

    return true;
}

bool OkidiaChatDatabase::vacuumInto(const QString& targetPath) {
    if (!m_open) return false;
    QSqlQuery q(m_db);
    q.prepare("VACUUM INTO ?");
    q.addBindValue(targetPath);
    return execQuery(q, "vacuumInto");
}

bool OkidiaChatDatabase::restoreFrom(const QString& sourcePath) {
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
