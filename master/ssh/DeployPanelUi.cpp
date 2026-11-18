#include "../../common/utils/Logger.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../session/SessionManager.h"
#include "DeployPanel.h"
#include "DeployProgressGrid.h"
#include "SshCredSelectorWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QNetworkInterface>
#include <QProcess>
#include <QPushButton>
#include <QToolButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

void DeployPanel::setupUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(mainSplitter, 1);

    // ── SOL TARAF: Tarama ──
    auto *leftWidget = new QWidget(mainSplitter);
    auto *leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    auto *topGroup = new QGroupBox(tr("Ağ Tarama ve Otomatik Keşif"), leftWidget);
    auto *topLayout = new QVBoxLayout(topGroup);
    
    auto *subnetLbl = new QLabel(tr("Ağ arayüzleri:"), topGroup);
    subnetLbl->setProperty("status", "info");
    subnetLbl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    topLayout->addWidget(subnetLbl);

    m_subnetTree = new QTreeWidget(topGroup);
    m_subnetTree->setMinimumHeight(130);
    m_subnetTree->setHeaderHidden(true);
    m_subnetTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_subnetTree->setSortingEnabled(true);
    m_subnetTree->setRootIsDecorated(true);
    m_subnetTree->setExpandsOnDoubleClick(false);

    QObject::connect(m_subnetTree, &QTreeWidget::itemChanged, [this](QTreeWidgetItem *item, int) {
        if (item->parent() == nullptr) {
            const Qt::CheckState st = item->checkState(0);
            for (int i = 0; i < item->childCount(); ++i) item->child(i)->setCheckState(0, st);
        }
    });

    populateSubnetTree();

    topLayout->addWidget(m_subnetTree);

    auto *rangeRow = new QHBoxLayout();
    m_rangeEdit = new QLineEdit(topGroup);
    m_rangeEdit->setPlaceholderText(tr("Manuel IP Aralığı: 192.168.1.1-254"));
    rangeRow->addWidget(m_rangeEdit, 1);
    topLayout->addLayout(rangeRow);

    auto *buttonsRow = new QHBoxLayout();
    m_scanProgress = new QProgressBar(topGroup);
    m_scanProgress->setVisible(false);
    m_scanProgress->setMaximumHeight(14);
    
    m_scanStatus = new QLabel(tr("Hazır"), topGroup);
    m_scanStatus->setProperty("status", "info");

    m_stopScanBtn = new QPushButton(tr("Durdur"), topGroup);
    m_stopScanBtn->setIcon(ThemeManager::icon(":/ui/stop.svg"));
    m_stopScanBtn->setEnabled(false);

    m_scanBtn = new QPushButton(tr("Ağı Tara"), topGroup);
    m_scanBtn->setIcon(ThemeManager::icon(":/ui/discovery.svg"));
    m_scanBtn->setProperty("class", "primary");

    m_addSavedHostsBtn = new QPushButton(tr("Kayıtlı Cihazları Ekle"), topGroup);
    m_addSavedHostsBtn->setProperty("class", "secondary");

    buttonsRow->addWidget(m_scanProgress, 1);
    buttonsRow->addWidget(m_scanStatus);
    buttonsRow->addWidget(m_stopScanBtn);
    buttonsRow->addWidget(m_addSavedHostsBtn);
    buttonsRow->addWidget(m_scanBtn);
    topLayout->addLayout(buttonsRow);
    leftLayout->addWidget(topGroup);

    // ── SAĞ TARAF: QStackedWidget (Tablo Listesi ⇄ Paralel Grid İzleme) ──
    m_rightStacked = new QStackedWidget(mainSplitter);

    // Sayfa 0: Tablo Listesi ve Kurulum Paneli
    auto *tablePageWidget = new QWidget(m_rightStacked);
    auto *tablePageLayout = new QVBoxLayout(tablePageWidget);
    tablePageLayout->setContentsMargins(0, 0, 0, 0);

    auto *tableGroup = new QGroupBox(tr("Ağ Cihazları ve Kurulum"), tablePageWidget);
    auto *tableLayout = new QVBoxLayout(tableGroup);
    
    auto *tableTopLay = new QHBoxLayout();
    m_refreshTableBtn = new QPushButton(tr("Yenile"), tableGroup);
    m_refreshTableBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_refreshTableBtn->setProperty("class", "secondary");
    m_refreshTableBtn->setToolTip(tr("Tablodaki cihazların durumunu ve SSH/sistem bilgilerini yeniden sorgula"));

    m_btnClients = new QToolButton(tableGroup);
    m_btnClients->setText(tr("İstemciler"));
    m_btnClients->setIcon(ThemeManager::icon(":/ui/discovery.svg"));
    m_btnClients->setToolTip(tr("Yeni istemci istekleri ve istemci yönetimi"));
    m_btnClients->setPopupMode(QToolButton::InstantPopup);
    m_btnClients->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_btnClients->setStyleSheet(
        "QToolButton {"
        "    padding: 4px 10px;"
        "    min-height: 26px;"
        "    max-height: 26px;"
        "    border-radius: 5px;"
        "    border: 1px solid palette(mid);"
        "}"
        "QToolButton::menu-indicator { width: 0; }"
        "QToolButton[hasRequests=\"true\"] {"
        "    background-color: #f38ba8;"
        "    color: #1e1e2e;"
        "    border-color: #f38ba8;"
        "    font-weight: 500;"
        "}"
        "QToolButton[hasRequests=\"true\"]:hover { background-color: #e87a96; }"
    );

    auto* clientsMenu = new QMenu(m_btnClients);
    m_actNewClients = clientsMenu->addAction(ThemeManager::icon(":/ui/discovery.svg"), tr("Yeni İstemci İstekleri"));
    m_actNewClients->setEnabled(false);
    connect(m_actNewClients, &QAction::triggered, this, &DeployPanel::discoveryPanelRequested);

    clientsMenu->addSeparator();

    auto* actManage = clientsMenu->addAction(ThemeManager::icon(":/ui/monitor.svg"), tr("İstemci Yönetimi"));
    connect(actManage, &QAction::triggered, this, &DeployPanel::manageClientsRequested);

    m_btnClients->setMenu(clientsMenu);

    m_backBtn = new QPushButton(tr("Geri Dön"), tableGroup);
    m_backBtn->setIcon(ThemeManager::icon(":/ui/arrow_left.svg"));
    m_backBtn->setProperty("class", "primary");
    m_backBtn->setVisible(false);

    m_showGridBtn = new QPushButton(tr("Yükleme Durumu / Günlükler"), tableGroup);
    m_showGridBtn->setIcon(ThemeManager::icon(":/ui/monitor.svg"));
    m_showGridBtn->setProperty("class", "secondary");
    m_showGridBtn->setVisible(false);

    m_clearHistoryBtn = new QPushButton(tr("Günlükleri Temizle"), tableGroup);
    m_clearHistoryBtn->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    m_clearHistoryBtn->setProperty("class", "danger");
    m_clearHistoryBtn->setToolTip(tr("Kayıtlı SSH kurulum günlüklerini ve geçmiş durum kartlarını yerel diskten siler."));
    m_clearHistoryBtn->setVisible(false);

    tableTopLay->addWidget(m_refreshTableBtn);
    tableTopLay->addWidget(m_btnClients);
    tableTopLay->addStretch();
    tableTopLay->addWidget(m_backBtn);
    tableTopLay->addWidget(m_showGridBtn);
    tableTopLay->addWidget(m_clearHistoryBtn);
    tableLayout->addLayout(tableTopLay);

    m_unifiedHostTable = new QTableWidget(0, 6, tableGroup);
    m_unifiedHostTable->setHorizontalHeaderLabels({tr("IP Adresi"), tr("Bilgisayar Adı"), tr("İşletim Sistemi"), "Okidia", tr("Durum"), tr("Kurulacak Paket")});
    OkidiaUtils::setupTableHeaders(m_unifiedHostTable, "deploy_unified_host_table", {130, 150, 140, 80, 150, 200}, true);
    m_unifiedHostTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_unifiedHostTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_unifiedHostTable->setAlternatingRowColors(true);
    m_unifiedHostTable->setSortingEnabled(true);
    m_unifiedHostTable->sortByColumn(0, Qt::AscendingOrder);
    m_unifiedHostTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableLayout->addWidget(m_unifiedHostTable, 1);

    setupFloatingBar(tableLayout);

    // Alt Kurulum Paneli
    auto *deployPanelLayout = new QHBoxLayout();
    
    m_pkgSourceCombo = new QComboBox(tableGroup);
    m_pkgSourceCombo->addItem(ThemeManager::icon(":/ui/file.svg"), tr("Klasör Seç (Toplu)"), "folder");
    m_pkgSourceCombo->addItem(ThemeManager::icon(":/ui/checked.svg"), tr("Akıllı Paket Çöz / İndir (okidia.net)"), "smart_download");
    m_pkgSourceCombo->addItem(ThemeManager::icon(":/ui/file.svg"), tr("Paket Dosyası Seç (El ile)"), "file");
    m_pkgSourceCombo->addItem(ThemeManager::icon(":/ui/refresh.svg"), tr("GitHub'dan Çek"), "github");
    
    // GitHub seçeneğini devre dışı bırak ve tooltip ekle
    int ghIndex = m_pkgSourceCombo->findData("github");
    if (ghIndex >= 0) {
        m_pkgSourceCombo->setItemData(ghIndex, tr("Yakında hizmetinizde"), Qt::ToolTipRole);
        // Qt QComboBox içindeki satırı pasif yapmak için:
        auto* model = qobject_cast<QStandardItemModel*>(m_pkgSourceCombo->model());
        if (model) {
            auto* item = model->item(ghIndex);
            if (item) item->setEnabled(false);
        }
    }
    
    m_pkgPathEdit = new QLineEdit(tableGroup);
    m_pkgPathEdit->setReadOnly(true);
    m_pkgPathEdit->setPlaceholderText(tr("Agent paketlerinin bulunduğu klasörü seçin..."));
    
    m_selectLocalPkgBtn = new QPushButton(tr("Gözat"), tableGroup);
    
    m_installBtn = new QPushButton(tr("Seçili Cihazlara Kur"), tableGroup);
    m_installBtn->setIcon(ThemeManager::icon(":/ui/add.svg"));
    m_installBtn->setProperty("class", "primary");
    m_installBtn->setEnabled(false);
    
    deployPanelLayout->addWidget(new QLabel(tr("Paket:"), tableGroup));
    deployPanelLayout->addWidget(m_pkgSourceCombo);
    deployPanelLayout->addWidget(m_pkgPathEdit, 1);
    deployPanelLayout->addWidget(m_selectLocalPkgBtn);
    deployPanelLayout->addWidget(m_installBtn);
    tableLayout->addLayout(deployPanelLayout);
    
    m_installProg = new QProgressBar(tableGroup);
    m_installProg->setVisible(false);
    tableLayout->addWidget(m_installProg);

    m_installLog = new QTextEdit(tableGroup);
    m_installLog->setReadOnly(true);
    m_installLog->setMaximumHeight(100);
    m_installLog->setFont(QFont("Monospace", 9));
    m_installLog->setVisible(false);
    tableLayout->addWidget(m_installLog);

    tablePageLayout->addWidget(tableGroup);
    m_rightStacked->addWidget(tablePageWidget);

    // Sayfa 1: Paralel Kurulum İzleme Grid Paneli
    m_progressGrid = new DeployProgressGrid(m_rightStacked);
    m_progressGrid->setStorageKey("ssh_deploy_history");
    m_progressGrid->loadHistory();
    m_rightStacked->addWidget(m_progressGrid);

    const bool hasSavedDeployLogs = !m_progressGrid->allCards().isEmpty() || m_progressGrid->hasSavedHistory();
    m_showGridBtn->setVisible(hasSavedDeployLogs);
    m_clearHistoryBtn->setVisible(hasSavedDeployLogs);

    mainSplitter->addWidget(leftWidget);
    mainSplitter->addWidget(m_rightStacked);
    mainSplitter->setSizes({300, 700});

    // Merkezi Kimlik Seçici (Arayüzde gösterilmez, arka planda yapılandırma ve diyaloglar için tutulur)
    m_credSelector = new SshCredSelectorWidget(m_session, nullptr);
    m_credSelector->hide();

    // Bağlantılar
    connect(m_rightStacked, &QStackedWidget::currentChanged, this, [this](int index) {
        bool hasHist = m_progressGrid && (!m_progressGrid->allCards().isEmpty() || m_progressGrid->hasSavedHistory());
        if (index == 0) {
            m_backBtn->setVisible(false);
            m_showGridBtn->setVisible(hasHist);
            m_clearHistoryBtn->setVisible(hasHist);
        } else {
            m_backBtn->setVisible(true);
            m_showGridBtn->setVisible(false);
            m_clearHistoryBtn->setVisible(false);
        }
    });

    connect(m_showGridBtn, &QPushButton::clicked, this, [this]() {
        if (m_rightStacked) m_rightStacked->setCurrentIndex(1);
    });
    connect(m_clearHistoryBtn, &QPushButton::clicked, this, [this]() {
        if (m_progressGrid) m_progressGrid->clearHistory(true);
    });
    connect(m_progressGrid, &DeployProgressGrid::historyCleared, this, [this]() {
        if (m_showGridBtn) m_showGridBtn->setVisible(false);
        if (m_clearHistoryBtn) m_clearHistoryBtn->setVisible(false);
        if (m_installLog) m_installLog->clear();
    });
    connect(m_backBtn, &QPushButton::clicked, this, [this]() {
        if (m_rightStacked) m_rightStacked->setCurrentIndex(0);
    });
    connect(m_progressGrid, &DeployProgressGrid::backToTableRequested, this, [this]() {
        if (m_rightStacked) m_rightStacked->setCurrentIndex(0);
    });

    connect(m_scanBtn, &QPushButton::clicked, this, &DeployPanel::onScanNetwork);
    connect(m_refreshTableBtn, &QPushButton::clicked, this, &DeployPanel::onRefreshTableClicked);
    connect(m_addSavedHostsBtn, &QPushButton::clicked, this, &DeployPanel::onAddSavedHosts);
    connect(m_stopScanBtn, &QPushButton::clicked, this, [this]() {
        if (m_scanner) { m_scanner->stop(); m_scanner->wait(); }
        m_scanBtn->setEnabled(true);
        m_stopScanBtn->setEnabled(false);
        m_scanProgress->setVisible(false);
        m_scanStatus->setText(tr("Durduruldu"));
    });
    
    connect(m_unifiedHostTable, &QTableWidget::itemSelectionChanged, this, &DeployPanel::onUnifiedTableSelectionChanged);
    connect(m_unifiedHostTable, &QTableWidget::itemDoubleClicked, this, &DeployPanel::onHostDoubleClicked);
    
    m_unifiedHostTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_unifiedHostTable, &QTableWidget::customContextMenuRequested, this, &DeployPanel::onTableContextMenu);

    connect(m_pkgSourceCombo, &QComboBox::currentIndexChanged, [this]() {
        QString type = m_pkgSourceCombo->currentData().toString();
        if (type == "folder") {
            m_pkgPathEdit->setPlaceholderText(tr("Agent paketlerinin bulunduğu klasörü seçin..."));
            m_selectLocalPkgBtn->setText(tr("Gözat"));
            m_selectLocalPkgBtn->setEnabled(true);
            m_pkgPathEdit->clear();
        } else if (type == "smart_download") {
            m_pkgPathEdit->setPlaceholderText(tr("okidia.net sunucusundan cihaz mimarilerine uygun paketler otomatik indirilecek."));
            m_selectLocalPkgBtn->setText(tr("İndir"));
            m_selectLocalPkgBtn->setEnabled(true);
            m_pkgPathEdit->clear();
        } else if (type == "file") {
            m_pkgPathEdit->setPlaceholderText(tr("Kurulacak agent.deb paketini seçin..."));
            m_selectLocalPkgBtn->setText(tr("Gözat"));
            m_selectLocalPkgBtn->setEnabled(true);
            m_pkgPathEdit->clear();
        } else if (type == "github") {
            m_pkgPathEdit->setPlaceholderText(tr("GitHub'dan en güncel sürümler manuel veya otomatik indirilecek."));
            m_selectLocalPkgBtn->setText(tr("Çek"));
            m_selectLocalPkgBtn->setEnabled(true);
            m_pkgPathEdit->clear();
        }
    });

    int folderIdx = m_pkgSourceCombo->findData("folder");
    if (folderIdx >= 0) {
        m_pkgSourceCombo->setCurrentIndex(folderIdx);
    }

    connect(m_selectLocalPkgBtn, &QPushButton::clicked, this, &DeployPanel::onBrowsePackage);

    connect(m_installBtn, &QPushButton::clicked, this, &DeployPanel::onInstallSelected);
}

void DeployPanel::setupFloatingBar(QLayout *layout) {
    m_floatingBar = new QWidget(this);
    m_floatingBar->setObjectName("floatingBar");
    m_floatingBar->setFixedHeight(50);
    m_floatingBar->setVisible(false);

    auto *lay = new QHBoxLayout(m_floatingBar);
    lay->setContentsMargins(15, 0, 15, 0);
    lay->setSpacing(10);
    lay->setAlignment(Qt::AlignVCenter);

    m_selectionCountLbl = new QLabel(tr("0 makine seçildi"), m_floatingBar);
    m_selectionCountLbl->setStyleSheet("font-weight: bold; font-size: 13px;");
    lay->addWidget(m_selectionCountLbl);

    lay->addStretch();

    auto *assignBtn = new QPushButton(tr("SSH Bilgisi Gir"), m_floatingBar);
    assignBtn->setIcon(ThemeManager::icon(":/ui/key.svg"));
    assignBtn->setProperty("class", "primary");
    assignBtn->setCursor(Qt::PointingHandCursor);

    auto *closeBtn = new QPushButton(tr("Seçimi Kaldır"), m_floatingBar);
    closeBtn->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    closeBtn->setCursor(Qt::PointingHandCursor);

    lay->addWidget(assignBtn);
    lay->addWidget(closeBtn);

    m_floatingBar->setStyleSheet(R"(
        #floatingBar {
            background-color: palette(alternate-base);
            border: 1px solid palette(highlight);
            border-radius: 8px;
            margin: 0px 5px 5px 5px;
        }
        QLabel { color: palette(window-text); font-weight: bold; font-size: 13px; }
        QPushButton[class="primary"] {
            background-color: palette(highlight);
            color: palette(highlighted-text);
            border-radius: 4px;
            font-weight: bold;
            padding: 4px 12px;
            border: none;
        }
    )");

    if (layout) layout->addWidget(m_floatingBar);

    connect(closeBtn, &QPushButton::clicked, this, &DeployPanel::onDeselectAll);
    connect(assignBtn, &QPushButton::clicked, this, &DeployPanel::onAssignBulkCredentials);
}

void DeployPanel::populateSubnetTree() {
    if (!m_subnetTree) return;

    QSet<QString> checkedRanges;
    QTreeWidgetItemIterator itCheck(m_subnetTree);
    while (*itCheck) {
        if ((*itCheck)->parent() != nullptr && (*itCheck)->checkState(0) == Qt::Checked) {
            checkedRanges.insert((*itCheck)->data(0, Qt::UserRole).toString());
        }
        ++itCheck;
    }

    m_subnetTree->clear();

    // 1. Fiziksel ve Yerel Ağ Kartları
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;

        const QString ifname = iface.name().toLower();
        if (ifname.startsWith("docker") || ifname.startsWith("veth") ||
            ifname.startsWith("virbr") || ifname.startsWith("dummy") ||
            ifname.startsWith("br-")) {
            continue;
        }

        QTreeWidgetItem *ifaceItem = nullptr;
        for (const auto &entry : iface.addressEntries()) {
            const auto addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
            const QString mask = entry.netmask().toString();
            quint32 m32 = 0;
            int cidr = 0;
            for (const QString &o : mask.split('.')) m32 = (m32 << 8) | o.toUInt();
            while (m32 & 0x80000000) { ++cidr; m32 <<= 1; }

            quint32 ip32 = 0;
            for (const QString &o : addr.toString().split('.')) ip32 = (ip32 << 8) | o.toUInt();
            quint32 nm = cidr == 0 ? 0 : (~0u << (32 - cidr));
            quint32 netIp = ip32 & nm;

            if (!ifaceItem) {
                ifaceItem = new QTreeWidgetItem(m_subnetTree);
                ifaceItem->setText(0, iface.humanReadableName());
                ifaceItem->setIcon(0, ThemeManager::icon(":/ui/network.svg"));
                ifaceItem->setCheckState(0, Qt::Unchecked);
                ifaceItem->setExpanded(true);
                QFont f = ifaceItem->font(0);
                f.setBold(true);
                ifaceItem->setFont(0, f);
            }

            auto addSubnet = [&](const QString &range, const QString &customLabel = QString(), bool checked = false) {
                QString label = customLabel.isEmpty() ? range : customLabel;
                if (customLabel.isEmpty()) {
                    QStringList p = range.split('/');
                    if (p.size() == 2) {
                        const int cidr2 = p[1].toInt();
                        if (cidr2 == 24) label = p[0].left(p[0].lastIndexOf('.')) + ".1-254";
                    }
                }
                auto *subItem = new QTreeWidgetItem(ifaceItem);
                subItem->setText(0, label);
                subItem->setData(0, Qt::UserRole, range);
                subItem->setCheckState(0, (checked || checkedRanges.contains(range)) ? Qt::Checked : Qt::Unchecked);
            };

            QString netStr = QString("%1.%2.%3.%4").arg((netIp >> 24) & 0xFF).arg((netIp >> 16) & 0xFF).arg((netIp >> 8) & 0xFF).arg(netIp & 0xFF);

            if (ifname.startsWith("tailscale")) {
                ifaceItem->setExpanded(true);
                QProcess tsProc;
                tsProc.start("tailscale", {"status", "--json"});
                if (!tsProc.waitForStarted() || !tsProc.waitForFinished(1000) || tsProc.exitCode() != 0) {
                    tsProc.start("/usr/bin/tailscale", {"status", "--json"});
                    tsProc.waitForFinished(1000);
                }
                if (tsProc.exitStatus() == QProcess::NormalExit && tsProc.exitCode() == 0) {
                    QJsonDocument doc = QJsonDocument::fromJson(tsProc.readAllStandardOutput());
                    if (doc.isObject()) {
                        QJsonObject peers = doc.object()["Peer"].toObject();
                        for (auto it = peers.begin(); it != peers.end(); ++it) {
                            QJsonObject p = it.value().toObject();
                            const QString host = p["HostName"].toString();
                            const QJsonArray ips = p["TailscaleIPs"].toArray();
                            for (const auto &v : ips) {
                                const QString ipStr = v.toString();
                                if (ipStr.contains('.')) {
                                    addSubnet(ipStr, QString("%1 (%2)").arg(host.isEmpty() ? ipStr : host, ipStr), false);
                                }
                            }
                        }
                    }
                }
            } else if (cidr >= 24) {
                addSubnet(QString("%1/%2").arg(netStr).arg(cidr), QString(), false);
            } else if (cidr >= 16) {
                quint32 cNet = ip32 & 0xFFFFFF00;
                QString cStr = QString("%1.%2.%3.0").arg((cNet >> 24) & 0xFF).arg((cNet >> 16) & 0xFF).arg((cNet >> 8) & 0xFF);
                addSubnet(cStr + "/24", QString("%1/24 (%2)").arg(cStr, tr("Mevcut C Sınıfı")), false);
                addSubnet(QString("%1/%2").arg(netStr).arg(cidr), QString("%1/%2 (%3)").arg(netStr).arg(cidr).arg(tr("Tüm Ağ")), false);
            } else {
                quint32 net = ip32 & nm, bcast = net | ~nm;
                for (quint32 blk = net; blk < bcast; blk += 256) {
                    const QString bip = QString("%1.%2.%3.0").arg((blk >> 24) & 0xFF).arg((blk >> 16) & 0xFF).arg((blk >> 8) & 0xFF);
                    addSubnet(bip + "/24", QString(), false);
                }
            }
        }
    }

    // Eski veya kalıntı özel alt ağ ayarları varsa temizle
    QSettings settings;
    if (settings.contains("DeployPanel/CustomSubnets") || settings.contains("DeployPanel/CustomSubnetsInit")) {
        settings.remove("DeployPanel/CustomSubnets");
        settings.remove("DeployPanel/CustomSubnetsInit");
    }
}
