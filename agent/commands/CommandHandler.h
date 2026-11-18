// agent/commands/CommandHandler.h
#pragma once

#include <QObject>
#include <QTimer>
#include <QQueue>
#include <QPointer>
#include <functional>
#include "../../common/network/Connection.h"
#include "../../common/network/LocalConnection.h"

class ScreenCapture;
class LocalPlaylistPlayer;
class AgentTray;

class CommandHandler : public QObject {
    Q_OBJECT

public:
    explicit CommandHandler(QObject* parent = nullptr);

    void handleEnvelope(const okidia::Envelope& envelope);

    // Servis tarafında — TLS Connection ile
    void setConnection(Connection* conn) {
        m_conn = conn;
        if (conn)
            m_sender = [conn](const okidia::Envelope& e) {
                conn->sendEnvelope(e);
            };
        else
            m_sender = nullptr;
    }

    // Tray tarafında — LocalConnection ile
    void setLocalConnection(LocalConnection* conn) {
        m_localConn = conn;
        if (conn)
            m_sender = [conn](const okidia::Envelope& e) {
                conn->sendEnvelope(e);
            };
        else
            m_sender = nullptr;
    }

    void setScreenCapture(ScreenCapture* capture) { m_capture = capture; }
    void setTray(AgentTray* tray) { m_tray = tray; }
    void setAgentType(uint32_t type) { m_agentType = type; }
    uint32_t agentType() const { return m_agentType; }
    bool isRemoteControlActive() const { return m_remoteControlActive; }
    bool isScreenStreamActive() const { return m_screenStreamActive; }
    void setScreenStreamActive(bool active) { m_screenStreamActive = active; }

    void screenLock(const okidia::ScreenLockCommand& cmd);
    void screenUnlock();
    void inputLock();
    void inputUnlock();

private:
    uint32_t         m_agentType  = 1;
    Connection*      m_conn       = nullptr;
    LocalConnection* m_localConn  = nullptr;
    std::function<void(const okidia::Envelope&)> m_sender;
    ScreenCapture*   m_capture    = nullptr;
    bool           m_remoteControlActive = false;
    bool           m_screenStreamActive = false;
    int            m_savedVolume  = 50;   // Mute öncesi ses seviyesi
    int            m_lastCursorShape = -1; // Son gönderilen imleç şekli
    int            m_lastCursorX = -1;
    int            m_lastCursorY = -1;

    // --- Ekran komutları ---
    void screenCapture();
    void screenSelect(const okidia::ScreenSelectCommand& cmd);
    void screenInfoRequest();
    void handleScreenConfig(const okidia::ScreenConfig& cmd);

    // --- Input komutları ---
    void inputTakeControl();
    void remoteControlStop();


    // --- Fare / klavye enjeksiyonu (uzak kontrol) ---
    void handleMouseEvent(const okidia::MouseEvent& evt);
    void handleKeyEvent(const okidia::KeyEvent& evt);

    // --- Güç komutları ---
    void powerShutdown(const okidia::PowerCommand& cmd);
    void powerRestart(const okidia::PowerCommand& cmd);
    void powerLogout();
    void powerHibernate();

    // --- Mesaj ---
    void showPopup(const okidia::ChatMessage& msg);

    // --- Ses ---
    void audioMute();
    void audioUnmute();
    void audioUnmuteOnly();
    void audioPlayLocal(const okidia::AudioPlayLocalCommand& cmd);
    void audioSetVolume(const okidia::AudioVolumeCommand& cmd);

    // --- Uygulama yönetimi ---
    void appClose(const okidia::AppCommand& cmd);
    void appListRequest();
    void appBlockByName(const okidia::AppCommand& cmd);
    void appBlockByHash(const okidia::AppCommand& cmd);
    void appBlockList(const okidia::AppCommand& cmd);
    void appWhitelistSet(const okidia::AppCommand& cmd);
    void appUnblock();

    // --- Uygulama Nöbetçisi (Watchdog) & Bildirim ---
    void initAppWatchdog();
    void checkBlockedApps();
    void notifyAppBlocked(const QString& appName);

    // --- Yardımcı ---
    void sendResult(bool success, const QString& message = "");
    void runCommand(const QString& program,
                    const QStringList& args,
                    bool waitForFinished = true);
    // Komutu çalıştır, exit code 0 ise true döner
    bool runCommandChecked(const QString& program,
                            const QStringList& args);
    QString runCommandOutput(const QString& program,
                             const QStringList& args);

private:
    struct PendingAnnounce {
        QString path;
        int repeat;
        bool isBell = false;
    };
    QQueue<PendingAnnounce> m_announceQueue;
    bool m_announcePlaying = false;
    bool m_currentPlayingIsBell = false;
    QPointer<LocalPlaylistPlayer> m_activePlayer;
    void playNextAnnounce();

    // Uygulama engelleme & Watchdog
    AgentTray*   m_tray = nullptr;
    QStringList  m_blockedApps;
    QTimer*      m_watchdogTimer = nullptr;
    qint64       m_lastBlockNotifyMs = 0;
};