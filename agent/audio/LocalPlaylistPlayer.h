// agent/audio/LocalPlaylistPlayer.h
#pragma once

#include <QObject>
#include <QWidget>

class LocalPlaylistPlayer : public QObject {
    Q_OBJECT

public:
    explicit LocalPlaylistPlayer(const QString& anonsPath,
                                 int repeatCount,
                                 bool isBell = false,
                                 QObject* parent = nullptr);
    ~LocalPlaylistPlayer();
    void start();
    void abort();
    bool isBell() const { return m_isBell; }

private:
    enum class State { Intro, Anons, Outro, Done };

    struct PlayAttempt {
        QString program;
        QStringList args;
    };

    void playFile(const QString& path);
    void tryPlayAttempt(const QList<PlayAttempt>& attempts, int index);
    void playWithQt(const QString& path);
    void advance();
    void showSplash();
    void closeSplash();
    QString resolveBellPath(const QString& requestedPath);
    void startFadeOut();
    void stopPlaybackAndFinish();

    QString       m_dingPath;
    QString       m_anonsPath;
    QString       m_currentPlayingPath;
    int           m_repeatCount;
    int           m_anonsPlayed = 0;
    bool          m_isBell      = false;
    State         m_state       = State::Intro;

    class QMediaPlayer* m_qtPlayer          = nullptr;
    class QAudioOutput* m_qtAudio           = nullptr;
    class QProcess*     m_currentProcess    = nullptr;
    class QTimer*       m_maxDurationTimer  = nullptr;
    class QTimer*       m_fadeOutTimer      = nullptr;
    float               m_currentVolume     = 1.0f;

    QWidget*      m_splash  = nullptr;
};

