// agent/input/InputController.h
#pragma once

#include <QString>

#if !defined(Q_OS_WIN)
// Forward declaration — X11 tipler sadece .cpp'de include edilir
struct _XDisplay;
typedef struct _XDisplay Display;
#endif

class InputController {
public:
    // --- Input kilitleme ---
    static bool lockInput();
    static bool unlockInput();
    static bool isInputLocked();

    // --- Uzak kontrol: olay enjeksiyonu ---
    // keysym: X11 KeySym veya standart sanal tuş değeri
    static bool injectKeyEvent(unsigned long keysym, bool pressed);
    static bool injectMouseMove(int x, int y);
    static bool injectMouseClick(int button, bool pressed, int x = -1, int y = -1);
    static bool injectMouseScroll(int delta);  // delta>0: yukarı, <0: aşağı

    // --- Yardımcı ---
    static bool isXAvailable();
    static void closeInjectDisplay();

private:
    static bool s_inputLocked;

#if !defined(Q_OS_WIN)
    static Display* s_grabDisplay;
    static Display* s_injectDisplay;
    static Display* getInjectDisplay();
    static Display* openDisplay();
#endif
};