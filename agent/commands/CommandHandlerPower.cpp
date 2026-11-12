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
    QString user = SystemInfo::username();
    if (SystemInfo::isDmUser(user) || user == "lightdm" || user == "gdm" || user == "sddm" || user == "winlogon") {
        Logger::warning(QObject::tr("CommandHandler: Display Manager oturumunda (%1) logout isteği engellendi.").arg(user));
        sendResult(false, QObject::tr("Giriş ekranı (Display Manager) oturumu kapatılamaz."));
        return;
    }

    Logger::info(QObject::tr("CommandHandler: Oturum kapatılıyor"));

#if defined(Q_OS_WIN)
    runCommand("shutdown.exe", {"/l"});
#else
    // Masaüstü ortamını tespit et ve uygun komutu çalıştır
    QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    QString session = qEnvironmentVariable("DESKTOP_SESSION").toLower();

    if (desktop.contains("gnome") || session.contains("gnome") ||
        desktop.contains("pardus")) {
        runCommand("gnome-session-quit", {"--logout", "--no-prompt"});
    } else if (desktop.contains("xfce") || session.contains("xfce")) {
        runCommand("xfce4-session-logout", {"--logout"});
    } else if (desktop.contains("kde") || session.contains("plasma")) {
        runCommand("qdbus", {"org.kde.ksmserver", "/KSMServer",
                             "org.kde.KSMServerInterface.logout", "0", "0", "0"});
    } else {
        // Evrensel fallback: loginctl ile kullanıcı oturumunu kapat
        QString user = SystemInfo::username();
        if (!user.isEmpty() && user != "root")
            runCommand("loginctl", {"terminate-user", user});
        else
            runCommand("loginctl", {"terminate-session", "auto"});
    }
#endif
    sendResult(true);
}

void CommandHandler::powerHibernate() {
    Logger::info(QObject::tr("CommandHandler: Uyku moduna alınıyor"));
    auto p = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Suspend);
    runCommand(p.first, p.second);
    sendResult(true);
}
