// agent/system/SystemInfo.cpp
#include "SystemInfo.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/PlatformService.h"

#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QAbstractSocket>
#include <QRegularExpression>
#include "../../common/utils/OkidiaUtils.h"
#include <QDir>
#include <QUuid>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QSettings>
#include <QThread>
#include <QStorageInfo>

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
#  include <wtsapi32.h>
#  include <userenv.h>
#  include <tlhelp32.h>
#  ifdef interface
#    undef interface
#  endif
#  pragma comment(lib, "wtsapi32.lib")
#else
#  include <unistd.h>
#  include <sys/statvfs.h>
#  include <pwd.h>
#endif

#include "../../common/OkidiaConfig.h"

SystemInfo::CpuTimes SystemInfo::s_prevCpuTimes;

#if defined(Q_OS_WIN)
static uint64_t FileTimeToUInt64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
#endif

float SystemInfo::readCpuUsage() {
#if defined(Q_OS_WIN)
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return 0.0f;
    }

    uint64_t idle = FileTimeToUInt64(idleTime);
    uint64_t kernel = FileTimeToUInt64(kernelTime);
    uint64_t user = FileTimeToUInt64(userTime);

    static uint64_t prevIdle = 0, prevKernel = 0, prevUser = 0;

    uint64_t idleDiff = idle - prevIdle;
    uint64_t kernelDiff = kernel - prevKernel;
    uint64_t userDiff = user - prevUser;

    prevIdle = idle;
    prevKernel = kernel;
    prevUser = user;

    uint64_t totalDiff = kernelDiff + userDiff;
    if (totalDiff == 0) return 0.0f;

    uint64_t busyDiff = totalDiff - idleDiff;
    return static_cast<float>(busyDiff * 100.0 / totalDiff);
#else
    CpuTimes curr = readCpuTimes();

    uint64_t totalDiff  = curr.total()  - s_prevCpuTimes.total();
    uint64_t activeDiff = curr.active() - s_prevCpuTimes.active();

    s_prevCpuTimes = curr;

    if (totalDiff == 0) return 0.0f;
    return static_cast<float>(activeDiff * 100.0 / totalDiff);
#endif
}

float SystemInfo::readMemUsage() {
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX memStatus = {};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        return static_cast<float>(memStatus.dwMemoryLoad);
    }
    return 0.0f;
#else
    QFile file("/proc/meminfo");
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("SystemInfo: /proc/meminfo açılamadı"));
        return 0.0f;
    }

    uint64_t memTotal = 0, memFree = 0,
             memBuffers = 0, memCached = 0;

    QTextStream stream(&file);
    QString line;
    while (stream.readLineInto(&line)) {
        if (line.startsWith("MemTotal:"))
            memTotal   = line.split(QRegularExpression("\\s+"),
                Qt::SkipEmptyParts)[1].toULongLong();
        else if (line.startsWith("MemFree:"))
            memFree    = line.split(QRegularExpression("\\s+"),
                Qt::SkipEmptyParts)[1].toULongLong();
        else if (line.startsWith("Buffers:"))
            memBuffers = line.split(QRegularExpression("\\s+"),
                Qt::SkipEmptyParts)[1].toULongLong();
        else if (line.startsWith("Cached:"))
            memCached  = line.split(QRegularExpression("\\s+"),
                Qt::SkipEmptyParts)[1].toULongLong();
    }
    file.close();

    if (memTotal == 0) return 0.0f;

    uint64_t memUsed = memTotal - memFree - memBuffers - memCached;
    return static_cast<float>(memUsed * 100.0 / memTotal);
#endif
}

SystemStats SystemInfo::readStats() {
    SystemStats stats;
    stats.cpuUsage  = readCpuUsage();
    stats.memUsage  = readMemUsage();
    stats.hostname  = hostname();
    stats.username  = username();
    stats.osVersion = osVersion();

    static QString s_cachedAgentVersion;
    if (s_cachedAgentVersion.isEmpty()) {
#if !defined(Q_OS_WIN)
        QProcess p;
        p.start("dpkg-query", {"-W", "-f=${Version}", "okidia-agent"});
        if (p.waitForFinished(1000) && p.exitCode() == 0) {
            s_cachedAgentVersion = p.readAllStandardOutput().trimmed();
        }
#endif
        if (s_cachedAgentVersion.isEmpty()) {
            s_cachedAgentVersion = Okidia::Version::string();
        }
    }
    stats.agentVersion = s_cachedAgentVersion;

#if defined(Q_OS_WIN)
    stats.osTag = QStringLiteral("windows");
    QString arch = QSysInfo::currentCpuArchitecture();
    stats.osArch = OkidiaUtils::normalizeArch(arch);

    MEMORYSTATUSEX memStatus = {};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        stats.memTotal = memStatus.ullTotalPhys;
        stats.memUsed = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
    }

    ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
        stats.diskTotal = totalNumberOfBytes.QuadPart;
        stats.diskUsed = totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart;
        if (stats.diskTotal > 0) {
            stats.diskUsage = static_cast<float>(stats.diskUsed) * 100.0f / stats.diskTotal;
        }
    }
#else
    // İşletim sistemi tespiti (Linux)
    QMap<QString, QString> osReleaseFields;
    QFile osRelFile("/etc/os-release");
    if (osRelFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&osRelFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int eqIdx = line.indexOf('=');
            if (eqIdx > 0) {
                QString key = line.left(eqIdx).trimmed();
                QString val = line.mid(eqIdx + 1).trimmed();
                if (val.startsWith('"') && val.endsWith('"')) {
                    val = val.mid(1, val.length() - 2);
                }
                osReleaseFields[key] = val;
            }
        }
        osRelFile.close();
    }
    
    QString debianVersionRaw;
    QFile debVerFile("/etc/debian_version");
    if (debVerFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&debVerFile);
        debianVersionRaw = in.readLine().trimmed();
        debVerFile.close();
    }

    stats.osTag = OkidiaUtils::parseOsTag(osReleaseFields, debianVersionRaw);

    QString arch = QSysInfo::currentCpuArchitecture();
    stats.osArch = OkidiaUtils::normalizeArch(arch);

    QFile file("/proc/meminfo");
    if (file.open(QIODevice::ReadOnly)) {
        uint64_t memFree = 0, memBuffers = 0, memCached = 0;
        QTextStream stream(&file);
        QString line;
        while (stream.readLineInto(&line)) {
            if (line.startsWith("MemTotal:"))
                stats.memTotal = line.split(QRegularExpression("\\s+"),
                    Qt::SkipEmptyParts)[1].toULongLong();
            else if (line.startsWith("MemFree:"))
                memFree = line.split(QRegularExpression("\\s+"),
                    Qt::SkipEmptyParts)[1].toULongLong();
            else if (line.startsWith("Buffers:"))
                memBuffers = line.split(QRegularExpression("\\s+"),
                    Qt::SkipEmptyParts)[1].toULongLong();
            else if (line.startsWith("Cached:"))
                memCached = line.split(QRegularExpression("\\s+"),
                    Qt::SkipEmptyParts)[1].toULongLong();
        }
        file.close();
        stats.memUsed = stats.memTotal - memFree - memBuffers - memCached;
    }

    // Disk Usage Calculation (Linux)
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        stats.diskTotal = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t diskFree = (uint64_t)vfs.f_bfree * vfs.f_frsize;
        stats.diskUsed = stats.diskTotal - diskFree;
        if (stats.diskTotal > 0) {
            stats.diskUsage = static_cast<float>(stats.diskUsed) * 100.0f / stats.diskTotal;
        }
    }
#endif

    stats.secureBoot = isSecureBootActive();
    stats.diskEncrypted = isStorageEncrypted();

    return stats;
}

bool SystemInfo::isSecureBootActive() {
    return OkidiaUtils::checkSecureBootEnabled();
}

bool SystemInfo::isStorageEncrypted() {
    return OkidiaUtils::checkDiskEncryption();
}

QString SystemInfo::hostname() {
    return QHostInfo::localHostName();
}

QString SystemInfo::username() {
#if defined(Q_OS_WIN)
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId != 0xFFFFFFFF) {
        LPWSTR pBuffer = nullptr;
        DWORD bytesReturned = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &pBuffer, &bytesReturned) && pBuffer) {
            QString user = QString::fromWCharArray(pBuffer).trimmed();
            WTSFreeMemory(pBuffer);
            if (!user.isEmpty()) return user;
        }
    }

    wchar_t acUserName[256];
    DWORD nUserName = sizeof(acUserName) / sizeof(wchar_t);
    if (GetUserNameW(acUserName, &nUserName)) {
        return QString::fromWCharArray(acUserName);
    }
    return QStringLiteral("User");
#else
    // Root olarak çalışırken gerçek oturum sahibini bul
    // 1. who komutu ile :0 ekranındaki kullanıcıyı al
    FILE* pipe = popen("who | awk '/:0/{print $1; exit}'", "r");
    if (pipe) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), pipe)) {
            pclose(pipe);
            QString user = QString::fromLatin1(buf).trimmed();
            if (!user.isEmpty() && user != "root") return user;
        } else {
            pclose(pipe);
        }
    }

    // 2. loginctl ile aktif oturumu bul (lightdm dahil)
    pipe = popen("loginctl list-sessions --no-legend 2>/dev/null | "
                 "awk '{print $3}' | grep -v '^root$' | head -1", "r");
    if (pipe) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), pipe)) {
            pclose(pipe);
            QString user = QString::fromLatin1(buf).trimmed();
            if (!user.isEmpty()) return user;
        } else {
            pclose(pipe);
        }
    }

    // 3. Display manager aktif mi kontrol et (login ekranı)
    for (const QString& xauth : {
            QString("/var/run/lightdm/root/:0"),
            QString("/run/lightdm/root/:0"),
            QString("/var/lib/lightdm/.Xauthority") }) {
        if (QFile::exists(xauth)) return "lightdm";
    }
    for (const QString& xauth : {
            QString("/run/sddm/.Xauthority"),
            QString("/var/run/sddm/.Xauthority") }) {
        if (QFile::exists(xauth)) return "sddm";
    }

    return "user";
#endif
}

QString SystemInfo::osVersion() {
#if defined(Q_OS_WIN)
    return QSysInfo::prettyProductName();
#else
    QString model;
    {
        QFile modelFile("/sys/firmware/devicetree/base/model");
        if (modelFile.open(QIODevice::ReadOnly)) {
            model = QString::fromUtf8(modelFile.readAll()).trimmed();
            modelFile.close();
        }
    }
    if (model.isEmpty()) {
        QFile modelFile("/proc/device-tree/model");
        if (modelFile.open(QIODevice::ReadOnly)) {
            model = QString::fromUtf8(modelFile.readAll()).trimmed();
            modelFile.close();
        }
    }

    QFile file("/etc/os-release");
    QString prettyName;
    if (file.open(QIODevice::ReadOnly)) {
        QTextStream stream(&file);
        QString line;
        while (stream.readLineInto(&line)) {
            if (line.startsWith("PRETTY_NAME=")) {
                prettyName = line.mid(12).remove('"');
                break;
            }
        }
        file.close();
    }

    if (prettyName.isEmpty()) {
        prettyName = "Linux";
    }

    const bool isRpi = !model.isEmpty() && 
                       (model.contains("Raspberry Pi", Qt::CaseInsensitive) || 
                        model.contains("Raspbian", Qt::CaseInsensitive));

    if (isRpi) {
        if (prettyName.contains("Raspbian GNU/Linux", Qt::CaseInsensitive)) {
            prettyName.replace("Raspbian GNU/Linux", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (prettyName.contains("Raspbian", Qt::CaseInsensitive)) {
            prettyName.replace("Raspbian", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (prettyName.contains("Debian GNU/Linux", Qt::CaseInsensitive)) {
            prettyName.replace("Debian GNU/Linux", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (prettyName.contains("Debian", Qt::CaseInsensitive)) {
            prettyName.replace("Debian", "Raspberry Pi OS", Qt::CaseInsensitive);
        } else if (!prettyName.contains("Raspberry Pi OS", Qt::CaseInsensitive)) {
            prettyName = "Raspberry Pi OS (" + prettyName + ")";
        }
    } else {
        if (prettyName.contains("Debian GNU/Linux", Qt::CaseInsensitive)) {
            prettyName.replace("Debian GNU/Linux", "Debian", Qt::CaseInsensitive);
        }
    }

    return prettyName.trimmed();
#endif
}

QString SystemInfo::macAddress(const QString& ifaceName) {
    if (!ifaceName.isEmpty()) {
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            if (iface.name() == ifaceName) {
                return iface.hardwareAddress();
            }
        }
    }

    // 1st pass: Prefer active (IsUp) physical non-loopback interfaces with valid MAC addresses
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            !iface.flags().testFlag(QNetworkInterface::IsLoopBack) &&
            !iface.hardwareAddress().isEmpty() &&
            iface.hardwareAddress() != "00:00:00:00:00:00") {
            return iface.hardwareAddress();
        }
    }

    // 2nd pass: Fallback to any physical non-loopback interface
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsLoopBack) &&
            !iface.hardwareAddress().isEmpty() &&
            iface.hardwareAddress() != "00:00:00:00:00:00") {
            return iface.hardwareAddress();
        }
    }

    return {};
}

QString SystemInfo::machineId() {
    static QString s_cachedMachineId;
    if (!s_cachedMachineId.isEmpty()) {
        return s_cachedMachineId;
    }

    const QString okidiaDir = OkidiaConfig::basePath();
    const QString machineIdPath = okidiaDir + "/machine_id";
    const QString installedMacPath = okidiaDir + "/installed_mac";

    auto writePublicFile = [](const QString& path, const QString& content) -> bool {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(content.toUtf8());
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ReadGroup | QFileDevice::ReadOther);
        return true;
    };

    QDir().mkpath(okidiaDir);

    QString baseSysId;

#if defined(Q_OS_WIN)
    // Windows Cryptography MachineGuid
    QSettings reg("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography", QSettings::NativeFormat);
    baseSysId = reg.value("MachineGuid").toString().trimmed();
#endif

    QString currentMac = macAddress("");
    if (currentMac.isEmpty() && baseSysId.isEmpty()) {
        for (int i = 0; i < 20; ++i) {
            QThread::msleep(100);
            currentMac = macAddress("");
            if (!currentMac.isEmpty()) break;
        }
    }

#if defined(Q_OS_WIN)
    if (baseSysId.isEmpty() && !currentMac.isEmpty()) {
        baseSysId = "win-" + currentMac.toLower();
    }
#else
    // 1. DMI UUID
    QFile dmiFile("/sys/class/dmi/id/product_uuid");
    if (dmiFile.open(QIODevice::ReadOnly)) {
        baseSysId = QString::fromLatin1(dmiFile.readAll()).trimmed();
        dmiFile.close();
    }

    // 2. Fallback to /etc/machine-id
    if (baseSysId.isEmpty() || baseSysId.toLower().startsWith("00000000") || baseSysId.toLower().startsWith("ffffffff")) {
        QFile sysFile("/etc/machine-id");
        if (sysFile.open(QIODevice::ReadOnly)) {
            baseSysId = QString::fromLatin1(sysFile.readAll()).trimmed();
            sysFile.close();
        }
        if (baseSysId.isEmpty()) {
            QFile dbusFile("/var/lib/dbus/machine-id");
            if (dbusFile.open(QIODevice::ReadOnly)) {
                baseSysId = QString::fromLatin1(dbusFile.readAll()).trimmed();
                dbusFile.close();
            }
        }
        if (baseSysId.isEmpty()) {
            baseSysId = "okidia-agent-fallback-id";
        }

        if (!currentMac.isEmpty()) {
            baseSysId += ":" + currentMac.toLower();
        }
    }
#endif

    QUuid deterministicUuid = QUuid::createUuidV5(QUuid(), baseSysId);
    QString deterministicId = deterministicUuid.toString(QUuid::WithoutBraces);

    QFile idFile(machineIdPath);
    if (idFile.exists()) {
        if (idFile.open(QIODevice::ReadOnly)) {
            QString existingId = QString::fromLatin1(idFile.readAll()).trimmed();
            idFile.close();

#if !defined(Q_OS_WIN)
            if (geteuid() != 0) {
                s_cachedMachineId = existingId;
                return existingId;
            }
#endif

            if (existingId != deterministicId) {
                Logger::warning(QObject::tr("SystemInfo: Klon veya donanım değişikliği tespit edildi! Kayıtlı ID (%1) != Deterministik ID (%2). Yeniden üretiliyor...").arg(existingId, deterministicId));

                QFile::remove(OkidiaConfig::agentCertDir() + "/agent.crt");
                QFile::remove(OkidiaConfig::agentCertDir() + "/agent.key");
                QFile::remove(OkidiaConfig::agentCertDir() + "/ca.crt");
                QFile::remove(OkidiaConfig::agentCertDir() + "/agent.csr");

                writePublicFile(machineIdPath, deterministicId);
                if (!currentMac.isEmpty()) {
                    writePublicFile(installedMacPath, currentMac);
                }
            } else {
                deterministicId = existingId;
            }
        }
    } else {
#if !defined(Q_OS_WIN)
        if (geteuid() != 0) {
            s_cachedMachineId = deterministicId;
            return s_cachedMachineId;
        }
#endif
        writePublicFile(machineIdPath, deterministicId);
        if (!currentMac.isEmpty()) {
            writePublicFile(installedMacPath, currentMac);
        }
    }

    s_cachedMachineId = deterministicId;
    return s_cachedMachineId;
}

QString SystemInfo::ipAddress() {
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                && !entry.ip().isLoopback()) {
                return entry.ip().toString();
            }
        }
    }
    return {};
}

QMap<QString, QString> SystemInfo::allMacAddresses() {
    QMap<QString, QString> result;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        if (iface.hardwareAddress().isEmpty()) continue;
        result[iface.name()] = iface.hardwareAddress();
    }
    return result;
}

SystemInfo::CpuTimes SystemInfo::readCpuTimes() {
#if defined(Q_OS_WIN)
    return {};
#else
    QFile file("/proc/stat");
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("SystemInfo: /proc/stat açılamadı"));
        return {};
    }

    CpuTimes times;
    QTextStream stream(&file);
    QString line = stream.readLine();
    file.close();

    QStringList parts = line.split(QRegularExpression("\\s+"),
                                    Qt::SkipEmptyParts);
    if (parts.size() >= 8) {
        times.user    = parts[1].toULongLong();
        times.nice    = parts[2].toULongLong();
        times.system  = parts[3].toULongLong();
        times.idle    = parts[4].toULongLong();
        times.iowait  = parts[5].toULongLong();
        times.irq     = parts[6].toULongLong();
        times.softirq = parts[7].toULongLong();
        if (parts.size() >= 9)
            times.steal = parts[8].toULongLong();
    }

    return times;
#endif
}

// ── Oturum ve Display Yöneticisi ──────────────────────────────────────────

#if defined(Q_OS_WIN)
bool SystemInfo::isWorkstationLocked()
{
    DWORD sessId = WTSGetActiveConsoleSessionId();
    if (sessId == 0xFFFFFFFF) {
        return true;
    }

    // 1. Kontrol: SCM (Windows Servis Yöneticisi) oturum kilit bildirimi
    if (PlatformService::isSessionLocked()) {
        return true;
    }

    // 2. Kontrol: WTSQuerySessionInformationW (WTSSessionInfoEx) üzerinden çekirdek kilit durumu
#ifdef WTS_SESSIONSTATE_LOCK
    LPWSTR pInfoBuf = nullptr;
    DWORD infoBytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessId, WTSSessionInfoEx, &pInfoBuf, &infoBytes) && pInfoBuf) {
        if (infoBytes >= sizeof(WTSINFOEXW)) {
            const WTSINFOEXW* infoEx = reinterpret_cast<const WTSINFOEXW*>(pInfoBuf);
            if (infoEx->Level == 1 && infoEx->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK) {
                WTSFreeMemory(pInfoBuf);
                return true;
            }
        }
        WTSFreeMemory(pInfoBuf);
    }
#endif

    // 3. Kontrol: Konsol oturumunda LogonUI.exe süreci
    // Not: LockApp.exe (Spotlight) kilit açıldıktan sonra bile UWP suspended olarak bellekte kalabildiği
    // için tek başına kilit göstergesi olarak kullanılmaz; LogonUI.exe yalnızca aktif Winlogon/kilit ekranında yaşar.
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"LogonUI.exe") == 0) {
                    DWORD procSessId = 0;
                    if (ProcessIdToSessionId(pe.th32ProcessID, &procSessId) && procSessId == sessId) {
                        CloseHandle(hSnap);
                        return true;
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    return false;
}
#else
bool SystemInfo::isWorkstationLocked()
{
    return false;
}
#endif

QString SystemInfo::activeDisplayUser()
{
#if defined(Q_OS_WIN)
    DWORD sessId = WTSGetActiveConsoleSessionId();
    if (sessId == 0xFFFFFFFF) {
        return QStringLiteral("winlogon");
    }

    LPWSTR pUser = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessId, WTSUserName, &pUser, &bytes) && pUser) {
        QString user = QString::fromWCharArray(pUser).trimmed();
        WTSFreeMemory(pUser);

        // Eğer oturum açmış bir kullanıcı varsa (ör. OPET-1 veya OPET-2),
        // DM/SYSTEM kullanıcısı değilse doğrudan bu kullanıcıyı dön.
        // Ekranın kilitli (Spotlight/LogonUI) olması kullanıcının OPET-1 olduğu gerçeğini değiştirmez;
        // Master paneli ve LocalServer yönlendirmesi her zaman oturum açan gerçek kullanıcıyı hedeflemelidir.
        if (!user.isEmpty() && !isDmUser(user)) {
            return user;
        }
    }
    return QStringLiteral("winlogon");
#else
    // Yöntem 1: loginctl seat0 aktif oturum
    {
        QProcess p1;
        p1.start("loginctl",
                 {"show-seat", "seat0", "--property=ActiveSession", "--value"});
        if (p1.waitForFinished(500) && p1.exitCode() == 0) {
            const QString sessionId =
                QString::fromLatin1(p1.readAllStandardOutput()).trimmed();
            if (!sessionId.isEmpty() &&
                sessionId != "0" &&
                sessionId != "(null)") {
                QProcess p2;
                p2.start("loginctl",
                         {"show-session", sessionId,
                          "--property=Name", "--value"});
                if (p2.waitForFinished(500) && p2.exitCode() == 0) {
                    const QString user =
                        QString::fromLatin1(p2.readAllStandardOutput()).trimmed();
                    if (!user.isEmpty()) {
                        return user;
                    }
                }
            }
        }
    }

    // Yöntem 1.5: loginctl list-sessions üzerinden aktif veya greeter (GDM/SDDM/LightDM) oturumunu bul
    {
        QProcess pList;
        pList.start(QStringLiteral("loginctl"), {QStringLiteral("list-sessions"), QStringLiteral("--no-legend")});
        if (pList.waitForFinished(500) && pList.exitCode() == 0) {
            QString fallbackGreeter;
            const QStringList lines = QString::fromLatin1(pList.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                if (parts.size() >= 3) {
                    const QString sessId = parts[0];
                    const QString sessUser = parts[2];
                    if (sessUser == QLatin1String("root")) continue;

                    QProcess pProps;
                    pProps.start(QStringLiteral("loginctl"),
                                 {QStringLiteral("show-session"), sessId,
                                  QStringLiteral("--property=State"),
                                  QStringLiteral("--property=Class"),
                                  QStringLiteral("--property=Type")});
                    if (pProps.waitForFinished(400) && pProps.exitCode() == 0) {
                        const QString out = QString::fromLatin1(pProps.readAllStandardOutput());
                        const bool isActive = out.contains(QStringLiteral("State=active"));
                        const bool isGreeter = out.contains(QStringLiteral("Class=greeter")) || isDmUser(sessUser);
                        const bool isGui = out.contains(QStringLiteral("Type=wayland")) || out.contains(QStringLiteral("Type=x11"));
                        if (isActive && isGui) {
                            return sessUser;
                        }
                        if (isGreeter && fallbackGreeter.isEmpty()) {
                            fallbackGreeter = sessUser;
                        }
                    }
                }
            }
            if (!fallbackGreeter.isEmpty()) {
                return fallbackGreeter;
            }
        }
    }

    // Yöntem 2: who çıktısında :0 ekranını kullanan kullanıcı
    {
        QProcess p;
        p.start("sh", {"-c", "who | awk '/:0/{print $1; exit}'"});
        if (p.waitForFinished(500) && p.exitCode() == 0) {
            const QString user =
                QString::fromLatin1(p.readAllStandardOutput()).trimmed();
            if (!user.isEmpty()) return user;
        }
    }

    // Yöntem 3: Aktif DM (GDM Wayland/Xorg, gdm-greeter DynamicUser, SDDM, LightDM) çalışma dizinlerinden gerçek DM kullanıcısını tespit et
    {
        QDir runUserDir(QStringLiteral("/run/user"));
        for (const QString& uidStr : runUserDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString rtDirPath = runUserDir.absoluteFilePath(uidStr);
            QDir rtDir(rtDirPath);
            const QStringList wl = rtDir.entryList(QStringList() << QStringLiteral("wayland-*"), QDir::System | QDir::Files);
            if (!wl.isEmpty() || QFile::exists(rtDirPath + QStringLiteral("/gdm/Xauthority"))) {
                bool okUid = false;
                uid_t u = static_cast<uid_t>(uidStr.toUInt(&okUid));
                if (okUid) {
                    struct passwd* pw = ::getpwuid(u);
                    if (pw && pw->pw_name) {
                        const QString uname = QString::fromUtf8(pw->pw_name);
                        if (isDmUser(uname)) return uname;
                    } else if (u >= 60000 || u < 1000) {
                        // systemd DynamicUser (gdm-greeter)
                        return QStringLiteral("gdm-greeter");
                    }
                }
            }
        }
    }
    if (QDir(QStringLiteral("/run/sddm")).exists() && !QDir(QStringLiteral("/run/sddm")).entryList(QDir::Files | QDir::Hidden).isEmpty()) {
        return QStringLiteral("sddm");
    }
    if (!dmXauthority().isEmpty()) {
        return QStringLiteral("lightdm");
    }

    return {};
#endif
}

QString SystemInfo::userDisplayName(const QString& username)
{
    if (username.isEmpty() || isDmUser(username) || username == QStringLiteral("root")) {
        return QString();
    }
#if defined(Q_OS_WIN)
    if (username.compare(QStringLiteral("winlogon"), Qt::CaseInsensitive) == 0 ||
        username.compare(QStringLiteral("SYSTEM"), Qt::CaseInsensitive) == 0) {
        return QString();
    }

    typedef DWORD(WINAPI* NetUserGetInfoFn)(LPCWSTR, LPCWSTR, DWORD, LPBYTE*);
    typedef DWORD(WINAPI* NetApiBufferFreeFn)(LPVOID);

    HMODULE hNetApi = LoadLibraryW(L"netapi32.dll");
    if (hNetApi) {
        auto pfnNetUserGetInfo = reinterpret_cast<NetUserGetInfoFn>(GetProcAddress(hNetApi, "NetUserGetInfo"));
        auto pfnNetApiBufferFree = reinterpret_cast<NetApiBufferFreeFn>(GetProcAddress(hNetApi, "NetApiBufferFree"));

        if (pfnNetUserGetInfo && pfnNetApiBufferFree) {
            LPBYTE pBuf = nullptr;
            struct USER_INFO_10_LOCAL {
                LPWSTR usri10_name;
                LPWSTR usri10_comment;
                LPWSTR usri10_usr_comment;
                LPWSTR usri10_full_name;
            };

            if (pfnNetUserGetInfo(nullptr, reinterpret_cast<LPCWSTR>(username.utf16()), 10, &pBuf) == 0 && pBuf) {
                auto* info = reinterpret_cast<USER_INFO_10_LOCAL*>(pBuf);
                QString fullName;
                if (info && info->usri10_full_name) {
                    fullName = QString::fromWCharArray(info->usri10_full_name).trimmed();
                }
                pfnNetApiBufferFree(pBuf);
                FreeLibrary(hNetApi);

                if (!fullName.isEmpty() && fullName != username) {
                    return fullName;
                }
                return QString();
            }
        }
        FreeLibrary(hNetApi);
    }
    return QString();
#else
    // 1. POSIX getpwnam (GECOS alanı: /etc/passwd veya LDAP/SSSD)
    struct passwd* pw = getpwnam(username.toUtf8().constData());
    if (pw && pw->pw_gecos) {
        QString gecos = QString::fromUtf8(pw->pw_gecos).split(',').first().trimmed();
        if (!gecos.isEmpty() && gecos != username) {
            return gecos;
        }
    }

    // 2. AccountsService fallback (/var/lib/AccountsService/users/<username>)
    QString actPath = QStringLiteral("/var/lib/AccountsService/users/") + username;
    if (QFile::exists(actPath)) {
        QSettings actSettings(actPath, QSettings::IniFormat);
        QString realName = actSettings.value(QStringLiteral("User/RealName")).toString().trimmed();
        if (!realName.isEmpty() && realName != username) {
            return realName;
        }
    }

    return QString();
#endif
}

QString SystemInfo::activeDmDisplay()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Console");
#else
    QProcess p1;
    p1.start("loginctl",
             {"show-seat", "seat0", "--property=ActiveSession", "--value"});
    if (p1.waitForFinished(500) && p1.exitCode() == 0) {
        const QString sessionId =
            QString::fromLatin1(p1.readAllStandardOutput()).trimmed();
        if (!sessionId.isEmpty() &&
            sessionId != "0" && sessionId != "(null)") {
            QProcess p2;
            p2.start("loginctl",
                     {"show-session", sessionId,
                      "--property=Display", "--value"});
            if (p2.waitForFinished(500) && p2.exitCode() == 0) {
                const QString display =
                    QString::fromLatin1(p2.readAllStandardOutput()).trimmed();
                if (!display.isEmpty()) {
                    Logger::info(QObject::tr("SystemInfo: Greeter ekranı -> %1").arg(display));
                    return display;
                }
            }
        }
    }
    return QStringLiteral(":0");
#endif
}

QString SystemInfo::dmXauthority()
{
#if defined(Q_OS_WIN)
    return {};
#else
    const QString display = activeDmDisplay();

    // 1. LightDM standart yolları
    for (const QString& base : {
            QStringLiteral("/run/lightdm/root"),
            QStringLiteral("/var/run/lightdm/root") })
    {
        const QString p = base + display;
        if (QFile::exists(p)) return p;
    }
    if (QFile::exists(QStringLiteral("/var/lib/lightdm/.Xauthority")))
        return QStringLiteral("/var/lib/lightdm/.Xauthority");

    // 2. SDDM dinamik {UUID} ve standart yolları (/run/sddm/{...} veya /var/run/sddm/{...})
    for (const QString& sddmDirPath : {
            QStringLiteral("/run/sddm"),
            QStringLiteral("/var/run/sddm") })
    {
        QDir sddmDir(sddmDirPath);
        if (sddmDir.exists()) {
            const QFileInfoList files = sddmDir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Time);
            for (const QFileInfo& fi : files) {
                if (fi.size() > 0) {
                    return fi.absoluteFilePath();
                }
            }
        }
    }
    if (QFile::exists(QStringLiteral("/var/lib/sddm/.Xauthority")))
        return QStringLiteral("/var/lib/sddm/.Xauthority");

    // 3. GDM / GDM3 (Xorg ve Xwayland yolları)
    for (const QString& gdmDirPath : {
            QStringLiteral("/run/gdm3"),
            QStringLiteral("/var/run/gdm3"),
            QStringLiteral("/run/gdm"),
            QStringLiteral("/var/run/gdm") })
    {
        QDir gdmDir(gdmDirPath);
        if (gdmDir.exists()) {
            const QStringList authDirs = gdmDir.entryList(QStringList() << QStringLiteral("auth-for-*"), QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString& ad : authDirs) {
                const QString dbPath = gdmDir.absoluteFilePath(ad + QStringLiteral("/database"));
                if (QFile::exists(dbPath)) return dbPath;
            }
        }
    }

    for (const char* dmName : {"gdm", "Debian-gdm", "gdm3", "sddm", "lightdm"}) {
        struct passwd* pw = ::getpwnam(dmName);
        if (!pw) continue;
        const QString rtDirPath = QStringLiteral("/run/user/%1").arg(pw->pw_uid);
        if (QFile::exists(rtDirPath + QStringLiteral("/gdm/Xauthority"))) {
            return rtDirPath + QStringLiteral("/gdm/Xauthority");
        }
        QDir rtDir(rtDirPath);
        if (rtDir.exists()) {
            const QStringList xfiles = rtDir.entryList(
                QStringList() << QStringLiteral(".mutter-Xwaylandauth*") << QStringLiteral("*Xauthority*") << QStringLiteral("xauth_*"),
                QDir::Files | QDir::Hidden | QDir::System);
            if (!xfiles.isEmpty()) {
                return rtDir.absoluteFilePath(xfiles.first());
            }
        }
    }

    // 4. Evrensel Fallback: Aktif seat0 oturumunun Leader PID'sinden veya Xorg -auth argümanından oku
    QProcess pSeat;
    pSeat.start(QStringLiteral("loginctl"), {QStringLiteral("show-seat"), QStringLiteral("seat0"), QStringLiteral("--property=ActiveSession"), QStringLiteral("--value")});
    if (pSeat.waitForFinished(400) && pSeat.exitCode() == 0) {
        const QString sessId = QString::fromLatin1(pSeat.readAllStandardOutput()).trimmed();
        if (!sessId.isEmpty() && sessId != QLatin1String("0")) {
            QProcess pLeader;
            pLeader.start(QStringLiteral("loginctl"), {QStringLiteral("show-session"), sessId, QStringLiteral("--property=Leader"), QStringLiteral("--value")});
            if (pLeader.waitForFinished(400) && pLeader.exitCode() == 0) {
                const QString lPid = QString::fromLatin1(pLeader.readAllStandardOutput()).trimmed();
                if (!lPid.isEmpty() && lPid != QLatin1String("0")) {
                    QFile envFile(QStringLiteral("/proc/%1/environ").arg(lPid));
                    if (envFile.open(QIODevice::ReadOnly)) {
                        for (const QByteArray& entry : envFile.readAll().split('\0')) {
                            if (entry.startsWith("XAUTHORITY=")) {
                                const QString xa = QString::fromUtf8(entry.mid(11));
                                if (QFile::exists(xa)) return xa;
                            }
                        }
                    }
                }
            }
        }
    }

    return {};
#endif
}

bool SystemInfo::isDmUser(const QString& username)
{
#if defined(Q_OS_WIN)
    return username.isEmpty() ||
           username.compare("winlogon", Qt::CaseInsensitive) == 0 ||
           username.compare("SYSTEM", Qt::CaseInsensitive) == 0 ||
           username.compare("LOCAL SERVICE", Qt::CaseInsensitive) == 0 ||
           username.compare("NETWORK SERVICE", Qt::CaseInsensitive) == 0 ||
           username.compare("DefaultAccount", Qt::CaseInsensitive) == 0;
#else
    const QString u = username.trimmed();
    if (u.isEmpty()) return true;
    if (u.startsWith(QStringLiteral("gdm"), Qt::CaseInsensitive) ||
        u.contains(QStringLiteral("greeter"), Qt::CaseInsensitive)) {
        return true;
    }
    return u == QLatin1String("lightdm")    ||
           u == QLatin1String("sddm")       ||
           u == QLatin1String("Debian-gdm") ||
           u == QLatin1String("lxdm")       ||
           u == QLatin1String("kdm")        ||
           u == QLatin1String("xdm")        ||
           u == QLatin1String("slim");
#endif
}

bool SystemInfo::launchProcessInConsoleSession(const QString& appPath, const QStringList& args, void** outProcessHandle)
{
#if defined(Q_OS_WIN)
    DWORD sessId = WTSGetActiveConsoleSessionId();
    if (sessId == 0xFFFFFFFF) return false;

    bool isWinlogon = false;
    for (const QString& a : args) {
        if (a.contains("winlogon", Qt::CaseInsensitive)) {
            isWinlogon = true;
            break;
        }
    }

    HANDLE hTargetToken = NULL;

    if (!isWinlogon) {
        // Gerçek kullanıcı oturumu: aktif oturumun birincil kullanıcı token'ını sorgula
        HANDLE hUserToken = NULL;
        if (WTSQueryUserToken(sessId, &hUserToken)) {
            if (!DuplicateTokenEx(hUserToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hTargetToken)) {
                hTargetToken = hUserToken;
            } else {
                CloseHandle(hUserToken);
            }
        } else {
            // Kullanıcı oturumu henüz hazır değil veya aktif bir kullanıcı token'ı yok.
            // SYSTEM token'ı ile winsta0\default üzerinde GUI süreci başlatmak Qt platform plugin (qwindows.dll)
            // başlatma hatasına ve çökme döngüsüne yol açar; bu nedenle SYSTEM fallback'i engellenir.
            Logger::warning(QObject::tr("SystemInfo: Konsol oturumu (%1) için kullanıcı token'ı alınamadı, SYSTEM hesabı ile GUI başlatma engellendi.").arg(sessId));
            return false;
        }
    }

    // Yalnızca isWinlogon ise servis (SYSTEM) token'ını çoğaltıp oturuma ata
    if (!hTargetToken) {
        HANDLE hServiceToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &hServiceToken)) {
            return false;
        }

        if (!DuplicateTokenEx(hServiceToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hTargetToken)) {
            CloseHandle(hServiceToken);
            return false;
        }
        CloseHandle(hServiceToken);

        if (!SetTokenInformation(hTargetToken, TokenSessionId, &sessId, sizeof(DWORD))) {
            CloseHandle(hTargetToken);
            return false;
        }
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);

    if (isWinlogon) {
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\Winlogon");
    } else {
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    }

    LPVOID pEnv = nullptr;
    // Session 0 (SYSTEM) ortam değişkenlerinin (örn. QT_QPA_PLATFORM) kullanıcı oturumuna
    // miras kalıp okidia-tray.exe sürecini bozmasını engellemek için bInherit = FALSE kullanılır.
    CreateEnvironmentBlock(&pEnv, hTargetToken, FALSE);

    PROCESS_INFORMATION pi = {};
    QString cmdLine = "\"" + QDir::toNativeSeparators(appPath) + "\"";
    for (const QString& a : args) {
        cmdLine += " \"" + a + "\"";
    }

    std::wstring wCmdLine = cmdLine.toStdWString();
    std::wstring wWorkDir = QDir::toNativeSeparators(QFileInfo(appPath).absolutePath()).toStdWString();
    BOOL res = CreateProcessAsUserW(
        hTargetToken,
        NULL,
        &wCmdLine[0],
        NULL,
        NULL,
        FALSE,
        NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
        pEnv,
        wWorkDir.empty() ? NULL : wWorkDir.c_str(),
        &si,
        &pi
    );

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hTargetToken);

    if (res) {
        CloseHandle(pi.hThread);
        if (outProcessHandle) {
            *outProcessHandle = pi.hProcess;
        } else {
            CloseHandle(pi.hProcess);
        }
        return true;
    }
    return false;
#else
    Q_UNUSED(appPath);
    Q_UNUSED(args);
    Q_UNUSED(outProcessHandle);
    return false;
#endif
}

bool SystemInfo::launchInputHelperInConsoleSession(const QString& appPath, const QStringList& args, void** outProcessHandle)
{
#if defined(Q_OS_WIN)
    DWORD sessId = WTSGetActiveConsoleSessionId();
    if (sessId == 0xFFFFFFFF) return false;

    HANDLE hServiceToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &hServiceToken)) {
        Logger::warning(QObject::tr("SystemInfo: Input helper için servis token'ı açılamadı (Hata: %1)").arg(GetLastError()));
        return false;
    }

    HANDLE hTargetToken = NULL;
    if (!DuplicateTokenEx(hServiceToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hTargetToken)) {
        Logger::warning(QObject::tr("SystemInfo: Input helper için servis token'ı çoğaltılamadı (Hata: %1)").arg(GetLastError()));
        CloseHandle(hServiceToken);
        return false;
    }
    CloseHandle(hServiceToken);

    if (!SetTokenInformation(hTargetToken, TokenSessionId, &sessId, sizeof(DWORD))) {
        Logger::warning(QObject::tr("SystemInfo: Input helper token oturumu (%1) atanamadı (Hata: %2)").arg(sessId).arg(GetLastError()));
        CloseHandle(hTargetToken);
        return false;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    LPVOID pEnv = nullptr;
    CreateEnvironmentBlock(&pEnv, hTargetToken, FALSE);

    PROCESS_INFORMATION pi = {};
    QString cmdLine = "\"" + QDir::toNativeSeparators(appPath) + "\"";
    for (const QString& a : args) {
        cmdLine += " \"" + a + "\"";
    }

    std::wstring wCmdLine = cmdLine.toStdWString();
    std::wstring wWorkDir = QDir::toNativeSeparators(QFileInfo(appPath).absolutePath()).toStdWString();
    BOOL res = CreateProcessAsUserW(
        hTargetToken,
        NULL,
        &wCmdLine[0],
        NULL,
        NULL,
        FALSE,
        NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        pEnv,
        wWorkDir.empty() ? NULL : wWorkDir.c_str(),
        &si,
        &pi
    );

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hTargetToken);

    if (res) {
        CloseHandle(pi.hThread);
        if (outProcessHandle) {
            *outProcessHandle = pi.hProcess;
        } else {
            CloseHandle(pi.hProcess);
        }
        return true;
    }
    Logger::warning(QObject::tr("SystemInfo: Input helper başlatılamadı (Hata: %1)").arg(GetLastError()));
    return false;
#else
    Q_UNUSED(appPath);
    Q_UNUSED(args);
    Q_UNUSED(outProcessHandle);
    return false;
#endif
}

void SystemInfo::applyWindowsUnattendedPolicies()
{
#if defined(Q_OS_WIN)
    auto ensureDword = [](HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD targetVal, int& updatedCount) {
        HKEY hKey = NULL;
        LONG res = RegCreateKeyExW(
            root,
            subKey,
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            NULL,
            &hKey,
            NULL
        );
        if (res != ERROR_SUCCESS) {
            return;
        }

        DWORD currentVal = 0;
        DWORD dataSize = sizeof(currentVal);
        DWORD type = 0;
        const bool alreadySet = (RegQueryValueExW(hKey, valueName, NULL, &type, reinterpret_cast<LPBYTE>(&currentVal), &dataSize) == ERROR_SUCCESS &&
                                 type == REG_DWORD && currentVal == targetVal);
        if (!alreadySet) {
            if (RegSetValueExW(hKey, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&targetVal), sizeof(targetVal)) == ERROR_SUCCESS) {
                ++updatedCount;
            }
        }
        RegCloseKey(hKey);
    };

    int updated = 0;
    // 1. CloudContent — SCOOBE / Tüketici önerileri / Soft Landing kapatma (OneDrive eşitlemesine dokunulmaz)
    const wchar_t* cloudContentKey = L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent";
    ensureDword(HKEY_LOCAL_MACHINE, cloudContentKey, L"DisableCloudOptimizedContent", 1, updated);
    ensureDword(HKEY_LOCAL_MACHINE, cloudContentKey, L"DisableConsumerAccountStateContent", 1, updated);
    ensureDword(HKEY_LOCAL_MACHINE, cloudContentKey, L"DisableWindowsConsumerFeatures", 1, updated);
    ensureDword(HKEY_LOCAL_MACHINE, cloudContentKey, L"DisableSoftLanding", 1, updated);

    // 2. OOBE — İlk açılış gizlilik/kurulum sihirbazı kapatma
    ensureDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\OOBE", L"DisablePrivacyExperience", 1, updated);

    // 3. System — İlk oturum açma animasyonunu kapatma
    ensureDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableFirstLogonAnimation", 0, updated);

    if (updated > 0) {
        Logger::info(QObject::tr("SystemInfo: Windows SCOOBE/OOBE katılımsız çalışma politikaları uygulandı (%1 HKLM anahtarı güncellendi).").arg(updated));
    }
#endif
}

void SystemInfo::applyWindowsUserUnattendedPolicies()
{
#if defined(Q_OS_WIN)
    auto ensureDword = [](HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD targetVal, int& updatedCount) {
        HKEY hKey = NULL;
        LONG res = RegCreateKeyExW(
            root,
            subKey,
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            NULL,
            &hKey,
            NULL
        );
        if (res != ERROR_SUCCESS) {
            return;
        }

        DWORD currentVal = 0;
        DWORD dataSize = sizeof(currentVal);
        DWORD type = 0;
        const bool alreadySet = (RegQueryValueExW(hKey, valueName, NULL, &type, reinterpret_cast<LPBYTE>(&currentVal), &dataSize) == ERROR_SUCCESS &&
                                 type == REG_DWORD && currentVal == targetVal);
        if (!alreadySet) {
            if (RegSetValueExW(hKey, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&targetVal), sizeof(targetVal)) == ERROR_SUCCESS) {
                ++updatedCount;
            }
        }
        RegCloseKey(hKey);
    };

    int updated = 0;
    // 1. UserProfileEngagement — "Cihazınızı kurmayı bitirelim" (SCOOBE)
    ensureDword(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\UserProfileEngagement",
                L"ScoobeSystemSettingEnabled",
                0,
                updated);

    // 2. ContentDeliveryManager — Welcome Experience ve ipuçları/öneriler
    const wchar_t* cdmKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
    ensureDword(HKEY_CURRENT_USER, cdmKey, L"SubscribedContent-310093Enabled", 0, updated);
    ensureDword(HKEY_CURRENT_USER, cdmKey, L"SubscribedContent-338389Enabled", 0, updated);
    ensureDword(HKEY_CURRENT_USER, cdmKey, L"SubscribedContent-338393Enabled", 0, updated);
    ensureDword(HKEY_CURRENT_USER, cdmKey, L"SubscribedContent-353694Enabled", 0, updated);
    ensureDword(HKEY_CURRENT_USER, cdmKey, L"SubscribedContent-353696Enabled", 0, updated);

    if (updated > 0) {
        Logger::info(QObject::tr("SystemInfo: Kullanıcı bazlı Windows SCOOBE/Welcome anahtarları uygulandı (%1 HKCU anahtarı güncellendi).").arg(updated));
    }
#endif
}

bool SystemInfo::launchProcessInUserSession(const QString& username, const QString& appPath, const QStringList& args)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(username);
    return launchProcessInConsoleSession(appPath, args, nullptr);
#else
    if (username.isEmpty() || isDmUser(username) || username == "root") {
        return false;
    }

    struct passwd* pw = getpwnam(username.toUtf8().constData());
    if (!pw) {
        Logger::warning(QObject::tr("SystemInfo: Kullanıcı bulunamadı: ") + username);
        return false;
    }
    uid_t uid = pw->pw_uid;

    QString display = ":0";
    QString xauth;
    QString waylandDisplay;
    QString sessionType;

    // Yöntem 1: loginctl üzerinden hedef kullanıcının oturum parametrelerini al
    QString sessId;
    {
        QProcess pList;
        pList.start("loginctl", {"list-sessions", "--no-legend"});
        if (pList.waitForFinished(500) && pList.exitCode() == 0) {
            const QStringList lines = QString::fromLatin1(pList.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                if (parts.size() >= 3 && parts[2] == username) {
                    sessId = parts[0];
                    break;
                }
            }
        }
        if (sessId.isEmpty()) {
            QProcess pSeat;
            pSeat.start("loginctl", {"show-seat", "seat0", "--property=ActiveSession", "--value"});
            if (pSeat.waitForFinished(500) && pSeat.exitCode() == 0) {
                sessId = QString::fromLatin1(pSeat.readAllStandardOutput()).trimmed();
            }
        }
    }

    if (!sessId.isEmpty() && sessId != "0" && sessId != "(null)") {
        QProcess pType;
        pType.start("loginctl", {"show-session", sessId, "--property=Type", "--value"});
        if (pType.waitForFinished(500) && pType.exitCode() == 0) {
            sessionType = QString::fromLatin1(pType.readAllStandardOutput()).trimmed();
        }

        QProcess pDisp;
        pDisp.start("loginctl", {"show-session", sessId, "--property=Display", "--value"});
        if (pDisp.waitForFinished(500) && pDisp.exitCode() == 0) {
            QString d = QString::fromLatin1(pDisp.readAllStandardOutput()).trimmed();
            if (!d.isEmpty()) display = d;
        }
        QProcess pLeader;
        pLeader.start("loginctl", {"show-session", sessId, "--property=Leader", "--value"});
        if (pLeader.waitForFinished(500) && pLeader.exitCode() == 0) {
            QString lPid = QString::fromLatin1(pLeader.readAllStandardOutput()).trimmed();
            if (!lPid.isEmpty() && lPid != "0") {
                QFile envFile(QString("/proc/%1/environ").arg(lPid));
                if (envFile.open(QIODevice::ReadOnly)) {
                    const QByteArray data = envFile.readAll();
                    const auto entries = data.split('\0');
                    for (const auto& entry : entries) {
                        if (entry.startsWith("XAUTHORITY=")) {
                            xauth = QString::fromUtf8(entry.mid(11));
                        } else if (entry.startsWith("DISPLAY=") && display == ":0") {
                            display = QString::fromUtf8(entry.mid(8));
                        } else if (entry.startsWith("WAYLAND_DISPLAY=")) {
                            waylandDisplay = QString::fromUtf8(entry.mid(16));
                        } else if (entry.startsWith("XDG_SESSION_TYPE=") && sessionType.isEmpty()) {
                            sessionType = QString::fromUtf8(entry.mid(17));
                        }
                    }
                }
            }
        }
    }

    // Yöntem 2: Standart dosya yolları (Xauthority + Wayland soketi + Xwayland)
    const QString runtimeDir = QString("/run/user/%1").arg(uid);
    if (waylandDisplay.isEmpty()) {
        QDir rtDir(runtimeDir);
        const QStringList wlSockets = rtDir.entryList(QStringList() << "wayland-*", QDir::System | QDir::Files);
        for (const QString& sock : wlSockets) {
            if (!sock.endsWith(".lock")) {
                waylandDisplay = sock;
                break;
            }
        }
        if (waylandDisplay.isEmpty() && sessionType == "wayland" && QFile::exists(runtimeDir + "/wayland-0")) {
            waylandDisplay = QStringLiteral("wayland-0");
        }
    }

    if (sessionType.isEmpty()) {
        sessionType = waylandDisplay.isEmpty() ? QStringLiteral("x11") : QStringLiteral("wayland");
    }

    if (xauth.isEmpty() || !QFile::exists(xauth)) {
        for (const QString& cand : {
            QString("/run/user/%1/gdm/Xauthority").arg(uid),
            QString("/run/user/%1/xauth").arg(uid),
            QString("/home/%1/.Xauthority").arg(username)
        }) {
            if (QFile::exists(cand)) {
                xauth = cand;
                break;
            }
        }
    }

    if (xauth.isEmpty() || !QFile::exists(xauth)) {
        QDir runUserDir(runtimeDir);
        QStringList xauthFiles = runUserDir.entryList(QStringList() << ".mutter-Xwaylandauth*" << "*Xauthority*" << "xauth_*", QDir::Files | QDir::Hidden);
        if (!xauthFiles.isEmpty()) {
            xauth = runUserDir.absoluteFilePath(xauthFiles.first());
        }
    }

    if (xauth.isEmpty() || !QFile::exists(xauth)) {
        xauth = QString("/home/%1/.Xauthority").arg(username);
    }

    const QString dbusAddr = QString("unix:path=/run/user/%1/bus").arg(uid);

    // Sistem dizininde (/usr/share/gnome-shell/extensions/okidia-kiosk@okidia.net) eklenti kuruluysa
    // kullanıcının ev dizinindeki (.local/share) kopyayı temizle ki SYSTEM eklentisi olarak yüklensin
    const QString sysExtDir = QStringLiteral("/usr/share/gnome-shell/extensions/okidia-kiosk@okidia.net");
    if (QFile::exists(sysExtDir + QStringLiteral("/extension.js")) && pw->pw_dir) {
        const QString userHome = QString::fromUtf8(pw->pw_dir);
        const QString userExtDir = userHome + QStringLiteral("/.local/share/gnome-shell/extensions/okidia-kiosk@okidia.net");
        if (QDir(userExtDir).exists()) {
            QDir(userExtDir).removeRecursively();
        }
    }

    QString waylandExports;
    if (!waylandDisplay.isEmpty()) {
        waylandExports = QString("QT_QPA_PLATFORM='wayland;xcb' WAYLAND_DISPLAY='%1' XDG_SESSION_TYPE='%2' ").arg(waylandDisplay, sessionType);
    } else {
        waylandExports = QString("XDG_SESSION_TYPE='%1' ").arg(sessionType);
    }

    QString cmdArgs = args.join(' ');
    QString fullCmd = QString("export %1DISPLAY='%2' XAUTHORITY='%3' XDG_RUNTIME_DIR='%4' PULSE_RUNTIME_PATH='%4/pulse' DBUS_SESSION_BUS_ADDRESS='%5'; "
                              "gnome-extensions enable okidia-kiosk@okidia.net >/dev/null 2>&1 || true; "
                              "nohup \"%6\" %7 >/dev/null 2>&1 &")
                      .arg(waylandExports, display, xauth, runtimeDir, dbusAddr, appPath, cmdArgs);

    QProcess proc;
    proc.start("su", {"-", username, "-c", fullCmd});
    bool started = proc.waitForFinished(2000);
    return started && (proc.exitCode() == 0);
#endif
}

bool SystemInfo::getAudioSession(QString& outUsername, QString& outUid)
{
    QString user = activeDisplayUser();
    if (user.isEmpty()) {
#if defined(Q_OS_WIN)
        user = "User";
#else
        user = "root";
#endif
    }

    outUsername = user;

#if defined(Q_OS_WIN)
    outUid = "1000";
    return true;
#else
    auto hasAudioSocket = [](const QString& uidStr) -> bool {
        if (uidStr.isEmpty() || uidStr == QLatin1String("0")) return false;
        const QString base = QStringLiteral("/run/user/") + uidStr;
        return QFile::exists(base + QStringLiteral("/pulse/native")) ||
               QFile::exists(base + QStringLiteral("/pipewire-0"));
    };

    // 1. Önce aktif GUI kullanıcısının kendi ses soketi (PulseAudio/PipeWire) var mı doğrula
    QString activeUid;
    if (!user.isEmpty() && user != QLatin1String("root")) {
        if (struct passwd* pw = ::getpwnam(user.toUtf8().constData())) {
            activeUid = QString::number(pw->pw_uid);
        } else {
            QProcess p;
            p.start(QStringLiteral("id"), {QStringLiteral("-u"), user});
            if (p.waitForFinished(1000) && p.exitCode() == 0) {
                activeUid = QString::fromLatin1(p.readAllStandardOutput()).trimmed();
            }
        }
        if (hasAudioSocket(activeUid)) {
            outUsername = user;
            outUid = activeUid;
            return true;
        }
    }

    // 2. Aktif ekran kullanıcısının ses soketi yoksa (örn. X11 LightDM greeter),
    //    /run/user/* altında canlı pulse/pipewire soketine sahip bir oturum ara (önce UID >= 1000).
    QString bestNormalUser, bestNormalUid;
    QString bestOtherUser, bestOtherUid;
    QDir runUserDir(QStringLiteral("/run/user"));
    for (const QString& uidStr : runUserDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!hasAudioSocket(uidStr)) continue;
        bool ok = false;
        const uint u = uidStr.toUInt(&ok);
        if (!ok || u == 0) continue;

        QString uname;
        if (struct passwd* pw = ::getpwuid(static_cast<uid_t>(u))) {
            if (pw->pw_name) uname = QString::fromUtf8(pw->pw_name);
        }
        if (uname.isEmpty()) uname = QStringLiteral("uid-") + uidStr;

        if (u >= 1000 && u < 60000 && bestNormalUid.isEmpty()) {
            bestNormalUser = uname;
            bestNormalUid = uidStr;
        } else if (bestOtherUid.isEmpty()) {
            bestOtherUser = uname;
            bestOtherUid = uidStr;
        }
    }

    if (!bestNormalUid.isEmpty()) {
        outUsername = bestNormalUser;
        outUid = bestNormalUid;
        return true;
    }
    if (!bestOtherUid.isEmpty()) {
        outUsername = bestOtherUser;
        outUid = bestOtherUid;
        return true;
    }

    // 3. Canlı soket bulunamadıysa en azından aktif kullanıcının geçerli /run/user/<uid> dizinini dön
    if (!activeUid.isEmpty() && QDir(QStringLiteral("/run/user/") + activeUid).exists()) {
        outUsername = user;
        outUid = activeUid;
        return true;
    }

    return false;
#endif
}

void SystemInfo::setSwitchUserDisabled(bool disabled)
{
#if defined(Q_OS_WIN)
    HKEY hKey;
    LONG res = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                               0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL);
    if (res == ERROR_SUCCESS) {
        DWORD val = disabled ? 1 : 0;
        RegSetValueExW(hKey, L"HideFastUserSwitching", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
        Logger::info(QObject::tr("SystemInfo: Windows HideFastUserSwitching güncellendi → ") + QString::number(val));
    }

    // Windows Shell ve LogonUI'ye ayar değişikliği anonsu (yeniden başlatma gerekmemesi için)
    DWORD_PTR dwResult = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"), SMTO_ABORTIFHUNG, 1000, &dwResult);
#else
    // 1. Polkit Kuralı (/etc/polkit-1/rules.d/99-okidia-no-switch.rules)
    const QString polkitPath = "/etc/polkit-1/rules.d/99-okidia-no-switch.rules";
    if (disabled) {
        QFile file(polkitPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&file);
            out << "// Okidia Agent - Switch User Kisitlama Kurali\n";
            out << "polkit.addRule(function(action, subject) {\n";
            out << "    if (action.id == \"org.freedesktop.DisplayManager.AccountsService.SwitchToUser\" ||\n";
            out << "        action.id == \"org.freedesktop.login1.chvt\") {\n";
            out << "        return polkit.Result.NO;\n";
            out << "    }\n";
            out << "});\n";
            file.close();
            QFile::setPermissions(polkitPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
            Logger::info(QObject::tr("SystemInfo: Polkit switch-user engelleme kuralı yazıldı: ") + polkitPath);
        }
    } else {
        if (QFile::exists(polkitPath)) {
            QFile::remove(polkitPath);
            Logger::info(QObject::tr("SystemInfo: Polkit switch-user kuralı kaldırıldı: ") + polkitPath);
        }
    }

    // 2. GNOME dconf Lockdown (/etc/dconf/db/local.d/00-okidia)
    const QString dconfDir = "/etc/dconf/db/local.d";
    const QString dconfPath = dconfDir + "/00-okidia";
    const QString dconfLockDir = dconfDir + "/locks";
    const QString dconfLockPath = dconfLockDir + "/00-okidia";
    if (disabled) {
        QDir().mkpath(dconfDir);
        QDir().mkpath(dconfLockDir);
        QFile file(dconfPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&file);
            out << "[org/gnome/desktop/lockdown]\n";
            out << "disable-user-switching=true\n";
            file.close();
        }
        QFile lockFile(dconfLockPath);
        if (lockFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&lockFile);
            out << "/org/gnome/desktop/lockdown/disable-user-switching\n";
            lockFile.close();
        }
        QProcess::execute("dconf", {"update"});
        Logger::info(QObject::tr("SystemInfo: GNOME dconf switch-user engellendi."));
    } else {
        if (QFile::exists(dconfPath)) {
            QFile::remove(dconfPath);
            QFile::remove(dconfLockPath);
            QProcess::execute("dconf", {"update"});
            Logger::info(QObject::tr("SystemInfo: GNOME dconf switch-user engeli kaldırıldı."));
        }
    }

    // 3. LightDM Kuralı (/etc/lightdm/lightdm.conf.d/99-okidia.conf)
    const QString lightdmDir = "/etc/lightdm/lightdm.conf.d";
    const QString lightdmPath = lightdmDir + "/99-okidia.conf";
    if (disabled) {
        QDir().mkpath(lightdmDir);
        QFile file(lightdmPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&file);
            out << "[Seat:*]\n";
            out << "allow-user-switching=false\n";
            file.close();
            Logger::info(QObject::tr("SystemInfo: LightDM allow-user-switching=false kuralı yazıldı: ") + lightdmPath);
        }
    } else {
        if (QFile::exists(lightdmPath)) {
            QFile::remove(lightdmPath);
            Logger::info(QObject::tr("SystemInfo: LightDM switch-user kuralı kaldırıldı: ") + lightdmPath);
        }
    }
#endif
}

void SystemInfo::setUsbProtectionEnabled(bool enabled) {
#if defined(Q_OS_LINUX)
    const QString udevRulePath = "/etc/udev/rules.d/99-okidia-usb-repair.rules";
    if (enabled) {
        QString rules = 
            "# Okidia USB Flas Bellek Otomatik Dosya Sistemi Onarimi (Auto-Heal)\n"
            "ACTION==\"add\", SUBSYSTEM==\"block\", ENV{DEVTYPE}==\"partition\", ENV{ID_BUS}==\"usb\", ENV{ID_FS_TYPE}==\"vfat\", RUN+=\"/bin/sh -c 'fsck.vfat -a -w /dev/%k || true'\"\n"
            "ACTION==\"add\", SUBSYSTEM==\"block\", ENV{DEVTYPE}==\"partition\", ENV{ID_BUS}==\"usb\", ENV{ID_FS_TYPE}==\"ntfs\", RUN+=\"/bin/sh -c 'ntfsfix -b -d /dev/%k || true'\"\n"
            "ACTION==\"add\", SUBSYSTEM==\"block\", ENV{DEVTYPE}==\"partition\", ENV{ID_BUS}==\"usb\", ENV{ID_FS_TYPE}==\"exfat\", RUN+=\"/bin/sh -c 'fsck.exfat -p -y /dev/%k || true'\"\n";

        QFile f(udevRulePath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(rules.toUtf8());
            f.close();
            QProcess::execute("udevadm", {"control", "--reload-rules"});
            Logger::info(QObject::tr("SystemInfo: USB dosya sistemi otomatik onarım udev kuralı aktif edildi."));
        } else {
            Logger::warning(QObject::tr("SystemInfo: udev kuralı yazılamadı: ") + udevRulePath);
        }
    } else {
        if (QFile::exists(udevRulePath)) {
            QFile::remove(udevRulePath);
            QProcess::execute("udevadm", {"control", "--reload-rules"});
            Logger::info(QObject::tr("SystemInfo: USB dosya sistemi otomatik onarım udev kuralı kaldırıldı."));
        }
    }
#else
    Q_UNUSED(enabled);
#endif
}

QList<SystemInfo::UsbDriveInfo> SystemInfo::mountedUsbDrives() {
    QList<UsbDriveInfo> drives;
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const auto& storage : volumes) {
        if (!storage.isValid() || !storage.isReady()) continue;
        QString root = storage.rootPath();
        QString dev = QString::fromUtf8(storage.device());

#if defined(Q_OS_WIN)
        UINT dType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
        if (dType == DRIVE_REMOVABLE) {
            UsbDriveInfo info;
            info.rootPath = root;
            info.device = root;
            info.displayName = storage.name().isEmpty() ? root : QString("%1 (%2)").arg(storage.name(), root);
            drives.append(info);
        }
#else
        if (root.startsWith("/media/") || root.startsWith("/run/media/")) {
            UsbDriveInfo info;
            info.rootPath = root;
            info.device = dev;
            QString name = storage.name();
            if (name.isEmpty()) {
                name = QFileInfo(root).fileName();
            }
            info.displayName = name;
            drives.append(info);
        }
#endif
    }
    return drives;
}

bool SystemInfo::safeEjectUsbDrives() {
#if defined(Q_OS_WIN)
    _flushall();
    return true;
#else
    QProcess pSync;
    pSync.start("sync", {});
    pSync.waitForFinished(3000);

    const auto drives = mountedUsbDrives();
    if (drives.isEmpty()) return true;

    bool allSuccess = true;
    for (const auto& d : drives) {
        QProcess p;
        p.start("udisksctl", {"unmount", "-b", d.device});
        if (!p.waitForFinished(3000) || p.exitCode() != 0) {
            QProcess p2;
            p2.start("umount", {d.rootPath});
            if (!p2.waitForFinished(3000) || p2.exitCode() != 0) {
                allSuccess = false;
            }
        }
    }
    return allSuccess;
#endif
}

bool SystemInfo::isRtcBatteryFaulty() {
    // 1. Sistem saati 2024 yılından eskiyse (örn. 1970 Epoch veya 2000 BIOS default) pil kesinlikle bitmiştir
    const int currentYear = QDateTime::currentDateTime().date().year();
    if (currentYear < 2024) {
        return true;
    }

#if !defined(Q_OS_WIN)
    // 2. Linux: /sys/class/rtc/rtc0 kontrolü
    QFile rtcDate("/sys/class/rtc/rtc0/date");
    if (rtcDate.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString line = QString::fromUtf8(rtcDate.readLine()).trimmed();
        rtcDate.close();
        if (!line.isEmpty()) {
            int rtcYear = line.section('-', 0, 0).toInt();
            if (rtcYear > 1900 && rtcYear < 2024) {
                return true;
            }
        }
    }
#endif

    return false;
}

