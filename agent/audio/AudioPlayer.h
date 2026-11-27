// agent/audio/AudioPlayer.h
#pragma once

#include <QObject>
#include <QTemporaryFile>
#include <QProcess>
#include <QQueue>
#include <QAudioFormat>
#include "../../common/network/Connection.h"

class QMediaPlayer;
class QAudioOutput;

class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer();
    void handleEnvelope(const okidia::Envelope& envelope);
    bool isLiveActive() const { return m_liveSink != nullptr; }

signals:
    void livePlaybackStateChanged(bool active);
    void playbackActivityChanged(bool active);

private slots:
    void onPlaybackStateChanged();

private:
    // Dosya oynatma — sıralı kuyruk
    struct AudioItem { QByteArray data; QString format; };

    QMediaPlayer*   m_player  = nullptr;
    QAudioOutput*   m_output  = nullptr;
    QTemporaryFile* m_tmpFile = nullptr;
    QQueue<AudioItem> m_queue;

    void enqueueFileData(const QByteArray& data, const QString& format);
    void playNext();

    // Canlı PCM — Saf Qt6 QAudioSink + Format Dönüştürücü + Jitter/Underrun Koruması
    class QAudioSink* m_liveSink       = nullptr;
    class QIODevice*  m_liveIoDevice   = nullptr;
    class QTimer*     m_liveFeedTimer  = nullptr;
    QAudioFormat      m_liveOutFormat;
    QByteArray        m_liveJitterBuf;
    bool              m_livePrimed     = false;
    qint64            m_lastPacketMs   = 0;
    int               m_liveSampleRate = 0;
    int               m_liveChannels   = 0;
    void startLiveSink(int sampleRate, int channels);
    void pushLivePcm(const QByteArray& pcm);
    void feedLiveSink();
    void stopLiveSink();

    void stopPlayback();
    void updatePlaybackActivity();
    bool m_wasPlaybackActive = false;
};
