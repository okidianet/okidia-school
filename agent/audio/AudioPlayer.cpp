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
#include <QUrl>

AudioPlayer::AudioPlayer(QObject* parent)
    : QObject(parent)
{
    m_player = new QMediaPlayer(this);
    m_output = new QAudioOutput(this);
    m_player->setAudioOutput(m_output);
    m_output->setVolume(1.0f);

    connect(m_player, &QMediaPlayer::errorOccurred,
            [](QMediaPlayer::Error, const QString& msg) {
        Logger::error(QObject::tr("AudioPlayer: ") + msg);
    });

    // Oynatma bitince kuyrukta varsa bir sonrakini çal
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &AudioPlayer::onPlaybackStateChanged);
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
        const int sr = cmd.sample_rate() > 0 ? (int)cmd.sample_rate() : 44100;
        const int ch = cmd.channels()    > 0 ? (int)cmd.channels()    : 1;
        if (!pcm.isEmpty()) {
            startLiveSink(sr, ch);
            pushLivePcm(pcm);
        }
        break;
    }

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
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
}

void AudioPlayer::onPlaybackStateChanged()
{
    if (m_player->playbackState() == QMediaPlayer::StoppedState
            && !m_queue.isEmpty()) {
        playNext();
    }
}

// ── Canlı PCM — Saf Qt6 QAudioSink ───────────────────────────────────────

void AudioPlayer::startLiveSink(int sampleRate, int channels)
{
    if (m_liveSink && m_liveSampleRate == sampleRate && m_liveChannels == channels) {
        return;
    }

    stopLiveSink();

    QAudioFormat format;
    format.setSampleRate(sampleRate > 0 ? sampleRate : 44100);
    format.setChannelCount(channels > 0 ? channels : 1);
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
    if (!defaultDevice.isFormatSupported(format)) {
        format = defaultDevice.preferredFormat();
        format.setSampleFormat(QAudioFormat::Int16);
    }

    m_liveSink = new QAudioSink(defaultDevice, format, this);
    m_liveSink->setVolume(1.0f);
    // Tampon boyutu: 200 ms (cızırtı ve underrun önleme)
    const int bufferBytes = format.bytesForDuration(200000);
    if (bufferBytes > 0) {
        m_liveSink->setBufferSize(bufferBytes);
    }

    m_liveIoDevice = m_liveSink->start();
    if (!m_liveIoDevice) {
        Logger::error(QObject::tr("AudioPlayer: QAudioSink başlatılamadı"));
        delete m_liveSink;
        m_liveSink = nullptr;
        return;
    }

    m_liveSampleRate = format.sampleRate();
    m_liveChannels   = format.channelCount();
    Logger::info(QObject::tr("AudioPlayer: Canlı ses oynatıcı (QAudioSink) başlatıldı — %1Hz %2ch")
                 .arg(m_liveSampleRate).arg(m_liveChannels));
}

void AudioPlayer::pushLivePcm(const QByteArray& pcm)
{
    if (!m_liveSink || !m_liveIoDevice || pcm.isEmpty()) return;
    m_liveIoDevice->write(pcm);
}

void AudioPlayer::stopLiveSink()
{
    if (m_liveSink) {
        m_liveSink->stop();
        delete m_liveSink;
        m_liveSink = nullptr;
        m_liveIoDevice = nullptr;
        m_liveSampleRate = 0;
        m_liveChannels = 0;
        Logger::info(QObject::tr("AudioPlayer: Canlı ses akışı durduruldu"));
    }
}

// ── Durdur ───────────────────────────────────────────────────────────────

void AudioPlayer::stopPlayback()
{
    m_queue.clear();  // kuyruğu temizle

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->stop();
        Logger::info(QObject::tr("AudioPlayer: Dosya oynatma durduruldu"));
    }
    if (m_tmpFile) { delete m_tmpFile; m_tmpFile = nullptr; }

    stopLiveSink();
}
