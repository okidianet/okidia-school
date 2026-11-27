// agent/commands/CommandHandlerAudio.cpp
#include "CommandHandler.h"
#include "../../common/utils/Logger.h"
#include "../audio/LocalPlaylistPlayer.h"
#include "../ui/PopupMessage/PopupMessage.h"
#include "../ui/BellToastBanner/BellToastBanner.h"
#include <QProcess>
#include <QRegularExpression>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#if !defined(Q_OS_WIN)
#  include <unistd.h>
#  include <pwd.h>
#endif

#include "../system/SystemInfo.h"

#if !defined(Q_OS_WIN)
static bool resolveAudioCommandContext(QString& outUid, QString& outGid, QString& outRunDir) {
    QString username, uid;
    if (SystemInfo::getAudioSession(username, uid) && !uid.isEmpty() && uid != QLatin1String("0")) {
        const QString runDir = QStringLiteral("/run/user/") + uid;
        if (QDir(runDir).exists()) {
            outUid = uid;
            outRunDir = runDir;
            const QFileInfo rtInfo(runDir);
            outGid = QString::number(rtInfo.groupId());
            if (struct passwd* pw = ::getpwuid(static_cast<uid_t>(uid.toUInt()))) {
                outGid = QString::number(pw->pw_gid);
            }
            return true;
        }
    }
    return false;
}

static void prepareLinuxAudioProcess(const QString& prog,
                                     const QStringList& args,
                                     QString& outProg,
                                     QStringList& outArgs,
                                     QProcessEnvironment& outEnv) {
    outEnv = QProcessEnvironment::systemEnvironment();
    outProg = prog;
    outArgs = args;

    if (::geteuid() != 0) {
        if (!outEnv.contains(QStringLiteral("PULSE_RUNTIME_PATH"))) {
            const QString xdg = outEnv.value(QStringLiteral("XDG_RUNTIME_DIR"));
            if (!xdg.isEmpty() && QDir(xdg + QStringLiteral("/pulse")).exists()) {
                outEnv.insert(QStringLiteral("PULSE_RUNTIME_PATH"), xdg + QStringLiteral("/pulse"));
            }
        }
        return;
    }

    QString uid, gid, runDir;
    if (resolveAudioCommandContext(uid, gid, runDir)) {
        outEnv.insert(QStringLiteral("PULSE_RUNTIME_PATH"), runDir + QStringLiteral("/pulse"));
        outEnv.insert(QStringLiteral("XDG_RUNTIME_DIR"), runDir);
        outEnv.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=") + runDir + QStringLiteral("/bus"));
        if (QFile::exists(QStringLiteral("/usr/bin/setpriv"))) {
            QStringList spArgs = {
                QStringLiteral("--reuid=") + uid,
                QStringLiteral("--regid=") + gid
            };
            if (::getpwuid(static_cast<uid_t>(uid.toUInt())) != nullptr) {
                spArgs << QStringLiteral("--init-groups");
            } else {
                spArgs << QStringLiteral("--clear-groups");
            }
            spArgs << prog << args;
            outProg = QStringLiteral("/usr/bin/setpriv");
            outArgs = spArgs;
        }
    }
}
#endif

static bool runAudioCommand(const QString& prog,
                             const QStringList& args) {
    QProcess p;
#if !defined(Q_OS_WIN)
    QString launchProg;
    QStringList launchArgs;
    QProcessEnvironment env;
    prepareLinuxAudioProcess(prog, args, launchProg, launchArgs, env);
    p.setProcessEnvironment(env);
    p.start(launchProg, launchArgs);
#else
    p.start(prog, args);
#endif
    if (!p.waitForStarted(800)) return false;
    if (!p.waitForFinished(1200)) {
        p.kill();
        p.waitForFinished(200);
        return false;
    }
    return p.exitCode() == 0;
}

static QString runAudioOutput(const QString& prog,
                               const QStringList& args) {
    QProcess p;
#if !defined(Q_OS_WIN)
    QString launchProg;
    QStringList launchArgs;
    QProcessEnvironment env;
    prepareLinuxAudioProcess(prog, args, launchProg, launchArgs, env);
    p.setProcessEnvironment(env);
    p.start(launchProg, launchArgs);
#else
    p.start(prog, args);
#endif
    if (!p.waitForStarted(800)) return QString();
    if (!p.waitForFinished(1200)) {
        p.kill();
        p.waitForFinished(200);
        return QString();
    }
    if (p.exitCode() != 0) return QString();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <endpointvolume.h>
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif

struct ScopedWindowsEndpointVolume {
    IAudioEndpointVolume* endpoint = nullptr;
    bool comInitialized = false;

    ScopedWindowsEndpointVolume() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comInitialized = (hr == S_OK || hr == S_FALSE);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            return;
        }
        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_INPROC_SERVER,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) return;

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        enumerator->Release();
        if (FAILED(hr) || !device) return;

        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                              nullptr, reinterpret_cast<void**>(&endpoint));
        device->Release();
        if (FAILED(hr)) {
            endpoint = nullptr;
        }
    }

    ~ScopedWindowsEndpointVolume() {
        if (endpoint) {
            endpoint->Release();
            endpoint = nullptr;
        }
        if (comInitialized) {
            CoUninitialize();
        }
    }
};
#endif

static void readSystemAudioState(int& outVolume, bool& outMuted) {
    outVolume = -1;
    outMuted = false;
#if defined(Q_OS_WIN)
    ScopedWindowsEndpointVolume ep;
    if (ep.endpoint) {
        float currentVol = 0.5f;
        if (SUCCEEDED(ep.endpoint->GetMasterVolumeLevelScalar(&currentVol))) {
            outVolume = qBound(0, qRound(currentVol * 100.0f), 100);
        }
        BOOL isMuted = FALSE;
        if (SUCCEEDED(ep.endpoint->GetMute(&isMuted))) {
            outMuted = (isMuted == TRUE);
        }
    }
#else
    QString vol = runAudioOutput("pactl", {"get-sink-volume", "@DEFAULT_SINK@"});
    if (!vol.isEmpty()) {
        QRegularExpression re(R"((\d+)%)");
        auto m = re.match(vol);
        if (m.hasMatch()) {
            outVolume = qBound(0, m.captured(1).toInt(), 100);
        }
        QString muteOut = runAudioOutput("pactl", {"get-sink-mute", "@DEFAULT_SINK@"});
        outMuted = muteOut.contains("yes", Qt::CaseInsensitive);
        return;
    }

    vol = runAudioOutput("wpctl", {"get-volume", "@DEFAULT_AUDIO_SINK@"});
    if (!vol.isEmpty()) {
        QRegularExpression re2(R"(Volume:\s*([\d.]+))");
        auto m2 = re2.match(vol);
        if (m2.hasMatch()) {
            outVolume = qBound(0, qRound(m2.captured(1).toDouble() * 100.0), 100);
        }
        outMuted = vol.contains("MUTED", Qt::CaseInsensitive);
        return;
    }

    vol = runAudioOutput("amixer", {"-D", "pulse", "sget", "Master"});
    if (!vol.isEmpty()) {
        QRegularExpression re3(R"(\[(\d+)%\])");
        auto m3 = re3.match(vol);
        if (m3.hasMatch()) {
            outVolume = qBound(0, m3.captured(1).toInt(), 100);
        }
        outMuted = vol.contains("[off]", Qt::CaseInsensitive);
    }
#endif
}

static void applySystemAudioState(int volume, bool muted) {
#if defined(Q_OS_WIN)
    ScopedWindowsEndpointVolume ep;
    if (ep.endpoint) {
        if (volume >= 0) {
            ep.endpoint->SetMasterVolumeLevelScalar(qBound(0, volume, 100) / 100.0f, nullptr);
        }
        ep.endpoint->SetMute(muted ? TRUE : FALSE, nullptr);
    }
#else
    if (!muted) {
        if (!runAudioCommand("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "0"})) {
            if (!runAudioCommand("wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "0"})) {
                runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", "unmute"});
            }
        }
    }

    if (volume >= 0) {
        const int clamped = qBound(0, volume, 100);
        const QString pctStr = QString::number(clamped) + "%";
        if (!runAudioCommand("pactl", {"set-sink-volume", "@DEFAULT_SINK@", pctStr})) {
            if (!runAudioCommand("wpctl", {"set-volume", "@DEFAULT_AUDIO_SINK@",
                                           QString::number(clamped / 100.0, 'f', 2)})) {
                runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", pctStr});
            }
        }
    }

    if (muted) {
        if (!runAudioCommand("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "1"})) {
            if (!runAudioCommand("wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "1"})) {
                runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", "mute"});
            }
        }
    }
#endif
}

void CommandHandler::setPriorityAudioActive(const QString& sourceKey, bool active) {
    if (sourceKey.isEmpty()) return;

    if (active) {
        const bool wasEmpty = m_priorityAudioSources.isEmpty();
        m_priorityAudioSources.insert(sourceKey);
        if (wasEmpty && !m_priorityBoostActive) {
            readSystemAudioState(m_boostSavedVolume, m_boostSavedMuted);
            m_priorityBoostActive = true;
            applySystemAudioState(100, false);
            Logger::info(QObject::tr("CommandHandler: Zil/Anons için sistem sesi %100'e yükseltildi (önceki: %1%, sessiz: %2)")
                         .arg(m_boostSavedVolume).arg(m_boostSavedMuted ? QObject::tr("Evet") : QObject::tr("Hayır")));
        }
    } else {
        m_priorityAudioSources.remove(sourceKey);
        if (m_priorityAudioSources.isEmpty() && m_priorityBoostActive) {
            m_priorityBoostActive = false;
            if (m_boostSavedVolume >= 0) {
                applySystemAudioState(m_boostSavedVolume, m_boostSavedMuted);
                Logger::info(QObject::tr("CommandHandler: Zil/Anons bitti, sistem sesi önceki ayarına geri yüklendi (%1%, sessiz: %2)")
                             .arg(m_boostSavedVolume).arg(m_boostSavedMuted ? QObject::tr("Evet") : QObject::tr("Hayır")));
            } else {
                Logger::info(QObject::tr("CommandHandler: Zil/Anons bitti, önceki ses seviyesi okunamadığı için mevcut sistem sesi korundu."));
            }
        }
    }
}

void CommandHandler::audioMute() {
    Logger::info(QObject::tr("CommandHandler: Ses kapatılıyor"));
    if (m_priorityBoostActive) {
        m_boostSavedMuted = true;
    }

#if defined(Q_OS_WIN)
    ScopedWindowsEndpointVolume ep;
    if (ep.endpoint) {
        float currentVol = 0.5f;
        if (SUCCEEDED(ep.endpoint->GetMasterVolumeLevelScalar(&currentVol))) {
            m_savedVolume = qBound(0, qRound(currentVol * 100.0f), 100);
            Logger::info(QObject::tr("CommandHandler: Windows ses seviyesi kaydedildi → %1%").arg(m_savedVolume));
        }
        ep.endpoint->SetMute(TRUE, nullptr);
    }
#else
    QString vol = runAudioOutput("pactl",
        {"get-sink-volume", "@DEFAULT_SINK@"});
    if (vol.isEmpty())
        vol = runAudioOutput("wpctl",
            {"get-volume", "@DEFAULT_AUDIO_SINK@"});

    QRegularExpression re(R"((\d+)%)");
    auto m = re.match(vol);
    if (m.hasMatch())
        m_savedVolume = m.captured(1).toInt();
    else {
        QRegularExpression re2(R"(Volume:\s*([\d.]+))");
        auto m2 = re2.match(vol);
        if (m2.hasMatch())
            m_savedVolume = qRound(m2.captured(1).toDouble() * 100);
    }

    Logger::info(QObject::tr("CommandHandler: Ses seviyesi kaydedildi → %1%").arg(m_savedVolume));

    if (!runAudioCommand("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "1"})) {
        if (!runAudioCommand("wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "1"})) {
            runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", "mute"});
        }
    }
#endif
    sendResult(true);
}

void CommandHandler::audioUnmute() {
    Logger::info(QObject::tr("CommandHandler: Ses açılıyor"));
    if (m_priorityBoostActive) {
        m_boostSavedMuted = false;
    }

#if defined(Q_OS_WIN)
    ScopedWindowsEndpointVolume ep;
    if (ep.endpoint) {
        ep.endpoint->SetMute(FALSE, nullptr);
        if (m_savedVolume > 0) {
            ep.endpoint->SetMasterVolumeLevelScalar(m_savedVolume / 100.0f, nullptr);
            Logger::info(QObject::tr("CommandHandler: Windows ses seviyesi geri yüklendi → %1%").arg(m_savedVolume));
        }
    }
#else
    if (!runAudioCommand("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "0"})) {
        if (!runAudioCommand("wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "0"})) {
            runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", "unmute"});
        }
    }

    if (m_savedVolume > 0) {
        QString pctStr = QString::number(m_savedVolume) + "%";
        Logger::info(QObject::tr("CommandHandler: Ses seviyesi geri yükleniyor → %1").arg(pctStr));
        if (!runAudioCommand("pactl",
                {"set-sink-volume", "@DEFAULT_SINK@", pctStr})) {
            if (!runAudioCommand("wpctl",
                    {"set-volume", "@DEFAULT_AUDIO_SINK@",
                     QString::number(m_savedVolume / 100.0, 'f', 2)})) {
                runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", pctStr});
            }
        }
    }
#endif
    sendResult(true);
}

void CommandHandler::audioUnmuteOnly() {
#if defined(Q_OS_WIN)
    ScopedWindowsEndpointVolume ep;
    if (ep.endpoint) {
        ep.endpoint->SetMute(FALSE, nullptr);
    }
#else
    if (!runAudioCommand("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "0"}))
        if (!runAudioCommand("wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "0"}))
            runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", "unmute"});
#endif
    sendResult(true);
}

void CommandHandler::audioPlayLocal(const okidia::AudioPlayLocalCommand& cmd) {
    const QString anonsPath  = QString::fromStdString(cmd.anons_path());
    const int     repeat     = qMax(1, cmd.repeat_count());
    const bool    isBell     = cmd.is_bell();
    const QString announceId = QString::fromStdString(cmd.announce_id());

    if (!announceId.isEmpty() && m_cancelledAnnounceIds.contains(announceId)) {
        Logger::info(QObject::tr("CommandHandler: Daha önce durdurulmuş (iptal edilmiş) anons komutu yoksayıldı (id=%1)").arg(announceId));
        sendResult(true);
        return;
    }

    if (isBell) {
        // Zil için başlık ve alt metin belirle
        QString bellTitle = QObject::tr("Okul Zili");
        QString bellMsg   = QObject::tr("Zil çalıyor...");
        if (anonsPath.contains("bell_student", Qt::CaseInsensitive)) {
            bellTitle = QObject::tr("Öğrenci Giriş Zili");
            bellMsg   = QObject::tr("Ders başlıyor, iyi dersler!");
        } else if (anonsPath.contains("bell_teacher", Qt::CaseInsensitive)) {
            bellTitle = QObject::tr("Öğretmen Giriş Zili");
            bellMsg   = QObject::tr("Öğretmenlerimiz için ders vakti.");
        } else if (anonsPath.contains("bell_recess", Qt::CaseInsensitive)) {
            bellTitle = QObject::tr("Teneffüs Zili");
            bellMsg   = QObject::tr("İyi teneffüsler!");
        }

        // Öncelik kuralı: O an Yönetici Anonsu çalıyorsa zil sesi çalınmaz, yalnızca Toast Banner gösterilir!
        if (m_announcePlaying && !m_currentPlayingIsBell) {
            Logger::info(QObject::tr("CommandHandler: Yönetici anonsu aktifken zil vaktine gelindi. Ses bastırıldı, görsel bildirim (Toast Banner) gösteriliyor → %1").arg(bellTitle));
            BellToastBanner::showBanner(bellTitle, bellMsg, 10000);
            sendResult(true);
            return;
        }

        // Normal zil durumu: Sağ üstte Toast Banner göster ve çal
        BellToastBanner::showBanner(bellTitle, bellMsg, 8000);
    } else {
        // Gelen Yönetici Anonsu: Eğer o an bir okul zili veya önceki bir anons çalıyorsa anında sustur (preemption)!
        if (m_announcePlaying) {
            Logger::info(QObject::tr("CommandHandler: Yeni Yönetici Anonsu geldi! Çalan ses anında susturuluyor (preemption)..."));
            m_announceQueue.clear();
            if (m_activePlayer) {
                QPointer<LocalPlaylistPlayer> oldPlayer = m_activePlayer;
                m_activePlayer = nullptr;
                oldPlayer->disconnect(this);
                oldPlayer->abort();
            }
            m_announcePlaying = false;
            m_currentPlayingIsBell = false;
            m_currentAnnounceId.clear();
        }
    }

    Logger::info(QObject::tr("CommandHandler: audioPlayLocal (kuyruğa ekleniyor) → %1 ×%2 (Zil: %3, id: %4)")
                 .arg(anonsPath).arg(repeat).arg(isBell ? "Evet" : "Hayır").arg(announceId));

    m_announceQueue.enqueue({anonsPath, repeat, isBell, announceId});
    sendResult(true);

    if (!m_announcePlaying) {
        playNextAnnounce();
    }
}

void CommandHandler::playNextAnnounce() {
    while (!m_announceQueue.isEmpty()) {
        auto next = m_announceQueue.head();
        if (!next.announceId.isEmpty() && m_cancelledAnnounceIds.contains(next.announceId)) {
            m_announceQueue.dequeue();
            continue;
        }
        break;
    }

    if (m_announceQueue.isEmpty()) {
        const bool wasPlaying = m_announcePlaying;
        m_announcePlaying = false;
        m_currentPlayingIsBell = false;
        m_currentAnnounceId.clear();
        m_activePlayer = nullptr;
        setPriorityAudioActive(QStringLiteral("local_announce"), false);
        if (wasPlaying) {
            emit announcePlaybackStateChanged(false);
        }
        return;
    }

    const bool wasPlaying = m_announcePlaying;
    m_announcePlaying = true;
    setPriorityAudioActive(QStringLiteral("local_announce"), true);
    auto item = m_announceQueue.dequeue();
    m_currentPlayingIsBell = item.isBell;
    m_currentAnnounceId = item.announceId;

    if (!wasPlaying) {
        emit announcePlaybackStateChanged(true);
    }

    Logger::info(QObject::tr("CommandHandler: Kuyruktaki anons başlatılıyor → %1 ×%2 (Zil: %3, id: %4)")
                 .arg(item.path).arg(item.repeat).arg(item.isBell ? "Evet" : "Hayır").arg(item.announceId));

    auto* player = new LocalPlaylistPlayer(item.path, item.repeat, item.isBell, parent());
    m_activePlayer = player;

    connect(player, &QObject::destroyed, this, [this, player]() {
        if (m_activePlayer == player) {
            m_activePlayer = nullptr;
            QTimer::singleShot(80, this, &CommandHandler::playNextAnnounce);
        }
    });
    player->start();
}

void CommandHandler::audioStopAll(const QString& announceId) {
    Logger::info(QObject::tr("CommandHandler: Anons ve yerel ses oynatımı durduruluyor (id=%1)")
                 .arg(announceId.isEmpty() ? QObject::tr("Tümü") : announceId));

    if (!announceId.isEmpty()) {
        m_cancelledAnnounceIds.insert(announceId);
        if (m_cancelledAnnounceIds.size() > 64) {
            m_cancelledAnnounceIds.clear();
            m_cancelledAnnounceIds.insert(announceId);
        }
    }

    // Kuyruğu temizle (eğer belirli bir announceId verilmişse onu, boşsa tüm anonsları)
    if (announceId.isEmpty()) {
        m_announceQueue.clear();
    } else {
        QQueue<PendingAnnounce> remaining;
        while (!m_announceQueue.isEmpty()) {
            auto item = m_announceQueue.dequeue();
            if (item.announceId != announceId && !item.announceId.isEmpty()) {
                remaining.enqueue(item);
            }
        }
        m_announceQueue = remaining;
    }

    if (m_activePlayer) {
        QPointer<LocalPlaylistPlayer> oldPlayer = m_activePlayer;
        m_activePlayer = nullptr;
        oldPlayer->disconnect(this);
        oldPlayer->abort();
    }

    const bool wasPlaying = m_announcePlaying;
    m_announcePlaying = false;
    m_currentPlayingIsBell = false;
    m_currentAnnounceId.clear();
    setPriorityAudioActive(QStringLiteral("local_announce"), false);

    if (wasPlaying) {
        emit announcePlaybackStateChanged(false);
    }
    sendResult(true);
}


void CommandHandler::audioSetVolume(const okidia::AudioVolumeCommand& cmd) {
    uint32_t vol = cmd.volume();
    if (m_priorityBoostActive) {
        m_boostSavedVolume = qBound(0, static_cast<int>(vol), 100);
    }
    if (vol > 100) vol = 100;
    Logger::info(QObject::tr("CommandHandler: Ses seviyesi → %1%").arg(vol));

#if defined(Q_OS_WIN)
    ScopedWindowsEndpointVolume ep;
    if (ep.endpoint) {
        ep.endpoint->SetMasterVolumeLevelScalar(vol / 100.0f, nullptr);
        if (vol > 0) {
            ep.endpoint->SetMute(FALSE, nullptr);
        }
    }
#else
    QString pctStr = QString::number(vol) + "%";
    if (!runAudioCommand("pactl",
            {"set-sink-volume", "@DEFAULT_SINK@", pctStr})) {
        if (!runAudioCommand("wpctl",
                {"set-volume", "@DEFAULT_AUDIO_SINK@",
                 QString::number(vol / 100.0, 'f', 2)})) {
            runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", pctStr});
        }
    }
#endif
    sendResult(true);
}

// ---------------------------------------------------------------------------
// Mesaj
// ---------------------------------------------------------------------------
void CommandHandler::showPopup(const okidia::ChatMessage& msg) {
    QString content = QString::fromStdString(msg.content());
    QString sender  = QString::fromStdString(msg.from());
    Logger::info(QObject::tr("CommandHandler: Popup → %1").arg(content));
    PopupMessage::show(content, sender);
    sendResult(true);
}
