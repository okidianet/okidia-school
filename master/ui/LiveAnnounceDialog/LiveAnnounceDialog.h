// master/ui/LiveAnnounceDialog/LiveAnnounceDialog.h
#pragma once

#include <QDialog>
#include <QStringList>
#include <QAudioDevice>

class SessionManager;
class QComboBox;
class QPushButton;
class QLabel;
class QTimer;

class LiveAnnounceDialog : public QDialog {
    Q_OBJECT

public:
    explicit LiveAnnounceDialog(SessionManager* session,
                                const QStringList& targets,
                                const QString& targetDescription,
                                QWidget* parent = nullptr);
    ~LiveAnnounceDialog() override;

private slots:
    void onToggleBroadcast();
    void onTimerTick();

private:
    void populateMicrophones();
    void setLiveState(bool isLive);

    SessionManager* m_session = nullptr;
    QStringList     m_targets;
    QString         m_targetDesc;

    QComboBox*      m_micCombo       = nullptr;
    QPushButton*    m_btnBroadcast   = nullptr;
    QPushButton*    m_btnClose       = nullptr;
    QLabel*         m_statusLabel    = nullptr;
    QLabel*         m_timerLabel     = nullptr;
    QTimer*         m_elapsedTimer   = nullptr;
    int             m_elapsedSeconds = 0;
};
