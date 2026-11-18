// master/ssh/SshManagerPanel.h
#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QTextEdit>
#include <QSpinBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QCheckBox>
#include <QMutex>

#include "SshClient.h"

class SessionManager;
class SshTerminalWidget;
class SshFileManagerWidget;
class SshCredSelectorWidget;

/**
 * SSH Yönetimi Paneli.
 */
class SshManagerPanel : public QWidget {
    Q_OBJECT

public:
    explicit SshManagerPanel(SessionManager* session, QWidget* parent = nullptr);
    ~SshManagerPanel();

    void openTerminalForClient(const QString& clientId);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    // Bağlantı Ayarları
    void saveSettings();
    void loadSettings();
    void onFetchActiveClients();

    // Terminal
    void onOpenTerminal();
    void onOpenFileManager();
    void onLocalPathSync(const QString& path);
    void onAddDevice();
    void onRefreshDevice(int row = -1);
    void onHostContextMenu(const QPoint& pos);
    void onAssignCredentials();
    void onManageCredentials();

    // Komut
    void onRunCommand();
    void onSaveTemplate();
    void onManageTemplates();

private:
    void setupUi();
    void appendLog(const QString& msg);
    void loadTemplates();
    SshClient::Config buildConfig(const QString& ip) const;

    SessionManager* m_session = nullptr;

    // Aktif Hedef Bilgisi
    QString m_activeIp;
    QLabel* m_selectionCountLbl = nullptr;

    // Bağlantı Ayarları UI
    QTableWidget* m_hostList  = nullptr; // Hedef IP'lerin listesi
    QPushButton* m_fetchClientsBtn = nullptr;
    QPushButton* m_addSavedHostsBtn = nullptr;
    QPushButton* m_addDeviceBtn = nullptr;
    QPushButton* m_assignCredBtn = nullptr;
    QPushButton* m_manageCredsBtn = nullptr;

    // Sekmeler
    QTabWidget* m_tabs = nullptr;

    // Aktif Bağlantılar sekmesi (Cihaz Odaklı)
    QTabWidget* m_activeTabs = nullptr;
    QPushButton* m_connectBtn     = nullptr;
    QPushButton* m_closeAllBtn    = nullptr;
    QCheckBox*   m_activeNohupCheck = nullptr;
    QSpinBox*    m_fontSizeSpin   = nullptr;

    // Komut sekmesi
    QComboBox* m_tplOsFilterCombo = nullptr;
    QComboBox* m_cmdPresetCombo = nullptr;
    QComboBox* m_targetOsFilterCombo = nullptr;
    QComboBox* m_cmdSpeedCombo = nullptr;
    QCheckBox* m_cmdNohupCheck = nullptr;
    QTextEdit* m_cmdEdit   = nullptr;
    QPushButton* m_runBtn  = nullptr;
    QPushButton* m_saveTplBtn = nullptr;
    QPushButton* m_mgrTplBtn = nullptr;
    QScrollArea* m_cmdScrollArea = nullptr;
    QWidget* m_cmdScrollContainer = nullptr;
    QVBoxLayout* m_cmdLogLayout = nullptr;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onAddSavedHosts();
    void updateHostRowOs(const QString& ip, const QString& osTag);

private:
    QString resolveHostOs(const QString& ip, const QString& fallbackHostname = QString());
    void asyncResolveHostOs(const QString& ip);

    // State
    QList<SshClient*> m_activeClients;
    QString m_lastLocalPath;

    // Toplu Komut Eşzamanlılık & Temiz İptal State
    std::atomic<uint64_t> m_cmdExecutionEpoch{0};
    QMutex m_runningCmdMutex;
    QList<QPointer<SshClient>> m_runningCmdClients;
};
