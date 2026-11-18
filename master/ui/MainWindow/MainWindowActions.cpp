// master/ui/MainWindow/MainWindowActions.cpp
// Menü çubuğu, araç çubuğu butonları, kısayollar ve paneller menüsü kurulumu.

#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../ssh/SshCredentialDialog.h"
#include "../../ssh/DeployPanel.h"
#include "../../ssh/OtaUpdatePanel.h"
#include "../../ssh/SshManagerPanel.h"
#include "../Widgets/MasterUpdateDialog.h"
#include "../../common/utils/OkidiaUtils.h"
#include <utils/Logger.h>

#include <QMenu>
#include <QAction>
#include <QSettings>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDockWidget>
#include <QTabWidget>
#include <QApplication>
#include <QMediaPlayer>
#include <QAudioOutput>
#include "../AnnounceRecordDialog/AnnounceRecordDialog.h"
#include "../LiveAnnounceDialog/LiveAnnounceDialog.h"
#include "../ReportIssueDialog/ReportIssueDialog.h"
#include "../UserManagementPanel/UserManagementPanel.h"
#include "../ReportPanel/ReportPanel.h"
#include "../ChatWindow/ChatWindow.h"

void MainWindow::setupConnections() {
    // Toggle kilit butonu
    if (ui->btnLockToggle) {
        ui->btnLockToggle->setCheckable(true);
        ui->btnLockToggle->setChecked(false);
        connect(ui->btnLockToggle, &QPushButton::clicked,
                this, &MainWindow::onLockToggle);
    }

    ui->btnBroadcast->setCheckable(true);
    connect(ui->btnBroadcast, &QPushButton::clicked,
            this, &MainWindow::onBroadcast);
    // "Sohbet" toolbar butonu
    if (ui->btnChat)
        connect(ui->btnChat, &QPushButton::clicked,
                this, &MainWindow::onShowChat);

    connect(ui->btnSendMessage, &QPushButton::clicked,
            this, &MainWindow::onSendMessage);
    // Dosya gönder — dropdown menü
    {
        auto* fileMenu = new QMenu(this);

        auto* actRW = fileMenu->addAction(tr("Dosya Gönder (Okuma/Yazma)"));
        actRW->setToolTip(tr("Öğrenci dosyayı değiştirebilir"));
        connect(actRW, &QAction::triggered, [this]() { onSendFile(false); });

        auto* actRO = fileMenu->addAction(tr("Salt Okunur Gönder"));
        actRO->setToolTip(tr("Öğrenci dosyayı açabilir ama kaydedemez"));
        connect(actRO, &QAction::triggered, [this]() { onSendFile(true); });

        ui->btnSendFile->setMenu(fileMenu);
        // Butona tıklayınca menü açılsın (ok işareti olmadan da)
        connect(ui->btnSendFile, &QPushButton::clicked,
                [this]() { ui->btnSendFile->showMenu(); });
    }
    // Faz 6
    // Sınav dropdown menüsü
    {
        auto* examMenu = new QMenu(this);

        auto* actStart = examMenu->addAction(ThemeManager::icon(":/ui/play.svg"), tr("Sınav Başlat"));
        connect(actStart, &QAction::triggered, this, &MainWindow::onStartExam);

        auto* actCollect = examMenu->addAction(ThemeManager::icon(":/ui/file.svg"), tr("Dosyaları Topla"));
        actCollect->setEnabled(false);
        connect(actCollect, &QAction::triggered, this, &MainWindow::onCollectExam);

        auto* actEnd = examMenu->addAction(ThemeManager::icon(":/ui/stop.svg"), tr("Sınavı Bitir"));
        actEnd->setEnabled(false);
        connect(actEnd, &QAction::triggered, this, &MainWindow::onEndExam);

        ui->btnExam->setMenu(examMenu);

        // Sınav durumu değişince menü aksiyonlarını güncelle
        connect(this, &MainWindow::examStateChanged,
                [actStart, actCollect, actEnd, this](bool active) {
            actStart->setEnabled(!active);
            actCollect->setEnabled(active);
            actEnd->setEnabled(active);
            ui->btnExam->setProperty("active", active);
            ui->btnExam->style()->unpolish(ui->btnExam);
            ui->btnExam->style()->polish(ui->btnExam);
            ui->btnExam->setText(active ? tr("Sınav ●") : tr("Sınav"));
        });
    }
    // Anons dropdown
    {
        auto* audioMenu = new QMenu(this);

        auto* actLiveMic = audioMenu->addAction(ThemeManager::icon(":/ui/record.svg"), tr("Canlı Anons (Mikrofon)"));
        auto* actLive    = audioMenu->addAction(ThemeManager::icon(":/ui/volume.svg"), tr("Anons Kaydet ve Yayınla"));
        auto* actFile    = audioMenu->addAction(ThemeManager::icon(":/ui/play.svg"), tr("Ses Dosyası Yayınla"));
        audioMenu->addSeparator();
        auto* actStop    = audioMenu->addAction(ThemeManager::icon(":/ui/stop.svg"), tr("Yayını Durdur"));
        actStop->setEnabled(false);

        // UI sıfırla
        auto resetBtn = [this, actLiveMic, actLive, actFile, actStop]() {
            actLiveMic->setEnabled(true);
            actLive->setEnabled(true);
            actFile->setEnabled(true);
            actStop->setEnabled(false);
            ui->btnAudio->setProperty("audioActive", false);
            ui->btnAudio->style()->unpolish(ui->btnAudio);
            ui->btnAudio->style()->polish(ui->btnAudio);
            ui->btnAudio->setText(tr("Anons"));
            if (m_audioResetTimer) m_audioResetTimer->stop();
        };

        // Canlı Anons (Mikrofon)
        connect(actLiveMic, &QAction::triggered, [this, resetBtn]() {
            if (!m_session) return;
            const QStringList targets = commandTargets();
            LiveAnnounceDialog dlg(m_session, targets, commandTargetsDescription(), this);
            dlg.exec();
            resetBtn();
        });

        // Dosyadan oynat
        connect(actFile, &QAction::triggered,
                [this, actLiveMic, actLive, actStop, resetBtn]() {
            if (!m_session) return;

            const QStringList targets = commandTargets();
            if (targets.isEmpty()) {
                showCommandStatus(false, tr("Hedef seçilmedi"));
                return;
            }

            QString path = OkidiaUtils::getOpenFileName(
                this, tr("Ses Dosyası Seç"), "audio_broadcast",
                tr("Ses Dosyaları (*.mp3 *.wav *.ogg *.flac *.aac *.m4a);;") +
                tr("Tüm Dosyalar (*)"));
            if (path.isEmpty()) return;

            AnnounceRecordDialog dlg(m_session, targets, commandTargetsDescription(), path, this);
            dlg.exec();

            if (dlg.wasPublished()) {
                actLiveMic->setEnabled(false);
                actLive->setEnabled(false);
                actStop->setEnabled(true);
                ui->btnAudio->setProperty("audioActive", true);
                ui->btnAudio->style()->unpolish(ui->btnAudio);
                ui->btnAudio->style()->polish(ui->btnAudio);
                ui->btnAudio->setText(tr("Oynatılıyor ●"));
                showCommandStatus(true, tr("Ses Yayını (%1) → %2 istemci")
                    .arg(QFileInfo(path).fileName())
                    .arg(dlg.targets().size()));

                // Probe sessiz oynat → bitince UI sıfırla
                auto* probe = new QMediaPlayer(this);
                auto* ao    = new QAudioOutput(this);
                probe->setAudioOutput(ao);
                ao->setVolume(0.0f);
                probe->setSource(QUrl::fromLocalFile(path));
                connect(probe, &QMediaPlayer::playbackStateChanged,
                        [this, probe, ao, resetBtn](QMediaPlayer::PlaybackState s) {
                    if (s != QMediaPlayer::StoppedState) return;
                    probe->deleteLater();
                    ao->deleteLater();
                    resetBtn();
                });
                probe->play();
            }
        });

        // Anons Kaydet — dialog aç, kaydet, Yayınla → file transfer
        connect(actLive, &QAction::triggered,
                [this]() {
            if (!m_session) return;

            const QStringList targets = commandTargets();
            if (targets.isEmpty()) {
                showCommandStatus(false, tr("Hedef seçilmedi"));
                return;
            }

            AnnounceRecordDialog dlg(m_session, targets, commandTargetsDescription(), QString(), this);
            dlg.exec();

            if (dlg.wasPublished()) {
                // Anons file transfer ile gönderildi — yayın modu değil
                showCommandStatus(true, tr("Anons → %1 istemci").arg(dlg.targets().size()));
            }
        });

        // Durdur
        connect(actStop, &QAction::triggered, [this, resetBtn]() {
            if (m_session) m_session->broadcastAudioStop();
            resetBtn();
            showCommandStatus(true, tr("Anons durduruldu"));
        });

        ui->btnAudio->setMenu(audioMenu);
    }

    // Güç dropdown menüsü
    {
        auto* powerMenu = new QMenu(this);

        auto* actWol = powerMenu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Bilgisayarları Aç (Wake-on-LAN)"));
        actWol->setToolTip(tr("Seçili veya görünür bilgisayarlara Wake-on-LAN sinyali gönderir."));
        connect(actWol, &QAction::triggered, this, &MainWindow::onPowerWakeOnLan);

        powerMenu->addSeparator();

        auto* actRestart = powerMenu->addAction(ThemeManager::icon(":/ui/broadcast.svg"), tr("Yeniden Başlat"));
        connect(actRestart, &QAction::triggered, this, &MainWindow::onPowerRestart);

        auto* actShutdown = powerMenu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Bilgisayarları Kapat"));
        connect(actShutdown, &QAction::triggered, this, &MainWindow::onPowerShutdown);

        auto* actLogout = powerMenu->addAction(ThemeManager::icon(":/ui/key.svg"), tr("Oturumları Kapat"));
        connect(actLogout, &QAction::triggered, this, &MainWindow::onPowerLogout);

        auto* actHibernate = powerMenu->addAction(tr("Askıya Al (Uyku)"));
        connect(actHibernate, &QAction::triggered, this, &MainWindow::onPowerHibernate);

        powerMenu->addSeparator();

        auto* actWolAll = powerMenu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Tüm Sınıfı Aç (Wake-on-LAN)"));
        connect(actWolAll, &QAction::triggered, this, &MainWindow::onPowerWakeOnLanAll);

        auto* actShutdownAll = powerMenu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Tüm Sınıfı Kapat"));
        connect(actShutdownAll, &QAction::triggered, this, &MainWindow::onPowerShutdownAll);

        ui->btnPower->setMenu(powerMenu);
        ui->btnPower->setPopupMode(QToolButton::InstantPopup);
    }
    connect(ui->btnSchedule,    &QPushButton::clicked,
            this, &MainWindow::onScheduleCommand);
    // Faz 7
    connect(ui->btnReport,      &QPushButton::clicked,
            this, &MainWindow::onShowReport);

    connect(ui->classTree, &QTreeWidget::itemSelectionChanged,
            this, &MainWindow::onTreeSelectionChanged);
    connect(ui->classTree, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::onTreeContextMenu);

    connect(ui->qualitySlider, &QSlider::valueChanged,
            this, &MainWindow::onQualityChanged);
    connect(ui->fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFpsChanged);
    if (ui->btnStreamToggle) {
        connect(ui->btnStreamToggle, &QPushButton::clicked, this, [this]() {
            int curInterval = ui->fpsCombo->currentData().toInt();
            if (curInterval == 0 || (m_session && m_session->isStreamPaused())) {
                // Şu an duraklatılmış -> Devam ettir
                QSettings s("okidia", "master");
                int lastInterval = s.value("users/" + m_activeUser.username + "/ui/fps_interval", 1000).toInt();
                if (lastInterval <= 0) lastInterval = 1000;
                int targetIdx = 3; // 1 FPS fallback (data: 1000)
                for (int i = 1; i < ui->fpsCombo->count(); ++i) {
                    if (ui->fpsCombo->itemData(i).toInt() == lastInterval) {
                        targetIdx = i;
                        break;
                    }
                }
                ui->fpsCombo->setCurrentIndex(targetIdx);
            } else {
                // Şu an oynatılıyor -> Duraklat
                ui->fpsCombo->setCurrentIndex(0);
            }
        });
    }

    // Başlangıç thumbnail boyutunu slider'ın başlangıç değerine göre uygula
    m_thumbPx = ui->qualitySlider->value();
    connect(ui->actionAyarlar,           &QAction::triggered,
            this, &MainWindow::onSettings);
    connect(ui->actionCikis,             &QAction::triggered,
            this, &MainWindow::onExit);
    connect(ui->actionHakkinda,          &QAction::triggered,
            this, &MainWindow::onAbout);
    if (ui->actionKullaniciYonetimi) {
        connect(ui->actionKullaniciYonetimi, &QAction::triggered,
                this, &MainWindow::onShowUserManagement);
    }
}

void MainWindow::setupMainMenu() {
    ui->menubar->clear();
    const bool isObs = (m_activeUser.role == "OBSERVER");

    // 1. Dosya
    QMenu* menuDosya = ui->menubar->addMenu(tr("Dosya"));
    if (!isObs) {
        menuDosya->addAction(ThemeManager::icon(":/ui/settings.svg"), tr("Ayarlar"), this, &MainWindow::onSettings);
        menuDosya->addSeparator();
    }
    QAction* actLock = menuDosya->addAction(ThemeManager::icon(":/ui/lock.svg"), tr("Ekranı Kilitle"), this, &MainWindow::lockScreen);
    actLock->setShortcut(QKeySequence("Ctrl+L"));
    actLock->setToolTip(tr("Master yönetim panelini parola ile kilitler"));

    menuDosya->addSeparator();
    menuDosya->addAction(ThemeManager::icon(":/ui/school.svg"), tr("Okul / Ağ Değiştir..."), this, &MainWindow::onSwitchSchool);
    menuDosya->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Çıkış"), this, &MainWindow::onExit);

    // 2. İstemci (Gözlemci için tamamen kapalı)
    if (!isObs) {
        QMenu* menuIstemci = ui->menubar->addMenu(tr("İstemci"));
        connect(menuIstemci, &QMenu::aboutToShow, this, [this, menuIstemci]() {
            QString clientId;
            QStringList sel = selectedClientIds();
            if (sel.size() == 1) clientId = sel.first();
            buildClientMenu(menuIstemci, clientId);
        });
    }

    // 3. Görünüm
    QMenu* menuGorunum = ui->menubar->addMenu(tr("Görünüm"));
    
    QMenu* menuTemalar = menuGorunum->addMenu(tr("Temalar"));
    auto themes = ThemeManager::instance().availableThemes();
    for (int i = 0; i < themes.size(); ++i) {
        const auto& t = themes[i];
        QAction* act = menuTemalar->addAction(t.name);
        connect(act, &QAction::triggered, this, [this, i]() {
            ThemeManager::instance().applyTheme(i);
            QSettings("okidia", "master").setValue("users/" + m_activeUser.username + "/ui/theme", i);
        });
    }

    if (!isObs) {
        menuGorunum->addSeparator();
        auto* actTree = menuGorunum->addAction(ThemeManager::icon(":/ui/folder.svg"), tr("İstemci Ağacı Paneli"));
        actTree->setCheckable(true);
        actTree->setChecked(ui && ui->leftPanel && ui->leftPanel->isVisible());
        actTree->setShortcut(QKeySequence("Ctrl+B"));
        connect(actTree, &QAction::triggered, this, [this](bool checked) {
            if (ui && ui->leftPanel) {
                ui->leftPanel->setVisible(checked);
            }
        });
    }

    // 4. Araçlar
    QMenu* menuAraclar = ui->menubar->addMenu(tr("Araçlar"));
    
    if (!isObs) {
        // Sınav (toolbar'daki gibi)
        QMenu* menuSinav = menuAraclar->addMenu(ThemeManager::icon(":/ui/student.svg"), tr("Sınav Yönetimi"));
        auto* actStart = menuSinav->addAction(ThemeManager::icon(":/ui/play.svg"), tr("Sınav Başlat"), this, &MainWindow::onStartExam);
        auto* actCollect = menuSinav->addAction(ThemeManager::icon(":/ui/file.svg"), tr("Dosyaları Topla"), this, &MainWindow::onCollectExam);
        auto* actEnd = menuSinav->addAction(ThemeManager::icon(":/ui/stop.svg"), tr("Sınavı Bitir"), this, &MainWindow::onEndExam);
        actCollect->setEnabled(false);
        actEnd->setEnabled(false);
        connect(this, &MainWindow::examStateChanged, [actStart, actCollect, actEnd](bool active) {
            actStart->setEnabled(!active);
            actCollect->setEnabled(active);
            actEnd->setEnabled(active);
        });

        menuAraclar->addAction(ThemeManager::icon(":/ui/clock.svg"), tr("Zamanlı Görevler"), this, &MainWindow::onScheduleCommand);

        menuAraclar->addSeparator();

        QAction* sshCredAct = menuAraclar->addAction(ThemeManager::icon(":/ui/key.svg"), tr("SSH Kimlik Yöneticisi..."));
        connect(sshCredAct, &QAction::triggered, this, [this]() {
            SshCredentialDialog dlg(m_session, this);
            dlg.exec();
        });

        if (m_activeUser.role == "YM") {
            m_actUninstallPass = menuAraclar->addAction(ThemeManager::icon(":/ui/shield.svg"), tr("İstemci Kaldırma Parolası (Windows)..."), this, &MainWindow::onChangeUninstallPassword);
            m_actUninstallPass->setToolTip(tr("Windows istemcilerde istemcinin yetkisiz silinmesini önleyen koruma parolasını yönet"));
        }

        menuAraclar->addSeparator();
    }

    menuAraclar->addAction(ThemeManager::icon(":/ui/terminal.svg"), tr("Sistem Günlükleri"), this, &MainWindow::onShowLogViewer);

    // 5. Pencereler (Windows)
    QMenu* menuPencereler = ui->menubar->addMenu(tr("Pencereler"));
    menuPencereler->setObjectName("menuPencereler");
    connect(menuPencereler, &QMenu::aboutToShow, this, [this, menuPencereler]() {
        menuPencereler->clear();

        // 1. Ana Ekran
        QAction* actMain = menuPencereler->addAction(ThemeManager::icon(":/ui/monitor.svg"), tr("Ana Ekran (İstemci İzleme)"));
        actMain->setShortcut(QKeySequence("Alt+1"));
        connect(actMain, &QAction::triggered, this, [this]() {
            if (isMinimized()) showNormal();
            raise();
            activateWindow();
        });

        // 2. Sohbet Penceresi
        QAction* actChat = menuPencereler->addAction(ThemeManager::icon(":/ui/chat.svg"), tr("Sohbet"));
        actChat->setCheckable(true);
        actChat->setChecked(m_chatWindow && m_chatWindow->isVisible());
        actChat->setShortcut(QKeySequence("Alt+2"));
        connect(actChat, &QAction::triggered, this, [this]() {
            onShowChat();
            if (m_chatWindow) {
                if (m_chatWindow->isMinimized()) m_chatWindow->showNormal();
                m_chatWindow->raise();
                m_chatWindow->activateWindow();
            }
        });

        // 3. Sistem ve Ağ Yönetimi
        if (m_activeUser.role != "OBSERVER" && m_sysMgmtWindow) {
            QString sysTitle = tr("Sistem ve Ağ Yönetimi");
            QAction* actSys = menuPencereler->addAction(ThemeManager::icon(":/ui/settings.svg"), sysTitle);
            actSys->setCheckable(true);
            actSys->setChecked(m_sysMgmtWindow->isVisible());
            actSys->setShortcut(QKeySequence("Alt+3"));
            connect(actSys, &QAction::triggered, this, [this](bool checked) {
                if (checked || !m_sysMgmtWindow->isVisible()) {
                    showSystemManagementWindow();
                } else {
                    m_sysMgmtWindow->hide();
                }
            });
        }

        // 4. Kullanıcı Yönetimi
        if (m_activeUser.role == "YM") {
            QAction* actUser = menuPencereler->addAction(ThemeManager::icon(":/ui/group.svg"), tr("Kullanıcı Yönetimi"));
            actUser->setCheckable(true);
            actUser->setChecked(m_userMgmtPanel && m_userMgmtPanel->isVisible());
            actUser->setShortcut(QKeySequence("Alt+4"));
            connect(actUser, &QAction::triggered, this, [this]() {
                onShowUserManagement();
                if (m_userMgmtPanel) {
                    if (m_userMgmtPanel->isMinimized()) m_userMgmtPanel->showNormal();
                    m_userMgmtPanel->raise();
                    m_userMgmtPanel->activateWindow();
                }
            });
        }

        // 5. İstemci Yönetimi (Kayıtlılar & Kara Liste)
        if (m_activeUser.role != "OBSERVER") {
            QAction* actCliMgmt = menuPencereler->addAction(ThemeManager::icon(":/ui/discovery.svg"), tr("İstemci Yönetimi (Kayıtlı Cihazlar & Kara Liste)"));
            actCliMgmt->setCheckable(true);
            actCliMgmt->setChecked(m_clientMgmtDialog && m_clientMgmtDialog->isVisible());
            actCliMgmt->setShortcut(QKeySequence("Alt+5"));
            connect(actCliMgmt, &QAction::triggered, this, &MainWindow::onManageClients);
        }

        // 6. Raporlar
        if (m_activeUser.role != "OBSERVER") {
            QAction* actReport = menuPencereler->addAction(ThemeManager::icon(":/ui/chart.svg"), tr("Raporlar"));
            actReport->setCheckable(true);
            actReport->setChecked(m_reportPanel && m_reportPanel->isVisible());
            actReport->setShortcut(QKeySequence("Alt+6"));
            connect(actReport, &QAction::triggered, this, [this]() {
                onShowReport();
                if (m_reportPanel) {
                    if (m_reportPanel->isMinimized()) m_reportPanel->showNormal();
                    m_reportPanel->raise();
                    m_reportPanel->activateWindow();
                }
            });
        }

        // 7. Açık Tam Ekran İzleme / Kontrol Pencereleri
        bool hasRemoteViews = false;
        for (auto* view : m_remoteViews) {
            if (view && view->isVisible()) {
                if (!hasRemoteViews) {
                    menuPencereler->addSeparator();
                    hasRemoteViews = true;
                }
                QString vTitle = view->windowTitle().isEmpty() ? tr("Tam Ekran İzleme") : view->windowTitle();
                QAction* actRemote = menuPencereler->addAction(ThemeManager::icon(":/ui/monitor.svg"), vTitle);
                connect(actRemote, &QAction::triggered, view, [view]() {
                    if (view->isMinimized()) view->showNormal();
                    view->raise();
                    view->activateWindow();
                });
            }
        }

        // 8. Yeni İstemci İstekleri (Discovery)
        int pendingDiscovery = m_discoveryTable ? m_discoveryTable->rowCount() : 0;
        menuPencereler->addSeparator();
        QString discText = (pendingDiscovery > 0)
            ? QString(tr("Yeni İstemci İstekleri (%1 Bekleyen)...")).arg(pendingDiscovery)
            : tr("Yeni İstemci İstekleri Paneli...");
        QAction* actDisc = menuPencereler->addAction(ThemeManager::icon(":/ui/network.svg"), discText);
        actDisc->setEnabled(!isObserver());
        connect(actDisc, &QAction::triggered, this, [this]() {
            if (m_discoveryPanel) {
                m_discoveryPanel->show();
                m_discoveryPanel->raise();
                m_discoveryPanel->activateWindow();
                const QPoint center = this->geometry().center();
                m_discoveryPanel->move(
                    center.x() - m_discoveryPanel->width()  / 2,
                    center.y() - m_discoveryPanel->height() / 2);
            }
        });

        menuPencereler->addSeparator();

        // Toplu İşlemler
        QAction* actBringAll = menuPencereler->addAction(tr("Tüm Pencereleri Öne Getir"));
        connect(actBringAll, &QAction::triggered, this, [this]() {
            if (isMinimized()) showNormal();
            raise();
            if (m_chatWindow && m_chatWindow->isVisible()) {
                if (m_chatWindow->isMinimized()) m_chatWindow->showNormal();
                m_chatWindow->raise();
            }
            if (m_sysMgmtWindow && m_sysMgmtWindow->isVisible()) {
                if (m_sysMgmtWindow->isMinimized()) m_sysMgmtWindow->showNormal();
                m_sysMgmtWindow->raise();
            }
            if (m_userMgmtPanel && m_userMgmtPanel->isVisible()) {
                if (m_userMgmtPanel->isMinimized()) m_userMgmtPanel->showNormal();
                m_userMgmtPanel->raise();
            }
            if (m_clientMgmtDialog && m_clientMgmtDialog->isVisible()) {
                if (m_clientMgmtDialog->isMinimized()) m_clientMgmtDialog->showNormal();
                m_clientMgmtDialog->raise();
            }
            if (m_reportPanel && m_reportPanel->isVisible()) {
                if (m_reportPanel->isMinimized()) m_reportPanel->showNormal();
                m_reportPanel->raise();
            }
            for (auto* view : m_remoteViews) {
                if (view && view->isVisible()) {
                    if (view->isMinimized()) view->showNormal();
                    view->raise();
                }
            }
        });

        QAction* actMinAll = menuPencereler->addAction(tr("Tüm Pencereleri Simge Durumuna Küçült"));
        connect(actMinAll, &QAction::triggered, this, [this]() {
            if (m_chatWindow && m_chatWindow->isVisible()) m_chatWindow->showMinimized();
            if (m_sysMgmtWindow && m_sysMgmtWindow->isVisible()) m_sysMgmtWindow->showMinimized();
            if (m_userMgmtPanel && m_userMgmtPanel->isVisible()) m_userMgmtPanel->showMinimized();
            if (m_clientMgmtDialog && m_clientMgmtDialog->isVisible()) m_clientMgmtDialog->showMinimized();
            if (m_reportPanel && m_reportPanel->isVisible()) m_reportPanel->showMinimized();
            for (auto* view : m_remoteViews) {
                if (view && view->isVisible()) view->showMinimized();
            }
            showMinimized();
        });
    });

    // 6. Sunucu (Server) — Sadece YM (Yönetici Master) için
    if (m_activeUser.role == "YM") {
        QMenu* menuSunucu = ui->menubar->addMenu(tr("Sunucu"));
        menuSunucu->setObjectName("menuSunucu");

        menuSunucu->addAction(ThemeManager::icon(":/ui/power.svg"), tr("Sunucu Bilgisayarını Aç (Wake-on-LAN)..."), this, &MainWindow::onBrokerWakeOnLan);
        menuSunucu->addSeparator();
        menuSunucu->addAction(ThemeManager::icon(":/ui/info.svg"), tr("Sunucu Durumu ve Sistem Bilgileri..."), this, &MainWindow::onBrokerInfo);
        menuSunucu->addAction(ThemeManager::icon(":/ui/terminal.svg"), tr("Sunucuya SSH ile Bağlan (Terminal & Dosyalar)..."), this, &MainWindow::onBrokerSshConnect);
        menuSunucu->addAction(ThemeManager::icon(":/ui/refresh.svg"), tr("Sunucu Paket Deposu ve Güncellemeler..."), this, &MainWindow::onBrokerUpdateManager);
        menuSunucu->addSeparator();
        menuSunucu->addAction(ThemeManager::icon(":/ui/play_cmd.svg"), tr("Okidia Broker Servisini Yeniden Başlat..."), this, &MainWindow::onBrokerRestartService);
        menuSunucu->addAction(ThemeManager::icon(":/ui/refresh.svg"), tr("Sunucu Bilgisayarını Yeniden Başlat (Reboot)..."), this, &MainWindow::onBrokerRebootServer);
    }

    // 7. Yardım
    QMenu* menuYardim = ui->menubar->addMenu(tr("Yardım"));
    menuYardim->addAction(ThemeManager::icon(":/ui/help.svg"), tr("Kullanım Kılavuzu"), this, []() {
        QDesktopServices::openUrl(QUrl("https://okidia.net/docs"));
    });
    menuYardim->addAction(ThemeManager::icon(":/ui/warning.svg"), tr("Hata ve Geri Bildirim Bildir..."), this, [this]() {
        ReportIssueDialog dlg(m_session, this);
        dlg.exec();
    });
    menuYardim->addSeparator();
    menuYardim->addAction(ThemeManager::icon(":/ui/info.svg"), tr("Hakkında"), this, &MainWindow::onAbout);

    // Kısıtlı Mod Menüsü
    m_menuKisitli = ui->menubar->addMenu(tr("Kısıtlı Mod"));
    m_menuKisitli->setObjectName("menuKisitli");
    ui->menubar->setStyleSheet("QMenuBar::item#menuKisitli { background-color: #d32f2f; color: white; font-weight: bold; } QMenuBar::item#menuKisitli:selected { background-color: #b71c1c; }");
    m_menuKisitli->menuAction()->setVisible(false);

    m_actReconnect = m_menuKisitli->addAction(tr("Yeniden Bağlan"));
    connect(m_actReconnect, &QAction::triggered, this, [this]() {
        int reply = QMessageBox::warning(this, tr("Yeniden Bağlan"),
            tr("Yeniden bağlandığınızda, bu hesabı kullanan diğer açık oturumlar kısıtlı moda düşecektir.\n\nEmin misiniz?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            emit logoutRequested();
        }
    });
}
