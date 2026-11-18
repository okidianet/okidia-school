// agent/commands/CommandHandlerApp.cpp
#include "CommandHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../system/SystemInfo.h"
#include "../ui/AgentTray/AgentTray.h"
#include <QProcess>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <QObject>

#if defined(Q_OS_WIN)
#  ifndef WINNT
#    define WINNT 1
#  endif
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
#  include <psapi.h>
#  include <tlhelp32.h>
// MinGW winuser.h içinde EndTask WINNT koruması altındadır.
extern "C" WINUSERAPI WINBOOL WINAPI EndTask(HWND hWnd, WINBOOL fShutDown, WINBOOL fForce);
#else
#  include <sys/types.h>
#  include <signal.h>
#endif
#if defined(HAVE_XTST)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <X11/Xatom.h>
#endif

#if defined(Q_OS_WIN)
struct WindowEnumData {
    okidia::AppListResponse* resp;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Uzunluğu 0 olan veya görünmeyen başlıkları atla
    wchar_t titleBuf[512] = {0};
    int len = GetWindowTextW(hwnd, titleBuf, 512);
    if (len == 0) return TRUE;

    // Tooltip veya popup gibi stilleri filtrele
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

    QString title = QString::fromWCharArray(titleBuf);
    if (title.trimmed().isEmpty()) return TRUE;

    QString exeName;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t exeBuf[MAX_PATH] = {0};
        DWORD exeLen = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exeBuf, &exeLen)) {
            exeName = QFileInfo(QString::fromWCharArray(exeBuf)).fileName();
        }
        CloseHandle(hProc);
    }

    static const QStringList skipExe = {
        "okidia-agent.exe", "okidia-tray.exe", "explorer.exe",
        "ShellExperienceHost.exe", "SearchHost.exe", "StartMenuExperienceHost.exe",
        "TextInputHost.exe", "SystemSettings.exe", "ApplicationFrameHost.exe",
        "okidia-master.exe", "okidia-broker.exe"
    };
    if (skipExe.contains(exeName, Qt::CaseInsensitive)) return TRUE;

    auto* data = reinterpret_cast<WindowEnumData*>(lParam);
    auto* app = data->resp->add_apps();
    app->set_name(title.toUtf8().toStdString());
    app->set_wid(reinterpret_cast<uint64_t>(hwnd));
    app->set_pid(QString::number(pid).toStdString());
    app->set_exe(exeName.toStdString());

    return TRUE;
}
#endif

void CommandHandler::appListRequest() {
    Logger::info(QObject::tr("CommandHandler: Uygulama listesi isteniyor"));

    okidia::AppListResponse resp;

#if defined(Q_OS_WIN)
    WindowEnumData enumData;
    enumData.resp = &resp;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&enumData));
#elif defined(HAVE_XTST)
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy) {
        // _NET_CLIENT_LIST ile tüm pencereleri al
        Atom netClientList = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
        Atom actualType; int actualFmt; ulong nItems, bytesAfter;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(dpy, DefaultRootWindow(dpy),
                netClientList, 0, 1024, False, XA_WINDOW,
                &actualType, &actualFmt, &nItems,
                &bytesAfter, &data) == Success && data) {

            auto* wins = reinterpret_cast<Window*>(data);
            for (ulong i = 0; i < nItems; ++i) {
                Window w = wins[i];

                // Görünür mü?
                XWindowAttributes attrs;
                if (!XGetWindowAttributes(dpy, w, &attrs) ||
                    attrs.map_state != IsViewable) continue;

                // Başlık al (_NET_WM_NAME önce, sonra XFetchName)
                QString title;
                Atom netWmName = XInternAtom(dpy, "_NET_WM_NAME", False);
                Atom utf8Str   = XInternAtom(dpy, "UTF8_STRING", False);
                unsigned char* prop = nullptr;
                ulong ni, ba; int af; Atom at;

                if (XGetWindowProperty(dpy, w, netWmName, 0, 1024,
                        False, utf8Str, &at, &af, &ni, &ba, &prop)
                        == Success && prop) {
                    title = QString::fromUtf8(
                        reinterpret_cast<const char*>(prop), ni);
                    XFree(prop);
                }
                if (title.isEmpty()) {
                    char* name = nullptr;
                    if (XFetchName(dpy, w, &name) && name) {
                        title = QString::fromLocal8Bit(name);
                        XFree(name);
                    }
                }
                if (title.isEmpty()) continue;

                // PID al (_NET_WM_PID)
                Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
                QString pidStr;
                unsigned char* pidData = nullptr;
                if (XGetWindowProperty(dpy, w, pidAtom, 0, 1, False,
                        XA_CARDINAL, &at, &af, &ni, &ba, &pidData)
                        == Success && pidData) {
                    ulong pid = *reinterpret_cast<ulong*>(pidData);
                    pidStr = QString::number(pid);
                    XFree(pidData);
                }

                // exe adı /proc/PID/comm'den al
                QString exe;
                if (!pidStr.isEmpty()) {
                    QFile comm("/proc/" + pidStr + "/comm");
                    if (comm.open(QIODevice::ReadOnly))
                        exe = comm.readAll().trimmed();
                }

                // Sistem ve gizlenmesi gereken uygulamaları filtrele
                static const QStringList skipExe = {
                    "okidia-agent", "plasmashell", "kwin_x11", "kwin_wayland",
                    "gnome-shell", "xfwm4", "openbox", "marco", "mutter",
                    "xfdesktop", "nautilus-desktop", "pcmanfm",
                    "xfce4-panel", "lxpanel", "tint2", "polybar", "waybar",
                    "ibus-daemon", "ibus-x11", "fcitx", "fcitx5",
                    "picom", "compton", "dunst", "notify-osd",
                    "okidia-master", "okidia-broker"
                };
                if (skipExe.contains(exe)) continue;

                // Masaüstü pencerelerini filtrele
                static const QStringList skipTitle = {
                    QObject::tr("Masaüstü"), "Desktop", "xfdesktop",
                    "Panel", QObject::tr("Görev Çubuğu"), "Taskbar"
                };
                if (skipTitle.contains(title)) continue;

                // Terminal uygulamalarında başlığı exe adıyla değiştir
                static const QStringList terminalExes = {
                    "gnome-terminal", "gnome-terminal-server",
                    "xterm", "xfce4-terminal", "konsole",
                    "tilix", "terminator", "alacritty", "kitty",
                    "st", "urxvt", "rxvt", "lxterminal",
                    "xfce4-terminal", "mate-terminal", "bash", "zsh", "fish"
                };
                QString displayName = title;
                if (terminalExes.contains(exe) ||
                    exe.contains("terminal", Qt::CaseInsensitive) ||
                    exe.contains("term", Qt::CaseInsensitive) ||
                    exe == "bash" || exe == "zsh" || exe == "fish") {
                    if (exe == "gnome-terminal-server" || exe == "gnome-terminal")
                        displayName = "GNOME Terminal";
                    else if (exe == "xfce4-terminal")
                        displayName = "XFCE Terminal";
                    else if (exe == "konsole")
                        displayName = "Konsole";
                    else
                        displayName = exe.at(0).toUpper() + exe.mid(1);
                }

                auto* app = resp.add_apps();
                app->set_name(displayName.toUtf8().toStdString());
                app->set_wid(static_cast<uint64_t>(w));
                app->set_pid(pidStr.toStdString());
                app->set_exe(exe.toStdString());
            }
            XFree(data);
        }
        XCloseDisplay(dpy);
    }
#endif

    okidia::Envelope env;
    env.set_type(okidia::MessageType::APP_LIST_RESPONSE);
    env.set_payload(resp.SerializeAsString());
    if (m_sender) m_sender(env);

    Logger::info(QObject::tr("CommandHandler: %1 uygulama gönderildi").arg(resp.apps_size()));
}

#if defined(Q_OS_WIN)
static BOOL CALLBACK CloseAllWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

    QString exeName;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t exeBuf[MAX_PATH] = {0};
        DWORD exeLen = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exeBuf, &exeLen)) {
            exeName = QFileInfo(QString::fromWCharArray(exeBuf)).fileName();
        }
        CloseHandle(hProc);
    }

    static const QStringList safeExes = {
        "okidia-agent.exe", "okidia-tray.exe", "explorer.exe",
        "ShellExperienceHost.exe", "SearchHost.exe", "StartMenuExperienceHost.exe",
        "TextInputHost.exe", "SystemSettings.exe", "ApplicationFrameHost.exe",
        "okidia-master.exe", "okidia-broker.exe"
    };
    if (safeExes.contains(exeName, Qt::CaseInsensitive)) return TRUE;

    EndTask(hwnd, FALSE, TRUE);
    HANDLE hTerm = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hTerm) {
        TerminateProcess(hTerm, 1);
        CloseHandle(hTerm);
    }
    return TRUE;
}
#endif

void CommandHandler::appClose(const okidia::AppCommand& cmd) {
    QString name = QString::fromStdString(cmd.name()).trimmed();
    Logger::info(QObject::tr("CommandHandler: Uygulama kapatılıyor (Hard Kill) → %1").arg(name));

    // ── Tüm pencereleri zorla kapat ──────────────────────────────────────────
    if (name.isEmpty() || name == "__ALL__") {
        Logger::info(QObject::tr("CommandHandler: Tüm pencereler sorgusuz zorla kapatılıyor..."));
#if defined(Q_OS_WIN)
        EnumWindows(CloseAllWindowsProc, 0);
#elif defined(HAVE_XTST)
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy) {
            Atom netClientList = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
            Atom actualType; int actualFmt; ulong nItems, bytesAfter;
            unsigned char* data = nullptr;

            if (XGetWindowProperty(dpy, DefaultRootWindow(dpy),
                    netClientList, 0, 1024, False, XA_WINDOW,
                    &actualType, &actualFmt, &nItems,
                    &bytesAfter, &data) == Success && data) {

                auto* wins = reinterpret_cast<Window*>(data);
                Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);

                static const QStringList safe = {
                    "okidia-agent", "okidia-tray", "plasmashell", "kwin_x11", "kwin_wayland",
                    "gnome-shell", "xfwm4", "openbox", "marco", "mutter", "xfdesktop",
                    "xfce4-panel", "lxpanel", "tint2", "polybar", "waybar", "Xorg", "Xwayland",
                    "okidia-master", "okidia-broker"
                };

                for (ulong i = 0; i < nItems; ++i) {
                    Window w = wins[i];
                    unsigned char* pidData = nullptr;
                    ulong ni, ba; int af; Atom at;
                    if (XGetWindowProperty(dpy, w, pidAtom, 0, 1, False,
                            XA_CARDINAL, &at, &af, &ni, &ba, &pidData) == Success && pidData) {
                        ulong pid = *reinterpret_cast<ulong*>(pidData);
                        XFree(pidData);
                        if (pid > 1) {
                            QString exe;
                            QFile comm("/proc/" + QString::number(pid) + "/comm");
                            if (comm.open(QIODevice::ReadOnly)) {
                                exe = comm.readAll().trimmed();
                            }
                            if (!safe.contains(exe)) {
                                kill(static_cast<pid_t>(pid), SIGKILL);
                                XKillClient(dpy, w);
                            }
                        }
                    }
                }
                XFree(data);
            }
            XFlush(dpy);
            XCloseDisplay(dpy);
        }
#endif
        sendResult(true, QObject::tr("Tüm pencereler zorla kapatıldı"));
        return;
    }

    // ── Tekil Pencere WID (Hard Kill & XKillClient / TerminateProcess) ────────
    bool isWid = false;
    quint64 widVal = name.toULongLong(&isWid);

    if (isWid) {
#if defined(Q_OS_WIN)
        HWND hwnd = reinterpret_cast<HWND>(widVal);
        if (hwnd && IsWindow(hwnd)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            EndTask(hwnd, FALSE, TRUE); // fForce = TRUE: onay diyaloglarını ezer
            if (pid != 0 && pid != GetCurrentProcessId()) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (hProc) {
                    TerminateProcess(hProc, 1);
                    CloseHandle(hProc);
                    Logger::info(QObject::tr("CommandHandler: Hard Kill (TerminateProcess) → PID: %1").arg(pid));
                }
            }
            sendResult(true, QObject::tr("Pencere zorla kapatıldı"));
            return;
        }
#elif defined(HAVE_XTST)
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy) {
            Window wid = static_cast<Window>(widVal);
            Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
            unsigned char* pidData = nullptr;
            ulong ni, ba; int af; Atom at;
            if (XGetWindowProperty(dpy, wid, pidAtom, 0, 1, False,
                                   XA_CARDINAL, &at, &af, &ni, &ba, &pidData) == Success && pidData) {
                ulong pid = *reinterpret_cast<ulong*>(pidData);
                XFree(pidData);
                if (pid > 1) {
                    kill(static_cast<pid_t>(pid), SIGKILL);
                    Logger::info(QObject::tr("CommandHandler: Hard Kill (SIGKILL) uygulandı → PID: %1").arg(pid));
                }
            }
            // X sunucusundan istemci bağlantısını doğrudan imha et (beforeunload diyaloglarını ezer)
            XKillClient(dpy, wid);
            XFlush(dpy);
            XCloseDisplay(dpy);
            sendResult(true, QObject::tr("Pencere zorla kapatıldı"));
            return;
        }
        runCommand("xdotool", {"windowclose", name});
        sendResult(true, QObject::tr("Pencere kapatıldı"));
        return;
#endif
    }

    // ── İsim veya Exe İle Kapatma (Hard Kill) ─────────────────────────────────
#if defined(Q_OS_WIN)
    QString exe = name.endsWith(".exe", Qt::CaseInsensitive) ? name : name + ".exe";
    runCommand("taskkill.exe", {"/F", "/T", "/IM", exe});
#else
    QString user = SystemInfo::username();
    if (!user.isEmpty() && user != "root") {
        runCommand("pkill", {"-9", "-u", user, "-x", name});
        runCommand("killall", {"-9", "-u", user, name});
    } else {
        runCommand("pkill", {"-9", "-x", name});
        runCommand("killall", {"-9", name});
    }
#endif

    sendResult(true, name + QObject::tr(" zorla kapatıldı"));
}

void CommandHandler::initAppWatchdog() {
    if (!m_watchdogTimer) {
        m_watchdogTimer = new QTimer(this);
        m_watchdogTimer->setInterval(1000);
        connect(m_watchdogTimer, &QTimer::timeout, this, &CommandHandler::checkBlockedApps);
    }
}

void CommandHandler::notifyAppBlocked(const QString& appName) {
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastBlockNotifyMs < 4000) {
        return; // 4 saniyelik anti-spam sınırlaması
    }
    m_lastBlockNotifyMs = now;

    QString title = QObject::tr("Uygulama Engellendi");
    QString msg = QObject::tr("'%1' uygulaması öğretmeniniz tarafından engellenmiştir.").arg(appName);

    if (m_tray && m_tray->trayIcon()) {
        m_tray->trayIcon()->showMessage(title, msg, QSystemTrayIcon::Warning, 3500);
    }
#if defined(Q_OS_LINUX)
    runCommand("notify-send", {"-u", "critical", "-i", "dialog-warning", title, msg}, false);
#endif
}

void CommandHandler::checkBlockedApps() {
    if (m_blockedApps.isEmpty()) return;

    static const QStringList safeList = {
        "okidia-agent", "okidia-tray", "okidia-master", "okidia-broker",
        "systemd", "kwin", "kwin_x11", "kwin_wayland", "plasmashell",
        "gnome-shell", "mutter", "xfwm4", "openbox", "Xorg", "Xwayland",
        "bash", "sh", "dbus-daemon", "pulseaudio", "pipewire"
    };

#if defined(Q_OS_WIN)
    static const QStringList safeWinExes = {
        "okidia-agent.exe", "okidia-tray.exe", "explorer.exe",
        "ShellExperienceHost.exe", "SearchHost.exe", "StartMenuExperienceHost.exe",
        "TextInputHost.exe", "SystemSettings.exe", "ApplicationFrameHost.exe",
        "okidia-master.exe", "okidia-broker.exe", "csrss.exe", "smss.exe",
        "services.exe", "lsass.exe", "svchost.exe", "wininit.exe", "winlogon.exe"
    };

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(hSnap, &pe32)) {
            do {
                if (pe32.th32ProcessID == 0 || pe32.th32ProcessID == GetCurrentProcessId()) continue;
                QString exeName = QString::fromWCharArray(pe32.szExeFile);
                if (safeWinExes.contains(exeName, Qt::CaseInsensitive)) continue;

                bool blocked = false;
                QString matchedApp;
                for (const QString& b : m_blockedApps) {
                    QString bClean = b;
                    if (bClean.endsWith(".exe", Qt::CaseInsensitive))
                        bClean.chop(4);
                    if (exeName.compare(b, Qt::CaseInsensitive) == 0 ||
                        exeName.startsWith(bClean, Qt::CaseInsensitive)) {
                        blocked = true;
                        matchedApp = b;
                        break;
                    }
                }

                if (blocked) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 1);
                        CloseHandle(hProc);
                        Logger::warning(QObject::tr("CommandHandler Watchdog: Engelli süreç imha edildi → %1 (PID %2)")
                                        .arg(exeName).arg(pe32.th32ProcessID));
                        notifyAppBlocked(matchedApp);
                    }
                }
            } while (Process32NextW(hSnap, &pe32));
        }
        CloseHandle(hSnap);
    }
#elif defined(Q_OS_LINUX)
    QDir procDir("/proc");
    const auto entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        bool ok = false;
        pid_t pid = static_cast<pid_t>(entry.toInt(&ok));
        if (!ok || pid <= 1) continue;

        QString comm;
        QFile commFile("/proc/" + entry + "/comm");
        if (commFile.open(QIODevice::ReadOnly)) {
            comm = QString::fromUtf8(commFile.readAll()).trimmed();
            commFile.close();
        }

        QString exePath = QFile::symLinkTarget("/proc/" + entry + "/exe");
        QString exeBase = QFileInfo(exePath).fileName();

        if (safeList.contains(comm, Qt::CaseInsensitive) ||
            safeList.contains(exeBase, Qt::CaseInsensitive)) {
            continue;
        }

        bool blocked = false;
        QString matchedApp;
        for (const QString& b : m_blockedApps) {
            if (comm.compare(b, Qt::CaseInsensitive) == 0 ||
                exeBase.compare(b, Qt::CaseInsensitive) == 0 ||
                comm.contains(b, Qt::CaseInsensitive) ||
                exeBase.contains(b, Qt::CaseInsensitive) ||
                exePath.contains(b, Qt::CaseInsensitive)) {
                blocked = true;
                matchedApp = b;
                break;
            }
        }

        if (blocked) {
            kill(pid, SIGKILL);
            Logger::warning(QObject::tr("CommandHandler Watchdog: Engelli süreç imha edildi (SIGKILL) → %1/%2 (PID %3)")
                            .arg(comm, exeBase).arg(pid));
            notifyAppBlocked(matchedApp);
        }
    }
#endif
}

void CommandHandler::appBlockByName(const okidia::AppCommand& cmd) {
    QString name = QString::fromStdString(cmd.name()).trimmed();
    Logger::info(QObject::tr("CommandHandler: Uygulama kara listeye alınıyor → %1").arg(name));

    if (!name.isEmpty() && !m_blockedApps.contains(name, Qt::CaseInsensitive)) {
        m_blockedApps.append(name);
    }

    initAppWatchdog();
    if (!m_watchdogTimer->isActive()) {
        m_watchdogTimer->start();
    }
    checkBlockedApps();

    sendResult(true, name + QObject::tr(" engellendi ve nöbetçi aktif"));
}

void CommandHandler::appBlockList(const okidia::AppCommand& cmd) {
    Logger::info(QObject::tr("CommandHandler: Uygulama kara listesi güncelleniyor (%1 adet)").arg(cmd.blacklist_size()));

    m_blockedApps.clear();
    for (const auto& app : cmd.blacklist()) {
        QString s = QString::fromStdString(app).trimmed();
        if (!s.isEmpty() && !m_blockedApps.contains(s, Qt::CaseInsensitive)) {
            m_blockedApps.append(s);
        }
    }

    if (!m_blockedApps.isEmpty()) {
        initAppWatchdog();
        if (!m_watchdogTimer->isActive()) {
            m_watchdogTimer->start();
        }
        checkBlockedApps();
        sendResult(true, QObject::tr("Uygulama kara listesi uygulandı (%1 uygulama)").arg(m_blockedApps.size()));
    } else {
        appUnblock();
    }
}

void CommandHandler::appBlockByHash(const okidia::AppCommand& cmd) {
    QString targetHash = QString::fromStdString(cmd.hash()).toLower();
    Logger::info(QObject::tr("CommandHandler: Uygulama engelleniyor (hash) → %1...").arg(targetHash.left(12)));

#if !defined(Q_OS_WIN)
    QDir procDir("/proc");
    const auto entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        bool ok;
        entry.toInt(&ok);
        if (!ok) continue;

        QString exePath = QFile::symLinkTarget("/proc/" + entry + "/exe");
        if (exePath.isEmpty()) continue;

        QFile exeFile(exePath);
        if (!exeFile.open(QIODevice::ReadOnly)) continue;
        QByteArray data = exeFile.readAll();
        exeFile.close();

        QString fileHash = OkidiaUtils::sha256(data);

        if (fileHash == targetHash) {
            Logger::info(QObject::tr("CommandHandler: Hash eşleşti → PID %1 (%2)").arg(entry, exePath));
            runCommand("kill", {"-9", entry});
        }
    }
#endif
    sendResult(true, QObject::tr("Hash ile engelleme tamamlandı"));
}

void CommandHandler::appWhitelistSet(const okidia::AppCommand& cmd) {
    Logger::info(QObject::tr("CommandHandler: Beyaz liste modu aktif"));
    QStringList allowed;
    for (const auto& name : cmd.whitelist()) {
        allowed << QString::fromStdString(name);
    }

#if !defined(Q_OS_WIN)
    QDir procDir("/proc");
    const auto entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QString currentUser = qgetenv("USER");

    for (const QString& entry : entries) {
        bool ok;
        entry.toInt(&ok);
        if (!ok) continue;

        QFileInfo info("/proc/" + entry);
        if (info.owner() != currentUser) continue;

        QString exePath = QFile::symLinkTarget("/proc/" + entry + "/exe");
        if (exePath.isEmpty()) continue;

        QString baseName = QFileInfo(exePath).baseName();

        if (baseName == "okidia-agent" || exePath.startsWith("/usr/lib") ||
            exePath.startsWith("/lib"))
            continue;

        if (!allowed.contains(baseName)) {
            Logger::info(QObject::tr("CommandHandler: Beyaz listede yok, kapatılıyor → %1").arg(baseName));
            runCommand("kill", {"-15", entry});
        }
    }
#endif
    sendResult(true, QObject::tr("Beyaz liste uygulandı"));
}

void CommandHandler::appUnblock() {
    Logger::info(QObject::tr("CommandHandler: Uygulama engeli kaldırılıyor"));
    m_blockedApps.clear();
    if (m_watchdogTimer && m_watchdogTimer->isActive()) {
        m_watchdogTimer->stop();
    }
    sendResult(true, QObject::tr("Uygulama engelleri kaldırıldı"));
}
