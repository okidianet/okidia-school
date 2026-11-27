// agent/broadcast/ScreenBroadcastReceiver.cpp
#include "ScreenBroadcastReceiver.h"
#include "../../common/utils/Logger.h"
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

static HHOOK s_hBroadcastKbHook = nullptr;
static LRESULT CALLBACK BroadcastKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool isAlt = (p->flags & LLKHF_ALTDOWN) != 0;
        if (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) return 1;
        if (isAlt && (p->vkCode == VK_TAB || p->vkCode == VK_ESCAPE || p->vkCode == VK_F4)) return 1;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && p->vkCode == VK_ESCAPE) return 1;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && p->vkCode == VK_ESCAPE) return 1;
    }
    return CallNextHookEx(s_hBroadcastKbHook, nCode, wParam, lParam);
}
#else
#  include "../capture/WaylandPortalCapture.h"
#endif

#include <QKeyEvent>
#include <QCloseEvent>
#include <QPainter>
#include <QLabel>
#include <QVBoxLayout>
#include <QApplication>
#include <QScreen>

// ---------------------------------------------------------------------------
ScreenBroadcastReceiver& ScreenBroadcastReceiver::instance() {
    static ScreenBroadcastReceiver s;
    return s;
}

ScreenBroadcastReceiver::ScreenBroadcastReceiver(QWidget* parent)
    : QWidget(parent,
              Qt::Window
            | Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
#if !defined(Q_OS_WIN)
            | (WaylandPortalCapture::isWaylandSession() ? Qt::WindowFlags() : Qt::WindowFlags(Qt::X11BypassWindowManagerHint))
#endif
            ) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setStyleSheet("background: #000000;");
    setFocusPolicy(Qt::StrongFocus);
}

// ---------------------------------------------------------------------------
void ScreenBroadcastReceiver::handleEnvelope(
    const okidia::Envelope& envelope) {
    switch (envelope.type()) {
    case okidia::MessageType::BROADCAST_BEGIN:
        handleBegin();
        break;
    case okidia::MessageType::BROADCAST_FRAME: {
        okidia::BroadcastFrame msg;
        if (msg.ParseFromString(envelope.payload()))
            handleFrame(msg);
        break;
    }
    case okidia::MessageType::BROADCAST_END:
        handleEnd();
        break;
    default:
        break;
    }
}

void ScreenBroadcastReceiver::handleBegin() {
    m_isBroadcasting = true;
    Logger::info(QObject::tr("ScreenBroadcastReceiver: Ekran yayını başladı"));
#if defined(Q_OS_WIN)
    if (!s_hBroadcastKbHook) {
        s_hBroadcastKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, BroadcastKeyboardProc, GetModuleHandle(NULL), 0);
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
#else
    WaylandPortalCapture::setGnomeLockdown(true);
#endif
    QRect geom = QApplication::primaryScreen()->geometry();
    setGeometry(geom);
    showFullScreen();
    raise();
    activateWindow();
    setFocus();
#if defined(Q_OS_WIN)
    SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#endif
    emit broadcastStarted();
}

void ScreenBroadcastReceiver::handleFrame(
    const okidia::BroadcastFrame& msg) {
    const auto& jpegStr = msg.jpeg_data();
    if (jpegStr.empty()) return;

    // Latest-Frame-Only drop: Eğer GUI iş parçacığı henüz önceki kareyi
    // çizmekle meşgulse (renderPending), ara kareleri decode etmeden atla;
    // doğrudan en son gelen baytları sakla.
    m_pendingJpegData = QByteArray(jpegStr.data(), static_cast<int>(jpegStr.size()));
    if (!m_renderPending) {
        m_renderPending = true;
        // Bir sonraki event loop döngüsünde decode & paint
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_pendingJpegData.isEmpty()) {
                QPixmap px;
                if (px.loadFromData(m_pendingJpegData, "JPEG")) {
                    m_frame = px;
                    update(); // paintEvent tetikle
                }
                m_pendingJpegData.clear();
            }
            m_renderPending = false;
        }, Qt::QueuedConnection);
    }
}

void ScreenBroadcastReceiver::handleEnd() {
    if (!m_isBroadcasting && isHidden()) return;
    m_isBroadcasting = false;
    Logger::info(QObject::tr("ScreenBroadcastReceiver: Ekran yayını bitti"));
#if defined(Q_OS_WIN)
    if (s_hBroadcastKbHook) {
        UnhookWindowsHookEx(s_hBroadcastKbHook);
        s_hBroadcastKbHook = nullptr;
    }
    SetThreadExecutionState(ES_CONTINUOUS);
#else
    WaylandPortalCapture::setGnomeLockdown(false);
#endif
    m_frame = QPixmap();
    m_pendingJpegData.clear();
    m_renderPending = false;
    hide();
    emit broadcastEnded();
}

// ---------------------------------------------------------------------------
void ScreenBroadcastReceiver::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (!m_frame.isNull()) {
        QPixmap scaled = m_frame.scaled(
            size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        int x = (width()  - scaled.width())  / 2;
        int y = (height() - scaled.height()) / 2;
        p.drawPixmap(x, y, scaled);
    } else {
        p.setPen(QColor("#6c7086"));
        p.setFont(QFont("sans", 16));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("Öğretmenin ekranı bekleniyor..."));
    }
}

void ScreenBroadcastReceiver::keyPressEvent(QKeyEvent* event) {
    // Yayın sırasında tüm klavye girişlerini yut (Öğrenci kaçamaz)
    event->accept();
}

void ScreenBroadcastReceiver::closeEvent(QCloseEvent* event) {
    event->ignore(); // Öğrenci kapatamaz
}
