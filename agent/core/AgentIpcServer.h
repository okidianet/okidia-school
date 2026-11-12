#pragma once

#include <QObject>
#include <QString>
#include "../../common/network/LocalConnection.h"
#include "../../common/network/Connection.h"

class TlsClient;
class FileReceiver;

class AgentIpcServer : public QObject {
    Q_OBJECT
public:
    explicit AgentIpcServer(TlsClient* tls, QObject* parent = nullptr);

    bool start();
    LocalServer* server() const { return m_localServer; }

    void sendToActive(const okidia::Envelope& env);
    void broadcast(const okidia::Envelope& env);

    // Active remote control caching
    void setRemoteControlActive(bool active) { m_remoteControlActive = active; }
    bool isRemoteControlActive() const { return m_remoteControlActive; }
    void setLastTakeControlEnv(const okidia::Envelope& env) { m_lastTakeControlEnv = env; }
    void setLastFpsSetEnv(const okidia::Envelope& env) { m_lastFpsSetEnv = env; }
    void clearFpsSetEnv() { m_lastFpsSetEnv.Clear(); }
    void setLastScreenSelectEnv(const okidia::Envelope& env) { m_lastScreenSelectEnv = env; }
    void clearRemoteControlEnv();

    // Active broadcast caching (Anti-Sabotaj Replay)
    void setLastBroadcastBeginEnv(const okidia::Envelope& env) { m_lastBroadcastBeginEnv = env; }
    void clearBroadcastEnv() { m_lastBroadcastBeginEnv.Clear(); }

    // Active exam caching (Anti-Sabotaj Replay)
    void setLastExamStartEnv(const okidia::Envelope& env) { m_lastExamStartEnv = env; }
    void clearExamEnv() { m_lastExamStartEnv.Clear(); }

    void replayActiveStateToTray(LocalConnection* conn);

    void setExpectedIpcToken(const QString& token) { m_expectedIpcToken = token; }
    QString expectedIpcToken() const { return m_expectedIpcToken; }

    // Tray'den alınan son ekran boyutları (SCREEN_INFO_REQUEST fallback için)
    uint32_t lastKnownScreenW() const { return m_lastKnownScreenW; }
    uint32_t lastKnownScreenH() const { return m_lastKnownScreenH; }

signals:
    void agentTypeUpdated(int newType);

private:
    void handleEnvelopeFromTray(LocalConnection* sender, const okidia::Envelope& env);

    TlsClient*    m_tls = nullptr;
    LocalServer*  m_localServer = nullptr;

    QString          m_expectedIpcToken;
    QSet<LocalConnection*> m_authenticatedClients;
    QSet<LocalConnection*> m_replayedClients;

    bool             m_remoteControlActive = false;
    okidia::Envelope m_lastTakeControlEnv;
    okidia::Envelope m_lastFpsSetEnv;
    okidia::Envelope m_lastScreenSelectEnv;
    okidia::Envelope m_lastBroadcastBeginEnv;
    okidia::Envelope m_lastExamStartEnv;

    // Tray'den gelen son ekran boyutları (fallback SCREEN_INFO_RESPONSE için)
    uint32_t         m_lastKnownScreenW = 0;
    uint32_t         m_lastKnownScreenH = 0;
};
