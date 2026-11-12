// agent/commands/CommandHandlerInput.cpp
#include "CommandHandler.h"
#include "../../common/utils/Logger.h"
#include "../input/InputController.h"
#include "../capture/ScreenCapture.h"
#include "../system/SystemInfo.h"

#ifdef HAVE_XTST
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/Xatom.h>
#endif
void CommandHandler::inputLock() {
    Logger::info(QObject::tr("CommandHandler: Input kilitleniyor"));
    bool ok = InputController::lockInput();
    if (ok) {
        Logger::info(QObject::tr("CommandHandler: Input kilitlendi"));
        sendResult(true, "Input kilitlendi");

        // Fare imlecini gizle (X11)
#ifdef HAVE_XTST
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy) {
            // Görünmez imleç oluştur
            Pixmap blank = XCreatePixmap(dpy, DefaultRootWindow(dpy), 1, 1, 1);
            XColor dummy = {};
            Cursor invisCursor = XCreatePixmapCursor(
                dpy, blank, blank, &dummy, &dummy, 0, 0);
            XDefineCursor(dpy, DefaultRootWindow(dpy), invisCursor);
            XFreeCursor(dpy, invisCursor);
            XFreePixmap(dpy, blank);
            XFlush(dpy);
            XCloseDisplay(dpy);
        }
#endif
    } else {
        Logger::warning(QObject::tr("CommandHandler: Input kilitleme başarısız"));
        sendResult(false, QObject::tr("Input kilitleme başarısız (XTest yok?)"));
    }
}

void CommandHandler::inputUnlock() {
    Logger::info(QObject::tr("CommandHandler: Input kilidi açılıyor"));
    bool ok = InputController::unlockInput();
    sendResult(ok, ok ? QObject::tr("Input kilidi açıldı") : QObject::tr("Input kilidi açılamadı"));

    // Fare imlecini geri getir
#ifdef HAVE_XTST
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy) {
        XUndefineCursor(dpy, DefaultRootWindow(dpy));
        XFlush(dpy);
        XCloseDisplay(dpy);
    }
#endif
}

void CommandHandler::inputTakeControl() {
    Logger::info(QObject::tr("CommandHandler: Uzak kontrol moduna geçiliyor"));
    if (InputController::isInputLocked())
        InputController::unlockInput();

    m_remoteControlActive = true;
    m_screenStreamActive  = true;
    if (m_capture) {
        m_capture->setDrawCursor(false); // Master yerel imlece sahip olduğundan akış üzerinde çift imleç oluşmasını engelle
        CaptureSettings settings = m_capture->settings();
        if (settings.maxWidth <= 0 || settings.scalePercent <= 25) {
            settings.maxWidth = 1920;
            settings.scalePercent = 100;
            settings.jpegQuality = 80;
            settings.fps = 20;
            m_capture->updateSettings(settings);
        }
    }
    sendResult(true, tr("Uzak kontrol aktif"));
}

void CommandHandler::remoteControlStop() {
    Logger::info(QObject::tr("CommandHandler: Uzak kontrol sonlandırılıyor"));
    m_remoteControlActive = false;
    m_screenStreamActive  = false;
    if (m_capture) {
        m_capture->setDrawCursor(true); // İzleme ve thumbnail için imleç çizimini yeniden aktif et
        CaptureSettings settings = m_capture->settings();
        settings.maxWidth = 0;
        settings.scalePercent = 25;
        settings.jpegQuality = 60;
        settings.fps = 5;
        m_capture->updateSettings(settings);
        m_capture->resetPreviousFrame();
    }
    sendResult(true, tr("Uzak kontrol durduruldu"));
}

void CommandHandler::handleMouseEvent(const okidia::MouseEvent& evt) {
    int x = evt.x();
    int y = evt.y();

    // Uzak kontrol koordinatlarını scaled çözünürlükten native çözünürlüğe ölçekle
    if (m_capture) {
        int scaledW = m_capture->scaledWidth();
        int scaledH = m_capture->scaledHeight();
        int nativeW = m_capture->screenWidth();
        int nativeH = m_capture->screenHeight();

        if (scaledW > 0 && scaledH > 0 && nativeW > 0 && nativeH > 0 &&
            (scaledW != nativeW || scaledH != nativeH)) {
            x = static_cast<int>(static_cast<qint64>(x) * nativeW / scaledW);
            y = static_cast<int>(static_cast<qint64>(y) * nativeH / scaledH);
        }
    }

    if (evt.is_move()) {
        InputController::injectMouseMove(x, y);
    } else if (evt.is_scroll()) {
        InputController::injectMouseScroll(evt.delta());
    } else {
        InputController::injectMouseClick(evt.button(), evt.pressed(), x, y);
    }
}

void CommandHandler::handleKeyEvent(const okidia::KeyEvent& evt) {
    InputController::injectKeyEvent(evt.keysym(), evt.pressed());
}

// ---------------------------------------------------------------------------
// Güç komutları
// ---------------------------------------------------------------------------
