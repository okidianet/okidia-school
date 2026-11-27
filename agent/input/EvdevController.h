// agent/input/EvdevController.h
#pragma once

#include <QObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>

/**
 * @brief Linux Wayland/Anti-Sabotaj için root servis seviyesinde çalışan
 *        evdev donanım kilidi (EVIOCGRAB) ve uinput sanal girdi enjektörü.
 */
class EvdevController : public QObject {
    Q_OBJECT
public:
    static EvdevController& instance();

    /**
     * @brief Aktif masaüstü oturumunun Wayland kullanıp kullanmadığını kontrol eder.
     */
    static bool isWaylandSessionActive();

    // --- Donanım Girdi Kilidi (EVIOCGRAB) ---
    bool lockInput();
    bool unlockInput();
    bool isInputLocked() const { return m_inputLocked; }

    // --- Uzaktan Kontrol (uinput sanal fare/klavye) ---
    bool injectMouseMove(int x, int y, int screenW, int screenH);
    bool injectMouseClick(int button, bool pressed, int x, int y, int screenW, int screenH);
    bool injectMouseScroll(int delta);
    bool injectKeyEvent(unsigned long keysym, bool pressed);

    void shutdown();

private:
    explicit EvdevController(QObject* parent = nullptr);
    ~EvdevController() override;

    EvdevController(const EvdevController&) = delete;
    EvdevController& operator=(const EvdevController&) = delete;

    void scanAndGrabDevices();
    bool ensureUinputMouse();
    bool ensureUinputKeyboard();
    static int keysymToLinuxKey(unsigned long keysym);

    bool m_inputLocked = false;
    QList<int> m_grabbedFds;
    QSet<QString> m_grabbedPaths;
    QTimer* m_hotplugTimer = nullptr;

    int m_uinputMouseFd = -1;
    int m_uinputKbdFd   = -1;
};

