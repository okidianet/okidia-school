// agent/input/EvdevController.cpp
#include "EvdevController.h"
#include "../../common/utils/Logger.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <cstring>
#endif

namespace {
#if defined(Q_OS_LINUX)
void emitEv(int fd, uint16_t type, uint16_t code, int32_t value) {
    if (fd < 0) return;
    struct input_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t unused = ::write(fd, &ev, sizeof(ev));
    Q_UNUSED(unused);
}

void emitSyn(int fd) {
    emitEv(fd, EV_SYN, SYN_REPORT, 0);
}
#endif
} // namespace

EvdevController& EvdevController::instance() {
    static EvdevController s_inst;
    return s_inst;
}

EvdevController::EvdevController(QObject* parent)
    : QObject(parent) {
    m_hotplugTimer = new QTimer(this);
    m_hotplugTimer->setInterval(2000);
    connect(m_hotplugTimer, &QTimer::timeout, this, &EvdevController::scanAndGrabDevices);
}

EvdevController::~EvdevController() {
    shutdown();
}

void EvdevController::shutdown() {
    unlockInput();
#if defined(Q_OS_LINUX)
    if (m_uinputMouseFd >= 0) {
        ::ioctl(m_uinputMouseFd, UI_DEV_DESTROY);
        ::close(m_uinputMouseFd);
        m_uinputMouseFd = -1;
    }
    if (m_uinputKbdFd >= 0) {
        ::ioctl(m_uinputKbdFd, UI_DEV_DESTROY);
        ::close(m_uinputKbdFd);
        m_uinputKbdFd = -1;
    }
#endif
}

bool EvdevController::isWaylandSessionActive() {
#if defined(Q_OS_LINUX)
    QProcess pSeat;
    pSeat.start(QStringLiteral("loginctl"), {QStringLiteral("show-seat"), QStringLiteral("seat0"), QStringLiteral("--property=ActiveSession"), QStringLiteral("--value")});
    if (pSeat.waitForFinished(400) && pSeat.exitCode() == 0) {
        const QString sessId = QString::fromLatin1(pSeat.readAllStandardOutput()).trimmed();
        if (!sessId.isEmpty() && sessId != QLatin1String("0") && sessId != QLatin1String("(null)")) {
            QProcess pType;
            pType.start(QStringLiteral("loginctl"), {QStringLiteral("show-session"), sessId, QStringLiteral("--property=Type"), QStringLiteral("--value")});
            if (pType.waitForFinished(400) && pType.exitCode() == 0) {
                const QString t = QString::fromLatin1(pType.readAllStandardOutput()).trimmed();
                if (t.compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0) {
                    return true;
                }
                if (t.compare(QStringLiteral("x11"), Qt::CaseInsensitive) == 0) {
                    return false;
                }
            }
        }
    }

    // Fallback: /run/user/*/wayland-* soket kontrolü
    QDir runUser(QStringLiteral("/run/user"));
    for (const QString& uidDir : runUser.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir d(runUser.absoluteFilePath(uidDir));
        const QStringList wl = d.entryList(QStringList() << QStringLiteral("wayland-*"), QDir::System | QDir::Files);
        for (const QString& s : wl) {
            if (!s.endsWith(QStringLiteral(".lock"))) {
                return true;
            }
        }
    }
#endif
    return false;
}

bool EvdevController::lockInput() {
#if !defined(Q_OS_LINUX)
    return false;
#else
    if (m_inputLocked) return true;
    m_inputLocked = true;
    scanAndGrabDevices();
    if (!m_hotplugTimer->isActive()) {
        m_hotplugTimer->start();
    }
    Logger::info(QObject::tr("EvdevController: Donanım girdi kilidi (evdev EVIOCGRAB) etkinleştirildi (%1 cihaz).")
                 .arg(m_grabbedFds.size()));
    return !m_grabbedFds.isEmpty();
#endif
}

bool EvdevController::unlockInput() {
#if !defined(Q_OS_LINUX)
    return false;
#else
    m_inputLocked = false;
    if (m_hotplugTimer->isActive()) {
        m_hotplugTimer->stop();
    }
    for (int fd : m_grabbedFds) {
        if (fd >= 0) {
            ::ioctl(fd, EVIOCGRAB, 0);
            ::close(fd);
        }
    }
    m_grabbedFds.clear();
    m_grabbedPaths.clear();
    Logger::info(QObject::tr("EvdevController: Donanım girdi kilidi (evdev) kaldırıldı."));
    return true;
#endif
}

void EvdevController::scanAndGrabDevices() {
#if defined(Q_OS_LINUX)
    if (!m_inputLocked) return;

    QDir inputDir(QStringLiteral("/dev/input"));
    const QStringList events = inputDir.entryList(QStringList() << QStringLiteral("event*"), QDir::System | QDir::Files);
    for (const QString& name : events) {
        const QString fullPath = inputDir.absoluteFilePath(name);
        if (m_grabbedPaths.contains(fullPath)) continue;

        int fd = ::open(fullPath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char devName[256] = {0};
        if (::ioctl(fd, EVIOCGNAME(sizeof(devName)), devName) >= 0) {
            if (std::strstr(devName, "OKIDIA Virtual") != nullptr ||
                std::strstr(devName, "Power Button") != nullptr ||
                std::strstr(devName, "Sleep Button") != nullptr ||
                std::strstr(devName, "Video Bus") != nullptr ||
                std::strstr(devName, "HDA ") != nullptr ||
                std::strstr(devName, "HDMI") != nullptr ||
                std::strstr(devName, "PC Speaker") != nullptr) {
                ::close(fd);
                continue;
            }
        }

        unsigned long evBits = 0;
        if (::ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), &evBits) >= 0) {
            const bool hasKey = (evBits & (1UL << EV_KEY)) != 0;
            const bool hasRel = (evBits & (1UL << EV_REL)) != 0;
            const bool hasAbs = (evBits & (1UL << EV_ABS)) != 0;
            if (!hasKey && !hasRel && !hasAbs) {
                ::close(fd);
                continue;
            }
        }

        if (::ioctl(fd, EVIOCGRAB, 1) == 0) {
            m_grabbedFds.append(fd);
            m_grabbedPaths.insert(fullPath);
        } else {
            ::close(fd);
        }
    }
#endif
}

bool EvdevController::ensureUinputMouse() {
#if !defined(Q_OS_LINUX)
    return false;
#else
    if (m_uinputMouseFd >= 0) return true;

    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        Logger::warning(QObject::tr("EvdevController: /dev/uinput (fare) açılamadı."));
        return false;
    }

    ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ::ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ::ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    ::ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ::ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ::ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    ::ioctl(fd, UI_SET_EVBIT, EV_REL);
    ::ioctl(fd, UI_SET_RELBIT, REL_WHEEL);

    struct uinput_abs_setup absX;
    std::memset(&absX, 0, sizeof(absX));
    absX.code = ABS_X;
    absX.absinfo.minimum = 0;
    absX.absinfo.maximum = 32767;
    ::ioctl(fd, UI_ABS_SETUP, &absX);

    struct uinput_abs_setup absY;
    std::memset(&absY, 0, sizeof(absY));
    absY.code = ABS_Y;
    absY.absinfo.minimum = 0;
    absY.absinfo.maximum = 32767;
    ::ioctl(fd, UI_ABS_SETUP, &absY);

    struct uinput_setup usetup;
    std::memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    std::strncpy(usetup.name, "OKIDIA Virtual Mouse", UINPUT_MAX_NAME_SIZE - 1);

    if (::ioctl(fd, UI_DEV_SETUP, &usetup) < 0 || ::ioctl(fd, UI_DEV_CREATE) < 0) {
        ::close(fd);
        Logger::warning(QObject::tr("EvdevController: Sanal fare (uinput) oluşturulamadı."));
        return false;
    }

    m_uinputMouseFd = fd;
    Logger::info(QObject::tr("EvdevController: Sanal fare (uinput) hazır."));
    return true;
#endif
}

bool EvdevController::ensureUinputKeyboard() {
#if !defined(Q_OS_LINUX)
    return false;
#else
    if (m_uinputKbdFd >= 0) return true;

    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        Logger::warning(QObject::tr("EvdevController: /dev/uinput (klavye) açılamadı."));
        return false;
    }

    ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int k = 1; k < 248; ++k) {
        ::ioctl(fd, UI_SET_KEYBIT, k);
    }

    struct uinput_setup usetup;
    std::memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5679;
    std::strncpy(usetup.name, "OKIDIA Virtual Keyboard", UINPUT_MAX_NAME_SIZE - 1);

    if (::ioctl(fd, UI_DEV_SETUP, &usetup) < 0 || ::ioctl(fd, UI_DEV_CREATE) < 0) {
        ::close(fd);
        Logger::warning(QObject::tr("EvdevController: Sanal klavye (uinput) oluşturulamadı."));
        return false;
    }

    m_uinputKbdFd = fd;
    Logger::info(QObject::tr("EvdevController: Sanal klavye (uinput) hazır."));
    return true;
#endif
}

bool EvdevController::injectMouseMove(int x, int y, int screenW, int screenH) {
#if !defined(Q_OS_LINUX)
    Q_UNUSED(x); Q_UNUSED(y); Q_UNUSED(screenW); Q_UNUSED(screenH);
    return false;
#else
    if (!ensureUinputMouse()) return false;
    if (screenW <= 0) screenW = 1920;
    if (screenH <= 0) screenH = 1080;

    int absX = qBound(0, static_cast<int>(static_cast<qint64>(x) * 32767 / screenW), 32767);
    int absY = qBound(0, static_cast<int>(static_cast<qint64>(y) * 32767 / screenH), 32767);

    emitEv(m_uinputMouseFd, EV_ABS, ABS_X, absX);
    emitEv(m_uinputMouseFd, EV_ABS, ABS_Y, absY);
    emitSyn(m_uinputMouseFd);
    return true;
#endif
}

bool EvdevController::injectMouseClick(int button, bool pressed, int x, int y, int screenW, int screenH) {
#if !defined(Q_OS_LINUX)
    Q_UNUSED(button); Q_UNUSED(pressed); Q_UNUSED(x); Q_UNUSED(y); Q_UNUSED(screenW); Q_UNUSED(screenH);
    return false;
#else
    if (!ensureUinputMouse()) return false;
    if (x >= 0 && y >= 0) {
        injectMouseMove(x, y, screenW, screenH);
    }

    uint16_t btnCode = BTN_LEFT;
    if (button == 1) btnCode = BTN_LEFT;
    else if (button == 2) btnCode = BTN_MIDDLE;
    else if (button == 3) btnCode = BTN_RIGHT;

    emitEv(m_uinputMouseFd, EV_KEY, btnCode, pressed ? 1 : 0);
    emitSyn(m_uinputMouseFd);
    return true;
#endif
}

bool EvdevController::injectMouseScroll(int delta) {
#if !defined(Q_OS_LINUX)
    Q_UNUSED(delta);
    return false;
#else
    if (!ensureUinputMouse()) return false;
    int step = (delta > 0) ? 1 : -1;
    emitEv(m_uinputMouseFd, EV_REL, REL_WHEEL, step);
    emitSyn(m_uinputMouseFd);
    return true;
#endif
}

int EvdevController::keysymToLinuxKey(unsigned long keysym) {
#if !defined(Q_OS_LINUX)
    Q_UNUSED(keysym);
    return 0;
#else
    // ASCII harfler (a-z, A-Z)
    if (keysym >= 'a' && keysym <= 'z') {
        static const int mapAZ[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
            KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
            KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
        };
        return mapAZ[keysym - 'a'];
    }
    if (keysym >= 'A' && keysym <= 'Z') {
        return keysymToLinuxKey(keysym + ('a' - 'A'));
    }
    if (keysym >= '0' && keysym <= '9') {
        static const int map09[10] = {
            KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9
        };
        return map09[keysym - '0'];
    }

    switch (keysym) {
    case 0x0020: return KEY_SPACE;
    case 0x002d: return KEY_MINUS;
    case 0x003d: return KEY_EQUAL;
    case 0x002c: return KEY_COMMA;
    case 0x002e: return KEY_DOT;
    case 0x002f: return KEY_SLASH;
    case 0x003b: return KEY_SEMICOLON;
    case 0x0027: return KEY_APOSTROPHE;
    case 0x005b: return KEY_LEFTBRACE;
    case 0x005d: return KEY_RIGHTBRACE;
    case 0x005c: return KEY_BACKSLASH;
    case 0xff08: return KEY_BACKSPACE;
    case 0xff09: return KEY_TAB;
    case 0xff0d: return KEY_ENTER;
    case 0xff1b: return KEY_ESC;
    case 0xffff: return KEY_DELETE;
    case 0xff50: return KEY_HOME;
    case 0xff51: return KEY_LEFT;
    case 0xff52: return KEY_UP;
    case 0xff53: return KEY_RIGHT;
    case 0xff54: return KEY_DOWN;
    case 0xff55: return KEY_PAGEUP;
    case 0xff56: return KEY_PAGEDOWN;
    case 0xff57: return KEY_END;
    case 0xff63: return KEY_INSERT;
    case 0xffe1: return KEY_LEFTSHIFT;
    case 0xffe2: return KEY_RIGHTSHIFT;
    case 0xffe3: return KEY_LEFTCTRL;
    case 0xffe4: return KEY_RIGHTCTRL;
    case 0xffe5: return KEY_CAPSLOCK;
    case 0xffe9: return KEY_LEFTALT;
    case 0xffea: return KEY_RIGHTALT;
    case 0xffeb: return KEY_LEFTMETA;
    case 0xffec: return KEY_RIGHTMETA;
    case 0xffbe: return KEY_F1;
    case 0xffbf: return KEY_F2;
    case 0xffc0: return KEY_F3;
    case 0xffc1: return KEY_F4;
    case 0xffc2: return KEY_F5;
    case 0xffc3: return KEY_F6;
    case 0xffc4: return KEY_F7;
    case 0xffc5: return KEY_F8;
    case 0xffc6: return KEY_F9;
    case 0xffc7: return KEY_F10;
    case 0xffc8: return KEY_F11;
    case 0xffc9: return KEY_F12;
    default: return 0;
    }
#endif
}

bool EvdevController::injectKeyEvent(unsigned long keysym, bool pressed) {
#if !defined(Q_OS_LINUX)
    Q_UNUSED(keysym); Q_UNUSED(pressed);
    return false;
#else
    if (!ensureUinputKeyboard()) return false;
    int code = keysymToLinuxKey(keysym);
    if (code <= 0) return false;

    emitEv(m_uinputKbdFd, EV_KEY, static_cast<uint16_t>(code), pressed ? 1 : 0);
    emitSyn(m_uinputKbdFd);
    return true;
#endif
}
