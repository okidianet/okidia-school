// agent/commands/CommandHandlerPower.cpp
#include "CommandHandler.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../system/SystemInfo.h"
#include <QProcess>
#include <QObject>

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
#endif

#include "../system/SessionSwitchManager.h"

void CommandHandler::powerShutdown(const okidia::PowerCommand& cmd) {
    SessionSwitchManager::instance().cleanupAutologinConfig();
    int delay = static_cast<int>(cmd.delay_s());
    Logger::info(QObject::tr("CommandHandler: Kapatılıyor → %1 saniye sonra").arg(delay));

    auto p = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Shutdown, delay);
    runCommand(p.first, p.second);
    sendResult(true);
}

void CommandHandler::powerRestart(const okidia::PowerCommand& cmd) {
    SessionSwitchManager::instance().cleanupAutologinConfig();
    int delay = static_cast<int>(cmd.delay_s());
    Logger::info(QObject::tr("CommandHandler: Yeniden başlatılıyor → %1 saniye sonra").arg(delay));

    auto p = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Reboot, delay);
    runCommand(p.first, p.second);
    sendResult(true);
}

void CommandHandler::powerLogout() {
    SessionSwitchManager::instance().cleanupAutologinConfig();

    QString user = SystemInfo::username();
    if (SystemInfo::isDmUser(user) || user == "lightdm" || user == "gdm" || user == "sddm" || user == "winlogon") {
        Logger::warning(QObject::tr("CommandHandler: Display Manager oturumunda (%1) logout isteği engellendi.").arg(user));
        sendResult(false, QObject::tr("Giriş ekranı (Display Manager) oturumu kapatılamaz."));
        return;
    }

    Logger::info(QObject::tr("CommandHandler: Oturum kapatılıyor"));

#if defined(Q_OS_WIN)
    runCommand("shutdown.exe", {"/l", "/f"});
#else
    // Masaüstü ortamını tespit et ve uygulama engelleyicilerine (inhibitor) / onay pencerelerine
    // takılmadan anında çıkış yapan komutu çalıştır
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    const QString session = qEnvironmentVariable("DESKTOP_SESSION").toLower();
    const QString gdmSess = qEnvironmentVariable("GDMSESSION").toLower();
    const QString combined = desktop + " " + session + " " + gdmSess;

    if (combined.contains("cinnamon")) {
        // Cinnamon (Pardus Cinnamon, Linux Mint, Debian Cinnamon):
        // --no-prompt ve --force ile onay diyaloglarını ve açık uygulama engellerini atla
        if (!runCommandChecked("cinnamon-session-quit", {"--logout", "--no-prompt", "--force"})) {
            runCommand("gdbus", {"call", "--session",
                                 "--dest", "org.gnome.SessionManager",
                                 "--object-path", "/org/gnome/SessionManager",
                                 "--method", "org.gnome.SessionManager.Logout", "2"}, false);
        }
    } else if (combined.contains("budgie")) {
        if (!runCommandChecked("budgie-session-quit", {"--logout", "--no-prompt", "--force"})) {
            runCommand("gnome-session-quit", {"--logout", "--no-prompt", "--force"}, false);
        }
    } else if (combined.contains("xfce")) {
        // XFCE4: --fast parametresi oturum kaydetme gecikmesini ve diyalogları atlar
        runCommand("xfce4-session-logout", {"--logout", "--fast"}, false);
    } else if (combined.contains("mate")) {
        // MATE: --force-logout açık uygulamaların çıkışı bloklamasını önler
        runCommand("mate-session-save", {"--force-logout"}, false);
    } else if (combined.contains("kde") || combined.contains("plasma")) {
        // KDE Plasma 6 (org.kde.Shutdown) ve Plasma 5 (org.kde.ksmserver)
        if (!runCommandChecked("gdbus", {"call", "--session",
                                         "--dest", "org.kde.Shutdown",
                                         "--object-path", "/Shutdown",
                                         "--method", "org.kde.Shutdown.logout"})) {
            if (!runCommandChecked("gdbus", {"call", "--session",
                                             "--dest", "org.kde.ksmserver",
                                             "--object-path", "/KSMServer",
                                             "--method", "org.kde.KSMServerInterface.logout",
                                             "0", "0", "0"})) {
                runCommand("qdbus", {"org.kde.ksmserver", "/KSMServer",
                                     "org.kde.KSMServerInterface.logout", "0", "0", "0"}, false);
            }
        }
    } else if (combined.contains("lxqt")) {
        runCommand("pkill", {"-SIGTERM", "-u", user, "-f", "lxqt-session"}, false);
    } else if (combined.contains("lxde") || combined.contains("openbox")) {
        runCommand("pkill", {"-SIGTERM", "-u", user, "-f", "lxsession|openbox"}, false);
    } else if (combined.contains("gnome") || combined.contains("ubuntu") ||
               combined.contains("pardus") || combined.contains("pantheon")) {
        // Pardus altında XDG_CURRENT_DESKTOP bazen "pardus:gnome" veya sadece "pardus" olabilir;
        // eğer süreçlerde cinnamon-session varsa önce onu dene, yoksa gnome-session-quit --force çalıştır.
        if (QProcess::execute("pgrep", {"-u", user, "-f", "cinnamon-session"}) == 0) {
            runCommand("cinnamon-session-quit", {"--logout", "--no-prompt", "--force"}, false);
        } else {
            runCommand("gnome-session-quit", {"--logout", "--no-prompt", "--force"}, false);
        }
        runCommand("gdbus", {"call", "--session",
                             "--dest", "org.gnome.SessionManager",
                             "--object-path", "/org/gnome/SessionManager",
                             "--method", "org.gnome.SessionManager.Logout", "2"}, false);
    }

    // Evrensel yedek (Watchdog): Masaüstü yöneticisi 900 ms içinde oturumu kapatmadıysa
    // doğrudan aktif grafik oturumunu (loginctl terminate-session) sonlandır.
    const QString sessId = qEnvironmentVariable("XDG_SESSION_ID").trimmed();
    QTimer::singleShot(900, [user, sessId]() {
        if (!sessId.isEmpty()) {
            QProcess::startDetached("loginctl", {"terminate-session", sessId});
        } else if (!user.isEmpty() && user != "root") {
            QProcess::startDetached("loginctl", {"terminate-user", user});
        } else {
            QProcess::startDetached("loginctl", {"terminate-session", "auto"});
        }
    });
#endif
    sendResult(true);
}

void CommandHandler::powerHibernate() {
    Logger::info(QObject::tr("CommandHandler: Uyku moduna alınıyor"));
    auto p = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Suspend);
    runCommand(p.first, p.second);
    sendResult(true);
}
