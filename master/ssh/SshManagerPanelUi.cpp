// master/ssh/SshManagerPanelUi.cpp
#include "SshManagerPanel.h"
#include "SshDeviceSessionWidget.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QTabBar>
#include <QToolButton>
#include <QMessageBox>

void SshManagerPanel::setupUi() {
    auto* mainLay = new QHBoxLayout(this);
    mainLay->setContentsMargins(10, 10, 10, 10);
    mainLay->setSpacing(10);

    // Sol panel: Bağlantı Ayarları & Hedefler
    auto* leftWidget = new QWidget(this);
    leftWidget->setFixedWidth(310);
    auto* leftLay = new QVBoxLayout(leftWidget);
    leftLay->setContentsMargins(0, 0, 0, 0);

    // Hedef IP'ler GroupBox
    auto* hostsGroup = new QGroupBox(tr("Hedef İstemciler"), leftWidget);
    auto* hostsLay = new QVBoxLayout(hostsGroup);
    
    m_fetchClientsBtn = new QPushButton(tr("İstemcileri Ekle"), hostsGroup);
    m_fetchClientsBtn->setToolTip(tr("Kayıtlı tüm çevrimiçi Okidia istemcileri ekle"));
    m_addSavedHostsBtn = new QPushButton(tr("Tüm Cihazları Ekle"), hostsGroup);
    m_addSavedHostsBtn->setToolTip(tr("SSH bilgileri bilinen tüm çevrimiçi cihazları ekle"));
    m_addSavedHostsBtn->setProperty("class", "secondary");
    
    m_hostList = new QTableWidget(0, 3, hostsGroup);
    m_hostList->setHorizontalHeaderLabels({tr("Cihaz Adı"), tr("IP"), tr("İS")});
    m_hostList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_hostList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_hostList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hostList->setContextMenuPolicy(Qt::CustomContextMenu);
    OkidiaUtils::setupTableHeaders(m_hostList, "ssh_host_list", {150, 110, 42}, false);
    m_hostList->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_hostList->setColumnWidth(2, 42);
    m_hostList->horizontalHeader()->setSortIndicatorShown(true);
    m_hostList->horizontalHeader()->setSectionsClickable(true);
    m_hostList->verticalHeader()->setVisible(false);
    m_hostList->setShowGrid(false);
    m_hostList->setSortingEnabled(true);
    m_hostList->sortByColumn(0, Qt::AscendingOrder);
    
    m_selectionCountLbl = new QLabel(tr("Seçilen: 0"), hostsGroup);

    auto* hostsMainBtnRow = new QHBoxLayout();
    m_addDeviceBtn = new QPushButton(ThemeManager::icon(":/ui/add.svg"), tr("Cihaz Ekle"), hostsGroup);
    m_addDeviceBtn->setProperty("class", "secondary");
    m_addDeviceBtn->setToolTip(tr("Listeye manuel IP ve cihaz adı (hostname) belirterek yeni bir hedef ekle"));

    m_assignCredBtn = new QPushButton(ThemeManager::icon(":/ui/key.svg"), tr("SSH Bilgisi Gir"), hostsGroup);
    m_assignCredBtn->setProperty("class", "primary");
    m_assignCredBtn->setToolTip(tr("Seçili cihazlara SSH kimlik bilgisi (kullanıcı adı, parola/anahtar) ata"));

    hostsMainBtnRow->addWidget(m_addDeviceBtn);
    hostsMainBtnRow->addWidget(m_assignCredBtn);

    auto* hostsBtnRow = new QHBoxLayout();
    m_manageCredsBtn = new QPushButton(ThemeManager::icon(":/ui/template_settings.svg"), tr("Yönet..."), hostsGroup);
    m_manageCredsBtn->setToolTip(tr("SSH Kimlik Yöneticisi penceresini aç"));

    auto* delHostBtn = new QPushButton("-", hostsGroup);
    delHostBtn->setToolTip(tr("Seçili cihazı listeden kaldır"));
    auto* clearHostBtn = new QPushButton(tr("Temizle"), hostsGroup);
    clearHostBtn->setToolTip(tr("Listeyi tamamen temizle"));

    hostsBtnRow->addWidget(delHostBtn);
    hostsBtnRow->addWidget(clearHostBtn);
    hostsBtnRow->addWidget(m_manageCredsBtn);

    hostsLay->addWidget(m_fetchClientsBtn);
    hostsLay->addWidget(m_addSavedHostsBtn);
    hostsLay->addWidget(m_hostList);
    hostsLay->addWidget(m_selectionCountLbl);
    hostsLay->addLayout(hostsMainBtnRow);
    hostsLay->addLayout(hostsBtnRow);
    leftLay->addWidget(hostsGroup);

    mainLay->addWidget(leftWidget);

    // Sağ panel: Sekmeler (Aktif Bağlantılar / Toplu Komut)
    auto* rightWidget = new QWidget(this);
    auto* rightLay = new QVBoxLayout(rightWidget);
    rightLay->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget(rightWidget);
    
    // -- 1. Aktif Bağlantılar Sekmesi (Cihaz Odaklı) --
    auto* activeTab = new QWidget(m_tabs);
    auto* activeLay = new QVBoxLayout(activeTab);
    auto* activeTopRow = new QHBoxLayout();
    
    m_connectBtn = new QPushButton(ThemeManager::icon(":/ui/network.svg"), tr("Seçili Cihazlara Bağlan"), activeTab);
    m_connectBtn->setProperty("class", "primary");
    m_closeAllBtn = new QPushButton(tr("Tümünü Kapat"), activeTab);
    m_closeAllBtn->setProperty("class", "danger");
    auto* clearBtn = new QPushButton(tr("Ekranı Temizle"), activeTab);
    
    m_activeNohupCheck = new QCheckBox(tr("Kapatırken arka planda sürdür (nohup/disown)"), activeTab);
    m_activeNohupCheck->setToolTip(tr("İşaretliyse, sekmeler veya panel kapatıldığında oturumdaki işlemler kesilmeden uzak makinede arka planda çalışmaya devam eder."));
    m_activeNohupCheck->setChecked(true);

    m_fontSizeSpin = new QSpinBox(activeTab);
    m_fontSizeSpin->setRange(8, 20);
    m_fontSizeSpin->setValue(11);
    m_fontSizeSpin->setSuffix(" pt");

    activeTopRow->addWidget(m_connectBtn);
    activeTopRow->addWidget(m_closeAllBtn);
    activeTopRow->addWidget(clearBtn);
    activeTopRow->addSpacing(10);
    activeTopRow->addWidget(m_activeNohupCheck);
    activeTopRow->addStretch(1);
    activeTopRow->addWidget(new QLabel(tr("Font:"), activeTab));
    activeTopRow->addWidget(m_fontSizeSpin);
    
    m_activeTabs = new QTabWidget(activeTab);
    m_activeTabs->setTabsClosable(true);
    m_activeTabs->setUsesScrollButtons(true);
    m_activeTabs->setElideMode(Qt::ElideRight);
    m_activeTabs->setDocumentMode(true);
    if (m_activeTabs->tabBar()) {
        m_activeTabs->tabBar()->installEventFilter(this);
    }
    
    activeLay->addLayout(activeTopRow);
    activeLay->addWidget(m_activeTabs, 1);
    m_tabs->addTab(activeTab, ThemeManager::icon(":/ui/monitor.svg"), tr("Aktif Bağlantılar"));

    // -- Toplu Komut Sekmesi --
    auto* cmdTab = new QWidget(m_tabs);
    auto* cmdLay = new QVBoxLayout(cmdTab);

    auto* cmdTopRow = new QHBoxLayout();
    auto* cmdLabel = new QLabel(tr("Gönderilecek Komut:"), cmdTab);
    cmdTopRow->addWidget(cmdLabel, 1);

    auto* tplFilterLabel = new QLabel(tr("Şablon Filtresi:"), cmdTab);
    m_tplOsFilterCombo = new QComboBox(cmdTab);
    m_tplOsFilterCombo->addItem(ThemeManager::icon(":/ui/settings.svg"), tr("Tüm Şablonlar"), "all");
    m_tplOsFilterCombo->addItem(ThemeManager::icon(":/ui/linux.svg"), tr("Linux Şablonları"), "linux");
    m_tplOsFilterCombo->addItem(ThemeManager::icon(":/ui/windows.svg"), tr("Windows Şablonları"), "windows");

    auto* presetLabel = new QLabel(tr("Hazır Şablonlar:"), cmdTab);
    m_cmdPresetCombo = new QComboBox(cmdTab);

    auto* refreshTplBtn = new QToolButton(cmdTab);
    refreshTplBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    refreshTplBtn->setToolTip(tr("Şablonları Yenile"));
    
    cmdTopRow->addWidget(tplFilterLabel);
    cmdTopRow->addWidget(m_tplOsFilterCombo);
    cmdTopRow->addSpacing(10);
    cmdTopRow->addWidget(presetLabel);
    cmdTopRow->addWidget(m_cmdPresetCombo, 2);
    cmdTopRow->addWidget(refreshTplBtn);

    m_cmdEdit = new QTextEdit(cmdTab);
    m_cmdEdit->setPlaceholderText("ls -la /tmp");
    m_cmdEdit->setMaximumHeight(100);
    
    auto* btnLay = new QHBoxLayout();
    m_runBtn = new QPushButton(tr(" Çalıştır"), cmdTab);
    m_runBtn->setIcon(ThemeManager::icon(":/ui/play_cmd.svg"));
    m_runBtn->setProperty("class", "primary");
    
    m_targetOsFilterCombo = new QComboBox(cmdTab);
    m_targetOsFilterCombo->addItem(ThemeManager::icon(":/ui/group.svg"), tr("Hedef: Tüm Seçili Cihazlar"), "all");
    m_targetOsFilterCombo->addItem(ThemeManager::icon(":/ui/linux.svg"), tr("Hedef: Sadece Linux"), "linux");
    m_targetOsFilterCombo->addItem(ThemeManager::icon(":/ui/windows.svg"), tr("Hedef: Sadece Windows"), "windows");
    m_targetOsFilterCombo->setToolTip(tr("Komutun yalnızca seçilen işletim sistemine sahip istemcilere gönderilmesini sağlar."));

    m_cmdSpeedCombo = new QComboBox(cmdTab);
    m_cmdSpeedCombo->addItem(ThemeManager::icon(":/ui/network.svg"), tr("Hız: Güvenli (2 Cihaz)"), 2);
    m_cmdSpeedCombo->addItem(ThemeManager::icon(":/ui/network.svg"), tr("Hız: Dengeli (4 Cihaz)"), 4);
    m_cmdSpeedCombo->addItem(ThemeManager::icon(":/ui/network.svg"), tr("Hız: Hızlı (8 Cihaz)"), 8);
    m_cmdSpeedCombo->setCurrentIndex(1); // Varsayılan: Dengeli (4)
    m_cmdSpeedCombo->setToolTip(tr("Zayıf Wi-Fi ve paylaşımlı ağlarda aynı anda kaç cihaza bağlanılacağını belirler."));

    m_cmdNohupCheck = new QCheckBox(tr("Arka planda kesintisiz çalıştır (nohup)"), cmdTab);
    m_cmdNohupCheck->setToolTip(tr("Bağlantı kopsa veya kapatılsa bile komutun uzak makinede kesilmeden arka planda çalışmasını sağlar."));

    m_saveTplBtn = new QPushButton(tr(" Şablona Kaydet"), cmdTab);
    m_saveTplBtn->setIcon(ThemeManager::icon(":/ui/save_template.svg"));
    
    m_mgrTplBtn = new QPushButton(tr(" Şablon Yöneticisi"), cmdTab);
    m_mgrTplBtn->setIcon(ThemeManager::icon(":/ui/template_settings.svg"));
    
    btnLay->addWidget(m_runBtn, 2);
    btnLay->addWidget(m_targetOsFilterCombo, 2);
    btnLay->addWidget(m_cmdSpeedCombo, 2);
    btnLay->addWidget(m_cmdNohupCheck, 2);
    btnLay->addWidget(m_saveTplBtn, 1);
    btnLay->addWidget(m_mgrTplBtn, 1);
    
    m_cmdScrollArea = new QScrollArea(cmdTab);
    m_cmdScrollArea->setWidgetResizable(true);
    m_cmdScrollContainer = new QWidget(m_cmdScrollArea);
    m_cmdLogLayout = new QVBoxLayout(m_cmdScrollContainer);
    m_cmdLogLayout->setAlignment(Qt::AlignTop);
    m_cmdScrollArea->setWidget(m_cmdScrollContainer);

    connect(m_tplOsFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        loadTemplates();
    });

    connect(refreshTplBtn, &QToolButton::clicked, this, [this]() {
        loadTemplates();
    });

    connect(m_cmdPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        QString cmdText = m_cmdPresetCombo->itemData(index).toString();
        QString targetOs = m_cmdPresetCombo->itemData(index, Qt::UserRole + 1).toString();
        bool isNohup = m_cmdPresetCombo->itemData(index, Qt::UserRole + 2).toBool();
        if (!cmdText.isEmpty()) {
            m_cmdEdit->setPlainText(cmdText);
            if (targetOs == "linux") {
                m_targetOsFilterCombo->setCurrentIndex(1);
            } else if (targetOs == "windows") {
                m_targetOsFilterCombo->setCurrentIndex(2);
            } else if (targetOs == "all") {
                m_targetOsFilterCombo->setCurrentIndex(0);
            }

            if (m_cmdNohupCheck) {
                m_cmdNohupCheck->setChecked(isNohup);
            }
        } else {
            m_cmdEdit->clear();
        }
    });

    cmdLay->addLayout(cmdTopRow);
    cmdLay->addWidget(m_cmdEdit);
    cmdLay->addLayout(btnLay);
    cmdLay->addWidget(m_cmdScrollArea, 1);
    m_tabs->addTab(cmdTab, tr("Toplu Komut"));

    mainLay->addWidget(m_tabs, 1);

    // Hedef listesi aksiyonları
    connect(m_fetchClientsBtn, &QPushButton::clicked, this, &SshManagerPanel::onFetchActiveClients);
    connect(m_addSavedHostsBtn, &QPushButton::clicked, this, &SshManagerPanel::onAddSavedHosts);
    connect(m_addDeviceBtn, &QPushButton::clicked, this, &SshManagerPanel::onAddDevice);
    connect(m_assignCredBtn, &QPushButton::clicked, this, &SshManagerPanel::onAssignCredentials);
    connect(m_manageCredsBtn, &QPushButton::clicked, this, &SshManagerPanel::onManageCredentials);
    connect(delHostBtn, &QPushButton::clicked, this, [this]() {
        QList<QTableWidgetItem*> selected = m_hostList->selectedItems();
        QList<int> rows;
        for (auto* item : selected) {
            if (item && !rows.contains(item->row())) rows.append(item->row());
        }
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int r : rows) m_hostList->removeRow(r);
    });
    connect(clearHostBtn, &QPushButton::clicked, this, [this]() {
        m_hostList->setRowCount(0);
    });

    connect(m_hostList, &QTableWidget::cellDoubleClicked, this, [this](int row, int /*col*/) {
        m_hostList->selectRow(row);
        onOpenTerminal();
    });
    connect(m_hostList, &QTableWidget::customContextMenuRequested, this, &SshManagerPanel::onHostContextMenu);

    connect(m_hostList, &QTableWidget::itemSelectionChanged, this, [this]() {
        QList<QTableWidgetItem*> selected = m_hostList->selectedItems();
        QSet<int> rows;
        for(auto* item : selected) {
            if(item) rows.insert(item->row());
        }

        m_selectionCountLbl->setText(tr("Seçilen: ") + QString::number(rows.size()));
        
        if (rows.size() == 1) {
            int row = *rows.begin();
            m_activeIp = m_hostList->item(row, 1)->data(Qt::UserRole).toString();
        } else {
            m_activeIp.clear();
        }
    });

    // Aktif Bağlantılar (Cihaz Odaklı)
    connect(m_connectBtn, &QPushButton::clicked, this, &SshManagerPanel::onOpenTerminal);
    connect(m_closeAllBtn, &QPushButton::clicked, this, [this]() {
        int count = m_activeTabs->count();
        if (count == 0) return;

        int ret = QMessageBox::question(
            this,
            tr("Tüm Bağlantıları Kapat"),
            tr("Açık olan %1 adet SSH bağlantı sekmesini kapatmak istediğinizden emin misiniz?").arg(count),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (ret != QMessageBox::Yes) {
            return;
        }

        bool nohupMode = m_activeNohupCheck && m_activeNohupCheck->isChecked();
        bool anyBusy = false;
        for (int i = 0; i < m_activeTabs->count(); ++i) {
            auto* sw = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(i));
            if (sw && sw->isTerminalBusy()) {
                anyBusy = true;
                break;
            }
        }

        bool safeDetach = nohupMode;
        if (anyBusy && !nohupMode) {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Aktif İşlemler Var"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText(tr("Bir veya daha fazla bağlantıda çalışan işlemler veya veri akışı tespit edildi.\n\n"
                              "Tüm sekmeleri kapatmak istiyor musunuz?"));
            auto* cancelBtn = msgBox.addButton(tr("İptal"), QMessageBox::RejectRole);
            auto* detachBtn = msgBox.addButton(tr("Arka Planda Bırak (Güvenli Kapat)"), QMessageBox::ActionRole);
            auto* forceBtn  = msgBox.addButton(tr("Zorla Kapat"), QMessageBox::DestructiveRole);
            msgBox.setDefaultButton(detachBtn);
            msgBox.exec();

            if (msgBox.clickedButton() == cancelBtn) {
                return;
            } else if (msgBox.clickedButton() == detachBtn) {
                safeDetach = true;
            }
        }

        while (m_activeTabs->count() > 0) {
            auto* sessionWidget = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(0));
            if (sessionWidget) {
                if (safeDetach) {
                    sessionWidget->safeDetachAllSessions();
                } else {
                    sessionWidget->stopAllSessions();
                }
            }
            m_activeTabs->removeTab(0);
            if (sessionWidget) sessionWidget->deleteLater();
        }
    });
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (auto* sessionWidget = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->currentWidget())) {
            sessionWidget->clearTerminalScreen();
        }
    });
    connect(m_fontSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int pt) {
        for (int i = 0; i < m_activeTabs->count(); ++i) {
            if (auto* sessionWidget = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(i))) {
                sessionWidget->setTerminalFontSize(pt);
            }
        }
    });
    connect(m_activeTabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto* sessionWidget = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(index));
        if (!sessionWidget) {
            m_activeTabs->removeTab(index);
            return;
        }

        bool nohupMode = m_activeNohupCheck && m_activeNohupCheck->isChecked();
        if (nohupMode) {
            sessionWidget->safeDetachAllSessions();
        } else if (sessionWidget->isTerminalBusy()) {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Aktif İşlem Uyarısı"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText(tr("<b>%1</b> (%2) cihazında aktif bir terminal oturumu veya çalışan bir komut tespit edildi.<br><br>"
                              "Sekmeyi kapatırsanız çalışan işlem yarıda kalabilir. Nasıl devam etmek istersiniz?")
                           .arg(sessionWidget->hostname(), sessionWidget->ip()));
            auto* cancelBtn = msgBox.addButton(tr("İptal"), QMessageBox::RejectRole);
            auto* detachBtn = msgBox.addButton(tr("Arka Planda Devam Ettir (Güvenli Kapat)"), QMessageBox::ActionRole);
            auto* forceBtn  = msgBox.addButton(tr("Zorla Kapat"), QMessageBox::DestructiveRole);
            msgBox.setDefaultButton(detachBtn);
            msgBox.exec();

            if (msgBox.clickedButton() == cancelBtn) {
                return;
            } else if (msgBox.clickedButton() == detachBtn) {
                sessionWidget->safeDetachAllSessions();
            } else {
                sessionWidget->stopAllSessions();
            }
        } else {
            sessionWidget->stopAllSessions();
        }

        m_activeTabs->removeTab(index);
        sessionWidget->deleteLater();
    });
    connect(m_activeTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index >= 0) {
            if (auto* sessionWidget = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(index))) {
                sessionWidget->focusTerminal();
            }
        }
    });

    // Komut
    connect(m_runBtn, &QPushButton::clicked, this, &SshManagerPanel::onRunCommand);
    connect(m_saveTplBtn, &QPushButton::clicked, this, &SshManagerPanel::onSaveTemplate);
    connect(m_mgrTplBtn, &QPushButton::clicked, this, &SshManagerPanel::onManageTemplates);
    
    loadTemplates();
}
