// common/utils/PlatformService.h
#pragma once

#include <QString>
#include <functional>

class QWidget;

class PlatformService {
public:
    static void notifyReady();
    static void notifyStatus(const QString& status);
    static void setupSignalHandlers();
    static bool setSystemTimeUtc(qint64 epochSeconds);
    static bool executeElevated(const QString& program, const QStringList& args, QWidget* parentWidget = nullptr);

#if defined(Q_OS_WIN)
    // Windows Service Manager (SCM) desteği
    static bool isRunningAsService();
    static bool runAsService(const QString& serviceName, std::function<int(int, char**)> runFunc, int argc, char** argv);
    static void stopService();
    static bool ensureTrayRunning(const QString& trayExeName = QString(), bool isWinlogon = false);
    static bool isSessionLocked();
    static void setSessionLocked(bool locked);
    static void setSessionChangeCallback(std::function<void()> cb);
#endif
};
