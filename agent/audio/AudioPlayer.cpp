// agent/audio/AudioPlayer.cpp
#include "AudioPlayer.h"
#include "../../common/utils/Logger.h"

#include <QAudioOutput>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QIODevice>
#include <QDir>
#include <QMediaPlayer>
#include <QTemporaryFile>
#include <QTimer>
#include <QDateTime>
#include <QtEndian>
#include <QUrl>
#include <cmath>
#include <vector>

namespace {

// Gelen Int16 PCM bloğunu hedef QAudioSink formatına (ör. Windows WASAPI 48kHz 2ch Float/Int16) dönüştürür
static QByteArray convertIncomingPcmToSinkFormat(const QByteArray& inPcm,
                                                 int inSampleRate,
                                                 int inChannels,
                                                 const QAudioFormat& outFmt) {
    if (inPcm.isEmpty() || inSampleRate <= 0 || inChannels <= 0) return {};

    const int outSr = qMax(8000, outFmt.sampleRate());
    const int outCh = qMax(1, outFmt.channelCount());
    const QAudioFormat::SampleFormat outSfmt = outFmt.sampleFormat();

    if (inSampleRate == outSr && inChannels == outCh && outSfmt == QAudioFormat::Int16) {
        return inPcm;
    }

    const int inFrameBytes = inChannels * 2;
    const int inFrames = inPcm.size() / inFrameBytes;
    if (inFrames <= 0) return {};

    std::vector<float> mono(inFrames, 0.0f);
    const uchar* src = reinterpret_cast<const uchar*>(inPcm.constData());
    for (int f = 0; f < inFrames; ++f) {
        float sum = 0.0f;
        for (int c = 0; c < inChannels; ++c) {
            qint16 s = qFromLittleEndian<qint16>(src + (f * inChannels + c) * 2);
            sum += static_cast<float>(s) / 32768.0f;
        }
        mono[f] = sum / static_cast<float>(inChannels);
    }

    const double ratio = static_cast<double>(inSampleRate) / static_cast<double>(outSr);
    const int outFrames = qMax(1, static_cast<int>(std::floor(inFrames / ratio)));

    int outBps = 2;
    if (outSfmt == QAudioFormat::Float || outSfmt == QAudioFormat::Int32) outBps = 4;
    else if (outSfmt == QAudioFormat::UInt8) outBps = 1;

    QByteArray out(outFrames * outCh * outBps, Qt::Uninitialized);
    uchar* dst = reinterpret_cast<uchar*>(out.data());

    for (int i = 0; i < outFrames; ++i) {
        const double srcPos = i * ratio;
        const int i0 = qMin(static_cast<int>(srcPos), inFrames - 1);
        const int i1 = qMin(i0 + 1, inFrames - 1);
        const float frac = static_cast<float>(srcPos - i0);
        const float val = qBound(-1.0f, mono[i0] * (1.0f - frac) + mono[i1] * frac, 1.0f);

        for (int c = 0; c < outCh; ++c) {
            uchar* cell = dst + (i * outCh + c) * outBps;
            if (outSfmt == QAudioFormat::Int16) {
                qToLittleEndian<qint16>(static_cast<qint16>(std::lrint(val * 32767.0f)), cell);
            } else if (outSfmt == QAudioFormat::Int32) {
                qToLittleEndian<qint32>(static_cast<qint32>(std::llround(static_cast<double>(val) * 2147483647.0)), cell);
            } else if (outSfmt == QAudioFormat::Float) {
                qToLittleEndian<float>(val, cell);
            } else if (outSfmt == QAudioFormat::UInt8) {
                *cell = static_cast<uchar>(qBound(0, static_cast<int>(std::lrint((val + 1.0f) * 127.5f)), 255));
            }
        }
    }

    return out;
}

} // namespace

AudioPlayer::AudioPlayer(QObject* parent)
    : QObject(parent)
{
    m_player = new QMediaPlayer(this);
    m_output = new QAudioOutput(this);
    m_player->setAudioOutput(m_output);
    m_output->setVolume(1.0f);

    connect(m_player, &QMediaPlayer::errorOccurred,
            [this](QMediaPlayer::Error, const QString& msg) {
        Logger::error(QObject::tr("AudioPlayer: ") + msg);
        if (!m_queue.isEmpty()) {
            playNext();
        } else {
            updatePlaybackActivity();
        }
    });

    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &AudioPlayer::onPlaybackStateChanged);

    m_liveFeedTimer = new QTimer(this);
    m_liveFeedTimer->setInterval(20);
    connect(m_liveFeedTimer, &QTimer::timeout, this, &AudioPlayer::feedLiveSink);
}

AudioPlayer::~AudioPlayer() { stopPlayback(); }

void AudioPlayer::handleEnvelope(const okidia::Envelope& envelope) {
    switch (envelope.type()) {

    case okidia::MessageType::AUDIO_BROADCAST_FILE: {
        okidia::AudioBroadcastCommand cmd;
        if (!cmd.ParseFromString(envelope.payload())) break;
        const QByteArray data = QByteArray::fromStdString(cmd.audio_data());
        const QString fmt     = QString::fromStdString(cmd.format());
        if (!data.isEmpty()) enqueueFileData(data, fmt);
        break;
    }

    case okidia::MessageType::AUDIO_BROADCAST_LIVE: {
        okidia::AudioBroadcastCommand cmd;
        if (!cmd.ParseFromString(envelope.payload())) break;
        const QByteArray pcm = QByteArray::fromStdString(cmd.audio_data());
        const int sr = cmd.sample_rate() > 0 ? static_cast<int>(cmd.sample_rate()) : 24000;
        const int ch = cmd.channels()    > 0 ? static_cast<int>(cmd.channels())    : 1;
        if (!pcm.isEmpty()) {
            startLiveSink(sr, ch);
            pushLivePcm(pcm);
        }
        break;
    }

    case okidia::MessageType::AUDIO_STOP:
    case okidia::MessageType::AUDIO_BROADCAST_STOP:
        stopPlayback();
        break;

    default: break;
    }
}

// ── Sıralı dosya oynatma kuyruğu ─────────────────────────────────────────

void AudioPlayer::enqueueFileData(const QByteArray& data, const QString& format)
{
    m_queue.enqueue({data, format});
    if (m_player->playbackState() != QMediaPlayer::PlayingState)
        playNext();
}

void AudioPlayer::playNext()
{
    if (m_queue.isEmpty()) return;

    const auto item = m_queue.dequeue();

    if (m_tmpFile) { delete m_tmpFile; m_tmpFile = nullptr; }

    QString ext = item.format.toLower();
    if (ext.isEmpty()) ext = "mp3";
    if (!ext.startsWith(".")) ext = "." + ext;

    m_tmpFile = new QTemporaryFile(
        QDir::tempPath() + "/okidia_audio_XXXXXX" + ext, this);
    m_tmpFile->setAutoRemove(true);

    if (!m_tmpFile->open()) {
        Logger::error(QObject::tr("AudioPlayer: Gecici dosya acilamadi"));
        delete m_tmpFile; m_tmpFile = nullptr;
        playNext();
        return;
    }
    m_tmpFile->write(item.data);
    m_tmpFile->flush();
    const QString path = m_tmpFile->fileName();
    m_tmpFile->close();

    Logger::info(QObject::tr("AudioPlayer: Caliniyor → ") + path +
                 " (" + QString::number(item.data.size() / 1024) + QObject::tr(" KB)"));
    if (!m_wasPlaybackActive) {
        m_wasPlaybackActive = true;
        emit playbackActivityChanged(true);
    }
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
}

void AudioPlayer::onPlaybackStateChanged()
{
    if (m_player->playbackState() == QMediaPlayer::StoppedState
            && !m_queue.isEmpty()) {
        playNext();
        return;
    }
    updatePlaybackActivity();
}

// ── Canlı PCM — Saf Qt6 QAudioSink + Format Dönüştürücü + Jitter Tamponu ──

void AudioPlayer::startLiveSink(int sampleRate, int channels)
{
    // H5 düzeltmesi: m_liveSampleRate ve m_liveChannels İSTENEN (gelen paket) formatını saklar,
    // m_liveOutFormat ise ses kartının gerçek çıkış formatını saklar!
    // Böylece Windows WASAPI 48kHz stereo tercih ettiğinde her pakette sink yıkılıp yeniden kurulmaz!
    if (m_liveSink && m_liveSampleRate == sampleRate && m_liveChannels == channels) {
        return;
    }

    stopLiveSink();

    QAudioFormat format;
    format.setSampleRate(sampleRate > 0 ? sampleRate : 24000);
    format.setChannelCount(channels > 0 ? channels : 1);
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
    if (!defaultDevice.isFormatSupported(format)) {
        format = defaultDevice.preferredFormat();
        if (format.sampleRate() <= 0) format.setSampleRate(48000);
        if (format.channelCount() <= 0) format.setChannelCount(2);
    }
    m_liveOutFormat  = format;
    m_liveSampleRate = sampleRate;
    m_liveChannels   = channels;

    m_liveSink = new QAudioSink(defaultDevice, m_liveOutFormat, this);
    m_liveSink->setVolume(1.0f);
    // Tampon boyutu: 240 ms (cızırtı ve underrun önleme)
    const int bufferBytes = m_liveOutFormat.bytesForDuration(240000);
    if (bufferBytes > 0) {
        m_liveSink->setBufferSize(bufferBytes);
    }

    m_liveIoDevice = m_liveSink->start();
    if (!m_liveIoDevice) {
        Logger::error(QObject::tr("AudioPlayer: QAudioSink başlatılamadı"));
        delete m_liveSink;
        m_liveSink = nullptr;
        m_liveSampleRate = 0;
        m_liveChannels = 0;
        return;
    }

    m_liveJitterBuf.clear();
    m_livePrimed = false;
    m_lastPacketMs = QDateTime::currentMSecsSinceEpoch();
    m_liveFeedTimer->start();
    updatePlaybackActivity();
    emit livePlaybackStateChanged(true);

    Logger::info(QObject::tr("AudioPlayer: Canlı ses oynatıcı (QAudioSink) başlatıldı — Gelen: %1Hz %2ch → Çıkış: %3Hz %4ch")
                 .arg(m_liveSampleRate).arg(m_liveChannels)
                 .arg(m_liveOutFormat.sampleRate()).arg(m_liveOutFormat.channelCount()));
}

void AudioPlayer::pushLivePcm(const QByteArray& pcm)
{
    if (!m_liveSink || !m_liveIoDevice || pcm.isEmpty()) return;

    m_lastPacketMs = QDateTime::currentMSecsSinceEpoch();
    const QByteArray converted = convertIncomingPcmToSinkFormat(pcm, m_liveSampleRate, m_liveChannels, m_liveOutFormat);
    if (converted.isEmpty()) return;

    // Aşırı gecikme (drift) birikmesini önlemek için maksimum 600 ms tampon sınırı
    const int maxJitterBytes = qMax(16384, m_liveOutFormat.bytesForDuration(600000));
    if (m_liveJitterBuf.size() + converted.size() > maxJitterBytes) {
        const int drop = (m_liveJitterBuf.size() + converted.size()) - maxJitterBytes;
        const int frameBytes = qMax(1, m_liveOutFormat.bytesPerFrame());
        const int alignedDrop = (drop / frameBytes) * frameBytes;
        if (alignedDrop > 0 && alignedDrop <= m_liveJitterBuf.size()) {
            m_liveJitterBuf.remove(0, alignedDrop);
        }
    }

    m_liveJitterBuf.append(converted);
    feedLiveSink();
}

void AudioPlayer::feedLiveSink()
{
    if (!m_liveSink || !m_liveIoDevice) return;

    // 3 saniye boyunca hiç paket gelmediyse canlı akışı temizle
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastPacketMs > 0 && (now - m_lastPacketMs > 3500) && m_liveJitterBuf.isEmpty()) {
        stopLiveSink();
        return;
    }

    // 120 ms ön-tamponlama (jitter pre-fill): kesikliği önler
    const int primeBytes = qMax(2048, m_liveOutFormat.bytesForDuration(120000));
    if (!m_livePrimed) {
        if (m_liveJitterBuf.size() < primeBytes) {
            return;
        }
        m_livePrimed = true;
    }

    if (m_liveJitterBuf.isEmpty()) {
        m_livePrimed = false;
        return;
    }

    const int freeBytes = m_liveSink->bytesFree();
    if (freeBytes <= 0) return;

    const int toWrite = qMin(freeBytes, static_cast<int>(m_liveJitterBuf.size()));
    const qint64 written = m_liveIoDevice->write(m_liveJitterBuf.constData(), toWrite);
    if (written > 0) {
        m_liveJitterBuf.remove(0, static_cast<int>(written));
    }
}

void AudioPlayer::stopLiveSink()
{
    if (m_liveFeedTimer && m_liveFeedTimer->isActive()) {
        m_liveFeedTimer->stop();
    }
    m_liveJitterBuf.clear();
    m_livePrimed = false;

    if (m_liveSink) {
        m_liveSink->stop();
        delete m_liveSink;
        m_liveSink = nullptr;
        m_liveIoDevice = nullptr;
        m_liveSampleRate = 0;
        m_liveChannels = 0;
        updatePlaybackActivity();
        emit livePlaybackStateChanged(false);
        Logger::info(QObject::tr("AudioPlayer: Canlı ses akışı durduruldu"));
    }
}

void AudioPlayer::updatePlaybackActivity()
{
    const bool fileActive = (m_player && m_player->playbackState() == QMediaPlayer::PlayingState) || !m_queue.isEmpty();
    const bool nowActive = (m_liveSink != nullptr) || fileActive;
    if (nowActive != m_wasPlaybackActive) {
        m_wasPlaybackActive = nowActive;
        emit playbackActivityChanged(nowActive);
    }
}

// ── Durdur ───────────────────────────────────────────────────────────────

void AudioPlayer::stopPlayback()
{
    m_queue.clear();

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->stop();
        Logger::info(QObject::tr("AudioPlayer: Dosya oynatma durduruldu"));
    }
    if (m_tmpFile) { delete m_tmpFile; m_tmpFile = nullptr; }

    stopLiveSink();
    updatePlaybackActivity();
}
