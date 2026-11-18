// common/utils/PlatformService.cpp
#include "PlatformService.h"
#include "Logger.h"
#include <QCoreApplication>
#include <QObject>
#include <QProcess>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QStandardPaths>
#include "../ui/ThemeManager.h"

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <wtsapi32.h>
#  include <userenv.h>
#  include <tlhelp32.h>
#  include <QDir>
#  include <QFile>
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif
#else
#  include <csignal>
#  ifdef HAVE_SYSTEMD
#    include <systemd/sd-daemon.h>
#  endif
#endif

#if defined(Q_OS_WIN)
#  include <atomic>
#  include <process.h>
#  include <QLocale>
static SERVICE_STATUS          s_serviceStatus = {};
static SERVICE_STATUS_HANDLE   s_serviceStatusHandle = nullptr;
static std::function<int(int, char**)> s_runFunc;
static int                     s_argc = 0;
static char**                  s_argv = nullptr;
static QString                 s_serviceName = QStringLiteral("OkidiaBroker");
static std::atomic<bool>       s_isSessionLocked{false};
static std::function<void()>   s_sessionChangeCallback = nullptr;

static DWORD WINAPI ServiceCtrlHandlerEx(DWORD request, DWORD eventType, LPVOID /*eventData*/, LPVOID /*context*/) {
    switch (request) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        Logger::info(QObject::tr("Windows Servisi durdurma/kapatma sinyali alındı: %1").arg(s_serviceName));
        s_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        s_serviceStatus.dwCheckPoint = 1;
        s_serviceStatus.dwWaitHint = 10000;
        SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);

        QCoreApplication::quit();
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventType == WTS_SESSION_LOCK) {
            s_isSessionLocked = true;
            Logger::info(QObject::tr("PlatformService: Oturum kilitlendi (WTS_SESSION_LOCK)"));
        } else if (eventType == WTS_SESSION_UNLOCK) {
            s_isSessionLocked = false;
            Logger::info(QObject::tr("PlatformService: Oturum kilidi açıldı (WTS_SESSION_UNLOCK)"));
        } else if (eventType == WTS_SESSION_LOGON) {
            s_isSessionLocked = false;
            Logger::info(QObject::tr("PlatformService: Kullanıcı oturum açtı (WTS_SESSION_LOGON)"));
        } else if (eventType == WTS_SESSION_LOGOFF) {
            s_isSessionLocked = true;
            Logger::info(QObject::tr("PlatformService: Kullanıcı oturumu kapattı (WTS_SESSION_LOGOFF)"));
        }
        if (s_sessionChangeCallback) {
            s_sessionChangeCallback();
        }
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMainEntry(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // Windows Servisi (Session 0) ve LocalSystem ortam güvenliği:
    // Alt süreçlere (okidia-tray.exe) sızmaması için QT_QPA_PLATFORM=offscreen tanımlanmaz;
    // güvenli yerel belirleyerek TLS/Locale çökmesini önle.
    qunsetenv("QT_QPA_PLATFORM");
    QLocale::setDefault(QLocale::c());

    std::wstring wName = s_serviceName.toStdWString();
    s_serviceStatusHandle = RegisterServiceCtrlHandlerExW(wName.c_str(), ServiceCtrlHandlerEx, nullptr);
    if (!s_serviceStatusHandle) {
        return;
    }

    s_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    s_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    s_serviceStatus.dwWin32ExitCode = 0;
    s_serviceStatus.dwServiceSpecificExitCode = 0;
    s_serviceStatus.dwCheckPoint = 0;
    s_serviceStatus.dwWaitHint = 10000;

    SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);

    // Servis çalışıyor durumuna geç
    s_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    s_serviceStatus.dwCheckPoint = 0;
    s_serviceStatus.dwWaitHint = 0;
    SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);

    // Qt uygulamasını (QCoreApplication) doğrudan bu servis iş parçacığında çalıştır.
    // QCoreApplication oluşturulmadan önce bu thread üzerinde herhangi bir Qt nesnesi
    // veya Logger çağrılmamalıdır; böylece MinGW/Qt TLS tablosu yetim (orphaned) kalmaz.
    if (s_runFunc) {
        s_runFunc(s_argc, s_argv);
    }

    s_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    s_serviceStatus.dwCheckPoint = 0;
    s_serviceStatus.dwWaitHint = 0;
    SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        Logger::info(QObject::tr("Konsol kapatma sinyali alındı, uygulama sonlandırılıyor..."));
        QCoreApplication::quit();
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void PlatformService::notifyReady() {
#if defined(Q_OS_WIN)
    Logger::info(QObject::tr("PlatformService: Windows servisi/süreci hazır"));
#else
#  ifdef HAVE_SYSTEMD
    sd_notify(0, "READY=1");
#  endif
#endif
}

void PlatformService::notifyStatus(const QString& status) {
#if defined(Q_OS_WIN)
    Q_UNUSED(status);
#else
#  ifdef HAVE_SYSTEMD
    std::string s = "STATUS=" + status.toStdString();
    sd_notify(0, s.c_str());
#  else
    Q_UNUSED(status);
#  endif
#endif
}

void PlatformService::setupSignalHandlers() {
#if defined(Q_OS_WIN)
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    std::signal(SIGTERM, [](int) {
        Logger::info(QObject::tr("PlatformService: SIGTERM alındı, kapatılıyor..."));
        QCoreApplication::quit();
    });
    std::signal(SIGINT, [](int) {
        Logger::info(QObject::tr("PlatformService: SIGINT alındı, kapatılıyor..."));
        QCoreApplication::quit();
    });
    std::signal(SIGHUP, [](int) {
        Logger::info(QObject::tr("PlatformService: SIGHUP alındı"));
    });
#endif
}

bool PlatformService::setSystemTimeUtc(qint64 epochSeconds) {
    if (epochSeconds <= 0) return false;

#if defined(Q_OS_WIN)
    // Windows epoch: 1 Ocak 1601
    // Unix epoch to Windows FILETIME conversion (100-nanosecond intervals)
    ULARGE_INTEGER ul;
    ul.QuadPart = (static_cast<ULONGLONG>(epochSeconds) * 10000000ULL) + 116444736000000000ULL;

    FILETIME ft;
    ft.dwLowDateTime = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;

    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) {
        Logger::error(QObject::tr("PlatformService: FileTimeToSystemTime başarısız!"));
        return false;
    }

    if (!SetSystemTime(&st)) {
        Logger::error(QObject::tr("PlatformService: Windows sistem saati ayarlanamadı (Hata: %1)").arg(GetLastError()));
        return false;
    }
    Logger::info(QObject::tr("PlatformService: Windows sistem saati başarıyla güncellendi (UTC: %1)").arg(epochSeconds));
    return true;
#else
    struct timespec ts = {};
    ts.tv_sec = static_cast<time_t>(epochSeconds);
    ts.tv_nsec = 0;

    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        Logger::error(QObject::tr("PlatformService: Linux sistem saati ayarlanamadı: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    Logger::info(QObject::tr("PlatformService: Linux sistem saati başarıyla güncellendi (UTC: %1)").arg(epochSeconds));
    return true;
#endif
}

bool PlatformService::executeElevated(const QString& program, const QStringList& args, QWidget* parentWidget) {
#if defined(Q_OS_WIN)
    QString paramStr;
    for (const QString& arg : args) {
        if (!paramStr.isEmpty()) paramStr += " ";
        if (arg.contains(" ")) {
            paramStr += "\"" + arg + "\"";
        } else {
            paramStr += arg;
        }
    }

    std::wstring wProg = program.toStdWString();
    std::wstring wParams = paramStr.toStdWString();

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = wProg.c_str();
    sei.lpParameters = wParams.empty() ? nullptr : wParams.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            CloseHandle(sei.hProcess);
        }
        Logger::info(QObject::tr("PlatformService: Windows runas ile işlem başarıyla başlatıldı: %1").arg(program));
        return true;
    } else {
        DWORD err = GetLastError();
        Logger::error(QObject::tr("PlatformService: Windows runas başarısız: %1 (Hata: %2)").arg(program).arg(err));
        if (parentWidget && err != ERROR_CANCELLED) {
            QMessageBox::critical(parentWidget, QObject::tr("Yetki Hatası"),
                                  QObject::tr("Yönetici yetkilendirmesi başarısız oldu veya reddedildi."));
        }
        return false;
    }
#else
    // Linux: Tutarlı, modern ve tek tip parola diyaloğu ile yetkilendirme
    QDialog dlg(parentWidget);
    dlg.setWindowTitle(QObject::tr("Sistem Yönetici Yetkisi"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(400);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(14);
    lay->setContentsMargins(24, 20, 24, 20);

    auto* titleLbl = new QLabel(QObject::tr("<b>Yönetici Yetkilendirmesi Gerekli</b>"), &dlg);
    titleLbl->setStyleSheet("font-size: 15px; font-weight: bold;");
    lay->addWidget(titleLbl);

    auto* descLbl = new QLabel(QObject::tr("Bu işlemi gerçekleştirmek için lütfen sistem (sudo/root) parolanızı girin:"), &dlg);
    descLbl->setWordWrap(true);
    descLbl->setStyleSheet("color: #7f8c8d; font-size: 13px;");
    lay->addWidget(descLbl);

    auto* pwdEdit = new QLineEdit(&dlg);
    pwdEdit->setEchoMode(QLineEdit::Password);
    pwdEdit->setPlaceholderText(QObject::tr("Sistem parolası"));
    pwdEdit->setMinimumHeight(36);
    lay->addWidget(pwdEdit);

    auto* errorLbl = new QLabel(&dlg);
    errorLbl->setProperty("status", "error");
    errorLbl->setStyleSheet("color: #e74c3c; font-size: 12px;");
    errorLbl->setVisible(false);
    lay->addWidget(errorLbl);

    auto* btnLay = new QHBoxLayout();
    btnLay->addStretch();
    auto* cancelBtn = new QPushButton(QObject::tr("İptal"), &dlg);
    cancelBtn->setMinimumHeight(34);
    cancelBtn->setMinimumWidth(80);
    cancelBtn->setAutoDefault(false);
    cancelBtn->setDefault(false);

    auto* okBtn = new QPushButton(QObject::tr("Onayla"), &dlg);
    okBtn->setProperty("class", "primary");
    okBtn->setMinimumHeight(34);
    okBtn->setMinimumWidth(90);
    okBtn->setAutoDefault(true);
    okBtn->setDefault(true);

    btnLay->addWidget(cancelBtn);
    btnLay->addWidget(okBtn);
    lay->addLayout(btnLay);

    QObject::connect(pwdEdit, &QLineEdit::returnPressed, okBtn, &QPushButton::animateClick);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(okBtn, &QPushButton::clicked, [&]() {
        if (pwdEdit->text().isEmpty()) {
            errorLbl->setText(QObject::tr("Parola boş bırakılamaz."));
            errorLbl->setVisible(true);
            return;
        }

        // Parolayı sudo -S -v ile doğrula
        QProcess testProc;
        testProc.start("sudo", QStringList() << "-S" << "-v");
        if (testProc.waitForStarted(1500)) {
            testProc.write(pwdEdit->text().toUtf8() + "\n");
            testProc.closeWriteChannel();
            testProc.waitForFinished(5000);
            if (testProc.exitCode() == 0) {
                dlg.accept();
                return;
            }
        }
        errorLbl->setText(QObject::tr("Hatalı parola! Lütfen tekrar deneyin."));
        errorLbl->setVisible(true);
        pwdEdit->selectAll();
        pwdEdit->setFocus();
    });

    if (dlg.exec() != QDialog::Accepted) {
        Logger::warning(QObject::tr("PlatformService: Yönetici yetkilendirmesi kullanıcı tarafından iptal edildi."));
        return false;
    }

    QString password = pwdEdit->text();

    // Doğrulanmış parola ile komutu çalıştır ve tamamlanmasını bekle
    QStringList sudoArgs;
    sudoArgs << "-S" << program << args;

    QProcess execProc;
    execProc.start("sudo", sudoArgs);
    if (execProc.waitForStarted(3000)) {
        execProc.write(password.toUtf8() + "\n");
        execProc.closeWriteChannel();
        // Paket yüklemeleri (dpkg/apt-get) biraz vakit alabileceği için 120 saniye beklenir
        execProc.waitForFinished(120000);
        if (execProc.exitCode() == 0) {
            Logger::info(QObject::tr("PlatformService: sudo -S ile yetkili işlem başarıyla tamamlandı: %1").arg(program));
            return true;
        } else {
            QString err = QString::fromLocal8Bit(execProc.readAllStandardError()).trimmed();
            if (err.isEmpty()) {
                err = QString::fromLocal8Bit(execProc.readAllStandardOutput()).trimmed();
            }
            Logger::error(QObject::tr("PlatformService: sudo -S işlemi hata ile sonuçlandı (%1): %2").arg(execProc.exitCode()).arg(err));
            QMessageBox::critical(parentWidget, QObject::tr("Hata"),
                                  QObject::tr("İşlem gerçekleştirilemedi: %1").arg(err));
            return false;
        }
    } else {
        Logger::error(QObject::tr("PlatformService: sudo başlatılamadı!"));
        QMessageBox::critical(parentWidget, QObject::tr("Yetki Hatası"),
                              QObject::tr("Yetkili işlem başlatılamadı."));
        return false;
    }
#endif
}

#if defined(Q_OS_WIN)
bool PlatformService::isRunningAsService() {
    HANDLE hProcess = GetCurrentProcess();
    DWORD sessionId = 0;
    if (ProcessIdToSessionId(GetProcessId(hProcess), &sessionId)) {
        // Windows'ta Session 0 servisler içindir
        return (sessionId == 0);
    }
    return false;
}

bool PlatformService::runAsService(const QString& serviceName, std::function<int(int, char**)> runFunc, int argc, char** argv) {
    s_serviceName = serviceName;
    s_runFunc = runFunc;
    s_argc = argc;
    s_argv = argv;

    std::wstring wName = s_serviceName.toStdWString();
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(wName.c_str()), ServiceMainEntry },
        { nullptr, nullptr }
    };

    if (StartServiceCtrlDispatcherW(serviceTable)) {
        return true;
    }

    DWORD err = GetLastError();
    // Eğer SCM'ye bağlanılamadıysa (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT == 1063):
    // Bu süreç bir Windows servisi olarak başlatılmamıştır (örneğin Session 0 SSH oturumu, konsol, test aracı vb.).
    // Süreci başarısız sonlandırmak yerine doğrudan konsol modunda çalıştır.
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        if (s_runFunc) {
            return (s_runFunc(s_argc, s_argv) == 0);
        }
    }

    return false;
}

void PlatformService::stopService() {
    if (s_serviceStatusHandle) {
        s_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(s_serviceStatusHandle, &s_serviceStatus);
    }
}

bool PlatformService::isSessionLocked() {
    return s_isSessionLocked.load();
}

void PlatformService::setSessionLocked(bool locked) {
    s_isSessionLocked.store(locked);
}

void PlatformService::setSessionChangeCallback(std::function<void()> cb) {
    s_sessionChangeCallback = std::move(cb);
}

#include <vector>

bool PlatformService::ensureTrayRunning(const QString& trayExeName, bool isWinlogon) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) return false;

    QString targetExe = trayExeName;
    if (targetExe.isEmpty()) {
        const QString appName = QCoreApplication::applicationName().toLower();
        if (appName.contains("broker")) {
            targetExe = "okidia-broker-tray.exe";
        } else {
            targetExe = "okidia-tray.exe";
        }
    }

    // Hedef oturumda bu tray zaten çalışıyor mu kontrol et
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnap, &pe)) {
            std::wstring targetExeW = targetExe.toStdWString();
            do {
                if (_wcsicmp(pe.szExeFile, targetExeW.c_str()) == 0) {
                    DWORD procSess = 0;
                    if (ProcessIdToSessionId(pe.th32ProcessID, &procSess) && procSess == sessionId) {
                        // Sürecin SYSTEM mi yoksa normal kullanıcı mı olduğunu denetle
                        bool isSystemProc = false;
                        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                        if (hProc) {
                            HANDLE hTok = NULL;
                            if (OpenProcessToken(hProc, TOKEN_QUERY, &hTok)) {
                                DWORD len = 0;
                                GetTokenInformation(hTok, TokenUser, NULL, 0, &len);
                                if (len > 0) {
                                    std::vector<BYTE> buf(len);
                                    if (GetTokenInformation(hTok, TokenUser, buf.data(), len, &len)) {
                                        TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
                                        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
                                        PSID systemSid = NULL;
                                        if (AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &systemSid)) {
                                            isSystemProc = EqualSid(tu->User.Sid, systemSid);
                                            FreeSid(systemSid);
                                        }
                                    }
                                }
                                CloseHandle(hTok);
                            }
                            CloseHandle(hProc);
                        }

                        // Eğer istenen tip ile çalışan süreç tipi eşleşiyorsa (Winlogon<->SYSTEM veya User<->non-SYSTEM)
                        if ((isWinlogon && isSystemProc) || (!isWinlogon && !isSystemProc)) {
                            CloseHandle(hSnap);
                            return true; // Zaten bu oturumda doğru tipte çalışıyor
                        }
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    HANDLE hTargetToken = nullptr;
    if (!isWinlogon) {
        HANDLE hUserToken = nullptr;
        if (WTSQueryUserToken(sessionId, &hUserToken)) {
            if (!DuplicateTokenEx(hUserToken, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &hTargetToken)) {
                hTargetToken = hUserToken;
            } else {
                CloseHandle(hUserToken);
            }
        }
    }

    // Yalnızca Winlogon oturumu için servis (SYSTEM) token'ını SessionId'ye ata.
    // Normal kullanıcı masaüstü oturumu için (isWinlogon == false) asla SYSTEM token'ı ile tray başlatılmaz.
    if (!hTargetToken && isWinlogon) {
        HANDLE hServiceToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &hServiceToken)) {
            if (DuplicateTokenEx(hServiceToken, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &hTargetToken)) {
                SetTokenInformation(hTargetToken, TokenSessionId, &sessionId, sizeof(DWORD));
            }
            CloseHandle(hServiceToken);
        }
    }

    if (!hTargetToken) return false;

    QString trayPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/" + targetExe);
    if (!QFile::exists(trayPath)) {
        if (targetExe.contains("broker")) {
            trayPath = "C:\\Program Files\\Okidia\\Broker\\bin\\okidia-broker-tray.exe";
        } else {
            trayPath = "C:\\Program Files\\Okidia\\Agent\\bin\\okidia-tray.exe";
        }
    }

    if (!QFile::exists(trayPath)) {
        Logger::warning(QObject::tr("PlatformService: %1 dosyası bulunamadı: %2").arg(targetExe, trayPath));
        CloseHandle(hTargetToken);
        return false;
    }

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.lpDesktop = isWinlogon ? const_cast<LPWSTR>(L"winsta0\\Winlogon") : const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi = {};

    LPVOID pEnv = nullptr;
    // Session 0 (SYSTEM) ortam değişkenlerinin kullanıcı oturumuna sızmasını engellemek için bInherit = FALSE
    CreateEnvironmentBlock(&pEnv, hTargetToken, FALSE);

    QString cmdStr = "\"" + QDir::toNativeSeparators(trayPath) + "\"";
    if (isWinlogon && targetExe.contains("agent", Qt::CaseInsensitive)) {
        cmdStr += " --username winlogon";
    }

    std::wstring cmd = cmdStr.toStdWString();
    std::wstring workDir = QDir::toNativeSeparators(QFileInfo(trayPath).absolutePath()).toStdWString();
    BOOL success = CreateProcessAsUserW(
        hTargetToken,
        nullptr,
        const_cast<LPWSTR>(cmd.c_str()),
        nullptr,
        nullptr,
        FALSE,
        NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
        pEnv,
        const_cast<LPWSTR>(workDir.c_str()),
        &si,
        &pi
    );

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Logger::info(QObject::tr("PlatformService: %1 oturumda başlatıldı (Session: %2, Winlogon: %3)").arg(targetExe).arg(sessionId).arg(isWinlogon ? "Evet" : "Hayır"));
    } else {
        Logger::error(QObject::tr("PlatformService: CreateProcessAsUserW başarısız (%1) - Hata: %2").arg(targetExe).arg(GetLastError()));
    }
    CloseHandle(hTargetToken);
    return success == TRUE;
}
#endif
