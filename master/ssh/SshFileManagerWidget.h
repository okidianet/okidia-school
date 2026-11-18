// master/ssh/SshFileManagerWidget.h
#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QMenu>
#include <QDateTime>
#include <QDir>
#include <QCheckBox>
#include <QStackedWidget>
#include <QListWidget>
#include <QToolButton>
#include "SshClient.h"

class SessionManager;
class QPushButton;
class QProgressBar;
class QLabel;
class QCheckBox;

class SshFileManagerWidget : public QWidget {
    Q_OBJECT

public:
    explicit SshFileManagerWidget(SessionManager* session, QWidget* parent = nullptr, const QString& initialLocalPath = QString());
    ~SshFileManagerWidget() override;

    // Uzak makineye bağlan ve dosya yöneticisini başlat
    void setConfig(const SshClient::Config& config);
    void connectToHost(const SshClient::Config& config);
    
    // SessionManager üzerinden agent bilgilerini bulup bağlanır
    void connectToAgent(const QString& clientId);

    void disconnectFromHost();
    bool isConnected() const;

    void runRemoteCommandWithSudo(const QString& normalCmd, const QString& sudoCmdTpl, const QString& opName);

signals:
    void logMessage(const QString& msg);
    void connectionStatusChanged(bool connected);
    void localPathChanged(const QString& path);

public slots:
    void setLocalPath(const QString& path);

private slots:
    
    // Remote slots
    void onRemoteUpClicked();
    void onRemoteRefreshClicked();
    void onRemoteTableDoubleClicked(int row, int column);
    void onRemoteItemDoubleClicked(QListWidgetItem* item);
    void showRemoteContextMenu(const QPoint& pos);
    void onRemoteRenameAction();
    void onRemoteDeleteAction();
    void onRemoteCreateFolderAction();

    // Local slots
    void onLocalUpClicked();
    void onLocalRefreshClicked();
    void onLocalTableDoubleClicked(int row, int column);
    void onLocalItemDoubleClicked(QListWidgetItem* item);
    void showLocalContextMenu(const QPoint& pos);
    void onLocalRenameAction();
    void onLocalDeleteAction();
    void onLocalCreateFolderAction();

    // Navigation slots
    void onRemotePrevClicked();
    void onRemoteNextClicked();
    void onRemoteHomeClicked();
    void onRemoteRootClicked();
    void onLocalPrevClicked();
    void onLocalNextClicked();
    void onLocalHomeClicked();
    void onLocalRootClicked();

    // View slots
    void onChangeViewMode(int mode);

    // Transfer slots
    void onTransferToRemoteClicked();
    void onTransferToLocalClicked();

private:
    enum ViewMode { Details, SmallIcons, LargeIcons };

    struct FileInfo {
        QString name;
        qint64 size;
        bool isDir;
        QString permissions;
        QDateTime mtime;
    };

    void setupUi();
    void loadRemoteDirectory(const QString& path);
    void renderRemoteFiles();
    
    void loadLocalDirectory(const QString& path);
    void renderLocalFiles();

    void setUiEnabled(bool enabled);
    void updateProgress(qint64 sent, qint64 total, const QString& operation);
    void openLocalFile(const QString& fullPath);
    void downloadAndOpenRemoteFile(const QString& remotePath, const QString& fileName, bool force = false);

    SessionManager*       m_session = nullptr;
    SshClient*            m_client = nullptr;
    SshClient::Config     m_config;
    QString               m_remoteCurrentPath;
    QString               m_localCurrentPath;
    QString               m_sudoPassword;

    bool                  m_isLocalWindows = false;
    bool                  m_isRemoteWindows = false;
    QString               m_remoteHomePath = "/";
    QString               m_remoteRootPath = "/";

    ViewMode              m_viewMode = Details;
    QList<FileInfo>       m_remoteFiles;
    QList<FileInfo>       m_localFiles;

    // History
    QStringList           m_remoteHistory;
    int                   m_remoteHistoryIdx = -1;
    bool                  m_remoteHistorySkip = false;

    QStringList           m_localHistory;
    int                   m_localHistoryIdx = -1;
    bool                  m_localHistorySkip = false;

    // Üst UI
    QToolButton*          m_viewMenuBtn = nullptr;

    // Remote UI Bileşenleri
    QLabel*               m_remoteHeaderLbl = nullptr;
    QPushButton*          m_remotePrevBtn = nullptr;
    QPushButton*          m_remoteNextBtn = nullptr;
    QPushButton*          m_remoteUpBtn = nullptr;
    QPushButton*          m_remoteHomeBtn = nullptr;
    QPushButton*          m_remoteRootBtn = nullptr;
    QPushButton*          m_remoteRefreshBtn = nullptr;
    QLineEdit*            m_remotePathEdit = nullptr;
    QStackedWidget*       m_remoteStack = nullptr;
    QTableWidget*         m_remoteFileTable = nullptr;
    QListWidget*          m_remoteIconList = nullptr;
    QCheckBox*            m_remoteShowHidden = nullptr;
    
    // Local UI Bileşenleri
    QPushButton*          m_localPrevBtn = nullptr;
    QPushButton*          m_localNextBtn = nullptr;
    QPushButton*          m_localUpBtn = nullptr;
    QPushButton*          m_localHomeBtn = nullptr;
    QPushButton*          m_localRootBtn = nullptr;
    QPushButton*          m_localRefreshBtn = nullptr;
    QLineEdit*            m_localPathEdit = nullptr;
    QStackedWidget*       m_localStack = nullptr;
    QTableWidget*         m_localFileTable = nullptr;
    QListWidget*          m_localIconList = nullptr;
    QCheckBox*            m_localShowHidden = nullptr;

    // Transfer Buttons
    QPushButton*          m_transferToRemoteBtn = nullptr;
    QPushButton*          m_transferToLocalBtn = nullptr;

    // Progress UI
    QProgressBar*         m_progressBar = nullptr;
    QLabel*               m_progressLbl = nullptr;

    bool                  m_loadingRemote = false;
    bool                  m_loadingLocal = false;
    bool                  m_syncingFromOtherTab = false;
};
