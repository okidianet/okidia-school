// common/db/OkidiaChatDatabase.h
// OKIDIA Sohbet (Chat) Veritabanı — Yalnızca Broker üzerinde çalışır.
// Konum: /var/lib/okidia/okidia_chat.db (üretim)
//
// Grup sohbet odalarını ve mesaj geçmişini yönetir.
// KVKK veri minimizasyonu gereği Master veya Agent üzerinde oluşturulmaz.
#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QList>
#include <QString>
#include "DbRecords.h"

class OkidiaChatDatabase : public QObject {
    Q_OBJECT

public:
    static OkidiaChatDatabase& instance();

    // ── Yaşam döngüsü ────────────────────────────────────────────────────
    bool open(const QString& path);
    void close();
    bool isOpen() const;
    QString path() const { return m_path; }

    bool vacuumInto(const QString& targetPath);
    bool restoreFrom(const QString& sourcePath);

    QSqlDatabase& db() { return m_db; }

    // ── Chat: Odalar ──────────────────────────────────────────────────────
    bool saveChatRoom(const QString& id, const QString& name,
                      const QString& classId = {},
                      bool permanent = false);
    bool deleteChatRoom(const QString& id);

    // ── Chat: Mesajlar ────────────────────────────────────────────────────
    bool saveChatMessage(const QString& id,
                         const QString& roomId,
                         const QString& fromHost,
                         int senderRole,
                         const QString& content,
                         qint64 timestamp,
                         bool isSystem = false);

    QList<ChatMessageRecord> getChatMessages(const QString& roomId,
                                              int limit = 200,
                                              qint64 beforeTimestamp = 0) const;

    void purgeChatMessages(int olderThanDays = -1);

    // ── Saklama ve Arşivleme (Rolling 2-Week Archive) ──────────────────────
    int  chatRetentionDays() const;
    void setChatRetentionDays(int days);
    int  archiveRetentionDays() const;
    void setArchiveRetentionDays(int days);
    bool archiveOldChat(int chatDays = -1);

private:
    explicit OkidiaChatDatabase(QObject* parent = nullptr);
    OkidiaChatDatabase(const OkidiaChatDatabase&)            = delete;
    OkidiaChatDatabase& operator=(const OkidiaChatDatabase&) = delete;

    bool createChatSchema();
    bool execQuery(const QString& sql) const;
    bool execQuery(QSqlQuery& q, const QString& context = {}) const;

    QSqlDatabase m_db;
    QString      m_path;
    bool         m_open = false;
};
