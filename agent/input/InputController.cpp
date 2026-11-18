// agent/input/InputController.cpp
#include "InputController.h"
#include "../../common/utils/Logger.h"
#include <QObject>
#include <map>
#include <vector>

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
#elif defined(HAVE_XTST)
#  include <X11/Xlib.h>
#  include <X11/keysym.h>
#  include <X11/extensions/XTest.h>
#endif

// Statik üyeler
bool InputController::s_inputLocked = false;

#if !defined(Q_OS_WIN)
Display* InputController::s_grabDisplay   = nullptr;
Display* InputController::s_injectDisplay = nullptr;

Display* InputController::openDisplay() {
#ifdef HAVE_XTST
    return XOpenDisplay(nullptr);
#else
    return nullptr;
#endif
}

Display* InputController::getInjectDisplay() {
#ifdef HAVE_XTST
    if (s_injectDisplay) {
        return s_injectDisplay;
    }
    s_injectDisplay = XOpenDisplay(nullptr);
    return s_injectDisplay;
#else
    return nullptr;
#endif
}

void InputController::closeInjectDisplay() {
#ifdef HAVE_XTST
    if (s_injectDisplay) {
        XCloseDisplay(s_injectDisplay);
        s_injectDisplay = nullptr;
    }
#endif
}

bool InputController::isXAvailable() {
#ifdef HAVE_XTST
    Display* d = openDisplay();
    if (!d) return false;
    XCloseDisplay(d);
    return true;
#else
    return false;
#endif
}

#else // Q_OS_WIN

void InputController::closeInjectDisplay() {
    // Windows tarafında kalıcı display gerekmez
}

bool InputController::isXAvailable() {
    return true; // Windows GDI / Desktop her zaman hazırdır
}

#endif // !defined(Q_OS_WIN)

// ---------------------------------------------------------------------------
// Input kilitleme
// ---------------------------------------------------------------------------
#if defined(Q_OS_WIN)
static HHOOK s_hKeyboardHook = nullptr;
static HHOOK s_hMouseHook = nullptr;

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && InputController::isInputLocked()) {
        return 1; // Girişi yut
    }
    return CallNextHookEx(s_hKeyboardHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && InputController::isInputLocked()) {
        return 1; // Girişi yut
    }
    return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);
}
#endif

bool InputController::lockInput() {
    if (s_inputLocked) return true;

#if defined(Q_OS_WIN)
    s_inputLocked = true;
    if (!s_hKeyboardHook) {
        s_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    }
    if (!s_hMouseHook) {
        s_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
    }
    Logger::info(QObject::tr("InputController: Windows giriş kilitlendi"));
    return true;
#elif defined(HAVE_XTST)
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;

    Window root = DefaultRootWindow(dpy);

    int r1 = XGrabPointer(
        dpy, root,
        False,
        0,
        GrabModeAsync, GrabModeAsync,
        None, None, CurrentTime);

    int r2 = XGrabKeyboard(
        dpy, root,
        False,
        GrabModeAsync, GrabModeAsync,
        CurrentTime);

    if (r1 != GrabSuccess || r2 != GrabSuccess) {
        XUngrabPointer(dpy, CurrentTime);
        XUngrabKeyboard(dpy, CurrentTime);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return false;
    }

    XFlush(dpy);
    s_grabDisplay = dpy;
    s_inputLocked = true;
    Logger::info(QObject::tr("InputController: X11 giriş kilitlendi"));
    return true;
#else
    return false;
#endif
}

bool InputController::unlockInput() {
    if (!s_inputLocked) return false;

#if defined(Q_OS_WIN)
    s_inputLocked = false;
    if (s_hKeyboardHook) {
        UnhookWindowsHookEx(s_hKeyboardHook);
        s_hKeyboardHook = nullptr;
    }
    if (s_hMouseHook) {
        UnhookWindowsHookEx(s_hMouseHook);
        s_hMouseHook = nullptr;
    }
    Logger::info(QObject::tr("InputController: Windows giriş kilidi kaldırıldı"));
    return true;
#elif defined(HAVE_XTST)
    if (!s_grabDisplay) return false;
    XUngrabPointer(s_grabDisplay, CurrentTime);
    XUngrabKeyboard(s_grabDisplay, CurrentTime);
    XFlush(s_grabDisplay);
    XCloseDisplay(s_grabDisplay);
    s_grabDisplay = nullptr;
    s_inputLocked = false;
    Logger::info(QObject::tr("InputController: X11 giriş kilidi kaldırıldı"));
    return true;
#else
    return false;
#endif
}

bool InputController::isInputLocked() {
    return s_inputLocked;
}

// ---------------------------------------------------------------------------
// Enjeksiyon: Klavye
// ---------------------------------------------------------------------------
#if defined(Q_OS_WIN)
static WORD keysymToVk(unsigned long keysym) {
    // Standart X11 / Qt KeySym'lerden Win32 VK eşleşmesi
    if (keysym >= 'a' && keysym <= 'z') return static_cast<WORD>(keysym - 'a' + 'A');
    if (keysym >= 'A' && keysym <= 'Z') return static_cast<WORD>(keysym);
    if (keysym >= '0' && keysym <= '9') return static_cast<WORD>(keysym);

    switch (keysym) {
    case 0xff08: return VK_BACK;       // XK_BackSpace
    case 0xff09: return VK_TAB;        // XK_Tab
    case 0xfe20: return VK_TAB;        // XK_ISO_Left_Tab (Shift+Tab)
    case 0xff0d: return VK_RETURN;     // XK_Return
    case 0xff8d: return VK_RETURN;     // XK_KP_Enter
    case 0xff1b: return VK_ESCAPE;     // XK_Escape
    case 0xff50: return VK_HOME;       // XK_Home
    case 0xff51: return VK_LEFT;       // XK_Left
    case 0xff52: return VK_UP;         // XK_Up
    case 0xff53: return VK_RIGHT;      // XK_Right
    case 0xff54: return VK_DOWN;       // XK_Down
    case 0xff55: return VK_PRIOR;      // XK_Page_Up
    case 0xff56: return VK_NEXT;       // XK_Page_Down
    case 0xff57: return VK_END;        // XK_End
    case 0xff63: return VK_INSERT;     // XK_Insert
    case 0xffff: return VK_DELETE;     // XK_Delete
    case 0xffe1: return VK_LSHIFT;     // XK_Shift_L
    case 0xffe2: return VK_RSHIFT;     // XK_Shift_R
    case 0xffe3: return VK_LCONTROL;   // XK_Control_L
    case 0xffe4: return VK_RCONTROL;   // XK_Control_R
    case 0xffe9: return VK_LMENU;      // XK_Alt_L
    case 0xffea: return VK_RMENU;      // XK_Alt_R
    case 0xfe03: return VK_RMENU;      // XK_ISO_Level3_Shift (AltGr)
    case 0xffeb: return VK_LWIN;       // XK_Super_L
    case 0xffec: return VK_RWIN;       // XK_Super_R
    case 0xffe5: return VK_CAPITAL;    // XK_Caps_Lock
    case 0xff7f: return VK_NUMLOCK;    // XK_Num_Lock
    case 0xff14: return VK_SCROLL;     // XK_Scroll_Lock
    case 0xff61: return VK_SNAPSHOT;   // XK_Print
    case 0xff13: return VK_PAUSE;      // XK_Pause
    case 0xffbe: return VK_F1;
    case 0xffbf: return VK_F2;
    case 0xffc0: return VK_F3;
    case 0xffc1: return VK_F4;
    case 0xffc2: return VK_F5;
    case 0xffc3: return VK_F6;
    case 0xffc4: return VK_F7;
    case 0xffc5: return VK_F8;
    case 0xffc6: return VK_F9;
    case 0xffc7: return VK_F10;
    case 0xffc8: return VK_F11;
    case 0xffc9: return VK_F12;
    case 0x0020: return VK_SPACE;
    // Numpad tuşları
    case 0xffb0: return VK_NUMPAD0;
    case 0xffb1: return VK_NUMPAD1;
    case 0xffb2: return VK_NUMPAD2;
    case 0xffb3: return VK_NUMPAD3;
    case 0xffb4: return VK_NUMPAD4;
    case 0xffb5: return VK_NUMPAD5;
    case 0xffb6: return VK_NUMPAD6;
    case 0xffb7: return VK_NUMPAD7;
    case 0xffb8: return VK_NUMPAD8;
    case 0xffb9: return VK_NUMPAD9;
    case 0xffaa: return VK_MULTIPLY;
    case 0xffab: return VK_ADD;
    case 0xffad: return VK_SUBTRACT;
    case 0xffae: return VK_DECIMAL;
    case 0xffaf: return VK_DIVIDE;
    default:     break;
    }
    return 0;
}
#endif

bool InputController::injectKeyEvent(unsigned long keysym, bool pressed) {
#if defined(Q_OS_WIN)
    WORD vk = keysymToVk(keysym);
    INPUT input = {};
    input.type = INPUT_KEYBOARD;

    if (vk != 0) {
        static bool s_ctrlPressed = false;
        static bool s_altPressed = false;

        if (vk == VK_LCONTROL || vk == VK_RCONTROL) {
            s_ctrlPressed = pressed;
        }
        if (vk == VK_LMENU || vk == VK_RMENU) {
            s_altPressed = pressed;
        }

        // Ctrl+Alt+Del tespiti: Windows'ta SendInput kernel seviyesinde SAS dizisini engeller.
        // okidia-agent SYSTEM servisi olarak çalıştığında sas.dll üzerinden SendSAS(FALSE) tetiklenir.
        if (vk == VK_DELETE && pressed && s_ctrlPressed && s_altPressed) {
            typedef VOID (WINAPI *SendSASFunc)(BOOL AsUser);
            HMODULE hSas = LoadLibraryW(L"sas.dll");
            if (hSas) {
                SendSASFunc pSendSAS = reinterpret_cast<SendSASFunc>(GetProcAddress(hSas, "SendSAS"));
                if (pSendSAS) {
                    pSendSAS(FALSE);
                }
                FreeLibrary(hSas);
            }
        }

        input.ki.wVk = vk;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = pressed ? 0 : KEYEVENTF_KEYUP;
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_UP || vk == VK_DOWN ||
            vk == VK_LEFT || vk == VK_RIGHT || vk == VK_DELETE || vk == VK_INSERT ||
            vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
            vk == VK_RMENU || vk == VK_RCONTROL) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        return SendInput(1, &input, sizeof(INPUT)) > 0;
    } else {
        // Unicode fallback (Shift/AltGr karakterleri, Türkçe harfler, $@½¾ vb.)
        uint32_t codepoint = static_cast<uint32_t>(keysym);
        if ((codepoint & 0xFF000000) == 0x01000000) {
            codepoint &= 0x00FFFFFF;
        }

        if (codepoint > 0xFFFF) {
            WORD high = static_cast<WORD>(0xD800 + ((codepoint - 0x10000) >> 10));
            WORD low  = static_cast<WORD>(0xDC00 + ((codepoint - 0x10000) & 0x3FF));
            INPUT inputs[2] = {};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wScan = high;
            inputs[0].ki.dwFlags = KEYEVENTF_UNICODE | (pressed ? 0 : KEYEVENTF_KEYUP);
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki.wScan = low;
            inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | (pressed ? 0 : KEYEVENTF_KEYUP);
            return SendInput(2, inputs, sizeof(INPUT)) == 2;
        } else {
            input.ki.wVk = 0;
            input.ki.wScan = static_cast<WORD>(codepoint);
            input.ki.dwFlags = KEYEVENTF_UNICODE | (pressed ? 0 : KEYEVENTF_KEYUP);
            return SendInput(1, &input, sizeof(INPUT)) > 0;
        }
    }
#elif defined(HAVE_XTST)
    Display* dpy = getInjectDisplay();
    if (!dpy) return false;

    // 0x01000000 | codepoint formatında gelip Latin-1 (<= 0xFF) aralığında olanları normalize et
    if ((keysym & 0xFF000000) == 0x01000000 && (keysym & 0x00FFFFFF) <= 0x00FF) {
        keysym = keysym & 0x00FFFFFF;
    }

    // 1. Mod tuşları (Shift, Ctrl, Alt, AltGr/ISO_Level3_Shift, Meta, CapsLock, NumLock, ScrollLock)
    bool isModifier = false;
    switch (keysym) {
    case 0xffe1: case 0xffe2: // Shift_L, Shift_R
    case 0xffe3: case 0xffe4: // Control_L, Control_R
    case 0xffe9: case 0xffea: // Alt_L, Alt_R
    case 0xffeb: case 0xffec: // Super_L, Super_R
    case 0xffe5:              // Caps_Lock
    case 0xff7f:              // Num_Lock
    case 0xff14:              // Scroll_Lock
    case 0xfe03: case 0xff7e: // ISO_Level3_Shift, Mode_switch (AltGr)
        isModifier = true;
        break;
    default:
        break;
    }

    if (isModifier) {
        KeyCode kc = XKeysymToKeycode(dpy, keysym);
        if (kc == 0 && (keysym == 0xfe03 || keysym == 0xff7e)) {
            kc = XKeysymToKeycode(dpy, 0xffea); // XK_Alt_R fallback
        }
        if (kc != 0) {
            XTestFakeKeyEvent(dpy, kc, pressed ? True : False, 0);
            XSync(dpy, False);
            return true;
        }
        return false;
    }

    // 2. Özel fonksiyon, navigasyon ve düzenleme tuşları (Enter, BackSpace, Tab, Ok tuşları, F1-F12 vb.)
    bool isSpecialKey = (keysym >= 0xff00 && keysym <= 0xffff);
    if (isSpecialKey) {
        KeyCode kc = XKeysymToKeycode(dpy, keysym);
        if (kc != 0) {
            XTestFakeKeyEvent(dpy, kc, pressed ? True : False, 0);
            XSync(dpy, False);
            return true;
        }
    }

    // 3. Karakterler ve Semboller (ASCII, Latin-1 e.g. $@½¾, Unicode e.g. €₺)
    KeyCode kc = XKeysymToKeycode(dpy, keysym);
    if (kc != 0) {
        // Eğer bu donanımsal tuşun Level 0 (modifiye edilmemiş) değeri tam olarak keysym ile eşleşmiyorsa
        // (örneğin '4' tuşunun AltGr seviyesinde '$' olması gibi), donanımsal tuşu doğrudan tetiklemek
        // aktif modifiye durumuna göre yanlış karakter üretilmesine yol açabilir.
        // Bu durumda dinamik haritalamaya yönlendiriyoruz.
        int nsyms = 0;
        KeySym* ksyms = XGetKeyboardMapping(dpy, kc, 1, &nsyms);
        KeySym level0 = NoSymbol;
        if (ksyms) {
            if (nsyms > 0) level0 = ksyms[0];
            XFree(ksyms);
        }
        if (level0 != keysym) {
            kc = 0;
        }
    }

    if (kc == 0) {
        static std::map<unsigned long, KeyCode> s_dynamicMappings;
        static std::map<KeyCode, unsigned long> s_reverseMappings;
        static int s_nextFreeKeycode = 200; // X11'de genellikle kullanılmayan 200..255 aralığı

        auto it = s_dynamicMappings.find(keysym);
        if (it != s_dynamicMappings.end()) {
            kc = it->second;
        } else {
            kc = static_cast<KeyCode>(s_nextFreeKeycode);
            s_nextFreeKeycode++;
            if (s_nextFreeKeycode > 255) {
                s_nextFreeKeycode = 200;
            }

            auto revIt = s_reverseMappings.find(kc);
            if (revIt != s_reverseMappings.end()) {
                s_dynamicMappings.erase(revIt->second);
            }

            s_dynamicMappings[keysym] = kc;
            s_reverseMappings[kc] = keysym;

            int keysyms_per_keycode = 4;
            int dummy = 0;
            KeySym* cur = XGetKeyboardMapping(dpy, kc, 1, &dummy);
            if (cur) {
                if (dummy > 0) keysyms_per_keycode = dummy;
                XFree(cur);
            }
            if (keysyms_per_keycode < 4) {
                keysyms_per_keycode = 4;
            }

            std::vector<KeySym> syms(keysyms_per_keycode, static_cast<KeySym>(keysym));
            XChangeKeyboardMapping(dpy, kc, keysyms_per_keycode, syms.data(), 1);
            XSync(dpy, False);
        }
    }

    if (kc == 0) return false;

    XTestFakeKeyEvent(dpy, kc, pressed ? True : False, 0);
    XSync(dpy, False);
    return true;
#else
    Q_UNUSED(keysym) Q_UNUSED(pressed)
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Enjeksiyon: Fare Hareketi
// ---------------------------------------------------------------------------
bool InputController::injectMouseMove(int x, int y) {
#if defined(Q_OS_WIN)
    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int screenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (screenWidth <= 0 || screenHeight <= 0) {
        screenWidth = GetSystemMetrics(SM_CXSCREEN);
        screenHeight = GetSystemMetrics(SM_CYSCREEN);
        screenLeft = 0;
        screenTop = 0;
    }

    double normalizedX = ((x - screenLeft) * 65535.0) / screenWidth;
    double normalizedY = ((y - screenTop) * 65535.0) / screenHeight;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(normalizedX);
    input.mi.dy = static_cast<LONG>(normalizedY);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    return SendInput(1, &input, sizeof(INPUT)) > 0;
#elif defined(HAVE_XTST)
    Display* dpy = getInjectDisplay();
    if (!dpy) return false;

    XTestFakeMotionEvent(dpy, -1, x, y, 0);
    XFlush(dpy);
    return true;
#else
    Q_UNUSED(x) Q_UNUSED(y)
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Enjeksiyon: Fare Tıklaması
// ---------------------------------------------------------------------------
bool InputController::injectMouseClick(int button, bool pressed, int x, int y) {
#if defined(Q_OS_WIN)
    INPUT input = {};
    input.type = INPUT_MOUSE;

    switch (button) {
    case 1: // Sol
        input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 2: // Orta
        input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 3: // Sağ
        input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    default:
        return false;
    }

    // Windows 11 DWM hit-testing: Eğer cihaz başında fiziksel fare hareketi yoksa
    // koordinatsız SendInput tıklamaları DWM tarafından pencerelere iletilmeyebilir.
    // Tıklamayla birlikte mutlak koordinatları göndermek doğru pencerenin tetiklenmesini sağlar.
    if (x >= 0 && y >= 0) {
        int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int screenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int screenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

        if (screenWidth <= 0 || screenHeight <= 0) {
            screenWidth = GetSystemMetrics(SM_CXSCREEN);
            screenHeight = GetSystemMetrics(SM_CYSCREEN);
            screenLeft = 0;
            screenTop = 0;
        }

        double normalizedX = ((x - screenLeft) * 65535.0) / screenWidth;
        double normalizedY = ((y - screenTop) * 65535.0) / screenHeight;

        input.mi.dx = static_cast<LONG>(normalizedX);
        input.mi.dy = static_cast<LONG>(normalizedY);
        input.mi.dwFlags |= (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK);
    }

    return SendInput(1, &input, sizeof(INPUT)) > 0;
#elif defined(HAVE_XTST)
    Display* dpy = getInjectDisplay();
    if (!dpy) return false;

    if (x >= 0 && y >= 0) {
        XTestFakeMotionEvent(dpy, -1, x, y, 0);
    }
    XTestFakeButtonEvent(dpy, button, pressed ? True : False, 0);
    XSync(dpy, False);
    return true;
#else
    Q_UNUSED(button) Q_UNUSED(pressed) Q_UNUSED(x) Q_UNUSED(y)
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Enjeksiyon: Fare Tekerleği
// ---------------------------------------------------------------------------
bool InputController::injectMouseScroll(int delta) {
#if defined(Q_OS_WIN)
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta * WHEEL_DELTA);

    return SendInput(1, &input, sizeof(INPUT)) > 0;
#elif defined(HAVE_XTST)
    Display* dpy = getInjectDisplay();
    if (!dpy) return false;

    int btn = (delta > 0) ? 4 : 5;
    int steps = qAbs(delta);

    for (int i = 0; i < steps; ++i) {
        XTestFakeButtonEvent(dpy, btn, True,  0);
        XTestFakeButtonEvent(dpy, btn, False, 0);
    }
    XSync(dpy, False);
    return true;
#else
    Q_UNUSED(delta)
    return false;
#endif
}