// agent/system/SessionSwitchManager.cpp
#include "SessionSwitchManager.h"
#include "SystemInfo.h"
#include "../../common/utils/Logger.h"
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QThread>

SessionSwitchManager& SessionSwitchManager::instance() {
    static SessionSwitchManager s_instance;
    return s_instance;
}

SessionSwitchManager::SessionSwitchManager(QObject* parent)
    : QObject(parent) {
    cleanupAutologinConfig();
}

QString SessionSwitchManager::getActiveGuiUser() const {
#if defined(Q_OS_LINUX)
    // 1. loginctl show-seat seat0 -p ActiveSession ile aktif GUI oturumunu bul
    QProcess seatProc;
    seatProc.start("loginctl", {"show-seat", "seat0", "-p", "ActiveSession"});
    if (seatProc.waitForFinished(2000) && seatProc.exitCode() == 0) {
        QString out = QString::fromUtf8(seatProc.readAllStandardOutput()).trimmed();
        if (out.startsWith("ActiveSession=")) {
            QString sessId = out.mid(14).trimmed();
            if (!sessId.isEmpty()) {
                QProcess userProc;
                userProc.start("loginctl", {"show-session", sessId, "-p", "Name"});
                if (userProc.waitForFinished(2000) && userProc.exitCode() == 0) {
                    QString uOut = QString::fromUtf8(userProc.readAllStandardOutput()).trimmed();
                    if (uOut.startsWith("Name=")) {
                        QString user = uOut.mid(5).trimmed();
                        if (!user.isEmpty() && user != "root" && !SystemInfo::isDmUser(user)) {
                            return user;
                        }
                    }
                }
            }
        }
    }

    // 2. Fallback: loginctl list-sessions --no-legend
    QProcess listProc;
    listProc.start("loginctl", {"list-sessions", "--no-legend"});
    if (listProc.waitForFinished(2000) && listProc.exitCode() == 0) {
        QString out = QString::fromUtf8(listProc.readAllStandardOutput());
        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            QStringList parts = line.simplified().split(' ');
            if (parts.size() >= 3) {
                QString user = parts[2];
                if (user != "root" && !SystemInfo::isDmUser(user)) {
                    return user;
                }
            }
        }
    }

    // 3. Fallback: who komutu ile aktif X11 (:0) oturumu
    QProcess whoProc;
    whoProc.start("who", {});
    if (whoProc.waitForFinished(2000) && whoProc.exitCode() == 0) {
        QString out = QString::fromUtf8(whoProc.readAllStandardOutput());
        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            if (line.contains("(:0)") || line.contains("(:1)") || line.contains("tty7")) {
                QString user = line.simplified().split(' ').value(0);
                if (!user.isEmpty()) return user;
            }
        }
    }
#endif
    return QString();
}

bool SessionSwitchManager::ensureSystemUser(const QString& username, const QString& displayName,
                                             const QString& password) {
    if (username.isEmpty()) return false;

#if defined(Q_OS_LINUX)
    // nopasswdlogin grubunun varlığını garantiye al
    QProcess::execute("groupadd", {"-f", "nopasswdlogin"});

    // 1. Kullanıcı zaten var mı kontrol et (id -u <username>)
    QProcess checkProc;
    checkProc.start("id", {"-u", username});
    checkProc.waitForFinished(3000);
    if (checkProc.exitCode() == 0) {
        Logger::info(QObject::tr("SessionSwitchManager: Sistem kullanıcısı zaten mevcut: ") + username +
                     QObject::tr(" (Mevcut parola korunuyor, ezilmez)"));
        // nopasswdlogin ve donanım gruplarında olduğundan emin ol
        QProcess::execute("usermod", {"-aG", "nopasswdlogin,audio,video,plugdev,netdev", username});
        return true;
    }

    // 2. Kullanıcı yoksa oluştur (useradd -m -s /bin/bash -c "Ad Soyad" username)
    QString comment = displayName.isEmpty() ? username : displayName;
    QStringList args = {"-m", "-s", "/bin/bash", "-c", comment, username};

    Logger::info(QObject::tr("SessionSwitchManager: Yeni öğretmen kullanıcısı oluşturuluyor: ") + username);
    QProcess addProc;
    addProc.start("useradd", args);
    addProc.waitForFinished(5000);

    if (addProc.exitCode() == 0) {
        Logger::info(QObject::tr("SessionSwitchManager: Kullanıcı başarıyla oluşturuldu: ") + username);
        // Şifresiz giriş ve donanım izinleri
        QProcess::execute("usermod", {"-aG", "nopasswdlogin,audio,video,plugdev,netdev", username});

        // Eğer öğretmen/yönetici parola belirtmişse ata, belirtilmemişse boş parola ata (PAM kilitlenmesini önlemek için)
        if (!password.isEmpty()) {
            QProcess chProc;
            chProc.start("chpasswd", {});
            chProc.waitForStarted(2000);
            chProc.write(QString("%1:%2\n").arg(username, password).toUtf8());
            chProc.closeWriteChannel();
            chProc.waitForFinished(3000);
            Logger::info(QObject::tr("SessionSwitchManager: Yeni öğretmen için şifre başarıyla atandı."));
        } else {
            // Boş şifre ayarla (LightDM ve PAM şifresiz giriş için)
            QProcess::execute("passwd", {"-d", username});
        }
        return true;
    } else {
        QString err = addProc.readAllStandardError().trimmed();
        Logger::error(QObject::tr("SessionSwitchManager: Kullanıcı oluşturulamadı (%1): %2")
                      .arg(username).arg(err));
        return false;
    }
#else
    Q_UNUSED(displayName);
    Q_UNUSED(password);
    Logger::info(QObject::tr("SessionSwitchManager (Windows): Oturum geçişi simüle edildi: ") + username);
    return true;
#endif
}

bool SessionSwitchManager::switchToUserSession(const QString& username) {
    if (username.isEmpty()) return false;

#if defined(Q_OS_LINUX)
    Logger::info(QObject::tr("SessionSwitchManager: Kullanıcı oturumuna geçiliyor → ") + username);

    // Sistemdeki mevcut aktif GUI kullanıcısını tespit et
    QString currentGuiUser = getActiveGuiUser();
    Logger::info(QObject::tr("SessionSwitchManager: Mevcut aktif GUI kullanıcısı: %1, İstenen öğretmen: %2")
                 .arg(currentGuiUser.isEmpty() ? tr("Bilinmiyor/Kilitli") : currentGuiUser)
                 .arg(username));

    // Eğer o an açık olan kullanıcı zaten bu öğretmense, oturumu kapatmaya gerek yok, masaüstü korunur!
    if (!currentGuiUser.isEmpty() && currentGuiUser == username) {
        Logger::info(QObject::tr("SessionSwitchManager: İstenen öğretmen (%1) zaten aktif oturumda. Masaüstü korunuyor.").arg(username));
        m_activeUser = username;
        return true;
    }

    m_activeUser = username;

    // Sistemdeki aktif Display Manager'ı tespit et (LightDM, GDM3/GDM, SDDM)
    QString activeDm = QStringLiteral("lightdm");
    {
        QFile defDm(QStringLiteral("/etc/X11/default-display-manager"));
        if (defDm.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString line = QString::fromUtf8(defDm.readAll()).trimmed();
            if (line.contains(QStringLiteral("gdm3"))) activeDm = QStringLiteral("gdm3");
            else if (line.contains(QStringLiteral("gdm"))) activeDm = QStringLiteral("gdm");
            else if (line.contains(QStringLiteral("sddm"))) activeDm = QStringLiteral("sddm");
            else if (line.contains(QStringLiteral("lightdm"))) activeDm = QStringLiteral("lightdm");
        } else {
            for (const QString& cand : {QStringLiteral("gdm3"), QStringLiteral("gdm"), QStringLiteral("sddm"), QStringLiteral("lightdm")}) {
                QProcess pAct;
                pAct.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), cand});
                if (pAct.waitForFinished(500) && pAct.exitCode() == 0) {
                    activeDm = cand;
                    break;
                }
            }
        }
    }

    Logger::info(QObject::tr("SessionSwitchManager: Aktif Display Manager tespit edildi → %1").arg(activeDm));

    if (activeDm == QStringLiteral("sddm")) {
        // ── SDDM Autologin ──
        QDir().mkpath(QStringLiteral("/etc/sddm.conf.d"));
        const QString sddmConf = QStringLiteral("/etc/sddm.conf.d/99-okidia-autologin.conf");
        QFile f(sddmConf);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "[Autologin]\n";
            ts << "User=" << username << "\n";
            ts << "Relogin=true\n";
            f.close();
            Logger::info(QObject::tr("SessionSwitchManager: SDDM autologin yapılandırması yazıldı → ") + username);
        }
        if (!currentGuiUser.isEmpty()) {
            forceLogoutUser(currentGuiUser);
            QThread::msleep(400);
        }
        QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("restart"), QStringLiteral("sddm")});
        QProcess::startDetached(QStringLiteral("bash"), {QStringLiteral("-c"), QStringLiteral("sleep 6 && rm -f /etc/sddm.conf.d/99-okidia-autologin.conf")});
        return true;
    }

    if (activeDm == QStringLiteral("gdm3") || activeDm == QStringLiteral("gdm")) {
        // ── GDM3 / GDM Autologin ──
        QString gdmConfPath = QStringLiteral("/etc/gdm3/custom.conf");
        if (!QFile::exists(gdmConfPath) && QFile::exists(QStringLiteral("/etc/gdm/custom.conf"))) {
            gdmConfPath = QStringLiteral("/etc/gdm/custom.conf");
        }
        const QString gdmBakPath = gdmConfPath + QStringLiteral(".okidia.bak");
        if (QFile::exists(gdmConfPath) && !QFile::exists(gdmBakPath)) {
            QFile::copy(gdmConfPath, gdmBakPath);
        }

        QFile f(gdmConfPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "[daemon]\n";
            ts << "AutomaticLoginEnable=True\n";
            ts << "AutomaticLogin=" << username << "\n";
            ts << "TimedLoginEnable=True\n";
            ts << "TimedLogin=" << username << "\n";
            ts << "TimedLoginDelay=0\n";
            f.close();
            Logger::info(QObject::tr("SessionSwitchManager: GDM autologin yapılandırması yazıldı → ") + username);
        }
        if (!currentGuiUser.isEmpty()) {
            forceLogoutUser(currentGuiUser);
            QThread::msleep(400);
        }
        QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("restart"), activeDm});
        const QString restoreCmd = QStringLiteral("sleep 8 && if [ -f '%1' ]; then mv -f '%1' '%2'; fi").arg(gdmBakPath, gdmConfPath);
        QProcess::startDetached(QStringLiteral("bash"), {QStringLiteral("-c"), restoreCmd});
        return true;
    }

    // ── LightDM Autologin (Varsayılan) ──
    QDir().mkpath("/etc/lightdm/lightdm.conf.d");
    QString confPath = "/etc/lightdm/lightdm.conf.d/99-okidia-autologin.conf";
    QFile confFile(confPath);
    if (confFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&confFile);
        ts << "[Seat:*]\n";
        ts << "autologin-user=" << username << "\n";
        ts << "autologin-user-timeout=0\n";
        confFile.close();
        Logger::info(QObject::tr("SessionSwitchManager: LightDM autologin yapılandırması yazıldı → ") + username);
    } else {
        Logger::error(QObject::tr("SessionSwitchManager: LightDM autologin dosyası yazılamadı: ") + confPath);
    }

    if (!currentGuiUser.isEmpty()) {
        Logger::info(QObject::tr("SessionSwitchManager: Önceki oturum (%1) sonlandırılıyor.").arg(currentGuiUser));
        forceLogoutUser(currentGuiUser);
        QThread::msleep(400);
    }

    Logger::info(QObject::tr("SessionSwitchManager: Display Manager yeniden başlatılıyor (systemctl restart lightdm)..."));
    QProcess::startDetached("systemctl", {"restart", "lightdm"});
    QProcess::startDetached("bash", {"-c", "sleep 6 && rm -f /etc/lightdm/lightdm.conf.d/99-okidia-autologin.conf"});

    return true;
#else
    m_activeUser = username;
    Logger::info(QObject::tr("SessionSwitchManager (Windows): Oturum maskesi kaldırıldı, aktif öğretmen: ") + username);
    return true;
#endif
}

bool SessionSwitchManager::terminateCurrentSession() {
#if defined(Q_OS_LINUX)
    // Teneffüste oturumu kapatmıyoruz; RAM'de açık tutarak sadece kilit perdesine alıyoruz
    Logger::info(QObject::tr("SessionSwitchManager: Öğretmen oturumu askıya alındı / kilit perdesine çekildi (kullanıcı: %1).").arg(m_activeUser));
    return true;
#else
    Logger::info(QObject::tr("SessionSwitchManager (Windows): Ekran maskelendi, arka plan oturumu açık tutuluyor."));
    return true;
#endif
}

bool SessionSwitchManager::forceLogoutUser(const QString& username) {
    if (username.isEmpty()) return false;

#if defined(Q_OS_LINUX)
    Logger::info(QObject::tr("SessionSwitchManager: Kullanıcı oturumu zorla kapatılıyor (loginctl terminate-user): ") + username);
    QProcess::execute("loginctl", {"terminate-user", username});
    return true;
#else
    Logger::info(QObject::tr("SessionSwitchManager (Windows): forceLogoutUser simüle edildi: ") + username);
    return true;
#endif
}

void SessionSwitchManager::cleanupAutologinConfig() {
#if defined(Q_OS_LINUX)
    for (const QString& confPath : {
            QStringLiteral("/etc/lightdm/lightdm.conf.d/99-okidia-autologin.conf"),
            QStringLiteral("/etc/sddm.conf.d/99-okidia-autologin.conf") }) {
        if (QFile::exists(confPath)) {
            if (QFile::remove(confPath)) {
                Logger::info(QObject::tr("SessionSwitchManager: Geçici autologin dosyası temizlendi: ") + confPath);
            } else {
                Logger::warning(QObject::tr("SessionSwitchManager: Geçici autologin dosyası silinemedi: ") + confPath);
            }
        }
    }

    for (const QString& gdmConf : {
            QStringLiteral("/etc/gdm3/custom.conf"),
            QStringLiteral("/etc/gdm/custom.conf") }) {
        const QString bak = gdmConf + QStringLiteral(".okidia.bak");
        if (QFile::exists(bak)) {
            QFile::remove(gdmConf);
            if (QFile::rename(bak, gdmConf)) {
                Logger::info(QObject::tr("SessionSwitchManager: Geçici GDM autologin yapılandırması geri yüklendi: ") + gdmConf);
            }
        }
    }
#endif
}

