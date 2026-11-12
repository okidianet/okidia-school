#include "OtaUpdater.h"
#include "../common/utils/Logger.h"
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTemporaryFile>
#include <QCryptographicHash>
#include <QObject>

#include "../common/crypto/KeyManager.h"
#include "../common/OkidiaConfig.h"

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

OtaUpdater::OtaUpdater(QObject* parent) : QObject(parent) {}

void OtaUpdater::installUpdate(const QString& pkgPath, const QByteArray& signature, const QByteArray& expectedHash, const QByteArray& masterCert) {
    Logger::info(QObject::tr("OtaUpdater: OTA güncelleme güvenlik doğrulaması başlatılıyor -> %1").arg(pkgPath));
    
    // 1. Kök sertifikaya güven doğrulaması
    QString caPath = OkidiaConfig::agentCertDir() + "/ca.crt";
    if (!QFile::exists(caPath)) {
        caPath = OkidiaConfig::systemBasePath() + "/certs/ca.crt";
    }
    if (!QFile::exists(caPath)) {
        caPath = OkidiaConfig::certDir() + "/ca.crt";
    }
    if (!KeyManager::verifyCertificateChain(masterCert, caPath)) {
        Logger::error(QObject::tr("OtaUpdater: OTA sertifikası geçersiz! Kök CA tarafından imzalanmamış. Güncelleme iptal edildi."));
        QFile::remove(pkgPath);
        return;
    }

    // 2. İmza Doğrulaması
    if (!KeyManager::verifySignatureWithCert(expectedHash, signature, masterCert)) {
        Logger::error(QObject::tr("OtaUpdater: OTA paket imzası GEÇERSİZ! Paket manipüle edilmiş olabilir. Güncelleme iptal edildi."));
        QFile::remove(pkgPath);
        return;
    }

    // 3. Dosya Hash Doğrulaması
    QFile file(pkgPath);
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("OtaUpdater: İndirilen OTA dosyası okunamıyor: %1").arg(pkgPath));
        return;
    }
    
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(&file);
    QByteArray actualHash = hasher.result();
    file.close();

    if (actualHash != expectedHash) {
        Logger::error(QObject::tr("OtaUpdater: OTA paketi bozuk veya hash uyuşmuyor! Beklenen: %1, Gelen: %2")
                     .arg(QString(expectedHash.toHex()), QString(actualHash.toHex())));
        QFile::remove(pkgPath);
        return;
    }

    Logger::info(QObject::tr("OtaUpdater: OTA paketi GÜVENLİ. Kurulum başlatılıyor..."));
    
    // ── Yerel Kararlı Paket Depolama (Rollback Cache) ─────────────────────
    {
        const QString cacheDir = OkidiaConfig::rollbackCacheDir();
        const QString cachePkg = cachedPackagePath();
        if (QFile::exists(cachePkg)) {
            QFile::remove(cachePkg);
        }
        if (QFile::copy(pkgPath, cachePkg)) {
            Logger::info(QObject::tr("OtaUpdater: Kararlı paket yerel geri alma (rollback) deposuna arşivlendi: %1").arg(cachePkg));
        } else {
            Logger::warning(QObject::tr("OtaUpdater: Kararlı paket rollback deposuna kopyalanamadı!"));
        }
    }

#if defined(Q_OS_WIN)
    // Windows OTA Kurulumu (Inno Setup Sessiz Kurulum)
    QString nativePkgPath = QDir::toNativeSeparators(pkgPath);
    QString scriptPath = QDir::tempPath() + "/okidia_ota_install.bat";
    QString nativeScriptPath = QDir::toNativeSeparators(scriptPath);
    QString otaLogPath = QDir::toNativeSeparators(QDir::tempPath() + "/okidia_agent_ota.log");
    QString innoLogPath = QDir::toNativeSeparators(QDir::tempPath() + "/okidia_agent_inno.log");
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString bat = "@echo off\r\n"
                      "setlocal enabledelayedexpansion\r\n"
                      "echo [%date% %time%] OTA Update script baslatildi. >> \"" + otaLogPath + "\"\r\n"
                      "echo [%date% %time%] Paket: \"" + nativePkgPath + "\" >> \"" + otaLogPath + "\"\r\n"
                      "echo [%date% %time%] Servis ve surecler durduruluyor... >> \"" + otaLogPath + "\"\r\n"
                      "net stop OkidiaAgent /y >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "sc stop OkidiaAgent >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "ping 127.0.0.1 -n 3 >nul\r\n"
                      "taskkill /F /IM okidia-agent.exe /T >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "taskkill /F /IM okidia-tray.exe /T >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "echo [%date% %time%] Servis durduruldu. DLL kilitlerinin serbest kalmasi bekleniyor... >> \"" + otaLogPath + "\"\r\n"
                      "ping 127.0.0.1 -n 3 >nul\r\n"
                      "echo [%date% %time%] Kurulum baslatiliyor (start /wait)... >> \"" + otaLogPath + "\"\r\n"
                      "start \"\" /wait \"" + nativePkgPath + "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=\"" + innoLogPath + "\"\r\n"
                      "set SETUP_EXIT=!errorlevel!\r\n"
                      "echo [%date% %time%] Kurulum tamamlandi. Exit code: !SETUP_EXIT! >> \"" + otaLogPath + "\"\r\n"
                      "echo [%date% %time%] Servis baslatiliyor... >> \"" + otaLogPath + "\"\r\n"
                      "sc start OkidiaAgent >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "net start OkidiaAgent >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "echo [%date% %time%] Temizlik yapiliyor... >> \"" + otaLogPath + "\"\r\n"
                      "del /F /Q \"" + nativePkgPath + "\"\r\n"
                      "del /F /Q \"%~f0\"\r\n";
        script.write(bat.toUtf8());
        script.close();
        
        Logger::info(QObject::tr("OtaUpdater: Windows güncelleme scripti başlatılıyor -> %1").arg(nativeScriptPath));
        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = (L"cmd.exe /c \"" + nativeScriptPath.toStdWString() + L"\"");
        BOOL launched = CreateProcessW(
            NULL,
            &cmd[0],
            NULL,
            NULL,
            FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            NULL,
            NULL,
            &si,
            &pi
        );
        if (launched) {
            DWORD childPid = pi.dwProcessId;
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            Logger::info(QObject::tr("OtaUpdater: Güncelleme süreci bağımsız süreç olarak başlatıldı (PID: %1).").arg(childPid));
        } else {
            Logger::warning(QObject::tr("OtaUpdater: CreateProcessW başarısız (Hata: %1), QProcess::startDetached ile deneniyor.").arg(GetLastError()));
            QProcess::startDetached("cmd.exe", QStringList() << "/c" << nativeScriptPath);
        }
    } else {
        Logger::error(QObject::tr("OtaUpdater: Kurulum betiği yazılamadı -> %1").arg(scriptPath));
    }
#else
    // Linux OTA Kurulumu (dpkg / systemd-run)
    QString scriptPath = QDir::tempPath() + "/okidia_ota_install.sh";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly)) {
        QString bash = "#!/bin/bash\n"
                       "export DEBIAN_FRONTEND=noninteractive\n"
                       "sleep 2\n"
                       "for i in 1 2 3 4 5; do dpkg -i \"" + pkgPath + "\" && break; apt-get install -f -y; dpkg -i \"" + pkgPath + "\" && break; sleep 3; done\n"
                       "systemctl restart okidia-agent.service\n"
                       "rm -f \"" + pkgPath + "\"\n"
                       "rm -f \"$0\"\n";
        script.write(bash.toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
        
        Logger::info(QObject::tr("OtaUpdater: Script çalıştırılıyor (systemd-run ile) -> %1").arg(scriptPath));
        QProcess::startDetached("systemd-run", QStringList() << "--description=OKIDIA OTA Installer" << "--collect" << "/bin/bash" << scriptPath);
    } else {
        Logger::error(QObject::tr("OtaUpdater: Kurulum betiği yazılamadı -> %1").arg(scriptPath));
    }
#endif
}

QString OtaUpdater::cachedPackagePath() {
    const QString cacheDir = OkidiaConfig::rollbackCacheDir();
#if defined(Q_OS_WIN)
    return cacheDir + "/okidia-agent-stable.exe";
#else
    return cacheDir + "/okidia-agent-stable.deb";
#endif
}

bool OtaUpdater::executeRollback(QString* errorMsg) {
    const QString pkgPath = cachedPackagePath();
    if (!QFile::exists(pkgPath)) {
        const QString err = QObject::tr("Yerel kararlı sürüm paketi bulunamadı: %1").arg(pkgPath);
        Logger::error(QObject::tr("OtaUpdater: Geri alma başarısız - ") + err);
        if (errorMsg) *errorMsg = err;
        return false;
    }

    Logger::info(QObject::tr("OtaUpdater: Kararlı sürüme geri alma (Rollback) başlatılıyor: %1").arg(pkgPath));

#if defined(Q_OS_WIN)
    QString nativePkgPath = QDir::toNativeSeparators(pkgPath);
    QString scriptPath = QDir::tempPath() + "/okidia_rollback_install.bat";
    QString nativeScriptPath = QDir::toNativeSeparators(scriptPath);
    QString otaLogPath = QDir::toNativeSeparators(QDir::tempPath() + "/okidia_agent_rollback.log");
    QString innoLogPath = QDir::toNativeSeparators(QDir::tempPath() + "/okidia_agent_rollback_inno.log");

    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString bat = "@echo off\r\n"
                      "setlocal enabledelayedexpansion\r\n"
                      "echo [%date% %time%] Rollback script baslatildi. >> \"" + otaLogPath + "\"\r\n"
                      "echo [%date% %time%] Paket: \"" + nativePkgPath + "\" >> \"" + otaLogPath + "\"\r\n"
                      "net stop OkidiaAgent /y >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "sc stop OkidiaAgent >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "ping 127.0.0.1 -n 3 >nul\r\n"
                      "taskkill /F /IM okidia-agent.exe /T >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "taskkill /F /IM okidia-tray.exe /T >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "ping 127.0.0.1 -n 3 >nul\r\n"
                      "start \"\" /wait \"" + nativePkgPath + "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=\"" + innoLogPath + "\"\r\n"
                      "sc start OkidiaAgent >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "net start OkidiaAgent >> \"" + otaLogPath + "\" 2>&1\r\n"
                      "del /F /Q \"%~f0\"\r\n";
        script.write(bat.toUtf8());
        script.close();

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = (L"cmd.exe /c \"" + nativeScriptPath.toStdWString() + L"\"");
        BOOL launched = CreateProcessW(
            NULL, &cmd[0], NULL, NULL, FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            NULL, NULL, &si, &pi
        );
        if (launched) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            QProcess::startDetached("cmd.exe", QStringList() << "/c" << nativeScriptPath);
        }
        return true;
    } else {
        if (errorMsg) *errorMsg = QObject::tr("Geri alma betiği oluşturulamadı.");
        return false;
    }
#else
    QString scriptPath = QDir::tempPath() + "/okidia_rollback_install.sh";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly)) {
        QString bash = "#!/bin/bash\n"
                       "export DEBIAN_FRONTEND=noninteractive\n"
                       "sleep 2\n"
                       "for i in 1 2 3 4 5; do dpkg -i \"" + pkgPath + "\" && break; apt-get install -f -y; dpkg -i \"" + pkgPath + "\" && break; sleep 3; done\n"
                       "systemctl restart okidia-agent.service\n"
                       "rm -f \"$0\"\n";
        script.write(bash.toUtf8());
        script.close();
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);

        Logger::info(QObject::tr("OtaUpdater: Geri alma betiği systemd-run ile başlatılıyor -> %1").arg(scriptPath));
        return QProcess::startDetached("systemd-run", QStringList() << "--description=OKIDIA Rollback Installer" << "--collect" << "/bin/bash" << scriptPath);
    } else {
        if (errorMsg) *errorMsg = QObject::tr("Geri alma betiği oluşturulamadı.");
        return false;
    }
#endif
}

