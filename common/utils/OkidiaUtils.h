// common/utils/OkidiaUtils.h
#pragma once

#include "../version/OkidiaVersion.h"
#include <QString>
#include <QByteArray>
#include <QSysInfo>
#include <QDateTime>
#include <QUuid>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QCollator>

#include <QObject>
#include <QCoreApplication>
#include <QTranslator>
#include <QLibraryInfo>
#include <QLocale>
#include <QTreeWidgetItem>
#include <QTableWidgetItem>
#include <QTableWidget>
#include <QHeaderView>
#include <QSettings>
#include <QTreeWidget>
#include <QMap>
#include <QRegularExpression>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QDialog>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// windows.h → winuser.h, MOUSE_EVENT ve KEY_EVENT'i sayısal makro olarak tanımlar.
// Bu makrolar okidia::MessageType enum değerleriyle çakışır; temizliyoruz.
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif
#else
#  include <sys/stat.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  include <string.h>
#endif

class OkidiaUtils {
public:
    /**
     * Özel anahtar dosyalarının (broker.key, agent.key, ca.key, master.key vb.)
     * izinlerini işletim sistemine uygun olarak sertleştirir.
     * Linux: chmod 0600 (yalnızca sahibi okur/yazar)
     * Windows: NTFS ACL ile inheritance kırılır; servis ise SYSTEM ve Administrators,
     *          Master ise mevcut kullanıcı ve Administrators tam yetkili yapılır, Users/Everyone kaldırılır.
     */
    static bool securePrivateKeyFile(const QString& filePath, bool isService = true) {
        if (filePath.isEmpty() || !QFile::exists(filePath)) {
            return false;
        }

#if defined(Q_OS_WIN)
        // Windows NTFS ACL sertleştirmesi:
        // icacls "filePath" /inheritance:r /grant:r "SYSTEM":F /grant:r "Administrators":F
        QString nativePath = QDir::toNativeSeparators(filePath);
        QString cmd;
        if (isService) {
            cmd = QString("icacls \"%1\" /inheritance:r /grant:r \"*S-1-5-18\":(F) /grant:r \"*S-1-5-32-544\":(F)")
                      .arg(nativePath);
        } else {
            // Master veya kullanıcı oturumu: SYSTEM, Administrators ve mevcut kullanıcı
            cmd = QString("icacls \"%1\" /inheritance:r /grant:r \"*S-1-5-18\":(F) /grant:r \"*S-1-5-32-544\":(F) /grant:r \"%USERNAME%\":(F)")
                      .arg(nativePath);
        }
        int exitCode = QProcess::execute("cmd.exe", {"/c", cmd});
        if (exitCode != 0) {
            // Alternatif doğrudan icacls çağrısı
            QStringList args;
            args << nativePath << "/inheritance:r" << "/grant:r" << "SYSTEM:(F)" << "/grant:r" << "Administrators:(F)";
            if (!isService) {
                QString curUser = qgetenv("USERNAME");
                if (!curUser.isEmpty()) {
                    args << "/grant:r" << (curUser + ":(F)");
                }
            }
            exitCode = QProcess::execute("icacls.exe", args);
        }
        return (exitCode == 0);
#else
        // POSIX / Linux: 0600 (-rw-------)
        bool qfOk = QFile::setPermissions(filePath, QFile::ReadOwner | QFile::WriteOwner);
        int posixOk = ::chmod(filePath.toLocal8Bit().constData(), S_IRUSR | S_IWUSR);
        return (qfOk && posixOk == 0);
#endif
    }

    /**
     * Özel anahtar veya sertifika dosyasının SHA-256 hash bütünlüğünü doğrular veya hash değerini üretir.
     */
    static QString computeFileSha256(const QString& filePath) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&f)) {
            return hash.result().toHex();
        }
        return QString();
    }

    /**
     * Anahtar dosyasının bütünlüğünü doğrular (boş veya bozuk olmadığını kontrol eder,
     * beklenen hash verilmişse karşılaştırır).
     */
    static bool verifyKeyIntegrity(const QString& keyPath, const QString& expectedSha256 = QString()) {
        QFile f(keyPath);
        if (!f.exists() || f.size() < 64) {
            return false;
        }
        QString currentHash = computeFileSha256(keyPath);
        if (currentHash.isEmpty()) {
            return false;
        }
        if (!expectedSha256.isEmpty()) {
            return (currentHash.compare(expectedSha256.trimmed(), Qt::CaseInsensitive) == 0);
        }
        return true;
    }

    /**
     * Bellekteki hassas veriyi (parola, simetrik anahtar vb.) güvenli şekilde sıfırlar.
     * Derleyicinin "dead-store" optimizasyonuyla sıfırlamayı atlamasını engeller.
     */
    static void secureZeroMemory(void* ptr, size_t size) {
        if (!ptr || size == 0) return;
#if defined(Q_OS_WIN)
        SecureZeroMemory(ptr, size);
#else
#  if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
        explicit_bzero(ptr, size);
#  else
        volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
        while (size--) *p++ = 0;
#  endif
#endif
    }

    /**
     * QByteArray içeriğindeki hassas baytları sıfırlar ve ardından temizler.
     */
    static void secureWipe(QByteArray& data) {
        if (data.isEmpty()) return;
        secureZeroMemory(data.data(), static_cast<size_t>(data.size()));
        data.clear();
    }

    /**
     * QString içeriğindeki hassas karakterleri sıfırlar ve ardından temizler.
     * str.data() çağrısı paylaşılan (COW) tamponu ayırır (detach),
     * böylece aynı veriyi referans alan diğer QString kopyalarının belleği bozulmaz.
     */
    static void secureWipe(QString& str) {
        if (str.isEmpty()) return;
        secureZeroMemory(reinterpret_cast<void*>(str.data()),
                         static_cast<size_t>(str.size() * sizeof(QChar)));
        str.clear();
    }

    /**
     * Bellek sayfasını takas (swap / pagefile) alanına yazılmaktan korur (RAM'e kilitler).
     */
    static bool secureLockMemory(void* ptr, size_t size) {
        if (!ptr || size == 0) return false;
#if defined(Q_OS_WIN)
        return VirtualLock(ptr, size) != 0;
#else
        return ::mlock(ptr, size) == 0;
#endif
    }

    /**
     * Kilitlenmiş bellek sayfasının kilidini kaldırır.
     */
    static void secureUnlockMemory(void* ptr, size_t size) {
        if (!ptr || size == 0) return;
#if defined(Q_OS_WIN)
        VirtualUnlock(ptr, size);
#else
        ::munlock(ptr, size);
#endif
    }

    /**
     * Geçici özel anahtar dosyasını diskten silmeden önce üzerine sıfır yazarak
     * sektör düzeyinde güvenli imha sağlar.
     */
    static bool secureRemoveFile(const QString& filePath) {
        if (filePath.isEmpty() || !QFile::exists(filePath)) return true;

        QFile file(filePath);
        if (file.open(QIODevice::ReadWrite)) {
            qint64 sz = file.size();
            if (sz > 0) {
                QByteArray zeros(static_cast<int>(qMin<qint64>(sz, 65536)), 0);
                qint64 written = 0;
                while (written < sz) {
                    qint64 chunk = qMin<qint64>(sz - written, zeros.size());
                    file.write(zeros.constData(), chunk);
                    written += chunk;
                }
                file.flush();
            }
            file.close();
        }
        return QFile::remove(filePath);
    }

    /**
     * OKIDIA lisans ve açık kaynak atıfları diyalog penceresini açar.
     */
    static void showLicenseDialog(QWidget* parent = nullptr) {
        QDialog dlg(parent);
        dlg.setWindowTitle(QObject::tr("OKIDIA Lisans Sözleşmesi ve Açık Kaynak Atıfları"));
        dlg.resize(680, 520);
        
        auto* lay = new QVBoxLayout(&dlg);
        auto* textEdit = new QTextEdit(&dlg);
        textEdit->setReadOnly(true);
        textEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        
        QString licText;
        const QStringList candidates = {
            "/usr/share/doc/okidia-master/copyright",
            "/usr/share/doc/okidia-agent/copyright",
            "/usr/share/doc/okidia-broker/copyright",
            "LICENSE.txt",
            "../LICENSE.txt",
            "../../LICENSE.txt"
        };
        for (const QString& path : candidates) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                licText = QString::fromUtf8(f.readAll());
                break;
            }
        }
        if (licText.isEmpty()) {
            licText = QObject::tr(
                "OKIDIA — Okul İçi Denetim ve İletişim Ağı\n"
                "Telif Hakkı (c) 2024-2026 OKIDIA (Onur Tamer Ekmenci). Tüm hakları saklıdır.\n\n"
                "Açık Kaynak Kütüphane Lisans Atıfları:\n"
                "1. Qt 6 Framework — GNU LGPLv3 (https://www.qt.io/licensing/)\n"
                "2. OpenSSL Toolkit — Apache 2.0 / OpenSSL License (https://www.openssl.org)\n"
                "3. Google Protocol Buffers — BSD 3-Clause (https://developers.google.com/protocol-buffers)\n"
                "4. libssh2 — BSD 3-Clause (https://www.libssh2.org)\n"
                "5. libjpeg-turbo — BSD-Style / IJG (https://libjpeg-turbo.org)\n"
                "6. SQLite — Public Domain (https://www.sqlite.org)\n"
                "7. Avahi — GNU LGPLv2.1+ (https://www.avahi.org)\n"
            );
        }
        textEdit->setPlainText(licText);
        lay->addWidget(textEdit);
        
        auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
        QObject::connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
        lay->addWidget(btnBox);
        
        dlg.exec();
    }
    /**
     * Uygulama genelinde çok dilli çeviri sistemini (i18n) yükler ve aktif eder.
     * Hem standart Qt bileşenleri (QFileDialog, QLineEdit sağ tık menüleri, MessageBox vb.)
     * hem de Okidia uygulama çevirilerini kurar.
     * @param app Çeviricinin kurulacağı uygulama referansı (QCoreApplication veya türevi).
     * @param explicitLang İsteğe bağlı açık dil kodu ("tr", "en", "0", "1", "2"). Boş veya "auto" bırakılırsa sistem diline bakar.
     * @return Yükleme başarılı olduysa true döner.
     */
    static bool installTranslator(QCoreApplication& app, const QString& explicitLang = QString()) {
        QString langCode = explicitLang.toLower().trimmed();
        static const QStringList supportedLangs = {"tr", "en"};

        if (langCode.isEmpty() || langCode == "auto" || langCode == "system" || langCode == "0" || langCode == "-1") {
            QString sysLocale = QLocale::system().name().toLower();
            QString sysLangPrefix = sysLocale.split('_').first().split('-').first();
            if (supportedLangs.contains(sysLangPrefix)) {
                langCode = sysLangPrefix;
            } else {
                // Sistem dili projede desteklenmiyorsa varsayılan dil: İNGİLİZCE (en)
                langCode = "en";
            }
        } else if (langCode == "1" || langCode == "tr" || langCode.startsWith("tr")) {
            langCode = "tr";
        } else if (langCode == "2" || langCode == "en" || langCode.startsWith("en")) {
            langCode = "en";
        } else {
            if (supportedLangs.contains(langCode)) {
                // geçerli kod
            } else {
                langCode = "en";
            }
        }

        // 1. Qt Standart Bileşen Çeviricisi (QFileDialog, QLineEdit/QTextEdit sağ tık menüsü, MessageBox vb.)
        if (langCode != "en") {
            auto* qtBaseTranslator = new QTranslator(&app);
            QString qtBaseName = QString("qtbase_%1").arg(langCode);
            bool qtBaseLoaded = false;

            // A) Gömülü Qt Kaynakları
            if (qtBaseTranslator->load(QString(":/i18n/") + qtBaseName + ".qm")) {
                qtBaseLoaded = true;
            } else if (qtBaseTranslator->load(QString(":/translations/") + qtBaseName + ".qm")) {
                qtBaseLoaded = true;
            }
            // B) Qt Resmi Çeviri Yolu (QLibraryInfo)
            else if (qtBaseTranslator->load(qtBaseName, QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
                qtBaseLoaded = true;
            }
            // C) Standart Linux Sistem Yolları (/usr/share/qt6/translations ve /usr/share/qt5/translations)
            else if (qtBaseTranslator->load(qtBaseName, "/usr/share/qt6/translations")) {
                qtBaseLoaded = true;
            }
            else if (qtBaseTranslator->load(qtBaseName, "/usr/share/qt5/translations")) {
                qtBaseLoaded = true;
            }
            // D) Çalıştırılabilir dizin yanındaki translations veya /usr/share/okidia/translations
            else if (qtBaseTranslator->load(qtBaseName, QCoreApplication::applicationDirPath() + "/translations")) {
                qtBaseLoaded = true;
            }
            else if (qtBaseTranslator->load(qtBaseName, "/usr/share/okidia/translations")) {
                qtBaseLoaded = true;
            }

            if (qtBaseLoaded) {
                app.installTranslator(qtBaseTranslator);
            } else {
                delete qtBaseTranslator;
            }
        }

        // 2. Okidia Uygulama Çeviricisi
        auto* translator = new QTranslator(&app);
        QString qmFileName = QString("okidia_%1.qm").arg(langCode);
        QString baseName = QString("okidia_%1").arg(langCode);

        bool loaded = false;
        // A) Gömülü Qt Kaynakları (Qt Resource :/i18n/)
        if (translator->load(QString(":/i18n/") + qmFileName)) {
            loaded = true;
        }
        // B) Alternatif Gömülü Kaynak Yolu (:/translations/)
        else if (translator->load(QString(":/translations/") + qmFileName)) {
            loaded = true;
        }
        // C) Çalıştırılabilir dosya yanındaki translations dizini
        else if (translator->load(baseName, QCoreApplication::applicationDirPath() + "/translations")) {
            loaded = true;
        }
        // D) Standart Linux sistem dizini (/usr/share/okidia/translations)
        else if (translator->load(baseName, "/usr/share/okidia/translations")) {
            loaded = true;
        }

        if (loaded) {
            app.installTranslator(translator);
            return true;
        } else {
            delete translator;
            return false;
        }
    }
    /**
     * Doğal sıralama (natural sort) için karşılaştırma yapar.
     * Sayısal değerleri kendi içinde sayı olarak değerlendirerek sıralar (örn. opet-1, opet-2, opet-10).
     */
    static int naturalCompare(const QString& a, const QString& b) {
        thread_local QCollator collator;
        thread_local bool initialized = false;
        if (!initialized) {
            collator.setNumericMode(true);
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            initialized = true;
        }
        return collator.compare(a, b);
    }

    /**
     * İki semantik/sayısal sürüm dizgisini karşılaştırır (örn. "0.1.12" vs "0.1.9").
     * v1 > v2 ise > 0, v1 < v2 ise < 0, eşitse 0 döner.
     */
    static int compareVersions(const QString& v1, const QString& v2) {
        QString cleanV1 = v1.startsWith('v', Qt::CaseInsensitive) ? v1.mid(1) : v1;
        QString cleanV2 = v2.startsWith('v', Qt::CaseInsensitive) ? v2.mid(1) : v2;
        QStringList parts1 = cleanV1.split(QRegularExpression("[^0-9]+"), Qt::SkipEmptyParts);
        QStringList parts2 = cleanV2.split(QRegularExpression("[^0-9]+"), Qt::SkipEmptyParts);
        int maxLen = qMax(parts1.size(), parts2.size());
        for (int i = 0; i < maxLen; ++i) {
            int num1 = (i < parts1.size()) ? parts1[i].toInt() : 0;
            int num2 = (i < parts2.size()) ? parts2[i].toInt() : 0;
            if (num1 != num2) {
                return (num1 > num2) ? 1 : -1;
            }
        }
        return naturalCompare(cleanV1, cleanV2);
    }
    /**
     * Mevcut zamanı saniye cinsinden döner (Unix epoch).
     */
    static qint64 nowSecs() {
        return QDateTime::currentSecsSinceEpoch();
    }

    /**
     * Protobuf timestamp alanları için uint64_t cinsinden epoch saniye döner.
     */
    static uint64_t nowSecsU64() {
        return static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch());
    }

    /**
     * /etc/os-release dosyasını okur ve key-value haritası olarak döner.
     */
    static QMap<QString, QString> readOsRelease() {
        QMap<QString, QString> fields;
        QFile file("/etc/os-release");
        if (!file.exists()) {
            file.setFileName("/usr/lib/os-release");
        }
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                int eq = line.indexOf('=');
                if (eq > 0) {
                    QString key = line.left(eq).trimmed();
                    QString val = line.mid(eq + 1).trimmed();
                    if (val.startsWith('"') && val.endsWith('"')) {
                        val = val.mid(1, val.length() - 2);
                    }
                    fields[key] = val;
                }
            }
            file.close();
        }
        return fields;
    }

    /**
     * /etc/debian_version dosyasını okur ve ilk satırı döner.
     */
    static QString readDebianVersion() {
        QFile debVerFile("/etc/debian_version");
        if (debVerFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&debVerFile);
            QString raw = in.readLine().trimmed();
            debVerFile.close();
            return raw;
        }
        return QString();
    }

    /**
     * İşletim sistemi tespiti için merkezi fonksiyon.
     * packaging/CMakeLists.txt ve DeployPanel ile birebir aynı öncelik sırasını izler.
     */
    static QString parseOsTag(const QMap<QString, QString>& osReleaseFields, const QString& debianVersionRaw) {
        QString cleanOsId = osReleaseFields.value("ID").toLower();
        cleanOsId.remove(QRegularExpression("[^a-z0-9_.-]"));
        QString idLike = osReleaseFields.value("ID_LIKE").toLower();

        QString cleanVer = osReleaseFields.value("VERSION_ID");
        cleanVer.remove(QRegularExpression("[^0-9.]"));
        QString majorVer = cleanVer.split('.').value(0);
        if (majorVer.isEmpty()) majorVer = "0";

        QString uCode = osReleaseFields.value("UBUNTU_CODENAME").toLower();
        QString vCode = osReleaseFields.value("VERSION_CODENAME").toLower();

        QString osTag = "";
        if (uCode == "noble") osTag = "ubuntu24";
        else if (uCode == "jammy") osTag = "ubuntu22";
        else if (uCode == "focal") osTag = "ubuntu20";
        
        if (osTag.isEmpty() && !vCode.isEmpty()) {
            if (vCode == "trixie" || vCode == "yirmibes") osTag = "debian13";
            else if (vCode == "bookworm" || vCode == "yirmiuc") osTag = "debian12";
            else if (vCode == "bullseye" || vCode == "yirmibir") osTag = "debian11";
            else if (vCode == "buster" || vCode == "ondokuz") osTag = "debian10";
            else if (vCode == "noble") osTag = "ubuntu24";
            else if (vCode == "jammy") osTag = "ubuntu22";
            else if (vCode == "focal") osTag = "ubuntu20";
        }

        if (osTag.isEmpty() && !debianVersionRaw.isEmpty()) {
            QString debLower = debianVersionRaw.toLower().trimmed();
            QRegularExpressionMatch match = QRegularExpression("^([0-9]+)").match(debLower);
            if (match.hasMatch()) {
                osTag = "debian" + match.captured(1);
            } else if (debLower.contains("trixie")) {
                osTag = "debian13";
            } else if (debLower.contains("bookworm")) {
                osTag = "debian12";
            } else if (debLower.contains("bullseye")) {
                osTag = "debian11";
            } else if (debLower.contains("buster")) {
                osTag = "debian10";
            }
        }

        if (osTag.isEmpty()) {
            if (cleanOsId.contains("pardus") || idLike.contains("pardus")) {
                if (majorVer == "25" || vCode.contains("yirmibes")) osTag = "debian13";
                else if (majorVer == "23" || vCode.contains("yirmiuc")) osTag = "debian12";
                else if (majorVer == "21" || vCode.contains("yirmibir")) osTag = "debian11";
                else if (majorVer == "19" || vCode.contains("ondokuz")) osTag = "debian10";
                else osTag = "debian12";
            }
            else if (cleanOsId == "ubuntu") osTag = "ubuntu" + majorVer;
            else if (cleanOsId == "debian") osTag = "debian" + majorVer;
            else if (cleanOsId == "arch" || idLike.contains("arch")) osTag = "arch";
            else if (idLike.contains("rhel") || idLike.contains("fedora") || idLike.contains("centos") || idLike.contains("suse")) osTag = "rhel";
            else {
                osTag = "unknown";
            }
        }

        return osTag;
    }

    /**
     * Ham işletim sistemi bilgisini (PRETTY_NAME veya osTag) standart paket osTag formatına normalize eder.
     */
    static QString normalizeOsTag(const QString& rawTagOrInfo) {
        QString s = rawTagOrInfo.toLower().trimmed();
        if (s.isEmpty() || s == "unknown") return QStringLiteral("unknown");

        if (s == "debian13" || s == "debian12" || s == "debian11" || s == "debian10" ||
            s == "ubuntu24" || s == "ubuntu22" || s == "ubuntu20" || s == "windows") {
            return s;
        }

        if (s.contains("win") || s.contains("microsoft")) {
            return QStringLiteral("windows");
        }

        if (s.contains("debian13") || s.contains("trixie") || s.contains("yirmibes") ||
            (s.contains("pardus") && s.contains("25")) ||
            (s.contains("debian") && QRegularExpression("\\b13\\b").match(s).hasMatch())) {
            return QStringLiteral("debian13");
        }
        if (s.contains("debian12") || s.contains("bookworm") || s.contains("yirmiuc") ||
            (s.contains("pardus") && s.contains("23")) ||
            (s.contains("debian") && QRegularExpression("\\b12\\b").match(s).hasMatch())) {
            return QStringLiteral("debian12");
        }
        if (s.contains("debian11") || s.contains("bullseye") || s.contains("yirmibir") ||
            (s.contains("pardus") && s.contains("21")) ||
            (s.contains("debian") && QRegularExpression("\\b11\\b").match(s).hasMatch())) {
            return QStringLiteral("debian11");
        }
        if (s.contains("debian10") || s.contains("buster") || s.contains("ondokuz") ||
            (s.contains("pardus") && s.contains("19")) ||
            (s.contains("debian") && QRegularExpression("\\b10\\b").match(s).hasMatch())) {
            return QStringLiteral("debian10");
        }
        if (s.contains("ubuntu24") || s.contains("noble") ||
            (s.contains("ubuntu") && s.contains("24")) ||
            (s.contains("mint") && s.contains("22"))) {
            return QStringLiteral("ubuntu24");
        }
        if (s.contains("ubuntu22") || s.contains("jammy") ||
            (s.contains("ubuntu") && s.contains("22")) ||
            (s.contains("mint") && s.contains("21"))) {
            return QStringLiteral("ubuntu22");
        }
        if (s.contains("ubuntu20") || s.contains("focal") ||
            (s.contains("ubuntu") && s.contains("20")) ||
            (s.contains("mint") && s.contains("20"))) {
            return QStringLiteral("ubuntu20");
        }

        return s;
    }

    /**
     * Yerel makinenin işletim sistemi paket etiketini (osTag) tespit eder.
     * Windows'ta "windows", Linux'ta /etc/os-release + /etc/debian_version okuyarak üretir.
     */
    static QString detectLocalOsTag() {
#if defined(Q_OS_WIN)
        return QStringLiteral("windows");
#else
        QMap<QString, QString> osRelease = readOsRelease();
        QString debVer = readDebianVersion();
        return parseOsTag(osRelease, debVer);
#endif
    }

    /**
     * Yerel makinenin normalize edilmiş işlemci mimarisini ("amd64", "arm64" vb.) döner.
     */
    static QString detectLocalOsArch() {
        return normalizeArch(QSysInfo::currentCpuArchitecture());
    }

    /**
     * Yerel makinenin kullanıcı dostu işletim sistemi adını döner.
     */
    static QString detectLocalOsPrettyName() {
#if defined(Q_OS_WIN)
        return cleanOsName(QSysInfo::prettyProductName());
#else
        QMap<QString, QString> osRelease = readOsRelease();
        QString pretty = osRelease.value("PRETTY_NAME");
        if (pretty.isEmpty()) pretty = QSysInfo::prettyProductName();
        return cleanOsName(pretty);
#endif
    }

    /**
     * İki işletim sistemi / paket etiketinin ikili (.deb / .exe) paket seviyesinde uyumlu olup olmadığını doğrular.
     * C++ kütüphane (GLIBC / libstdc++ / Qt) ABI uyumluluğu nedeniyle Linux'ta aynı majör osTag zorunludur.
     */
    static bool isOsPackageCompatible(const QString& pkgTag, const QString& systemTag) {
        if (pkgTag.isEmpty() || systemTag.isEmpty()) return false;
        const QString p = normalizeOsTag(pkgTag);
        const QString s = normalizeOsTag(systemTag);
        if (p == "unknown" || s == "unknown") return false;
        if (p == s) return true;

        const bool pWin = (p == "windows" || p.contains("win"));
        const bool sWin = (s == "windows" || s.contains("win"));
        if (pWin && sWin) return true;

        return false;
    }

    /**
     * İşletim sistemi adından sondaki parantezli mimari bilgisini temizler (örn. "Linux Mint 22.3 (x86_64)" -> "Linux Mint 22.3")
     */
    static QString cleanOsName(const QString& osName) {
        QString clean = osName.trimmed();
        // Regex matches trailing (arch) such as (x86_64), (amd64), (arm64), (aarch64), (i386), etc.
        clean.remove(QRegularExpression("\\s*\\((?:x86_64|amd64|arm64|aarch64|i386|i686|armv7l|x86)\\)$", QRegularExpression::CaseInsensitiveOption));
        return clean.trimmed();
    }

    /**
     * Mimariyi standardize eder (x86_64 -> amd64, aarch64 -> arm64)
     */
    static QString normalizeArch(const QString& arch) {
        QString normalized = arch.toLower().trimmed();
        if (normalized == "x86_64") return "amd64";
        if (normalized == "aarch64") return "arm64";
        return normalized;
    }

    /**
     * Paket dosya adından (örn. okidia-agent_debian12_0.1.890_amd64.deb veya okidia-agent-setup-v0.1.890.exe)
     * sürüm numarasını ("0.1.890") çıkarır.
     */
    static QString extractPackageVersion(const QString& fileName) {
        QString cleanName = QFileInfo(fileName).fileName();
        QStringList parts = cleanName.split('_');
        if (parts.size() >= 3) {
            QString candidate = parts[2].trimmed();
            if (candidate.startsWith('v', Qt::CaseInsensitive)) candidate = candidate.mid(1);
            if (QRegularExpression("^\\d+\\.\\d+").match(candidate).hasMatch()) {
                return candidate;
            }
        }
        static const QRegularExpression verRegex(R"(v?(\d+\.\d+\.\d+(?:\.\d+)?))");
        QRegularExpressionMatch match = verRegex.match(cleanName);
        if (match.hasMatch()) {
            return match.captured(1);
        }
        return QString();
    }

    /**
     * OTA paket seçiminde kullanılacak regex deseni
     */
    static QString getAgentPackageRegex(const QString& osTag, const QString& osArch) {
        QString tag = normalizeOsTag(osTag);
        if (tag.contains("win") || tag.contains("windows")) {
            return QString("^(okidia-agent-setup.*\\.exe|okidia-agent.*win.*\\.exe)$");
        }
        QString arch = normalizeArch(osArch);
        return QString("^okidia-agent_%1_.*_%2\\.deb$").arg(tag, arch);
    }

    /**
     * Broker paket seçiminde kullanılacak regex deseni
     */
    static QString getBrokerPackageRegex(const QString& osTag, const QString& osArch) {
        QString tag = normalizeOsTag(osTag);
        if (tag.contains("win") || tag.contains("windows")) {
            return QString("^(okidia-broker-setup.*\\.exe|okidia-broker.*win.*\\.exe)$");
        }
        QString arch = normalizeArch(osArch);
        return QString("^okidia-broker_%1_.*_%2\\.deb$").arg(tag, arch);
    }

    /**
     * Master paket seçiminde kullanılacak regex deseni
     */
    static QString getMasterPackageRegex(const QString& osTag, const QString& osArch) {
        QString tag = normalizeOsTag(osTag);
        if (tag.contains("win") || tag.contains("windows")) {
            return QString("^(okidia-master-setup.*\\.exe|okidia-master.*win.*\\.exe)$");
        }
        QString arch = normalizeArch(osArch);
        return QString("^okidia-master_%1_.*_%2\\.deb$").arg(tag, arch);
    }

    /**
     * Hedef MAC adresine standart Wake-on-LAN Magic Packet (UDP Broadcast Port 9 ve 7) gönderir.
     * Tüm aktif ağ arayüzlerinin Broadcast adreslerine ve genel 255.255.255.255 adresine yayın yapar.
     * @param macStr MAC adresi ("00:11:22:33:44:55", "00-11-22-33-44-55" veya "001122334455")
     * @param targetIp İsteğe bağlı hedef IPv4 adresi
     * @return Magic packet başarıyla oluşturulup sokete verildiyse true döner.
     */
    static bool sendWakeOnLan(const QString& macStr, const QString& targetIp = QString()) {
        QString cleanMac = macStr.trimmed();
        cleanMac = cleanMac.remove(':').remove('-').remove('.');
        if (cleanMac.length() != 12) {
            return false;
        }

        QByteArray macBytes;
        for (int i = 0; i < 12; i += 2) {
            bool byteOk = false;
            uint val = cleanMac.mid(i, 2).toUInt(&byteOk, 16);
            if (!byteOk) return false;
            macBytes.append(static_cast<char>(val));
        }

        if (macBytes.size() != 6) {
            return false;
        }

        // Magic Packet: 6x 0xFF + 16x MAC (toplam 102 byte)
        QByteArray packet;
        packet.fill(static_cast<char>(0xFF), 6);
        for (int i = 0; i < 16; ++i) {
            packet.append(macBytes);
        }

        QUdpSocket socket;

        // 1. Genel Broadcast (255.255.255.255)
        socket.writeDatagram(packet, QHostAddress::Broadcast, 9);
        socket.writeDatagram(packet, QHostAddress::Broadcast, 7);

        // 2. Doğrudan hedef IP (varsa)
        if (!targetIp.isEmpty()) {
            QHostAddress targetAddr(targetIp);
            socket.writeDatagram(packet, targetAddr, 9);
            socket.writeDatagram(packet, targetAddr, 7);
        }

        // 3. Tüm yerel ağ arayüzlerinin Broadcast adresleri
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const auto& iface : interfaces) {
            if (!iface.flags().testFlag(QNetworkInterface::IsUp) || 
                iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const auto& entry : iface.addressEntries()) {
                QHostAddress bcast = entry.broadcast();
                if (!bcast.isNull() && bcast != QHostAddress::Broadcast) {
                    socket.writeDatagram(packet, bcast, 9);
                    socket.writeDatagram(packet, bcast, 7);
                }
            }
        }
        return true;
    }

    /**
     * Timestamp'i kısa saat:dakika formatında döner (sohbet pencereleri için).
     */
    static QString formatShortTime(qint64 epochSecs) {
        return QDateTime::fromSecsSinceEpoch(epochSecs).toString("hh:mm");
    }

    /**
     * Süslü parantez içermeyen temiz bir UUID üretir.
     */
    static QString generateUuid() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    /**
     * Makinenin donanım/işletim sistemi bazlı benzersiz kimliğini döner.
     */
    static QString getMachineId() {
        QByteArray mid = QSysInfo::machineUniqueId();
        if (!mid.isEmpty()) {
            return QString::fromUtf8(mid).trimmed();
        }
        return QUuid::createUuidV5(QUuid(), QSysInfo::machineHostName()).toString(QUuid::WithoutBraces);
    }

    /**
     * Verinin SHA256 özetini hex formatında döner.
     */
    static QString sha256(const QByteArray& data) {
        return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
    }

    /**
     * Dosyanın SHA256 özetini döner.
     */
    static QString fileSha256(const QString& filePath) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) return QString();
        
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&file)) {
            return hash.result().toHex();
        }
        return QString();
    }

    /**
     * Dosya boyutunu insan okuyabileceği formata çevirir (B, KB, MB, GB).
     */
    static QString formatSize(qint64 bytes) {
        double size = static_cast<double>(bytes);
        QStringList units = {"B", "KB", "MB", "GB", "TB"};
        int unitIdx = 0;
        while (size >= 1024 && unitIdx < units.size() - 1) {
            size /= 1024;
            unitIdx++;
        }
        return QString::number(size, 'f', unitIdx > 0 ? 1 : 0) + " " + units[unitIdx];
    }

    /**
     * Tarih ve saati standart OKIDIA formatında döner.
     */
    static QString formatDateTime(qint64 timestamp = 0) {
        if (timestamp == 0) timestamp = nowSecs();
        QDateTime dt = QDateTime::fromSecsSinceEpoch(timestamp);
        return dt.toString("yyyy-MM-dd hh:mm:ss");
    }

    static QString formatDateTime(const QDateTime& dt) {
        return dt.toString("yyyy-MM-dd hh:mm:ss");
    }

    /**
     * Kurtarma kodunu tire, boşluk gibi format karakterlerinden temizleyip standartlaştırır.
     */
    static QString cleanRecoveryKey(const QString& key) {
        return key.trimmed().replace("-", "").replace(" ", "").toUpper();
    }

    /**
     * Kurtarma kodu dosyası için standart metin ve tavsiye notu şablonunu döner.
     */
    static QString getRecoveryKeyFileContent(const QString& key, const QString& titleText) {
        QString content;
        content += "==================================================\n";
        content += titleText + "\n";
        content += "==================================================\n\n";
        content += QObject::tr("Tarih             : ") + QDateTime::currentDateTime().toString("dd.MM.yyyy hh:mm:ss") + "\n";
        content += QObject::tr("Kurtarma Kodu     : ") + key + "\n\n";
        content += QObject::tr("ÖNEMLİ UYARI:\n");
        content += QObject::tr("Yönetici şifrenizi unutursanız, sisteminizi sadece bu kod yardımıyla\n");
        content += QObject::tr("kurtarabilirsiniz. Bu dosyayı güvenli bir yerde saklayınız.\n\n");
        content += QObject::tr("TAVSİYE VE İŞ SÜREKLİLİĞİ NOTU:\n");
        content += QObject::tr("Bu belgenin fiziksel çıktısını alarak (yazdırıp) okul idaresine (okul\n");
        content += QObject::tr("yönetimi / okul müdürlüğü) teslim etmeniz önemle tavsiye edilir.\n");
        content += QObject::tr("Yöneticinin değişmesi, ulaşılamaz olması veya olağanüstü acil durumlar\n");
        content += QObject::tr("karşısında sistem yönetiminin devralınabilmesi ve okul denetim ağının\n");
        content += QObject::tr("işlevsiz kalmaması için bu kodun okulun güvenli fiziksel arşivinde veya\n");
        content += QObject::tr("kasasında saklanması gerekmektedir.\n");
        return content;
    }

    /**
     * Anakart/BIOS Donanım UUID bilgisini çeker.
     * Bulamazsa /etc/machine-id kullanır, o da yoksa fallback üreterek kararlı bir UUID döner.
     */
    static QString hardwareUuid() {
        QString baseSysId;
        
        // 1. DMI UUID oku (Linux root veya yetkili süreçler için)
        QFile dmiFile("/sys/class/dmi/id/product_uuid");
        if (dmiFile.open(QIODevice::ReadOnly)) {
            baseSysId = QString::fromLatin1(dmiFile.readAll()).trimmed();
            dmiFile.close();
        }
        
        // 2. Fallback: machine-id
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
                baseSysId = "okidia-hardware-fallback-id";
            }
        }
        
        QUuid deterministicUuid = QUuid::createUuidV5(QUuid(), baseSysId);
        return deterministicUuid.toString(QUuid::WithoutBraces);
    }

    // =========================================================================
    // İŞLETİM SİSTEMİ & UZAK KOMUT MOTORU (OS & REMOTE COMMAND ABSTRACTION)
    // =========================================================================

    /**
     * Verilen işletim sistemi etiket veya adının Windows olup olmadığını tespit eder.
     */
    static bool isWindows(const QString& osTagOrName) {
        QString s = osTagOrName.toLower().trimmed();
        return s.contains("windows") || s.contains("win10") || s.contains("win11") || s.contains("win32") || s.contains("win64") || s.contains("microsoft");
    }

    /**
     * Kullanıcı adının bir Display Manager (DM / Login ekranı) kullanıcısı olup olmadığını tespit eder.
     */
    static bool isDmUser(const QString& username) {
        static const QSet<QString> dmUsers = {
            "lightdm", "gdm", "gdm3", "sddm", "kdm", "lxdm", "xdm", "slim", "winlogon"
        };
        return dmUsers.contains(username.toLower().trimmed());
    }

    /**
     * İşletim sistemine göre varsayılan kök dizin yolunu döndürür ("C:/" veya "/").
     */
    static QString remoteRootPath(bool isWindows) {
        return isWindows ? "C:/" : "/";
    }

    /**
     * İşletim sistemine göre varsayılan ev/kullanıcılar dizin yolunu döndürür ("C:/Users" veya "/home").
     */
    static QString remoteHomePath(bool isWindows) {
        return isWindows ? "C:/Users" : "/home";
    }

    /**
     * Uzak makinedeki geçici klasör yolunu (ve varsa dosya adını) döndürür.
     */
    static QString remoteTempPath(bool isWindows, const QString& filename = QString()) {
        QString base = isWindows ? "C:/Windows/Temp" : "/tmp";
        if (filename.isEmpty()) return base;
        return base + "/" + filename;
    }

    /**
     * Uzak dosya yollarını temizler, karışık / ve \ karakterlerini işletim sistemine göre standartlaştırır.
     * @param forDisplay true ise Windows için native ters eğik çizgi ('\') formatına dönüştürür.
     */
    static QString normalizeRemotePath(const QString& path, bool isWindows, bool forDisplay = false) {
        QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
        if (isWindows) {
            if (clean.endsWith(':')) clean += "/";
            if (clean.isEmpty()) clean = "C:/";
            if (forDisplay) {
                return QDir::toNativeSeparators(clean);
            }
            return clean;
        } else {
            if (clean.isEmpty()) clean = "/";
            return clean;
        }
    }

    /**
     * Uzak dizin oluşturma komutu üretir.
     */
    static QString remoteMkdirCmd(const QString& path, bool isWindows) {
        if (isWindows) {
            QString native = QDir::toNativeSeparators(path);
            native.replace("\"", "\\\"");
            return QString("cmd.exe /c mkdir \"%1\"").arg(native);
        } else {
            QString esc = path;
            esc.replace("\"", "\\\"");
            return QString("mkdir -p \"%1\"").arg(esc);
        }
    }

    /**
     * Uzak dosya/klasör yeniden adlandırma veya taşıma komutu üretir.
     */
    static QString remoteMoveCmd(const QString& srcPath, const QString& dstPath, bool isWindows) {
        if (isWindows) {
            QString nativeSrc = QDir::toNativeSeparators(srcPath);
            QString nativeDst = QDir::toNativeSeparators(dstPath);
            nativeSrc.replace("\"", "\\\"");
            nativeDst.replace("\"", "\\\"");
            return QString("cmd.exe /c move /Y \"%1\" \"%2\"").arg(nativeSrc, nativeDst);
        } else {
            QString escSrc = srcPath;
            QString escDst = dstPath;
            escSrc.replace("\"", "\\\"");
            escDst.replace("\"", "\\\"");
            return QString("mv \"%1\" \"%2\"").arg(escSrc, escDst);
        }
    }

    /**
     * Uzak dosya veya klasör silme komutu üretir.
     */
    static QString remoteDeleteCmd(const QString& path, bool isDir, bool isWindows) {
        if (isWindows) {
            QString native = QDir::toNativeSeparators(path);
            native.replace("\"", "\\\"");
            return isDir ? QString("cmd.exe /c rmdir /s /q \"%1\"").arg(native)
                         : QString("cmd.exe /c del /f /q \"%1\"").arg(native);
        } else {
            QString esc = path;
            esc.replace("\"", "\\\"");
            return isDir ? QString("rm -rf \"%1\"").arg(esc)
                         : QString("rm -f \"%1\"").arg(esc);
        }
    }

    /**
     * Belirtilen kaynak klasörün içeriğini yerel bir .zip arşivine paketler.
     * Linux'ta zip komutu veya Python3 zipfile, Windows'ta tar.exe veya PowerShell kullanılır.
     */
    static bool createLocalZipArchive(const QString& zipFilePath, const QString& sourceDir) {
        QDir sDir(sourceDir);
        if (!sDir.exists()) return false;

        QFile::remove(zipFilePath);

#ifdef Q_OS_WIN
        // Windows 10/11: yerleşik tar.exe -a zip formatını otomatik destekler
        QProcess proc;
        QString winTar = "tar.exe";
        QStringList args = {"-a", "-c", "-f", QDir::toNativeSeparators(zipFilePath), "-C", QDir::toNativeSeparators(sourceDir), "."};
        proc.start(winTar, args);
        if (proc.waitForFinished(30000) && proc.exitCode() == 0 && QFile::exists(zipFilePath) && QFileInfo(zipFilePath).size() > 0) {
            return true;
        }

        // Fallback: PowerShell Compress-Archive
        QProcess psProc;
        QString psScript = QString("Compress-Archive -Path '%1\\*' -DestinationPath '%2' -Force")
            .arg(QDir::toNativeSeparators(sourceDir), QDir::toNativeSeparators(zipFilePath));
        psProc.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", psScript});
        if (psProc.waitForFinished(45000) && psProc.exitCode() == 0 && QFile::exists(zipFilePath)) {
            return true;
        }
        return false;
#else
        // Linux: Öncelikle zip komutunu dene
        QProcess proc;
        proc.setWorkingDirectory(sourceDir);
        proc.start("zip", {"-q", "-r", zipFilePath, "."});
        if (proc.waitForFinished(30000) && proc.exitCode() == 0 && QFile::exists(zipFilePath) && QFileInfo(zipFilePath).size() > 0) {
            return true;
        }

        // Fallback: Python3 ile zipfile modülü (her Linux dağıtımında yerleşik)
        QProcess pyProc;
        QString pyScript = QString(
            "import zipfile, os, sys\n"
            "src = sys.argv[1]\n"
            "dst = sys.argv[2]\n"
            "with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zf:\n"
            "    for root, dirs, files in os.walk(src):\n"
            "        for f in files:\n"
            "            fp = os.path.join(root, f)\n"
            "            arcname = os.path.relpath(fp, src)\n"
            "            zf.write(fp, arcname)\n"
        );
        pyProc.start("python3", {"-c", pyScript, sourceDir, zipFilePath});
        if (pyProc.waitForFinished(30000) && pyProc.exitCode() == 0 && QFile::exists(zipFilePath)) {
            return true;
        }
        return false;
#endif
    }

    /**
     * Uzak klasörü sıkıştırma (tar) komutu üretir.
     */
    static QString remoteTarCompressCmd(const QString& archivePath, const QString& parentDir, const QString& folderName, bool isWindows) {
        if (isWindows) {
            return QString("cmd.exe /c tar -czf \"%1\" -C \"%2\" \"%3\"")
                    .arg(QDir::toNativeSeparators(archivePath), QDir::toNativeSeparators(parentDir), folderName);
        } else {
            QString rParentEscaped = parentDir; rParentEscaped.replace("\"", "\\\"");
            return QString("tar -czf %1 -C \"%2\" \"%3\"").arg(archivePath, rParentEscaped, folderName);
        }
    }

    /**
     * Uzak arşivi açma ve geçici arşivi temizleme komutu üretir.
     */
    static QString remoteTarExtractCmd(const QString& archivePath, const QString& destDir, bool isWindows, bool overwrite = true, const QString& renameTarget = QString(), const QString& uuid = QString()) {
        if (isWindows) {
            QString natArchive = QDir::toNativeSeparators(archivePath);
            QString natDest = QDir::toNativeSeparators(destDir);
            QString cmd;
            if (overwrite && renameTarget.isEmpty()) {
                cmd += QString("cmd.exe /c rmdir /s /q \"%1\" & ").arg(natDest);
            }
            if (!renameTarget.isEmpty()) {
                QString tmpExt = "C:/Windows/Temp/okidia_ext_" + uuid;
                QString natTmp = QDir::toNativeSeparators(tmpExt);
                cmd += QString("cmd.exe /c mkdir \"%1\" & tar -xzf \"%2\" -C \"%1\" & move /Y \"%1\\%3\" \"%4\" & rmdir /s /q \"%1\" & del /f /q \"%2\"")
                        .arg(natTmp, natArchive, destDir, QDir::toNativeSeparators(renameTarget));
            } else {
                cmd += QString("cmd.exe /c tar -xzf \"%1\" -C \"%2\" & del /f /q \"%1\"").arg(natArchive, natDest);
            }
            return cmd;
        } else {
            QString cmd;
            if (overwrite && renameTarget.isEmpty()) {
                QString rDestEscaped = destDir; rDestEscaped.replace("\"", "\\\"");
                cmd += QString("rm -rf \"%1\" && ").arg(rDestEscaped);
            }
            if (!renameTarget.isEmpty()) {
                QString tmpExt = "/tmp/okidia_ext_" + uuid;
                cmd = QString("mkdir -p %1 && tar -xzf %2 -C %1 && mv %1/\"%3\" \"%4\" && rm -rf %1 && rm -f %2")
                        .arg(tmpExt, archivePath, destDir, renameTarget);
            } else {
                QString rDestEscaped = destDir; rDestEscaped.replace("\"", "\\\"");
                cmd += QString("tar -xzf %1 -C \"%2\" && rm -f %1").arg(archivePath, rDestEscaped);
            }
            return cmd;
        }
    }

    /**
     * Uzak log dosyası okuma komutu ("type ..." vs "cat ...").
     */
    static QString remoteReadLogCmd(const QString& logPath, bool isWindows) {
        if (isWindows) {
            QString native = QDir::toNativeSeparators(logPath);
            return QString("type \"%1\" 2>nul").arg(native);
        } else {
            return QString("cat \"%1\" 2>/dev/null").arg(logPath);
        }
    }

    /**
     * Uzak servis durumunu sorgulama komutu ("sc query ..." vs "systemctl is-active ...").
     */
    static QString remoteServiceQueryCmd(const QString& serviceName, bool isWindows) {
        if (isWindows) {
            return QString("sc query %1 2>nul").arg(serviceName);
        } else {
            return QString("systemctl is-active %1 2>/dev/null").arg(serviceName);
        }
    }

    // -------------------------------------------------------------------------
    // YEREL SİSTEM GÜÇ VE OTURUM YÖNETİMİ
    // -------------------------------------------------------------------------
    enum class PowerAction { Shutdown, Reboot, Logout, Suspend };

    static QPair<QString, QStringList> localPowerCommand(PowerAction action, int delaySec = 0) {
#if defined(Q_OS_WIN)
        switch (action) {
            case PowerAction::Shutdown:
                return {"shutdown.exe", {"/s", "/t", QString::number(delaySec)}};
            case PowerAction::Reboot:
                return {"shutdown.exe", {"/r", "/t", QString::number(delaySec)}};
            case PowerAction::Logout:
                return {"shutdown.exe", {"/l"}};
            case PowerAction::Suspend:
                return {"rundll32.exe", {"powrprof.dll,SetSuspendState", "0,1,0"}};
        }
#else
        switch (action) {
            case PowerAction::Shutdown:
                return {"shutdown", {"-h", delaySec > 0 ? "+" + QString::number(delaySec / 60) : "now"}};
            case PowerAction::Reboot:
                return {"shutdown", {"-r", delaySec > 0 ? "+" + QString::number(delaySec / 60) : "now"}};
            case PowerAction::Logout:
                return {"loginctl", {"terminate-session", "auto"}};
            case PowerAction::Suspend:
                return {"systemctl", {"suspend"}};
        }
#endif
        return {};
    }

    /**
     * Tablo sütun genişliklerini, sırasını ve sıralama durumunu kullanıcı bazlı olarak QSettings'e kaydeder.
     */
    static void saveTableState(QTableWidget* table, const QString& tableName, const QString& username = QString()) {
        if (!table || !table->horizontalHeader()) return;
        QSettings s("okidia", "master");
        QString userKey = username.trimmed();
        if (userKey.isEmpty()) {
            userKey = s.value("last_username", "default").toString();
        }
        if (userKey.isEmpty()) userKey = "default";
        s.setValue(QString("users/%1/tables/%2_header").arg(userKey, tableName), table->horizontalHeader()->saveState());
    }

    /**
     * Kullanıcı bazlı olarak kaydedilmiş tablo durumunu (genişlikler, sıra, sıralama) geri yükler.
     * Kayıt bulunursa ve uygulandıysa true döner.
     */
    static bool restoreTableState(QTableWidget* table, const QString& tableName, const QString& username = QString()) {
        if (!table || !table->horizontalHeader()) return false;
        QSettings s("okidia", "master");
        QString userKey = username.trimmed();
        if (userKey.isEmpty()) {
            userKey = s.value("last_username", "default").toString();
        }
        if (userKey.isEmpty()) userKey = "default";
        QByteArray state = s.value(QString("users/%1/tables/%2_header").arg(userKey, tableName)).toByteArray();
        if (!state.isEmpty()) {
            return table->horizontalHeader()->restoreState(state);
        }
        return false;
    }

    /**
     * Tablo başlığını Interactive moduna alır, kaydedilmiş kullanıcı ayarını yükler veya varsayılan genişlikleri uygular.
     * Ayrıca sütun genişliği her değiştiğinde otomatik olarak kullanıcı bazlı kaydetme bağlantısını kurar.
     */
    static void setupTableHeaders(QTableWidget* table, const QString& tableName,
                                  const QList<int>& defaultWidths = {},
                                  bool stretchLast = false,
                                  const QString& username = QString()) {
        if (!table || !table->horizontalHeader()) return;
        auto* header = table->horizontalHeader();
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setStretchLastSection(stretchLast);

        bool restored = restoreTableState(table, tableName, username);
        if (!restored) {
            for (int col = 0; col < defaultWidths.size() && col < table->columnCount(); ++col) {
                if (defaultWidths[col] > 0) {
                    table->setColumnWidth(col, defaultWidths[col]);
                }
            }
        }

        // Kullanıcı sütun genişliğini, yerini veya sıralamasını değiştirdiğinde otomatik kaydet
        QObject::connect(header, &QHeaderView::sectionResized, table, [table, tableName, username](int, int, int) {
            saveTableState(table, tableName, username);
        });
        QObject::connect(header, &QHeaderView::sectionMoved, table, [table, tableName, username](int, int, int) {
            saveTableState(table, tableName, username);
        });
        QObject::connect(header, &QHeaderView::sortIndicatorChanged, table, [table, tableName, username](int, Qt::SortOrder) {
            saveTableState(table, tableName, username);
        });
    }

    /**
     * Kullanıcı bazlı son kullanılan dizini döner.
     * @param context Diyalog bağlamı (örn. "ota_packages", "file_transfer", "ssh_keys", "reports", "general")
     * @param defaultPath Eğer kayıtlı yol yoksa kullanılacak varsayılan yol
     * @param username İsteğe bağlı Master kullanıcı adı
     */
    static QString getLastDirectory(const QString& context = "general", const QString& defaultPath = QString(), const QString& username = QString()) {
        QSettings s("okidia", "master");
        QString userKey = username.trimmed();
        if (userKey.isEmpty()) {
            userKey = s.value("last_username", "default").toString();
        }
        if (userKey.isEmpty()) userKey = "default";

        // 1. Önce bu bağlama özel kayıtlı dizini kontrol et
        QString path = s.value(QString("users/%1/paths/%2").arg(userKey, context)).toString().trimmed();
        
        // 2. Yoksa son kullanılan genel dizini kontrol et
        if (path.isEmpty() || (!QDir(path).exists() && !QFile::exists(path))) {
            path = s.value(QString("users/%1/paths/last_global_dir").arg(userKey)).toString().trimmed();
        }

        // 3. Hala yoksa veya yol artık mevcut değilse, verilen defaultPath'i kontrol et
        if (path.isEmpty() || (!QDir(path).exists() && !QFile::exists(path))) {
            if (!defaultPath.isEmpty() && (QDir(defaultPath).exists() || QFile::exists(defaultPath))) {
                QFileInfo dfi(defaultPath);
                return dfi.isDir() ? dfi.absoluteFilePath() : dfi.absolutePath();
            }
            return QDir::homePath();
        }

        QFileInfo fi(path);
        return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    }

    /**
     * Kullanıcı bazlı son kullanılan dizini kaydeder.
     * @param pathOrFile Seçilen dosya veya dizin yolu
     * @param context Diyalog bağlamı (örn. "ota_packages", "file_transfer", "ssh_keys", "reports", "general")
     * @param username İsteğe bağlı Master kullanıcı adı
     */
    static void setLastDirectory(const QString& pathOrFile, const QString& context = "general", const QString& username = QString()) {
        QString clean = pathOrFile.trimmed();
        if (clean.isEmpty()) return;
        QFileInfo fi(clean);
        QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
        if (dir.isEmpty() || !QDir(dir).exists()) return;

        QSettings s("okidia", "master");
        QString userKey = username.trimmed();
        if (userKey.isEmpty()) {
            userKey = s.value("last_username", "default").toString();
        }
        if (userKey.isEmpty()) userKey = "default";

        s.setValue(QString("users/%1/paths/%2").arg(userKey, context), dir);
        s.setValue(QString("users/%1/paths/last_global_dir").arg(userKey), dir);
    }

    /**
     * Son açılan dizini hatırlayan dosya açma diyaloğu
     */
    static QString getOpenFileName(QWidget* parent = nullptr,
                                   const QString& caption = QString(),
                                   const QString& context = "general",
                                   const QString& filter = QString(),
                                   QString* selectedFilter = nullptr,
                                   QFileDialog::Options options = QFileDialog::Options(),
                                   const QString& defaultPath = QString()) {
        QString initDir = getLastDirectory(context, defaultPath);
        QString res = QFileDialog::getOpenFileName(parent, caption, initDir, filter, selectedFilter, options);
        if (!res.isEmpty()) {
            setLastDirectory(res, context);
        }
        return res;
    }

    /**
     * Son açılan dizini hatırlayan çoklu dosya seçme diyaloğu
     */
    static QStringList getOpenFileNames(QWidget* parent = nullptr,
                                       const QString& caption = QString(),
                                       const QString& context = "general",
                                       const QString& filter = QString(),
                                       QString* selectedFilter = nullptr,
                                       QFileDialog::Options options = QFileDialog::Options(),
                                       const QString& defaultPath = QString()) {
        QString initDir = getLastDirectory(context, defaultPath);
        QStringList res = QFileDialog::getOpenFileNames(parent, caption, initDir, filter, selectedFilter, options);
        if (!res.isEmpty()) {
            setLastDirectory(res.first(), context);
        }
        return res;
    }

    /**
     * Son açılan dizini hatırlayan dosya kaydetme diyaloğu
     */
    static QString getSaveFileName(QWidget* parent = nullptr,
                                   const QString& caption = QString(),
                                   const QString& context = "general",
                                   const QString& defaultFileName = QString(),
                                   const QString& filter = QString(),
                                   QString* selectedFilter = nullptr,
                                   QFileDialog::Options options = QFileDialog::Options()) {
        QFileInfo fi(defaultFileName);
        QString fallbackDir = fi.isAbsolute() ? fi.absolutePath() : QString();
        QString initDir = getLastDirectory(context, fallbackDir);
        QString startPath;
        if (defaultFileName.isEmpty()) {
            startPath = initDir;
        } else {
            QString fname = fi.fileName();
            startPath = fname.isEmpty() ? initDir : QDir(initDir).filePath(fname);
        }
        QString res = QFileDialog::getSaveFileName(parent, caption, startPath, filter, selectedFilter, options);
        if (!res.isEmpty()) {
            setLastDirectory(res, context);
        }
        return res;
    }

    /**
     * Son açılan dizini hatırlayan klasör seçme diyaloğu
     */
    static QString getExistingDirectory(QWidget* parent = nullptr,
                                        const QString& caption = QString(),
                                        const QString& context = "general",
                                        QFileDialog::Options options = QFileDialog::ShowDirsOnly,
                                        const QString& defaultPath = QString()) {
        QString initDir = getLastDirectory(context, defaultPath);
        QString res = QFileDialog::getExistingDirectory(parent, caption, initDir, options);
        if (!res.isEmpty()) {
            setLastDirectory(res, context);
        }
        return res;
    }

    /**
     * Hedef bağlama noktasının veya sistem kök dizininin şifrelenmiş (dm-crypt/LUKS, BitLocker)
     * olup olmadığını kontrol eder.
     * @param mountPoint Kontrol edilecek bağlama noktası ("/" veya "C:")
     * @return Disk şifreli ise true döner.
     */
    static bool checkDiskEncryption(const QString& mountPoint = QString()) {
#if defined(Q_OS_WIN)
        QString drive = mountPoint.trimmed().toUpper();
        if (drive.isEmpty()) {
            drive = "C:";
        } else if (drive.length() == 1) {
            drive += ":";
        } else if (drive.length() > 2) {
            drive = drive.left(2);
        }

        // manage-bde -status C: sorgusu
        QProcess proc;
        proc.start("manage-bde.exe", {"-status", drive});
        if (proc.waitForFinished(3000) && proc.exitCode() == 0) {
            QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).toLower();
            // Conversion Status: Fully Encrypted or Protection Status: Protection On
            if (out.contains("protection on") || out.contains("koruma acik") ||
                out.contains("fully encrypted") || out.contains("tamamen sifreli") ||
                out.contains("tümüyle şifreli") || out.contains("şifrelendi")) {
                return true;
            }
        }
        return false;
#else
        QString target = mountPoint.trimmed();
        if (target.isEmpty()) {
            target = "/";
        }

        // 1. lsblk ile bağlama noktasının blok aygıt tipini sorgula
        QProcess proc;
        proc.start("lsblk", {"-n", "-o", "TYPE", target});
        if (proc.waitForFinished(2000) && proc.exitCode() == 0) {
            QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toLower();
            if (out.contains("crypt")) {
                return true;
            }
        }

        // 2. Fallback: /proc/mounts veya /sys/block/dm-* incelemesi
        QFile mountsFile("/proc/mounts");
        if (mountsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&mountsFile);
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                QStringList parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 2 && parts[1] == target) {
                    QString dev = parts[0];
                    if (dev.contains("/mapper/") && (dev.contains("luks") || dev.contains("crypt"))) {
                        return true;
                    }
                    if (dev.startsWith("/dev/dm-")) {
                        QString dmName = dev.section('/', -1);
                        QFile uuidFile("/sys/block/" + dmName + "/dm/uuid");
                        if (uuidFile.open(QIODevice::ReadOnly)) {
                            QString uuid = QString::fromUtf8(uuidFile.readAll()).trimmed().toLower();
                            if (uuid.startsWith("crypto_luks") || uuid.startsWith("crypt-")) {
                                return true;
                            }
                        }
                    }
                }
            }
            mountsFile.close();
        }

        return false;
#endif
    }

    /**
     * Sistemin UEFI Secure Boot (Güvenli Önyükleme) durumunu denetler.
     * @return Secure Boot aktif ise true döner.
     */
    static bool checkSecureBootEnabled() {
#if defined(Q_OS_WIN)
        // 1. Windows Registry: HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\SecureBoot\State -> UEFISecureBootEnabled
        QSettings reg("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", QSettings::NativeFormat);
        QVariant val = reg.value("UEFISecureBootEnabled");
        if (val.isValid() && val.toUInt() == 1) {
            return true;
        }

        // 2. PowerShell Fallback: Confirm-SecureBootUEFI
        QProcess proc;
        proc.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", "Confirm-SecureBootUEFI"});
        if (proc.waitForFinished(3000) && proc.exitCode() == 0) {
            QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed().toLower();
            if (out == "true") {
                return true;
            }
        }
        return false;
#else
        // 1. Linux efivars tespiti: /sys/firmware/efi/efivars/SecureBoot-*
        QDir efivarsDir("/sys/firmware/efi/efivars");
        if (efivarsDir.exists()) {
            QStringList matches = efivarsDir.entryList(QStringList{"SecureBoot-*"}, QDir::Files);
            for (const QString& match : matches) {
                QFile f(efivarsDir.filePath(match));
                if (f.open(QIODevice::ReadOnly)) {
                    QByteArray data = f.readAll();
                    // EFI değişkeni ilk 4 bayt attributes, 5. bayt (data[4]) değişken değeridir.
                    if (data.size() >= 5 && static_cast<unsigned char>(data.at(4)) == 1) {
                        return true;
                    }
                }
            }
        }

        // 2. mokutil fallback (varsa)
        QProcess proc;
        proc.start("mokutil", {"--sb-state"});
        if (proc.waitForFinished(1000) && proc.exitCode() == 0) {
            QString out = QString::fromUtf8(proc.readAllStandardOutput()).toLower();
            if (out.contains("secureboot enabled")) {
                return true;
            }
        }

        return false;
#endif
    }
};

class OkidiaTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override {
        QVariant myPrio = data(0, Qt::UserRole);
        QVariant otherPrio = other.data(0, Qt::UserRole);
        
        auto getPriority = [](const QString& type) -> int {
            if (type == "all_clients") return 0;
            if (type == "class") return 1;
            if (type == "group") return 2;
            if (type == "subnet") return 3;
            if (type == "client") return 4;
            return 99;
        };

        if (myPrio.isValid() && otherPrio.isValid()) {
            int p1 = getPriority(myPrio.toString());
            int p2 = getPriority(otherPrio.toString());
            if (p1 != p2) return p1 < p2;
        }

        int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column < 0) column = 0;
        return OkidiaUtils::naturalCompare(text(column), other.text(column)) < 0;
    }
};

class OkidiaTableItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override {
        const QVariant myPrio = data(Qt::UserRole);
        const QVariant otherPrio = other.data(Qt::UserRole);
        
        if (myPrio.userType() == QMetaType::Int && otherPrio.userType() == QMetaType::Int) {
            int p1 = myPrio.toInt();
            int p2 = otherPrio.toInt();
            if (p1 != p2) {
                bool isDescending = false;
                if (tableWidget() && tableWidget()->horizontalHeader()) {
                    isDescending = (tableWidget()->horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder);
                }
                return isDescending ? (p1 > p2) : (p1 < p2);
            }
        }

        const QVariant mySortKey = data(Qt::UserRole + 1);
        const QVariant otherSortKey = other.data(Qt::UserRole + 1);
        if (mySortKey.isValid() && otherSortKey.isValid()) {
            if (mySortKey.userType() == QMetaType::ULongLong || mySortKey.userType() == QMetaType::UInt ||
                mySortKey.userType() == QMetaType::LongLong || mySortKey.userType() == QMetaType::Int) {
                return mySortKey.toULongLong() < otherSortKey.toULongLong();
            }
            if (mySortKey.userType() == QMetaType::Double) {
                return mySortKey.toDouble() < otherSortKey.toDouble();
            }
        }

        return OkidiaUtils::naturalCompare(text(), other.text()) < 0;
    }
};
