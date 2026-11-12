// broker/BellScheduler.h
#pragma once

#include <QObject>
#include <QTimer>
#include <QSet>
#include <QDateTime>
#include <QTimeZone>
#include "ClientRegistry.h"
#include "../common/OkidiaConfig.h"

/**
 * BellScheduler — Okul Dijital Zil ve Teneffüs Kilit Zamanlayıcısı
 *
 * Broker arka planında sürekli çalışarak:
 * 1. Ders giriş (öğrenci/öğretmen) ve çıkış (teneffüs) saatlerini takip eder.
 * 2. Zil saatinde Ajanlara hafif yerel ses çalma komutu (AUDIO_PLAY_LOCAL) fırlatır.
 * 3. Teneffüste Akıllı Tahtalara nezaket süreli (grace period) SCREEN_LOCK gönderir.
 */
class BellScheduler : public QObject {
    Q_OBJECT

public:
    explicit BellScheduler(ClientRegistry* registry, QObject* parent = nullptr);
    ~BellScheduler() override = default;

    void start();
    void stop();

    void setSchoolTimeZone(const QTimeZone& tz);
    void updateSchedule();

    // Manuel tetikleme (Test/Deneme amaçlı)
    void triggerStudentBell();
    void triggerTeacherBell();
    void triggerRecessBell(bool isLastLessonOfDay = false);

    // Durum Uzlaşması (Sabah ilk ders öncesi veya teneffüste yeni açılan istemcileri anında kilitleme)
    void reconcileClientBellState(const QString& clientId);

    // Öğretmen PIN/QR ile veya Master'dan manuel kilit açtığında aynı teneffüs içinde tekrar kilitlememek için işaretle
    void notifyManualUnlock(const QString& clientId);
    void clearClientUnlockState(const QString& clientId);

private slots:
    void onCheckSchedule();

private:
    void broadcastLocalAudio(const QString& assetPath);
    void broadcastRecessLock(bool isLastLessonOfDay = false);
    void broadcastTeacherUnlock();
    void broadcastAutoShutdown();

    ClientRegistry* m_registry = nullptr;
    QTimer*         m_timer    = nullptr;
    QSet<QString>   m_firedKeys;
    QSet<QString>   m_manuallyUnlockedClients;
    int             m_lastDayOfYear = -1;
    QTimeZone       m_schoolTimeZone = QTimeZone::systemTimeZone();
};

