// broker/BellScheduler.cpp
#include "BellScheduler.h"
#include "../common/utils/Logger.h"
#include "../common/db/OkidiaDatabase.h"
#include "messages.pb.h"

BellScheduler::BellScheduler(ClientRegistry* registry, QObject* parent)
    : QObject(parent), m_registry(registry) {
    m_timer = new QTimer(this);
    m_timer->setInterval(5000); // Her 5 saniyede bir kontrol et
    connect(m_timer, &QTimer::timeout, this, &BellScheduler::onCheckSchedule);
}

void BellScheduler::start() {
    if (m_timer && !m_timer->isActive()) {
        m_timer->start();
        Logger::info(QObject::tr("BellScheduler: Okul zil ve teneffüs zamanlayıcısı başlatıldı (Saat Dilimi: %1).")
                     .arg(QString::fromUtf8(m_schoolTimeZone.id())));
    }
}

void BellScheduler::stop() {
    if (m_timer && m_timer->isActive()) {
        m_timer->stop();
        Logger::info(QObject::tr("BellScheduler: Okul zil ve teneffüs zamanlayıcısı durduruldu."));
    }
}

void BellScheduler::setSchoolTimeZone(const QTimeZone& tz) {
    if (tz.isValid()) {
        m_schoolTimeZone = tz;
        Logger::info(QObject::tr("BellScheduler: Okul saat dilimi güncellendi: %1")
                     .arg(QString::fromUtf8(m_schoolTimeZone.id())));
    }
}

void BellScheduler::updateSchedule() {
    m_firedKeys.clear();
    m_manuallyUnlockedClients.clear();
    Logger::info(QObject::tr("BellScheduler: Ders programı ve zil kuralları yenilendi."));
}

void BellScheduler::notifyManualUnlock(const QString& clientId) {
    if (!clientId.isEmpty()) {
        m_manuallyUnlockedClients.insert(clientId);
    }
}

void BellScheduler::clearClientUnlockState(const QString& clientId) {
    if (!clientId.isEmpty()) {
        m_manuallyUnlockedClients.remove(clientId);
    }
}

void BellScheduler::onCheckSchedule() {
    if (!OkidiaConfig::isBellSmartboardEnabled() && !OkidiaConfig::isBellStudentPcEnabled()) {
        return;
    }

    QDateTime now = m_schoolTimeZone.isValid() 
                        ? QDateTime::currentDateTime().toTimeZone(m_schoolTimeZone) 
                        : QDateTime::currentDateTime();

    // Hafta sonu (Cumartesi = 6, Pazar = 7) okul zili ve otomatik kilit devre dışı bırakılır
    if (now.date().dayOfWeek() >= 6) {
        return;
    }

    int currentDay = now.date().dayOfYear();
    if (currentDay != m_lastDayOfYear) {
        m_firedKeys.clear();
        m_manuallyUnlockedClients.clear();
        m_lastDayOfYear = currentDay;
    }

    QString currentTimeStr = now.time().toString("HH:mm");
    const auto schedule = OkidiaConfig::schoolSchedule();

    for (int i = 0; i < schedule.size(); ++i) {
        const auto& period = schedule[i];
        const bool isLastLessonOfDay = (i == schedule.size() - 1);

        // 1. Teneffüs / Çıkış Zili (Öncelikli ders bitimi ve kilit)
        if (!period.recessBell.isEmpty() && period.recessBell == currentTimeStr) {
            QString key = QString("%1_L%2_recess").arg(currentTimeStr).arg(period.lessonNumber);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("BellScheduler: %1. Ders Teneffüs Zili çalıyor (%2)").arg(period.lessonNumber).arg(currentTimeStr));
                triggerRecessBell(isLastLessonOfDay);
            }
        }

        // 2. Öğretmen Giriş Zili
        if (!period.teacherBell.isEmpty() && period.teacherBell == currentTimeStr) {
            QString key = QString("%1_L%2_teacher").arg(currentTimeStr).arg(period.lessonNumber);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("BellScheduler: %1. Ders Öğretmen Zili çalıyor (%2)").arg(period.lessonNumber).arg(currentTimeStr));
                triggerTeacherBell();
            }
        }

        // 3. Öğrenci Giriş Zili
        if (!period.studentBell.isEmpty() && period.studentBell == currentTimeStr) {
            QString key = QString("%1_L%2_student").arg(currentTimeStr).arg(period.lessonNumber);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("BellScheduler: %1. Ders Öğrenci Zili çalıyor (%2)").arg(period.lessonNumber).arg(currentTimeStr));
                triggerStudentBell();
            }
        }
    }

    // 4. Mesai Bitimi Otomatik Kapatma (Enerji Tasarrufu)
    if (OkidiaConfig::isAutoShutdownEnabled()) {
        QString shutdownTime = OkidiaConfig::autoShutdownTime();
        if (!shutdownTime.isEmpty() && shutdownTime == currentTimeStr) {
            QString key = QString("%1_auto_shutdown").arg(currentTimeStr);
            if (!m_firedKeys.contains(key)) {
                m_firedKeys.insert(key);
                Logger::info(QObject::tr("BellScheduler: Mesai bitimi otomatik kapatma devrede (%1)").arg(currentTimeStr));
                broadcastAutoShutdown();
            }
        }
    }
}

void BellScheduler::triggerStudentBell() {
    broadcastLocalAudio(OkidiaConfig::bellStudentAssetPath());
}

void BellScheduler::triggerTeacherBell() {
    broadcastLocalAudio(OkidiaConfig::bellTeacherAssetPath());
    broadcastTeacherUnlock();
}

void BellScheduler::triggerRecessBell(bool isLastLessonOfDay) {
    // Yeni teneffüs başladığında önceki teneffüsteki manuel öğretmen kilit açma istisnalarını sıfırla
    m_manuallyUnlockedClients.clear();

    broadcastLocalAudio(OkidiaConfig::bellRecessAssetPath());

    if (OkidiaConfig::isBellAutoLockRecess() || OkidiaConfig::isRecessLogoutStudentPCs()) {
        broadcastRecessLock(isLastLessonOfDay);
    }
}

void BellScheduler::reconcileClientBellState(const QString& clientId) {
    if (!m_registry || clientId.isEmpty()) return;

    if (!OkidiaConfig::isBellSmartboardEnabled() && !OkidiaConfig::isBellStudentPcEnabled()) {
        return;
    }

    bool autoLockSmartboards = OkidiaConfig::isBellAutoLockRecess();
    bool lockOrLogoutStudentPCs = OkidiaConfig::isRecessLogoutStudentPCs() ||
                                  (OkidiaConfig::isBellStudentPcEnabled() && OkidiaConfig::isBellAutoLockRecess());

    if (!autoLockSmartboards && !lockOrLogoutStudentPCs) {
        return;
    }

    // Öğretmen bu teneffüs/sabah periyodu içinde cihazı PIN/QR veya Master ile iradi olarak açtıysa
    // oturum geçişi kaynaklı CLIENT_INFO yüzünden cihazı tekrar kilitleme
    if (m_manuallyUnlockedClients.contains(clientId)) {
        Logger::info(QObject::tr("BellScheduler: %1 öğretmen tarafından bu periyotta açıldığı için otomatik kilit uzlaşması atlandı.")
                     .arg(clientId));
        return;
    }

    QDateTime now = m_schoolTimeZone.isValid()
                        ? QDateTime::currentDateTime().toTimeZone(m_schoolTimeZone)
                        : QDateTime::currentDateTime();

    // Hafta sonu otomatik kilit uygulanmaz
    if (now.date().dayOfWeek() >= 6) {
        return;
    }

    const auto schedule = OkidiaConfig::schoolSchedule();
    if (schedule.isEmpty()) return;

    auto parseBellTime = [](const QString& str) -> QTime {
        QTime t = QTime::fromString(str.trimmed(), "HH:mm");
        if (!t.isValid()) t = QTime::fromString(str.trimmed(), "H:mm");
        return t;
    };

    auto lessonStartTime = [&](const OkidiaConfig::LessonPeriod& p) -> QTime {
        QTime t = parseBellTime(p.teacherBell);
        if (!t.isValid()) t = parseBellTime(p.studentBell);
        return t;
    };

    const QTime curTime = now.time();
    const QTime firstLessonStart = lessonStartTime(schedule.first());
    const QTime lastLessonRecess = parseBellTime(schedule.last().recessBell);

    bool isPreFirstLesson = false;
    bool isInterLessonRecess = false;
    bool isAfterLastBell = false;

    if (firstLessonStart.isValid() && curTime >= QTime(5, 0) && curTime < firstLessonStart) {
        isPreFirstLesson = true;
    } else if (lastLessonRecess.isValid() && curTime >= lastLessonRecess) {
        isAfterLastBell = true;
    } else {
        for (int i = 0; i < schedule.size() - 1; ++i) {
            QTime rTime = parseBellTime(schedule[i].recessBell);
            QTime nextStart = lessonStartTime(schedule[i + 1]);
            if (rTime.isValid() && nextStart.isValid() && curTime >= rTime && curTime < nextStart) {
                isInterLessonRecess = true;
                break;
            }
        }
    }

    // Bağlı istemciyi bul
    Connection* clientConn = nullptr;
    QString machineId = clientId;
    QString hostname = clientId;
    for (const auto& c : m_registry->allClients()) {
        if (c.clientId == clientId) {
            clientConn = c.connection;
            machineId = c.machineId;
            hostname = c.hostname;
            break;
        }
    }
    if (!clientConn || !clientConn->isConnected()) return;

    auto agentRec = OkidiaDatabase::instance().agent(machineId);
    int agentType = agentRec.agentType;

    // Tip 2 (İdari PC) asla kilitlenmez
    if (agentType == 2) return;

    // Özel Profil kontrolü: Teneffüs kilidi kapalıysa kilit ve uzlaşmadan tamamen muaftır (Örn: Zil PC)
    bool customRecessLock = false;
    if (agentType >= 100) {
        auto prof = OkidiaDatabase::instance().customProfile(agentType);
        if (prof.id > 0) {
            customRecessLock = prof.recessLock;
        }
        if (!customRecessLock) {
            return;
        }
    }

    uint32_t sbPolicy = OkidiaConfig::smartboardLockPolicy();

    // ─────────────────────────────────────────────────────────────
    // 1. AKTİF DERS SAATİ UZLAŞMASI (Ders Başlamışken Açılan Cihazlar)
    // ─────────────────────────────────────────────────────────────
    if (!isPreFirstLesson && !isInterLessonRecess && !isAfterLastBell) {
        // Cihaz teneffüste kilitlenip kapatılmış ve ders saatinde açılmışsa,
        // kapalıyken kaçırdığı SCREEN_UNLOCK sinyalini burada telafi et!

        // Aktif sınav, ekran yayını, uzaktan yönetim veya ekran izleme varsa dokunma (Anti-Sabotaj)
        const auto activeLocks = m_registry->activeLocks();
        if (activeLocks.contains(clientId)) {
            const QString lt = activeLocks[clientId].lockType;
            if (lt == "Exam" || lt == "Broadcast" || lt == "RemoteControl" || lt == "ScreenView") {
                return;
            }
            // Öğretmen/yönetici tarafından Master üzerinden verilmiş manuel bir kilit varsa dokunma
            if (activeLocks[clientId].masterUserId != "system_bell" && !activeLocks[clientId].masterUserId.isEmpty()) {
                return;
            }
        }

        bool unlockSmartboards = OkidiaConfig::isBellAutoUnlockTeacher() && (sbPolicy != 2);
        bool shouldUnlock = false;

        if (agentType == 3 && unlockSmartboards) {
            shouldUnlock = true;
        } else if (agentType == 1 || agentType == 0 || (agentType >= 100 && customRecessLock)) {
            // Sabah erken veya teneffüste system_bell tarafından kilitlenmiş Öğrenci / Özel PC ise aç
            if (activeLocks.contains(clientId) && activeLocks[clientId].masterUserId == "system_bell") {
                shouldUnlock = true;
            }
        }

        if (shouldUnlock) {
            okidia::Envelope unlockEnv;
            unlockEnv.set_type(okidia::MessageType::SCREEN_UNLOCK);
            unlockEnv.set_target_id(clientId.toStdString());
            clientConn->sendEnvelope(unlockEnv);

            if (activeLocks.contains(clientId) && activeLocks[clientId].masterUserId == "system_bell") {
                m_registry->releaseAgentLock(clientId);
            }

            Logger::info(QObject::tr("BellScheduler: Ders saati içinde açılan/bağlanan %1 (%2) için ekran kilidi açıldı (SCREEN_UNLOCK reconcile).")
                         .arg(hostname, clientId));
        }
        return;
    }

    // Son çıkış zili sonrası (Gün sonu) ise Hibrit Kurs/Etüt politikasını denetle
    if (isAfterLastBell) {
        int lastBellMode = OkidiaConfig::lastBellLockBehavior();
        if (lastBellMode == 1) {
            // Kurs / Etüt Serbest Modu: Son zilden sonra asla kilitleme
            return;
        } else if (lastBellMode == 0) {
            // Akıllı Hibrit: Öğrenci PC'leri kilitleme; Akıllı Tahtaları yalnızca QR/PIN modundaysa kilitle
            if (agentType != 3 || sbPolicy != 2) {
                return;
            }
        }
        // lastBellMode == 2 (Her Zaman Kilitle) ise aşağıdan normal kilit akışı devam eder
    }

    QString lockMsg = isPreFirstLesson
        ? QObject::tr("Ders Başlamadı — Ekran Kilitli")
        : QObject::tr("Ders Bitti, İyi Teneffüsler!");

    if (agentType == 3 && autoLockSmartboards) {
        okidia::ScreenLockCommand lockCmd;
        lockCmd.set_message(lockMsg.toStdString());
        lockCmd.set_lock_policy(sbPolicy);
        lockCmd.set_grace_period_sec(0); // Yeni açılan cihazda başında öğretmen olmadığı için anında kilitle

        okidia::Envelope env;
        env.set_type(okidia::MessageType::SCREEN_LOCK);
        env.set_payload(lockCmd.SerializeAsString());
        clientConn->sendEnvelope(env);

        m_registry->acquireAgentLock(clientId, "system_bell", QObject::tr("Teneffüs Zamanlayıcısı"), "ScreenLock");
        Logger::info(QObject::tr("BellScheduler: Ders dışı/teneffüs saatinde açılan Akıllı Tahta anında kilitlendi → %1 (%2)")
                     .arg(hostname, clientId));
    } else if (((agentType == 1 || agentType == 0) || (agentType >= 100 && customRecessLock)) && lockOrLogoutStudentPCs) {
        // Öğrenci PC'de sonsuz oturum kapatma (autologin-logout) döngüsü oluşmaması için
        // teneffüs ortasında veya sabah erken açılışta klasik ekran perdesi (lock_policy = 1, grace_period_sec = 0) uygulanır.
        okidia::ScreenLockCommand lockCmd;
        lockCmd.set_message(lockMsg.toStdString());
        lockCmd.set_lock_policy(1);
        lockCmd.set_grace_period_sec(0);

        okidia::Envelope env;
        env.set_type(okidia::MessageType::SCREEN_LOCK);
        env.set_payload(lockCmd.SerializeAsString());
        clientConn->sendEnvelope(env);

        m_registry->acquireAgentLock(clientId, "system_bell", QObject::tr("Teneffüs Zamanlayıcısı"), "ScreenLock");
        Logger::info(QObject::tr("BellScheduler: Ders dışı/teneffüs saatinde açılan Öğrenci PC anında kilitlendi → %1 (%2)")
                     .arg(hostname, clientId));
    }
}

void BellScheduler::broadcastLocalAudio(const QString& assetPath) {
    if (!m_registry) return;

    bool playSmartboards = OkidiaConfig::isBellSmartboardEnabled();
    bool playStudentPCs  = OkidiaConfig::isBellStudentPcEnabled();

    if (!playSmartboards && !playStudentPCs) return;

    okidia::AudioPlayLocalCommand cmd;
    cmd.set_anons_path(assetPath.toStdString());
    cmd.set_repeat_count(1);
    cmd.set_is_bell(true); // Standart anons değil, ders/teneffüs zili

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_PLAY_LOCAL);
    env.set_payload(cmd.SerializeAsString());

    const auto clients = m_registry->allClients();
    for (const auto& client : clients) {
        if (!client.connection || !client.connection->isConnected()) {
            continue;
        }

        auto agentRec = OkidiaDatabase::instance().agent(client.machineId);
        int type = agentRec.agentType;

        // Tip 2: İdari PC (Zil ve anons sisteminden kesinlikle soyutlanmış)
        if (type == 2) {
            continue;
        }

        bool shouldPlay = false;
        if (type == 3 && playSmartboards) {
            shouldPlay = true; // Akıllı Tahta
        } else if ((type == 1 || type == 0) && playStudentPCs) {
            shouldPlay = true; // Öğrenci PC veya henüz atanmamış standart istemci
        } else if (type >= 100) {
            auto prof = OkidiaDatabase::instance().customProfile(type);
            if (prof.id > 0 && prof.playBell) {
                shouldPlay = true; // Özel Profil (Örn: Zil & Amfi PC)
            }
        }

        if (shouldPlay) {
            client.connection->sendEnvelope(env);
        }
    }
}

void BellScheduler::broadcastRecessLock(bool isLastLessonOfDay) {
    if (!m_registry) return;

    int graceSec = OkidiaConfig::bellGracePeriodSec();
    if (graceSec <= 0) graceSec = 30;

    uint32_t lockPolicy = OkidiaConfig::smartboardLockPolicy();
    bool autoLockSmartboards = OkidiaConfig::isBellAutoLockRecess();
    bool logoutStudentPCs    = OkidiaConfig::isRecessLogoutStudentPCs();

    // Gün sonu (en son dersin çıkış zili) için Hibrit Kurs / Etüt politikasını uygula
    if (isLastLessonOfDay) {
        int lastBellMode = OkidiaConfig::lastBellLockBehavior();
        if (lastBellMode == 1) {
            // 1 = Kurs / Etüt Serbest Modu (Son Zilde Asla Kilitleme)
            Logger::info(QObject::tr("BellScheduler: Gün sonu çıkış zili — Kurs/Etüt serbest modu aktif olduğu için otomatik kilit ve oturum kapatma atlandı."));
            return;
        } else if (lastBellMode == 0) {
            // 0 = Akıllı Hibrit (Varsayılan):
            // - Klasik Perde modunda bir daha öğretmen zili çalmayacağı için kurs/etüt öğretmenini engellememek adına kilitleme yapılmaz.
            // - Dinamik QR & PIN modunda ise öğrenciler çıkarken tahta kilitlenir; kurs öğretmeni kendi PIN koduyla açabilir.
            // - Öğrenci PC'lerde kurs/etüt kullanımı için son zilde oturum kapatılmaz (enerji koruması mesai sonu kapatma saatiyle sağlanır).
            logoutStudentPCs = false;
            if (lockPolicy != 2) {
                autoLockSmartboards = false;
                Logger::info(QObject::tr("BellScheduler: Gün sonu çıkış zili (Akıllı Hibrit) — Klasik perde modunda kurs/etüt kullanımı için otomatik kilit atlandı."));
            } else {
                Logger::info(QObject::tr("BellScheduler: Gün sonu çıkış zili (Akıllı Hibrit) — QR/PIN kilit modunda Akıllı Tahtalar kilitleniyor (kurs öğretmeni PIN ile açabilir)."));
            }
        }
        // lastBellMode == 2 (Her Zaman Kilitle) ise mevcut autoLockSmartboards ve logoutStudentPCs aynen uygulanır
    }

    if (!autoLockSmartboards && !logoutStudentPCs) return;

    // Tip 3: Akıllı Tahta için teneffüs kilit komutu
    okidia::ScreenLockCommand lockCmd;
    lockCmd.set_message(QObject::tr("Ders Bitti, İyi Teneffüsler!").toStdString());
    lockCmd.set_lock_policy(lockPolicy);
    lockCmd.set_grace_period_sec(static_cast<uint32_t>(graceSec));

    okidia::Envelope smartboardEnv;
    smartboardEnv.set_type(okidia::MessageType::SCREEN_LOCK);
    smartboardEnv.set_payload(lockCmd.SerializeAsString());

    // Tip 1: Öğrenci Bilgisayarı için teneffüs oturum kapatma komutu (lock_policy = 3)
    okidia::ScreenLockCommand logoutCmd;
    logoutCmd.set_message(QObject::tr("Ders Bitti, İyi Teneffüsler!").toStdString());
    logoutCmd.set_lock_policy(3); // 3 = Teneffüs Oturumu Kapat
    logoutCmd.set_grace_period_sec(static_cast<uint32_t>(graceSec));

    okidia::Envelope studentEnv;
    studentEnv.set_type(okidia::MessageType::SCREEN_LOCK);
    studentEnv.set_payload(logoutCmd.SerializeAsString());

    const auto clients = m_registry->allClients();
    for (const auto& client : clients) {
        if (!client.connection || !client.connection->isConnected()) {
            continue;
        }

        auto agentRec = OkidiaDatabase::instance().agent(client.machineId);
        int agentType = agentRec.agentType;

        if (agentType == 3 && autoLockSmartboards) {
            client.connection->sendEnvelope(smartboardEnv);
            m_registry->acquireAgentLock(client.clientId, "system_bell", QObject::tr("Teneffüs Zamanlayıcısı"), "ScreenLock");
        } else if ((agentType == 1 || agentType == 0) && logoutStudentPCs) {
            client.connection->sendEnvelope(studentEnv);
        } else if (agentType >= 100 && logoutStudentPCs) {
            auto prof = OkidiaDatabase::instance().customProfile(agentType);
            if (prof.id > 0 && prof.recessLock) {
                client.connection->sendEnvelope(studentEnv);
            }
        }
    }
}

void BellScheduler::broadcastTeacherUnlock() {
    if (!m_registry) return;

    bool unlockSmartboards = OkidiaConfig::isBellAutoUnlockTeacher() && (OkidiaConfig::smartboardLockPolicy() != 2);
    const auto activeLocks = m_registry->activeLocks();

    const auto clients = m_registry->allClients();
    for (const auto& client : clients) {
        if (!client.connection || !client.connection->isConnected()) {
            continue;
        }

        // Aktif sınav, ekran yayını veya uzak kontrol varsa kesinlikle dokunma
        if (activeLocks.contains(client.clientId)) {
            const QString lt = activeLocks[client.clientId].lockType;
            if (lt == "Exam" || lt == "Broadcast" || lt == "RemoteControl" || lt == "ScreenView") {
                continue;
            }
        }

        auto agentRec = OkidiaDatabase::instance().agent(client.machineId);
        int agentType = agentRec.agentType;

        bool shouldUnlock = false;
        if (agentType == 3 && unlockSmartboards) {
            shouldUnlock = true;
        } else if (agentType == 1 || agentType == 0 || agentType >= 100) {
            // Sabah ilk ders öncesi veya teneffüs ortasında açıldığı için system_bell tarafından kilitlenmiş Öğrenci / Özel PC'leri aç
            if (activeLocks.contains(client.clientId) && activeLocks[client.clientId].masterUserId == "system_bell") {
                shouldUnlock = true;
            }
        }

        if (shouldUnlock) {
            okidia::Envelope unlockEnv;
            unlockEnv.set_type(okidia::MessageType::SCREEN_UNLOCK);
            unlockEnv.set_target_id(client.clientId.toStdString());
            client.connection->sendEnvelope(unlockEnv);

            okidia::Envelope inputUnlockEnv;
            inputUnlockEnv.set_type(okidia::MessageType::INPUT_UNLOCK);
            inputUnlockEnv.set_target_id(client.clientId.toStdString());
            client.connection->sendEnvelope(inputUnlockEnv);

            m_registry->releaseAgentLock(client.clientId);
            Logger::info(QObject::tr("BellScheduler: Öğretmen zili ile istemci kilidi otomatik kaldırıldı → %1 (%2)")
                         .arg(client.hostname, client.clientId));
        }
    }
}

void BellScheduler::broadcastAutoShutdown() {
    if (!m_registry) return;

    bool shutdownSmartboards = OkidiaConfig::autoShutdownSmartboards();
    bool shutdownStudentPCs  = OkidiaConfig::autoShutdownStudentPCs();

    if (!shutdownSmartboards && !shutdownStudentPCs) return;

    okidia::PowerCommand pCmd;
    pCmd.set_delay_s(30);
    pCmd.set_reason(QObject::tr("Mesai bitimi enerji tasarrufu nedeniyle bilgisayar kapatılıyor.").toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::POWER_SHUTDOWN);
    env.set_payload(pCmd.SerializeAsString());

    const auto clients = m_registry->allClients();
    for (const auto& client : clients) {
        if (!client.connection || !client.connection->isConnected()) {
            continue;
        }

        auto agentRec = OkidiaDatabase::instance().agent(client.machineId);
        int type = agentRec.agentType;

        // Tip 2: İdari PC (KVKK ve idari çalışma gereği ASLA otomatik kapatılmaz)
        if (type == 2) {
            continue;
        }

        bool shouldShutdown = false;
        if (type == 3 && shutdownSmartboards) {
            shouldShutdown = true; // Akıllı Tahta
        } else if (type == 1 && shutdownStudentPCs) {
            shouldShutdown = true; // Öğrenci PC
        } else if (type >= 100 && shutdownStudentPCs) {
            auto prof = OkidiaDatabase::instance().customProfile(type);
            if (prof.id > 0 && prof.recessLock) {
                shouldShutdown = true; // Sadece teneffüste kapanabilen özel profiller mesai sonunda kapatılsın
            }
        }

        if (shouldShutdown) {
            client.connection->sendEnvelope(env);
            Logger::info(QObject::tr("BellScheduler: Enerji tasarrufu otomatik kapatma emri gönderildi: %1 (%2, Tip %3)")
                         .arg(client.hostname, client.machineId).arg(type));
        }
    }
}
