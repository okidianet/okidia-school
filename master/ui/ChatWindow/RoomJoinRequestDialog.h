// master/ui/ChatWindow/RoomJoinRequestDialog.h
#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>

class RoomJoinRequestDialog : public QDialog {
    Q_OBJECT
public:
    explicit RoomJoinRequestDialog(const QString& requestId,
                                   const QString& roomId,
                                   const QString& roomName,
                                   const QString& fromUsername,
                                   const QString& fromDisplay,
                                   QWidget* parent = nullptr);
    ~RoomJoinRequestDialog() override = default;

    QString requestId() const { return m_requestId; }
    QString roomId() const { return m_roomId; }
    QString fromUsername() const { return m_fromUsername; }

private slots:
    void onTick();
    void onAccept();
    void onReject();

private:
    QString m_requestId;
    QString m_roomId;
    QString m_roomName;
    QString m_fromUsername;
    QString m_fromDisplay;

    QLabel*       m_titleLabel    = nullptr;
    QLabel*       m_messageLabel  = nullptr;
    QLabel*       m_timerLabel    = nullptr;
    QProgressBar* m_progressBar   = nullptr;
    QPushButton*  m_acceptBtn     = nullptr;
    QPushButton*  m_rejectBtn     = nullptr;

    QTimer* m_timer    = nullptr;
    int     m_timeLeft = 30;
};
