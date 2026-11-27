#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <QTextEdit>
#include <QSplitter>
#include <QThread>
#include <QListWidget>
#include <QTreeWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <atomic>
#include <string>

#include "SshClient.h"

class SessionManager;

struct AgentPackage {
    QString fileName;
    QString remoteTmpPath;
    QString installCmd;
    QString osLabel;
    bool    isValid() const { return !fileName.isEmpty(); }
};

AgentPackage agentPackageFor(const QString& osTag, const QString& arch, const QString& label);

class NetworkScanner;
class SshCredSelectorWidget;
class QStackedWidget;
class DeployProgressGrid;
class QThreadPool;

class DeployPanel : public QWidget {
    Q_OBJECT

public:
    explicit DeployPanel(SessionManager* session, QWidget* parent = nullptr);
    ~DeployPanel() override;

    void updateDiscoveryCount(int count);

protected:
    void showEvent(QShowEvent* event) override;

signals:
    void agentInstalledOn(const QString& ip, const QString& hostname);
    void globalContextMenuRequested(const QPoint& pos);
    void logMessage(const QString& msg);
    void discoveryPanelRequested();
    void manageClientsRequested();
    void manageCustomProfilesRequested();

private slots:
    // Ağ tarama
    void onScanNetwork();
    void onScanProgress(int current, int total);
    void onHostUpdated(const QString& ip, const QString& hostname, const QString& osHint = QString());
    void onHostFound(const QString& ip);
    void onScanFinished();
    void onAddSavedHosts();
    void onRefreshTableClicked();

    // Toplu işlemler
    void onAssignBulkCredentials();
    void onOpenCredentialDialog(const QStringList& ips);
    
    // Kurulum
    void onBrowsePackage();
    void onInstallSelected();
    
    // Tablo
    void onUnifiedTableSelectionChanged();
    void onDeselectAll();
    void onHostDoubleClicked(QTableWidgetItem* item);
    void onTableContextMenu(const QPoint& pos);

    // L3 / VLAN ve Alt Ağ Yönetimi
    void onUpdateBrokerHostOnSelected();

private:
    void setupUi();
    void setupFloatingBar(QLayout* layout);
    void populateSubnetTree();

    void appendLog(const QString& msg);
    void probeRowViaSsh(int row, const QString& ip, const SshClient::Config& cfg);
    bool hasMatchingCredential(const QString& ip, const QString& hostname) const;
    static QThreadPool* probeThreadPool();
    static void cancelPendingProbes();

    // UI bileşenleri
    QTreeWidget*        m_subnetTree    = nullptr;
    QLineEdit*          m_rangeEdit     = nullptr;
    QPushButton*        m_scanBtn       = nullptr;
    QPushButton*        m_stopScanBtn   = nullptr;
    QPushButton*        m_addSavedHostsBtn = nullptr;
    QPushButton*        m_refreshTableBtn  = nullptr;
    class QToolButton*  m_btnClients       = nullptr;
    class QAction*      m_actNewClients    = nullptr;
    QProgressBar*       m_scanProgress  = nullptr;
    QLabel*             m_scanStatus    = nullptr;

    QStackedWidget*     m_rightStacked     = nullptr;
    DeployProgressGrid* m_progressGrid     = nullptr;
    QTableWidget*       m_unifiedHostTable = nullptr; 
    QPushButton*        m_showGridBtn      = nullptr;
    QPushButton*        m_clearHistoryBtn  = nullptr;
    QPushButton*        m_backBtn          = nullptr; 

    QWidget*            m_floatingBar   = nullptr;
    QLabel*             m_selectionCountLbl = nullptr;

    QComboBox*          m_pkgSourceCombo = nullptr;
    QLineEdit*          m_pkgPathEdit    = nullptr;
    QPushButton*        m_selectLocalPkgBtn = nullptr;
    QPushButton*        m_installBtn    = nullptr;
    QProgressBar*       m_installProg   = nullptr;
    QTextEdit*          m_installLog    = nullptr;
    SshCredSelectorWidget* m_credSelector = nullptr;

    // State
    SessionManager*     m_session       = nullptr;
    QString             m_agentPkgPath;
    NetworkScanner*     m_scanner       = nullptr;
    QList<SshClient*>   m_activeClients;
};

class NetworkScanner : public QThread {
    Q_OBJECT

public:
    explicit NetworkScanner(const QStringList& ipRanges, QObject* parent = nullptr);
    void stop() { m_stop.store(true); }

signals:
    void progress(int current, int total);
    void hostFound(const QString& ip);
    void hostUpdated(const QString& ip, const QString& hostname, const QString& osHint = QString());
    void finished();

protected:
    void run() override;

private:
    QStringList expandRange(const QString& range) const;
    QStringList m_ranges;
    std::atomic<bool> m_stop{false};
};
