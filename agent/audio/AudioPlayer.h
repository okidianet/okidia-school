// agent/audio/AudioPlayer.h
#pragma once

#include <QObject>
#include <QTemporaryFile>
#include <QProcess>
#include <QQueue>
#include "../../common/network/Connection.h"

class QMediaPlayer;
class QAudioOutput;

class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer();
    void handleEnvelope(const okidia::Envelope& envelope);

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

    // Canlı PCM — Saf Qt6 QAudioSink (çapraz platform)
    class QAudioSink* m_liveSink       = nullptr;
    class QIODevice*  m_liveIoDevice   = nullptr;
    int               m_liveSampleRate = 0;
    int               m_liveChannels   = 0;
    void startLiveSink(int sampleRate, int channels);
    void pushLivePcm(const QByteArray& pcm);
    void stopLiveSink();

    void stopPlayback();
};
