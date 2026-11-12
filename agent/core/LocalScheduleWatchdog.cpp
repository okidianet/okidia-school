// agent/core/LocalScheduleWatchdog.cpp
#include "LocalScheduleWatchdog.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTime>
#include "../../common/utils/Logger.h"
#include "../system/PersistentStateManager.h"

LocalScheduleWatchdog::LocalScheduleWatchdog(QObject* parent)
    : QObject(parent)
{
    loadFromDisk();

    m_timer = new QTimer(this);
    m_timer->setInterval(15000); // 15 saniyede bir tam dakika kontrolü
    connect(m_timer, &QTimer::timeout, this, &LocalScheduleWatchdog::onCheckSchedule);
    m_timer->start();
}

QString LocalScheduleWatchdog::configFilePath() const {
#if defined(Q_OS_WIN)
    return "C:/ProgramData/Okidia/schedule.json";
#else
    return "/var/lib/okidia/schedule.json";
#endif
}

void LocalScheduleWatchdog::setBrokerConnected(bool connected) {
    if (m_brokerConnected != connected) {
        m_brokerConnected = connected;
        if (m_brokerConnected) {
            Logger::info(QObject::tr("LocalScheduleWatchdog: Broker bağlantısı aktif. Yerel otonom zil/kilit pasif modda."));
        } else {
            Logger::warning(QObject::tr("LocalScheduleWatchdog: Broker bağlantısı kesildi! Yerel otonom zil & kilit sistemi DEVREDE."));
        }
    }
}

void LocalScheduleWatchdog::setRtcBatteryLow(bool low) {
    m_rtcBatteryLow = low;
    if (m_rtcBatteryLow) {
        Logger::warning(QObject::tr("LocalScheduleWatchdog: BIOS pili bitik / sistem saati şüpheli! Otonom kilit askıya alındı (fail-safe mod)."));
    }
}

void LocalScheduleWatchdog::updateSchedule(const QString& json,
                                            uint32_t lockPolicy,
                                            const QString& timeZone,
                                            bool smartboardBell,
                                            bool studentPcBell,
                                            bool autoLockRecess,
                                            int lastBellMode)
{
    saveToDisk(json, lockPolicy, timeZone, smartboardBell, studentPcBell, autoLockRecess, lastBellMode);
    loadFromDisk();
}

void LocalScheduleWatchdog::saveToDisk(const QString& json,
                                       uint32_t lockPolicy,
                                       const QString& timeZone,
                                       bool smartboardBell,
                                       bool studentPcBell,
                                       bool autoLockRecess,
                                       int lastBellMode)
{
    const QString path = configFilePath();
    QDir().mkpath(QFileInfo(path).dir().absolutePath());

    QJsonObject root;
    root["schedule_json"] = json;
    root["lock_policy"] = static_cast<int>(lockPolicy);
    root["timezone"] = timeZone;
    root["bell_smartboard"] = smartboardBell;
    root["bell_student_pc"] = studentPcBell;
    root["auto_lock_recess"] = autoLockRecess;
    root["last_bell_mode"] = lastBellMode;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
        Logger::info(QObject::tr("LocalScheduleWatchdog: Güncel ders programı yerel diske kaydedildi -> ") + path);
    } else {
        Logger::error(QObject::tr("LocalScheduleWatchdog: Ders programı dosyaya yazılamadı: ") + path);
    }
}

void LocalScheduleWatchdog::loadFromDisk() {
    const QString path = configFilePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) return;

    QJsonObject root = doc.object();
    m_lockPolicy = static_cast<uint32_t>(root.value("lock_policy").toInt(1));
    m_bellSmartboardEnabled = root.value("bell_smartboard").toBool(true);
    m_bellStudentPcEnabled = root.value("bell_student_pc").toBool(false);
    m_bellAutoLockRecess = root.value("auto_lock_recess").toBool(true);
    m_lastBellLockBehavior = root.value("last_bell_mode").toInt(0);

    QString tzStr = root.value("timezone").toString();
    if (!tzStr.isEmpty()) {
        QTimeZone tz(tzStr.toUtf8());
        if (tz.isValid()) {
            m_timeZone = tz;
        }
    }

    QString schedJson = root.value("schedule_json").toString();
    if (schedJson.isEmpty()) return;

    QJsonDocument sDoc = QJsonDocument::fromJson(schedJson.toUtf8());
    if (!sDoc.isArray()) return;

    m_schedule.clear();
    const auto arr = sDoc.array();
    for (const auto& val : arr) {
        if (!val.isObject()) continue;
        QJsonObject o = val.toObject();
        OkidiaConfig::LessonPeriod p;
        p.lessonNumber = o.value("lessonNumber").toInt();
        p.studentBell  = o.value("studentBell").toString();
        p.teacherBell  = o.value("teacherBell").toString();
        p.recessBell   = o.value("recessBell").toString();
        m_schedule.append(p);
    }

    Logger::info(QObject::tr("LocalScheduleWatchdog: Yerel diskten %1 periyotluk ders programı yüklendi.")
                 .arg(m_schedule.size()));
}

void LocalScheduleWatchdog::onCheckSchedule() {
    // 1. Broker bağlıysa otonom tetikleyici pasif kalır (çift tetiklemeyi önler)
    if (m_brokerConnected) return;

    // 2. Program boşsa yapacak işlem yok
    if (m_schedule.isEmpty()) return;

    // 3. BIOS pili bitikse ve ağ yoksa kilit mekanizmasını güvenli moda al (fail-safe)
    if (m_rtcBatteryLow) return;

    QDateTime now = m_timeZone.isValid()
                        ? QDateTime::currentDateTime().toTimeZone(m_timeZone)
                        : QDateTime::currentDateTime();

    // Hafta sonu otonom zil/kilit işletilmez
    if (now.date().dayOfWeek() >= 6) return;

    int currentDay = now.date().dayOfYear();
    if (currentDay != m_lastDayOfYear) {
        m_firedKeys.clear();
        m_lastDayOfYear = currentDay;
    }

    auto parseBellTime = [](const QString& str) -> QTime {
        QTime t = QTime::fromString(str.trimmed(), "HH:mm");
        if (!t.isValid()) t = QTime::fromString(str.trimmed(), "H:mm");
        return t;
    };

    // 4. Otonom Ders Saati İçi Kilit Telafisi (Cihaz ders başlamışken offline açıldıysa)
    if (m_bellAutoLockRecess && m_lockPolicy != 2 && PersistentStateManager::isScreenLocked()) {
        const QTime curTime = now.time();
        for (const auto& period : m_schedule) {
            QTime startTime = parseBellTime(period.teacherBell);
            if (!startTime.isValid()) startTime = parseBellTime(period.studentBell);
            QTime endTime = parseBellTime(period.recessBell);

            if (startTime.isValid() && endTime.isValid() && curTime >= startTime && curTime < endTime) {
                QString unlockKey = QString("L%1_auto_reconcile_unlock").arg(period.lessonNumber);
                if (!m_firedKeys.contains(unlockKey)) {
                    m_firedKeys.insert(unlockKey);
                    Logger::info(QObject::tr("LocalScheduleWatchdog [OTONOM]: %1. Ders saati içinde açılan cihazın ekran kilidi kaldırıldı.")
                                 .arg(period.lessonNumber));
                    PersistentStateManager::clearScreenLock();
                    emit triggerSmartboardUnlock();
                }
                break;
            }
        }
    }

    QString currentTimeStr = now.time().toString("HH:mm");

    for (int i = 0; i < m_schedule.size(); ++i) {
        const auto& period = m_schedule[i];
        const bool isLastLessonOfDay = (i == m_schedule.size() - 1);

        // 1. Teneffüs / Çıkış Zili
        if (!period.recessBell.isEmpty() && period.recessBell == currentTimeStr) {
            QString key = QString("%1_L%2_recess").arg(currentTimeStr).arg(period.lessonNumber);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("LocalScheduleWatchdog [OTONOM]: %1. Ders Teneffüs Zili çalıyor (%2)")
                             .arg(period.lessonNumber).arg(currentTimeStr));
                emit triggerLocalAudio(OkidiaConfig::bellRecessAssetPath());

                if (m_bellAutoLockRecess) {
                    if (isLastLessonOfDay && m_lastBellLockBehavior == 1) {
                        // Kurs/Etüt serbest modu
                    } else {
                        emit triggerSmartboardLock();
                    }
                }
            }
        }

        // 2. Öğretmen Giriş Zili
        if (!period.teacherBell.isEmpty() && period.teacherBell == currentTimeStr) {
            QString key = QString("%1_L%2_teacher").arg(currentTimeStr).arg(period.lessonNumber);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("LocalScheduleWatchdog [OTONOM]: %1. Ders Öğretmen Zili çalıyor (%2)")
                             .arg(period.lessonNumber).arg(currentTimeStr));
                emit triggerLocalAudio(OkidiaConfig::bellTeacherAssetPath());
                emit triggerSmartboardUnlock();
            }
        }

        // 3. Öğrenci Giriş Zili
        if (!period.studentBell.isEmpty() && period.studentBell == currentTimeStr) {
            QString key = QString("%1_L%2_student").arg(currentTimeStr).arg(period.lessonNumber);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("LocalScheduleWatchdog [OTONOM]: %1. Ders Öğrenci Zili çalıyor (%2)")
                             .arg(period.lessonNumber).arg(currentTimeStr));
                emit triggerLocalAudio(OkidiaConfig::bellStudentAssetPath());
            }
        }
    }
}

