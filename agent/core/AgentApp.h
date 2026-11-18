#pragma once

#include <QObject>
#include <QTimer>
#include <QProcess>
#include "../../common/network/TlsClient.h"
#include "../../common/network/Connection.h"

class WebFilter;
class FileReceiver;
class AgentIpcServer;
class AgentDiscoveryCoordinator;

class AgentApp : public QObject {
    Q_OBJECT
public:
    explicit AgentApp(QObject* parent = nullptr);
    ~AgentApp() override;

    bool init();

private slots:
    void onBrokerConnected(Connection* conn);
    void onBrokerEnvelopeReceived(Connection* conn, const okidia::Envelope& env);
    void onMetricsTimer();
    void onHeartbeatTimer();
    void checkSession();

private:
    void sendClientInfo(const QString& username, uint32_t w = 0, uint32_t h = 0);
    void sendSystemMetrics();
    void sendBlackFrame();
    void tryTriggerOta();

    static bool isUiMessage(okidia::MessageType t);

    TlsClient*                 m_tls = nullptr;
    AgentIpcServer*            m_ipcServer = nullptr;
    AgentDiscoveryCoordinator* m_discovery = nullptr;
    WebFilter*                 m_webFilter = nullptr;
    FileReceiver*              m_otaReceiver = nullptr;

    QTimer* m_metricsTimer = nullptr;
    QTimer* m_heartbeatTimer = nullptr;
    QTimer* m_sessionTimer = nullptr;

    int  m_agentType = 1;
    bool m_detailedMetrics = false;

    QString    m_currentActive;
    QProcess*  m_dmTrayProc = nullptr;
#if defined(Q_OS_WIN)
    void*      m_hDmTrayProc = nullptr;
    void*      m_hUserTrayProc = nullptr;
    void*      m_hUserTrayWait = nullptr;
#endif
    qint64     m_lastUserTrayLaunch = 0;
    int        m_userTrayCrashCount = 0;
    qint64     m_userTrayBackoffUntil = 0;

    QString    m_ipcToken;
    QString    m_pendingOtaPath;
    QByteArray m_pendingSig;
    QByteArray m_pendingHash;
    QByteArray m_pendingCert;
    bool       m_timeSyncedThisSession = false;
    bool       m_rtcBatteryLow = false;
    class LocalScheduleWatchdog* m_scheduleWatchdog = nullptr;
};
