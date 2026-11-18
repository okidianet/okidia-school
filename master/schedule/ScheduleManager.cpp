#include "ScheduleManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../audit/AuditLog.h"

#include <QUuid>
#include <QDateTime>

ScheduleManager::ScheduleManager(SessionManager* session,
                                  QObject* parent)
    : QObject(parent)
    , m_session(session) {}

// ---------------------------------------------------------------------------
QString ScheduleManager::schedule(const ScheduledTask& task) {
    QString id = task.id.isEmpty()
        ? OkidiaUtils::generateUuid().left(8)
        : task.id;

    qint64 msecUntil;
    if (task.runAt.isValid() && task.runAt > QDateTime::currentDateTime()) {
        msecUntil = QDateTime::currentDateTime().msecsTo(task.runAt);
    } else {
        // delay_s yoksa hemen
        msecUntil = 0;
    }

    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(static_cast<int>(
        qBound(static_cast<qint64>(0), msecUntil,
               static_cast<qint64>(INT_MAX))));

    connect(timer, &QTimer::timeout, [this, id]() {
        execute(id);
    });

    Entry entry;
    entry.task  = task;
    entry.task.id = id;
    entry.timer = timer;
    m_entries[id] = entry;

    timer->start();

    const QString target = task.targetClientId.isEmpty() ? QObject::tr("Tüm Sınıf") : task.targetClientId;

    Logger::info(QObject::tr("ScheduleManager: Görev planlandı → [") + id + "] " +
                 task.label + " @ " +
                 (task.runAt.isValid()
                    ? task.runAt.toString(QObject::tr("HH:mm:ss"))
                    : QString::number(msecUntil / 1000) + QObject::tr("s sonra")));

    AuditLog::instance().recordCommand({}, target,
        QObject::tr("Zamanlı görev planlandı: ") + task.label);

    return id;
}

bool ScheduleManager::cancel(const QString& taskId) {
    if (!m_entries.contains(taskId)) return false;

    Entry& e = m_entries[taskId];
    e.timer->stop();
    e.timer->deleteLater();

    const QString target = e.task.targetClientId.isEmpty() ? QObject::tr("Tüm Sınıf") : e.task.targetClientId;
    Logger::info(QObject::tr("ScheduleManager: Görev iptal → ") + taskId);
    AuditLog::instance().recordCommand({}, target,
        QObject::tr("Zamanlı görev iptal edildi: ") + e.task.label);

    m_entries.remove(taskId);
    emit taskCancelled(taskId);
    return true;
}

QList<ScheduledTask> ScheduleManager::pending() const {
    QList<ScheduledTask> list;
    for (const auto& e : m_entries)
        list << e.task;
    return list;
}

// ---------------------------------------------------------------------------
void ScheduleManager::execute(const QString& taskId) {
    if (!m_entries.contains(taskId)) return;

    Entry& e = m_entries[taskId];
    ScheduledTask& task = e.task;

    Logger::info(QObject::tr("ScheduleManager: Görev çalıştırılıyor → [") +
                 taskId + "] " + task.label);

    const QString target = task.targetClientId.isEmpty() ? QObject::tr("Tüm Sınıf") : task.targetClientId;

    // Raporlama (AuditLog) için komut tipine göre uygun kategoriye kaydet
    switch (task.envelope.type()) {
    case okidia::MessageType::POWER_SHUTDOWN:
    case okidia::MessageType::POWER_RESTART:
    case okidia::MessageType::POWER_LOGOUT:
    case okidia::MessageType::POWER_HIBERNATE:
        AuditLog::instance().recordPower({}, target,
            QObject::tr("Zamanlı görev çalıştırıldı: ") + task.label);
        break;

    case okidia::MessageType::SCREEN_LOCK:
    case okidia::MessageType::SCREEN_UNLOCK:
    case okidia::MessageType::INPUT_LOCK:
    case okidia::MessageType::INPUT_UNLOCK:
    case okidia::MessageType::WEB_BLOCK_ALL:
    case okidia::MessageType::WEB_UNBLOCK:
        AuditLog::instance().recordSecurity({}, target,
            QObject::tr("Zamanlı görev çalıştırıldı: ") + task.label);
        break;

    case okidia::MessageType::EXAM_START:
    case okidia::MessageType::EXAM_COLLECT:
    case okidia::MessageType::EXAM_END:
        AuditLog::instance().recordExam({}, target,
            QObject::tr("Zamanlı görev çalıştırıldı: ") + task.label);
        break;

    default:
        AuditLog::instance().recordCommand({}, target,
            QObject::tr("Zamanlı görev çalıştırıldı: ") + task.label);
        break;
    }

    if (m_session) {
        if (task.targetClientId.isEmpty()) {
            m_session->broadcastCommand(task.envelope);
        } else {
            m_session->sendCommand(task.targetClientId, task.envelope);
        }
    }

    emit taskExecuted(taskId, task.label);

    // Temizle
    e.timer->deleteLater();
    m_entries.remove(taskId);
}
