// master/ssh/SshFileManagerWidget.cpp
#include "SshFileManagerWidget.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>
#include <QDateTime>
#include <QStyle>
#include <QApplication>
#include <QTimer>
#include <QtConcurrent>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QProcess>
#include <QActionGroup>
#include <QToolButton>
#include <QStackedWidget>
#include <QListWidget>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <algorithm>

SshFileManagerWidget::SshFileManagerWidget(SessionManager* session, QWidget* parent, const QString& initialLocalPath)
    : QWidget(parent), m_session(session)
{
#if defined(Q_OS_WIN)
    m_isLocalWindows = true;
#else
    m_isLocalWindows = false;
#endif

    m_client = new SshClient(this);
    m_remoteCurrentPath = "/";
    m_localCurrentPath = initialLocalPath.isEmpty()
        ? OkidiaUtils::getLastDirectory("ssh_sftp_local", QDir::homePath())
        : initialLocalPath;

    setupUi();
    setUiEnabled(false);

    // Bağlantı Sinyalleri
    connect(m_client, &SshClient::connected, this, [this]() {
        emit logMessage(tr("Dosya Yöneticisi: SSH bağlantısı başarılı."));
        emit connectionStatusChanged(true);
        setUiEnabled(true);
        
        QString hostname = property("hostname").toString();
        QString ip = property("ip").toString();
        if (!hostname.isEmpty() || !ip.isEmpty()) {
            m_remoteHeaderLbl->setText(QString(tr("<b>Uzak Dosyalar (%1 - %2)</b>")).arg(hostname, ip));
        }

        // Uzak işletim sistemini ve home/root dizinini tespit et
        std::ignore = QtConcurrent::run([this]() {
            bool isWin = false;
            bool osKnown = false;

            // 1. Session'da bilinen istemci bilgisi var mı? (Hızlı yol — SSH gerektirmez)
            QString targetIp = property("ip").toString();
            QString clientId = property("clientId").toString();
            if (m_session) {
                for (const auto& agent : m_session->allClientStates()) {
                    if ((!targetIp.isEmpty() && agent.ip == targetIp) || (!clientId.isEmpty() && (agent.clientId == clientId || agent.machineId == clientId))) {
                        if (agent.osTag.toLower().contains("windows") || agent.osPrettyName.toLower().contains("windows")) {
                            isWin = true;
                        }
                        osKnown = true;  // bulundu; SSH sorgusu gereksiz
                        break;
                    }
                }
            }

            // 2. SSH üzerinden OS tespiti — yalnızca session bilgisi yoksa çalıştır
            if (!osKnown) {
                // Önce hızlı Linux testi (uname anında cevap verir, cmd.exe gibi 3 sn timeout yok)
                QString unameOut = m_client->executeSync("uname -s", 2000).trimmed();
                if (unameOut.startsWith("Linux", Qt::CaseInsensitive) ||
                    unameOut.startsWith("Darwin", Qt::CaseInsensitive) ||
                    unameOut.startsWith("FreeBSD", Qt::CaseInsensitive)) {
                    isWin = false;
                } else {
                    // Linux değilse Windows olabilir; Windows-spesifik kontrol
                    QString osEnv = m_client->executeSync("cmd.exe /c echo %OS%", 3000).trimmed();
                    if (osEnv.contains("Windows", Qt::CaseInsensitive)) {
                        isWin = true;
                    } else {
                        QString verOut = m_client->executeSync("ver", 2000).trimmed();
                        if (verOut.contains("Windows", Qt::CaseInsensitive) || verOut.contains("Microsoft", Qt::CaseInsensitive)) {
                            isWin = true;
                        }
                    }
                }
            }

            QString homePath;
            QString rootPath;

            if (isWin) {
                rootPath = "C:/";
                QString userProfile = m_client->executeSync("cmd.exe /c echo %USERPROFILE%", 3000).trimmed();
                if (!userProfile.isEmpty() && !userProfile.contains("%USERPROFILE%")) {
                    homePath = OkidiaUtils::fromRemotePathSeparators(userProfile);
                } else {
                    QString psHome = m_client->executeSync("powershell -NoProfile -Command \"$HOME\"", 3000).trimmed();
                    if (!psHome.isEmpty() && psHome.contains(":")) {
                        homePath = OkidiaUtils::fromRemotePathSeparators(psHome);
                    } else {
                        homePath = "C:/Users";
                    }
                }
            } else {
                rootPath = "/";
                QString unixHome = m_client->executeSync("echo $HOME", 3000).trimmed();
                if (!unixHome.isEmpty() && unixHome.startsWith("/")) {
                    homePath = unixHome;
                } else {
                    homePath = "/home";
                }
            }

            QMetaObject::invokeMethod(this, [this, isWin, homePath, rootPath]() {
                m_isRemoteWindows = isWin;
                m_remoteHomePath = OkidiaUtils::normalizeRemotePath(homePath, isWin, false);
                m_remoteRootPath = OkidiaUtils::normalizeRemotePath(rootPath, isWin, false);
                m_remoteCurrentPath = m_remoteHomePath;
                m_remotePathEdit->setPlaceholderText(isWin ? tr("Uzak Dizin Yolu (C:\\Users\\...)") : tr("Uzak Dizin Yolu (/home/...)"));
                m_remoteRootBtn->setToolTip(isWin ? tr("Bilgisayarım (Sürücüler)") : tr("Kök Dizin (/)"));
                loadRemoteDirectory(m_remoteHomePath);
            }, Qt::QueuedConnection);
        });
    });

    connect(m_client, &SshClient::errorOccurred, this, [this](const QString& err) {
        emit logMessage("Dosya Yöneticisi Hatası: " + err);
        m_loadingRemote = false;
        m_progressLbl->clear();
        m_progressBar->setVisible(false);
        QApplication::restoreOverrideCursor();
        setUiEnabled(false);
        emit connectionStatusChanged(false);
        QMessageBox::critical(this, tr("Dosya Yöneticisi Hatası"), tr("SSH bağlantı hatası: ") + err);
    });

    connect(m_client, &SshClient::disconnected, this, [this]() {
        emit logMessage("Dosya Yöneticisi: Bağlantı kesildi.");
        m_loadingRemote = false;
        m_progressLbl->clear();
        m_progressBar->setVisible(false);
        QApplication::restoreOverrideCursor();
        setUiEnabled(false);
        emit connectionStatusChanged(false);
    });

    loadLocalDirectory(m_localCurrentPath);
}

SshFileManagerWidget::~SshFileManagerWidget() {
    if (m_client) {
        m_client->disconnectFromHost();
    }
    m_config.wipe();
}

void SshFileManagerWidget::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(8, 8, 8, 8);
    mainLay->setSpacing(6);

    auto* topLay = new QHBoxLayout();
    topLay->addStretch();
    
    m_viewMenuBtn = new QToolButton(this);
    m_viewMenuBtn->setText(tr("Görünüm"));
    m_viewMenuBtn->setPopupMode(QToolButton::InstantPopup);
    QMenu* viewMenu = new QMenu(m_viewMenuBtn);
    
    QAction* actDetails = viewMenu->addAction(tr("Ayrıntılar"));
    QAction* actSmall = viewMenu->addAction(tr("Küçük Simgeler"));
    QAction* actLarge = viewMenu->addAction(tr("Büyük Simgeler"));
    actDetails->setCheckable(true);
    actSmall->setCheckable(true);
    actLarge->setCheckable(true);
    
    QActionGroup* viewGroup = new QActionGroup(this);
    viewGroup->addAction(actDetails);
    viewGroup->addAction(actSmall);
    viewGroup->addAction(actLarge);
    actDetails->setChecked(true);
    
    m_viewMenuBtn->setMenu(viewMenu);
    topLay->addWidget(m_viewMenuBtn);
    
    connect(actDetails, &QAction::triggered, this, [this]() { onChangeViewMode(Details); });
    connect(actSmall, &QAction::triggered, this, [this]() { onChangeViewMode(SmallIcons); });
    connect(actLarge, &QAction::triggered, this, [this]() { onChangeViewMode(LargeIcons); });
    
    mainLay->addLayout(topLay);

    auto* splitLay = new QHBoxLayout();
    splitLay->setSpacing(6);

    // --- LOCAL PANE ---
    auto* localGroup = new QWidget(this);
    auto* localLay = new QVBoxLayout(localGroup);
    localLay->setContentsMargins(0,0,0,0);
    
    auto* localHeaderLbl = new QLabel(tr("<b>Yerel Dosyalar (Master)</b>"), this);
    localLay->addWidget(localHeaderLbl);

    auto* localNavLay = new QHBoxLayout();
    m_localPrevBtn = new QPushButton(this);
    m_localPrevBtn->setIcon(ThemeManager::icon(":/ui/arrow_left.svg"));
    m_localPrevBtn->setFixedWidth(32);
    m_localPrevBtn->setToolTip(tr("Önceki"));
    
    m_localNextBtn = new QPushButton(this);
    m_localNextBtn->setIcon(ThemeManager::icon(":/ui/arrow_right.svg"));
    m_localNextBtn->setFixedWidth(32);
    m_localNextBtn->setToolTip(tr("Sonraki"));
    
    m_localUpBtn = new QPushButton(this);
    m_localUpBtn->setIcon(ThemeManager::icon(":/ui/arrow_up.svg"));
    m_localUpBtn->setFixedWidth(32);
    m_localUpBtn->setToolTip(tr("Bir Üst Klasör"));
    
    m_localHomeBtn = new QPushButton(this);
    m_localHomeBtn->setIcon(ThemeManager::icon(":/ui/home.svg"));
    m_localHomeBtn->setFixedWidth(32);
    m_localHomeBtn->setToolTip(tr("Kullanıcı Klasörü"));
    
    m_localRootBtn = new QPushButton(this);
    m_localRootBtn->setIcon(ThemeManager::icon(":/ui/root_drive.svg"));
    m_localRootBtn->setFixedWidth(32);
    m_localRootBtn->setToolTip(m_isLocalWindows ? tr("Bilgisayarım (Sürücüler)") : tr("Kök Dizin (/)"));
    
    m_localPathEdit = new QLineEdit(this);
    m_localRefreshBtn = new QPushButton(this);
    m_localRefreshBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_localRefreshBtn->setFixedWidth(32);
    m_localRefreshBtn->setToolTip(tr("Tazele"));
    m_localShowHidden = new QCheckBox(tr("Gizli Dosyalar"), this);
    
    localNavLay->addWidget(m_localPrevBtn);
    localNavLay->addWidget(m_localNextBtn);
    localNavLay->addWidget(m_localUpBtn);
    localNavLay->addWidget(m_localHomeBtn);
    localNavLay->addWidget(m_localRootBtn);
    localNavLay->addWidget(m_localPathEdit, 1);
    localNavLay->addWidget(m_localRefreshBtn);
    localNavLay->addWidget(m_localShowHidden);
    localLay->addLayout(localNavLay);

    m_localStack = new QStackedWidget(this);
    
    m_localFileTable = new QTableWidget(0, 5, this);
    m_localFileTable->setHorizontalHeaderLabels({tr("İsim"), tr("Boyut"), tr("Tür"), tr("İzinler"), tr("Değiştirilme Tarihi")});
    m_localFileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_localFileTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localFileTable->setAlternatingRowColors(true);
    m_localFileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_localFileTable->setContextMenuPolicy(Qt::CustomContextMenu);
    OkidiaUtils::setupTableHeaders(m_localFileTable, "local_file_table", {220, 85, 85, 90, 145}, false);
    m_localFileTable->horizontalHeader()->setSortIndicatorShown(true);
    m_localFileTable->setSortingEnabled(true);
    if (!m_localFileTable->horizontalHeader()->isSortIndicatorShown() || m_localFileTable->horizontalHeader()->sortIndicatorSection() < 0) {
        m_localFileTable->sortByColumn(4, Qt::DescendingOrder);
        m_localFileTable->horizontalHeader()->setSortIndicator(4, Qt::DescendingOrder);
    }
    
    m_localIconList = new QListWidget(this);
    m_localIconList->setViewMode(QListView::IconMode);
    m_localIconList->setResizeMode(QListView::Adjust);
    m_localIconList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localIconList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_localIconList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_localIconList->setMovement(QListView::Static);
    m_localIconList->setSpacing(5);
    
    m_localStack->addWidget(m_localFileTable);
    m_localStack->addWidget(m_localIconList);
    m_localStack->setCurrentWidget(m_localFileTable);
    
    localLay->addWidget(m_localStack, 1);
    splitLay->addWidget(localGroup, 1);

    // --- MIDDLE TRANSFER BUTTONS ---
    auto* midLay = new QVBoxLayout();
    midLay->setAlignment(Qt::AlignCenter);
    m_transferToRemoteBtn = new QPushButton(this);
    m_transferToRemoteBtn->setIcon(ThemeManager::icon(":/ui/transfer_right.svg"));
    m_transferToRemoteBtn->setIconSize(QSize(20, 20));
    m_transferToRemoteBtn->setToolTip(tr("Dosya(ları) Gönder"));
    m_transferToRemoteBtn->setFixedWidth(40);
    m_transferToRemoteBtn->setFixedHeight(40);
    
    m_transferToLocalBtn = new QPushButton(this);
    m_transferToLocalBtn->setIcon(ThemeManager::icon(":/ui/transfer_left.svg"));
    m_transferToLocalBtn->setIconSize(QSize(20, 20));
    m_transferToLocalBtn->setToolTip(tr("Dosya(ları) Al"));
    m_transferToLocalBtn->setFixedWidth(40);
    m_transferToLocalBtn->setFixedHeight(40);
    
    midLay->addWidget(m_transferToRemoteBtn);
    midLay->addWidget(m_transferToLocalBtn);
    splitLay->addLayout(midLay);

    // --- REMOTE PANE ---
    auto* remoteGroup = new QWidget(this);
    auto* remoteLay = new QVBoxLayout(remoteGroup);
    remoteLay->setContentsMargins(0,0,0,0);

    m_remoteHeaderLbl = new QLabel(tr("<b>Uzak Dosyalar (İstemci)</b>"), this);
    remoteLay->addWidget(m_remoteHeaderLbl);

    auto* remoteNavLay = new QHBoxLayout();
    m_remotePrevBtn = new QPushButton(this);
    m_remotePrevBtn->setIcon(ThemeManager::icon(":/ui/arrow_left.svg"));
    m_remotePrevBtn->setFixedWidth(32);
    m_remotePrevBtn->setToolTip(tr("Önceki"));
    
    m_remoteNextBtn = new QPushButton(this);
    m_remoteNextBtn->setIcon(ThemeManager::icon(":/ui/arrow_right.svg"));
    m_remoteNextBtn->setFixedWidth(32);
    m_remoteNextBtn->setToolTip(tr("Sonraki"));
    
    m_remoteUpBtn = new QPushButton(this);
    m_remoteUpBtn->setIcon(ThemeManager::icon(":/ui/arrow_up.svg"));
    m_remoteUpBtn->setFixedWidth(32);
    m_remoteUpBtn->setToolTip(tr("Bir Üst Klasör"));
    
    m_remoteHomeBtn = new QPushButton(this);
    m_remoteHomeBtn->setIcon(ThemeManager::icon(":/ui/home.svg"));
    m_remoteHomeBtn->setFixedWidth(32);
    m_remoteHomeBtn->setToolTip(tr("Kullanıcı Klasörü"));
    
    m_remoteRootBtn = new QPushButton(this);
    m_remoteRootBtn->setIcon(ThemeManager::icon(":/ui/root_drive.svg"));
    m_remoteRootBtn->setFixedWidth(32);
    m_remoteRootBtn->setToolTip(tr("Kök Dizin (/)"));
    
    m_remotePathEdit = new QLineEdit(this);
    m_remotePathEdit->setPlaceholderText(tr("Uzak Dizin Yolu (/home/user)"));
    m_remoteRefreshBtn = new QPushButton(this);
    m_remoteRefreshBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_remoteRefreshBtn->setFixedWidth(32);
    m_remoteRefreshBtn->setToolTip(tr("Tazele"));
    m_remoteShowHidden = new QCheckBox(tr("Gizli Dosyalar"), this);
    
    remoteNavLay->addWidget(m_remotePrevBtn);
    remoteNavLay->addWidget(m_remoteNextBtn);
    remoteNavLay->addWidget(m_remoteUpBtn);
    remoteNavLay->addWidget(m_remoteHomeBtn);
    remoteNavLay->addWidget(m_remoteRootBtn);
    remoteNavLay->addWidget(m_remotePathEdit, 1);
    remoteNavLay->addWidget(m_remoteRefreshBtn);
    remoteNavLay->addWidget(m_remoteShowHidden);
    remoteLay->addLayout(remoteNavLay);

    m_remoteStack = new QStackedWidget(this);

    m_remoteFileTable = new QTableWidget(0, 5, this);
    m_remoteFileTable->setHorizontalHeaderLabels({tr("İsim"), tr("Boyut"), tr("Tür"), tr("İzinler"), tr("Değiştirilme Tarihi")});
    m_remoteFileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remoteFileTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteFileTable->setAlternatingRowColors(true);
    m_remoteFileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteFileTable->setContextMenuPolicy(Qt::CustomContextMenu);
    OkidiaUtils::setupTableHeaders(m_remoteFileTable, "remote_file_table", {220, 85, 85, 90, 145}, false);
    m_remoteFileTable->horizontalHeader()->setSortIndicatorShown(true);
    m_remoteFileTable->setSortingEnabled(true);
    if (!m_remoteFileTable->horizontalHeader()->isSortIndicatorShown() || m_remoteFileTable->horizontalHeader()->sortIndicatorSection() < 0) {
        m_remoteFileTable->sortByColumn(4, Qt::DescendingOrder);
        m_remoteFileTable->horizontalHeader()->setSortIndicator(4, Qt::DescendingOrder);
    }
    
    m_remoteIconList = new QListWidget(this);
    m_remoteIconList->setViewMode(QListView::IconMode);
    m_remoteIconList->setResizeMode(QListView::Adjust);
    m_remoteIconList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteIconList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteIconList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_remoteIconList->setMovement(QListView::Static);
    m_remoteIconList->setSpacing(5);

    m_remoteStack->addWidget(m_remoteFileTable);
    m_remoteStack->addWidget(m_remoteIconList);
    m_remoteStack->setCurrentWidget(m_remoteFileTable);

    remoteLay->addWidget(m_remoteStack, 1);
    splitLay->addWidget(remoteGroup, 1);

    mainLay->addLayout(splitLay, 1);

    // ── İlerleme Barı ──
    auto* progressLay = new QHBoxLayout();
    m_progressLbl = new QLabel(this);
    m_progressLbl->setProperty("status", "info");
    m_progressBar = new QProgressBar(this);
    m_progressBar->setFixedHeight(12);
    m_progressBar->setVisible(false);

    progressLay->addWidget(m_progressLbl);
    progressLay->addWidget(m_progressBar, 1);
    mainLay->addLayout(progressLay);

    // ── Bağlantılar ──
    
    // Remote connections
    connect(m_remotePrevBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onRemotePrevClicked);
    connect(m_remoteNextBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onRemoteNextClicked);
    connect(m_remoteHomeBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onRemoteHomeClicked);
    connect(m_remoteRootBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onRemoteRootClicked);
    connect(m_remoteUpBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onRemoteUpClicked);
    connect(m_remoteRefreshBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onRemoteRefreshClicked);
    connect(m_remoteFileTable, &QTableWidget::cellDoubleClicked, this, &SshFileManagerWidget::onRemoteTableDoubleClicked);
    connect(m_remoteIconList, &QListWidget::itemDoubleClicked, this, &SshFileManagerWidget::onRemoteItemDoubleClicked);
    connect(m_remoteFileTable, &QTableWidget::customContextMenuRequested, this, &SshFileManagerWidget::showRemoteContextMenu);
    connect(m_remoteIconList, &QListWidget::customContextMenuRequested, this, &SshFileManagerWidget::showRemoteContextMenu);
    connect(m_remotePathEdit, &QLineEdit::returnPressed, [this]() {
        loadRemoteDirectory(m_remotePathEdit->text().trimmed());
    });
    connect(m_remoteShowHidden, &QCheckBox::toggled, [this]() {
        if (!m_remoteCurrentPath.isEmpty()) {
            loadRemoteDirectory(m_remoteCurrentPath);
        }
    });

    // Local connections
    connect(m_localPrevBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onLocalPrevClicked);
    connect(m_localNextBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onLocalNextClicked);
    connect(m_localHomeBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onLocalHomeClicked);
    connect(m_localRootBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onLocalRootClicked);
    connect(m_localUpBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onLocalUpClicked);
    connect(m_localRefreshBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onLocalRefreshClicked);
    connect(m_localFileTable, &QTableWidget::cellDoubleClicked, this, &SshFileManagerWidget::onLocalTableDoubleClicked);
    connect(m_localIconList, &QListWidget::itemDoubleClicked, this, &SshFileManagerWidget::onLocalItemDoubleClicked);
    connect(m_localFileTable, &QTableWidget::customContextMenuRequested, this, &SshFileManagerWidget::showLocalContextMenu);
    connect(m_localIconList, &QListWidget::customContextMenuRequested, this, &SshFileManagerWidget::showLocalContextMenu);
    connect(m_localPathEdit, &QLineEdit::returnPressed, [this]() {
        loadLocalDirectory(m_localPathEdit->text().trimmed());
    });
    connect(m_localShowHidden, &QCheckBox::toggled, [this]() {
        if (!m_localCurrentPath.isEmpty()) {
            loadLocalDirectory(m_localCurrentPath);
        }
    });

    // Transfer connections
    connect(m_transferToRemoteBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onTransferToRemoteClicked);
    connect(m_transferToLocalBtn, &QPushButton::clicked, this, &SshFileManagerWidget::onTransferToLocalClicked);
}

void SshFileManagerWidget::onChangeViewMode(int mode) {
    m_viewMode = static_cast<ViewMode>(mode);
    
    if (m_viewMode == Details) {
        m_remoteStack->setCurrentWidget(m_remoteFileTable);
        m_localStack->setCurrentWidget(m_localFileTable);
    } else {
        m_remoteStack->setCurrentWidget(m_remoteIconList);
        m_localStack->setCurrentWidget(m_localIconList);
        
        int iconSize = (m_viewMode == SmallIcons) ? 48 : 96;
        int gridSize = (m_viewMode == SmallIcons) ? 80 : 120;
        
        m_remoteIconList->setIconSize(QSize(iconSize, iconSize));
        m_remoteIconList->setGridSize(QSize(gridSize, gridSize + 20));
        
        m_localIconList->setIconSize(QSize(iconSize, iconSize));
        m_localIconList->setGridSize(QSize(gridSize, gridSize + 20));
    }
    
    renderRemoteFiles();
    renderLocalFiles();
}

void SshFileManagerWidget::setUiEnabled(bool enabled) {
    m_remotePrevBtn->setEnabled(enabled);
    m_remoteNextBtn->setEnabled(enabled);
    m_remoteHomeBtn->setEnabled(enabled);
    m_remoteRootBtn->setEnabled(enabled);
    m_remoteUpBtn->setEnabled(enabled);
    m_remotePathEdit->setEnabled(enabled);
    m_remoteRefreshBtn->setEnabled(enabled);
    m_remoteFileTable->setEnabled(enabled);
    
    m_localPrevBtn->setEnabled(enabled);
    m_localNextBtn->setEnabled(enabled);
    m_localHomeBtn->setEnabled(enabled);
    m_localRootBtn->setEnabled(enabled);
    m_localUpBtn->setEnabled(enabled);
    m_localPathEdit->setEnabled(enabled);
    m_localRefreshBtn->setEnabled(enabled);
    m_localFileTable->setEnabled(enabled);
    
    m_transferToRemoteBtn->setEnabled(enabled);
    m_transferToLocalBtn->setEnabled(enabled);
}

void SshFileManagerWidget::setConfig(const SshClient::Config& config) {
    m_config = config;
}

void SshFileManagerWidget::connectToHost(const SshClient::Config& config) {
    disconnectFromHost();
    m_config = config;
    
    QString hostname = property("hostname").toString();
    QString ip = property("ip").toString();
    if (m_remoteHeaderLbl) {
        if (!hostname.isEmpty() || !ip.isEmpty()) {
            m_remoteHeaderLbl->setText(QString(tr("<b>Uzak Dosyalar (%1 - %2)</b>")).arg(hostname, ip));
        } else {
            m_remoteHeaderLbl->setText(QString(tr("<b>Uzak Dosyalar (%1)</b>")).arg(config.host));
        }
    }

    m_client->connectToHost(m_config);
}

void SshFileManagerWidget::connectToAgent(const QString& clientId) {
    if (!m_session) return;
    
    SshClient::Config cfg;
    cfg.host = "";
    cfg.port = 22;
    cfg.connectTimeoutMs = 4000;
    cfg.hostKeyCheck = false;

    QString ip;
    QString hostname;
    for (const auto& agent : m_session->allClientStates()) {
        if (agent.clientId == clientId || agent.machineId == clientId) {
            ip = agent.ip;
            hostname = agent.hostname;
            cfg.host = ip;
            break;
        }
    }
    
    if (ip.isEmpty()) {
        QMessageBox::critical(this, tr("Bağlantı Hatası"), tr("İstemci IP adresi bulunamadı."));
        return;
    }

    QString user, pass;
    bool isKey = false;
    bool found = false;

    const auto brokerCreds = m_session->sshCredentials();
    for (const auto& c : brokerCreds) {
        if ((c.targetId == ip || c.targetId == clientId || c.machineId == clientId) && c.targetType == "agent") {
            user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
        }
    }

    if (found) {
        cfg.username = user;
        if (isKey) {
            cfg.authMethod = SshClient::AuthMethod::PrivateKey;
            cfg.privateKeyPath = pass;
            cfg.password.clear();
        } else {
            cfg.authMethod = SshClient::AuthMethod::Password;
            cfg.password = pass;
            cfg.privateKeyPath.clear();
        }
    } else {
        cfg.username = "root";
        cfg.authMethod = SshClient::AuthMethod::Password;
        cfg.password = "";
    }

    connectToHost(cfg);
    if (m_remoteHeaderLbl && !hostname.isEmpty()) {
        m_remoteHeaderLbl->setText(QString("<b>Uzak Dosyalar (%1)</b>").arg(hostname));
    }
}

void SshFileManagerWidget::disconnectFromHost() {
    if (m_client->isConnected()) {
        m_client->disconnectFromHost();
    }
}

bool SshFileManagerWidget::isConnected() const {
    return m_client && m_client->isConnected();
}

void SshFileManagerWidget::updateProgress(qint64 sent, qint64 total, const QString& operation) {
    if (total > 0) {
        m_progressBar->setMaximum(100);
        int val = static_cast<int>(sent * 100 / total);
        m_progressBar->setValue(val);
        m_progressLbl->setText(QString("%1: %2%").arg(operation).arg(val));
    } else {
        m_progressBar->setMaximum(0);
        m_progressLbl->setText(QString("%1: %2 bayt").arg(operation).arg(sent));
    }
}
