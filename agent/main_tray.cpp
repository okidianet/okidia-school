// agent/main_tray.cpp — okidia-tray (QApplication, kullanıcı oturumu)
//
// Normal kullanım: XDG autostart ile her kullanıcı girişinde başlar.
// Login ekranı için: agent servisi "--username lightdm" argümanıyla başlatır.
//
// --username <ad>  argümanı verilirse, o isim CLIENT_INFO username alanına
// yazılır. Agent LocalServer'ın bağlantı–kullanıcı eşlemesi bu isme bakarak
// aktif tray routing yapar.

#include <QApplication>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QImage>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QPointer>
#include <QDateTime>
#include <QLocalServer>
#include <QLocalSocket>
#include "auth/OfflineAuthManager.h"
#include "ui/SmartboardLockOverlay/SmartboardLockOverlay.h"

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <QLocale>
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif

// MinGW GCC / Qt6Core TLS null-pointer destructors exception filter:
// Qt6Core'da iş parçacığı sonlanırken (LdrShutdownThread) QSystemLocale TLS yıkıcısı,
// LocalSystem veya arka plan servislerinde rbx=NULL iken [rbx+0x50] okumaya çalışarak
// veya serbest bırakılmış emutls hafızasına [rsi]=0 yazarak c0000005 (Access Violation)
// oluşturabilir. Bu VEH filtresi bu spesifik durumu yakalar, doğrudan epilogue'a
// (add rsp, 28h; pop rbx; pop rsi; ret) atlatır ve sürecin çökmesini engeller.
static LONG WINAPI QtTlsCrashFilter(PEXCEPTION_POINTERS pEx) {
    if (pEx && pEx->ExceptionRecord && pEx->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        PCONTEXT ctx = pEx->ContextRecord;
        if (ctx) {
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
                // 1. mov rax, [rbx+50h] (48 8B 43 50) -> rbx=NULL, doğrudan epilogue'a atla (+0x1E)
                if (rip[0] == 0x48 && rip[1] == 0x8B && rip[2] == 0x43 && rip[3] == 0x50) {
                    ctx->Rip += 0x1E;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                // 2. mov qword ptr [rsi], 0 (48 C7 06 00 00 00 00) -> rsi emutls serbest kalmış, epilogue'a atla (+0x07)
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

#include "capture/ScreenCapture.h"
#include "commands/CommandHandler.h"
#include "chat/ChatHandler.h"
#include "exam/ExamHandler.h"
#include "audio/AudioPlayer.h"
#include "transfer/FileReceiver.h"
#include "broadcast/ScreenBroadcastReceiver.h"
#include "ui/AgentTray/AgentTray.h"
#include "system/SystemInfo.h"
#include "../common/utils/Logger.h"
#include "../common/utils/OkidiaUtils.h"
#include "../common/network/LocalConnection.h"
#include "../common/OkidiaConfig.h"
#include "../common/ui/ThemeManager.h"

// ── Yardımcı: Tam ekran karesi gönder (Gizlilik maskesi dahil) ───────────
static void sendFullFrame(LocalConnection* localConn, ScreenCapture* capture, uint32_t type, CommandHandler* cmdHandler) {
    if (!localConn || !localConn->isConnected()) return;

    // Backpressure guard: Sokette aşırı veri birikmişse ekranı boş yere yakalama
    if (localConn->bytesToWrite() > 256 * 1024) {
        return;
    }

    if (type == 2 && cmdHandler && !cmdHandler->isScreenStreamActive()) {
        // İdari PC: Ekran görüntüsü gönderme (Ağ tasarrufu)
        return;
    }

    QByteArray rf = capture->captureFullFrame();
    if (rf.isEmpty()) return;

    okidia::ScreenFrame frame;
    frame.set_width(static_cast<uint32_t>(capture->screenWidth()));
    frame.set_height(static_cast<uint32_t>(capture->screenHeight()));
    frame.set_jpeg_data(rf.constData(), rf.size());  // B2: zero-copy
    frame.set_is_full_frame(true);
    
    okidia::Envelope fEnv;
    fEnv.set_type(okidia::MessageType::SCREEN_FRAME);
    fEnv.set_payload(frame.SerializeAsString());
    localConn->sendEnvelope(fEnv);
    Logger::info(QObject::tr("Tray: Tam ekran karesi gönderildi"));
}

static std::string buildScreenResolutionsString(ScreenCapture* capture) {
    const auto monitors = ScreenCapture::availableMonitors();
    QStringList scrList;
    for (const auto& m : monitors) {
        if (m.width > 0 && m.height > 0) {
            scrList << QString("%1x%2").arg(m.width).arg(m.height);
        }
    }
    if (scrList.isEmpty() && capture) {
        scrList << QString("%1x%2").arg(capture->screenWidth()).arg(capture->screenHeight());
    }
    return scrList.join(" / ").toStdString();
}

int main(int argc, char* argv[]) {
    // ── --username ve --ipc-token argümanlarını QApplication'dan ÖNCE ayrıştır ──
    QString forcedUsername;
    QString cliIpcToken;
    for (int i = 1; i < argc - 1; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--username") {
            forcedUsername = QString::fromLocal8Bit(argv[i + 1]);
        } else if (QString::fromLocal8Bit(argv[i]) == "--ipc-token") {
            cliIpcToken = QString::fromLocal8Bit(argv[i + 1]);
        }
    }

    QString resolvedUser = QString::fromLocal8Bit(qgetenv("USER"));
    if (resolvedUser.isEmpty()) resolvedUser = QString::fromLocal8Bit(qgetenv("USERNAME"));
    if (resolvedUser.isEmpty()) resolvedUser = QString::fromLocal8Bit(qgetenv("LOGNAME"));
    const QString trayUser = forcedUsername.isEmpty() ? resolvedUser : forcedUsername;

#if defined(Q_OS_WIN)
    AddVectoredExceptionHandler(1, QtTlsCrashFilter);
    QLocale::setDefault(QLocale::c());

    // Üst süreçten veya sistem ortamından sızmış olabilecek QT_QPA_PLATFORM=offscreen
    // değişkenini temizle; okidia-tray her zaman qwindows.dll eklentisi ile çalışmalıdır.
    qunsetenv("QT_QPA_PLATFORM");

    // Tekil örnek (Single-Instance) koruması QApplication oluşturulmadan ÖNCE çalışmalıdır;
    // böylece olası bir başlatma hatasında dahi ekranda birden fazla süreç/pop-up birikemez.
    const QString mutexName = "Global\\OkidiaTrayLock_" + trayUser;
    HANDLE hTrayMutex = CreateMutexW(NULL, TRUE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
    if (hTrayMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hTrayMutex);
        return 0;
    }
#endif
#if !defined(Q_OS_WIN)
    if (qgetenv("DISPLAY").isEmpty() && qgetenv("WAYLAND_DISPLAY").isEmpty()) {
        std::cerr << "Hata: okidia-tray grafiksel bir masaüstü oturumu gerektirir ($DISPLAY veya $WAYLAND_DISPLAY bulunamadı).\n";
        return 1;
    }
#endif
    // Uygulama dizinini açıkça kütüphane/eklenti arama yoluna ekle
    QString appDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (!appDir.isEmpty()) {
        QCoreApplication::addLibraryPath(appDir);
        QCoreApplication::addLibraryPath(appDir + "/platforms");
    }

    QApplication app(argc, argv);
    app.setOrganizationName("okidia");
    app.setApplicationName("tray");
    app.setDesktopFileName("okidia-tray");
    app.setApplicationVersion(Okidia::Version::string());
    app.setQuitOnLastWindowClosed(false);

    // ── Uygulama ve pencere ikonu (görev çubuğu + pencere başlığı) ──────────
    {
        QIcon appIcon = QIcon::fromTheme("okidia-agent");
        appIcon.addFile(":/icons/agent/okidia-agent-connected-16.png", QSize(16, 16));
        appIcon.addFile(":/icons/agent/okidia-agent-connected-22.png", QSize(22, 22));
        appIcon.addFile(":/icons/agent/okidia-agent-connected-32.png", QSize(32, 32));
        app.setWindowIcon(appIcon);
    }

    OkidiaUtils::installTranslator(app);

    ThemeManager::instance().initialize(&app);

    // Sistem temasına uyum sağla
    QPalette sysPal = app.palette();
    int yiq = (sysPal.color(QPalette::Window).red() * 299 + 
               sysPal.color(QPalette::Window).green() * 587 + 
               sysPal.color(QPalette::Window).blue() * 114) / 1000;
    ThemeManager::ThemeMode sysMode = (yiq >= 128) ? ThemeManager::ThemeMode::Light : ThemeManager::ThemeMode::Dark;
    
    int themeToApply = 0;
    const auto themes = ThemeManager::instance().availableThemes();
    for (int i = 0; i < themes.size(); ++i) {
        if (themes[i].mode == sysMode) {
            themeToApply = i;
            break;
        }
    }
    ThemeManager::instance().applyTheme(themeToApply);

    auto getCurrentIpcToken = [cliIpcToken]() -> QString {
        const QString tokenPath = OkidiaConfig::runDir() + "/ipc_token";
        QFile tokenFile(tokenPath);
        if (tokenFile.open(QIODevice::ReadOnly)) {
            QString diskToken = QString::fromUtf8(tokenFile.readAll()).trimmed();
            tokenFile.close();
            if (!diskToken.isEmpty()) {
                return diskToken;
            }
        }
        return cliIpcToken;
    };

    const QString lockName = "okidia-tray-" + trayUser;
    {
        QLocalSocket probe;
        probe.connectToServer(lockName);
        if (probe.waitForConnected(300)) {
            probe.disconnectFromServer();
            Logger::info(QObject::tr("AgentTray: Zaten çalışıyor (") + trayUser + QObject::tr("), ikinci instance kapatılıyor."));
#if defined(Q_OS_WIN)
            if (hTrayMutex) CloseHandle(hTrayMutex);
#endif
            return 0;
        }
    }
    auto* lockServer = new QLocalServer(&app);
    QLocalServer::removeServer(lockName);
    lockServer->listen(lockName);
    QObject::connect(lockServer, &QLocalServer::newConnection, [lockServer]() {
        if (auto* s = lockServer->nextPendingConnection()) s->deleteLater();
    });

    // Efektif kullanıcı adı: argüman varsa onu kullan, yoksa sistemi sorgula
    // Kullanıcı adını başlangıçta BİR KEZ al — sonraki sorgulamalarda değişmez.
    // Switch user sonrası A'nın tray'i SystemInfo::username() ile 'lightdm' görebilir;
    // bu sabit değer sayesinde yanlış kayıt olmaz, routing doğru çalışır.
    // DM tray (--username lightdm): login ekranı için sabit.
    // Normal tray: oturumu açan kullanıcı, değişmez.
    const QString startupUsername = forcedUsername.isEmpty()
        ? SystemInfo::username()
        : forcedUsername;

    Logger::init(OkidiaConfig::logDir() + "/okidia-agent-tray.log");
    Logger::info(QObject::tr("OKIDIA Agent Tray başlatılıyor..."));
    Logger::info(QObject::tr("Kullanıcı: ") + startupUsername);
    if (!forcedUsername.isEmpty())
        Logger::info(QObject::tr("Mod: DM tray (login ekranı)"));

    // ── UI bileşenleri ────────────────────────────────────────────────────
    CaptureSettings captureSettings;
    captureSettings.jpegQuality = 60;
    captureSettings.tileSize    = 64;
    auto* capture      = new ScreenCapture(captureSettings);
    auto* cmdHandler   = new CommandHandler(&app);
    auto* chatHandler  = new ChatHandler(&app);
    auto* fileRecv     = new FileReceiver(&app);
    auto* examHandler  = new ExamHandler(&app);
    auto* audioPlayer  = new AudioPlayer(&app);
    auto* tray         = new AgentTray(&app);
    auto& broadcastRx  = ScreenBroadcastReceiver::instance();

    // Dosya alındığında bildirim + açma seçeneği
    QObject::connect(fileRecv, &FileReceiver::fileReceived,
                     [tray](const QString& path, qint64 size) {
        const QString fileName = QFileInfo(path).fileName();
        const QString sizeStr  = QString::number(size / 1024);
        Logger::info(QObject::tr("Tray: Dosya alındı → ") + path);

        tray->trayIcon()->showMessage(
            QObject::tr("Dosya Alındı"),
            fileName + " (" + sizeStr + " KB)",
            QSystemTrayIcon::Information, 6000);

        if (size < 20 * 1024 * 1024) {
            QMessageBox* mb = new QMessageBox(
                QMessageBox::Information,
                QObject::tr("Dosya Alındı"),
                QObject::tr("Öğretmeniniz bir dosya gönderdi: %1\n\nDosyayı şimdi açmak ister misiniz?").arg(fileName),
                QMessageBox::Yes | QMessageBox::No);
            mb->setAttribute(Qt::WA_DeleteOnClose);
            mb->setWindowFlags(mb->windowFlags() | Qt::WindowStaysOnTopHint);
            QObject::connect(mb, &QMessageBox::finished,
                             [path](int result) {
                if (result == QMessageBox::Yes)
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });
            mb->show();
            mb->raise();
        }
    });

    // ── Servise bağlan ────────────────────────────────────────────────────
    const QString socketPath = OkidiaConfig::runDir() + "/agent.sock";
    auto* localConn = new LocalConnection(socketPath, &app);

    QObject::connect(fileRecv, &FileReceiver::otaUpdateReady,
                     [localConn](const QString& path) {
        okidia::Envelope env;
        env.set_type(okidia::MessageType::OTA_UPDATE_INSTALL);
        env.set_payload(path.toStdString());
        localConn->sendEnvelope(env);
    });

    // Bağlantı kurulunca tray'i aktif et ve ilk bilgileri gönder
    QObject::connect(localConn, &LocalConnection::connected,
                     [startupUsername, getCurrentIpcToken, tray, cmdHandler, chatHandler,
                      examHandler, localConn, capture]() {
        Logger::info(QObject::tr("Tray: Servise bağlandı"));

        // IPC Token varsa servise doğrulat (Kriptografik Güvenlik)
        // Her yeni bağlantıda diskteki güncel token'ı tazele (servis yeniden başlatılmış olabilir)
        const QString activeToken = getCurrentIpcToken();
        if (!activeToken.isEmpty()) {
            okidia::Envelope tokenEnv;
            tokenEnv.set_type(okidia::MessageType::AGENT_IPC_HANDSHAKE);
            tokenEnv.set_payload(activeToken.toUtf8().toStdString());
            localConn->sendEnvelope(tokenEnv);
        }

        // El kaldır/indir mesajları için bağlantıyı kaydet
        tray->setLocalConnection(localConn);
        cmdHandler->setLocalConnection(localConn);
        cmdHandler->setScreenCapture(capture);
        cmdHandler->setTray(tray);
        chatHandler->setConnection(localConn);
        examHandler->setConnection(nullptr);

        // CLIENT_INFO: ekran boyutu + efektif kullanıcı adı
        SystemStats stats = SystemInfo::readStats();
        chatHandler->setMyInfo(SystemInfo::machineId(),
                                stats.hostname,
                                startupUsername);
        okidia::ClientInfo ci;
        ci.set_hostname(stats.hostname.toStdString());
        ci.set_username(startupUsername.toStdString());
        ci.set_display_name(SystemInfo::userDisplayName(startupUsername).toStdString());
        ci.set_os_version(stats.osVersion.toStdString());
        ci.set_os_tag(stats.osTag.toStdString());
        ci.set_os_arch(stats.osArch.toStdString());
        ci.set_agent_version(stats.agentVersion.toStdString());
        ci.set_machine_id(SystemInfo::machineId().toStdString());
        ci.set_ip_address(SystemInfo::ipAddress().toStdString());
        ci.set_screen_width(static_cast<uint32_t>(capture->screenWidth()));
        ci.set_screen_height(static_cast<uint32_t>(capture->screenHeight()));
        ci.set_screens(buildScreenResolutionsString(capture));
        ci.set_agent_type(cmdHandler->agentType());
        okidia::Envelope ciEnv;
        ciEnv.set_type(okidia::MessageType::CLIENT_INFO);
        ciEnv.set_payload(ci.SerializeAsString());
        localConn->sendEnvelope(ciEnv);

        // İlk tam frame (Helper kullanarak)
        sendFullFrame(localConn, capture, cmdHandler->agentType(), cmdHandler);
    });

    QObject::connect(localConn, &LocalConnection::disconnected, [tray]() {
        Logger::warning(QObject::tr("Tray: Servis bağlantısı kesildi, yeniden denenecek..."));
        tray->setConnected(false);
        tray->clearConnection();
    });

    // Akıllı Tahta dokunmatik Numpad'den PIN ile açıldığında durumu servise bildir
    QObject::connect(&SmartboardLockOverlay::instance(), &SmartboardLockOverlay::unlockedByTeacher,
                     [localConn, tray](const QString& username, const QString& displayName, const QString& password) {
        tray->setLocked(false);
        okidia::SmartboardUnlockNotify notify;
        notify.set_username(username.toStdString());
        notify.set_display_name(displayName.toStdString());
        notify.set_password(password.toStdString());
        notify.set_timestamp(static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch()));

        okidia::Envelope env;
        env.set_type(okidia::MessageType::SMARTBOARD_UNLOCK_NOTIFY);
        env.set_payload(notify.SerializeAsString());
        localConn->sendEnvelope(env);
        Logger::info(QObject::tr("Tray: Akıllı Tahta PIN ile açıldı, durum servise bildirildi: ") + username);
    });

    // ── Servisten gelen mesajları işle ────────────────────────────────────
    // ── Ekran tile timer (varsayılan 200ms) ───────────────────────────────
    QTimer* screenTimer = new QTimer(&app);
    screenTimer->setInterval(200);

    static QPointer<QMessageBox> s_pendingRemoteControlDialog;

    // ── Ekran yayını başladığında / bittiğinde izolasyon ve bant genişliği koruması ──
    QObject::connect(&broadcastRx, &ScreenBroadcastReceiver::broadcastStarted, [screenTimer, cmdHandler]() {
        Logger::info(QObject::tr("Tray: Ekran yayını başladı. Thumbnail akışı durduruluyor, uzak izleme/kontrol sonlandırılıyor."));
        screenTimer->stop();
        if (s_pendingRemoteControlDialog) {
            s_pendingRemoteControlDialog->setProperty("cancelled_by_master", true);
            s_pendingRemoteControlDialog->close();
        }
        cmdHandler->setScreenStreamActive(false);
    });

    QObject::connect(&broadcastRx, &ScreenBroadcastReceiver::broadcastEnded, [screenTimer]() {
        Logger::info(QObject::tr("Tray: Ekran yayını sona erdi."));
    });

    QObject::connect(localConn, &LocalConnection::envelopeReceived,
                     [startupUsername, cmdHandler, chatHandler, fileRecv,
                      tray, &broadcastRx, examHandler, audioPlayer,
                      capture, localConn, screenTimer]
                     (const okidia::Envelope& env) {

        switch (env.type()) {
        // Ekran / input / popup / uygulama komutları
        case okidia::MessageType::SCREEN_LOCK:
            tray->setLocked(true);
            cmdHandler->handleEnvelope(env);
            break;
        case okidia::MessageType::SCREEN_UNLOCK:
            tray->setLocked(false);
            cmdHandler->handleEnvelope(env);
            break;
        case okidia::MessageType::REMOTE_CONTROL_REQUEST: {
            if (broadcastRx.isBroadcasting()) {
                Logger::info(QObject::tr("Tray: Aktif ekran yayını olduğu için uzak erişim isteği reddedildi."));
                okidia::RemoteControlResponse resp;
                resp.set_accepted(false);
                resp.set_reason(QObject::tr("Cihazda aktif ekran yayını mevcut.").toStdString());
                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::REMOTE_CONTROL_RESPONSE);
                resEnv.set_payload(resp.SerializeAsString());
                localConn->sendEnvelope(resEnv);
                break;
            }

            uint32_t currentType = cmdHandler->agentType();
            if (currentType == 2) {
                if (s_pendingRemoteControlDialog) {
                    s_pendingRemoteControlDialog->setProperty("cancelled_by_master", true);
                    s_pendingRemoteControlDialog->close();
                }

                okidia::RemoteControlRequest req;
                req.ParseFromString(env.payload());
                QString title = req.is_control() ? QObject::tr("Uzak Kontrol Talebi") : QObject::tr("Ekran İzleme Talebi");
                QString msg   = req.is_control() ? 
                                QObject::tr("Yönetici bilgisayarınızın kontrolünü almak istiyor.\nOnaylıyor musunuz?") :
                                QObject::tr("Yönetici ekranınızı tam ekran izlemek istiyor.\nOnaylıyor musunuz?");
                
                QMessageBox* mb = new QMessageBox(QMessageBox::Question, title, msg, QMessageBox::Yes | QMessageBox::No);
                mb->setAttribute(Qt::WA_DeleteOnClose);
                mb->setWindowFlags(mb->windowFlags() | Qt::WindowStaysOnTopHint);
                s_pendingRemoteControlDialog = mb;
                
                // 120 saniye içinde onay gelmezse otomatik kapat (Reddetmiş say)
                QTimer* mbTimer = new QTimer(mb);
                mbTimer->setSingleShot(true);
                QObject::connect(mbTimer, &QTimer::timeout, [mb]() {
                    mb->setProperty("timeout_closed", true);
                    mb->close();
                });
                mbTimer->start(120000);

                QObject::connect(mb, &QMessageBox::finished, [localConn, cmdHandler, mb](int result) {
                    if (mb->property("cancelled_by_master").toBool()) {
                        Logger::info(QObject::tr("Tray: Uzak erişim isteği Master tarafından iptal edildiği için yanıt gönderilmiyor."));
                        return;
                    }
                    okidia::RemoteControlResponse resp;
                    if (result == QMessageBox::Yes) {
                        Logger::info(QObject::tr("Tray: Uzak erişim kullanıcı tarafından onaylandı."));
                        resp.set_accepted(true);
                        cmdHandler->setScreenStreamActive(true);
                    } else {
                        Logger::info(QObject::tr("Tray: Uzak erişim reddedildi veya zaman aşımına uğradı."));
                        resp.set_accepted(false);
                        if (mb->property("timeout_closed").toBool()) {
                            resp.set_reason("timeout");
                        } else {
                            resp.set_reason("Kullanıcı isteği reddetti.");
                        }
                    }
                    okidia::Envelope resEnv;
                    resEnv.set_type(okidia::MessageType::REMOTE_CONTROL_RESPONSE);
                    resEnv.set_payload(resp.SerializeAsString());
                    localConn->sendEnvelope(resEnv);
                });
                mb->show();
                mb->raise();
            } else {
                // Tip 2 değilse otomatik onay
                okidia::RemoteControlResponse resp;
                resp.set_accepted(true);
                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::REMOTE_CONTROL_RESPONSE);
                resEnv.set_payload(resp.SerializeAsString());
                localConn->sendEnvelope(resEnv);
            }
            break;
        }

        case okidia::MessageType::REMOTE_CONTROL_STOP: {
            // Açık bir bekleme diyaloğu varsa Master iptal ettiğinde anında kapat
            if (s_pendingRemoteControlDialog) {
                s_pendingRemoteControlDialog->setProperty("cancelled_by_master", true);
                s_pendingRemoteControlDialog->close();
            }
            for (auto* w : QApplication::topLevelWidgets()) {
                if (auto* mb = qobject_cast<QMessageBox*>(w)) {
                    if (mb->windowTitle().contains("Kontrol", Qt::CaseInsensitive) || 
                        mb->windowTitle().contains("İzleme", Qt::CaseInsensitive) ||
                        mb->windowTitle().contains("Control", Qt::CaseInsensitive)) {
                        mb->setProperty("cancelled_by_master", true);
                        mb->close();
                    }
                }
            }
            screenTimer->setInterval(1000); // Normal thumbnail hızına geri dön
            cmdHandler->handleEnvelope(env);
            capture->resetPreviousFrame();
            break;
        }

        case okidia::MessageType::INPUT_TAKE_CONTROL:
            screenTimer->setInterval(50); // 50ms = 20 FPS yüksek hızlı akış
            if (!screenTimer->isActive()) screenTimer->start();
            cmdHandler->handleEnvelope(env);
            break;

        case okidia::MessageType::SCREEN_CAPTURE:
        case okidia::MessageType::SCREEN_SELECT:
        case okidia::MessageType::SCREEN_INFO_REQUEST:
        case okidia::MessageType::INPUT_LOCK:
        case okidia::MessageType::INPUT_UNLOCK:
        case okidia::MessageType::MOUSE_EVENT:
        case okidia::MessageType::KEY_EVENT:
        case okidia::MessageType::MSG_POPUP:
        case okidia::MessageType::MSG_BROADCAST:
        case okidia::MessageType::APP_LIST_REQUEST:
        case okidia::MessageType::APP_CLOSE:
        case okidia::MessageType::AUDIO_UNMUTE_ONLY:
        case okidia::MessageType::AUDIO_PLAY_LOCAL:
        case okidia::MessageType::AUDIO_MUTE:
        case okidia::MessageType::AUDIO_UNMUTE:
        case okidia::MessageType::AUDIO_SET_VOLUME:
            cmdHandler->handleEnvelope(env);
            break;

        // Master elin indirilmesini istedi (örn. öğretmen eli kapattı)
        case okidia::MessageType::HAND_LOWER:
            tray->setHandRaised(false);
            break;

        // Sohbet
        case okidia::MessageType::CHAT_MESSAGE:
        case okidia::MessageType::CHAT_LIST_RESPONSE:
        case okidia::MessageType::CHAT_INVITE:
        case okidia::MessageType::CHAT_INVITE_REJECT:
        case okidia::MessageType::CHAT_ROOM_CREATED:
        case okidia::MessageType::CHAT_MEMBER_JOINED:
        case okidia::MessageType::CHAT_MEMBER_LEFT:
        case okidia::MessageType::CHAT_LOCK_STATE:
        case okidia::MessageType::PRIVATE_CHAT_REQUEST:
        case okidia::MessageType::PRIVATE_CHAT_ACCEPT:
        case okidia::MessageType::PRIVATE_CHAT_REJECT:
        case okidia::MessageType::PRIVATE_CHAT_OPEN:
        case okidia::MessageType::PRIVATE_CHAT_MESSAGE:
        case okidia::MessageType::PRIVATE_CHAT_CLOSE:
            chatHandler->handleEnvelope(env);
            break;

        // Dosya transferi
        case okidia::MessageType::FILE_TRANSFER_BEGIN:
        case okidia::MessageType::FILE_TRANSFER_CHUNK:
        case okidia::MessageType::FILE_TRANSFER_END:
            fileRecv->handleEnvelope(env);
            break;

        // Öğretmen Anahtar/Tohum Senkronizasyonu (Air-Gapped QR Kilit)
        case okidia::MessageType::TEACHER_KEY_SYNC_REQ: {
            okidia::TeacherKeySyncReq syncReq;
            if (syncReq.ParseFromString(env.payload())) {
                for (int i = 0; i < syncReq.teachers_size(); ++i) {
                    const auto& item = syncReq.teachers(i);
                    OfflineAuthManager::instance().upsertTeacher(
                        QString::fromStdString(item.teacher_id()),
                        QString::fromStdString(item.display_name()),
                        QString::fromStdString(item.username()),
                        QString::fromStdString(item.seed_hex())
                    );
                }
                Logger::info(QObject::tr("Tray: %1 adet öğretmen tohumu yerel depoya senkronize edildi.")
                             .arg(syncReq.teachers_size()));

                okidia::TeacherKeySyncRes syncRes;
                syncRes.set_success(true);
                syncRes.set_synced_count(syncReq.teachers_size());

                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::TEACHER_KEY_SYNC_RES);
                resEnv.set_payload(syncRes.SerializeAsString());
                localConn->sendEnvelope(resEnv);
            }
            break;
        }

        // Ekran yayını
        case okidia::MessageType::BROADCAST_BEGIN:
        case okidia::MessageType::BROADCAST_FRAME:
        case okidia::MessageType::BROADCAST_END:
            broadcastRx.handleEnvelope(env);
            break;

        // Ses yayını
        case okidia::MessageType::AUDIO_BROADCAST_FILE:
        case okidia::MessageType::AUDIO_BROADCAST_LIVE:
        case okidia::MessageType::AUDIO_BROADCAST_STOP:
            audioPlayer->handleEnvelope(env);
            break;

        // Sınav
        case okidia::MessageType::EXAM_START:
        case okidia::MessageType::EXAM_COLLECT:
            examHandler->handleEnvelope(env);
            break;
        case okidia::MessageType::EXAM_END:
            examHandler->handleEnvelope(env);
            // Sınav bittiğinde kısıtlamaları otomatik kaldır
            tray->setLocked(false);
            cmdHandler->screenUnlock();
            cmdHandler->inputUnlock();
            break;

        // Refresh: ekran bilgisi + tam frame yeniden gönder
        case okidia::MessageType::REFRESH_REQUEST: {
            SystemStats stats = SystemInfo::readStats();
            okidia::ClientInfo ci;
            ci.set_hostname(stats.hostname.toStdString());
            ci.set_username(startupUsername.toStdString());
            ci.set_display_name(SystemInfo::userDisplayName(startupUsername).toStdString());
            ci.set_os_version(stats.osVersion.toStdString());
            ci.set_os_tag(stats.osTag.toStdString());
            ci.set_os_arch(stats.osArch.toStdString());
            ci.set_agent_version(stats.agentVersion.toStdString());
            ci.set_machine_id(SystemInfo::machineId().toStdString());
            ci.set_ip_address(SystemInfo::ipAddress().toStdString());
            ci.set_screen_width(static_cast<uint32_t>(capture->screenWidth()));
            ci.set_screen_height(static_cast<uint32_t>(capture->screenHeight()));
            ci.set_screens(buildScreenResolutionsString(capture));
            ci.set_agent_type(cmdHandler->agentType());
            okidia::Envelope ciEnv;
            ciEnv.set_type(okidia::MessageType::CLIENT_INFO);
            ciEnv.set_payload(ci.SerializeAsString());
            localConn->sendEnvelope(ciEnv);

            sendFullFrame(localConn, capture, cmdHandler->agentType(), cmdHandler);
            Logger::info(QObject::tr("Tray: Refresh işlendi"));
            break;
        }
        
        case okidia::MessageType::SCREEN_STREAM_PAUSE: {
            if (cmdHandler->isRemoteControlActive() || cmdHandler->isScreenStreamActive()) {
                Logger::info(QObject::tr("Tray: Uzak kontrol aktif olduğu için SCREEN_STREAM_PAUSE yoksayıldı."));
                break;
            }
            screenTimer->stop();
            Logger::info(QObject::tr("Tray: Ekran akışı duraklatıldı (SCREEN_STREAM_PAUSE)"));
            break;
        }

        case okidia::MessageType::SCREEN_FPS_SET: {
            okidia::ScreenConfig config;
            if (config.ParseFromString(env.payload())) {
                if (config.interval_ms() > 0) {
                    bool isRemote = cmdHandler->isRemoteControlActive() || cmdHandler->isScreenStreamActive();
                    if (!isRemote || config.max_width() > 0) {
                        screenTimer->setInterval(config.interval_ms());
                        Logger::info(QObject::tr("Tray: Ekran akış hızı güncellendi → %1 ms").arg(config.interval_ms()));
                    } else {
                        Logger::info(QObject::tr("Tray: Uzak kontrol aktif, thumbnail FPS (%1 ms) ezmesi engellendi").arg(config.interval_ms()));
                    }
                }
            }
            // ScreenCapture ayarlarının (maxWidth, jpegQuality) da güncellenmesi için
            cmdHandler->handleEnvelope(env);

            // İlk Kare Garantisi (Keyframe / I-Frame):
            // m_previousFrame sıfırlanır; ilk captureDirtyTiles döngüsünde tüm ekran tam karo kümesi olarak basılır
            capture->resetPreviousFrame();

            if (!broadcastRx.isBroadcasting()) {
                if (!screenTimer->isActive()) {
                    screenTimer->start();
                    Logger::info(QObject::tr("Tray: Ekran akışı başlatıldı (SCREEN_FPS_SET)"));
                }
            } else {
                Logger::info(QObject::tr("Tray: Aktif ekran yayını olduğu için thumbnail akışı başlatılmadı."));
            }
            break;
        }

        case okidia::MessageType::AGENT_TYPE_SET: {
            uint32_t newType = std::stoul(env.payload());
            uint32_t currentType = cmdHandler->agentType();

            if (newType == currentType) {
                // Tip zaten aynı, sessizce onayla (Servis'in senkronize olması için)
                okidia::AgentTypeSetResult res;
                res.set_requested_type(newType);
                res.set_approved(true);
                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::AGENT_TYPE_SET_RESULT);
                resEnv.set_payload(res.SerializeAsString());
                localConn->sendEnvelope(resEnv);
                break;
            }

            // GİZLİLİK KORUMASI: Sadece İdari PC (Tip 2) durumundan çıkış onaya tabidir.
            // İdari PC olmak (gizlilik artışı) onay gerektirmez.
            bool needsApproval = (currentType == 2 && newType != 2);
            
            Logger::info(QObject::tr("Tray: Tip değişim isteği ← Mevcut:%1 Yeni:%2 OnayGerekli:%3")
                         .arg(currentType).arg(newType).arg(needsApproval ? "EVET" : "HAYIR"));
            
            if (needsApproval) {
                // Onay iste
                QString typeName = (newType == 1) ? QObject::tr("Öğrenci PC") 
                                 : (newType == 2) ? QObject::tr("İdari PC") 
                                 : (newType == 3) ? QObject::tr("Akıllı Tahta") 
                                 : (newType >= 100) ? QObject::tr("Özel Profil") 
                                 : QObject::tr("Bilinmeyen");
                QMessageBox* mb = new QMessageBox(
                    QMessageBox::Question,
                    QObject::tr("Güvenlik Onayı"),
                    QObject::tr("Yönetici bilgisayar tipinizi '%1' olarak değiştirmek istiyor.\n\n"
                                "Bu işlem GİZLİLİK durumunu değiştirecektir.\n\n"
                                "Onaylıyor musunuz?").arg(typeName),
                    QMessageBox::Yes | QMessageBox::No);
                mb->setAttribute(Qt::WA_DeleteOnClose);
                mb->setWindowFlags(mb->windowFlags() | Qt::WindowStaysOnTopHint);
                
                // 5 dakika içinde onay gelmezse otomatik kapat (Reddetmiş say)
                QTimer::singleShot(300000, mb, &QMessageBox::close);

                QObject::connect(mb, &QMessageBox::finished, [newType, cmdHandler, localConn, capture](int result) {
                    okidia::AgentTypeSetResult res;
                    res.set_requested_type(newType);
                    res.set_approved(result == QMessageBox::Yes);
                    
                    if (result == QMessageBox::Yes) {
                        cmdHandler->setAgentType(newType);
                        Logger::info(QObject::tr("Tray: Tip değişikliği kullanıcı tarafından onaylandı → ") + QString::number(newType));
                        // Tipi hemen Master'a yansıt (Yeşil kutu veya Gerçek görüntü)
                        sendFullFrame(localConn, capture, newType, cmdHandler);
                    } else {
                        res.set_reason("Kullanıcı gizlilik uyarısını reddetti.");
                        Logger::info(QObject::tr("Tray: Tip değişikliği kullanıcı tarafından REDDEDİLDİ"));
                    }
                    
                    okidia::Envelope resEnv;
                    resEnv.set_type(okidia::MessageType::AGENT_TYPE_SET_RESULT);
                    resEnv.set_payload(res.SerializeAsString());
                    localConn->sendEnvelope(resEnv);
                });
                mb->show();
                mb->raise();
            } else {
                // Hemen uygula ve bildirim göster (onay gerektirmeyen durumlar)
                cmdHandler->setAgentType(newType);
                Logger::info(QObject::tr("Tray: Tip atandı (Onay gerekmiyor) → ") + QString::number(newType));
                
                // Tipi hemen Master'a yansıt (Yeşil kutu veya Gerçek görüntü)
                sendFullFrame(localConn, capture, newType, cmdHandler);

                QString typeName = (newType == 1) ? QObject::tr("Öğrenci PC") : (newType == 2 ? QObject::tr("İdari PC") : (newType == 3 ? QObject::tr("Akıllı Tahta") : "Bilinmeyen"));
                tray->trayIcon()->showMessage(QObject::tr("Agent Tipi Güncellendi"), 
                                             "Yeni tip: " + typeName,
                                             QSystemTrayIcon::Information, 3000);
            
                // Sonucu bildir (onay gerektirmeyen durumlar için de cevap gönderelim ki broker bilsin)
                okidia::AgentTypeSetResult res;
                res.set_requested_type(newType);
                res.set_approved(true);
                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::AGENT_TYPE_SET_RESULT);
                resEnv.set_payload(res.SerializeAsString());
                localConn->sendEnvelope(resEnv);
                
                Logger::info(QObject::tr("Tray: Tip atandı → ") + QString::number(newType));
            }
            break;
        }
        
        case okidia::MessageType::AGENT_STATUS: {
            okidia::AgentStatus st;
            if (st.ParseFromString(env.payload())) {
                Logger::info(QObject::tr("Tray: Broker durum güncellemesi → %1")
                             .arg(st.broker_connected() ? QObject::tr("Bağlı") : QObject::tr("Bağlantı Yok")));
                tray->setConnected(st.broker_connected());
            }
            break;
        }

        case okidia::MessageType::AGENT_ALIAS_SET: {
            okidia::AgentAliasSet aliasMsg;
            if (aliasMsg.ParseFromString(env.payload())) {
                QString aliasStr = QString::fromStdString(aliasMsg.alias()).trimmed();
                OkidiaConfig::setAgentAlias(aliasStr);
                SmartboardLockOverlay::instance().setAlias(aliasStr);
                Logger::info(QObject::tr("Tray: İstemci takma adı güncellendi → ") + aliasStr);
            }
            break;
        }

        case okidia::MessageType::AGENT_POLICY_SET: {
            okidia::AgentPolicySet policy;
            if (policy.ParseFromString(env.payload())) {
                QSettings s("okidia", "agent");
                s.setValue("policy/usb_protection_enabled", policy.usb_protection_enabled());
                Logger::info(QObject::tr("Tray: Sistem politikası güncellendi (USB Koruma: %1)")
                             .arg(policy.usb_protection_enabled() ? QObject::tr("AÇIK") : QObject::tr("KAPALI")));
            }
            break;
        }

        default:
            Logger::info(QObject::tr("Tray: İşlenmeyen mesaj → tip:") +
                         QString::number(env.type()));
            break;
        }
    });

    QObject::connect(screenTimer, &QTimer::timeout,
                     [localConn, capture, cmdHandler]() {
        if (!localConn->isConnected()) return;

        // Unix soket tampon belleğinde birikme varsa (servis TLS soketinin boşalmasını bekliyor demektir),
        // yeni ekran karesi yakalamayı atla. Böylece gecikme (lag) önlenir.
        // Uzak masaüstü bağlantısı (yüksek kalite/fps) aktifse buffer limitini yüksek tut (2 MB).
        qint64 bufferLimit = (capture->settings().scalePercent > 40) ? 2 * 1024 * 1024 : 128 * 1024;
        if (localConn->bytesToWrite() > bufferLimit) {
            return;
        }

        uint32_t currentType = cmdHandler->agentType();
        


        // İdari PC: Ekran görüntülerini gönderme (Master'a boş/yeşil bir sinyal gitmiş olacak)
        if (currentType == 2 && !cmdHandler->isScreenStreamActive()) {
            // Sadece thumbnail online statüsünü korumak için gerekirse çok nadir boş update gönderilebilir
            // Şimdilik hiç gönderilmemesi yeterli (İlk frame yeşil olarak kalacaktır)
            return;
        }

        auto tiles = capture->captureDirtyTiles();
        
        static qint64 lastKeepAlive = 0;
        qint64 now = QDateTime::currentMSecsSinceEpoch();

        if (tiles.empty()) {
            // Master tarafındaki watchdog zamanlayıcısının (4 saniye) 
            // statik ekranlarda yanlışlıkla devreye girmesini önlemek için
            // 2 saniyede bir boş bir keep-alive gönder.
            if (now - lastKeepAlive < 2000) {
                return;
            }
        }
        lastKeepAlive = now;

        okidia::ScreenTileUpdate update;
        update.set_screen_width(static_cast<uint32_t>(capture->scaledWidth()));
        update.set_screen_height(static_cast<uint32_t>(capture->scaledHeight()));
        for (const auto& t : tiles) {
            auto* tile = update.add_tiles();
            tile->set_x(t.x);
            tile->set_y(t.y);
            tile->set_width(t.width);
            tile->set_height(t.height);
            tile->set_jpeg_data(t.jpegData.constData(), t.jpegData.size());  // B2: zero-copy
        }

        okidia::Envelope env;
        env.set_type(okidia::MessageType::SCREEN_TILE_UPDATE);
        env.set_payload(update.SerializeAsString());
        localConn->sendEnvelope(env);
    });
    // screenTimer başlangıçta durdurulmuş kalır (Talep tabanlı akış).
    // Yalnızca Broker/Master'dan SCREEN_FPS_SET geldiğinde start() edilir.

    // ── Servise bağlan — servis henüz hazır olmayabilir, tekrar dene ──────
    QTimer* reconnectTimer = new QTimer(&app);
    reconnectTimer->setInterval(3000);
    QObject::connect(reconnectTimer, &QTimer::timeout,
                     [localConn]() {
        if (localConn->isConnected()) return;
        Logger::info(QObject::tr("Tray: Servise bağlanılıyor → %1/agent.sock").arg(OkidiaConfig::runDir()));
        localConn->connectToServer();
    });
    localConn->connectToServer();
    reconnectTimer->start();


    Logger::info(QObject::tr("Tray: Hazır"));
    Logger::info(QObject::tr("Tray: System tray mevcut: %1")
                 .arg(QSystemTrayIcon::isSystemTrayAvailable() ? "evet" : QObject::tr("hayır")));

    // Cinnamon/XFCE'de system tray geç hazır olabiliyor
    // DM tray'i için gecikmeyi kıs (login ekranı tray ikonu olmayacak zaten)
    const bool isDmTray = !forcedUsername.isEmpty();
    const int trayDelay = isDmTray ? 300 : 2000;

    // ── Tray sinyalleri ───────────────────────────────────────────────────
    // "Sohbet" menü öğesi → sohbet penceresini aç
    QObject::connect(tray, &AgentTray::chatStartRequested,
                     [chatHandler]() {
        if (chatHandler)
            chatHandler->showChatWindow();
    });

    QObject::connect(chatHandler, &ChatHandler::chatAvailabilityChanged,
                     [tray](bool available) {
        if (tray) tray->setChatEnabled(available);
    });

    QObject::connect(chatHandler, &ChatHandler::handRaiseSent,
                     [tray](const QString& note) {
        Q_UNUSED(note);
        if (tray) {
            tray->setHandRaised(true);
        }
    });

    QTimer::singleShot(trayDelay, [tray]() {
        tray->show();
        Logger::info(QObject::tr("Tray: show() çağrıldı"));
    });

    return app.exec();
}
