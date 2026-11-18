// master/ssh/OtaUpdatePanel.h
#pragma once
#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QStackedWidget>
#include <QMap>
#include <QSet>
#include "../session/SessionManager.h"
#include "../transfer/OtaSender.h"
#include "DeployProgressGrid.h"

class OtaUpdatePanel : public QWidget {
    Q_OBJECT
public:
    explicit OtaUpdatePanel(SessionManager* session, QWidget* parent = nullptr);
    ~OtaUpdatePanel() override;

public slots:
    void refreshAgentList();

private slots:
    void onFetchFromGithub();
    void onSelectLocalFolder();
    void onSelectLocalPackages();
    void onSendToBroker();
    void onPackagesDownloaded(const QStringList& paths);
    void onBrokerReport(const QString& msg, bool success);
    void onManageBrokerClicked();
    void onUpdateBrokerClicked();
    void onClientStateChanged(const QString& clientId, const ClientState& state);
    void onClientDisconnected(const QString& clientId);
    void onTableContextMenu(const QPoint& pos);
    void onOtaTimeoutCheck();
    void openRemoteAgentLog(const QString& machineId, const QString& ip, const QString& hostname);

private:
    void setupUi();
    void updatePackageMappings();
    void restoreHistoryAndReconcile();
    void updateTopButtonsVisibility();

    SessionManager* m_session = nullptr;
    OtaSender* m_sender = nullptr;

    QStackedWidget*     m_leftStacked = nullptr;
    QTableWidget*       m_agentTable = nullptr;
    DeployProgressGrid* m_progressGrid = nullptr;
    QPushButton*        m_refreshBtn = nullptr;
    QPushButton*        m_showGridBtn = nullptr;
    QPushButton*        m_clearHistoryBtn = nullptr;
    QPushButton*        m_backBtn = nullptr;
    QTimer*             m_otaTimeoutTimer = nullptr;
    
    QPushButton* m_githubBtn = nullptr;
    QPushButton* m_localFolderBtn = nullptr;
    QPushButton* m_localDirBtn = nullptr;
    QPushButton* m_manageBrokerBtn = nullptr;
    QPushButton* m_updateBrokerBtn = nullptr;
    QLabel* m_selectedPackagesLabel = nullptr;
    QStringList m_selectedPackagePaths;
    QString m_selectedBrokerPackagePath;
    
    QPushButton* m_sendToBrokerBtn = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_statusLabel = nullptr;

    QMap<QString, QString>          m_targetVersions;     // key (machineId/ip/hostname) -> targetVer
    QMap<QString, QString>          m_machineIdToIp;      // machineId -> ip
    QMap<QString, HostInstallCard*> m_cardByMachineId;    // machineId -> card
    QMap<HostInstallCard*, QString> m_cardTargetVersions; // card -> targetVer
    QMap<HostInstallCard*, qint64>  m_cardActiveStartSecs;// card -> active install start timestamp
    QSet<HostInstallCard*>          m_offlineCards;       // gönderim anında çevrimdışı olan istemci kartları
};

