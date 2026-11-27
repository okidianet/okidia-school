// agent/audio/LocalPlaylistPlayer.cpp
#include "LocalPlaylistPlayer.h"
#include "../../common/utils/Logger.h"
#include "../../common/OkidiaConfig.h"
#include "../../common/ui/ThemeManager.h"

#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QBuffer>
#include <QtEndian>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#include <cmath>
#include <vector>
#if !defined(Q_OS_WIN)
#  include <unistd.h>
#  include <pwd.h>
#  include "../capture/WaylandPortalCapture.h"
#endif
#include "../system/SystemInfo.h"

static bool resolveLinuxAudioUser(QString& outUser, QString& outUid, QString& outGid, QString& outRunDir) {
#if !defined(Q_OS_WIN)
    QString username, uid;
    if (SystemInfo::getAudioSession(username, uid) && !uid.isEmpty() && uid != QLatin1String("0")) {
        const QString runDir = QStringLiteral("/run/user/") + uid;
        if (QDir(runDir).exists()) {
            outUser = username;
            outUid = uid;
            outRunDir = runDir;
            const QFileInfo rtInfo(runDir);
            outGid = QString::number(rtInfo.groupId());
            if (struct passwd* pw = ::getpwuid(static_cast<uid_t>(uid.toUInt()))) {
                outGid = QString::number(pw->pw_gid);
                if (pw->pw_name) {
                    outUser = QString::fromUtf8(pw->pw_name);
                }
            }
            return true;
        }
    }
#else
    Q_UNUSED(outUser);
    Q_UNUSED(outUid);
    Q_UNUSED(outGid);
    Q_UNUSED(outRunDir);
#endif
    return false;
}

static bool isRootWithoutAudioEnv() {
#if !defined(Q_OS_WIN)
    if (::geteuid() == 0) {
        return qEnvironmentVariable("XDG_RUNTIME_DIR").isEmpty() &&
               qEnvironmentVariable("PULSE_RUNTIME_PATH").isEmpty();
    }
#endif
    return false;
}

static QString detectAudioMagicFormat(const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray hdr = f.read(16);
        f.close();
        if (hdr.size() >= 12 && hdr.startsWith("RIFF") && hdr.mid(8, 4) == "WAVE") {
            return QStringLiteral("wav");
        }
        if (hdr.size() >= 4 && hdr.startsWith("OggS")) {
            return QStringLiteral("ogg");
        }
        if (hdr.size() >= 4 && hdr.startsWith("fLaC")) {
            return QStringLiteral("flac");
        }
        if (hdr.size() >= 8 && hdr.mid(4, 4) == "ftyp") {
            return QStringLiteral("m4a");
        }
        if (hdr.size() >= 3 && hdr.startsWith("ID3")) {
            return QStringLiteral("mp3");
        }
        if (hdr.size() >= 2 &&
            static_cast<unsigned char>(hdr[0]) == 0xFF &&
            (static_cast<unsigned char>(hdr[1]) & 0xE0) == 0xE0) {
            return QStringLiteral("mp3");
        }
    }
    return QFileInfo(path).suffix().toLower();
}

static QProcessEnvironment audioEnv() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#if !defined(Q_OS_WIN)
    if (::geteuid() != 0) {
        if (!env.contains("PULSE_RUNTIME_PATH")) {
            const QString xdg = env.value("XDG_RUNTIME_DIR");
            if (!xdg.isEmpty() && QDir(xdg + "/pulse").exists()) {
                env.insert("PULSE_RUNTIME_PATH", xdg + "/pulse");
            }
        }
        return env;
    }

    if (!env.contains("PULSE_RUNTIME_PATH") && !env.contains("XDG_RUNTIME_DIR")) {
        QString username, uid, gid, userRunDir;
        if (resolveLinuxAudioUser(username, uid, gid, userRunDir)) {
            env.insert("PULSE_RUNTIME_PATH", userRunDir + "/pulse");
            env.insert("XDG_RUNTIME_DIR", userRunDir);
            env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + userRunDir + "/bus");
            Logger::info(QObject::tr("LocalPlaylistPlayer: Ses oturumu tespit edildi -> %1 (UID: %2)").arg(username, uid));
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
    if (!QFile::exists(m_anonsPath)) {
        QString filename = QFileInfo(m_anonsPath).fileName();
        QString tempResolved = QDir::tempPath() + "/" + filename;
        if (QFile::exists(tempResolved)) {
            m_anonsPath = tempResolved;
            Logger::info(QObject::tr("LocalPlaylistPlayer: Anons geçici dizinde tespit edildi → ") + m_anonsPath);
        }
    }

    if (!QFile::exists(m_dingPath)) {
        QString appDirDing = QCoreApplication::applicationDirPath() + "/sounds/dingdong.wav";
        QString resDirDing = QCoreApplication::applicationDirPath() + "/../resources/sound/dingdong.wav";
        if (QFile::exists(appDirDing)) {
            m_dingPath = appDirDing;
        } else if (QFile::exists(resDirDing)) {
            m_dingPath = resDirDing;
        } else if (QFile::exists(":/sound/dingdong.wav")) {
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
    if (m_maxDurationTimer) m_maxDurationTimer->stop();
    if (m_splashWatchdog) m_splashWatchdog->stop();
    if (m_fadeOutTimer) m_fadeOutTimer->stop();
    stopQtStartWatchdog();
    if (m_wavSink) {
        m_wavSink->disconnect(this);
        m_wavSink->stop();
        delete m_wavSink;
        m_wavSink = nullptr;
    }
    if (m_wavBuffer) {
        m_wavBuffer->close();
        delete m_wavBuffer;
        m_wavBuffer = nullptr;
    }
    terminateActiveProcess();
    if (m_qtPlayer) {
        m_qtPlayer->disconnect(this);
        m_qtPlayer->stop();
    }
    closeSplash();
}

void LocalPlaylistPlayer::terminateActiveProcess() {
    if (!m_currentProcess) return;
    QProcess* p = m_currentProcess;
    m_currentProcess = nullptr;
    p->disconnect(this);

    if (p->state() != QProcess::NotRunning) {
#if defined(Q_OS_WIN)
        const qint64 pid = p->processId();
        if (pid > 0) {
            QProcess::execute("taskkill.exe", {"/F", "/T", "/PID", QString::number(pid)});
        }
        p->kill();
#else
        p->terminate();
        if (!p->waitForFinished(150)) {
            p->kill();
        }
#endif
    }
    p->deleteLater();
}

QString LocalPlaylistPlayer::resolveBellPath(const QString& requestedPath) {
    if (QFile::exists(requestedPath) && QFileInfo(requestedPath).size() > 0) {
        return requestedPath;
    }

    QString fname = QFileInfo(requestedPath).fileName();
    if (!fname.isEmpty()) {
        QString localAsset = OkidiaConfig::assetsDir() + "/" + fname;
        if (QFile::exists(localAsset) && QFileInfo(localAsset).size() > 0) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: Hedef zil yerel varlık dizininde bulundu → ") + localAsset);
            return localAsset;
        }

        QString tempResolved = QDir::tempPath() + "/" + fname;
        if (QFile::exists(tempResolved) && QFileInfo(tempResolved).size() > 0) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: Zil geçici dizinde bulundu → ") + tempResolved);
            return tempResolved;
        }
    }

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

        // Splash penceresi için bağımsız emniyet watchdog'u (dosya boyutuna göre tahmini süre + marj, max 5 dk)
        const qint64 fsz = QFileInfo(m_anonsPath).size();
        const int estSingleMs = qBound(8000, static_cast<int>((fsz / 48) + 8000), 180000);
        const int maxWaitMs = qMin(300000, estSingleMs * qMax(1, m_repeatCount) + 15000);
        m_splashWatchdog = new QTimer(this);
        m_splashWatchdog->setSingleShot(true);
        connect(m_splashWatchdog, &QTimer::timeout, this, [this]() {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Anons emniyet zaman aşımı doldu, oynatma ve popup kapatılıyor."));
            abort();
        });
        m_splashWatchdog->start(maxWaitMs);

        playFile(m_dingPath);
    } else {
        m_state = State::Anons;
        m_anonsPlayed = 1;
        m_anonsPath = resolveBellPath(m_anonsPath);
        Logger::info(QObject::tr("LocalPlaylistPlayer: Okul zili başlatıldı — hedef=%1 (Tavan: 60 sn)")
                     .arg(m_anonsPath));

        m_maxDurationTimer = new QTimer(this);
        m_maxDurationTimer->setSingleShot(true);
        connect(m_maxDurationTimer, &QTimer::timeout, this, [this]() {
            Logger::info(QObject::tr("LocalPlaylistPlayer: 60 saniye zil tavan süresi doldu, ses sonlandırılıyor."));
            stopPlaybackAndFinish();
        });
        m_maxDurationTimer->start(60000);

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
    m_fadeOutTimer->setInterval(250);

    connect(m_fadeOutTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Done) {
            m_fadeOutTimer->stop();
            return;
        }

        m_currentVolume -= (1.0f / 20.0f);
        if (m_currentVolume <= 0.05f) {
            m_currentVolume = 0.0f;
            m_fadeOutTimer->stop();
            if (m_wavSink) m_wavSink->setVolume(0.0f);
            if (m_qtAudio) m_qtAudio->setVolume(0.0f);
            stopPlaybackAndFinish();
        } else {
            if (m_wavSink) m_wavSink->setVolume(m_currentVolume);
            if (m_qtAudio) m_qtAudio->setVolume(m_currentVolume);
        }
    });

    m_fadeOutTimer->start();
}

void LocalPlaylistPlayer::stopQtStartWatchdog() {
    if (m_qtStartWatchdog) {
        m_qtStartWatchdog->stop();
    }
}

void LocalPlaylistPlayer::stopPlaybackAndFinish() {
    if (m_maxDurationTimer && m_maxDurationTimer->isActive()) {
        m_maxDurationTimer->stop();
    }
    if (m_fadeOutTimer && m_fadeOutTimer->isActive()) {
        m_fadeOutTimer->stop();
    }
    stopQtStartWatchdog();

    if (m_wavSink) {
        m_wavSink->disconnect(this);
        m_wavSink->stop();
        delete m_wavSink;
        m_wavSink = nullptr;
    }
    if (m_wavBuffer) {
        m_wavBuffer->close();
        delete m_wavBuffer;
        m_wavBuffer = nullptr;
    }

    terminateActiveProcess();

    if (m_qtPlayer) {
        m_qtPlayer->disconnect(this);
        m_qtPlayer->stop();
    }

    advance();
}

void LocalPlaylistPlayer::abort() {
    if (m_state == State::Done) return;
    Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma zorla durduruluyor (abort)..."));
    m_state = State::Done;

    if (m_maxDurationTimer && m_maxDurationTimer->isActive()) m_maxDurationTimer->stop();
    if (m_splashWatchdog && m_splashWatchdog->isActive()) m_splashWatchdog->stop();
    if (m_fadeOutTimer && m_fadeOutTimer->isActive()) m_fadeOutTimer->stop();
    stopQtStartWatchdog();

    if (m_wavSink) {
        m_wavSink->disconnect(this);
        m_wavSink->stop();
        delete m_wavSink;
        m_wavSink = nullptr;
    }
    if (m_wavBuffer) {
        m_wavBuffer->close();
        delete m_wavBuffer;
        m_wavBuffer = nullptr;
    }

    terminateActiveProcess();

    if (m_qtPlayer) {
        m_qtPlayer->disconnect(this);
        m_qtPlayer->stop();
    }

    if (!m_isBell && (m_anonsPath.startsWith("/tmp/") || m_anonsPath.startsWith(QDir::tempPath()))) {
        QFile::remove(m_anonsPath);
    }
    closeSplash();
    deleteLater();
}

// ---------------------------------------------------------------------------
// Birleşik Süreç İçi WAV Oynatıcı (Saf Qt6 QAudioSink + Format Dönüştürücü)
// Harici süreç (pw-play, mpv, PowerShell WMPlayer) başlatmadan, tüm platformlarda
// sıfır kesiklik ve anında durdurulabilirlik sağlar.
// ---------------------------------------------------------------------------
bool LocalPlaylistPlayer::tryPlayWavWithAudioSink(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;

    const QByteArray wavBytes = f.readAll();
    f.close();

    if (wavBytes.size() <= 44 || !wavBytes.startsWith("RIFF") || wavBytes.mid(8, 4) != "WAVE") {
        return false;
    }

    int idx = 12;
    quint16 audioFmt = 0;
    quint16 wavCh = 1;
    quint32 wavSr = 24000;
    quint16 wavBits = 16;
    int dataOff = 0;
    int dataLen = 0;

    while (idx + 8 <= wavBytes.size()) {
        const QByteArray cid = wavBytes.mid(idx, 4);
        const quint32 csz = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(wavBytes.constData() + idx + 4));
        if (cid == "fmt " && csz >= 16) {
            audioFmt = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(wavBytes.constData() + idx + 8));
            wavCh    = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(wavBytes.constData() + idx + 10));
            wavSr    = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(wavBytes.constData() + idx + 12));
            wavBits  = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(wavBytes.constData() + idx + 22));
        } else if (cid == "data") {
            dataOff = idx + 8;
            dataLen = qMin<int>(csz, wavBytes.size() - dataOff);
            break;
        }
        idx += 8 + csz;
    }

    if (audioFmt != 1 || wavBits != 16 || dataOff <= 0 || dataLen <= 0 || wavCh == 0 || wavSr < 8000 || wavSr > 96000) {
        return false;
    }

    const QAudioDevice outDev = QMediaDevices::defaultAudioOutput();
    if (outDev.isNull()) {
        return false;
    }

    if (m_wavSink) {
        m_wavSink->disconnect(this);
        m_wavSink->stop();
        delete m_wavSink;
        m_wavSink = nullptr;
    }
    if (m_wavBuffer) {
        m_wavBuffer->close();
        delete m_wavBuffer;
        m_wavBuffer = nullptr;
    }

    QAudioFormat outFmt;
    outFmt.setSampleRate(static_cast<int>(wavSr));
    outFmt.setChannelCount(static_cast<int>(wavCh));
    outFmt.setSampleFormat(QAudioFormat::Int16);

    if (!outDev.isFormatSupported(outFmt)) {
        outFmt = outDev.preferredFormat();
        if (outFmt.sampleRate() <= 0) outFmt.setSampleRate(48000);
        if (outFmt.channelCount() <= 0) outFmt.setChannelCount(2);
    }

    QByteArray outPcm;
    if (outFmt.sampleRate() == static_cast<int>(wavSr) &&
        outFmt.channelCount() == static_cast<int>(wavCh) &&
        outFmt.sampleFormat() == QAudioFormat::Int16) {
        outPcm = wavBytes.mid(dataOff, dataLen);
    } else {
        const int inFrames = dataLen / (wavCh * 2);
        if (inFrames <= 0) return false;

        std::vector<float> left(inFrames, 0.0f);
        std::vector<float> right(inFrames, 0.0f);
        const uchar* srcPtr = reinterpret_cast<const uchar*>(wavBytes.constData() + dataOff);
        for (int i = 0; i < inFrames; ++i) {
            qint16 sL = qFromLittleEndian<qint16>(srcPtr + (i * wavCh) * 2);
            qint16 sR = (wavCh >= 2) ? qFromLittleEndian<qint16>(srcPtr + (i * wavCh + 1) * 2) : sL;
            left[i]  = static_cast<float>(sL) / 32768.0f;
            right[i] = static_cast<float>(sR) / 32768.0f;
        }

        const int outSr = qMax(8000, outFmt.sampleRate());
        const int outCh = qMax(1, outFmt.channelCount());
        const double ratio = static_cast<double>(wavSr) / static_cast<double>(outSr);
        const int outFrames = qMax(1, static_cast<int>(std::floor(inFrames / ratio)));

        int outBps = 2;
        if (outFmt.sampleFormat() == QAudioFormat::Float || outFmt.sampleFormat() == QAudioFormat::Int32) outBps = 4;
        else if (outFmt.sampleFormat() == QAudioFormat::UInt8) outBps = 1;

        outPcm.resize(outFrames * outCh * outBps);
        uchar* dstPtr = reinterpret_cast<uchar*>(outPcm.data());

        for (int i = 0; i < outFrames; ++i) {
            const double srcPos = i * ratio;
            const int i0 = qMin(static_cast<int>(srcPos), inFrames - 1);
            const int i1 = qMin(i0 + 1, inFrames - 1);
            const float frac = static_cast<float>(srcPos - i0);
            const float vL = qBound(-1.0f, left[i0] * (1.0f - frac) + left[i1] * frac, 1.0f);
            const float vR = qBound(-1.0f, right[i0] * (1.0f - frac) + right[i1] * frac, 1.0f);

            for (int c = 0; c < outCh; ++c) {
                const float val = (outCh == 1) ? ((vL + vR) * 0.5f) : ((c == 0) ? vL : vR);
                uchar* cell = dstPtr + (i * outCh + c) * outBps;
                if (outFmt.sampleFormat() == QAudioFormat::Int16) {
                    qToLittleEndian<qint16>(static_cast<qint16>(std::lrint(val * 32767.0f)), cell);
                } else if (outFmt.sampleFormat() == QAudioFormat::Int32) {
                    qToLittleEndian<qint32>(static_cast<qint32>(std::llround(static_cast<double>(val) * 2147483647.0)), cell);
                } else if (outFmt.sampleFormat() == QAudioFormat::Float) {
                    qToLittleEndian<float>(val, cell);
                } else if (outFmt.sampleFormat() == QAudioFormat::UInt8) {
                    *cell = static_cast<uchar>(qBound(0, static_cast<int>(std::lrint((val + 1.0f) * 127.5f)), 255));
                }
            }
        }
    }

    m_wavBuffer = new QBuffer(this);
    m_wavBuffer->setData(outPcm);
    m_wavBuffer->open(QIODevice::ReadOnly);

    m_wavSink = new QAudioSink(outDev, outFmt, this);
    m_currentVolume = 1.0f;
    m_wavSink->setVolume(1.0f);
    // 250 ms donanım tamponu: Wayland compositor veya arka plan yükü altında xrun/kesikliği önler
    const int bufSz = outFmt.bytesForDuration(250000);
    if (bufSz > 0) {
        m_wavSink->setBufferSize(bufSz);
    }

    connect(m_wavSink, &QAudioSink::stateChanged, this, [this](QAudio::State st) {
        if (m_state == State::Done || !m_wavSink) return;
        if (st == QAudio::IdleState || (st == QAudio::StoppedState && m_wavSink->error() != QAudio::NoError)) {
            QAudioSink* finishedSink = m_wavSink;
            QBuffer* finishedBuf = m_wavBuffer;
            m_wavSink = nullptr;
            m_wavBuffer = nullptr;
            finishedSink->disconnect(this);
            finishedSink->stop();
            finishedSink->deleteLater();
            if (finishedBuf) {
                finishedBuf->close();
                finishedBuf->deleteLater();
            }
            Logger::info(QObject::tr("LocalPlaylistPlayer: QAudioSink WAV oynatımı tamamlandı."));
            advance();
        }
    });

    Logger::info(QObject::tr("LocalPlaylistPlayer: Saf QAudioSink ile WAV oynatılıyor → %1 (%2Hz %3ch)")
                 .arg(path).arg(outFmt.sampleRate()).arg(outFmt.channelCount()));
    m_wavSink->start(m_wavBuffer);
    if (m_wavSink->error() != QAudio::NoError) {
        Logger::warning(QObject::tr("LocalPlaylistPlayer: QAudioSink başlatma hatası (%1), yedek oynatıcıya geçiliyor...")
                        .arg(static_cast<int>(m_wavSink->error())));
        m_wavSink->disconnect(this);
        delete m_wavSink;
        m_wavSink = nullptr;
        delete m_wavBuffer;
        m_wavBuffer = nullptr;
        return false;
    }

    return true;
}

void LocalPlaylistPlayer::playFile(const QString& path) {
    if (m_state == State::Done) return;

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

    m_currentPlayingPath  = path;
    m_qtFallbackAllowed   = false;
    m_qtAlreadyTried      = false;
    m_qtPlaybackConfirmed = false;
    stopQtStartWatchdog();

    const QString fmt = detectAudioMagicFormat(path);
    const qint64 fSize = QFileInfo(path).size();
    const bool rootNoAudioEnv = isRootWithoutAudioEnv();
    Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma hazırlanıyor → %1 (Boyut: %2 KB, Tespit edilen format: %3, Root/DM: %4)")
                 .arg(path).arg(fSize / 1024).arg(fmt, rootNoAudioEnv ? QObject::tr("Evet") : QObject::tr("Hayır")));

    // X11 DM tray (root, XDG_RUNTIME_DIR yok) ortamında süreç içi Qt ses motorları
    // PulseAudio/PipeWire'a bağlanamaz; doğrudan aktif ses oturumu bağlamındaki harici oynatıcıya geç.
    if (rootNoAudioEnv) {
        playWithExternalFallback(path);
        return;
    }

    // 1. ÖNCE SAF Süreç İçi QAudioSink WAV Motorunu dene (Anonslar, dingdong.wav ve gerçek WAV ziller)
    if (fmt == QLatin1String("wav") && tryPlayWavWithAudioSink(path)) {
        return;
    }

    // 2. Sıkıştırılmış yerel zil/ses dosyaları (.mp3, .ogg, .m4a vb.) için ÖNCE süreç içi Qt6 QMediaPlayer dene.
    //    Böylece Pardus/Debian akıllı tahtalarda PipeWire/PulseAudio varsayılan HDMI çıkışı kullanılır
    //    ve 55. sn yumuşak sönümleme (fade-out) sorunsuz çalışır.
    //    QMediaPlayer hata verirse VEYA 2.5 sn içinde fiilen başlamazsa otomatik harici CLI yedeğine geçilir.
    const bool isCompressed = (fmt == QLatin1String("mp3") || fmt == QLatin1String("ogg") ||
                               fmt == QLatin1String("aac") || fmt == QLatin1String("m4a") ||
                               fmt == QLatin1String("flac"));
    if (isCompressed) {
        if (QMediaDevices::defaultAudioOutput().isNull()) {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Varsayılan Qt ses cihazı bulunamadı, harici oynatıcı yedeğine geçiliyor..."));
            playWithExternalFallback(path);
            return;
        }
        playWithQt(path, true);
        return;
    }

    playWithExternalFallback(path);
}

void LocalPlaylistPlayer::playWithExternalFallback(const QString& path) {
    if (m_state == State::Done) return;

    const QString fmt = detectAudioMagicFormat(path);
    const bool isCompressed = (fmt == QLatin1String("mp3") || fmt == QLatin1String("ogg") ||
                               fmt == QLatin1String("aac") || fmt == QLatin1String("m4a") ||
                               fmt == QLatin1String("flac"));

    QList<PlayAttempt> attempts;

#if defined(Q_OS_WIN)
    if (!QStandardPaths::findExecutable("mpv").isEmpty()) {
        attempts.append({"mpv", {"--no-video", "--really-quiet", path}});
    }
    if (!QStandardPaths::findExecutable("ffplay").isEmpty()) {
        attempts.append({"ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", path}});
    }
    // Windows'ta sıkıştırılmış dosyalar için sonsuz döngüye giren PowerShell WMPlayer.OCX KULLANILMAZ;
    // doğrudan QMediaPlayer (Windows Media Foundation) kullanılır!
    if (!isCompressed) {
        const QString winPath = QDir::toNativeSeparators(path);
        attempts.append({"powershell.exe", {
            "-ExecutionPolicy", "Bypass", "-NoProfile", "-NonInteractive",
            "-Command", QString("(New-Object Media.SoundPlayer '%1').PlaySync()").arg(winPath)
        }});
    }
#else
    if (!QStandardPaths::findExecutable("mpv").isEmpty()) {
        attempts.append({"mpv", {"--no-video", "--really-quiet", path}});
    }
    if (!QStandardPaths::findExecutable("ffplay").isEmpty()) {
        attempts.append({"ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", path}});
    }
    if (fmt == QLatin1String("mp3") && !QStandardPaths::findExecutable("mpg123").isEmpty()) {
        // Pardus/Debian akıllı tahtalarda ALSA'nın boş analog çıkışına düşmemesi için önce PulseAudio/PipeWire zorla
        attempts.append({"mpg123", {"-q", "-o", "pulse", path}});
        attempts.append({"mpg123", {"-q", path}});
    }
    if (fmt == QLatin1String("ogg") && !QStandardPaths::findExecutable("ogg123").isEmpty()) {
        attempts.append({"ogg123", {"-q", "-d", "pulse", path}});
        attempts.append({"ogg123", {"-q", path}});
    }
    if (!QStandardPaths::findExecutable("gst-play-1.0").isEmpty()) {
        attempts.append({"gst-play-1.0", {"--no-interactive", "--quiet", path}});
    }
    if (!isCompressed && !QStandardPaths::findExecutable("pw-play").isEmpty()) {
        attempts.append({"pw-play", {path}});
    }
    if (!isCompressed && !QStandardPaths::findExecutable("paplay").isEmpty()) {
        attempts.append({"paplay", {path}});
    }
    if (!isCompressed && !QStandardPaths::findExecutable("aplay").isEmpty()) {
        attempts.append({"aplay", {path}});
    }
#endif

    if (attempts.isEmpty()) {
        if (!m_qtAlreadyTried) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: QMediaPlayer ile çalınıyor → ") + path);
            playWithQt(path, false);
        } else {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Tüm oynatma yöntemleri tükendi, atlanıyor → ") + path);
            advance();
        }
        return;
    }

    tryPlayAttempt(attempts, 0);
}

void LocalPlaylistPlayer::tryPlayAttempt(const QList<PlayAttempt>& attempts, int index) {
    if (m_state == State::Done) return;

    if (index >= attempts.size()) {
        if (!m_qtAlreadyTried) {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Harici oynatma komutları tükendi, QMediaPlayer'a geçiliyor..."));
            playWithQt(m_currentPlayingPath, false);
        } else {
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Tüm ses oynatıcılar başarısız oldu → ") + m_currentPlayingPath);
            advance();
        }
        return;
    }

    auto attempt = attempts[index];
    Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma deneniyor (%1) → %2 %3")
                 .arg(index + 1)
                 .arg(attempt.program)
                 .arg(attempt.args.join(" ")));

    QProcess* p = new QProcess(this);
    QProcessEnvironment pEnv = audioEnv();
    QString launchProg = attempt.program;
    QStringList launchArgs = attempt.args;

#if !defined(Q_OS_WIN)
    // Root yetkisiyle (ör. X11 LightDM DM tray) çalışırken PulseAudio/PipeWire soketine
    // Access Denied yememek için harici oynatıcıyı aktif ses oturumu kullanıcısının UID/GID'siyle çalıştır.
    if (::geteuid() == 0) {
        QString audioUser, audioUid, audioGid, audioRunDir;
        if (resolveLinuxAudioUser(audioUser, audioUid, audioGid, audioRunDir)) {
            pEnv.insert(QStringLiteral("XDG_RUNTIME_DIR"), audioRunDir);
            pEnv.insert(QStringLiteral("PULSE_RUNTIME_PATH"), audioRunDir + QStringLiteral("/pulse"));
            pEnv.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=") + audioRunDir + QStringLiteral("/bus"));
            if (QFile::exists(QStringLiteral("/usr/bin/setpriv"))) {
                QStringList spArgs = {
                    QStringLiteral("--reuid=") + audioUid,
                    QStringLiteral("--regid=") + audioGid
                };
                if (::getpwuid(static_cast<uid_t>(audioUid.toUInt())) != nullptr) {
                    spArgs << QStringLiteral("--init-groups");
                } else {
                    spArgs << QStringLiteral("--clear-groups");
                }
                spArgs << launchProg << launchArgs;
                launchProg = QStringLiteral("/usr/bin/setpriv");
                launchArgs = spArgs;
            }
        }
    }
#endif

    p->setProcessEnvironment(pEnv);
    m_currentProcess = p;

    auto handleFailure = [this, p, attempts, index]() {
        if (m_state == State::Done) return;
        if (m_currentProcess == p) m_currentProcess = nullptr;
        p->disconnect();
        p->deleteLater();
        tryPlayAttempt(attempts, index + 1);
    };

    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p, handleFailure](int exitCode) {
        if (m_state == State::Done) {
            p->deleteLater();
            return;
        }
        if (m_currentProcess == p) m_currentProcess = nullptr;
        if (exitCode == 0) {
            Logger::info(QObject::tr("LocalPlaylistPlayer: Oynatma başarılı."));
            p->deleteLater();
            advance();
        } else {
            const QString errOut = QString::fromUtf8(p->readAllStandardError()).trimmed();
            Logger::warning(QObject::tr("LocalPlaylistPlayer: Süreç sıfır dışı çıkış koduyla bitti (%1)%2")
                            .arg(exitCode)
                            .arg(errOut.isEmpty() ? QString() : (": " + errOut.left(200))));
            handleFailure();
        }
    });

    connect(p, &QProcess::errorOccurred, this, [handleFailure](QProcess::ProcessError) {
        Logger::warning(QObject::tr("LocalPlaylistPlayer: Süreç çalıştırma hatası oluştu."));
        handleFailure();
    });

    p->start(launchProg, launchArgs);
}

void LocalPlaylistPlayer::playWithQt(const QString& path, bool allowExternalFallback) {
    if (m_state == State::Done) return;

    m_qtAlreadyTried      = true;
    m_qtFallbackAllowed   = allowExternalFallback;
    m_qtPlaybackConfirmed = false;
    stopQtStartWatchdog();

    const QAudioDevice defaultOut = QMediaDevices::defaultAudioOutput();

    if (!m_qtPlayer) {
        m_qtPlayer = new QMediaPlayer(this);
        m_qtAudio = defaultOut.isNull()
            ? new QAudioOutput(this)
            : new QAudioOutput(defaultOut, this);
        m_currentVolume = 1.0f;
        m_qtAudio->setVolume(1.0f);
        m_qtAudio->setMuted(false);
        m_qtPlayer->setAudioOutput(m_qtAudio);

        connect(m_qtPlayer, &QMediaPlayer::positionChanged,
                this, [this](qint64 pos) {
            if (pos > 0 && !m_qtPlaybackConfirmed) {
                m_qtPlaybackConfirmed = true;
                stopQtStartWatchdog();
            }
        });

        connect(m_qtPlayer, &QMediaPlayer::mediaStatusChanged,
                this, [this](QMediaPlayer::MediaStatus status) {
            if (m_state == State::Done) return;
            if (status == QMediaPlayer::EndOfMedia) {
                stopQtStartWatchdog();
                Logger::info(QObject::tr("LocalPlaylistPlayer: QMediaPlayer oynatımı tamamlandı."));
                advance();
            } else if (status == QMediaPlayer::InvalidMedia) {
                stopQtStartWatchdog();
                Logger::warning(QObject::tr("LocalPlaylistPlayer: QMediaPlayer geçersiz medya bildirdi → ") + m_currentPlayingPath);
                if (m_qtFallbackAllowed) {
                    m_qtFallbackAllowed = false;
                    m_qtPlayer->stop();
                    playWithExternalFallback(m_currentPlayingPath);
                } else {
                    advance();
                }
            }
        });

        connect(m_qtPlayer, &QMediaPlayer::errorOccurred,
                this, [this](QMediaPlayer::Error, const QString& errorString) {
            if (m_state == State::Done) return;
            stopQtStartWatchdog();
            Logger::warning(QObject::tr("LocalPlaylistPlayer: QMediaPlayer hatası: ") + errorString);
            if (m_qtFallbackAllowed) {
                m_qtFallbackAllowed = false;
                m_qtPlayer->stop();
                playWithExternalFallback(m_currentPlayingPath);
            } else {
                advance();
            }
        });
    } else if (m_qtAudio) {
        if (!defaultOut.isNull()) {
            m_qtAudio->setDevice(defaultOut);
        }
        m_currentVolume = 1.0f;
        m_qtAudio->setVolume(1.0f);
        m_qtAudio->setMuted(false);
    }

    if (allowExternalFallback) {
        if (!m_qtStartWatchdog) {
            m_qtStartWatchdog = new QTimer(this);
            m_qtStartWatchdog->setSingleShot(true);
            connect(m_qtStartWatchdog, &QTimer::timeout, this, [this]() {
                if (m_state == State::Done || m_qtPlaybackConfirmed || !m_qtFallbackAllowed) return;
                Logger::warning(QObject::tr("LocalPlaylistPlayer: QMediaPlayer 2.5 sn içinde oynatmayı başlatamadı (durum: %1, medya: %2), harici oynatıcı yedeğine geçiliyor → %3")
                                .arg(m_qtPlayer ? static_cast<int>(m_qtPlayer->playbackState()) : -1)
                                .arg(m_qtPlayer ? static_cast<int>(m_qtPlayer->mediaStatus()) : -1)
                                .arg(m_currentPlayingPath));
                m_qtFallbackAllowed = false;
                if (m_qtPlayer) {
                    m_qtPlayer->stop();
                }
                playWithExternalFallback(m_currentPlayingPath);
            });
        }
        m_qtStartWatchdog->start(2500);
    }

    Logger::info(QObject::tr("LocalPlaylistPlayer: QMediaPlayer ile oynatılıyor → %1 (Cihaz: %2)")
                 .arg(path, defaultOut.isNull() ? QObject::tr("Varsayılan") : defaultOut.description()));
    m_qtPlayer->setSource(QUrl::fromLocalFile(path));
    m_qtPlayer->play();
}

void LocalPlaylistPlayer::advance() {
    if (m_state == State::Done) return;

    switch (m_state) {
    case State::Intro: {
        m_state       = State::Anons;
        m_anonsPlayed = 0;
        QTimer::singleShot(350, this, [this]() {
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
            m_state = State::Done;
            Logger::info(QObject::tr("LocalPlaylistPlayer: Okul zili sekansı tamamlandı."));
            if (m_maxDurationTimer) m_maxDurationTimer->stop();
            if (m_fadeOutTimer) m_fadeOutTimer->stop();

            closeSplash();
            deleteLater();
            return;
        }

        if (m_anonsPlayed < m_repeatCount) {
            ++m_anonsPlayed;
            QTimer::singleShot(500, this, [this]() {
                if (m_state != State::Anons) return;
                Logger::info(QObject::tr("LocalPlaylistPlayer: Anons %1/%2")
                             .arg(m_anonsPlayed).arg(m_repeatCount));
                playFile(m_anonsPath);
            });
        } else {
            m_state = State::Outro;
            QTimer::singleShot(350, this, [this]() {
                if (m_state != State::Outro) return;
                Logger::info(QObject::tr("LocalPlaylistPlayer: Kapanış ding-dong çalınıyor..."));
                playFile(m_dingPath);
            });
        }
        break;
    }

    case State::Outro: {
        m_state = State::Done;
        if (m_splashWatchdog) m_splashWatchdog->stop();
        Logger::info(QObject::tr("LocalPlaylistPlayer: Anons sekansı tamamlandı."));
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

    m_splash->setWindowTitle(QStringLiteral("Okidia Announcement"));
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
    row->setSpacing(12);

    auto* ico = new QLabel("", inner);
    ico->setPixmap(ThemeManager::icon(":/ui/volume.svg").pixmap(28, 28));
    ico->setStyleSheet("background:transparent;");
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
}

void LocalPlaylistPlayer::closeSplash() {
    if (m_splash) {
        m_splash->hide();
        m_splash->close();
        m_splash->deleteLater();
        m_splash = nullptr;
    }
}
