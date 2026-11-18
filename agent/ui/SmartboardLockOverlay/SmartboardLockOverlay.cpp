// agent/ui/SmartboardLockOverlay/SmartboardLockOverlay.cpp
#include "SmartboardLockOverlay.h"
#include "../../auth/OfflineAuthManager.h"
#include "../../system/SessionSwitchManager.h"
#include "../../../common/utils/QrCodeGenerator.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/OkidiaConfig.h"
#include "../../system/SystemInfo.h"

#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QFrame>
#include <QProcess>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
static HHOOK s_hSmartboardLockHook = nullptr;
static LRESULT CALLBACK SmartboardLockProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool isAlt = (p->flags & LLKHF_ALTDOWN) != 0;
        if (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) return 1;
        if (isAlt && (p->vkCode == VK_TAB || p->vkCode == VK_ESCAPE || p->vkCode == VK_F4)) return 1;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && p->vkCode == VK_ESCAPE) return 1;
    }
    return CallNextHookEx(s_hSmartboardLockHook, nCode, wParam, lParam);
}
#endif

SmartboardLockOverlay& SmartboardLockOverlay::instance() {
    static SmartboardLockOverlay s_instance;
    return s_instance;
}

SmartboardLockOverlay::SmartboardLockOverlay(QWidget* parent)
    : QWidget(parent,
              Qt::Window
            | Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
#if !defined(Q_OS_WIN)
            | Qt::X11BypassWindowManagerHint
#endif
            ) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi();

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, &SmartboardLockOverlay::onTickTimer);
}

void SmartboardLockOverlay::setupUi() {
    setObjectName("SmartboardLockOverlay");
    setStyleSheet("QWidget#SmartboardLockOverlay { background-color: #0d1117; color: #ffffff; }");

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(40, 40, 40, 40);
    rootLayout->setSpacing(20);

    // Üst Banner: Okul Logosu ve Okul Adı
    m_bannerFrame = new QFrame(this);
    m_bannerFrame->setObjectName("bannerFrame");
    m_bannerFrame->setStyleSheet(
        "QFrame#bannerFrame { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #161b22, stop:0.5 #21262d, stop:1 #161b22); "
        "border: 1px solid #30363d; border-radius: 16px; padding: 10px 28px; }"
    );
    auto* bannerLayout = new QHBoxLayout(m_bannerFrame);
    bannerLayout->setAlignment(Qt::AlignCenter);
    bannerLayout->setSpacing(20);

    // Okul Logosu (Yerel Varlık)
    m_logoLabel = new QLabel(m_bannerFrame);
    m_logoLabel->setAlignment(Qt::AlignCenter);
    m_logoLabel->setFixedSize(72, 72);
    m_logoLabel->setScaledContents(true);
    m_logoLabel->setVisible(false);
    bannerLayout->addWidget(m_logoLabel);

    // Okul Adı
    m_schoolLabel = new QLabel(m_bannerFrame);
    m_schoolLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_schoolLabel->setStyleSheet("font-size: 26px; font-weight: bold; color: #ffffff; letter-spacing: 1px;");
    bannerLayout->addWidget(m_schoolLabel);

    rootLayout->addWidget(m_bannerFrame, 0, Qt::AlignCenter);

    // Orta Alan (Solda QR Kod, Sağda Dokunmatik Numpad) - Kompakt ve Şık
    auto* centerLayout = new QHBoxLayout();
    centerLayout->setSpacing(32);
    centerLayout->setAlignment(Qt::AlignCenter);

    // --- SOL PANEL: QR Kod & Geri Sayım ---
    auto* qrContainer = new QFrame(this);
    qrContainer->setStyleSheet("QFrame { background-color: #161b22; border: 1px solid #30363d; border-radius: 12px; padding: 12px; }");
    auto* qrLayout = new QVBoxLayout(qrContainer);
    qrLayout->setAlignment(Qt::AlignCenter);
    qrLayout->setSpacing(10);

    m_qrLabel = new QLabel(qrContainer);
    m_qrLabel->setFixedSize(240, 240);
    m_qrLabel->setScaledContents(true);
    m_qrLabel->setAlignment(Qt::AlignCenter);
    m_qrLabel->setStyleSheet("background-color: #ffffff; border-radius: 8px; padding: 4px;");
    qrLayout->addWidget(m_qrLabel);

    m_timerLabel = new QLabel(tr("QR Kod Yenileniyor: 60 sn"), qrContainer);
    m_timerLabel->setAlignment(Qt::AlignCenter);
    m_timerLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #3fb950;");
    qrLayout->addWidget(m_timerLabel);

    centerLayout->addWidget(qrContainer);

    // --- SAĞ PANEL: Numpad & PIN Girişi ---
    m_numpadContainer = new QFrame(this);
    m_numpadContainer->setStyleSheet("QFrame { background-color: #161b22; border: 1px solid #30363d; border-radius: 12px; padding: 12px; }");
    auto* numpadLayout = new QVBoxLayout(m_numpadContainer);
    numpadLayout->setAlignment(Qt::AlignCenter);
    numpadLayout->setSpacing(10);

    // PIN Display Kutusu - Okunaklı, 6 haneli hücreli ve ferah tasarım
    m_pinDotsLabel = new QLabel(tr("PIN Girin"), m_numpadContainer);
    m_pinDotsLabel->setFixedHeight(50);
    m_pinDotsLabel->setFixedWidth(240);
    m_pinDotsLabel->setAlignment(Qt::AlignCenter);
    m_pinDotsLabel->setStyleSheet("background-color: #0d1117; border: 2px solid #30363d; border-radius: 8px; font-size: 16px; font-weight: bold; color: #8b949e; padding: 0px;");
    numpadLayout->addWidget(m_pinDotsLabel);

    // 3x4 Numpad Izgarası (Daha kompakt butonlar)
    auto* grid = new QGridLayout();
    grid->setSpacing(8);

    const QString btnStyle = 
        "QPushButton { background-color: #21262d; border: 1px solid #30363d; border-radius: 8px; color: #ffffff; font-size: 18px; font-weight: bold; min-width: 65px; min-height: 44px; }"
        "QPushButton:pressed { background-color: #388bfd; color: #ffffff; }"
        "QPushButton:hover { border-color: #58a6ff; }";

    const QString okBtnStyle = 
        "QPushButton { background-color: #238636; border: 1px solid #2ea043; border-radius: 8px; color: #ffffff; font-size: 15px; font-weight: bold; min-width: 65px; min-height: 44px; }"
        "QPushButton:pressed { background-color: #2ea043; }"
        "QPushButton:hover { background-color: #2ea043; }";

    const QString clearBtnStyle = 
        "QPushButton { background-color: #da3633; border: 1px solid #f85149; border-radius: 8px; color: #ffffff; font-size: 16px; font-weight: bold; min-width: 65px; min-height: 44px; }"
        "QPushButton:pressed { background-color: #b62324; }"
        "QPushButton:hover { background-color: #b62324; }";

    int num = 1;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            QString s = QString::number(num++);
            auto* b = new QPushButton(s, m_numpadContainer);
            b->setStyleSheet(btnStyle);
            connect(b, &QPushButton::clicked, [this, s]() { onNumpadClicked(s); });
            grid->addWidget(b, r, c);
        }
    }

    // Son Satır: C - 0 - OK
    auto* btnClear = new QPushButton(tr("C"), m_numpadContainer);
    btnClear->setStyleSheet(clearBtnStyle);
    connect(btnClear, &QPushButton::clicked, this, &SmartboardLockOverlay::onClearClicked);
    grid->addWidget(btnClear, 3, 0);

    auto* btnZero = new QPushButton("0", m_numpadContainer);
    btnZero->setStyleSheet(btnStyle);
    connect(btnZero, &QPushButton::clicked, [this]() { onNumpadClicked("0"); });
    grid->addWidget(btnZero, 3, 1);

    auto* btnOk = new QPushButton(tr("GİRİŞ"), m_numpadContainer);
    btnOk->setStyleSheet(okBtnStyle);
    connect(btnOk, &QPushButton::clicked, this, &SmartboardLockOverlay::onOkClicked);
    grid->addWidget(btnOk, 3, 2);

    numpadLayout->addLayout(grid);

    m_statusLabel = new QLabel(QString(), m_numpadContainer);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #f85149;");
    numpadLayout->addWidget(m_statusLabel);

    centerLayout->addWidget(m_numpadContainer);
    rootLayout->addLayout(centerLayout);

    // Takma Ad / Sınıf Adı Rozeti
    m_aliasLabel = new QLabel(this);
    m_aliasLabel->setAlignment(Qt::AlignCenter);
    m_aliasLabel->setStyleSheet("font-size: 20px; font-weight: bold; color: #58a6ff; background-color: #161b22; border: 1px solid #30363d; border-radius: 10px; padding: 6px 24px;");
    m_aliasLabel->setVisible(false);
    rootLayout->addWidget(m_aliasLabel, 0, Qt::AlignCenter);

    // Hızlı Flaş Bellek Güvenle Çıkarma Butonu
    m_btnEjectUsb = new QPushButton(tr("⏏️ Flaş Belleği Güvenle Çıkar"), this);
    m_btnEjectUsb->setIcon(ThemeManager::icon(":/ui/root_drive.svg"));
    m_btnEjectUsb->setMinimumHeight(44);
    m_btnEjectUsb->setMinimumWidth(260);
    m_btnEjectUsb->setStyleSheet(
        "QPushButton {"
        "  background-color: #161b22;"
        "  border: 1px solid #30363d;"
        "  border-radius: 10px;"
        "  color: #58a6ff;"
        "  font-size: 15px;"
        "  font-weight: bold;"
        "  padding: 8px 24px;"
        "}"
        "QPushButton:hover { background-color: #21262d; border-color: #58a6ff; }"
        "QPushButton:pressed { background-color: #1f6feb; color: #ffffff; }"
        "QPushButton:disabled { color: #2ea043; border-color: #2ea043; }"
    );
    m_btnEjectUsb->setVisible(false);
    connect(m_btnEjectUsb, &QPushButton::clicked, this, &SmartboardLockOverlay::onEjectUsb);
    rootLayout->addWidget(m_btnEjectUsb, 0, Qt::AlignCenter);

    // Alt Bilgi
    auto* footer = new QLabel(tr("Okidia Sınıf Yönetim Sistemi • Çevrimdışı Güvenli Kilit"), this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet("font-size: 12px; color: #484f58;");
    rootLayout->addWidget(footer);

    // Çözünürlük ve Çoklu Ekran / Projeksiyon Dinleyicileri
    if (QApplication::primaryScreen()) {
        connect(QApplication::primaryScreen(), &QScreen::geometryChanged,
                this, &SmartboardLockOverlay::updateGeometryToScreen);
    }

    if (qApp) {
        connect(qApp, &QGuiApplication::screenAdded, this, &SmartboardLockOverlay::onScreenAdded);
        connect(qApp, &QGuiApplication::screenRemoved, this, &SmartboardLockOverlay::onScreenRemoved);
    }
}

void SmartboardLockOverlay::updateGeometryToScreen() {
    if (!m_locked) return;
    updateAllScreens();
}

void SmartboardLockOverlay::onScreenAdded(QScreen* screen) {
    Q_UNUSED(screen)
    if (!m_locked) return;
    updateAllScreens();
}

void SmartboardLockOverlay::onScreenRemoved(QScreen* screen) {
    Q_UNUSED(screen)
    if (!m_locked) return;
    updateAllScreens();
}

void SmartboardLockOverlay::updateAllScreens() {
    if (!m_locked) return;

    QList<QScreen*> screens = QApplication::screens();
    if (screens.isEmpty()) return;

    // 1. Birincil Dokunmatik Akıllı Tahta Ekranı: QR + Numpad içeren ana overlay
    QScreen* primary = QApplication::primaryScreen();
    if (primary) {
        QRect scrGeom = primary->geometry();
        if (geometry() != scrGeom) {
            setGeometry(scrGeom);
        }
        if (windowState() != Qt::WindowFullScreen) {
            showFullScreen();
        }
        raise();
    }

    // 2. İkincil Ekranlar (Projeksiyon / HDMI Monitör):
    // Tuş takımı projeksiyona kaymaz; projeksiyon ekranı karartılarak masaüstü sızıntısı önlenir.
    QList<QScreen*> secondaryScreens;
    for (QScreen* s : screens) {
        if (s != primary) {
            secondaryScreens.append(s);
        }
    }

    // Fazlalık karartma pencerelerini sil (ekran çıkarıldıysa)
    while (m_extraOverlays.size() > secondaryScreens.size()) {
        QWidget* extra = m_extraOverlays.takeLast();
        if (extra) {
            extra->hide();
            extra->deleteLater();
        }
    }

    // Eksik karartma pencerelerini oluştur (projeksiyon/ek monitör bağlandıysa)
    while (m_extraOverlays.size() < secondaryScreens.size()) {
        QWidget* extra = new QWidget(nullptr,
            Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
#if !defined(Q_OS_WIN)
            | Qt::X11BypassWindowManagerHint
#endif
        );
        extra->setStyleSheet("background-color: #0d1117; color: #ffffff;");
        auto* lay = new QVBoxLayout(extra);
        lay->setAlignment(Qt::AlignCenter);

        auto* iconLbl = new QLabel(extra);
        iconLbl->setAlignment(Qt::AlignCenter);
        QIcon lockIcon = ThemeManager::icon(":/ui/lock.svg");
        if (!lockIcon.isNull()) {
            iconLbl->setPixmap(lockIcon.pixmap(48, 48));
            lay->addWidget(iconLbl);
        }

        auto* lbl = new QLabel(tr("Okidia Akıllı Tahta Kilitli\n\nLütfen ana tahta ekranından QR kod tarayın veya PIN girin."), extra);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("font-size: 20px; font-weight: bold; color: #8b949e;");
        lay->addWidget(lbl);

        extra->setCursor(Qt::BlankCursor);
        m_extraOverlays.append(extra);
    }

    // İkincil ekranların geometrisini ve tam ekran durumunu güncelle
    for (int i = 0; i < secondaryScreens.size(); ++i) {
        QScreen* secScreen = secondaryScreens[i];
        QWidget* extra = m_extraOverlays[i];
        if (extra->geometry() != secScreen->geometry()) {
            extra->setGeometry(secScreen->geometry());
        }
        if (extra->windowState() != Qt::WindowFullScreen) {
            extra->showFullScreen();
        }
        if (!extra->isActiveWindow()) {
            extra->raise();
        }
    }
}

void SmartboardLockOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_locked) {
        updateAllScreens();
    }
}

void SmartboardLockOverlay::setAlias(const QString& alias) {
    if (m_aliasLabel) {
        if (!alias.trimmed().isEmpty()) {
            m_aliasLabel->setText(alias.trimmed());
            m_aliasLabel->setVisible(m_locked);
        } else {
            m_aliasLabel->setVisible(false);
        }
    }
}

void SmartboardLockOverlay::showSmartboardLock(const QString& message) {
    Q_UNUSED(message);

    if (m_locked) {
        // Zaten kilitli: oturumu ve QR kodu yeniden üretme, titremeyi ve z-order yarışını önle
        updateAllScreens();
        raise();
        activateWindow();
        return;
    }

    m_enteredPin.clear();
    updatePinDisplay();
    m_statusLabel->clear();
    m_locked = true;

    // Okul Logosu ve Okul Adını yükle
    QString logoPath = OkidiaConfig::schoolLogoPath();
    if (!QFile::exists(logoPath)) {
        QString base = OkidiaConfig::assetsDir() + "/school_logo";
        for (const QString& ext : {".png", ".jpg", ".jpeg", ".svg", ".webp"}) {
            if (QFile::exists(base + ext)) {
                logoPath = base + ext;
                break;
            }
        }
    }

    if (QFile::exists(logoPath)) {
        QPixmap pm(logoPath);
        if (!pm.isNull()) {
            m_logoLabel->setPixmap(pm.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_logoLabel->setVisible(true);
        } else {
            m_logoLabel->setVisible(false);
        }
    } else {
        m_logoLabel->setVisible(false);
    }

    QSettings sApp("okidia", "agent");
    QString schoolName = sApp.value("school/name", "").toString().trimmed();
    if (schoolName.isEmpty()) {
        QSettings sMaster("okidia", "master");
        schoolName = sMaster.value("school/name", "").toString().trimmed();
    }
    if (!schoolName.isEmpty()) {
        m_schoolLabel->setText(schoolName);
    } else {
        m_schoolLabel->setText(tr("OKİDİA AKILLI TAHTA SİSTEMİ"));
    }
    m_schoolLabel->setVisible(true);

    // Takma Ad / Sınıf Adı
    QString alias = OkidiaConfig::agentAlias().trimmed();
    if (!alias.isEmpty()) {
        m_aliasLabel->setText(alias);
        m_aliasLabel->setVisible(true);
    } else {
        m_aliasLabel->setVisible(false);
    }

    // En güncel öğretmen tohumlarını diskten tazele
    OfflineAuthManager::instance().loadStore();

    // Tam ekran kapla (tüm monitörler ve projeksiyon dahil)
    updateAllScreens();

#if defined(Q_OS_WIN)
    if (!s_hSmartboardLockHook) {
        s_hSmartboardLockHook = SetWindowsHookEx(WH_KEYBOARD_LL, SmartboardLockProc, GetModuleHandle(nullptr), 0);
    }
#else
    // Linux: Eğer OS/LightDM ekranı daha önceden kilitlenmişse, önce kilidi serbest bırak
    QProcess::startDetached("loginctl", {"unlock-session"});
    QProcess::startDetached("light-locker-command", {"-a"});

    // light-locker ve tüm masaüstü ekran koruyucularını dondur (SIGSTOP).
    // Böylece Okidia kilitliyken arkada boşta kalma algılayıp LightDM kilit ekranını açamazlar.
    QProcess::startDetached("pkill", {"-STOP", "light-locker"});
    QProcess::startDetached("pkill", {"-STOP", "xfce4-screensaver"});
    QProcess::startDetached("pkill", {"-STOP", "mate-screensaver"});
    QProcess::startDetached("pkill", {"-STOP", "gnome-screensaver"});
    QProcess::startDetached("pkill", {"-STOP", "xscreensaver"});

    // X11 ekran koruyucu ve DPMS güç yönetimini askıya al, idle sayacını sıfırla
    QProcess::startDetached("xset", {"s", "off", "-dpms"});
    QProcess::startDetached("xset", {"s", "reset"});
#endif

    // Ekranı aç ve odaklan
    show();
    setWindowState(Qt::WindowFullScreen);
    raise();
    activateWindow();

    // Dinamik QR kodunu başlat
    refreshQrCode();
    m_tickTimer->start();

    // Çözünürlük değişimi veya masaüstü kaçaklarını önlemek için periyodik kontrol & üstte tutma
    if (!m_topTimer) {
        m_topTimer = new QTimer(this);
        connect(m_topTimer, &QTimer::timeout, this, [this]() {
            if (m_locked) {
                if (isMinimized()) {
                    showNormal();
                    showFullScreen();
                }
                if (!isActiveWindow()) {
                    raise();
                }
                updateAllScreens();
            }
        });
    }
    m_topTimer->start(2000);

    Logger::info(QObject::tr("SmartboardLockOverlay: Akıllı Tahta kilit ekranı gösterildi."));

    bool usbProtection = QSettings("okidia", "agent").value("policy/usb_protection_enabled", false).toBool()
                      || OkidiaConfig::isUsbProtectionEnabled();
    if (usbProtection) {
        QProcess::startDetached("sync", {});
        if (m_btnEjectUsb) {
            auto drives = SystemInfo::mountedUsbDrives();
            if (!drives.isEmpty()) {
                m_btnEjectUsb->setText(tr("⏏️ Flaş Belleği Güvenle Çıkar"));
                m_btnEjectUsb->setEnabled(true);
                m_btnEjectUsb->setVisible(true);
            } else {
                m_btnEjectUsb->setVisible(false);
            }
        }
    }
}

void SmartboardLockOverlay::hideSmartboardLock() {
    if (!m_locked && isHidden()) {
        return;
    }
    m_tickTimer->stop();
    if (m_topTimer) {
        m_topTimer->stop();
    }
    m_locked = false;
    OfflineAuthManager::instance().endLockSession();

    // İkincil karartma pencerelerini kapat
    for (QWidget* extra : m_extraOverlays) {
        if (extra) {
            extra->hide();
            extra->close();
            extra->deleteLater();
        }
    }
    m_extraOverlays.clear();

#if defined(Q_OS_WIN)
    if (s_hSmartboardLockHook) {
        UnhookWindowsHookEx(s_hSmartboardLockHook);
        s_hSmartboardLockHook = nullptr;
    }
#else
    // Linux: Ekran koruyucuları ve light-locker'ı tekrar devam ettir (SIGCONT)
    QProcess::startDetached("pkill", {"-CONT", "light-locker"});
    QProcess::startDetached("pkill", {"-CONT", "xfce4-screensaver"});
    QProcess::startDetached("pkill", {"-CONT", "mate-screensaver"});
    QProcess::startDetached("pkill", {"-CONT", "gnome-screensaver"});
    QProcess::startDetached("pkill", {"-CONT", "xscreensaver"});

    // Normal ekran koruyucu ve DPMS ayarlarını geri yükle
    QProcess::startDetached("xset", {"s", "on", "+dpms"});
#endif

    hide();
    close();
    releaseKeyboard();
    releaseMouse();
    emit lockScreenClosed();
    Logger::info(QObject::tr("SmartboardLockOverlay: Kilit ekranı kapatıldı."));
}

void SmartboardLockOverlay::refreshQrCode() {
    QString qrData = OfflineAuthManager::instance().startLockSession(QStringLiteral("board-agent-01"));
    QPixmap qrPixmap = QrCodeGenerator::generateQrPixmap(qrData, 250);
    m_qrLabel->setPixmap(qrPixmap);

    m_remainingSeconds = OfflineAuthManager::instance().remainingSeconds();
    m_timerLabel->setText(tr("QR Kod Yenileniyor: %1 sn").arg(m_remainingSeconds));
}

void SmartboardLockOverlay::onTickTimer() {
    if (!m_locked) return;
    m_remainingSeconds = OfflineAuthManager::instance().remainingSeconds();
    qint64 curWindow = OfflineAuthManager::instance().currentWindow();
    qint64 nowWindow = QDateTime::currentSecsSinceEpoch() / 60;

    if (m_remainingSeconds <= 0 || curWindow != nowWindow) {
        refreshQrCode();
    } else {
        m_timerLabel->setText(tr("QR Kod Yenileniyor: %1 sn").arg(m_remainingSeconds));
    }

    bool usbProtection = QSettings("okidia", "agent").value("policy/usb_protection_enabled", false).toBool()
                      || OkidiaConfig::isUsbProtectionEnabled();
    if (usbProtection && m_btnEjectUsb) {
        auto drives = SystemInfo::mountedUsbDrives();
        if (!drives.isEmpty()) {
            if (!m_btnEjectUsb->isVisible()) {
                m_btnEjectUsb->setText(tr("⏏️ Flaş Belleği Güvenle Çıkar"));
                m_btnEjectUsb->setEnabled(true);
                m_btnEjectUsb->setVisible(true);
            }
        } else {
            m_btnEjectUsb->setVisible(false);
        }
    } else if (m_btnEjectUsb) {
        m_btnEjectUsb->setVisible(false);
    }

#if !defined(Q_OS_WIN)
    // Her 15 saniyede bir X11 idle sayacını sıfırla ki light-locker / LightDM devreye girmesin
    m_xsetResetCounter++;
    if (m_xsetResetCounter >= 15) {
        m_xsetResetCounter = 0;
        QProcess::startDetached("xset", {"s", "reset"});
    }
#endif
}

void SmartboardLockOverlay::onNumpadClicked(const QString& digit) {
    if (m_enteredPin.length() >= 6) return;
    m_enteredPin.append(digit);
    updatePinDisplay();
    m_statusLabel->clear();

    if (m_enteredPin.length() == 6) {
        onOkClicked();
    }
}

void SmartboardLockOverlay::onClearClicked() {
    m_enteredPin.clear();
    updatePinDisplay();
    m_statusLabel->clear();
}

void SmartboardLockOverlay::updatePinDisplay() {
    if (m_enteredPin.isEmpty()) {
        m_pinDotsLabel->setText(tr("PIN Girin"));
        m_pinDotsLabel->setStyleSheet("background-color: #0d1117; border: 2px solid #30363d; border-radius: 8px; font-size: 16px; font-weight: bold; color: #8b949e; padding: 0px;");
    } else {
        // 6 haneli PIN için her rakamın sabit genişlikte ve kutu içinde hizalı durmasını sağla
        QString html = QStringLiteral("<table width=\"100%\" height=\"100%\" style=\"border-collapse: collapse;\"><tr>");
        for (int i = 0; i < 6; ++i) {
            if (i < m_enteredPin.length()) {
                html += QString("<td align=\"center\" valign=\"middle\" style=\"font-size: 24px; font-weight: bold; color: #ffffff; width: 16.6%;\">%1</td>")
                            .arg(m_enteredPin.at(i));
            } else {
                html += QStringLiteral("<td align=\"center\" valign=\"middle\" style=\"font-size: 20px; font-weight: normal; color: #30363d; width: 16.6%;\">•</td>");
            }
        }
        html += QStringLiteral("</tr></table>");
        m_pinDotsLabel->setText(html);
        m_pinDotsLabel->setStyleSheet("background-color: #0d1117; border: 2px solid #2ea043; border-radius: 8px; padding: 0px;");
    }
}

void SmartboardLockOverlay::onOkClicked() {
    if (m_enteredPin.length() != 6) {
        m_statusLabel->setText(tr("Lütfen 6 haneli PIN girin"));
        return;
    }

    QString username, displayName, password;
    bool valid = OfflineAuthManager::instance().verifyPin(m_enteredPin, &username, &displayName, &password);
    if (valid) {
        m_statusLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #3fb950;");
        m_statusLabel->setText(tr("Hoş geldiniz, %1!").arg(displayName));
        QApplication::processEvents();

        // 1. Ekran kilidini kapat
        hideSmartboardLock();

        // 2. Sinyal yay (okidia-agent root servisi oturum geçişini yürütecek, Master ve Broker bilgilendirilecek)
        emit unlockedByTeacher(username, displayName, password);
    } else {
        m_statusLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #f85149;");
        m_statusLabel->setText(tr("Geçersiz veya süresi dolmuş PIN!"));
        m_enteredPin.clear();
        updatePinDisplay();
    }
}

void SmartboardLockOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
        onNumpadClicked(QString::number(event->key() - Qt::Key_0));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete || event->key() == Qt::Key_Escape) {
        onClearClicked();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        onOkClicked();
        event->accept();
        return;
    }
    event->ignore();
}

void SmartboardLockOverlay::keyReleaseEvent(QKeyEvent* event) {
    event->ignore();
}

void SmartboardLockOverlay::mousePressEvent(QMouseEvent* event) {
    event->accept();
}

void SmartboardLockOverlay::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
}

void SmartboardLockOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    event->accept();
}

void SmartboardLockOverlay::closeEvent(QCloseEvent* event) {
    if (m_locked) {
        event->ignore();
    } else {
        event->accept();
    }
}

void SmartboardLockOverlay::onEjectUsb() {
    bool ok = SystemInfo::safeEjectUsbDrives();
    if (ok) {
        m_btnEjectUsb->setText(tr("Flaş Bellek Güvenle Ayrıldı ✓"));
        m_btnEjectUsb->setEnabled(false);
        Logger::info(QObject::tr("SmartboardLockOverlay: Flaş bellek kilit ekranından güvenle ayrıldı."));
    } else {
        m_btnEjectUsb->setText(tr("Aygıt Meşgul (Tekrar Deneyin)"));
    }
}
