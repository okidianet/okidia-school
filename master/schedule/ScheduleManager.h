// master/schedule/ScheduleManager.h
#pragma once

#include <QObject>
#include <QMap>
#include <QTimer>
#include "../../common/network/Connection.h"

struct ScheduledTask {
    QString       id;
    QString       label;
    QDateTime     runAt;
    okidia::Envelope envelope;   // Çalıştırılacak komut
    QString       targetClientId; // Boş = broadcast
};

/**
 * Zamanlı komut yöneticisi.
 *
 * Kullanım:
 *   manager->schedule(task);  → QTimer ile bekle
 *   manager->cancel(taskId);  → iptal et
 *   manager->pending();       → bekleyen görevler listesi
 *
 * Görev zamanı gelince SessionManager üzerinden komutu gönderir.
 */
class SessionManager;

class ScheduleManager : public QObject {
    Q_OBJECT

public:
    explicit ScheduleManager(SessionManager* session,
                              QObject* parent = nullptr);

    // Görev ekle — id döner
    QString schedule(const ScheduledTask& task);

    // Görev iptal et
    bool cancel(const QString& taskId);

    // Bekleyen görevler
    QList<ScheduledTask> pending() const;

signals:
    void taskExecuted(const QString& taskId, const QString& label);
    void taskCancelled(const QString& taskId);

private:
    SessionManager* m_session = nullptr;

    struct Entry {
        ScheduledTask task;
        QTimer*       timer = nullptr;
    };

    QMap<QString, Entry> m_entries;

    void execute(const QString& taskId);
};
