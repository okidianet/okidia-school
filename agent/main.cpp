// agent/main.cpp — okidia-agent servisi (QCoreApplication, root)
// Broker bağlantısı, komut yönlendirme, güç/web/ses-vol komutları.
// UI gerektiren komutlar LocalServer üzerinden aktif tray'e iletilir.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QLocale>
#include <iostream>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

// MinGW GCC / Qt6Core TLS null-pointer destructors exception filter:
// Qt6Core'da iş parçacığı sonlanırken (LdrShutdownThread) QSystemLocale TLS yıkıcısı,
// LocalSystem veya arka plan servislerinde rbx=NULL iken [rbx+0x50] okumaya çalışarak
// veya serbest bırakılmış emutls hafızasına [rsi]=0 yazarak c0000005 (Access Violation)
// oluşturabilir. Bu VEH filtresi bu spesifik durumu yakalar, doğrudan epilogue'a
// (add rsp, 28h; pop rbx; pop rsi; ret) atlatır ve servisin çökmesini engelleyerek
// kesintisiz devam etmesini sağlar.
static LONG WINAPI QtTlsCrashFilter(PEXCEPTION_POINTERS pEx) {
    if (pEx && pEx->ExceptionRecord && pEx->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        PCONTEXT ctx = pEx->ContextRecord;
        if (ctx) {
            // 1. Qt6Core!__dyn_tls_dtor stack-unwind koruması:
            // MinGW libwinpthread (std::thread) kapanırken emutls_destroy, Qt6Core!__dyn_tls_dtor'dan
            // önce çalışıp thread_local (QCollator/QSystemLocale) belleklerini serbest bıraktığında
            // oluşan Use-After-Free çökmesini çağrı derinliğinden bağımsız olarak yakalar ve
            // doğrudan __dyn_tls_dtor epilogue'una (add rsp, 28h; pop rbx; pop rsi; ret) sarar.
            if (ctx->Rsp > 0x10000) {
                const ULONG64* sp = reinterpret_cast<const ULONG64*>(ctx->Rsp);
                for (int i = 0; i < 48; ++i) {
                    if (IsBadReadPtr(&sp[i], sizeof(ULONG64))) break;
                    ULONG64 retAddr = sp[i];
                    if (retAddr > 0x100000 && !IsBadReadPtr(reinterpret_cast<const void*>(retAddr - 0xAD), 0xC0)) {
                        const unsigned char* base = reinterpret_cast<const unsigned char*>(retAddr - 0xAD);
                        if (base[0] == 0x56 && base[1] == 0x53 && base[2] == 0x48 &&
                            base[3] == 0x83 && base[4] == 0xEC && base[5] == 0x28 &&
                            base[0x17] == 0x48 && base[0x18] == 0x83 && base[0x19] == 0xC4 &&
                            base[0x1A] == 0x28 && base[0x1B] == 0x5B && base[0x1C] == 0x5E &&
                            base[0x1D] == 0xC3 && base[0xAB] == 0xFF && base[0xAC] == 0x13) {
                            ctx->Rsp = reinterpret_cast<ULONG64>(&sp[i + 1]);
                            ctx->Rip = reinterpret_cast<ULONG64>(base + 0x17);
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }
            }

            const unsigned char* rip = reinterpret_cast<const unsigned char*>(ctx->Rip);
            if (rip && !IsBadReadPtr(rip, 8)) {
                // 2. mov rax, [rbx+50h] (48 8B 43 50) -> rbx=NULL, doğrudan epilogue'a atla (+0x1E)
                if (rip[0] == 0x48 && rip[1] == 0x8B && rip[2] == 0x43 && rip[3] == 0x50) {
                    ctx->Rip += 0x1E;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // 3. mov qword ptr [rsi], 0 (48 C7 06 00 00 00 00) -> rsi emutls serbest kalmış, epilogue'a atla (+0x07)
                if (rip[0] == 0x48 && rip[1] == 0xC7 && rip[2] == 0x06 && rip[3] == 0x00) {
                    ctx->Rip += 0x07;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#include "core/AgentApp.h"
#include "system/SystemInfo.h"
#include "../common/utils/PlatformService.h"
#include "../common/utils/Logger.h"
#include "../common/OkidiaConfig.h"
#include "../common/version/OkidiaVersion.h"

static int runAgentApp(int argc, char* argv[]) {
#if defined(Q_OS_WIN)
    // Windows Servisi (Session 0) için güvenli yerel:
    // Not: okidia-agent bir QCoreApplication olduğu için QPA platform eklentisi kullanmaz;
    // alt süreçlere (okidia-tray.exe) sızmaması için QT_QPA_PLATFORM=offscreen tanımlanmamalıdır.
    qunsetenv("QT_QPA_PLATFORM");
    QLocale::setDefault(QLocale::c());
#endif
    QCoreApplication app(argc, argv);
    app.setOrganizationName("okidia");
    app.setApplicationName("agent");
    app.setApplicationVersion(Okidia::Version::string());

    QCommandLineParser parser;
    parser.setApplicationDescription("OKIDIA Agent Servisi");
    parser.addHelpOption();
    parser.addVersionOption();
    
    QCommandLineOption daemonOption("daemon", QObject::tr("İstemciyi arka planda (servis olarak) başlatır."));
    parser.addOption(daemonOption);

    parser.process(app);

    if (!parser.isSet(daemonOption)) {
        std::cerr << QObject::tr("Hata: okidia-agent doğrudan çalıştırılamaz.\n").toUtf8().constData();
        std::cerr << QObject::tr("Servis modunda başlatmak için '--daemon' parametresi gereklidir.\n\n").toUtf8().constData();
        std::cerr << QObject::tr("Kullanım:\n").toUtf8().constData();
        std::cerr << QObject::tr("  okidia-agent --daemon   # Servisi başlatır\n").toUtf8().constData();
        std::cerr << QObject::tr("  okidia-agent --version  # Sürüm bilgisini gösterir\n").toUtf8().constData();
        return 1;
    }

    Logger::init(OkidiaConfig::logDir() + "/okidia-agent.log");
    PlatformService::setupSignalHandlers();
    PlatformService::notifyReady();

    Logger::info(QObject::tr("OKIDIA Agent (servis) başlatılıyor..."));
    Logger::info(QObject::tr("Hostname   : ") + SystemInfo::hostname());
    Logger::info(QObject::tr("Machine-ID : ") + SystemInfo::machineId());

    AgentApp agentApp(&app);
    if (!agentApp.init()) {
        Logger::error(QObject::tr("Agent: Uygulama başlatılamadı!"));
        return 1;
    }

    return app.exec();
}

int main(int argc, char* argv[]) {
#if defined(Q_OS_WIN)
    AddVectoredExceptionHandler(1, QtTlsCrashFilter);

    // Windows Servisi (Session 0) ve arka plan servisi ortam güvenliği:
    // LocalSystem hesabında eksik olan HKCU\Control Panel\International nedeniyle
    // QSystemLocale çökmesini engellemek için güvenli varsayılan yerel (locale) ata.
    // Alt süreçlere (okidia-tray.exe) sızmaması için QT_QPA_PLATFORM ayarlanmaz.
    qunsetenv("QT_QPA_PLATFORM");
    QLocale::setDefault(QLocale::c());

    if (PlatformService::isRunningAsService()) {
        return PlatformService::runAsService(QStringLiteral("OkidiaAgent"), runAgentApp, argc, argv) ? 0 : 1;
    }
#endif
    return runAgentApp(argc, argv);
}
