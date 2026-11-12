// common/OkidiaConfig.h
#pragma once

#include <QString>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

/**
 * OKIDIA merkezi yapılandırma ve yol yöneticisi.
 */
class OkidiaConfig {
public:
    static constexpr int DEFAULT_PORT = 11200;

    // ── L3 / VLAN İzolasyonu ve DNS Fallback Tanımları ───────────────────────
    static constexpr const char* DEFAULT_BROKER_HOSTNAME = "okidia-broker";
    static constexpr const char* DEFAULT_BROKER_FQDN     = "broker.okidia.local";

    // ── Saklama ve Arşivleme Süreleri (Retention Policies) ────────────────────
    static constexpr int DEFAULT_LOG_RETENTION_DAYS     = 28;  // Sistem logları (4 hafta)
    static constexpr int DEFAULT_AUDIT_RETENTION_DAYS   = 28;  // Adli denetim canlı penceresi (4 hafta)
    static constexpr int DEFAULT_CHAT_RETENTION_DAYS    = 14;  // Canlı sohbet penceresi (2 hafta)
    static constexpr int DEFAULT_ARCHIVE_RETENTION_DAYS = 365; // Sıkıştırılmış .db.gz arşivleri (1 yıl)
    static constexpr qint64 DEFAULT_MAX_LOG_SIZE_MB     = 10;  // 10 MB log rotasyon sınırı

    static OkidiaConfig& instance() {
        static OkidiaConfig inst;
        return inst;
    }

    // ── Üretim / Geliştirme Modu ──────────────────────────────────────────────
    static bool isProduction() {
#if defined(Q_OS_WIN)
        // Windows'ta Master, Agent ve Broker her zaman profil tabanlı prod modunda çalışır
        return true;
#else
        return QFile::exists("/etc/okidia/config") || QFile::exists("/etc/okidia");
#endif
    }

    // ── Genel Sistem Yolları (Broker ve Agent Servisleri İçin) ─────────────────
    static QString systemBasePath() {
#if defined(Q_OS_WIN)
        QString path = "C:/ProgramData/Okidia";
        QDir().mkpath(path);
        return path;
#else
        return isProduction() ? "/etc/okidia" : QDir::currentPath() + "/../etc";
#endif
    }

    // Geriye dönük uyumluluk
    static QString basePath() {
        return systemBasePath();
    }

    // ── Master Profil & Ağ Bazlı Veritabanı Yolları ──────────────────────────
    static QString masterDbDir(const QString& profileName = "DefaultAdmin", const QString& networkName = QString());
    static QString masterDbPath(const QString& profileName = "DefaultAdmin", const QString& networkName = QString());

    // ── Veritabanı Dizini (Bileşen Bazlı Yalıtılmış) ───────────────────────────
    static QString dbDir() {
        const QString appName = QCoreApplication::applicationName().toLower();
#if defined(Q_OS_WIN)
        if (appName == "master") {
            return masterDbDir();
        } else if (appName.contains("agent") || appName == "tray") {
            QString path = systemBasePath() + "/Agent/db";
            QDir().mkpath(path);
            return path;
        } else {
            // Broker (veya varsayılan servis)
            QString path = systemBasePath() + "/Broker/db";
            QDir().mkpath(path);
            return path;
        }
#else
        if (appName == "master") {
            return masterDbDir();
        }
        if (!isProduction()) {
            QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/okidia/db";
            QDir().mkpath(path);
            return path;
        }
        return "/var/lib/okidia";
#endif
    }

    // ── Sertifika Dizinleri ───────────────────────────────────────────────────
    static QString certDir(const QString& sub = "") {
#if defined(Q_OS_WIN)
        QString base = systemBasePath() + "/certs";
        QDir().mkpath(base);
        if (sub.isEmpty()) return base;
        return base + "/" + sub;
#else
        QString base = isProduction() ? "/etc/okidia/certs" : QDir::currentPath() + "/../certs/test";
        if (sub.isEmpty()) return base;
        return base + "/" + sub;
#endif
    }

    static QString agentCertDir() {
#if defined(Q_OS_WIN)
        QString newPath = systemBasePath() + "/Agent/certs";
        QDir().mkpath(newPath);
        // Otomatik geriye dönük uyumluluk ve geçiş:
        if (!QFile::exists(newPath + "/agent.crt")) {
            QStringList oldPaths = {
                systemBasePath() + "/certs/agent",
                systemBasePath() + "/certs"
            };
            for (const QString& oldPath : oldPaths) {
                if (QFile::exists(oldPath + "/agent.crt")) {
                    QFile::copy(oldPath + "/agent.crt", newPath + "/agent.crt");
                    QFile::copy(oldPath + "/agent.key", newPath + "/agent.key");
                    QFile::copy(oldPath + "/ca.crt", newPath + "/ca.crt");
                    break;
                } else if (QFile::exists(oldPath + "/client.crt")) {
                    QFile::copy(oldPath + "/client.crt", newPath + "/agent.crt");
                    QFile::copy(oldPath + "/client.key", newPath + "/agent.key");
                    QFile::copy(oldPath + "/ca.crt", newPath + "/ca.crt");
                    break;
                }
            }
        }
        return newPath;
#else
        return certDir("agent");
#endif
    }

    static QString brokerCertDir() {
#if defined(Q_OS_WIN)
        QString newPath = systemBasePath() + "/Broker/certs";
        QDir().mkpath(newPath);
        if (!QFile::exists(newPath + "/broker.crt") && !QFile::exists(newPath + "/server.crt")) {
            QStringList oldPaths = {
                systemBasePath() + "/certs/broker",
                systemBasePath() + "/certs"
            };
            for (const QString& oldPath : oldPaths) {
                if (QFile::exists(oldPath + "/broker.crt")) {
                    QFile::copy(oldPath + "/broker.crt", newPath + "/broker.crt");
                    QFile::copy(oldPath + "/broker.key", newPath + "/broker.key");
                    QFile::copy(oldPath + "/ca.crt", newPath + "/ca.crt");
                    break;
                } else if (QFile::exists(oldPath + "/server.crt")) {
                    QFile::copy(oldPath + "/server.crt", newPath + "/broker.crt");
                    QFile::copy(oldPath + "/server.key", newPath + "/broker.key");
                    QFile::copy(oldPath + "/ca.crt", newPath + "/ca.crt");
                    break;
                }
            }
        }
        return newPath;
#else
        return certDir("broker");
#endif
    }

    static QString masterCertBaseDir() {
#if defined(Q_OS_WIN)
        QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Okidia/Master/certs/Profiles";
        QDir().mkpath(path);
        return path;
#else
        // Kullanıcı yerel dizini (~/.local/share/okidia/certs/master/Profiles)
        QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/okidia/certs/master/Profiles";
        QDir().mkpath(path);
        return path;
#endif
    }

    static QString sanitizeNetworkName(const QString& name) {
        QString safeName;
        for (const QChar& ch : name.trimmed()) {
            if (ch.isLetterOrNumber() || ch == '_' || ch == '-') {
                safeName += ch;
            } else if (ch.isSpace()) {
                safeName += '_';
            }
        }
        return safeName.isEmpty() ? "Network" : safeName;
    }

    static QString networkKey(const QString& schoolName, const QString& caFingerprint = QString()) {
        QString safe = sanitizeNetworkName(schoolName);
        if (caFingerprint.trimmed().isEmpty()) {
            return safe;
        }
        QString cleanFp = caFingerprint;
        cleanFp = cleanFp.remove(':').remove(' ').remove('-').toLower();
        QString shortHash = cleanFp.left(8);
        if (shortHash.isEmpty()) {
            return safe;
        }
        return safe + "_" + shortHash;
    }

    static QString parseNetworkKey(const QString& netKey, QString* outShortHash = nullptr) {
        if (outShortHash) outShortHash->clear();
        int underIdx = netKey.lastIndexOf('_');
        if (underIdx > 0) {
            QString suffix = netKey.mid(underIdx + 1);
            bool isHexHash = (suffix.length() == 8);
            if (isHexHash) {
                for (const QChar& c : suffix) {
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                        isHexHash = false;
                        break;
                    }
                }
            }
            if (isHexHash) {
                if (outShortHash) *outShortHash = suffix.toLower();
                QString name = netKey.left(underIdx);
                name.replace('_', ' ');
                return name.trimmed();
            }
        }
        QString name = netKey;
        name.replace('_', ' ');
        return name.trimmed();
    }

    static bool isValidNetworkDir(const QString& dirPath, const QString& netKey) {
        if (netKey.isEmpty() || netKey == "Network" || netKey == "DefaultNetwork") {
            return false;
        }
        return QFile::exists(dirPath + "/ca.crt") || QFile::exists(dirPath + "/master.crt");
    }

    static void cleanupGhostNetworks(const QString& activeOpenDbPath = QString()) {
        QDir baseDir(masterCertBaseDir());
        if (!baseDir.exists()) return;
        const QString cleanDbPath = QDir::cleanPath(activeOpenDbPath);

        for (const QString& userDir : baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir netDir(baseDir.filePath(userDir + "/Networks"));
            if (!netDir.exists()) continue;
            for (const QString& netKey : netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString fullPath = QDir::cleanPath(netDir.filePath(netKey));
                if (!isValidNetworkDir(fullPath, netKey)) {
                    if (!cleanDbPath.isEmpty() && cleanDbPath.startsWith(fullPath, Qt::CaseInsensitive)) {
                        continue; // Açık olan DB dosyasını tutan dizini kapatılana kadar atla
                    }
                    QDir(fullPath).removeRecursively();
                }
            }
        }
    }

    static bool removeNetworkProfile(const QString& netKey) {
        if (netKey.trimmed().isEmpty()) return false;
        QDir baseDir(masterCertBaseDir());
        if (!baseDir.exists()) return false;
        bool anyRemoved = false;
        for (const QString& userDir : baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString targetPath = baseDir.filePath(userDir + "/Networks/" + netKey);
            QDir d(targetPath);
            if (d.exists()) {
                if (d.removeRecursively()) {
                    anyRemoved = true;
                }
            }
        }
        return anyRemoved;
    }

    static QString currentNetworkKey() {
        QSettings s("okidia", "master");
        QString key = s.value("net/activeNetworkKey", "").toString().trimmed();
        if (!key.isEmpty() && key != "Network" && key != "DefaultNetwork") return key;

        QString school = s.value("school/name", "").toString().trimmed();
        QString cafp = s.value("net/activeCaFingerprint", "").toString().trimmed();
        if (!school.isEmpty() && school != "Network" && school != "DefaultNetwork") {
            QString computed = networkKey(school, cafp);
            QDir netDir(masterCertBaseDir() + "/DefaultAdmin/Networks");
            if (netDir.exists() && !netDir.exists(computed)) {
                for (const QString& d : netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    if (d.startsWith(computed + "_", Qt::CaseInsensitive) && isValidNetworkDir(netDir.filePath(d), d)) {
                        return d;
                    }
                }
            }
            return computed;
        }

        QDir baseDir(masterCertBaseDir());
        if (baseDir.exists()) {
            for (const QString& userDir : baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir netDir(baseDir.filePath(userDir + "/Networks"));
                if (!netDir.exists()) continue;
                for (const QString& d : netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    if (isValidNetworkDir(netDir.filePath(d), d)) {
                        return d;
                    }
                }
            }
        }
        return s.value("net/activeNetwork", "Network").toString();
    }

    static QString currentNetworkName() {
        QSettings s("okidia", "master");
        QString school = s.value("school/name", "").toString().trimmed();
        if (!school.isEmpty() && school != "Network" && school != "DefaultNetwork") return school;
        QString key = currentNetworkKey();
        if (!key.isEmpty() && key != "Network" && key != "DefaultNetwork") {
            return parseNetworkKey(key);
        }
        return QString();
    }

    static void migrateLegacyDefaultNetwork(const QString& profileName, const QString& targetNetworkKey) {
        if (targetNetworkKey.isEmpty() || targetNetworkKey == "DefaultNetwork" || targetNetworkKey == "Network") return;
        QString base = masterCertBaseDir() + "/" + profileName + "/Networks/";
        QString legacyDir = base + "DefaultNetwork";
        QString newDir = base + targetNetworkKey;
        if (QDir(legacyDir).exists()) {
            if (!QDir(newDir).exists()) {
                QDir().rename(legacyDir, newDir);
            } else {
                QDir(legacyDir).removeRecursively();
            }
        }
    }

    static QString userCertDir(const QString& profileName = "DefaultAdmin", const QString& networkName = QString()) {
        QString net = networkName.isEmpty() ? currentNetworkKey() : networkName;
        migrateLegacyDefaultNetwork(profileName, net);

        QString path = masterCertBaseDir() + "/" + profileName + "/Networks/" + net;
        QDir().mkpath(path);
        return path;
    }

    static QString userCaPath(const QString& profileName = "DefaultAdmin", const QString& networkName = QString()) {
        return userCertDir(profileName, networkName) + "/ca.crt";
    }

    static QString userCertPath(const QString& profileName = "DefaultAdmin", const QString& networkName = QString()) {
        return userCertDir(profileName, networkName) + "/master.crt";
    }

    static QString userKeyPath(const QString& profileName = "DefaultAdmin", const QString& networkName = QString()) {
        return userCertDir(profileName, networkName) + "/master.key";
    }

    // ── Varlık (Asset: Logo, Zil vb.) Depolama Dizini ─────────────────────────
    static QString assetsDir() {
#if defined(Q_OS_WIN)
        QString path = systemBasePath() + "/assets";
        QDir().mkpath(path);
        return path;
#else
        if (isProduction()) {
            QDir().mkpath("/var/lib/okidia/assets");
            return "/var/lib/okidia/assets";
        }
        QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/okidia/assets";
        QDir().mkpath(path);
        return path;
#endif
    }

    static QString schoolLogoPath() {
        return assetsDir() + "/school_logo.png";
    }

    static QString bellStudentAssetPath() {
        return assetsDir() + "/bell_student.mp3";
    }

    static QString bellTeacherAssetPath() {
        return assetsDir() + "/bell_teacher.mp3";
    }

    static QString bellRecessAssetPath() {
        return assetsDir() + "/bell_recess.mp3";
    }

    // ── Ders & Zil Saatleri Modeli ──────────────────────────────────────────
    struct LessonPeriod {
        int     lessonNumber = 1;
        QString studentBell  = "08:50";
        QString teacherBell  = "08:52";
        QString recessBell   = "09:30";
    };

    static int schoolEducationType() {
        QSettings s("okidia", "school");
        return s.value("education_type", 0).toInt(); // 0 = Tam Gün (Normal), 1 = İkili Eğitim
    }

    static int schoolDailyLessonCount() {
        QSettings s("okidia", "school");
        return s.value("daily_lesson_count", 8).toInt();
    }

    static bool isAutoShutdownEnabled() {
        QSettings s("okidia", "school");
        return s.value("power/auto_shutdown_enabled", true).toBool();
    }

    static QString autoShutdownTime() {
        QSettings s("okidia", "school");
        return s.value("power/auto_shutdown_time", "17:30").toString();
    }

    static bool autoShutdownSmartboards() {
        QSettings s("okidia", "school");
        return s.value("power/shutdown_smartboards", true).toBool();
    }

    static bool autoShutdownStudentPCs() {
        QSettings s("okidia", "school");
        return s.value("power/shutdown_student_pcs", true).toBool();
    }

    static QList<LessonPeriod> defaultSchoolSchedule(int lessonCount = 8, int educationType = 0) {
        if (lessonCount <= 0) lessonCount = 8;

        if (educationType == 1) {
            // İkili Eğitim: lessonCount = Bir devredeki ders saati sayısı (örn. 6, 7 veya 8)
            // Toplam 2 * lessonCount ders üretilir (Sabahçı Grubu + Öğlenci Grubu)
            QList<LessonPeriod> list;
            QTime curTime(7, 30); // Sabahçı grubu başlama saati 07:30

            // 1. Sabahçı Grubu
            for (int i = 1; i <= lessonCount; ++i) {
                QTime studentBell = curTime.addSecs(-120); // 2 dk önce öğrenci zili
                QTime teacherBell = curTime;               // Ders başlama zili
                QTime recessBell  = curTime.addSecs(40 * 60); // 40 dk ders
                list.append({i, studentBell.toString("HH:mm"), teacherBell.toString("HH:mm"), recessBell.toString("HH:mm")});
                curTime = recessBell.addSecs(10 * 60); // 10 dk teneffüs
            }

            // Sabah ve öğle grubu arası devir/temizlik boşluğu (30 dk)
            curTime = curTime.addSecs(20 * 60);

            // 2. Öğlenci Grubu
            for (int i = 1; i <= lessonCount; ++i) {
                int lessonNum = lessonCount + i;
                QTime studentBell = curTime.addSecs(-120);
                QTime teacherBell = curTime;
                QTime recessBell  = curTime.addSecs(40 * 60);
                list.append({lessonNum, studentBell.toString("HH:mm"), teacherBell.toString("HH:mm"), recessBell.toString("HH:mm")});
                curTime = recessBell.addSecs(10 * 60);
            }
            return list;
        }

        // Tam Gün (Normal Eğitim: 1..lessonCount)
        QList<LessonPeriod> list;
        QTime curTime(8, 50); // İlk ders 08:50
        for (int i = 1; i <= lessonCount; ++i) {
            QTime studentBell = curTime.addSecs(-120);
            QTime teacherBell = curTime;
            QTime recessBell  = curTime.addSecs(40 * 60);
            list.append({i, studentBell.toString("HH:mm"), teacherBell.toString("HH:mm"), recessBell.toString("HH:mm")});

            if (i == 4) {
                // 4. dersten sonra 50 dk öğle arası (12:00 - 12:50)
                curTime = recessBell.addSecs(50 * 60);
            } else {
                curTime = recessBell.addSecs(10 * 60); // 10 dk teneffüs
            }
        }
        return list;
    }

    static QString agentAlias() {
        QSettings s("okidia", "agent");
        return s.value("identity/alias", "").toString();
    }

    static void setAgentAlias(const QString& alias) {
        QSettings s("okidia", "agent");
        s.setValue("identity/alias", alias);
    }

    static bool isBellSmartboardEnabled() {
        QSettings s("okidia", "school");
        return s.value("bell/smartboard_enabled", false).toBool();
    }

    static bool isBellStudentPcEnabled() {
        QSettings s("okidia", "school");
        return s.value("bell/student_pc_enabled", false).toBool();
    }

    static bool isBellEnabled() {
        return isBellSmartboardEnabled() || isBellStudentPcEnabled();
    }

    static bool isBellAutoLockRecess() {
        QSettings s("okidia", "school");
        return s.value("bell/auto_lock_recess", false).toBool();
    }

    static bool isBellAutoUnlockTeacher() {
        QSettings s("okidia", "school");
        return s.value("bell/auto_unlock_teacher", false).toBool();
    }

    static bool isRecessLogoutStudentPCs() {
        QSettings s("okidia", "school");
        return s.value("bell/recess_logout_student_pcs", false).toBool();
    }

    static bool isDisableSwitchUserStudentPCs() {
        QSettings s("okidia", "school");
        return s.value("policy/disable_switch_user_student_pcs", false).toBool();
    }

    static bool isUsbProtectionEnabled() {
        QSettings s("okidia", "school");
        return s.value("policy/usb_protection_enabled", false).toBool();
    }

    static int bellGracePeriodSec() {
        QSettings s("okidia", "school");
        return s.value("bell/grace_period_sec", 30).toInt();
    }

    // 0 = Akıllı Hibrit (Klasik Perdede Kilitleme, QR/PIN'de Kilitle - Varsayılan)
    // 1 = Kurs / Etüt Serbest Modu (Son Zilde Asla Kilitleme)
    // 2 = Her Zaman Kilitle (Son Zilde de Normal Teneffüs Gibi Kilitle)
    static int lastBellLockBehavior() {
        QSettings s("okidia", "school");
        return s.value("bell/last_bell_lock_behavior", 0).toInt();
    }

    static void setLastBellLockBehavior(int mode) {
        QSettings s("okidia", "school");
        s.setValue("bell/last_bell_lock_behavior", mode);
        s.sync();
    }

    static QList<LessonPeriod> schoolSchedule() {
        QSettings s("okidia", "school");
        if (!s.contains("schedule/count")) {
            return defaultSchoolSchedule(schoolDailyLessonCount(), schoolEducationType());
        }
        int count = s.value("schedule/count", 0).toInt();
        if (count <= 0) return defaultSchoolSchedule(schoolDailyLessonCount(), schoolEducationType());

        QList<LessonPeriod> list;
        for (int i = 0; i < count; ++i) {
            LessonPeriod p;
            p.lessonNumber = s.value(QString("schedule/%1/lesson").arg(i), i + 1).toInt();
            p.studentBell  = s.value(QString("schedule/%1/student").arg(i), "08:48").toString();
            p.teacherBell  = s.value(QString("schedule/%1/teacher").arg(i), "08:50").toString();
            p.recessBell   = s.value(QString("schedule/%1/recess").arg(i), "09:30").toString();
            list.append(p);
        }
        return list;
    }

    static bool saveSchoolSchedule(const QList<LessonPeriod>& schedule,
                                   bool bellSmartboardEnabled,
                                   bool bellStudentPcEnabled,
                                   bool autoLockRecess,
                                   bool autoUnlockTeacher = false,
                                   bool recessLogoutStudentPCs = false,
                                   bool disableSwitchUserStudentPCs = false,
                                   bool usbProtectionEnabled = false,
                                   int gracePeriodSec = 30,
                                   int lastBellBehavior = 0) {
        QSettings s("okidia", "school");
        s.setValue("bell/smartboard_enabled", bellSmartboardEnabled);
        s.setValue("bell/student_pc_enabled", bellStudentPcEnabled);
        s.setValue("bell/enabled", bellSmartboardEnabled || bellStudentPcEnabled);
        s.setValue("bell/auto_lock_recess", autoLockRecess);
        s.setValue("bell/auto_unlock_teacher", autoUnlockTeacher);
        s.setValue("bell/recess_logout_student_pcs", recessLogoutStudentPCs);
        s.setValue("policy/disable_switch_user_student_pcs", disableSwitchUserStudentPCs);
        s.setValue("policy/usb_protection_enabled", usbProtectionEnabled);
        s.setValue("bell/grace_period_sec", gracePeriodSec);
        s.setValue("bell/last_bell_lock_behavior", lastBellBehavior);

        s.setValue("schedule/count", schedule.size());
        for (int i = 0; i < schedule.size(); ++i) {
            s.setValue(QString("schedule/%1/lesson").arg(i), schedule[i].lessonNumber);
            s.setValue(QString("schedule/%1/student").arg(i), schedule[i].studentBell);
            s.setValue(QString("schedule/%1/teacher").arg(i), schedule[i].teacherBell);
            s.setValue(QString("schedule/%1/recess").arg(i), schedule[i].recessBell);
        }
        s.sync();
        return (s.status() == QSettings::NoError);
    }

    static bool saveSchoolPowerAndType(int educationType, int dailyLessonCount, bool autoShutdown, const QString& shutdownTime, bool shutdownSmartboards, bool shutdownStudentPCs) {
        QSettings s("okidia", "school");
        s.setValue("education_type", educationType);
        s.setValue("daily_lesson_count", dailyLessonCount);
        s.setValue("power/auto_shutdown_enabled", autoShutdown);
        s.setValue("power/auto_shutdown_time", shutdownTime.trimmed());
        s.setValue("power/shutdown_smartboards", shutdownSmartboards);
        s.setValue("power/shutdown_student_pcs", shutdownStudentPCs);
        s.sync();
        return (s.status() == QSettings::NoError);
    }

    static uint32_t smartboardLockPolicy() {
        QSettings s("okidia", "school");
        return s.value("smartboard/lockPolicy", 1).toUInt(); // Varsayılan 1 (Klasik Perde)
    }

    static void setSmartboardLockPolicy(uint32_t policy) {
        QSettings s("okidia", "school");
        s.setValue("smartboard/lockPolicy", policy);
        s.sync();
    }

    static QString exportSchoolConfigJson() {
        QSettings s("okidia", "school");
        QJsonObject root;
        root["bell_smartboard_enabled"] = isBellSmartboardEnabled();
        root["bell_student_pc_enabled"] = isBellStudentPcEnabled();
        root["bell_auto_lock_recess"]   = isBellAutoLockRecess();
        root["bell_auto_unlock_teacher"] = isBellAutoUnlockTeacher();
        root["recess_logout_student_pcs"] = isRecessLogoutStudentPCs();
        root["disable_switch_user_student_pcs"] = isDisableSwitchUserStudentPCs();
        root["usb_protection_enabled"] = isUsbProtectionEnabled();
        root["bell_grace_period_sec"]   = bellGracePeriodSec();
        root["last_bell_lock_behavior"] = lastBellLockBehavior();
        root["education_type"]          = schoolEducationType();
        root["daily_lesson_count"]      = schoolDailyLessonCount();
        root["auto_shutdown_enabled"]   = isAutoShutdownEnabled();
        root["auto_shutdown_time"]      = autoShutdownTime();
        root["shutdown_smartboards"]    = autoShutdownSmartboards();
        root["shutdown_student_pcs"]    = autoShutdownStudentPCs();
        root["smartboard_lock_policy"]  = static_cast<int>(smartboardLockPolicy());

        QJsonArray schedArr;
        const auto schedule = schoolSchedule();
        for (const auto& p : schedule) {
            QJsonObject item;
            item["lesson"]  = p.lessonNumber;
            item["student"] = p.studentBell;
            item["teacher"] = p.teacherBell;
            item["recess"]  = p.recessBell;
            schedArr.append(item);
        }
        root["schedule"] = schedArr;

        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    static bool importSchoolConfigJson(const QString& jsonStr) {
        if (jsonStr.trimmed().isEmpty()) return false;
        QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
        if (!doc.isObject()) return false;

        QJsonObject root = doc.object();
        QSettings s("okidia", "school");

        if (root.contains("bell_smartboard_enabled")) s.setValue("bell/smartboard_enabled", root["bell_smartboard_enabled"].toBool());
        if (root.contains("bell_student_pc_enabled"))  s.setValue("bell/student_pc_enabled",  root["bell_student_pc_enabled"].toBool());
        bool bellEnabled = s.value("bell/smartboard_enabled", false).toBool() || s.value("bell/student_pc_enabled", false).toBool();
        s.setValue("bell/enabled", bellEnabled);

        if (root.contains("bell_auto_lock_recess"))   s.setValue("bell/auto_lock_recess",   root["bell_auto_lock_recess"].toBool());
        if (root.contains("bell_auto_unlock_teacher")) s.setValue("bell/auto_unlock_teacher", root["bell_auto_unlock_teacher"].toBool());
        if (root.contains("recess_logout_student_pcs")) s.setValue("bell/recess_logout_student_pcs", root["recess_logout_student_pcs"].toBool());
        if (root.contains("disable_switch_user_student_pcs")) s.setValue("policy/disable_switch_user_student_pcs", root["disable_switch_user_student_pcs"].toBool());
        if (root.contains("usb_protection_enabled")) s.setValue("policy/usb_protection_enabled", root["usb_protection_enabled"].toBool());
        if (root.contains("bell_grace_period_sec"))   s.setValue("bell/grace_period_sec",   root["bell_grace_period_sec"].toInt(30));
        if (root.contains("last_bell_lock_behavior")) s.setValue("bell/last_bell_lock_behavior", root["last_bell_lock_behavior"].toInt(0));
        if (root.contains("education_type"))          s.setValue("education_type",          root["education_type"].toInt(0));
        if (root.contains("daily_lesson_count"))      s.setValue("daily_lesson_count",      root["daily_lesson_count"].toInt(8));
        if (root.contains("auto_shutdown_enabled"))   s.setValue("power/auto_shutdown_enabled", root["auto_shutdown_enabled"].toBool());
        if (root.contains("auto_shutdown_time"))      s.setValue("power/auto_shutdown_time",    root["auto_shutdown_time"].toString());
        if (root.contains("shutdown_smartboards"))    s.setValue("power/shutdown_smartboards",  root["shutdown_smartboards"].toBool());
        if (root.contains("shutdown_student_pcs"))    s.setValue("power/shutdown_student_pcs",  root["shutdown_student_pcs"].toBool());
        if (root.contains("smartboard_lock_policy"))  s.setValue("smartboard/lockPolicy",       root["smartboard_lock_policy"].toInt(1));

        if (root.contains("schedule") && root["schedule"].isArray()) {
            QJsonArray schedArr = root["schedule"].toArray();
            s.setValue("schedule/count", schedArr.size());
            for (int i = 0; i < schedArr.size(); ++i) {
                QJsonObject item = schedArr[i].toObject();
                s.setValue(QString("schedule/%1/lesson").arg(i),  item["lesson"].toInt(i + 1));
                s.setValue(QString("schedule/%1/student").arg(i), item["student"].toString("08:48"));
                s.setValue(QString("schedule/%1/teacher").arg(i), item["teacher"].toString("08:50"));
                s.setValue(QString("schedule/%1/recess").arg(i),  item["recess"].toString("09:30"));
            }
        }
        s.sync();
        return (s.status() == QSettings::NoError);
    }

    // ── Çalışma / Soket Dizini ────────────────────────────────────────────────
    static QString runDir() {
        const QString appName = QCoreApplication::applicationName().toLower();
#if defined(Q_OS_WIN)
        if (appName == "master") {
            QString path = QDir::tempPath() + "/Okidia/Master/run";
            QDir().mkpath(path);
            return path;
        } else if (appName.contains("agent") || appName == "tray") {
            QString path = systemBasePath() + "/Agent/run";
            QDir().mkpath(path);
            return path;
        } else {
            QString path = systemBasePath() + "/Broker/run";
            QDir().mkpath(path);
            return path;
        }
#else
        if (isProduction()) return "/run/okidia";
        QString path = QDir::tempPath() + "/okidia";
        QDir().mkpath(path);
        return path;
#endif
    }

    // ── Log Dizini ────────────────────────────────────────────────────────────
    static QString logDir() {
        const QString appName = QCoreApplication::applicationName().toLower();
#if defined(Q_OS_WIN)
        if (appName == "master") {
            QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Okidia/Master/logs";
            QDir().mkpath(path);
            return path;
        } else if (appName.contains("agent") || appName == "tray") {
            QString path = systemBasePath() + "/Agent/logs";
            QDir().mkpath(path);
            return path;
        } else {
            QString path = systemBasePath() + "/Broker/logs";
            QDir().mkpath(path);
            return path;
        }
#else
        if (isProduction()) {
            if (appName.contains("broker") || appName.contains("agent")) {
                QDir().mkpath("/var/log/okidia");
                return "/var/log/okidia";
            }
        }
        QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/okidia/logs";
        QDir().mkpath(path);
        return path;
#endif
    }

    // ── Yerel Kararlı Sürüm (Rollback) Arşiv Dizini ────────────────────────────
    static QString rollbackCacheDir() {
#if defined(Q_OS_WIN)
        QString path = systemBasePath() + "/Agent/rollback";
        QDir().mkpath(path);
        return path;
#else
        QString path = isProduction() ? "/var/lib/okidia/rollback"
                                      : (QDir::tempPath() + "/okidia_rollback");
        QDir().mkpath(path);
        return path;
#endif
    }


    // ── Ayarlar ──────────────────────────────────────────────────────────────
    QString brokerIp() const { return m_brokerIp; }
    QString brokerName() const { return m_brokerName.isEmpty() ? "OKIDIA-Broker" : m_brokerName; }
    int brokerPort() const { return m_brokerPort; }
    int agentPort()  const { return m_agentPort;  }

    int logRetentionDays()     const { return m_logRetentionDays; }
    int auditRetentionDays()   const { return m_auditRetentionDays; }
    int chatRetentionDays()    const { return m_chatRetentionDays; }
    int archiveRetentionDays() const { return m_archiveRetentionDays; }

    static bool saveBrokerPort(int port) {
        QString path = isProduction() ? (systemBasePath() + "/config")
                                      : (QDir::homePath() + "/.config/okidia/config");
        QFileInfo fi(path);
        QDir().mkpath(fi.dir().absolutePath());

        QSettings s(path, QSettings::IniFormat);
        s.setValue("network/broker_port", port);
        s.sync();
        instance().reload();
        return (s.status() == QSettings::NoError);
    }

    static bool saveBrokerName(const QString& name) {
        QString path = isProduction() ? (systemBasePath() + "/config")
                                      : (QDir::homePath() + "/.config/okidia/config");
        QFileInfo fi(path);
        QDir().mkpath(fi.dir().absolutePath());

        QSettings s(path, QSettings::IniFormat);
        s.setValue("network/broker_name", name.trimmed());
        s.sync();
        instance().reload();
        return (s.status() == QSettings::NoError);
    }

    static bool saveBrokerConfig(const QString& name, int port) {
        QString path = isProduction() ? (systemBasePath() + "/config")
                                      : (QDir::homePath() + "/.config/okidia/config");
        QFileInfo fi(path);
        QDir().mkpath(fi.dir().absolutePath());

        QSettings s(path, QSettings::IniFormat);
        if (!name.trimmed().isEmpty()) {
            s.setValue("network/broker_name", name.trimmed());
        }
        if (port > 0) {
            s.setValue("network/broker_port", port);
        }
        s.sync();
        instance().reload();
        return (s.status() == QSettings::NoError);
    }

    static bool saveRetentionConfig(int logDays, int auditDays, int chatDays, int archiveDays) {
        QString path = isProduction() ? (systemBasePath() + "/config")
                                      : (QDir::homePath() + "/.config/okidia/config");
        QFileInfo fi(path);
        QDir().mkpath(fi.dir().absolutePath());

        QSettings s(path, QSettings::IniFormat);
        if (logDays > 0)     s.setValue("retention/log_days", logDays);
        if (auditDays > 0)   s.setValue("retention/audit_days", auditDays);
        if (chatDays > 0)    s.setValue("retention/chat_days", chatDays);
        if (archiveDays > 0) s.setValue("retention/archive_days", archiveDays);
        s.sync();
        instance().reload();
        return (s.status() == QSettings::NoError);
    }

    void reload() {
        const QStringList paths = {
            systemBasePath() + "/config",
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Okidia/Master/config",
            QDir::homePath() + "/.config/okidia/config"
        };
        for (const QString& path : paths) {
            if (QFile::exists(path)) {
                QSettings s(path, QSettings::IniFormat);
                m_brokerIp   = s.value("network/broker_ip", "").toString();
                m_brokerName = s.value("network/broker_name", "OKIDIA-Broker").toString();
                m_brokerPort = s.value("network/broker_port", DEFAULT_PORT).toInt();
                m_agentPort  = s.value("network/agent_port",  DEFAULT_PORT).toInt();

                m_logRetentionDays     = s.value("retention/log_days", DEFAULT_LOG_RETENTION_DAYS).toInt();
                m_auditRetentionDays   = s.value("retention/audit_days", DEFAULT_AUDIT_RETENTION_DAYS).toInt();
                m_chatRetentionDays    = s.value("retention/chat_days", DEFAULT_CHAT_RETENTION_DAYS).toInt();
                m_archiveRetentionDays = s.value("retention/archive_days", DEFAULT_ARCHIVE_RETENTION_DAYS).toInt();
            }
        }
    }

private:
    OkidiaConfig() { reload(); }

    QString m_brokerIp;
    QString m_brokerName = "OKIDIA-Broker";
    int m_brokerPort = DEFAULT_PORT;
    int m_agentPort  = DEFAULT_PORT;

    int m_logRetentionDays     = DEFAULT_LOG_RETENTION_DAYS;
    int m_auditRetentionDays   = DEFAULT_AUDIT_RETENTION_DAYS;
    int m_chatRetentionDays    = DEFAULT_CHAT_RETENTION_DAYS;
    int m_archiveRetentionDays = DEFAULT_ARCHIVE_RETENTION_DAYS;
};

inline QString OkidiaConfig::masterDbDir(const QString& profileName, const QString& networkName) {
    QString net = networkName.isEmpty() ? currentNetworkKey() : networkName;
    migrateLegacyDefaultNetwork(profileName, net);
    QString path = userCertDir(profileName, net) + "/db";
    QDir().mkpath(path);
    return path;
}

inline QString OkidiaConfig::masterDbPath(const QString& profileName, const QString& networkName) {
    return masterDbDir(profileName, networkName) + "/okidia.db";
}

#define OKIDIA_PORT (OkidiaConfig::instance().brokerPort())
#define OKIDIA_CONFIG OkidiaConfig::instance()
