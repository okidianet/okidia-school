#include "ScreenLockOverlay.h"
#include "ui_ScreenLockOverlay.h"

#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QTimer>
#include <QProcess>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif

static HHOOK s_hScreenLockHook = nullptr;

static LRESULT CALLBACK ScreenLockKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool isAlt = (p->flags & LLKHF_ALTDOWN) != 0;

        // WinKey, Alt+Tab, Alt+Esc, Ctrl+Esc, Alt+F4 tuş kombinasyonlarını yut
        if (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) return 1;
        if (isAlt && (p->vkCode == VK_TAB || p->vkCode == VK_ESCAPE || p->vkCode == VK_F4)) return 1;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && p->vkCode == VK_ESCAPE) return 1;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && p->vkCode == VK_ESCAPE) return 1;
    }
    return CallNextHookEx(s_hScreenLockHook, nCode, wParam, lParam);
}
#endif

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
ScreenLockOverlay& ScreenLockOverlay::instance() {
    static ScreenLockOverlay s_instance;
    return s_instance;
}

ScreenLockOverlay::ScreenLockOverlay(QWidget* parent)
    : QWidget(parent,
              Qt::Window
            | Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
#if !defined(Q_OS_WIN)
            | Qt::X11BypassWindowManagerHint
#endif
            )
    , ui(new Ui::ScreenLockOverlay) {
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_AcceptTouchEvents, true);

    // Tüm klavye olayları bu widget'a gelsin
    setFocusPolicy(Qt::StrongFocus);
}

ScreenLockOverlay::~ScreenLockOverlay() {
    delete ui;
}

// ---------------------------------------------------------------------------
// Genel arayüz
// ---------------------------------------------------------------------------
void ScreenLockOverlay::showLock(const QString& message) {
    if (m_locked) return;
    m_locked = true;

#if defined(Q_OS_WIN)
    if (!s_hScreenLockHook) {
        s_hScreenLockHook = SetWindowsHookExW(WH_KEYBOARD_LL, ScreenLockKeyboardProc, GetModuleHandle(NULL), 0);
    }
#endif

    coverAllScreens(message.isEmpty()
                    ? tr("Lütfen öğretmeninizi dinleyin.")
                    : message);
}

void ScreenLockOverlay::hideLock() {
    if (!m_locked && isHidden()) return;
    m_locked = false;
    m_currentMessage.clear();

#if defined(Q_OS_WIN)
    if (s_hScreenLockHook) {
        UnhookWindowsHookEx(s_hScreenLockHook);
        s_hScreenLockHook = nullptr;
    }
#else
    // Ekran koruyucuları ve light-locker'ı tekrar devam ettir (SIGCONT)
    QProcess::startDetached("pkill", {"-CONT", "light-locker"});
    QProcess::startDetached("pkill", {"-CONT", "xfce4-screensaver"});
    QProcess::startDetached("pkill", {"-CONT", "mate-screensaver"});
    QProcess::startDetached("pkill", {"-CONT", "gnome-screensaver"});
    QProcess::startDetached("pkill", {"-CONT", "xscreensaver"});

    // X11 ekran koruyucu ve DPMS ayarlarını geri yükle
    QProcess::startDetached("xset", {"s", "on", "+dpms"});
#endif

    if (m_topTimer) {
        m_topTimer->stop();
    }

    // Ek ekran overlay'lerini kapat
    for (ScreenLockOverlay* w : m_extraScreens) {
        w->hide();
        w->close();
        w->deleteLater();
    }
    m_extraScreens.clear();

    // Ana overlay'i gizle ve kapat
    hide();
    close();
    releaseKeyboard();
    releaseMouse();

    // Fare imlecini geri getir
    unsetCursor();
}

void ScreenLockOverlay::setMessage(const QString& message) {
    m_currentMessage = message;
    ui->messageLabel->setText(message);
    for (ScreenLockOverlay* w : m_extraScreens)
        w->ui->messageLabel->setText(message);
}

// ---------------------------------------------------------------------------
// Ekran kaplama
// ---------------------------------------------------------------------------
void ScreenLockOverlay::coverAllScreens(const QString& message) {
    m_currentMessage = message;
    QList<QScreen*> screens = QApplication::screens();
    // Birincil ekran: bu instance
    coverScreen(0, message);

    // Ek ekranlar: yeni overlay pencereleri
    for (int i = 1; i < screens.size(); ++i) {
        ScreenLockOverlay* extra = new ScreenLockOverlay();
        extra->coverScreen(i, message);
        m_extraScreens.append(extra);
    }

    // Yeni monitör / projeksiyon takıldığında veya çıkarıldığında dinamik güncelle
    static bool s_screensConnected = false;
    if (!s_screensConnected && qApp) {
        s_screensConnected = true;
        connect(qApp, &QGuiApplication::screenAdded, this, &ScreenLockOverlay::onScreenAdded);
        connect(qApp, &QGuiApplication::screenRemoved, this, &ScreenLockOverlay::onScreenRemoved);
    }

#if !defined(Q_OS_WIN)
    // 1. Eğer OS/LightDM ekranı daha önceden kilitlenmişse, önce kilidi serbest bırak
    QProcess::startDetached("loginctl", {"unlock-session"});
    QProcess::startDetached("light-locker-command", {"-d"});

    // 2. light-locker ve tüm masaüstü ekran koruyucularını dondur (SIGSTOP).
    // Böylece Okidia kilitliyken arkada boşta kalma algılayıp LightDM kilit ekranını açamazlar.
    QProcess::startDetached("pkill", {"-STOP", "light-locker"});
    QProcess::startDetached("pkill", {"-STOP", "xfce4-screensaver"});
    QProcess::startDetached("pkill", {"-STOP", "mate-screensaver"});
    QProcess::startDetached("pkill", {"-STOP", "gnome-screensaver"});
    QProcess::startDetached("pkill", {"-STOP", "xscreensaver"});

    // 3. Sanal klavyeleri (ekran klavyesi) öldürerek kilit ekranının üstünde kalmalarını engelle
    int sysRc = system("killall -9 onboard maliit-keyboard florence matchbox-keyboard qtvirtualkeyboard 2>/dev/null");
    Q_UNUSED(sysRc);

    // 4. X11 ekran koruyucu ve DPMS'i askıya al
    QProcess::startDetached("xset", {"s", "off", "-dpms"});
    QProcess::startDetached("xset", {"s", "reset"});
#endif

    // Sürekli olarak en üstte kalmaya zorlamak için bir timer başlat
    if (!m_topTimer) {
        m_topTimer = new QTimer(this);
        connect(m_topTimer, &QTimer::timeout, this, &ScreenLockOverlay::updateScreens);
    }
    m_topTimer->start(2000); // 2 saniyede bir kontrol et ve idle sayacını sıfırla

    // Focus ve grab ana overlay'de
    raise();
    setFocus();
    grabKeyboard();
    grabMouse();

    // Fare imlecini gizle
    setCursor(Qt::BlankCursor);
    for (auto* extra : m_extraScreens)
        extra->setCursor(Qt::BlankCursor);
}

void ScreenLockOverlay::coverScreen(int screenIndex,
                                     const QString& message) {
    QList<QScreen*> screens = QApplication::screens();
    if (screenIndex >= screens.size()) return;

    QRect geom = screens[screenIndex]->geometry();
    setGeometry(geom);
    ui->messageLabel->setText(message);

    showFullScreen();
    raise();
    activateWindow();
}

void ScreenLockOverlay::onScreenAdded(QScreen* screen) {
    Q_UNUSED(screen)
    if (!m_locked) return;
    updateScreens();
}

void ScreenLockOverlay::onScreenRemoved(QScreen* screen) {
    Q_UNUSED(screen)
    if (!m_locked) return;
    updateScreens();
}

void ScreenLockOverlay::updateScreens() {
    if (!m_locked) return;

#if !defined(Q_OS_WIN)
    QProcess::startDetached("xset", {"s", "reset"});
#endif

    QList<QScreen*> screens = QApplication::screens();
    if (screens.isEmpty()) return;

    // 1. Birincil ekranın boyutunu eşitle
    QRect primaryGeom = screens[0]->geometry();
    if (geometry() != primaryGeom) {
        setGeometry(primaryGeom);
    }
    if (isMinimized()) {
        showNormal();
        showFullScreen();
    }
    if (!isActiveWindow()) {
        raise();
        activateWindow();
    }

    // 2. Ek ekranları denetle (eklenen/çıkarılan varsa listeyi senkronize et)
    int requiredExtra = screens.size() - 1;

    // Fazlalık overlay'leri temizle (ekran çıkarıldıysa)
    while (m_extraScreens.size() > requiredExtra) {
        ScreenLockOverlay* extra = m_extraScreens.takeLast();
        if (extra) {
            extra->hide();
            extra->deleteLater();
        }
    }

    // Eksik overlay'leri oluştur (yeni ekran/projeksiyon takıldıysa)
    while (m_extraScreens.size() < requiredExtra) {
        int idx = m_extraScreens.size() + 1;
        auto* extra = new ScreenLockOverlay();
        extra->coverScreen(idx, m_currentMessage);
        extra->setCursor(Qt::BlankCursor);
        m_extraScreens.append(extra);
    }

    // Mevcut ek ekranların geometrilerini ve üstte kalmasını güncelle
    for (int i = 0; i < m_extraScreens.size(); ++i) {
        int screenIdx = i + 1;
        if (screenIdx < screens.size()) {
            QRect geom = screens[screenIdx]->geometry();
            if (m_extraScreens[i]->geometry() != geom) {
                m_extraScreens[i]->setGeometry(geom);
            }
            if (m_extraScreens[i]->isMinimized()) {
                m_extraScreens[i]->showNormal();
                m_extraScreens[i]->showFullScreen();
            }
            if (!m_extraScreens[i]->isActiveWindow()) {
                m_extraScreens[i]->raise();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Olay yutucuları — kilitteyken kullanıcı giriş yapamaz
// ---------------------------------------------------------------------------
bool ScreenLockOverlay::event(QEvent* event) {
    if (m_locked) {
        switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::TabletEnterProximity:
        case QEvent::TabletLeaveProximity:
            event->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void ScreenLockOverlay::keyPressEvent(QKeyEvent* event) {
    event->accept();
}

void ScreenLockOverlay::keyReleaseEvent(QKeyEvent* event) {
    event->accept();
}

void ScreenLockOverlay::mousePressEvent(QMouseEvent* event) {
    event->accept();
}

void ScreenLockOverlay::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
}

void ScreenLockOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    event->accept();
}

void ScreenLockOverlay::closeEvent(QCloseEvent* event) {
    if (m_locked) {
        event->ignore();
        return;
    }
    event->accept();
}
