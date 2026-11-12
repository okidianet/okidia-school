// common/db/OkidiaAuditDatabase.h
// OKIDIA Denetim (Audit) ve WORM Veritabanı — Yalnızca Broker üzerinde çalışır.
// Konum: /var/lib/okidia/okidia_audit.db (üretim)
//
// 5651 ve KVKK gereği adli delil niteliğindeki logları tutar.
// ForensicAudit ve Security kayıtları WORM (Write Once Read Many) SQLite tetikleyicileri ile korunur.
#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QList>
#include <QString>
#include "DbRecords.h"

class OkidiaAuditDatabase : public QObject {
    Q_OBJECT

public:
    static OkidiaAuditDatabase& instance();

    // ── Yaşam döngüsü ────────────────────────────────────────────────────
    bool open(const QString& path);
    void close();
    bool isOpen() const;
    QString path() const { return m_path; }

    bool vacuumInto(const QString& targetPath);
    bool restoreFrom(const QString& sourcePath);

    // Kuyruktaki bekleyen kayıtları anında diske yaz
    void flushQueue();

    QSqlDatabase& db() { return m_db; }

    // ── Denetim Kayıtları (Audit Log) ────────────────────────────────────
    void logCommand(const QString& actor,
                    const QString& target,
                    const QString& detail,
                    const QString& category = "Command",
                    const QString& userId   = {},
                    const QString& masterId = {},
                    qint64         timestamp = 0);

    QList<AuditRecord> queryAuditLogAdvanced(qint64 fromTimestamp = 0,
                                             qint64 toTimestamp   = 0,
                                             const QString& category = {},
                                             const QString& actor    = {},
                                             const QString& target   = {},
                                             const QString& searchQuery = {},
                                             int limit = 100,
                                             int offset = 0,
                                             int* totalCountOut = nullptr) const;

    QList<AuditRecord> queryAuditLog(qint64 fromTimestamp = 0,
                                     qint64 toTimestamp   = 0,
                                     const QString& actor = {},
                                     int limit            = 500) const;

    void purgeAuditLog(int olderThanDays = -1);

    // ── Özel sohbet kayıt defteri (sadece meta, içerik yok - KVKK) ────────
    bool logPrivateChatSession(const QString& sessionId,
                               const QString& fromMachineId, const QString& fromHost,
                               const QString& fromUser,
                               const QString& toMachineId,   const QString& toHost,
                               const QString& toUser);
    bool closePrivateChatSession(const QString& sessionId);

    // ── Saklama ve Arşivleme (Rolling 2-Week Archive) ──────────────────────
    int  auditRetentionDays() const;
    void setAuditRetentionDays(int days);
    int  archiveRetentionDays() const;
    void setArchiveRetentionDays(int days);
    bool archiveOldAudit(int auditDays = -1);

private:
    explicit OkidiaAuditDatabase(QObject* parent = nullptr);
    OkidiaAuditDatabase(const OkidiaAuditDatabase&)            = delete;
    OkidiaAuditDatabase& operator=(const OkidiaAuditDatabase&) = delete;

    bool createAuditSchema();
    bool execQuery(const QString& sql) const;
    bool execQuery(QSqlQuery& q, const QString& context = {}) const;
    AuditRecord rowToAudit(const QSqlQuery& q) const;

    QSqlDatabase m_db;
    QString      m_path;
    bool         m_open = false;

    struct PendingAuditLog {
        QString actor;
        QString target;
        QString detail;
        QString category;
        QString userId;
        QString masterId;
        qint64  timestamp;
    };
    QList<PendingAuditLog> m_writeQueue;
    class QTimer*          m_flushTimer = nullptr;
    static constexpr int   MAX_QUEUE_SIZE = 50;
    static constexpr int   FLUSH_INTERVAL_MS = 1000;
};
