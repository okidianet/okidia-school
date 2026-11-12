// agent/audio/LocalPlaylistPlayer.cpp
#include "LocalPlaylistPlayer.h"
#include "../../common/utils/Logger.h"
#include "../../common/OkidiaConfig.h"

#include <QUrl>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#if !defined(Q_OS_WIN)
#  include <unistd.h>
#endif
#include "../system/SystemInfo.h"

static QProcessEnvironment audioEnv() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#if !defined(Q_OS_WIN)
    // Eğer root değilsek, sistem ortamını olduğu gibi kullan
    if (::geteuid() != 0) {
        return env;
    }

    // Root olarak çalışırken aktif kullanıcı ses oturumu varsa bağla
    if (!env.contains("PULSE_RUNTIME_PATH") && !env.contains("XDG_RUNTIME_DIR")) {
        QString username, uid;
        if (SystemInfo::getAudioSession(username, uid) && !uid.isEmpty()) {
            QString userRunDir = "/run/user/" + uid;
            if (QDir(userRunDir).exists()) {
                env.insert("PULSE_RUNTIME_PATH", userRunDir + "/pulse");
                env.insert("XDG_RUNTIME_DIR", userRunDir);
                env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + userRunDir + "/bus");
                Logger::info(QObject::tr("LocalPlaylistPlayer: Ses oturumu tespit edildi -> %1 (UID: %2)").arg(username, uid));
            }
        }
    }
#endif
    return env;
}

LocalPlaylistPlayer::LocalPlaylistPlayer(const QString& anonsPath,
                                         int repeatCount,
                                         bool isBell,
                                         QObject* parent)
    : QObject(parent)
    , m_dingPath("/usr/share/okidia/sounds/dingdong.wav")
    , m_anonsPath(anonsPath)
    , m_repeatCount(qMax(1, repeatCount))
    , m_isBell(isBell)
{
    // Windows ve farklı dosya yolları uyumluluğu:
    // Broker anons yolunu /tmp/... olarak göndermiş olabilir; Windows'ta veya özel ortamlarda dosya QDir::tempPath() altındadır.
    if (!QFile::exists(m_anonsPath)) {
        QString filename = QFileInfo(m_anonsPath).fileName();
        QString tempResolved = QDir::tempPath() + "/" + filename;
        if (QFile::exists(tempResolved)) {
            m_anonsPath = tempResolved;
            Logger::info(QObject::tr("LocalPlaylistPlayer: Anons geçici dizinde tespit edildi → ") + m_anonsPath);
        }
    }

    // dingdong.wav dosyasının varlığını teyit et; yoksa alternatif yollara ve gömülü kaynağa bak
    if (!QFile::exists(m_dingPath)) {
        QString appDirDing = QCoreApplication::applicationDirPath() + "/sounds/dingdong.wav";
        QString resDirDing = QCoreApplication::applicationDirPath() + "/../resources/sound/dingdong.wav";
        if (QFile::exists(appDirDing)) {
            m_dingPath = appDirDing;
        } else if (QFile::exists(resDirDing)) {
            m_dingPath = resDirDing;
        } else if (QFile::exists(":/sound/dingdong.wav")) {
            // Gömülü Qt kaynağından geçici dizine çıkar
            QString tempDing = QDir::tempPath() + "/okidia_dingdong.wav";
            if (!QFile::exists(tempDing) || QFile(tempDing).size() == 0) {
                QFile::remove(tempDing);
                if (!QFile::copy(":/sound/dingdong.wav", tempDing)) {
                    tempDing = QDir::tempPath() + QString("/okidia_dingdong_%1.wav").arg(QCoreApplication::applicationPid());
                    QFile::remove(tempDing);
                    QFile::copy(":/sound/dingdong.wav", tempDing);
                }
            }
            if (QFile::exists(tempDing)) {
                m_dingPath = tempDing;
            }
        }
    }
    Logger::info(QObject::tr("LocalPlaylistPlayer: Ding-dong dosyası tespit edildi → ") + m_dingPath);
}

LocalPlaylistPlayer::~LocalPlaylistPlayer() {
    if (m_maxDurationTimer) {
        m_maxDurationTimer->stop();
    }
    if (m_fadeOutTimer) {
        m_fadeOutTimer->stop();
    }
    if (m_currentProcess && m_currentProcess->state() != QProcess::NotRunning) {
        m_currentProcess->terminate();
        m_currentProcess->kill();
    }
    if (m_qtPlayer) {
        m_qtPlayer->stop();
    }
}

QString LocalPlaylistPlayer::resolveBellPath(const QString& requestedPath) {
    if (QFile::exists(requestedPath) && QFileInfo(requestedPath).size() > 0) {
        return requestedPath;
    }

    QString fname = QFileInfo(requestedPath).fileName();
    if (!fname.isEmpty()) {
        // 1. İstemcinin kendi yerel varlık dizininde doğrudan isim kontrolü
        QString localAsset = OkidiaConfig::assetsDir() + "/" + fname;
        if (QFile::exists(localAsset) && QFileInfo(localAsset).size() > 0) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: Hedef zil yerel varlık dizininde bulundu → ") + localAsset);
            return localAsset;
        }

        // 2. Windows veya geçici dizinde isim bazlı kontrol
        QString tempResolved = QDir::tempPath() + "/" + fname;
        if (QFile::exists(tempResolved) && QFileInfo(tempResolved).size() > 0) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: Zil geçici dizinde bulundu → ") + tempResolved);
            return tempResolved;
        }
    }

    // 3. Tipe Özel Akıllı Arama (Öğrenci, Öğretmen veya Teneffüs ziline özel dosya eşleştirme)
    QString targetPrefix;
    if (requestedPath.contains("bell_student", Qt::CaseInsensitive) || requestedPath.contains("student", Qt::CaseInsensitive)) {
        targetPrefix = "bell_student";
    } else if (requestedPath.contains("bell_teacher", Qt::CaseInsensitive) || requestedPath.contains("teacher", Qt::CaseInsensitive)) {
        targetPrefix = "bell_teacher";
    } else if (requestedPath.contains("bell_recess", Qt::CaseInsensitive) || requestedPath.contains("recess", Qt::CaseInsensitive)) {
        targetPrefix = "bell_recess";
    }

    if (!targetPrefix.isEmpty()) {
        QDir assets(OkidiaConfig::assetsDir());
        if (assets.exists()) {
            QStringList typeFilters;
            typeFilters << (targetPrefix + ".*") << (targetPrefix + "_*.*");
            QFileInfoList list = assets.entryInfoList(typeFilters, QDir::Files, QDir::Time);
            for (const auto& fi : list) {
                if (fi.size() > 0) {
                    Logger::info(QObject::tr("LocalPlaylistPlayer: Hedef zil tipi (%1) ile eşleşen melodi çalınacak → %2")
                                 .arg(targetPrefix, fi.absoluteFilePath()));
                    return fi.absoluteFilePath();
                }
            }
        }
    }

    // Son çare: Tek seferlik saf dingdong.wav
    Logger::warning(QObject::tr("LocalPlaylistPlayer: Hiçbir zil dosyası bulunamadı, varsayılan melodi çalınacak: %1")
                    .arg(m_dingPath));
    return m_dingPath;
}

void LocalPlaylistPlayer::start() {
    if (!m_isBell) {
        showSplash();
        m_state = State::Intro;
        Logger::info(QObject::tr("LocalPlaylistPlayer: Yönetici anonsu başlatıldı — anons=%1 tekrar=%2")
                     .arg(m_anonsPath).arg(m_repeatCount));
        playFile(m_dingPath);
    } else {
        // Okul Zili: ASLA splash açılmaz, ASLA intro/outro dingdong işletilmez!
        m_state = State::Anons;
        m_anonsPlayed = 1;

        // Akıllı fallback ile zil dosyasını garantile
        m_anonsPath = resolveBellPath(m_anonsPath);
        Logger::info(QObject::tr("LocalPlaylistPlayer: Okul zili başlatıldı — hedef=%1 (Tavan: 60 sn)")
                     .arg(m_anonsPath));

        // 1. Maksimum 60 saniye tavan süresi sayacı
        m_maxDurationTimer = new QTimer(this);
        m_maxDurationTimer->setSingleShot(true);
        connect(m_maxDurationTimer, &QTimer::timeout, this, [this]() {
            Logger::info(QObject::tr("LocalPlaylistPlayer: 60 saniye zil tavan süresi doldu, ses sonlandırılıyor."));
            stopPlaybackAndFinish();
        });
        m_maxDurationTimer->start(60000); // 60 saniye

        // 2. 55. saniyede 5 saniyelik Fade-Out (yumuşak sönümleme)
        QTimer::singleShot(55000, this, [this]() {
            if (m_state != State::Done) {
                startFadeOut();
            }
        });

        playFile(m_anonsPath);
    }
}

void LocalPlaylistPlayer::startFadeOut() {
    if (m_state == State::Done || m_fadeOutTimer) return;

    Logger::info(QObject::tr("LocalPlaylistPlayer: 55. saniye doldu, yumuşak sönümleme (Fade-Out) devrede..."));
    m_fadeOutTimer = new QTimer(this);
    m_fadeOutTimer->setInterval(250); // 5 saniyede 20 adım

    connect(m_fadeOutTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Done) {
            m_fadeOutTimer->stop();
            return;
        }

        m_currentVolume -= (1.0f / 20.0f);
        if (m_currentVolume <= 0.05f) {
            m_currentVolume = 0.0f;
            m_fadeOutTimer->stop();
            if (m_qtAudio) {
                m_qtAudio->setVolume(0.0f);
            }
            stopPlaybackAndFinish();
        } else {
            if (m_qtAudio) {
                m_qtAudio->setVolume(m_currentVolume);
            }
        }
    });

    m_fadeOutTimer->start();
}

void LocalPlaylistPlayer::stopPlaybackAndFinish() {
    if (m_maxDurationTimer && m_maxDurationTimer->isActive()) {
        m_maxDurationTimer->stop();
    }
    if (m_fadeOutTimer && m_fadeOutTimer->isActive()) {
        m_fadeOutTimer->stop();
    }

    if (m_currentProcess && m_currentProcess->state() != QProcess::NotRunning) {
        m_currentProcess->terminate();
        QTimer::singleShot(400, m_currentProcess, &QProcess::kill);
    }

    if (m_qtPlayer) {
        m_qtPlayer->stop();
    }

    advance();
}

void LocalPlaylistPlayer::abort() {
    Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma zorla durduruluyor (abort)..."));
    m_state = State::Done;
    if (m_maxDurationTimer && m_maxDurationTimer->isActive()) {
        m_maxDurationTimer->stop();
    }
    if (m_fadeOutTimer && m_fadeOutTimer->isActive()) {
        m_fadeOutTimer->stop();
    }

    if (m_currentProcess && m_currentProcess->state() != QProcess::NotRunning) {
        m_currentProcess->terminate();
        QTimer::singleShot(200, m_currentProcess, &QProcess::kill);
    }

    if (m_qtPlayer) {
        m_qtPlayer->stop();
    }

    if (m_anonsPath.startsWith("/tmp/") || m_anonsPath.startsWith(QDir::tempPath())) {
        QFile::remove(m_anonsPath);
    }
    closeSplash();
    deleteLater();
}

void LocalPlaylistPlayer::playFile(const QString& path) {
    if (!QFile::exists(path) || QFileInfo(path).size() == 0) {
        if (m_isBell && path != m_dingPath && QFile::exists(m_dingPath)) {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Zil dosyası bozuk veya bulunamadı (%1), yedek ses çalınıyor: %2")
                         .arg(path, m_dingPath));
            playFile(m_dingPath);
            return;
        }
        Logger::warning(QObject::tr("LocalPlaylistPlayer: Dosya yok, atlanıyor → ") + path);
        QTimer::singleShot(100, this, &LocalPlaylistPlayer::advance);
        return;
    }

    m_currentPlayingPath = path;
    const QString ext = QFileInfo(path).suffix().toLower();
    const bool isCompressed = (ext == "mp3" || ext == "ogg" || ext == "aac" || ext == "m4a" || ext == "flac");

    QList<PlayAttempt> attempts;

#if defined(Q_OS_WIN)
    // Windows 10/11: CLI Oynatıcılar ve Powershell COM Medya Motoru
    if (!QStandardPaths::findExecutable("mpv").isEmpty()) {
        attempts.append({"mpv", {"--no-video", "--really-quiet", path}});
    }
    if (!QStandardPaths::findExecutable("ffplay").isEmpty()) {
        attempts.append({"ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", path}});
    }

    // Windows Native PowerShell ile kilitli/login ekranında sorunsuz medya oynatma
    const QString winPath = QDir::toNativeSeparators(path);
    if (!isCompressed) {
        // WAV formatı için hafif SoundPlayer
        attempts.append({"powershell.exe", {
            "-ExecutionPolicy", "Bypass", "-NoProfile", "-NonInteractive",
            "-Command", QString("(New-Object Media.SoundPlayer '%1').PlaySync()").arg(winPath)
        }});
    } else {
        // MP3 / AAC vb. sıkıştırılmış sesler için yerel Windows Media Player motoru
        attempts.append({"powershell.exe", {
            "-ExecutionPolicy", "Bypass", "-NoProfile", "-NonInteractive",
            "-Command", QString("$w=New-Object -ComObject WMPlayer.OCX;$w.URL='%1';$w.controls.play();while($w.playState -ne 1 -and $w.playState -ne 8 -and $w.playState -ne 10){Start-Sleep -m 200}").arg(winPath)
        }});
    }
#else
    // Linux: Oturum açıkken veya kilit/LightDM ekranındayken ses zinciri

    // 1. PipeWire pw-play (hem WAV hem MP3 destekler)
    if (!QStandardPaths::findExecutable("pw-play").isEmpty()) {
        attempts.append({"pw-play", {path}});
    }

    // 2. mpv (kilit/login ekranında doğrudan ALSA/Pulse oynatır)
    if (!QStandardPaths::findExecutable("mpv").isEmpty()) {
        attempts.append({"mpv", {"--no-video", "--really-quiet", path}});
    }

    // 3. ffplay (ffmpeg yüklüyse kilit ekranında garantili oynatır)
    if (!QStandardPaths::findExecutable("ffplay").isEmpty()) {
        attempts.append({"ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", path}});
    }

    // 4. mpg123 (MP3 için en hafif ve doğrudan ALSA/Pulse çalan araç)
    if (ext == "mp3" && !QStandardPaths::findExecutable("mpg123").isEmpty()) {
        attempts.append({"mpg123", {"-q", path}});
    }

    // 5. paplay (Sadece WAV/raw PCM için)
    if (!isCompressed && !QStandardPaths::findExecutable("paplay").isEmpty()) {
        attempts.append({"paplay", {path}});
    }

    // 6. aplay (ALSA doğrudan donanım çıkışı - kilit ekranında ve WAV için hayat kurtarıcı)
    if (!isCompressed && !QStandardPaths::findExecutable("aplay").isEmpty()) {
        attempts.append({"aplay", {path}});
        attempts.append({"aplay", {"-D", "default", path}});
    }
#endif

    if (attempts.isEmpty()) {
        Logger::info(QObject::tr("LocalPlaylistPlayer: Harici CLI oynatıcı bulunamadı, QMediaPlayer ile çalınıyor."));
        playWithQt(path);
        return;
    }

    tryPlayAttempt(attempts, 0);
}

void LocalPlaylistPlayer::tryPlayAttempt(const QList<PlayAttempt>& attempts, int index) {
    if (index >= attempts.size()) {
        Logger::warning(QObject::tr("LocalPlaylistPlayer: Harici oynatma komutları tükendi, QMediaPlayer'a geçiliyor..."));
        playWithQt(m_currentPlayingPath);
        return;
    }

    auto attempt = attempts[index];
    Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma deneniyor (%1) → %2 %3")
                 .arg(index + 1)
                 .arg(attempt.program)
                 .arg(attempt.args.join(" ")));

    QProcess* p = new QProcess(this);
    p->setProcessEnvironment(audioEnv());
    m_currentProcess = p;

    auto handleFailure = [this, p, attempts, index]() {
        if (m_currentProcess == p) m_currentProcess = nullptr;
        p->disconnect();
        p->deleteLater();
        tryPlayAttempt(attempts, index + 1);
    };

    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p, handleFailure](int exitCode) {
        if (m_currentProcess == p) m_currentProcess = nullptr;
        if (exitCode == 0) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma başarılı."));
            p->deleteLater();
            advance();
        } else {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Süreç sıfır dışı çıkış koduyla bitti (%1).").arg(exitCode));
            handleFailure();
        }
    });

    connect(p, &QProcess::errorOccurred, this, [handleFailure](QProcess::ProcessError) {
        Logger::warning(QObject::tr("LocalPlaylistPlayer: Süreç çalıştırma hatası oluştu."));
        handleFailure();
    });

    p->start(attempt.program, attempt.args);
}

void LocalPlaylistPlayer::playWithQt(const QString& path) {
    if (!m_qtPlayer) {
        m_qtPlayer = new QMediaPlayer(this);
        m_qtAudio = new QAudioOutput(this);
        m_currentVolume = 1.0f;
        m_qtAudio->setVolume(1.0f);
        m_qtAudio->setMuted(false);
        m_qtPlayer->setAudioOutput(m_qtAudio);

        connect(m_qtPlayer, &QMediaPlayer::mediaStatusChanged,
                this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::EndOfMedia) {
                Logger::info(QObject::tr("LocalPlaylistPlayer: QMediaPlayer oynatımı tamamlandı."));
                advance();
            }
        });

        connect(m_qtPlayer, &QMediaPlayer::errorOccurred,
                this, [this](QMediaPlayer::Error, const QString& errorString) {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: QMediaPlayer hatası: ") + errorString);
            advance();
        });
    } else if (m_qtAudio) {
        m_currentVolume = 1.0f;
        m_qtAudio->setVolume(1.0f);
        m_qtAudio->setMuted(false);
    }

    Logger::info(QObject::tr("LocalPlaylistPlayer: QMediaPlayer ile oynatılıyor → ") + path);
    m_qtPlayer->setSource(QUrl::fromLocalFile(path));
    m_qtPlayer->play();
}

void LocalPlaylistPlayer::advance() {
    if (m_state == State::Done) return;

    switch (m_state) {
    case State::Intro: {
        m_state       = State::Anons;
        m_anonsPlayed = 0;
        // Açılış ding-dong sesinden sonra anons başlamadan 400ms doğal duraklama
        QTimer::singleShot(400, this, [this]() {
            if (m_state != State::Anons) return;
            m_anonsPlayed = 1;
            Logger::info(QObject::tr("LocalPlaylistPlayer: Anons %1/%2")
                         .arg(m_anonsPlayed).arg(m_repeatCount));
            playFile(m_anonsPath);
        });
        break;
    }

    case State::Anons: {
        if (m_isBell) {
            // Okul Zili: Tek bir saf zil çalınır; ASLA outro dingdong çalınmaz ve zil dosyaları ASLA silinmez!
            m_state = State::Done;
            Logger::info(QObject::tr("LocalPlaylistPlayer: Okul zili sekansı tamamlandı."));
            if (m_maxDurationTimer) m_maxDurationTimer->stop();
            if (m_fadeOutTimer) m_fadeOutTimer->stop();

            closeSplash();
            deleteLater();
            return;
        }

        // Yönetici Anonsu: Tekrarlar ve kapanış dingdong işletilir
        if (m_anonsPlayed < m_repeatCount) {
            ++m_anonsPlayed;
            // Tekrarlanan anonslar arasında 600ms doğal duraklama
            QTimer::singleShot(600, this, [this]() {
                if (m_state != State::Anons) return;
                Logger::info(QObject::tr("LocalPlaylistPlayer: Anons %1/%2")
                             .arg(m_anonsPlayed).arg(m_repeatCount));
                playFile(m_anonsPath);
            });
        } else {
            m_state = State::Outro;
            // Kapanış ding-dong sesinden önce 400ms doğal duraklama
            QTimer::singleShot(400, this, [this]() {
                if (m_state != State::Outro) return;
                Logger::info(QObject::tr("LocalPlaylistPlayer: Kapanış ding-dong çalınıyor..."));
                playFile(m_dingPath);
            });
        }
        break;
    }

    case State::Outro: {
        m_state = State::Done;
        Logger::info(QObject::tr("LocalPlaylistPlayer: Anons sekansı tamamlandı."));
        // Geçici anons dosyasını sil (Linux /tmp veya Windows Temp)
        if (m_anonsPath.startsWith("/tmp/") || m_anonsPath.startsWith(QDir::tempPath())) {
            if (QFile::remove(m_anonsPath))
                Logger::info(QObject::tr("LocalPlaylistPlayer: Silindi → ") + m_anonsPath);
        }
        closeSplash();
        deleteLater();
        break;
    }

    case State::Done:
        break;
    }
}

void LocalPlaylistPlayer::showSplash() {
    m_splash = new QWidget(nullptr,
        Qt::Window          |
        Qt::FramelessWindowHint |
        Qt::WindowStaysOnTopHint |
        Qt::Tool);

    m_splash->setFixedSize(340, 100);
    m_splash->setAttribute(Qt::WA_ShowWithoutActivating);
    m_splash->setAttribute(Qt::WA_TranslucentBackground);
    m_splash->setStyleSheet(R"(
        QWidget#splash {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #2a2d4a, stop:1 #1a1b2e);
            border: 1px solid #4a90d9;
            border-radius: 12px;
        }
    )");

    auto* inner = new QWidget(m_splash);
    inner->setObjectName("splash");
    inner->setGeometry(0, 0, 340, 100);

    auto* lay = new QVBoxLayout(inner);
    lay->setContentsMargins(20, 14, 20, 14);
    lay->setSpacing(6);

    auto* row = new QHBoxLayout();
    row->setSpacing(10);

    auto* ico = new QLabel("", inner);
    ico->setStyleSheet("font-size:28px; background:transparent;");
    ico->setFixedWidth(36);

    auto* textCol = new QVBoxLayout();
    auto* title = new QLabel(tr("Yönetici Anonsu"), inner);
    title->setStyleSheet(
        "color:#e8eaf6; font-size:14px; font-weight:bold;"
        "background:transparent;");
    auto* sub = new QLabel(tr("Lütfen bekleyiniz..."), inner);
    sub->setStyleSheet(
        "color:#7986cb; font-size:11px; background:transparent;");

    textCol->addWidget(title);
    textCol->addWidget(sub);

    row->addWidget(ico);
    row->addLayout(textCol);
    lay->addLayout(row);

    if (QScreen* s = QApplication::primaryScreen())
        m_splash->move(s->geometry().center() - m_splash->rect().center());

    m_splash->show();
    m_splash->raise();
    m_splash->activateWindow();
}

void LocalPlaylistPlayer::closeSplash() {
    if (m_splash) {
        m_splash->hide();
        m_splash->deleteLater();
        m_splash = nullptr;
    }
}
