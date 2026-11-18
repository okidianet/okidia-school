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
#include <QObject>

#if !defined(Q_OS_WIN)
#  include <unistd.h>
#endif

#include "../system/SystemInfo.h"

static QProcessEnvironment audioEnv() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#if !defined(Q_OS_WIN)
    if (::geteuid() != 0) {
        return env;
    }
    if (!env.contains("PULSE_RUNTIME_PATH")) {
        QString username, uid;
        if (SystemInfo::getAudioSession(username, uid) && !uid.isEmpty()) {
            env.insert("PULSE_RUNTIME_PATH", "/run/user/" + uid + "/pulse");
            env.insert("XDG_RUNTIME_DIR", "/run/user/" + uid);
            env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/" + uid + "/bus");
            Logger::info(QObject::tr("CommandHandlerAudio: Ses oturumu tespit edildi -> %1 (UID: %2)").arg(username, uid));
        } else {
            env.insert("PULSE_RUNTIME_PATH", "/run/user/1000/pulse");
            env.insert("XDG_RUNTIME_DIR", "/run/user/1000");
        }
    }
#endif
    return env;
}

static bool runAudioCommand(const QString& prog,
                             const QStringList& args) {
    QProcess p;
    p.setProcessEnvironment(audioEnv());
    p.start(prog, args);
    if (!p.waitForStarted(2000)) return false;
    p.waitForFinished(5000);
    return p.exitCode() == 0;
}

static QString runAudioOutput(const QString& prog,
                               const QStringList& args) {
    QProcess p;
    p.setProcessEnvironment(audioEnv());
    p.start(prog, args);
    p.waitForFinished(5000);
    return p.readAllStandardOutput().trimmed();
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

static IAudioEndpointVolume* getWindowsEndpointVolume() {
    CoInitialize(nullptr);
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return nullptr;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return nullptr;

    IAudioEndpointVolume* endpoint = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                          nullptr, reinterpret_cast<void**>(&endpoint));
    device->Release();
    if (FAILED(hr) || !endpoint) return nullptr;

    return endpoint;
}
#endif

void CommandHandler::audioMute() {
    Logger::info(QObject::tr("CommandHandler: Ses kapatılıyor"));

#if defined(Q_OS_WIN)
    IAudioEndpointVolume* ep = getWindowsEndpointVolume();
    if (ep) {
        float currentVol = 0.5f;
        if (SUCCEEDED(ep->GetMasterVolumeLevelScalar(&currentVol))) {
            m_savedVolume = qBound(0, qRound(currentVol * 100.0f), 100);
            Logger::info(QObject::tr("CommandHandler: Windows ses seviyesi kaydedildi → %1%").arg(m_savedVolume));
        }
        ep->SetMute(TRUE, nullptr);
        ep->Release();
    }
    CoUninitialize();
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

#if defined(Q_OS_WIN)
    IAudioEndpointVolume* ep = getWindowsEndpointVolume();
    if (ep) {
        ep->SetMute(FALSE, nullptr);
        if (m_savedVolume > 0) {
            ep->SetMasterVolumeLevelScalar(m_savedVolume / 100.0f, nullptr);
            Logger::info(QObject::tr("CommandHandler: Windows ses seviyesi geri yüklendi → %1%").arg(m_savedVolume));
        }
        ep->Release();
    }
    CoUninitialize();
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
    IAudioEndpointVolume* ep = getWindowsEndpointVolume();
    if (ep) {
        ep->SetMute(FALSE, nullptr);
        ep->Release();
    }
    CoUninitialize();
#else
    if (!runAudioCommand("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "0"}))
        if (!runAudioCommand("wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "0"}))
            runAudioCommand("amixer", {"-D", "pulse", "sset", "Master", "unmute"});
#endif
    sendResult(true);
}

void CommandHandler::audioPlayLocal(const okidia::AudioPlayLocalCommand& cmd) {
    audioUnmuteOnly();

    const QString anonsPath = QString::fromStdString(cmd.anons_path());
    const int     repeat    = qMax(1, cmd.repeat_count());
    const bool    isBell    = cmd.is_bell();

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
        // Gelen Yönetici Anonsu: Eğer o an bir okul zili çalıyorsa zili anında sustur (preemption)!
        if (m_announcePlaying && m_currentPlayingIsBell) {
            Logger::info(QObject::tr("CommandHandler: Yönetici Anonsu geldi! Çalan okul zili anında susturuluyor (preemption)..."));
            // Bekleyen başka zil varsa kuyruğu temizle
            m_announceQueue.clear();
            if (m_activePlayer) {
                m_activePlayer->abort();
            }
        }
    }

    Logger::info(QObject::tr("CommandHandler: audioPlayLocal (kuyruğa ekleniyor) → %1 ×%2 (Zil: %3)")
                 .arg(anonsPath).arg(repeat).arg(isBell ? "Evet" : "Hayır"));

    m_announceQueue.enqueue({anonsPath, repeat, isBell});
    sendResult(true);

    if (!m_announcePlaying) {
        playNextAnnounce();
    }
}

void CommandHandler::playNextAnnounce() {
    if (m_announceQueue.isEmpty()) {
        m_announcePlaying = false;
        m_currentPlayingIsBell = false;
        m_activePlayer = nullptr;
        return;
    }

    m_announcePlaying = true;
    auto item = m_announceQueue.dequeue();
    m_currentPlayingIsBell = item.isBell;

    Logger::info(QObject::tr("CommandHandler: Kuyruktaki anons başlatılıyor → %1 ×%2 (Zil: %3)")
                 .arg(item.path).arg(item.repeat).arg(item.isBell ? "Evet" : "Hayır"));

    auto* player = new LocalPlaylistPlayer(item.path, item.repeat, item.isBell, parent());
    m_activePlayer = player;

    connect(player, &QObject::destroyed, this, [this]() {
        m_activePlayer = nullptr;
        QTimer::singleShot(100, this, &CommandHandler::playNextAnnounce);
    });
    player->start();
}


void CommandHandler::audioSetVolume(const okidia::AudioVolumeCommand& cmd) {
    uint32_t vol = cmd.volume();
    if (vol > 100) vol = 100;
    Logger::info(QObject::tr("CommandHandler: Ses seviyesi → %1%").arg(vol));

#if defined(Q_OS_WIN)
    IAudioEndpointVolume* ep = getWindowsEndpointVolume();
    if (ep) {
        ep->SetMasterVolumeLevelScalar(vol / 100.0f, nullptr);
        if (vol > 0) {
            ep->SetMute(FALSE, nullptr);
        }
        ep->Release();
    }
    CoUninitialize();
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
