// master/main.cpp
// OKIDIA Master Application Entry Point
// Supports Zero-Config Discovery, mTLS session routing, and autonomous OTA update management.
#include <QApplication>
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <grp.h>
#include <sys/types.h>
#endif
#include <QDir>
#include <QIcon>
#include <QPalette>
#include <QColor>

#include <QSettings>
#include <QMessageBox>

#include <QFile>
#include <QStandardPaths>
#include <QEventLoop>
#include <QProcess>
#include <QSysInfo>
#include "ui/MainWindow/MainWindow.h"
#include "ui/SetupWizard/SetupWizard.h"
#include "ui/LoginDialog/LoginDialog.h"
#include "session/SessionManager.h"
#include "audit/AuditLog.h"
#include "license/LicenseManager.h"
#include "../common/utils/Logger.h"
#include "../common/utils/OkidiaUtils.h"
#include "../common/ui/ThemeManager.h"
#include "../common/OkidiaConfig.h"

#include <QTranslator>
#include <QSslSocket>
#include "../common/crypto/KeyManager.h"

int main(int argc, char* argv[]) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication app(argc, argv);
    app.setApplicationName("master");

#ifdef Q_OS_WIN
    if (QSslSocket::supportsSsl()) {
        QSslSocket::setActiveBackend("openssl");
        Logger::info(QObject::tr("Master: TLS Motoru Aktif → %1 (OpenSSL %2)")
            .arg(QSslSocket::activeBackend())
            .arg(QSslSocket::sslLibraryVersionString()));
    }
#endif

    // --- Dil ayarı yükleme (Translation) ---
    QSettings langSettings("okidia", "master");
    int langIdx = langSettings.value("ui/language", 0).toInt();
    QString explicitLang;
    if (langIdx == 1) {
        explicitLang = "tr";
    } else if (langIdx == 2) {
        explicitLang = "en";
    } else {
        explicitLang = "auto";
    }
    OkidiaUtils::installTranslator(app, explicitLang);

#ifdef Q_OS_UNIX
    // --- Grup yetkisi kontrolü ---
    {
        int ngroups = 0;
        gid_t groups[256];
        ngroups = getgroups(256, groups);

        struct group *grp = getgrnam("okidia");
        if (grp) {
            bool inProcessGroups = false;
            for (int i = 0; i < ngroups; ++i) {
                if (groups[i] == grp->gr_gid) {
                    inProcessGroups = true;
                    break;
                }
            }

            if (!inProcessGroups) {
                char *user = getlogin();
                if (user) {
                    bool inGroupFile = false;
                    for (char **mem = grp->gr_mem; *mem; ++mem) {
                        if (QString(*mem) == user) {
                            inGroupFile = true;
                            break;
                        }
                    }

                    if (inGroupFile) {
                        QMessageBox::critical(nullptr, QObject::tr("Grup Yetkisi Bekleniyor"),
                            QObject::tr("OKIDIA kurulumu tamamlandı ancak 'okidia' grup izinlerinin mevcut "
                                        "oturumunuzda aktifleşmesi gerekiyor.\n\n"
                                        "Lütfen oturumunuzu kapatıp yeniden açın (veya bilgisayarı yeniden başlatın) "
                                        "ve uygulamayı tekrar çalıştırın."));
                        return 1;
                    }
                }
            }
        }
    }
#endif

    // ── Uygulama ikonu (görev çubuğu + pencere başlığı) ──────────
    // addFile resource yollarında QFile::exists() gerektirmez.
    // Qt eksik dosyaları sessizce atlar.
    {
        QIcon appIcon;
        appIcon.addFile(":/icons/master/okidia-master-16.png",  QSize(16,16));
        appIcon.addFile(":/icons/master/okidia-master-22.png",  QSize(22,22));
        appIcon.addFile(":/icons/master/okidia-master-32.png",  QSize(32,32));
        appIcon.addFile(":/icons/master/okidia-master-48.png",  QSize(48,48));
        appIcon.addFile(":/icons/master/okidia-master-64.png",  QSize(64,64));
        appIcon.addFile(":/icons/master/okidia-master-128.png", QSize(128,128));
        appIcon.addFile(":/icons/master/okidia-master-256.png", QSize(256,256));
        qDebug() << "Icon sizes:" << appIcon.availableSizes();
        app.setWindowIcon(appIcon);
    }

    // --- Tek instance kontrolü ---
    const QString pidFilePath =
        QDir::tempPath() + "/okidia-master.pid";
    {
        QFile existing(pidFilePath);
        if (existing.open(QIODevice::ReadOnly)) {
            qint64 pid = existing.readAll().trimmed().toLongLong();
            existing.close();
            bool isActuallyRunning = false;
#if defined(Q_OS_UNIX)
            if (pid > 0 && QFile::exists(QString("/proc/%1").arg(pid))) {
                QFile commFile(QString("/proc/%1/comm").arg(pid));
                if (commFile.open(QIODevice::ReadOnly)) {
                    QString comm = QString::fromUtf8(commFile.readAll()).trimmed();
                    if (comm.contains("okidia-master", Qt::CaseInsensitive)) {
                        isActuallyRunning = true;
                    }
                }
            }
#endif
            if (isActuallyRunning) {
                // QApplication başlamadan önce sadece stderr'e yaz
                fprintf(stderr, "%s\n",
                        QObject::tr("OKIDIA Master zaten çalışıyor (PID: %1)").arg(pid).toUtf8().constData());
                return 0;
            }
            QFile::remove(pidFilePath);
        }
        QFile pidFile(pidFilePath);
        if (pidFile.open(QIODevice::WriteOnly)) {
            pidFile.write(QString::number(
                QCoreApplication::applicationPid()).toUtf8());
        }
    }
    app.setApplicationVersion(Okidia::Version::string());
    app.setOrganizationName("okidia");

    // ── Dirty Exit Kontrolü ve Temizlik ────────────────────────────────────
    QSettings settings("okidia", "master");
    bool lastExitSafe = settings.value("system/safe_exit", true).toBool();
    bool wasRememberMe = settings.value("login/remember_me", true).toBool();

    if (!lastExitSafe && !wasRememberMe) {
        Logger::warning(QObject::tr("Master: Bir önceki oturum düzgün kapatılmamış!"));
    }
    // Şimdi 'false' yapıyoruz, eğer düzgün kapanırsa 'true' olacak
    settings.setValue("system/safe_exit", false);
    settings.sync();

    // --- Logger başlat ---
    Logger::init(OkidiaConfig::logDir() + "/okidia-master.log");
    Logger::info(QObject::tr("OKIDIA Master başlatılıyor... v") + app.applicationVersion());

    // --- AuditLog başlat (broker DB hazır olduktan sonra init çağrılır) ---
    // init() broker DB açılınca main loop içinde tekrar çağrılır

    // --- Sertifika dizini seç ---
    const bool isProd = OkidiaConfig::isProduction();

    // --- Lisans yükle ---
    QString masterCertFile = OkidiaConfig::userCertPath();
    if (!QFile::exists(masterCertFile)) {
        masterCertFile = OkidiaConfig::masterCertBaseDir() + "/master.crt";
    }
    if (QFile::exists(masterCertFile)) {
        LicenseManager::instance().load(masterCertFile);
        LicenseManager::instance().startPeriodicSync("https://lisans.okidia.net");
    }

    // --- Tema: ThemeManager başlat ve kayıtlara göre uygula ---
    {
        ThemeManager::instance().initialize(&app);
        QSettings s("okidia", "master");
        int themeIdx = s.value("ui/theme", 0).toInt();
        ThemeManager::instance().applyTheme(themeIdx);
    }

    // ── Geçici / Artık Ağ Klasörlerini Temizle ────────────────────────
    OkidiaConfig::cleanupGhostNetworks();

    // ── Veritabanı ────────────────────────────────────────────────────
    {
        QString dbPath = OkidiaConfig::dbDir() + "/okidia.db";
        if (OkidiaDatabase::instance().open(dbPath)) {
            Logger::info(QObject::tr("Master: Yerel veritabanı açıldı: ") + dbPath);
        } else {
            Logger::error(QObject::tr("Master: Yerel veritabanı açılamadı!"));
        }

        // AuditLog initializes in network-only mode
        AuditLog::instance().init();
    }

    bool isConfigured = false;
    QString initialCert, initialKey, initialCa;
    
    // Kurulu sertifikaları kontrol et
    QDir baseDir(OkidiaConfig::masterCertBaseDir());
    if (baseDir.exists()) {
        // Mevcut kullanıcıları tara
        for (const QString& d : baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString c = OkidiaConfig::userCertPath(d);
            QString ca = OkidiaConfig::userCaPath(d);
            QString k = OkidiaConfig::userKeyPath(d);
            if (QFile::exists(c) && QFile::exists(k) && QFile(k).size() > 0) {
                initialCert = c;
                initialKey  = k;
                initialCa   = ca;
                isConfigured = true;
                break;
            }
        }

        // Fallback: Eski genel ca.crt varsa onu kullan
        if (initialCa.isEmpty() || !QFile::exists(initialCa)) {
            QString fallbackCa = OkidiaConfig::masterCertBaseDir() + "/ca.crt";
            if (QFile::exists(fallbackCa)) initialCa = fallbackCa;
        }

        // Fallback: Ana master.crt ve master.key
        if (!isConfigured) {
            QString fallbackCert = OkidiaConfig::masterCertBaseDir() + "/master.crt";
            QString fallbackKey  = OkidiaConfig::masterCertBaseDir() + "/master.key";
            if (QFile::exists(fallbackCert) && QFile::exists(fallbackKey) && QFile(fallbackKey).size() > 0) {
                initialCert = fallbackCert;
                initialKey  = fallbackKey;
                isConfigured = true;
                Logger::info(QObject::tr("Master: Ana master sertifikası kullanılıyor: ") + fallbackCert);
            }
        }
    }

    if (!isConfigured) {
        Logger::info(QObject::tr("Master: Kurulu bir kullanıcı/sertifika bulunamadı → Kurulum Sihirbazı başlatılıyor."));
        SetupWizard wiz;
        if (wiz.exec() != QDialog::Accepted) {
            Logger::info(QObject::tr("Kurulum iptal edildi."));
            QFile::remove(pidFilePath);
            return 0;
        }
        // Sihirbaz tamamlandı, ayarları tazele ve veritabanını yeni okul dizinine geçir
        OkidiaConfig::instance().reload();
        OkidiaDatabase::instance().switchDatabase(OkidiaConfig::dbDir() + "/okidia.db");
        OkidiaConfig::cleanupGhostNetworks(OkidiaDatabase::instance().path());

        if (!wiz.masterCertPath().isEmpty() && QFile::exists(wiz.masterCertPath())
            && !wiz.masterKeyPath().isEmpty() && QFile::exists(wiz.masterKeyPath())
            && QFile(wiz.masterKeyPath()).size() > 0) {
            initialCert = wiz.masterCertPath();
            initialKey  = wiz.masterKeyPath();
            initialCa   = wiz.caCertPath();
        } else {
            // Yeni oluşan sertifikaları tara
            QDir updatedBaseDir(OkidiaConfig::masterCertBaseDir());
            for (const QString& d : updatedBaseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString c = OkidiaConfig::userCertPath(d);
                QString ca = OkidiaConfig::userCaPath(d);
                QString k = OkidiaConfig::userKeyPath(d);
                if (QFile::exists(c) && QFile::exists(k) && QFile(k).size() > 0) {
                    initialCert = c;
                    initialKey  = k;
                    initialCa   = ca;
                    break;
                }
            }
        }
    }

    // --- Session Manager (Baştan oluşturuyoruz ki Discovery başlasın) ---
    SessionManager* session = new SessionManager();
    AuditLog::instance().setSessionManager(session);
    session->startAutoDiscovery(initialCert, initialKey, initialCa);

    // ── Login → MainWindow döngüsü ────────────────────────────────────────
    while (true) {
        UserRecord activeUser;
        bool isRememberMe = true;
        bool isOfflineSsh = false;

        {
            // LoginDialog kendi kapsamında yaşamalı; MainWindow açıkken arka planda
            // loginResult sinyallerini dinleyip m_isOfflineMode/kimlik durumunu bozmasın.
            LoginDialog loginDlg;
            loginDlg.setSessionManager(session);
            if (loginDlg.exec() != QDialog::Accepted) {
                Logger::info(QObject::tr("Login iptal edildi, çıkılıyor."));
                delete session;
                QFile::remove(pidFilePath);
                return 0;
            }
            activeUser = loginDlg.loggedInUser();
            isRememberMe = loginDlg.rememberMe();
            isOfflineSsh = loginDlg.isOfflineSshMode();
        }
        if (isOfflineSsh) {
            session->setOfflineMode(true);
            Logger::info(QObject::tr("Master: Bağımsız Çevrimdışı SSH Modu ile başlatıldı."));
        }
        
        QSettings loginSettings("okidia", "master");
        loginSettings.setValue("login/remember_me", isRememberMe);
        
        // Yükle ve uygula bu kullanıcının temasını
        {
            int themeIdx = loginSettings.value("users/" + activeUser.username + "/ui/theme", 0).toInt();
            ThemeManager::instance().applyTheme(themeIdx);
        }
        
        Logger::info(QObject::tr("Aktif kullanıcı: ") + activeUser.username
                     + " (" + activeUser.role + ")");
        
        // Kullanıcı profiline ait veritabanına geçiş yap
        {
            QString userDbPath = OkidiaConfig::masterDbPath(activeUser.id);
            if (!OkidiaDatabase::instance().switchDatabase(userDbPath)) {
                Logger::warning(QObject::tr("Master: Kullanıcı profili veritabanına geçilemedi, varsayılan DB kullanılacak: ") + userDbPath);
            } else {
                Logger::info(QObject::tr("Master: Kullanıcı profili veritabanı aktif: ") + userDbPath);
            }
        }
        
        // Login kaydı
        AuditLog::instance().setActiveUser(activeUser.id, activeUser.username);
        AuditLog::instance().recordAuth(activeUser.username,
            QObject::tr("Giriş yapıldı (") + activeUser.role + ")");

        // ── Sertifika Kontrolü ve Otomatik Enrolment ───────────────────
        const QString activeNetworkKey = OkidiaConfig::currentNetworkKey();
        QString clientCert = OkidiaConfig::userCertPath(activeUser.id, activeNetworkKey);
        QString clientKey  = OkidiaConfig::userKeyPath(activeUser.id, activeNetworkKey);
        QString caCert     = OkidiaConfig::userCaPath(activeUser.id, activeNetworkKey);
        if (!QFile::exists(caCert)) {
            QString fallbackCa = OkidiaConfig::userCaPath("DefaultAdmin", activeNetworkKey);
            if (QFile::exists(fallbackCa)) caCert = fallbackCa;
            else if (QFile::exists(OkidiaConfig::masterCertBaseDir() + "/ca.crt"))
                caCert = OkidiaConfig::masterCertBaseDir() + "/ca.crt";
        }

        // Eğer aktif kullanıcının kendi sertifikası henüz oluşmamışsa, makinedeki DefaultAdmin sertifikasını devral
        if (!isOfflineSsh && (!QFile::exists(clientCert) || !QFile::exists(clientKey) || QFile(clientKey).size() == 0)) {
            QString defCert = OkidiaConfig::userCertPath("DefaultAdmin", activeNetworkKey);
            QString defKey  = OkidiaConfig::userKeyPath("DefaultAdmin", activeNetworkKey);
            QString defCa   = OkidiaConfig::userCaPath("DefaultAdmin", activeNetworkKey);
            if (QFile::exists(defCert) && QFile::exists(defKey) && QFile(defKey).size() > 0) {
                QDir().mkpath(QFileInfo(clientCert).path());
                QFile::remove(clientCert);
                QFile::remove(clientKey);
                if (QFile::copy(defCert, clientCert) && QFile::copy(defKey, clientKey)) {
                    OkidiaUtils::securePrivateKeyFile(clientKey, false);
                    if (QFile::exists(defCa)) {
                        QFile::remove(caCert);
                        QFile::copy(defCa, caCert);
                    }
                    Logger::info(QObject::tr("Master: DefaultAdmin sertifikası kullanıcı profiline aktarıldı → ") + activeUser.username);
                }
            }
        }

        if (!isOfflineSsh && !QFile::exists(clientCert)) {
            Logger::info(QObject::tr("Master: Sertifika yok → Otomatik kayıt (Enrolment) başlatılıyor: ") + activeUser.username);
            
            // CSR üret (Saf C++ OpenSSL BIO)
            const QString csrPath = QDir::tempPath() + "/okidia_enroll.csr";
            if (!QFile::exists(clientKey)) {
                QDir().mkpath(QFileInfo(clientKey).path());
                KeyManager::generateKeyPair(clientKey, clientKey + ".pub");
            }
            
            KeyManager::generateCsr(clientKey, csrPath, activeUser.id);
            
            QFile f(csrPath);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray csrPem = f.readAll();
                f.close();
                QFile::remove(csrPath);

                QEventLoop loop;
                QTimer timeoutTimer;
                timeoutTimer.setSingleShot(true);
                QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
                timeoutTimer.start(10000); // 10 saniye limit

                bool enrollOk = false;
                QString enrollErr = QObject::tr("Zaman aşımı: Broker'dan yanıt alınamadı.");
                
                QObject::connect(session, &SessionManager::enrollResult, 
                    session, [&](bool ok, const QString& cert, const QString& ca, const QString& err){
                        if (ok) {
                            QFile cf(clientCert);
                            QDir().mkpath(QFileInfo(clientCert).path());
                            if (cf.open(QIODevice::WriteOnly)) {
                                cf.write(cert.toUtf8());
                                cf.close();
                            }

                            QString activeCa = OkidiaConfig::userCaPath(activeUser.id);
                            QFile caf(activeCa);
                            QDir().mkpath(QFileInfo(caf).path());
                            if (caf.open(QIODevice::WriteOnly)) {
                                caf.write(ca.toUtf8());
                                caf.close();
                            }
                            caCert = activeCa;
                            enrollOk = true;
                        } else {
                            enrollErr = err;
                        }
                        loop.quit();
                    },
                    Qt::SingleShotConnection  // Bir kez tetiklensin, tekrar bağlanmaya yol açmasın
                );
                
                session->requestEnroll(csrPem, QSysInfo::machineHostName());
                loop.exec();

                if (!enrollOk) {
                    Logger::warning(QObject::tr("Master: Kayıt başarısız → ") + enrollErr);
                    QMessageBox::critical(nullptr, QObject::tr("Kayıt Hatası"), QObject::tr("Sertifika alınamadı:\n") + enrollErr);
                    continue; // Login'e dön
                }
                Logger::info(QObject::tr("Master: Sertifika başarıyla alındı. Yetkili bağlantı için yeniden bağlanılıyor..."));
                
                // Broker'a yeni sertifikayla bağlan — mDNS zaten arıyor
                // Sertifika yolları güncellendi, anonim bağlantıyı kopar ki
                // SessionManager yeni cert ile tekrar bağlansın
                session->connectToBroker(clientCert, clientKey, caCert);
                if (session->tlsClient()) {
                    session->tlsClient()->reconnectWithCert(clientCert, clientKey, caCert);
                }

                // Broker bağlantısını bekle (max 3sn)
                if (!session->isConnected()) {
                    QEventLoop reconnectLoop;
                    QTimer reconnectTimeout;
                    reconnectTimeout.setSingleShot(true);
                    QObject::connect(&reconnectTimeout, &QTimer::timeout,
                                     &reconnectLoop, &QEventLoop::quit);
                    QObject::connect(session, &SessionManager::brokerConnected,
                                     &reconnectLoop, &QEventLoop::quit,
                                     Qt::SingleShotConnection);
                    reconnectTimeout.start(3000);
                    reconnectLoop.exec();
                }
            }
        } else if (!isOfflineSsh) {
            // Sertifika zaten var — anonim bağlıysa veya farklı kullanıcı sertifikası gerekiyorsa yenile
            if (QFile::exists(clientCert)) {
                session->connectToBroker(clientCert, clientKey, caCert);
                if (session->tlsClient() &&
                    (session->tlsClient()->isAnonymous() || !session->isConnected())) {
                    Logger::info(QObject::tr("Master: Sertifika mevcut, yetkili bağlantı tazeleniyor..."));
                    session->tlsClient()->reconnectWithCert(clientCert, clientKey, caCert);
                    // Bağlantı kurulana kadar max 2sn bekle
                    if (!session->isConnected()) {
                        QEventLoop waitLoop;
                        QObject::connect(session, &SessionManager::brokerConnected,
                                         &waitLoop, &QEventLoop::quit,
                                         Qt::SingleShotConnection);
                        QTimer::singleShot(2000, &waitLoop, &QEventLoop::quit);
                        waitLoop.exec();
                    }
                }
            }
        }

        // --- Ana pencere ---
        MainWindow w(session);

        // ÖNEM: loadFromDatabase(), setActiveUser()'dan ÖNCE çağrılmalı.
        // setActiveUser() içinde reloadTree() çağrılır; bu sırada m_clientStates
        // ve m_clientMeta dolu olmalıdır. Aksi hâlde tree boş çizilir.
        session->loadFromDatabase();

        if (!activeUser.id.isEmpty())
            w.setActiveUser(activeUser);

        if (isOfflineSsh) {
            w.applyOfflineModeConstraints(true);
        }

        bool wantsLogout = false;
        QEventLoop loop;
        QObject::connect(&w, &MainWindow::logoutRequested, [&]() {
            wantsLogout = true;
            AuditLog::instance().recordAuth(activeUser.username, QObject::tr("Çıkış yapıldı"));
            
            app.setQuitOnLastWindowClosed(false);
            w.hide();
            
            loop.quit();
        });
        QObject::connect(&w, &MainWindow::windowClosed, &loop, &QEventLoop::quit);

        app.setQuitOnLastWindowClosed(false);
        w.show();
        Logger::info(QObject::tr("OKIDIA Master hazır"));
        loop.exec();

        // --- Logout Sonrası Temizlik (Eğer Beni Hatırla işaretli değilse) ---
        if (isProd && !isRememberMe) {
            Logger::info(QObject::tr("Geçici oturum sonlandı, kimlik verileri temizleniyor: ") + activeUser.username);
            QFile::remove(clientCert);
            QFile::remove(clientKey);
        }

        // Güvenli çıkış yapıldı
        QSettings finalSettings("okidia", "master");
        finalSettings.setValue("system/safe_exit", true);
        finalSettings.sync();

        if (!wantsLogout) {
            break;
        }

        // Logout yapıldıysa döngü devam eder, kimlik bilgilerini temizle ve SessionManager'ı anonim moda al.
        // clearCredentials() çağrısı ZORUNLU: aksi hâlde onBrokerConnected() içindeki
        // otomatik login mekanizması eski kimlik bilgileriyle login ekranını anında kapatır.
        session->clearCredentials();
        session->connectToBroker("", "", ""); 
        if (!session->isConnected()) {
            session->triggerReconnect();
        } 
    }

    delete session;
    QFile::remove(pidFilePath);
    return 0;
}
// OTA test trigger
