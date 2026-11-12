// agent/core/LocalScheduleWatchdog.h
#pragma once

#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QTimeZone>
#include <QSet>
#include <QList>
#include <QString>
#include "../../common/OkidiaConfig.h"

class LocalScheduleWatchdog : public QObject {
    Q_OBJECT

public:
    explicit LocalScheduleWatchdog(QObject* parent = nullptr);
    ~LocalScheduleWatchdog() override = default;

    void setBrokerConnected(bool connected);
    void setRtcBatteryLow(bool low);
    bool isRtcBatteryLow() const { return m_rtcBatteryLow; }

    void updateSchedule(const QString& json,
                        uint32_t lockPolicy,
                        const QString& timeZone,
                        bool smartboardBell,
                        bool studentPcBell,
                        bool autoLockRecess,
                        int lastBellMode);

signals:
    void triggerLocalAudio(const QString& filePath);
    void triggerSmartboardLock();
    void triggerSmartboardUnlock();

private slots:
    void onCheckSchedule();

private:
    void loadFromDisk();
    void saveToDisk(const QString& json,
                    uint32_t lockPolicy,
                    const QString& timeZone,
                    bool smartboardBell,
                    bool studentPcBell,
                    bool autoLockRecess,
                    int lastBellMode);
    QString configFilePath() const;

    QTimer* m_timer = nullptr;
    bool m_brokerConnected = false;
    bool m_rtcBatteryLow = false;
    int m_lastDayOfYear = -1;
    QSet<QString> m_firedKeys;

    QList<OkidiaConfig::LessonPeriod> m_schedule;
    uint32_t m_lockPolicy = 1;
    QTimeZone m_timeZone = QTimeZone::systemTimeZone();
    bool m_bellSmartboardEnabled = true;
    bool m_bellStudentPcEnabled = false;
    bool m_bellAutoLockRecess = true;
    int m_lastBellLockBehavior = 0;
};

