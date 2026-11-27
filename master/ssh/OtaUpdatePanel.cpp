#include "OtaUpdatePanel.h"
#include "GitHubFetcher.h"
#include "BrokerUpdateManagerDialog.h"
#include "PackageRequirementSolver.h"
#include "../ui/DownloadManagerDialog.h"
#include "../ui/LogViewerDialog/LogViewerDialog.h"
#include "HostInstallCard.h"
#include "DeployWorker.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../audit/AuditLog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileInfo>
#include <QSplitter>
#include <QFileDialog>
#include <QStackedWidget>
#include <QProcess>
#include <QRegularExpression>
#include <QMenu>
#include <QTimer>
#include <QDateTime>

OtaUpdatePanel::OtaUpdatePanel(SessionManager* session, QWidget* parent)
    : QWidget(parent), m_session(session) {
    setupUi();
    restoreHistoryAndReconcile();
    refreshAgentList();
    if (m_session) {
        connect(m_session, &SessionManager::otaBrokerReportReceived, this, &OtaUpdatePanel::onBrokerReport);
        connect(m_session, &SessionManager::clientStateChanged, this, &OtaUpdatePanel::onClientStateChanged);
        connect(m_session, &SessionManager::databaseReloaded, this, &OtaUpdatePanel::refreshAgentList);
        connect(m_session, &SessionManager::clientConnected, this, [this](const QString& clientId, const ClientState& state) {
            refreshAgentList();
            onClientStateChanged(clientId, state);
        });
        connect(m_session, &SessionManager::clientDisconnected, this, [this](const QString& clientId) {
            refreshAgentList();
            onClientDisconnected(clientId);
        });
        connect(m_session, &SessionManager::dbMasterDeviceDeleteResult, this, [this](bool success, const QString& errorMsg) {
            if (!success) {
                QMessageBox::warning(this, tr("Silme Hatası"),
                    errorMsg.isEmpty() ? tr("Master bilgisayar kaydı silinemedi.") : errorMsg);
            }
        });
        connect(m_session, &SessionManager::brokerDisconnected, this, [this]() {
            if (m_statusLabel && !m_statusLabel->text().contains(tr("yeniden başlatılıyor"), Qt::CaseInsensitive)) {
                m_statusLabel->setText(tr("Sunucu bağlantısı kesildi — yeniden bağlanılıyor..."));
            }
        });
        connect(m_session, &SessionManager::switchedToOnlineMode, this, [this]() {
            refreshAgentList();
            if (m_statusLabel) {
                m_statusLabel->setText(tr("Sunucu bağlantısı yeniden kuruldu. Paket listesi güncellendi."));
            }
        });
    }
}

OtaUpdatePanel::~OtaUpdatePanel() {}

void OtaUpdatePanel::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    mainLay->addWidget(splitter);

    // Sol taraf: Ajan Listesi & Grid Görünümü (Stacked)
    auto* leftWidget = new QWidget(splitter);
    auto* leftLay = new QVBoxLayout(leftWidget);
    leftLay->setContentsMargins(0, 0, 0, 0);

    auto* topLay = new QHBoxLayout();
    topLay->addWidget(new QLabel(tr("<b>Kayıtlı Tüm Bilgisayarlar</b>"), this));
    
    m_backBtn = new QPushButton(tr("Geri Dön"), this);
    m_backBtn->setIcon(ThemeManager::icon(":/ui/arrow_left.svg"));
    m_backBtn->setProperty("class", "primary");
    m_backBtn->setVisible(false);

    m_showGridBtn = new QPushButton(tr("Yükleme Durumu / Günlükler"), this);
    m_showGridBtn->setIcon(ThemeManager::icon(":/ui/monitor.svg"));
    m_showGridBtn->setProperty("class", "secondary");
    m_showGridBtn->setVisible(false);

    m_clearHistoryBtn = new QPushButton(tr("Günlükleri Temizle"), this);
    m_clearHistoryBtn->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    m_clearHistoryBtn->setProperty("class", "danger");
    m_clearHistoryBtn->setToolTip(tr("Kayıtlı OTA güncelleme günlüklerini ve geçmiş durum kartlarını yerel diskten siler."));
    m_clearHistoryBtn->setVisible(false);

    m_refreshBtn = new QPushButton(tr("Listeyi Yenile"), this);
    m_refreshBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    
    topLay->addStretch();
    topLay->addWidget(m_backBtn);
    topLay->addWidget(m_showGridBtn);
    topLay->addWidget(m_clearHistoryBtn);
    topLay->addWidget(m_refreshBtn);
    leftLay->addLayout(topLay);

    m_leftStacked = new QStackedWidget(leftWidget);

    m_agentTable = new QTableWidget(0, 6, m_leftStacked);
    m_agentTable->setHorizontalHeaderLabels({"", tr("Bilgisayar Adı"), tr("Sürüm"), tr("İşletim Sistemi"), tr("Mimari"), tr("Kurulacak Paket")});
    m_agentTable->horizontalHeaderItem(0)->setIcon(ThemeManager::icon(":/ui/network.svg"));
    OkidiaUtils::setupTableHeaders(m_agentTable, "ota_agent_table", {40, 150, 90, 130, 90, 180}, true);
    m_agentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_agentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_agentTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_leftStacked->addWidget(m_agentTable); // Page 0

    m_progressGrid = new DeployProgressGrid(m_leftStacked);
    m_progressGrid->setStorageKey("ota_update_history");
    m_leftStacked->addWidget(m_progressGrid); // Page 1

    m_otaTimeoutTimer = new QTimer(this);
    m_otaTimeoutTimer->setInterval(5000);
    connect(m_otaTimeoutTimer, &QTimer::timeout, this, &OtaUpdatePanel::onOtaTimeoutCheck);

    leftLay->addWidget(m_leftStacked);
    splitter->addWidget(leftWidget);

    // Sağ taraf: Kontroller
    auto* rightWidget = new QWidget(splitter);
    auto* rightLay = new QVBoxLayout(rightWidget);
    rightLay->setContentsMargins(10, 0, 0, 0);

    rightLay->addWidget(new QLabel(tr("<b>1. İstemci / Sunucu Paketi Seç</b>"), this));
    
    auto* btnLay = new QHBoxLayout();
    m_githubBtn = new QPushButton(tr(" Akıllı Paket İndir (okidia.net)"), this);
    m_githubBtn->setIcon(ThemeManager::icon(":/ui/checked.svg"));
    m_githubBtn->setProperty("class", "primary");
    m_githubBtn->setMinimumHeight(40);
    btnLay->addWidget(m_githubBtn);

    m_localFolderBtn = new QPushButton(tr(" Klasör Seç"), this);
    m_localFolderBtn->setIcon(ThemeManager::icon(":/ui/folder.svg"));
    m_localFolderBtn->setProperty("class", "secondary");
    m_localFolderBtn->setMinimumHeight(40);
    btnLay->addWidget(m_localFolderBtn);

    m_localDirBtn = new QPushButton(tr(" Dosya Seç"), this);
    m_localDirBtn->setIcon(ThemeManager::icon(":/ui/file.svg"));
    m_localDirBtn->setProperty("class", "secondary");
    m_localDirBtn->setMinimumHeight(40);
    btnLay->addWidget(m_localDirBtn);

    rightLay->addLayout(btnLay);

    m_selectedPackagesLabel = new QLabel(tr("Henüz paket seçilmedi."), this);
    m_selectedPackagesLabel->setWordWrap(true);
    rightLay->addWidget(m_selectedPackagesLabel);

    rightLay->addSpacing(20);

    rightLay->addWidget(new QLabel(tr("<b>2. Sunucuya Paketleri Gönder</b>"), this));
    auto* infoLbl = new QLabel(tr("Seçilen paketler güvenli TLS kanalı üzerinden Sunucu'nun (Broker) güncelleme deposuna (OTA - Over-The-Air) aktarılacak; istemciler açıkken veya açıldıklarında otonom olarak güncellenecektir."), this);
    infoLbl->setWordWrap(true);
    rightLay->addWidget(infoLbl);
    
    m_sendToBrokerBtn = new QPushButton(tr(" Sunucuya Aktar"), this);
    m_sendToBrokerBtn->setIcon(ThemeManager::icon(":/ui/upload.svg"));
    m_sendToBrokerBtn->setProperty("class", "danger");
    m_sendToBrokerBtn->setMinimumHeight(40);
    m_sendToBrokerBtn->setEnabled(false);
    rightLay->addWidget(m_sendToBrokerBtn);

    m_updateBrokerBtn = new QPushButton(tr(" Sunucuyu Güncelle"), this);
    m_updateBrokerBtn->setIcon(ThemeManager::icon(":/ui/root_drive.svg"));
    m_updateBrokerBtn->setProperty("class", "danger");
    m_updateBrokerBtn->setMinimumHeight(40);
    m_updateBrokerBtn->setVisible(false);
    rightLay->addWidget(m_updateBrokerBtn);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    rightLay->addWidget(m_progressBar);

    m_statusLabel = new QLabel("", this);
    rightLay->addWidget(m_statusLabel);

    rightLay->addStretch();
    
    rightLay->addWidget(new QLabel(tr("<b>3. Sunucu Güncelleme Deposunu Yönet</b>"), this));
    auto* manageInfoLbl = new QLabel(tr("Sunucu üzerinde birikmiş olan eski OTA paketlerini temizleyebilirsiniz."), this);
    manageInfoLbl->setWordWrap(true);
    rightLay->addWidget(manageInfoLbl);
    
    m_manageBrokerBtn = new QPushButton(tr(" Sunucu Güncelleme Deposunu Aç"), this);
    m_manageBrokerBtn->setIcon(ThemeManager::icon(":/ui/folder.svg"));
    m_manageBrokerBtn->setProperty("class", "secondary");
    m_manageBrokerBtn->setMinimumHeight(40);
    rightLay->addWidget(m_manageBrokerBtn);
    
    splitter->addWidget(rightWidget);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    connect(m_leftStacked, &QStackedWidget::currentChanged, this, [this](int) {
        updateTopButtonsVisibility();
    });

    connect(m_refreshBtn, &QPushButton::clicked, this, &OtaUpdatePanel::refreshAgentList);
    connect(m_showGridBtn, &QPushButton::clicked, this, [this]() {
        if (m_leftStacked) m_leftStacked->setCurrentIndex(1);
    });
    connect(m_clearHistoryBtn, &QPushButton::clicked, this, [this]() {
        if (m_progressGrid) {
            m_progressGrid->clearHistory(true);
        }
    });
    connect(m_backBtn, &QPushButton::clicked, this, [this]() {
        if (m_leftStacked) m_leftStacked->setCurrentIndex(0);
    });
    connect(m_progressGrid, &DeployProgressGrid::backToTableRequested, this, [this]() {
        if (m_leftStacked) m_leftStacked->setCurrentIndex(0);
    });
    connect(m_progressGrid, &DeployProgressGrid::historyCleared, this, [this]() {
        m_targetVersions.clear();
        m_machineIdToIp.clear();
        m_cardByMachineId.clear();
        m_cardTargetVersions.clear();
        m_cardActiveStartSecs.clear();
        m_offlineCards.clear();
        if (m_otaTimeoutTimer) m_otaTimeoutTimer->stop();
        updateTopButtonsVisibility();
    });
    connect(m_progressGrid, &DeployProgressGrid::remoteLogRequested,
            this, &OtaUpdatePanel::openRemoteAgentLog);
    connect(m_agentTable, &QTableWidget::customContextMenuRequested,
            this, &OtaUpdatePanel::onTableContextMenu);
    connect(m_githubBtn, &QPushButton::clicked, this, &OtaUpdatePanel::onFetchFromGithub);
    connect(m_localFolderBtn, &QPushButton::clicked, this, &OtaUpdatePanel::onSelectLocalFolder);
    connect(m_localDirBtn, &QPushButton::clicked, this, &OtaUpdatePanel::onSelectLocalPackages);
    connect(m_sendToBrokerBtn, &QPushButton::clicked, this, &OtaUpdatePanel::onSendToBroker);
    connect(m_updateBrokerBtn, &QPushButton::clicked, this, &OtaUpdatePanel::onUpdateBrokerClicked);
    connect(m_manageBrokerBtn, &QPushButton::clicked, this, &OtaUpdatePanel::onManageBrokerClicked);
}

void OtaUpdatePanel::refreshAgentList() {
    if (!m_session) return;
    
    QList<AgentRecord> allAgents;
    if (OkidiaDatabase::instance().isOpen()) {
        allAgents = OkidiaDatabase::instance().allAgents();
    }
    
    // Aktif session'ları (online olanları) bir hash haritasında toplayalım
    QList<ClientState> activeStates = m_session->allClientStates();
    QHash<QString, ClientState> onlineMap;
    for (const ClientState& st : activeStates) {
        onlineMap[st.machineId] = st;
    }

    // Kayıtlı Master Cihazları
    QList<MasterDeviceRecord> allMasterDevices;
    if (OkidiaDatabase::instance().isOpen()) {
        allMasterDevices = OkidiaDatabase::instance().allMasterDevices();
    }

    // Bu bilgisayar (Self): Hostname bazında takip edilir (klonlanmış VM'lerde /etc/machine-id aynı olabileceği için).
    QString myHostName = QSysInfo::machineHostName().trimmed();
    QString myMachineId = QUuid::createUuidV5(QUuid(), "master-host:" + myHostName.toLower()).toString(QUuid::WithoutBraces);
    QString myOsTag = OkidiaUtils::detectLocalOsTag();
    QString myOsArch = OkidiaUtils::detectLocalOsArch();
    QString myOsPretty = OkidiaUtils::detectLocalOsPrettyName();
    QString myVer = Okidia::Version::string();
    QString activeUsername = m_session ? m_session->activeUser().username : "";

    bool selfFound = false;
    for (auto& md : allMasterDevices) {
        if (md.hostname.trimmed().compare(myHostName, Qt::CaseInsensitive) == 0) {
            selfFound = true;
            // Yerel bilgisayarın sürüm veya OS bilgisi güncellendiyse yerel DB'yi de senkron tut
            if (md.masterVer != myVer || md.osTag != myOsTag || md.osArch != myOsArch) {
                md.masterVer = myVer;
                md.osTag = myOsTag;
                md.osArch = myOsArch;
                md.osPrettyName = myOsPretty;
                if (!activeUsername.isEmpty()) md.lastUsername = activeUsername;
                md.lastSeen = OkidiaUtils::nowSecs();
                if (OkidiaDatabase::instance().isOpen()) {
                    OkidiaDatabase::instance().upsertMasterDevice(md);
                }
            }
            break;
        }
    }
    if (!selfFound) {
        MasterDeviceRecord selfDev;
        selfDev.machineId = myMachineId;
        selfDev.hostname = myHostName;
        selfDev.masterVer = myVer;
        selfDev.osTag = myOsTag;
        selfDev.osArch = myOsArch;
        selfDev.osPrettyName = myOsPretty;
        selfDev.lastUsername = activeUsername;
        selfDev.lastSeen = OkidiaUtils::nowSecs();
        allMasterDevices.prepend(selfDev);
    }

    // Her kullanıcı adının en son hangi bilgisayarda (hostname) görüldüğünü haritala
    // (Aynı kullanıcı birden fazla bilgisayarda oturum açtığında yalnızca en son bağlandığı bilgisayar çevrimiçi olabilir)
    QHash<QString, QString> latestHostForUser;
    QHash<QString, qint64> latestSeenForUser;
    for (const auto& md : allMasterDevices) {
        QString uLower = md.lastUsername.trimmed().toLower();
        if (uLower.isEmpty()) continue;
        if (!latestSeenForUser.contains(uLower) || md.lastSeen >= latestSeenForUser[uLower]) {
            latestSeenForUser[uLower] = md.lastSeen;
            latestHostForUser[uLower] = md.hostname.trimmed().toLower();
        }
    }

    // Toplam satır sayısı: 1 Sunucu (Broker) + Kayıtlı Master Bilgisayarları + Kayıtlı İstemciler
    m_agentTable->clearContents();
    m_agentTable->setRowCount(1 + allMasterDevices.size() + allAgents.size());

    // ── 0. SATIR: BROKER SUNUCUSU (EN ÜSTTE) ──────────────────────────
    bool isBrokerOnline = m_session->isConnected();
    QString brokerHost = (m_session->tlsClient()) ? m_session->tlsClient()->host() : "";
    QString brokerVer = m_session->brokerVersion();
    if (brokerVer.isEmpty()) brokerVer = "-";
    QString brokerOsPretty = m_session->brokerOs();
    QString brokerOsTag = OkidiaUtils::normalizeOsTag(m_session->brokerOsTag());
    if (brokerOsTag.isEmpty() || brokerOsTag == "unknown") {
        brokerOsTag = OkidiaUtils::normalizeOsTag(brokerOsPretty);
    }
    QString brokerArch = OkidiaUtils::normalizeArch(m_session->brokerArch());
    if (brokerArch.isEmpty() || brokerArch == "unknown") brokerArch = "amd64";

    QString brokerOsDisplay = brokerOsPretty.isEmpty() ? tr("Linux (Sunucu)") : OkidiaUtils::cleanOsName(brokerOsPretty);

    OkidiaTableItem* srvStatusItem = new OkidiaTableItem();
    srvStatusItem->setIcon(ThemeManager::icon(isBrokerOnline ? ":/ui/root_drive.svg" : ":/ui/offline.svg"));
    srvStatusItem->setText(isBrokerOnline ? tr("Sunucu (Aktif)") : tr("Sunucu (Bağlantı Yok)"));
    srvStatusItem->setTextAlignment(Qt::AlignCenter);

    QString srvNameText = QString("★ %1 %2").arg(tr("Broker Sunucusu"), brokerHost.isEmpty() ? "" : "(" + brokerHost + ")").trimmed();
    OkidiaTableItem* srvHostItem = new OkidiaTableItem(srvNameText);
    srvHostItem->setData(Qt::UserRole, "broker_server");
    
    // Broker tüm IP adresleri tooltip'i
    QStringList allIps = m_session->brokerAllIps();
    if (allIps.isEmpty() && !brokerHost.isEmpty()) {
        allIps.append(brokerHost);
    }
    if (!allIps.isEmpty()) {
        srvHostItem->setToolTip(tr("Sunucu Aktif IP Adresleri:\n%1").arg(allIps.join(", ")));
    }

    OkidiaTableItem* srvVerItem = new OkidiaTableItem(brokerVer);
    OkidiaTableItem* srvOsItem = new OkidiaTableItem(brokerOsDisplay);
    OkidiaTableItem* srvArchItem = new OkidiaTableItem(brokerArch);
    srvArchItem->setData(Qt::UserRole, "broker:" + brokerOsTag);
    OkidiaTableItem* srvPackageItem = new OkidiaTableItem("-");

    QList<QTableWidgetItem*> srvRowItems = {srvStatusItem, srvHostItem, srvVerItem, srvOsItem, srvArchItem, srvPackageItem};
    QColor serverBg(230, 126, 34, 38); // Dikkat çekici sıcak amber/turuncu dolgu
    for (int col = 0; col < srvRowItems.size(); ++col) {
        QTableWidgetItem* item = srvRowItems[col];
        item->setBackground(QBrush(serverBg));
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        m_agentTable->setItem(0, col, item);
    }

    // ── 1..M SATIRLARI: KAYITLI MASTER BİLGİSAYARLARI (ORTADA) ────────
    int currentRow = 1;
    QColor masterBg(52, 152, 219, 25); // Zarif açık mavi dolgu

    for (int m = 0; m < allMasterDevices.size(); ++m) {
        const MasterDeviceRecord& mdev = allMasterDevices[m];
        // Her Master bilgisayarı kesin olarak hostname bilgisine göre ayırt edilir (kullanıcı adına veya klon machineId'ye göre değil)
        bool isSelf = (mdev.hostname.trimmed().compare(myHostName, Qt::CaseInsensitive) == 0);
        bool isOnline = isSelf;
        if (!isSelf && !mdev.lastUsername.isEmpty()) {
            QString uLower = mdev.lastUsername.trimmed().toLower();
            // Eğer aynı kullanıcı şu an bu bilgisayarda (isSelf) açıksa, diğer bilgisayar çevrimiçi değildir
            if (uLower != activeUsername.trimmed().toLower() &&
                m_session->isMasterOnline("master:" + mdev.lastUsername) &&
                latestHostForUser.value(uLower) == mdev.hostname.trimmed().toLower()) {
                isOnline = true;
            }
        }

        OkidiaTableItem* mStatusItem = new OkidiaTableItem();
        mStatusItem->setIcon(ThemeManager::icon(isOnline ? ":/ui/online.svg" : ":/ui/offline.svg"));
        mStatusItem->setText(isOnline ? tr("Çevrimiçi") : tr("Çevrimdışı"));
        mStatusItem->setTextAlignment(Qt::AlignCenter);

        QString hostText = QString("◆ %1").arg(mdev.hostname);
        if (!mdev.lastUsername.isEmpty()) {
            hostText += QString(" [%1]").arg(mdev.lastUsername);
        }
        if (isSelf) {
            hostText += tr(" (Bu Bilgisayar)");
        }
        OkidiaTableItem* mHostItem = new OkidiaTableItem(hostText);
        mHostItem->setData(Qt::UserRole, mdev.hostname);
        mHostItem->setData(Qt::UserRole + 1, mdev.machineId);
        mHostItem->setData(Qt::UserRole + 2, isSelf);
        if (mdev.lastSeen > 0) {
            QString dateStr = QDateTime::fromSecsSinceEpoch(mdev.lastSeen).toString("dd.MM.yyyy HH:mm");
            mHostItem->setToolTip(tr("Bilgisayar Adı (Hostname): %1\nSon Görülme / Giriş: %2\nIP: %3\nSon Kullanıcı: %4")
                                      .arg(mdev.hostname, dateStr, mdev.ip, mdev.lastUsername));
        }

        // Sürüm bilgisi: Yalnızca bu bilgisayar (hostname == myHostName) ise yerel sürüm,
        // diğer tüm Master bilgisayarları için o bilgisayarın (hostname) veritabanındaki kendi sürümü!
        QString mVer = isSelf ? myVer : (mdev.masterVer.isEmpty() ? "-" : mdev.masterVer);
        OkidiaTableItem* mVerItem = new OkidiaTableItem(mVer);

        // İşletim sistemi ve mimari bilgisi
        QString mOsDisplay = mdev.osPrettyName.isEmpty() ? tr("Linux (Master)") : OkidiaUtils::cleanOsName(mdev.osPrettyName);
        QString prettyDerived = OkidiaUtils::normalizeOsTag(mdev.osPrettyName);
        QString mOsTag = (prettyDerived == "debian12" || prettyDerived == "debian13" ||
                          prettyDerived == "debian11" || prettyDerived == "debian10" ||
                          prettyDerived == "ubuntu24" || prettyDerived == "ubuntu22" ||
                          prettyDerived == "ubuntu20" || prettyDerived == "windows")
                         ? prettyDerived
                         : OkidiaUtils::normalizeOsTag(mdev.osTag);
        if (mOsTag.isEmpty() || mOsTag == "unknown") mOsTag = "debian12";
        QString mOsArch = mdev.osArch.isEmpty() ? "amd64" : OkidiaUtils::normalizeArch(mdev.osArch);

        if (isSelf) {
            mOsTag = myOsTag;
            mOsArch = myOsArch;
            mOsDisplay = myOsPretty;
        }

        OkidiaTableItem* mOsItem = new OkidiaTableItem(mOsDisplay);
        OkidiaTableItem* mArchItem = new OkidiaTableItem(mOsArch);
        mArchItem->setData(Qt::UserRole, "master:" + mOsTag);

        OkidiaTableItem* mPackageItem = new OkidiaTableItem("-");

        QList<QTableWidgetItem*> mRowItems = {mStatusItem, mHostItem, mVerItem, mOsItem, mArchItem, mPackageItem};
        for (int col = 0; col < mRowItems.size(); ++col) {
            QTableWidgetItem* itm = mRowItems[col];
            itm->setBackground(QBrush(masterBg));
            if (isSelf) {
                QFont f = itm->font();
                f.setBold(true);
                itm->setFont(f);
            }
            m_agentTable->setItem(currentRow, col, itm);
        }
        currentRow++;
    }

    // ── M+1..N SATIRLARI: KAYITLI İSTEMCİLER (EN ALTTA) ───────────────
    for (int i = 0; i < allAgents.size(); ++i) {
        int row = currentRow++;
        const AgentRecord& rec = allAgents[i];
        bool isOnline = onlineMap.contains(rec.machineId) && onlineMap[rec.machineId].connected;
        
        OkidiaTableItem* statusItem = new OkidiaTableItem();
        statusItem->setIcon(ThemeManager::icon(isOnline ? ":/ui/online.svg" : ":/ui/offline.svg"));
        statusItem->setText(isOnline ? tr("Çevrimiçi") : tr("Çevrimdışı"));
        statusItem->setTextAlignment(Qt::AlignCenter);

        OkidiaTableItem* hostItem = new OkidiaTableItem(rec.hostname);
        hostItem->setData(Qt::UserRole, rec.machineId);
        
        // Versiyon ve OS bilgisini aktif durumdan, yoksa DB'den al
        QString agentVer = isOnline ? onlineMap[rec.machineId].agentVersion : rec.agentVer;
        OkidiaTableItem* verItem = new OkidiaTableItem(agentVer);
        
        QString osStr = isOnline ? onlineMap[rec.machineId].osPrettyName
                                 : (rec.osPrettyName.isEmpty() ? rec.osInfo : rec.osPrettyName);
        QString rawTag = isOnline ? onlineMap[rec.machineId].osTag : rec.osInfo;
        QString prettyDerived = OkidiaUtils::normalizeOsTag(osStr);
        QString realOsTag = (prettyDerived == "debian12" || prettyDerived == "debian13" ||
                             prettyDerived == "debian11" || prettyDerived == "debian10" ||
                             prettyDerived == "ubuntu24" || prettyDerived == "ubuntu22" ||
                             prettyDerived == "ubuntu20" || prettyDerived == "windows")
                            ? prettyDerived
                            : OkidiaUtils::normalizeOsTag(rawTag);

        if (osStr.isEmpty()) osStr = realOsTag;
        osStr = OkidiaUtils::cleanOsName(osStr);
        
        QString osArch = isOnline ? onlineMap[rec.machineId].osArch : rec.osArch;
        osArch = OkidiaUtils::normalizeArch(osArch);
        if (osArch.isEmpty() || osArch == "unknown") osArch = "amd64";
        
        OkidiaTableItem* osItem = new OkidiaTableItem(osStr);
        OkidiaTableItem* archItem = new OkidiaTableItem(osArch);
        archItem->setData(Qt::UserRole, realOsTag);
        
        OkidiaTableItem* packageItem = new OkidiaTableItem("-");

        m_agentTable->setItem(row, 0, statusItem);
        m_agentTable->setItem(row, 1, hostItem);
        m_agentTable->setItem(row, 2, verItem);
        m_agentTable->setItem(row, 3, osItem);
        m_agentTable->setItem(row, 4, archItem);
        m_agentTable->setItem(row, 5, packageItem);

        // Eğer geçmişten yüklenen veya bekleyen bir kart varsa ve istemci bu arada hedef sürüme güncellendiyse kartı güncelle
        if (!rec.machineId.isEmpty() && m_cardByMachineId.contains(rec.machineId)) {
            HostInstallCard* c = m_cardByMachineId.value(rec.machineId);
            QString tVer = m_cardTargetVersions.value(c, c ? c->targetVersion() : QString());
            if (c && !c->isSuccess() && !agentVer.isEmpty() && agentVer != "-" && !tVer.isEmpty() &&
                OkidiaUtils::compareVersions(agentVer, tVer) >= 0) {
                m_offlineCards.remove(c);
                m_cardActiveStartSecs.remove(c);
                c->onLogMessage(c->ip(), tr("İstemcinin yeni sürüme (%1) güncellendiği doğrulandı.").arg(agentVer));
                c->onFinished(c->ip(), true, agentVer,
                              tr("İstemci OTA paketini başarıyla kurdu. Güncel sürüm: %1").arg(agentVer));
                if (m_progressGrid) m_progressGrid->updateSummary();
            }
        }
    }
    
    updatePackageMappings();
    updateTopButtonsVisibility();
}

void OtaUpdatePanel::onFetchFromGithub() {
    QList<TargetNode> nodes;

    // Tablodaki tüm satırları (0: Broker, 1..M: Master, M+1..N: Agent) gerçek rol ve osTag bilgileriyle ekle
    for (int row = 0; row < m_agentTable->rowCount(); ++row) {
        auto* hostItem = m_agentTable->item(row, 1);
        auto* archItem = m_agentTable->item(row, 4);
        if (!archItem) continue;

        QString dataStr = archItem->data(Qt::UserRole).toString();
        QString role = "agent";
        QString osTag = dataStr;
        if (dataStr.startsWith("broker:")) {
            role = "broker";
            osTag = dataStr.mid(7).trimmed();
        } else if (dataStr.startsWith("master:")) {
            role = "master";
            osTag = dataStr.mid(7).trimmed();
        } else {
            osTag = dataStr.trimmed();
        }
        osTag = OkidiaUtils::normalizeOsTag(osTag);
        if (osTag.isEmpty() || osTag == "unknown") continue;

        TargetNode node;
        node.id = hostItem ? hostItem->text() : QString::number(row);
        node.name = node.id;
        node.osTag = osTag;
        node.arch = OkidiaUtils::normalizeArch(archItem->text());
        node.role = role;
        nodes.append(node);
    }

    QStringList searchDirs = { PackageRequirementSolver::defaultPackageCacheDir() };
    for (const QString& p : m_selectedPackagePaths) {
        QString pDir = QFileInfo(p).absolutePath();
        if (!searchDirs.contains(pDir)) searchDirs.append(pDir);
    }

    QList<PackageRequirement> reqs = PackageRequirementSolver::solve(nodes, {}, searchDirs);

    auto* dlg = new DownloadManagerDialog(reqs, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DownloadManagerDialog::packagesReady, this, &OtaUpdatePanel::onPackagesDownloaded);
    dlg->exec();
}

void OtaUpdatePanel::onSelectLocalFolder() {
    QString dirPath = OkidiaUtils::getExistingDirectory(
        this,
        tr("Güncelleme Paketlerinin Bulunduğu Klasörü Seçin"),
        "ota_packages",
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks | QFileDialog::DontUseNativeDialog
    );
    if (dirPath.isEmpty()) return;

    QDir dir(dirPath);
    QFileInfoList fileList = dir.entryInfoList({"*.deb", "*.exe"}, QDir::Files);
    if (fileList.isEmpty()) {
        QMessageBox::warning(this, tr("Paket Bulunamadı"),
            tr("Seçilen klasörde uygun kurulum paketi (.deb veya .exe) bulunamadı."));
        return;
    }

    QStringList paths;
    for (const QFileInfo& fi : fileList) {
        paths.append(fi.absoluteFilePath());
    }
    onPackagesDownloaded(paths);
}

void OtaUpdatePanel::onSelectLocalPackages() {
    QStringList files = OkidiaUtils::getOpenFileNames(
        this,
        tr("Güncelleme Paketlerini Seçin"),
        "ota_packages",
        tr("Güncelleme Paketleri (*.deb *.exe);;Debian Paketleri (*.deb);;Windows Paketleri (*.exe);;Tüm Dosyalar (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog
    );
    
    if (!files.isEmpty()) {
        onPackagesDownloaded(files);
    }
}

void OtaUpdatePanel::onPackagesDownloaded(const QStringList& paths) {
    m_selectedPackagePaths = paths;
    m_selectedBrokerPackagePath.clear();

    if (paths.isEmpty()) {
        m_selectedPackagesLabel->setText(tr("Henüz paket seçilmedi."));
        m_sendToBrokerBtn->setEnabled(false);
        m_sendToBrokerBtn->setToolTip("");
        if (m_updateBrokerBtn) m_updateBrokerBtn->setVisible(false);
    }
    
    updatePackageMappings();
}

void OtaUpdatePanel::onSendToBroker() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Hata"), tr("Sunucu (Broker) bağlantısı yok!"));
        return;
    }

    if (m_selectedPackagePaths.isEmpty()) return;

    // Aktif session'ları haritala (machineId bazlı)
    QList<ClientState> activeStates = m_session->allClientStates();
    QHash<QString, ClientState> onlineMap;
    for (const ClientState& st : activeStates) {
        if (!st.machineId.isEmpty()) {
            onlineMap[st.machineId] = st;
        }
    }

    QList<AgentRecord> allAgents;
    QHash<QString, AgentRecord> dbAgentMap;
    if (OkidiaDatabase::instance().isOpen()) {
        allAgents = OkidiaDatabase::instance().allAgents();
        for (const AgentRecord& r : allAgents) {
            dbAgentMap[r.machineId] = r;
        }
    }

    // Grid'i hazırla
    m_progressGrid->clearCards();
    m_targetVersions.clear();
    m_machineIdToIp.clear();
    m_cardByMachineId.clear();
    m_cardTargetVersions.clear();
    m_cardActiveStartSecs.clear();
    m_offlineCards.clear();
    if (m_otaTimeoutTimer) m_otaTimeoutTimer->stop();

    int matchedCardCount = 0;
    QSet<QString> packagesToSendSet;

    // 1. Tablodaki Master ve Agent satırlarına atanan en güncel paketleri topla
    for (int row = 1; row < m_agentTable->rowCount(); ++row) {
        QTableWidgetItem* hostItem = m_agentTable->item(row, 1);
        QTableWidgetItem* verItem = m_agentTable->item(row, 2);
        QTableWidgetItem* osItem = m_agentTable->item(row, 3);
        QTableWidgetItem* archItem = m_agentTable->item(row, 4);
        QTableWidgetItem* packageItem = m_agentTable->item(row, 5);

        if (!hostItem || !packageItem || !archItem) continue;

        QString fullPkgPath = packageItem->data(Qt::UserRole).toString();
        QString assignedPackage = packageItem->text();

        // Master satırlarında eşleşen bir paket varsa (veya klasörde diğer Master'lar için paket varsa)
        // uzaktan SSH/push kartı açılmaz ama paket Broker deposuna yüklenir ki ilgili Master login olduğunda bildirim alsın.
        if (archItem->data(Qt::UserRole).toString().startsWith("master:")) {
            if (!fullPkgPath.isEmpty() && QFile::exists(fullPkgPath)) {
                packagesToSendSet.insert(fullPkgPath);
            }
            continue;
        }

        if (assignedPackage.isEmpty() || assignedPackage == "-" || 
            assignedPackage == tr("Eşleşen Paket Yok") || assignedPackage == tr("Güncelleme Gerekiyor") ||
            assignedPackage == tr("Bilgi Eksik") || assignedPackage == tr("GÜNCEL")) {
            continue;
        }

        if (fullPkgPath.isEmpty()) {
            for (const QString& p : m_selectedPackagePaths) {
                if (QFileInfo(p).fileName() == assignedPackage) {
                    fullPkgPath = p;
                    break;
                }
            }
        }
        if (fullPkgPath.isEmpty()) continue;

        packagesToSendSet.insert(fullPkgPath);

        QString targetVer = OkidiaUtils::extractPackageVersion(assignedPackage);
        if (targetVer.isEmpty()) targetVer = "0.0.0";

        QString hostname = hostItem->text();
        QString machineId = hostItem->data(Qt::UserRole).toString();
        QString ip = "";
        QString currentVer = verItem ? verItem->text() : "";
        QString osStr = osItem ? osItem->text() : "";

        bool isOnline = (!machineId.isEmpty() && onlineMap.contains(machineId) && onlineMap[machineId].connected);

        if (isOnline) {
            ip = onlineMap[machineId].ip;
        } else if (!machineId.isEmpty() && dbAgentMap.contains(machineId)) {
            ip = dbAgentMap[machineId].ip;
        } else {
            for (const AgentRecord& rec : allAgents) {
                if (rec.hostname == hostname) {
                    ip = rec.ip;
                    machineId = rec.machineId;
                    isOnline = (!machineId.isEmpty() && onlineMap.contains(machineId) && onlineMap[machineId].connected);
                    break;
                }
            }
        }
        if (ip.isEmpty()) ip = machineId.isEmpty() ? hostname : machineId;

        m_targetVersions[ip] = targetVer;
        m_targetVersions[hostname] = targetVer;
        if (!machineId.isEmpty()) {
            m_targetVersions[machineId] = targetVer;
            m_machineIdToIp[machineId] = ip;
        }

        // Eğer istemci zaten hedef sürümde veya daha yeni bir sürümdeyse
        if (!currentVer.isEmpty() && currentVer != "0.0.0" && currentVer != "-" &&
            OkidiaUtils::compareVersions(currentVer, targetVer) >= 0) {
            auto* card = new HostInstallCard(ip, hostname, osStr, fullPkgPath, m_progressGrid);
            card->setMachineId(machineId);
            card->setTargetVersion(targetVer);
            m_cardTargetVersions[card] = targetVer;
            if (!machineId.isEmpty()) {
                m_cardByMachineId[machineId] = card;
            }
            card->onFinished(ip, true, currentVer, tr("İstemci zaten en güncel sürümde (%1)").arg(currentVer));
            card->onLogMessage(ip, tr("İstemcinin sürümü (%1) hedef sürümle (%2) aynı veya daha yeni. Güncelleme gerekmiyor.").arg(currentVer, targetVer));
            m_progressGrid->addCard(card);
            matchedCardCount++;
            continue;
        }

        auto* card = new HostInstallCard(ip, hostname, osStr, fullPkgPath, m_progressGrid);
        card->setMachineId(machineId);
        card->setTargetVersion(targetVer);
        m_cardTargetVersions[card] = targetVer;
        if (!machineId.isEmpty()) {
            m_cardByMachineId[machineId] = card;
        }

        card->onLogMessage(ip, tr("Seçilen OTA paketi: %1").arg(assignedPackage));
        card->onLogMessage(ip, tr("Mevcut sürüm: %1 → Hedef sürüm: %2").arg(currentVer.isEmpty() ? tr("Bilinmiyor") : currentVer, targetVer));
        if (isOnline) {
            card->onStepChanged(ip, static_cast<int>(DeployWorker::Step::Uploading),
                                tr("Master → Sunucu Aktarılıyor (%1)...").arg(targetVer));
            card->onLogMessage(ip, tr("Durum: İstemci çevrimiçi. Sunucu aktarımı sonrası otonom kurulum başlayacak."));
        } else {
            m_offlineCards.insert(card);
            card->onStepChanged(ip, static_cast<int>(DeployWorker::Step::QueuedOffline),
                                tr("Çevrimdışı — Sunucu Deposuna Aktarılıyor (%1)...").arg(targetVer));
            card->onLogMessage(ip, tr("Durum: İstemci şu anda çevrimdışı (kapalı). Paket Sunucu deposuna aktarılıyor; cihaz açıldığında otonom kurulacak."));
        }
        
        m_progressGrid->addCard(card);
        matchedCardCount++;
    }

    // Seçilen klasörde/dosyalarda henüz tabloda kayıtlı olmayan diğer Master veya Agent bilgisayarları için
    // bulunan en güncel paketleri de (her grup için yalnızca en yüksek versiyonu) Broker deposuna dahil et.
    QMap<QString, QPair<QString, QString>> bestByGroup; // groupKey -> (ver, fullPath)
    for (const QString& p : m_selectedPackagePaths) {
        QFileInfo fi(p);
        QString fname = fi.fileName();
        if (fname.startsWith("okidia-broker", Qt::CaseInsensitive)) continue;
        if (!fname.startsWith("okidia-agent", Qt::CaseInsensitive) &&
            !fname.startsWith("okidia-master", Qt::CaseInsensitive)) {
            continue;
        }
        QString ver = OkidiaUtils::extractPackageVersion(fname);
        if (ver.isEmpty()) continue;

        QString rolePrefix = fname.startsWith("okidia-master", Qt::CaseInsensitive) ? "master" : "agent";
        QString groupKey;
        if (fname.endsWith(".exe", Qt::CaseInsensitive)) {
            groupKey = rolePrefix + "_windows_amd64";
        } else {
            QStringList parts = fname.split('_');
            if (parts.size() >= 4) {
                groupKey = rolePrefix + "_" + OkidiaUtils::normalizeOsTag(parts[1]) + "_" + OkidiaUtils::normalizeArch(parts[3].section('.', 0, 0));
            } else {
                groupKey = fname;
            }
        }
        if (!bestByGroup.contains(groupKey) ||
            OkidiaUtils::compareVersions(ver, bestByGroup[groupKey].first) > 0) {
            bestByGroup[groupKey] = qMakePair(ver, p);
        }
    }
    for (auto it = bestByGroup.constBegin(); it != bestByGroup.constEnd(); ++it) {
        packagesToSendSet.insert(it.value().second);
    }

    QStringList agentAndMasterPkgs = packagesToSendSet.values();
    std::sort(agentAndMasterPkgs.begin(), agentAndMasterPkgs.end());

    // Sunucu sürüm kontrolü: Gönderilecek paketlerin sürümü sunucudan yüksekse önce sunucu güncellenmelidir
    QString brokerVer = m_session ? m_session->brokerVersion().trimmed() : "";
    for (const QString& p : agentAndMasterPkgs) {
        QString pkgVer = OkidiaUtils::extractPackageVersion(QFileInfo(p).fileName());
        if (!pkgVer.isEmpty() && !brokerVer.isEmpty() && brokerVer != "-" &&
            OkidiaUtils::compareVersions(pkgVer, brokerVer) > 0) {
            QMessageBox::warning(this, tr("Önce Sunucuyu Güncelleyin"),
                tr("Seçilen paketlerin sürümü (%1) mevcut sunucu sürümünden (%2) daha yeni!\n"
                   "İstemcilerin ve Master'ların sorunsuz çalışabilmesi için lütfen önce Sunucuyu Güncelleyin.").arg(pkgVer, brokerVer));
            return;
        }
    }

    if (matchedCardCount > 0) {
        m_leftStacked->setCurrentIndex(1); // Grid görünümüne geç
        m_showGridBtn->setVisible(true);
        m_progressGrid->updateSummary();
    } else if (!agentAndMasterPkgs.isEmpty()) {
        // İstemci eşleşmesi yok (örneğin sadece Master paketi seçildi veya açık olan istemciler zaten güncel)
        m_statusLabel->setText(tr("Paketler Sunucu deposuna aktarılıyor..."));
        m_progressBar->setVisible(true);
    } else if (!m_selectedBrokerPackagePath.isEmpty()) {
        // İstemci eşleşmesi yok ama Broker paketi seçildiyse doğrudan Broker güncellemesini başlat
        onUpdateBrokerClicked();
        return;
    } else {
        QMessageBox::information(this, tr("Bilgi"), tr("Seçilen paketlerle eşleşen veya güncellenmesi gereken istemci bulunamadı."));
        return;
    }

    if (m_sender) {
        m_sender->deleteLater();
    }

    m_sender = new OtaSender(m_session->tlsClient()->connection(), m_session->activeUser().id, this);
    
    connect(m_sender, &OtaSender::progress, this, [this](int currentFileIdx, int totalFiles, qint64 bytesSent, qint64 totalBytes) {
        m_progressBar->setVisible(true);
        m_progressBar->setMaximum(totalBytes);
        m_progressBar->setValue(bytesSent);
        m_statusLabel->setText(QString(tr("Dosya %1/%2 Sunucuya aktarılıyor...")).arg(currentFileIdx).arg(totalFiles));

        int pct = totalBytes > 0 ? (int)((bytesSent * 100) / totalBytes) : 0;
        for (HostInstallCard* c : m_progressGrid->allCards()) {
            if (!c->isFinished() && c->currentStep() == DeployWorker::Step::Uploading) {
                c->onUploadProgress(c->ip(), bytesSent, totalBytes, pct);
            }
        }
        m_progressGrid->updateSummary();
    });

    connect(m_sender, &OtaSender::finished, this, [this](bool success, const QString& errorMsg) {
        m_progressBar->setVisible(false);
        m_sendToBrokerBtn->setEnabled(true);
        if (success) {
            AuditLog::instance().recordDeployment({}, tr("Broker / Depo"), tr("OTA Güncelleme paketleri sunucuya başarıyla yüklendi"));
            m_statusLabel->setText(tr("Paketler Sunucuya başarıyla aktarıldı! Dağıtım bekleniyor..."));
            m_selectedPackagePaths.clear();
            m_selectedPackagesLabel->setText(tr("Paketler Sunucuya gönderildi."));
            m_sendToBrokerBtn->setEnabled(false);

            const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
            bool hasActiveOnlineInstall = false;
            for (HostInstallCard* c : m_progressGrid->allCards()) {
                if (m_offlineCards.contains(c) && !c->isFinished()) {
                    QString tVer = m_cardTargetVersions.value(c, m_targetVersions.value(c->ip()));
                    c->onLogMessage(c->ip(), tr("Paket Sunucu (Broker) güncelleme deposuna başarıyla kaydedildi."));
                    c->onLogMessage(c->ip(), tr("İstemci şu anda çevrimdışı (kapalı). Bilgisayar açılıp ağa bağlandığında (%1) otonom olarak kurulacaktır.").arg(tVer));
                    c->onStepChanged(c->ip(), static_cast<int>(DeployWorker::Step::QueuedOffline),
                                     tr("Depoda Hazır — Açıldığında Kurulacak (%1)").arg(tVer));
                } else if (!c->isFinished() &&
                           (c->currentStep() == DeployWorker::Step::Uploading ||
                            c->currentStep() == DeployWorker::Step::Pending)) {
                    c->onStepChanged(c->ip(), static_cast<int>(DeployWorker::Step::Installing),
                                     tr("Sunucu Depoladı → İstemcilere İletiliyor..."));
                    c->onLogMessage(c->ip(), tr("Paket Sunucu deposuna yüklendi. Sunucu otonom dosya aktarımı başlattı."));
                    m_cardActiveStartSecs[c] = nowSecs;
                    hasActiveOnlineInstall = true;
                }
            }
            if (hasActiveOnlineInstall && m_otaTimeoutTimer && !m_otaTimeoutTimer->isActive()) {
                m_otaTimeoutTimer->start();
            }
        } else {
            AuditLog::instance().recordDeployment({}, tr("Broker / Depo"), tr("OTA Paket yükleme hatası: ") + errorMsg);
            m_statusLabel->setText(QString(tr("<font color='red'>Sunucu aktarımı başarısız: %1</font>")).arg(errorMsg));
            for (HostInstallCard* c : m_progressGrid->allCards()) {
                if (!c->isFinished()) {
                    m_offlineCards.remove(c);
                    m_cardActiveStartSecs.remove(c);
                    c->onFinished(c->ip(), false, errorMsg, tr("Master → Sunucu aktarımı başarısız: ") + errorMsg);
                }
            }
        }
        m_progressGrid->updateSummary();
        updateTopButtonsVisibility();
    });


    m_sendToBrokerBtn->setEnabled(false);
    if (!agentAndMasterPkgs.isEmpty()) {
        m_sender->sendPackages(agentAndMasterPkgs);
    }
}

void OtaUpdatePanel::onBrokerReport(const QString& msg, bool success) {
    if (success) {
        m_statusLabel->setText(QString("<font color='green'>%1</font>").arg(msg));
    } else {
        m_statusLabel->setText(QString("<font color='red'>%1</font>").arg(msg));
    }

    if (m_progressGrid) {
        for (HostInstallCard* c : m_progressGrid->allCards()) {
            if (m_offlineCards.contains(c) && success) {
                continue;
            }
            if (!c->isFinished()) {
                c->onLogMessage(c->ip(), tr("Sunucu Raporu: ") + msg);
                if (success &&
                    (c->currentStep() == DeployWorker::Step::Uploading ||
                     c->currentStep() == DeployWorker::Step::Installing)) {
                    c->onStepChanged(c->ip(), static_cast<int>(DeployWorker::Step::Verifying),
                                     tr("İstemci İndiriyor & Otonom Kuruluyor..."));
                }
            }
        }
        m_progressGrid->updateSummary();
    }
}

void OtaUpdatePanel::onClientDisconnected(const QString& clientId) {
    if (!m_progressGrid || clientId.isEmpty()) return;

    HostInstallCard* c = m_cardByMachineId.value(clientId, nullptr);
    if (!c && m_machineIdToIp.contains(clientId)) {
        c = m_progressGrid->card(m_machineIdToIp.value(clientId));
    }
    if (c && !c->isFinished() && !m_offlineCards.contains(c)) {
        c->onStepChanged(c->ip(), static_cast<int>(DeployWorker::Step::Verifying),
                         tr("Servis Yeniden Başlatılıyor (Yeni Sürüm Bekleniyor)..."));
        c->onLogMessage(c->ip(), tr("İstemci kurulumu tamamlamak üzere servisi yeniden başlatıyor, yeniden bağlantı bekleniyor..."));
        m_progressGrid->updateSummary();
    }
}

void OtaUpdatePanel::onClientStateChanged(const QString& clientId, const ClientState& state) {
    if (!m_progressGrid) return;

    HostInstallCard* c = nullptr;
    if (!state.machineId.isEmpty() && m_cardByMachineId.contains(state.machineId)) {
        c = m_cardByMachineId.value(state.machineId);
    }
    if (!c && !clientId.isEmpty() && m_cardByMachineId.contains(clientId)) {
        c = m_cardByMachineId.value(clientId);
    }
    if (!c && m_machineIdToIp.contains(state.machineId)) {
        c = m_progressGrid->card(m_machineIdToIp.value(state.machineId));
    }
    if (!c && m_machineIdToIp.contains(clientId)) {
        c = m_progressGrid->card(m_machineIdToIp.value(clientId));
    }
    if (!c && !state.ip.isEmpty()) {
        c = m_progressGrid->card(state.ip);
    }
    if (!c && !state.hostname.isEmpty()) {
        c = m_progressGrid->card(state.hostname);
    }

    if (!c) return;

    QString targetVer = m_cardTargetVersions.value(c);
    if (targetVer.isEmpty() && !state.machineId.isEmpty() && m_targetVersions.contains(state.machineId)) {
        targetVer = m_targetVersions.value(state.machineId);
    }
    if (targetVer.isEmpty() && !clientId.isEmpty() && m_targetVersions.contains(clientId)) {
        targetVer = m_targetVersions.value(clientId);
    }
    if (targetVer.isEmpty() && m_targetVersions.contains(c->ip())) {
        targetVer = m_targetVersions.value(c->ip());
    }
    if (targetVer.isEmpty() && m_targetVersions.contains(c->hostname())) {
        targetVer = m_targetVersions.value(c->hostname());
    }
    if (targetVer.isEmpty() && m_targetVersions.contains(state.ip)) {
        targetVer = m_targetVersions.value(state.ip);
    }

    // Gönderim sırasında çevrimdışı olan bir istemci sonradan açıldıysa canlı duruma geçir
    if ((m_offlineCards.contains(c) || c->isQueuedOffline()) && state.connected) {
        m_offlineCards.remove(c);
        if (!state.agentVersion.isEmpty() && !targetVer.isEmpty() &&
            OkidiaUtils::compareVersions(state.agentVersion, targetVer) >= 0) {
            m_cardActiveStartSecs.remove(c);
            c->onLogMessage(c->ip(), tr("Çevrimdışı istemci açıldı ve yeni sürüm (%1) doğrulandı.").arg(state.agentVersion));
            c->onFinished(c->ip(), true, state.agentVersion,
                          tr("İstemci OTA paketini başarıyla kurdu ve yeni sürümle bağlandı: %1").arg(state.agentVersion));
        } else {
            m_cardActiveStartSecs[c] = QDateTime::currentSecsSinceEpoch();
            if (m_otaTimeoutTimer && !m_otaTimeoutTimer->isActive()) {
                m_otaTimeoutTimer->start();
            }
            c->onLogMessage(c->ip(), tr("Çevrimdışı istemci açıldı (%1). Sunucu OTA paketini (%2) iletiyor...")
                                     .arg(state.agentVersion.isEmpty() ? tr("Bilinmiyor") : state.agentVersion, targetVer));
            c->onStepChanged(c->ip(), static_cast<int>(DeployWorker::Step::Verifying),
                             tr("Cihaz Açıldı → İndiriyor & Otonom Kuruluyor..."));
        }
        m_progressGrid->updateSummary();
        refreshAgentList();
        return;
    }

    if (!c->isFinished() && !c->isQueuedOffline()) {
        // Eğer istemcinin versiyonu hedef versiyona ulaştıysa veya eşit/büyükse
        if (!state.agentVersion.isEmpty() && !targetVer.isEmpty()) {
            if (OkidiaUtils::compareVersions(state.agentVersion, targetVer) >= 0) {
                m_cardActiveStartSecs.remove(c);
                c->onLogMessage(c->ip(), tr("Yeni sürüm doğrulandı: %1 (Hedef: %2)").arg(state.agentVersion, targetVer));
                c->onFinished(c->ip(), true, state.agentVersion,
                              tr("İstemci OTA paketini başarıyla kurdu ve yeni sürümle bağlandı: %1").arg(state.agentVersion));
                m_progressGrid->updateSummary();
                refreshAgentList();

                bool allDone = true;
                for (HostInstallCard* cardItem : m_progressGrid->allCards()) {
                    if (!cardItem->isFinished() && !cardItem->isQueuedOffline()) {
                        allDone = false;
                        break;
                    }
                }
                if (allDone && m_statusLabel) {
                    m_statusLabel->setText(tr("<font color='green'>Açık olan istemcilerin OTA güncelleme işlemi tamamlandı.</font>"));
                }
            } else if (state.connected && m_cardActiveStartSecs.contains(c)) {
                // İstemci kurulum aşamasındayken (Verifying) yeniden bağlandıysa ama hala eski sürümdeyse ve üzerinden >= 25 sn geçtiyse
                qint64 elapsed = QDateTime::currentSecsSinceEpoch() - m_cardActiveStartSecs.value(c);
                if (elapsed >= 25 && c->currentStep() == DeployWorker::Step::Verifying) {
                    m_cardActiveStartSecs.remove(c);
                    QString errText = tr("Kurulum sonrası eski sürümle (%1) bağlandı (Hedef: %2)").arg(state.agentVersion, targetVer);
                    c->onLogMessage(c->ip(), tr("HATA: İstemci servisi yeniden başladı ancak sürüm değişmedi (%1).").arg(state.agentVersion));
                    c->onFinished(c->ip(), false, errText,
                                  tr("İstemci OTA paketini kurmayı denedi ancak eski sürümle (%1) geri bağlandı.\n"
                                     "Olası sebepler: Disk dolu, dpkg/kurulum kilidi veya paket imza/bağımlılık hatası.\n"
                                     "Detaylı kök neden için 'İstemci Sistem Günlüğünü Aç (Canlı)' butonuna tıklayarak okidia-agent.log dosyasını inceleyebilirsiniz.")
                                  .arg(state.agentVersion));
                    m_progressGrid->updateSummary();
                    refreshAgentList();
                }
            }
        }
    }
}

void OtaUpdatePanel::updatePackageMappings() {
    if (m_agentTable->rowCount() == 0) return;

    const bool isYM = m_session && m_session->activeUser().role == "YM";
    const QString brokerVer = m_session ? m_session->brokerVersion().trimmed() : "";

    // 0. Satır: Broker Sunucusu için en yüksek versiyonlu uyumlu paketi bul
    m_selectedBrokerPackagePath.clear();
    QString bestBrokerPkgVer;
    QTableWidgetItem* srvArchItem = m_agentTable->item(0, 4);
    QTableWidgetItem* srvPackageItem = m_agentTable->item(0, 5);
    if (srvArchItem && srvPackageItem) {
        QString srvArch = OkidiaUtils::normalizeArch(srvArchItem->text());
        QString srvData = srvArchItem->data(Qt::UserRole).toString();
        QString srvOsTag = srvData.startsWith("broker:") ? srvData.mid(7).trimmed() : srvData.trimmed();
        srvOsTag = OkidiaUtils::normalizeOsTag(srvOsTag);

        QRegularExpression brokerRegex(OkidiaUtils::getBrokerPackageRegex(srvOsTag, srvArch));
        for (const QString& path : m_selectedPackagePaths) {
            QFileInfo fi(path);
            if (brokerRegex.match(fi.fileName()).hasMatch()) {
                QString candVer = OkidiaUtils::extractPackageVersion(fi.fileName());
                if (m_selectedBrokerPackagePath.isEmpty() ||
                    OkidiaUtils::compareVersions(candVer, bestBrokerPkgVer) > 0) {
                    m_selectedBrokerPackagePath = path;
                    bestBrokerPkgVer = candVer;
                }
            }
        }

        bool brokerAlreadyUpToDateWithPkg = (!brokerVer.isEmpty() && brokerVer != "-" &&
                                             !bestBrokerPkgVer.isEmpty() &&
                                             OkidiaUtils::compareVersions(brokerVer, bestBrokerPkgVer) >= 0);

        if (!m_selectedBrokerPackagePath.isEmpty() && !brokerAlreadyUpToDateWithPkg) {
            srvPackageItem->setText(QFileInfo(m_selectedBrokerPackagePath).fileName());
            srvPackageItem->setData(Qt::UserRole, m_selectedBrokerPackagePath);
            srvPackageItem->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
            srvPackageItem->setForeground(QBrush(QColor(230, 126, 34)));
        } else if (!brokerVer.isEmpty() && brokerVer != "-" &&
                   (brokerAlreadyUpToDateWithPkg || m_selectedPackagePaths.isEmpty())) {
            m_selectedBrokerPackagePath.clear();
            srvPackageItem->setText(tr("GÜNCEL"));
            srvPackageItem->setData(Qt::UserRole, QString());
            srvPackageItem->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
            srvPackageItem->setForeground(QBrush(ThemeManager::instance().successColor()));
        } else {
            srvPackageItem->setText("-");
            srvPackageItem->setData(Qt::UserRole, QString());
            srvPackageItem->setIcon(QIcon());
            srvPackageItem->setForeground(m_agentTable->palette().text());
        }
    }

    // 1..N Satırları: Kayıtlı Master'lar (hostname bazlı) ve Kayıtlı İstemciler (machineId bazlı)
    bool hasAnyDistributablePkg = false;
    bool hasNewerPackageThanBroker = false;

    for (const QString& path : m_selectedPackagePaths) {
        QFileInfo fi(path);
        QString fname = fi.fileName();
        if (!fname.startsWith("okidia-broker", Qt::CaseInsensitive)) {
            hasAnyDistributablePkg = true;
        }
        QString pkgVer = OkidiaUtils::extractPackageVersion(fname);
        if (!pkgVer.isEmpty() && !brokerVer.isEmpty() && brokerVer != "-" &&
            OkidiaUtils::compareVersions(pkgVer, brokerVer) > 0) {
            hasNewerPackageThanBroker = true;
        }
    }

    for (int i = 1; i < m_agentTable->rowCount(); ++i) {
        QTableWidgetItem* archItem = m_agentTable->item(i, 4);
        QTableWidgetItem* packageItem = m_agentTable->item(i, 5);
        if (!archItem || !packageItem) continue;
        
        QString osArch = OkidiaUtils::normalizeArch(archItem->text().trimmed());
        QString dataStr = archItem->data(Qt::UserRole).toString();
        bool isMasterRow = dataStr.startsWith("master:");
        QString osTag = isMasterRow ? dataStr.mid(7).trimmed() : dataStr.trimmed();
        osTag = OkidiaUtils::normalizeOsTag(osTag);

        QString currentVer = m_agentTable->item(i, 2) ? m_agentTable->item(i, 2)->text().trimmed() : "";

        // Gerçekten mimari veya işletim sistemi bilinmiyorsa
        if (osArch.isEmpty() || osArch == "unknown" || osTag.isEmpty() || osTag == "unknown") {
            packageItem->setText(tr("Bilgi Eksik"));
            packageItem->setData(Qt::UserRole, QString());
            packageItem->setIcon(ThemeManager::icon(":/ui/alert_triangle.svg"));
            packageItem->setForeground(QBrush(ThemeManager::instance().warningColor()));
            continue;
        }

        // Seçilen klasör/dosyalar içinde bu satırın (role, osTag, osArch) bilgisine uyan EN YÜKSEK versiyonlu paketi bul
        // (İstemci Dağıtım panelindeki Klasör Seç mantığıyla birebir aynı)
        QRegularExpression regex(isMasterRow ? OkidiaUtils::getMasterPackageRegex(osTag, osArch)
                                             : OkidiaUtils::getAgentPackageRegex(osTag, osArch));
        QString assignedPackage;
        QString assignedFullPath;
        QString highestVer;

        for (const QString& path : m_selectedPackagePaths) {
            QFileInfo fi(path);
            if (regex.match(fi.fileName()).hasMatch()) {
                QString candVer = OkidiaUtils::extractPackageVersion(fi.fileName());
                if (assignedPackage.isEmpty() ||
                    (!candVer.isEmpty() && OkidiaUtils::compareVersions(candVer, highestVer) > 0)) {
                    assignedPackage = fi.fileName();
                    assignedFullPath = path;
                    highestVer = candVer;
                }
            }
        }

        // 1. Eşleşen bir paket seçildiyse: Bu bilgisayarın (hostname/machineId) kendi sürümünü paket sürümüyle karşılaştır
        if (!assignedPackage.isEmpty()) {
            if (!currentVer.isEmpty() && currentVer != "-" && !highestVer.isEmpty() &&
                OkidiaUtils::compareVersions(currentVer, highestVer) >= 0) {
                packageItem->setText(tr("GÜNCEL"));
                // Master paketi ise (diğer Master bilgisayarları için sunucu deposuna yüklenebilsin diye) dosya yolunu koru
                packageItem->setData(Qt::UserRole, isMasterRow ? assignedFullPath : QString());
                packageItem->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
                packageItem->setForeground(QBrush(ThemeManager::instance().successColor()));
            } else {
                packageItem->setText(assignedPackage);
                packageItem->setData(Qt::UserRole, assignedFullPath);
                packageItem->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
                packageItem->setForeground(QBrush(ThemeManager::instance().successColor()));
            }
            continue;
        }

        // 2. Eşleşen paket seçilmediyse: Bu bilgisayarın (hostname/machineId) kendi sürümünü Sunucu (Broker) sürümüyle karşılaştır
        bool isUpToDateWithBroker = (!currentVer.isEmpty() && currentVer != "-" &&
                                     !brokerVer.isEmpty() && brokerVer != "-" &&
                                     OkidiaUtils::compareVersions(currentVer, brokerVer) >= 0);
        if (isUpToDateWithBroker) {
            packageItem->setText(tr("GÜNCEL"));
            packageItem->setData(Qt::UserRole, QString());
            packageItem->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
            packageItem->setForeground(QBrush(ThemeManager::instance().successColor()));
        } else {
            packageItem->setText(tr("Güncelleme Gerekiyor"));
            packageItem->setData(Qt::UserRole, QString());
            packageItem->setIcon(ThemeManager::icon(":/ui/alert_triangle.svg"));
            packageItem->setForeground(QBrush(ThemeManager::instance().warningColor()));
        }
    }

    // Seçilen paketlerin özet etiketini ve buton durumlarını güncelle
    if (!m_selectedPackagePaths.isEmpty()) {
        QString lblText = QString(tr("<b>%1 paket seçildi:</b><br>")).arg(m_selectedPackagePaths.size());
        for (const QString& p : m_selectedPackagePaths) {
            QFileInfo fi(p);
            if (fi.fileName().startsWith("okidia-broker", Qt::CaseInsensitive)) {
                lblText += QString(tr("<font color='#e67e22'>• %1 (Sunucu Paketi)</font><br>")).arg(fi.fileName());
            } else if (fi.fileName().startsWith("okidia-master", Qt::CaseInsensitive)) {
                lblText += QString(tr("<font color='#3498db'>• %1 (Master Dağıtım Paketi)</font><br>")).arg(fi.fileName());
            } else {
                lblText += QString(tr("<font color='#2ecc71'>• %1 (İstemci Paketi)</font><br>")).arg(fi.fileName());
            }
        }
        m_selectedPackagesLabel->setText(lblText);

        // Eğer sunucunun kendisi güncellenmeli ise (m_selectedBrokerPackagePath doluysa veya paketler sunucudan yeniyse)
        if (hasNewerPackageThanBroker || !m_selectedBrokerPackagePath.isEmpty()) {
            m_sendToBrokerBtn->setEnabled(false);
            m_sendToBrokerBtn->setToolTip(tr("Önce Sunucuyu Güncelleyin"));
            if (m_updateBrokerBtn) {
                m_updateBrokerBtn->setVisible(isYM && !m_selectedBrokerPackagePath.isEmpty());
                m_updateBrokerBtn->setEnabled(isYM && !m_selectedBrokerPackagePath.isEmpty());
            }
        } else {
            m_sendToBrokerBtn->setEnabled(hasAnyDistributablePkg);
            m_sendToBrokerBtn->setToolTip("");
            if (m_updateBrokerBtn) {
                m_updateBrokerBtn->setVisible(false);
                m_updateBrokerBtn->setEnabled(false);
            }
        }
    }
}

void OtaUpdatePanel::onManageBrokerClicked() {
    BrokerUpdateManagerDialog dlg(m_session, this);
    dlg.exec();
}

void OtaUpdatePanel::onUpdateBrokerClicked() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Hata"), tr("Sunucu (Broker) bağlantısı yok!"));
        return;
    }
    if (m_selectedBrokerPackagePath.isEmpty() || !QFile::exists(m_selectedBrokerPackagePath)) {
        QMessageBox::warning(this, tr("Hata"), tr("Sunucu güncelleme paketi seçilmedi!"));
        return;
    }

    int res = QMessageBox::question(this, tr("Sunucu Güncellemesi"),
        tr("Seçilen Sunucu paketi (%1) sunucuya aktarılacak ve sunucu servisi otonom olarak güncellenip yeniden başlatılacaktır.\n\n"
           "İşleme devam etmek istiyor musunuz?").arg(QFileInfo(m_selectedBrokerPackagePath).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (res != QMessageBox::Yes) return;

    if (m_sender) {
        m_sender->deleteLater();
    }

    m_sender = new OtaSender(m_session->tlsClient()->connection(), m_session->activeUser().id, this);
    connect(m_sender, &OtaSender::progress, this, [this](int currentFileIdx, int totalFiles, qint64 bytesSent, qint64 totalBytes) {
        Q_UNUSED(currentFileIdx);
        Q_UNUSED(totalFiles);
        m_progressBar->setVisible(true);
        m_progressBar->setMaximum(totalBytes);
        m_progressBar->setValue(bytesSent);
        m_statusLabel->setText(tr("Sunucu güncelleme paketi aktarılıyor..."));
    });
    connect(m_sender, &OtaSender::finished, this, [this](bool success, const QString& errorMsg) {
        m_progressBar->setVisible(false);
        if (m_updateBrokerBtn) m_updateBrokerBtn->setEnabled(true);
        if (success) {
            AuditLog::instance().recordDeployment({}, tr("Broker"), tr("Sunucu (Broker) self-update güncelleme paketi yüklendi"));
            m_statusLabel->setText(tr("Sunucu paketi başarıyla aktarıldı. Sunucu yeniden başlatılıyor..."));
            QMessageBox::information(this, tr("Güncelleme Başlatıldı"),
                tr("Sunucu güncelleme paketi başarıyla iletildi.\n"
                   "Sunucu kendini güncelleyip servisi yeniden başlatacaktır. Master 2-3 saniye içinde otomatik tekrar bağlanacaktır."));
#if defined(Q_OS_WIN)
            // Eğer Broker yerel makinedeyse güncelleme tray'i kapatmış olabilir, 5 sn sonra geri başlat
            QTimer::singleShot(5000, []() {
                QString trayPath = "C:/Program Files/Okidia/Broker/bin/okidia-broker-tray.exe";
                if (QFile::exists(trayPath)) {
                    QProcess::startDetached(trayPath, QStringList());
                }
            });
#endif
        } else {
            AuditLog::instance().recordDeployment({}, tr("Broker"), tr("Sunucu self-update yükleme hatası: ") + errorMsg);
            m_statusLabel->setText(QString(tr("<font color='red'>Sunucu aktarımı başarısız: %1</font>")).arg(errorMsg));
        }
    });

    if (m_updateBrokerBtn) m_updateBrokerBtn->setEnabled(false);
    m_sender->sendBrokerSelfUpdate(m_selectedBrokerPackagePath);
}

void OtaUpdatePanel::restoreHistoryAndReconcile() {
    if (!m_progressGrid) return;
    if (!m_progressGrid->loadHistory()) {
        updateTopButtonsVisibility();
        return;
    }

    m_targetVersions.clear();
    m_machineIdToIp.clear();
    m_cardByMachineId.clear();
    m_cardTargetVersions.clear();
    m_cardActiveStartSecs.clear();
    m_offlineCards.clear();

    for (HostInstallCard* c : m_progressGrid->allCards()) {
        if (!c) continue;
        QString tVer = c->targetVersion();
        if (tVer.isEmpty() && !c->pkgPath().isEmpty()) {
            tVer = OkidiaUtils::extractPackageVersion(QFileInfo(c->pkgPath()).fileName());
            c->setTargetVersion(tVer);
        }
        if (!tVer.isEmpty()) {
            m_cardTargetVersions[c] = tVer;
            m_targetVersions[c->ip()] = tVer;
            m_targetVersions[c->hostname()] = tVer;
            if (!c->machineId().isEmpty()) {
                m_targetVersions[c->machineId()] = tVer;
            }
        }
        if (!c->machineId().isEmpty()) {
            m_cardByMachineId[c->machineId()] = c;
            m_machineIdToIp[c->machineId()] = c->ip();
        }
        if (c->isQueuedOffline()) {
            m_offlineCards.insert(c);
        }
    }
    updateTopButtonsVisibility();
}

void OtaUpdatePanel::updateTopButtonsVisibility() {
    if (!m_leftStacked) return;
    int index = m_leftStacked->currentIndex();
    bool hasHistory = m_progressGrid && (!m_progressGrid->allCards().isEmpty() || m_progressGrid->hasSavedHistory());
    if (index == 0) {
        if (m_backBtn) m_backBtn->setVisible(false);
        if (m_showGridBtn) m_showGridBtn->setVisible(hasHistory);
        if (m_clearHistoryBtn) m_clearHistoryBtn->setVisible(hasHistory);
        if (m_refreshBtn) m_refreshBtn->setVisible(true);
    } else {
        if (m_backBtn) m_backBtn->setVisible(true);
        if (m_showGridBtn) m_showGridBtn->setVisible(false);
        if (m_clearHistoryBtn) m_clearHistoryBtn->setVisible(false);
        if (m_refreshBtn) m_refreshBtn->setVisible(false);
    }
}

void OtaUpdatePanel::onOtaTimeoutCheck() {
    if (!m_progressGrid) return;

    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    static constexpr qint64 kOtaInstallTimeoutSecs = 120; // 2 dakika kurulum zaman aşımı

    bool anyActiveRemaining = false;
    bool stateChanged = false;

    for (HostInstallCard* c : m_progressGrid->allCards()) {
        if (!c || c->isFinished() || c->isQueuedOffline()) continue;

        if (m_cardActiveStartSecs.contains(c)) {
            qint64 elapsed = nowSecs - m_cardActiveStartSecs.value(c);
            if (elapsed >= kOtaInstallTimeoutSecs) {
                m_cardActiveStartSecs.remove(c);
                QString tVer = m_cardTargetVersions.value(c, c->targetVersion());
                c->onLogMessage(c->ip(), tr("ZAMAN AŞIMI: İstemci %1 saniye içinde yeni sürümle (%2) bağlanmadı.")
                                         .arg(kOtaInstallTimeoutSecs).arg(tVer));
                c->onFinished(c->ip(), false,
                              tr("Kurulum Zaman Aşımı (%1 sn)").arg(kOtaInstallTimeoutSecs),
                              tr("İstemciye OTA paketi iletildi ancak %1 saniye içinde yeni sürümle (%2) bağlantı doğrulanamadı.\n"
                                 "Olası sebepler: İstemci kurulum sırasında kapatıldı, ağ bağlantısı koptu veya paket kurulumu hata verdi.\n"
                                 "İstemci çevrimiçi olduğunda 'İstemci Sistem Günlüğünü Aç (Canlı)' butonuyla okidia-agent.log dosyasını inceleyebilirsiniz.")
                              .arg(kOtaInstallTimeoutSecs).arg(tVer));
                stateChanged = true;
            } else {
                anyActiveRemaining = true;
            }
        }
    }

    if (stateChanged) {
        m_progressGrid->updateSummary();
    }
    if (!anyActiveRemaining && m_otaTimeoutTimer) {
        m_otaTimeoutTimer->stop();
    }
}

void OtaUpdatePanel::openRemoteAgentLog(const QString& machineId, const QString& ip, const QString& hostname) {
    Q_UNUSED(ip);
    Q_UNUSED(hostname);
    if (!m_session) return;

    auto* dlg = new LogViewerDialog(m_session, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    if (!machineId.isEmpty()) {
        dlg->setTarget(machineId);
    }
    dlg->show();
}

void OtaUpdatePanel::onTableContextMenu(const QPoint& pos) {
    if (!m_agentTable) return;
    QTableWidgetItem* item = m_agentTable->itemAt(pos);
    if (!item) return;

    int row = item->row();
    auto* hostItem = m_agentTable->item(row, 1);
    auto* archItem = m_agentTable->item(row, 4);
    if (!hostItem || !archItem) return;

    QString roleData = archItem->data(Qt::UserRole).toString();
    bool isBrokerRow = roleData.startsWith("broker:");
    bool isMasterRow = roleData.startsWith("master:");
    QString machineId = hostItem->data(Qt::UserRole).toString();
    QString hostname = hostItem->text();

    QMenu menu(this);

    if (!isBrokerRow && !isMasterRow) {
        HostInstallCard* card = m_cardByMachineId.value(machineId, nullptr);
        if (!card && m_progressGrid) {
            card = m_progressGrid->card(hostname);
        }

        QAction* viewCardLogAct = menu.addAction(
            ThemeManager::icon(":/ui/file.svg"),
            tr("Son Kurulum / Güncelleme Günlüğünü İncele"));
        viewCardLogAct->setEnabled(card != nullptr);
        connect(viewCardLogAct, &QAction::triggered, this, [card]() {
            if (card) card->showLogDialog();
        });

        QAction* liveLogAct = menu.addAction(
            ThemeManager::icon(":/ui/monitor.svg"),
            tr("İstemci Sistem Günlüğünü Aç (Canlı — okidia-agent.log)"));
        connect(liveLogAct, &QAction::triggered, this, [this, machineId, hostname]() {
            openRemoteAgentLog(machineId, QString(), hostname);
        });
    } else if (isBrokerRow) {
        QAction* brokerLogAct = menu.addAction(
            ThemeManager::icon(":/ui/monitor.svg"),
            tr("Sunucu (Broker) Sistem Günlüğünü Aç"));
        connect(brokerLogAct, &QAction::triggered, this, [this]() {
            openRemoteAgentLog("broker", QString(), tr("Broker Sunucusu"));
        });
    } else if (isMasterRow && m_session && m_session->activeUser().role == "YM") {
        // Seçili tüm silinebilir Master satırlarını topla (aktif yerel bilgisayar hariç)
        QList<QPair<QString, QString>> targetsToDelete; // (machineId, hostname)
        QSet<int> selectedRows;
        for (const QModelIndex& idx : m_agentTable->selectionModel()->selectedRows()) {
            selectedRows.insert(idx.row());
        }
        if (!selectedRows.contains(row)) {
            selectedRows.clear();
            selectedRows.insert(row);
        }
        for (int r : selectedRows) {
            auto* hItm = m_agentTable->item(r, 1);
            auto* aItm = m_agentTable->item(r, 4);
            if (!hItm || !aItm) continue;
            if (!aItm->data(Qt::UserRole).toString().startsWith("master:")) continue;
            if (hItm->data(Qt::UserRole + 2).toBool()) continue; // (Bu Bilgisayar) korunur
            const QString hName = hItm->data(Qt::UserRole).toString().trimmed();
            const QString mId = hItm->data(Qt::UserRole + 1).toString().trimmed();
            if (!hName.isEmpty() || !mId.isEmpty()) {
                targetsToDelete.append(qMakePair(mId, hName));
            }
        }

        if (targetsToDelete.isEmpty()) {
            QAction* selfDisabledAct = menu.addAction(
                ThemeManager::icon(":/ui/trash.svg"),
                tr("Bu Bilgisayar Silinemez (Aktif Oturum)"));
            selfDisabledAct->setEnabled(false);
        } else {
            const QString actLabel = (targetsToDelete.size() == 1)
                ? tr("Master Bilgisayar Kaydını Sil (%1)").arg(targetsToDelete.first().second)
                : tr("Seçili %1 Master Bilgisayar Kaydını Sil").arg(targetsToDelete.size());
            QAction* delMasterAct = menu.addAction(
                ThemeManager::icon(":/ui/trash.svg"),
                actLabel);
            connect(delMasterAct, &QAction::triggered, this, [this, targetsToDelete]() {
                if (!m_session || !m_session->isConnected()) {
                    QMessageBox::warning(this, tr("Hata"), tr("Sunucu (Broker) bağlantısı yok!"));
                    return;
                }
                QStringList names;
                for (const auto& pair : targetsToDelete) {
                    names << (pair.second.isEmpty() ? pair.first : pair.second);
                }
                const QString msg = (targetsToDelete.size() == 1)
                    ? tr("<b>%1</b> adlı Master bilgisayar kaydı veritabanından silinecek.<br><br>"
                         "Bu bilgisayar hâlâ sistemde kullanılıyorsa, bir sonraki Master girişinde "
                         "çift kayıt oluşturmadan otomatik olarak listeye yeniden eklenecektir.<br><br>"
                         "Devam etmek istiyor musunuz?").arg(names.first().toHtmlEscaped())
                    : tr("Seçili <b>%1</b> Master bilgisayar kaydı veritabanından silinecek:<br>"
                         "<code>%2</code><br><br>"
                         "Bu bilgisayarlardan hâlâ aktif olanlar, bir sonraki Master girişinde "
                         "çift kayıt oluşturmadan otomatik olarak listeye yeniden eklenecektir.<br><br>"
                         "Devam etmek istiyor musunuz?").arg(targetsToDelete.size()).arg(names.join(", ").toHtmlEscaped());

                if (QMessageBox::question(this, tr("Master Bilgisayar Kaydını Sil"), msg,
                                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
                    return;
                }

                for (const auto& pair : targetsToDelete) {
                    m_session->sendDbMasterDeviceDelete(pair.first, pair.second);
                    AuditLog::instance().recordDeployment(
                        {},
                        pair.second.isEmpty() ? pair.first : pair.second,
                        tr("Master bilgisayar kaydı OTA listesinden silindi"));
                }
            });
        }
    }

    if (m_progressGrid && (!m_progressGrid->allCards().isEmpty() || m_progressGrid->hasSavedHistory())) {
        if (!menu.isEmpty()) menu.addSeparator();
        QAction* clearAct = menu.addAction(
            ThemeManager::icon(":/ui/clear.svg"),
            tr("Kayıtlı Kurulum Günlüklerini Sil"));
        connect(clearAct, &QAction::triggered, this, [this]() {
            if (m_progressGrid) m_progressGrid->clearHistory(true);
        });
    }

    if (!menu.isEmpty()) {
        menu.exec(m_agentTable->viewport()->mapToGlobal(pos));
    }
}



