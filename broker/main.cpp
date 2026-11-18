// broker/main.cpp
// OKIDIA Broker Servisi — saf headless servis (QCoreApplication).
// Masaüstü varsa okidia-broker-tray XDG autostart ile ayrı başlar,
// /run/okidia/broker.sock üzerinden IPC yapar.

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QLocalServer>
#include <QTimer>
#include <QCryptographicHash>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include "messages.pb.h"
#include "../common/utils/Logger.h"
#include "../common/utils/OkidiaUtils.h"
#include "../common/network/TlsServer.h"
#include "../common/network/MdnsManager.h"
#include "../common/OkidiaConfig.h"
#include "ClientRegistry.h"
#include "MulticastManager.h"
#include "../common/db/OkidiaDatabase.h"
#include "../common/db/OkidiaAuditDatabase.h"
#include "../common/db/OkidiaChatDatabase.h"
#include "../common/db/DbRecords.h"
#include <QRandomGenerator>
#include <QUuid>
#include <QDateTime>
#include <QTemporaryFile>
#include <QProcess>
#include <QStandardPaths>
#include <QNetworkInterface>
#include <QNetworkInformation>
#include <QStorageInfo>
#include "../common/crypto/AesGcm.h"
#include "../common/crypto/BackupPackage.h"
#include "../common/crypto/KeyManager.h"
#include "../common/utils/PlatformService.h"

#include "BrokerIpcServer.h"
#include "CertSigner.h"
#include "BrokerTransferManager.h"
#include "EnrollmentManager.h"
#include "BellScheduler.h"
#include <QTimeZone>

#include "handlers/BrokerContext.h"
#include "handlers/MessageHandler.h"

// ── runBrokerApp ─────────────────────────────────────────────────────────────
static int runBrokerApp(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("okidia");
    app.setApplicationName("broker");
    app.setApplicationVersion(Okidia::Version::string());

    Logger::init(OkidiaConfig::logDir() + "/okidia-broker.log");
    Logger::info(QObject::tr("OKIDIA Broker başlatılıyor..."));

    PlatformService::setupSignalHandlers();

    QCommandLineParser parser;
    parser.setApplicationDescription("OKIDIA Broker Service");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption resetOption("reset-admin", "Yönetici (YM) şifresini sıfırla.", "yeni_şifre");
    parser.addOption(resetOption);

    QCommandLineOption restoreOption("restore", "Yedek dosyasından (.okbak) sistemi ve sertifikaları geri yükle.", "dosya_yolu");
    parser.addOption(restoreOption);

    QCommandLineOption passwordOption("password", "Yedek dosyasının şifresi.", "şifre");
    parser.addOption(passwordOption);

    QCommandLineOption showCodeOption("show-setup-code", "Sunucunun ilk kurulum kodunu (Setup Code) terminalde gösterir.");
    parser.addOption(showCodeOption);
    
    parser.process(app);

    if (parser.isSet(restoreOption)) {
        QString backupPath = parser.value(restoreOption);
        if (!QFile::exists(backupPath)) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Yedek dosyası bulunamadı → %1").arg(backupPath).toUtf8().constData());
            return 1;
        }

        QString pwd = parser.value(passwordOption);
        if (pwd.isEmpty()) {
            printf("%s ", QObject::tr("Yedekleme Parolasını Girin:").toUtf8().constData());
            fflush(stdout);
            char buf[256];
            if (fgets(buf, sizeof(buf), stdin)) {
                pwd = QString::fromUtf8(buf).trimmed();
            }
        }

        if (pwd.isEmpty()) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Parola boş olamaz.").toUtf8().constData());
            return 1;
        }

        BackupContent content;
        QString err;
        bool unpacked = BackupPackage::unpackFromFile(backupPath, pwd, content, &err);
        if (!unpacked) {
            // Aday varyasyonları otomatik olarak dene (Tireli/tiresiz, OKIDIA- önekli/öneksiz, legacy broker.key)
            QStringList candidates;
            QString trimmed = pwd.trimmed();
            if (!candidates.contains(trimmed)) candidates << trimmed;
            
            QString clean = OkidiaUtils::cleanRecoveryKey(pwd);
            if (!clean.isEmpty() && !candidates.contains(clean)) candidates << clean;

            if (!trimmed.startsWith("OKIDIA-")) {
                QString withPrefix = "OKIDIA-" + trimmed;
                if (!candidates.contains(withPrefix)) candidates << withPrefix;
            }
            if (trimmed.startsWith("OKIDIA-")) {
                QString withoutPrefix = trimmed.mid(7);
                if (!candidates.contains(withoutPrefix)) candidates << withoutPrefix;
            }

            // Geriye dönük uyumluluk: Yerel broker.key varsa hash'ini dene
            QFile kf(OkidiaConfig::brokerCertDir() + "/broker.key");
            if (kf.open(QIODevice::ReadOnly)) {
                QString legacyKey = QString::fromUtf8(QCryptographicHash::hash(kf.readAll(), QCryptographicHash::Sha256).toHex());
                kf.close();
                if (!candidates.contains(legacyKey)) candidates << legacyKey;
            }

            for (const QString& cand : candidates) {
                if (BackupPackage::unpackFromFile(backupPath, cand, content, &err)) {
                    unpacked = true;
                    break;
                }
            }
        }

        if (!unpacked) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Yedek paketi açılamadı: %1").arg(err).toUtf8().constData());
            return 1;
        }

        // 1. Veritabanını geri yükle
        const QString dbDir = OkidiaConfig::dbDir();
        QDir().mkpath(dbDir);
        QString dbPath = dbDir + "/okidia.db";
        QFile::remove(dbPath + "-wal");
        QFile::remove(dbPath + "-shm");
        
        QFile dbFile(dbPath);
        if (!dbFile.open(QIODevice::WriteOnly)) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Veritabanı dosyası açılamadı → %1").arg(dbPath).toUtf8().constData());
            return 1;
        }
        dbFile.write(content.databaseData);
        dbFile.close();

        // 1.b Audit veritabanını geri yükle (Varsa)
        if (!content.auditData.isEmpty()) {
            QString auditPath = dbDir + "/okidia_audit.db";
            QFile::remove(auditPath + "-wal");
            QFile::remove(auditPath + "-shm");
            QFile af(auditPath);
            if (af.open(QIODevice::WriteOnly)) {
                af.write(content.auditData);
                af.close();
                printf("  • %s\n", QObject::tr("Adli Denetim veritabanı yüklendi (okidia_audit.db)").toUtf8().constData());
            }
        }

        // 1.c Chat veritabanını geri yükle (Varsa)
        if (!content.chatData.isEmpty()) {
            QString chatPath = dbDir + "/okidia_chat.db";
            QFile::remove(chatPath + "-wal");
            QFile::remove(chatPath + "-shm");
            QFile cf(chatPath);
            if (cf.open(QIODevice::WriteOnly)) {
                cf.write(content.chatData);
                cf.close();
                printf("  • %s\n", QObject::tr("Sohbet veritabanı yüklendi (okidia_chat.db)").toUtf8().constData());
            }
        }

        // 2. Kök CA ve Broker sertifikalarını geri yükle
        const QString certDir = OkidiaConfig::brokerCertDir();
        QDir().mkpath(certDir);
        for (auto it = content.certFiles.begin(); it != content.certFiles.end(); ++it) {
            QString outCertPath = certDir + "/" + it.key();
            QFile cf(outCertPath);
            if (cf.open(QIODevice::WriteOnly)) {
                cf.write(it.value());
                cf.close();
                printf("  • %s\n", QObject::tr("Sertifika yüklendi: %1").arg(it.key()).toUtf8().constData());
            }
        }

        printf("%s\n", QObject::tr("Başarılı: Sistem veritabanı, adli loglar ve Kök CA güvenlik sertifikaları başarıyla geri yüklendi.").toUtf8().constData());
        return 0;
    }

    if (parser.isSet(resetOption)) {
        QString newPw = parser.value(resetOption);
        if (newPw.length() < 8) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Şifre en az 8 karakter olmalıdır.").toUtf8().constData());
            return 1;
        }
        const QString dbDir = OkidiaConfig::dbDir();
        if (!OkidiaDatabase::instance().open(dbDir + "/okidia.db")) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Veritabanı açılamadı.").toUtf8().constData());
            return 1;
        }
        QList<UserRecord> users = OkidiaDatabase::instance().allUsers();
        QString ymId;
        for (const auto& u : users) if (u.role == "YM") { ymId = u.id; break; }
        if (ymId.isEmpty()) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Kayıtlı YM bulunamadı.").toUtf8().constData());
            return 1;
        }
        QString hash = OkidiaUtils::sha256(newPw.toUtf8());
        if (OkidiaDatabase::instance().setUserPassword(ymId, hash, false)) {
            printf("%s\n", QObject::tr("Başarılı: YM şifresi sıfırlandı.").toUtf8().constData());
            return 0;
        }
        return 1;
    }

    if (parser.isSet(showCodeOption)) {
        const QString dbDir = OkidiaConfig::dbDir();
        if (!OkidiaDatabase::instance().open(dbDir + "/okidia.db")) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Çekirdek veritabanı açılamadı.").toUtf8().constData());
            return 1;
        }
        QString code = OkidiaDatabase::instance().metaValue("setup_code");
        if (code.isEmpty()) {
            fprintf(stderr, "%s\n", QObject::tr("Hata: Kayıtlı kurulum kodu bulunamadı.").toUtf8().constData());
            return 1;
        }
        fprintf(stdout, "\n");
        fprintf(stdout, "  ╔══════════════════════════════════════════════╗\n");
        fprintf(stdout, "  ║       OKIDIA — SUNUCU KURULUM KODU           ║\n");
        fprintf(stdout, "  ╠══════════════════════════════════════════════╣\n");
        fprintf(stdout, "  ║  Kod: %-38s║\n", code.toLatin1().constData());
        fprintf(stdout, "  ╚══════════════════════════════════════════════╝\n");
        fprintf(stdout, "\n");
        fflush(stdout);
        return 0;
    }

    // ── Veritabanı ────────────────────────────────────────────────────────────
    {
        const QString dbDir = OkidiaConfig::dbDir();
        if (!OkidiaDatabase::instance().open(dbDir + "/okidia.db")) {
            Logger::error(QObject::tr("Broker: Çekirdek veritabanı açılamadı, çıkılıyor."));
            return 1;
        }
        if (!OkidiaAuditDatabase::instance().open(dbDir + "/okidia_audit.db")) {
            Logger::error(QObject::tr("Broker: Denetim (Audit) veritabanı açılamadı, çıkılıyor."));
            return 1;
        }
        if (!OkidiaChatDatabase::instance().open(dbDir + "/okidia_chat.db")) {
            Logger::error(QObject::tr("Broker: Sohbet (Chat) veritabanı açılamadı, çıkılıyor."));
            return 1;
        }
        // Süresi dolmuş bekleyen komutları temizle
        OkidiaDatabase::instance().purgePendingCommands();
    }

    // ── Kurulum kodu ──────────────────────────────────────────────────────────
    QString setupCode = OkidiaDatabase::instance().metaValue("setup_code");
    if (setupCode.isEmpty()) {
        const QString chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (int i = 0; i < 8; ++i)
            setupCode += chars[QRandomGenerator::global()->bounded(chars.size())];
        setupCode = "OKIDIA-" + setupCode.left(4) + "-" + setupCode.mid(4);
        OkidiaDatabase::instance().setMetaValue("setup_code", setupCode);
        OkidiaDatabase::instance().setMetaValue("setup_code_used", "0");
    }
    const bool codeUsed =
        OkidiaDatabase::instance().metaValue("setup_code_used") == "1";
    if (!codeUsed) {
        fprintf(stdout, "\n");
        fprintf(stdout, "  ╔══════════════════════════════════════════════╗\n");
        fprintf(stdout, "  ║       OKIDIA — İLK KURULUM KODU             ║\n");
        fprintf(stdout, "  ╠══════════════════════════════════════════════╣\n");
        fprintf(stdout, "  ║  Kod: %-38s║\n", setupCode.toLatin1().constData());
        fprintf(stdout, "  ╠══════════════════════════════════════════════╣\n");
        fprintf(stdout, "  ║  Bu kodu Master kurulumunda girin.          ║\n");
        fprintf(stdout, "  ║  Tek kullanımlıktır, lütfen kaydedin!       ║\n");
        fprintf(stdout, "  ╚══════════════════════════════════════════════╝\n");
        fprintf(stdout, "\n");
        fflush(stdout);
        Logger::info(QObject::tr("Broker: Kurulum kodu hazır → ") + setupCode);
        
        // Setup kodunu kurulum betiklerinin alabilmesi için dosyalara yaz
        QDir().mkpath(OkidiaConfig::dbDir());
        QFile scFile(OkidiaConfig::dbDir() + "/setup_code.txt");
        if (scFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            scFile.write(setupCode.toUtf8());
            scFile.close();
            scFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
        }

#if defined(Q_OS_WIN)
        // Windows'ta doğrudan C:/ProgramData/Okidia/Broker/setup_code.txt içine de yaz
        QString altPath = OkidiaConfig::systemBasePath() + "/Broker/setup_code.txt";
        QFile scAlt(altPath);
        if (scAlt.open(QIODevice::WriteOnly | QIODevice::Text)) {
            scAlt.write(setupCode.toUtf8());
            scAlt.close();
        }
#endif
    } else {
        setupCode.clear();
        QFile::remove(OkidiaConfig::dbDir() + "/setup_code.txt");
#if defined(Q_OS_WIN)
        QFile::remove(OkidiaConfig::systemBasePath() + "/Broker/setup_code.txt");
#endif
        Logger::info(QObject::tr("Broker: Kurulum kodu daha önce kullanıldı."));
    }

    // ── IPC sunucu (tray bağlantısı) ─────────────────────────────────────────
    auto* ipc = new BrokerIpcServer(&app);
    ipc->listen(OkidiaConfig::runDir() + "/broker.sock");

    // ── Sertifika yolları & Otomatik Üretim ───────────────────────────────────
    QString certDir = OkidiaConfig::brokerCertDir();
    QDir().mkpath(certDir);

    QString caCrt  = certDir + "/ca.crt";
    QString caKey  = certDir + "/ca.key";
    QString srvCrt = certDir + "/broker.crt";
    QString srvKey = certDir + "/broker.key";

    // Eğer broker.crt yok fakat server.crt varsa onu kullan
    if (!QFile::exists(srvCrt) && QFile::exists(certDir + "/server.crt")) {
        srvCrt = certDir + "/server.crt";
        srvKey = certDir + "/server.key";
    }

    // Geliştirme/test ortamı geri dönüş kontrolü
    if (!QFile::exists(caCrt) && QFile::exists("../certs/test/ca.crt")) {
        certDir = "../certs/test";
        caCrt   = certDir + "/ca.crt";
        caKey   = certDir + "/ca.key";
        srvCrt  = certDir + "/server.crt";
        srvKey  = certDir + "/server.key";
    }

    // Eğer Kök CA sertifikaları henüz yoksa (Sıfır kurulum), yerel OpenSSL motoru ile otomatik üret
    if (!QFile::exists(caCrt) || !QFile::exists(caKey)) {
        Logger::info(QObject::tr("Broker: Kök CA sertifikası bulunamadı, otomatik üretiliyor..."));
        if (!KeyManager::generateCaCertificate(caKey, caCrt, "OKIDIA Root CA")) {
            Logger::error(QObject::tr("Broker: Kök CA sertifikası üretilemedi!"));
            return 1;
        }
    }

    // Eğer Sunucu sertifikası henüz yoksa (Sıfır kurulum), Kök CA ile imzalı otomatik üret
    if (!QFile::exists(srvCrt) || !QFile::exists(srvKey)) {
        Logger::info(QObject::tr("Broker: Sunucu (Broker) sertifikası bulunamadı, otomatik üretiliyor..."));
        srvCrt = certDir + "/broker.crt";
        srvKey = certDir + "/broker.key";
        if (!KeyManager::generateSignedCertificate(srvKey, srvCrt, caKey, caCrt, "OKIDIA Broker")) {
            Logger::error(QObject::tr("Broker: Sunucu sertifikası üretilemedi!"));
            return 1;
        }
        // Geriye dönük uyumluluk için server.crt / server.key kopyalarını da oluştur
        QFile::copy(srvCrt, certDir + "/server.crt");
        QFile::copy(srvKey, certDir + "/server.key");
    }

    // Serial dosyasını garantiye al
    if (!QFile::exists(certDir + "/ca.srl")) {
        QFile srl(certDir + "/ca.srl");
        if (srl.open(QIODevice::WriteOnly | QIODevice::Text)) {
            srl.write("01\n");
            srl.close();
        }
    }

    // Özel anahtar dosya izinlerini sertleştir (0600 / NTFS ACL)
    OkidiaUtils::securePrivateKeyFile(caKey, true);
    OkidiaUtils::securePrivateKeyFile(srvKey, true);
    if (QFile::exists(certDir + "/server.key")) {
        OkidiaUtils::securePrivateKeyFile(certDir + "/server.key", true);
    }

    // Anahtar bütünlük kontrolü (Key Integrity Check)
    if (!OkidiaUtils::verifyKeyIntegrity(caKey)) {
        Logger::error(QObject::tr("Broker: Kök CA anahtarı bozuk veya okunamıyor: %1").arg(caKey));
        return 1;
    }
    if (!OkidiaUtils::verifyKeyIntegrity(srvKey)) {
        Logger::error(QObject::tr("Broker: Sunucu anahtarı bozuk veya okunamıyor: %1").arg(srvKey));
        return 1;
    }
    Logger::info(QObject::tr("Broker: Özel anahtar izinleri ve bütünlük kontrolü doğrulandı (SHA-256 ca: %1, srv: %2)")
                 .arg(OkidiaUtils::computeFileSha256(caKey).left(12), OkidiaUtils::computeFileSha256(srvKey).left(12)));

    Logger::info(QObject::tr("Broker: Sertifika dizini → ") + certDir);
    Logger::info(QObject::tr("Broker: Sunucu CRT → ") + srvCrt);
    Logger::info(QObject::tr("Broker: Kök CA → ") + caCrt);
    QString caFingerprint = KeyManager::getFingerprint(caCrt);
    Logger::info(QObject::tr("Broker: Kök CA Fingerprint → ") + caFingerprint);
    const int brokerPort = OkidiaConfig::instance().brokerPort();
    QString brokerName = OkidiaConfig::instance().brokerName();
    Logger::info(QObject::tr("Broker: İsim → %1, Port → %2").arg(brokerName).arg(brokerPort));

    // ── Client Registry + TLS Sunucu + mDNS ───────────────────────────────────
    auto* registry = new ClientRegistry(&app);
    registry->loadRevokedList();
    auto* server   = new TlsServer(&app);
    auto* multicast = new MulticastManager(registry, &app);
    auto* transferManager = new BrokerTransferManager(registry, &app);
    auto* mdns     = new MdnsManager(&app);

    auto* bellScheduler = new BellScheduler(registry, &app);

    // Veritabanından kayıtlı okul saat dilimi, ders programı ve kilit politikasını yükle
    QString savedTz = OkidiaDatabase::instance().metaValue("school_timezone");
    if (!savedTz.isEmpty()) {
        QTimeZone tz(savedTz.toUtf8());
        if (tz.isValid()) {
            bellScheduler->setSchoolTimeZone(tz);
        }
    }
    QString savedSched = OkidiaDatabase::instance().metaValue("school_schedule_json");
    if (!savedSched.isEmpty()) {
        OkidiaConfig::importSchoolConfigJson(savedSched);
        bellScheduler->updateSchedule();
    }
    QString savedLockPolicy = OkidiaDatabase::instance().metaValue("smartboard_lock_policy");
    if (!savedLockPolicy.isEmpty()) {
        OkidiaConfig::setSmartboardLockPolicy(savedLockPolicy.toUInt());
    }

    bellScheduler->start();

    if (!server->startListening(brokerPort, srvCrt, srvKey, caCrt)) {
        Logger::error(QObject::tr("Broker: Sunucu başlatılamadı!"));
        return 1;
    }
    QString savedSchoolName = OkidiaDatabase::instance().metaValue("school_name").trimmed();
    if (!savedSchoolName.isEmpty()) {
        brokerName = savedSchoolName;
    }
    mdns->registerService(brokerName, "_okidia._tcp", brokerPort, "SB", caFingerprint);

    std::function<void()> broadcastStatus;

    auto changeBrokerConfig = [&app, server, mdns, srvCrt, srvKey, caCrt, caFingerprint, &broadcastStatus](const QString& newName, quint16 newPort) -> bool {
        QString finalName = newName.trimmed().isEmpty() ? OkidiaConfig::instance().brokerName() : newName.trimmed();
        quint16 finalPort = newPort > 0 ? newPort : static_cast<quint16>(OkidiaConfig::instance().brokerPort());

        if (!OkidiaConfig::saveBrokerConfig(finalName, finalPort)) {
            Logger::error(QObject::tr("Broker: Config dosyasına yeni yapılandırma kaydedilemedi: %1 : %2").arg(finalName).arg(finalPort));
            return false;
        }
        Logger::info(QObject::tr("Broker: Yapılandırma güncellendi (İsim: %1, Port: %2), sunucu ve mDNS yeniden başlatılıyor...").arg(finalName).arg(finalPort));
        QTimer::singleShot(400, [&app, server, mdns, finalName, finalPort, srvCrt, srvKey, caCrt, caFingerprint, &broadcastStatus]() {
            mdns->unregisterService();
            server->stopListening();
            if (server->startListening(finalPort, srvCrt, srvKey, caCrt)) {
                mdns->registerService(finalName, "_okidia._tcp", finalPort, "SB", caFingerprint);
                Logger::info(QObject::tr("Broker: Yeni yapılandırmayla dinleniyor ve mDNS anonsu güncellendi → %1:%2").arg(finalName).arg(finalPort));
            } else {
                Logger::error(QObject::tr("Broker: Yeni portta dinleme başlatılamadı → %1").arg(finalPort));
            }
            if (broadcastStatus) broadcastStatus();
        });
        return true;
    };

    auto changeBrokerPort = [changeBrokerConfig](quint16 newPort) -> bool {
        return changeBrokerConfig(QString(), newPort);
    };

    // ── Chat mesaj batch buffer (5sn'de bir DB'ye yaz) ───────────────────────
    QList<ChatBatchItem> chatBatch;

    // ── Sohbet & Üyelik Durum Takibi ──────────────────────────────────────────
    // Aktif oda üyelikleri: roomId -> (clientId -> ChatMemberEvent)
    QMap<QString, QMap<QString, okidia::ChatMemberEvent>> activeRoomMembers;
    // Bağlantı -> Katıldığı odalar ve clientId'ler
    QMap<Connection*, QSet<QPair<QString, QString>>> connToRoomMembers;
    // Agent sohbet penceresi açık olan istemciler (clientId -> bool)
    QMap<QString, bool> agentWindowOpenMap;

    auto* chatFlushTimer = new QTimer(&app);
    chatFlushTimer->setInterval(5000);
    QObject::connect(chatFlushTimer, &QTimer::timeout, [&]() {
        if (chatBatch.isEmpty()) return;
        QList<ChatBatchItem> pending;
        pending.swap(chatBatch);
        for (const auto& item : pending) {
            OkidiaChatDatabase::instance().saveChatMessage(
                item.id, item.roomId, item.fromHost,
                item.senderRole, item.content,
                item.timestamp, item.isSystem);
        }
        Logger::info(QObject::tr("Broker: ") + QString::number(pending.size())
                     + QObject::tr(" sohbet mesajı DB'ye yazıldı"));
    });
    chatFlushTimer->start();

    // ── Periyodik temizlik (Purge) ───────────────────────────────────────────
    auto* purgeTimer = new QTimer(&app);
    purgeTimer->setInterval(24 * 3600 * 1000); // Günde bir kez

    auto purgeLogic = []() {
        int auditDays = OkidiaAuditDatabase::instance().auditRetentionDays();
        int chatDays  = OkidiaChatDatabase::instance().chatRetentionDays();
        // Önce eski kayıtları sıkıştırılmış haftalık .db.gz dosyası olarak archive/ klasörüne arşivle
        OkidiaChatDatabase::instance().archiveOldChat(chatDays);
        OkidiaAuditDatabase::instance().archiveOldAudit(auditDays);
        // Ardından canlı tablolarda süresi dolanları temizle
        OkidiaAuditDatabase::instance().purgeAuditLog(auditDays);
        OkidiaChatDatabase::instance().purgeChatMessages(chatDays);
        OkidiaDatabase::instance().purgePendingCommands();
        Logger::info(QObject::tr("Broker: Periyodik temizlik ve arşivleme tamamlandı (audit: %1 gün, chat: %2 gün)")
                     .arg(auditDays).arg(chatDays));
    };

    QObject::connect(purgeTimer, &QTimer::timeout, purgeLogic);
    purgeTimer->start();
    // İlk çalışmada da yap (10 sn sonra)
    QTimer::singleShot(10000, purgeLogic);

    // ── Otomatik Yedekleme ve Master Replikasyonu (Anacron Modeli) ────────────
    auto runAutoBackupLogic = [registry]() {
        bool autoBackupEnabled = OkidiaDatabase::instance().metaValue("auto_backup_enabled") != "0"; // varsayılan açık
        if (!autoBackupEnabled) return;

        // 1. Depolama alanı denetimi (Storage Safety Check)
        QStorageInfo storage(OkidiaConfig::dbDir());
        storage.refresh();
        qint64 availableBytes = storage.bytesAvailable();

        QFileInfo fiDb(OkidiaConfig::dbDir() + "/okidia.db");
        QFileInfo fiAudit(OkidiaConfig::dbDir() + "/okidia_audit.db");
        QFileInfo fiChat(OkidiaConfig::dbDir() + "/okidia_chat.db");
        qint64 totalDbSize = fiDb.size() + (fiAudit.exists() ? fiAudit.size() : 0) + (fiChat.exists() ? fiChat.size() : 0);
        qint64 minRequired = qMax(totalDbSize * 3, qint64(150 * 1024 * 1024)); // 3x DB boyutu veya min 150 MB

        QString backupDir = OkidiaConfig::dbDir() + "/backups";
        QDir().mkpath(backupDir);

        if (availableBytes < minRequired) {
            // Acil durum temizliği (Emergency Prune): En eski yedekleri silerek disk açmayı dene
            QDir bDir(backupDir);
            QFileInfoList oldBackups = bDir.entryInfoList(QStringList() << "*.okbak", QDir::Files, QDir::Time | QDir::Reversed);
            while (oldBackups.size() > 1 && availableBytes < minRequired) {
                QFileInfo oldFile = oldBackups.takeFirst();
                QFile::remove(oldFile.absoluteFilePath());
                Logger::warning(QObject::tr("Broker: Acil disk alanı açmak için eski yedek silindi (Emergency Prune) → ") + oldFile.fileName());
                storage.refresh();
                availableBytes = storage.bytesAvailable();
            }
        }

        if (availableBytes < minRequired) {
            QString warnMsg = QObject::tr("Yetersiz disk alanı! Otomatik yedekleme iptal edildi. Gerekli: %1 MB, Boş: %2 MB")
                .arg(minRequired / (1024 * 1024))
                .arg(availableBytes / (1024 * 1024));
            Logger::error(QObject::tr("Broker: ") + warnMsg);

            // Çevrimiçi Master konsollarına telemetri uyarısı gönder
            okidia::BrokerStorageWarning bsw;
            bsw.set_available_bytes(static_cast<uint64_t>(availableBytes));
            bsw.set_required_bytes(static_cast<uint64_t>(minRequired));
            bsw.set_warning_message(warnMsg.toStdString());

            okidia::Envelope warnEnv;
            warnEnv.set_type(okidia::MessageType::BROKER_STORAGE_WARNING);
            warnEnv.set_payload(bsw.SerializeAsString());

            for (Connection* c : registry->masterConnections()) {
                c->sendEnvelope(warnEnv);
            }
            return;
        }

        Logger::info(QObject::tr("Broker: Otomatik sistem yedeği alınıyor..."));
        
        QString tempDb = QDir::tempPath() + "/okidia_autobackup_" + OkidiaUtils::generateUuid() + ".db";
        if (!OkidiaDatabase::instance().vacuumInto(tempDb)) {
            Logger::error(QObject::tr("Broker: Otomatik yedekleme VACUUM INTO başarısız!"));
            return;
        }

        QFile f(tempDb);
        if (!f.open(QIODevice::ReadOnly)) {
            QFile::remove(tempDb);
            return;
        }
        QByteArray dbData = f.readAll();
        f.close();
        QFile::remove(tempDb);

        BackupContent content;
        content.meta.version = "1.0";
        content.meta.timestamp = OkidiaUtils::nowSecs();
        content.meta.creator = "Broker-Auto";
        content.meta.description = QObject::tr("Broker Otomatik Sistem Yedeği");
        content.databaseData = dbData;

        // Audit veritabanı (varsayılan dahil)
        bool includeAudit = OkidiaDatabase::instance().metaValue("auto_backup_include_audit") != "0";
        if (includeAudit) {
            QString tempAudit = QDir::tempPath() + "/okidia_autobackup_audit_" + OkidiaUtils::generateUuid() + ".db";
            if (OkidiaAuditDatabase::instance().vacuumInto(tempAudit)) {
                QFile af(tempAudit);
                if (af.open(QIODevice::ReadOnly)) {
                    content.auditData = af.readAll();
                    af.close();
                }
                QFile::remove(tempAudit);
            }
        }

        // Chat veritabanı (varsayılan kapalı)
        bool includeChat = OkidiaDatabase::instance().metaValue("auto_backup_include_chat") == "1";
        if (includeChat) {
            QString tempChat = QDir::tempPath() + "/okidia_autobackup_chat_" + OkidiaUtils::generateUuid() + ".db";
            if (OkidiaChatDatabase::instance().vacuumInto(tempChat)) {
                QFile cf(tempChat);
                if (cf.open(QIODevice::ReadOnly)) {
                    content.chatData = cf.readAll();
                    cf.close();
                }
                QFile::remove(tempChat);
            }
        }

        // Sertifikaları ekle
        QStringList criticalFiles = {"ca.crt", "ca.key", "ca.srl", "broker.crt", "broker.key"};
        QDir cert_dir(OkidiaConfig::brokerCertDir());
        for (const QString& fName : criticalFiles) {
            QFile cf(cert_dir.absoluteFilePath(fName));
            if (cf.exists() && cf.open(QIODevice::ReadOnly)) {
                content.certFiles[fName] = cf.readAll();
                cf.close();
            }
        }

        QString backupPassword = OkidiaDatabase::instance().metaValue("backup_password");
        if (backupPassword.isEmpty()) {
            // Özel parola girilmemişse Kurtarılabilir Varsayılan Sistem Parolası:
            // 1. Tercih: Sunucunun kalıcı Kurulum Kodu (setup_code)
            QString sc = OkidiaDatabase::instance().metaValue("setup_code");
            if (!sc.isEmpty()) {
                backupPassword = sc;
            } else {
                // 2. Tercih: YM Kurtarma Kodu hash'i
                QString rh = OkidiaDatabase::instance().metaValue("recovery_key_hash");
                if (!rh.isEmpty()) {
                    backupPassword = rh;
                } else {
                    // 3. Fallback: broker.key hash'i
                    QFile kf(OkidiaConfig::brokerCertDir() + "/broker.key");
                    if (kf.open(QIODevice::ReadOnly)) {
                        backupPassword = QString::fromUtf8(QCryptographicHash::hash(kf.readAll(), QCryptographicHash::Sha256).toHex());
                        kf.close();
                    }
                }
            }
        }
        QString filename = QString("okidia_%1.okbak").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        QString fullPath = backupDir + "/" + filename;

        QString err;
        if (BackupPackage::packToFile(fullPath, backupPassword, content, &err)) {
            Logger::info(QObject::tr("Broker: Otomatik sistem yedeği oluşturuldu → ") + fullPath);
            OkidiaDatabase::instance().setMetaValue("last_auto_backup", QString::number(OkidiaUtils::nowSecs()));

            // Retention temizliği (Varsayılan 7 yedek)
            int retention = OkidiaDatabase::instance().metaValue("auto_backup_retention").toInt();
            if (retention <= 0) retention = 7;
            
            QDir bDir(backupDir);
            QFileInfoList list = bDir.entryInfoList(QStringList() << "*.okbak", QDir::Files, QDir::Time | QDir::Reversed);
            while (list.size() > retention) {
                QFileInfo oldFile = list.takeFirst();
                QFile::remove(oldFile.absoluteFilePath());
                Logger::info(QObject::tr("Broker: Eski otomatik yedek silindi (retention) → ") + oldFile.fileName());
            }

            // İkincil Master Replikasyonu (Anons Modeli)
            QList<QString> replicaUserIds = OkidiaDatabase::instance().backupReplicaUserIds();
            if (!replicaUserIds.isEmpty()) {
                QFileInfo fi(fullPath);
                okidia::BackupReplicaCheck check;
                check.set_filename(filename.toStdString());
                check.set_total_size(static_cast<uint64_t>(fi.size()));

                okidia::Envelope replicaEnv;
                replicaEnv.set_type(okidia::MessageType::BACKUP_REPLICA_CHECK);
                replicaEnv.set_payload(check.SerializeAsString());

                for (const QString& rUid : replicaUserIds) {
                    for (Connection* conn : registry->masterConnections()) {
                        if (registry->authenticatedUserId(conn) == rUid) {
                            const QString connMid = registry->authenticatedMachineId(conn);
                            const QString connHost = registry->authenticatedHostname(conn);
                            if (OkidiaDatabase::instance().isReplicaDeviceAuthorized(rUid, connMid, connHost)) {
                                conn->sendEnvelope(replicaEnv);
                                Logger::info(QObject::tr("Broker: Yetkili yedek cihazına (%1) yeni yedek anons edildi → %2 (%3)")
                                             .arg(connHost, rUid, filename));
                            } else {
                                Logger::info(QObject::tr("Broker: %1 kullanıcısı tanımlı yedek bilgisayarı dışında (%2) bağlı olduğu için yedek anonsu atlandı.")
                                             .arg(rUid, connHost));
                            }
                        }
                    }
                }
            }
        } else {
            Logger::error(QObject::tr("Broker: Otomatik yedekleme başarısız → ") + err);
        }
    };

    // Anacron Modeli: Son 24 saatte yedek alınmamışsa (gece kapalı kalmışsa) açılıştan 5 dk sonra al
    qint64 lastAutoBackup = OkidiaDatabase::instance().metaValue("last_auto_backup").toLongLong();
    qint64 nowSec = OkidiaUtils::nowSecs();
    if (lastAutoBackup == 0 || (nowSec - lastAutoBackup) >= 24 * 3600) {
        Logger::info(QObject::tr("Broker: Son 24 saatte otomatik yedek bulunamadı. Açılıştan 5 dakika sonra kaçırılan yedekleme yapılacak..."));
        QTimer::singleShot(300000, &app, [runAutoBackupLogic]() {
            runAutoBackupLogic();
        });
    }

    // Gece 03:00 periyodik zamanlayıcısı
    auto* nightlyTimer = new QTimer(&app);
    auto armNightlyTimer = [nightlyTimer]() {
        QTime targetTime(3, 0, 0);
        QDateTime now = QDateTime::currentDateTime();
        QDateTime next = now;
        next.setTime(targetTime);
        if (now >= next) {
            next = next.addDays(1);
        }
        qint64 msToNext = now.msecsTo(next);
        nightlyTimer->setSingleShot(true);
        nightlyTimer->start(static_cast<int>(msToNext));
        Logger::info(QObject::tr("Broker: Bir sonraki gece yedeklemesi zamanlandı → ") + next.toString("dd.MM.yyyy HH:mm"));
    };

    QObject::connect(nightlyTimer, &QTimer::timeout, [&app, runAutoBackupLogic, armNightlyTimer]() {
        runAutoBackupLogic();
        armNightlyTimer();
    });
    armNightlyTimer();

    // ── Durum broadcast yardımcısı ───────────────────────────────────────────
    broadcastStatus = [&]() {
        okidia::BrokerStatus st;
        st.set_ready(true);
        st.set_master_conn(registry->hasMaster());
        st.set_client_count(registry->allClients().size());
        st.set_broker_name(OkidiaConfig::instance().brokerName().toStdString());
        st.set_broker_port(static_cast<uint32_t>(OkidiaConfig::instance().brokerPort()));

        const bool codeUsed =
            OkidiaDatabase::instance().metaValue("setup_code_used") == "1";
        if (codeUsed) {
            setupCode.clear();
        }
        st.set_setup_code(setupCode.toStdString());
        for (const QString& id : registry->pendingIds())
            st.add_pending_ips(id.toStdString());

        // Sisteme bağlı olan master'ların Ad Soyad (displayName) bilgisini ekle
        for (Connection* mConn : registry->masterConnections()) {
            QString displayName;
            QString uId = registry->authenticatedUserId(mConn);
            if (!uId.isEmpty()) {
                UserRecord u = OkidiaDatabase::instance().userById(uId);
                if (!u.displayName.isEmpty()) {
                    displayName = u.displayName;
                } else if (!u.username.isEmpty()) {
                    displayName = u.username;
                }
            }
            if (displayName.isEmpty()) {
                // Eğer sertifikadan kimliği alabiliyorsak
                QString peerId;
                QSslCertificate peerCert = mConn->sslSocket() ? mConn->sslSocket()->peerCertificate() : QSslCertificate();
                if (!peerCert.isNull()) {
                    peerId = peerCert.subjectInfo(QSslCertificate::CommonName).join("");
                }
                if (peerId.startsWith("okidia-master-")) {
                    displayName = "Yönetici Master";
                } else if (!peerId.isEmpty()) {
                    UserRecord u = OkidiaDatabase::instance().userById(peerId);
                    if (!u.displayName.isEmpty()) {
                        displayName = u.displayName;
                    } else if (!u.username.isEmpty()) {
                        displayName = u.username;
                    }
                }
            }
            if (displayName.isEmpty()) {
                displayName = "Anonim Master";
            }
            st.add_master_names(displayName.toStdString());
        }

        ipc->broadcastStatus(st);
    };

    EnrollmentManager enrollmentManager(registry);
    enrollmentManager.setBroadcastStatusCallback([&]() {
        broadcastStatus();
    });

    // ── Broker Kök CA Zaman Denetimi (BIOS Pili / Desync Kontrolü) ───────────
    qint64 caNotBefore = KeyManager::getCertificateNotBefore(caCrt);
    qint64 brokerNow = OkidiaUtils::nowSecs();
    if (caNotBefore > 0 && brokerNow < caNotBefore) {
        Logger::error(QObject::tr("Broker: KRİTİK ZAMAN UYUMSUZLUĞU! Sistem saati (%1) Kök CA tarihinden (%2) geride! Sunucu BIOS pili bitmiş olabilir!")
                      .arg(brokerNow).arg(caNotBefore));

        int remainingLogins = OkidiaDatabase::instance().metaValue("time_desync_grace_logins").toInt();
        if (remainingLogins <= 0 && OkidiaDatabase::instance().metaValue("time_desync_grace_logins").isEmpty()) {
            remainingLogins = 3; // Varsayılan 3 oturum toleransı
            OkidiaDatabase::instance().setMetaValue("time_desync_grace_logins", "3");
        }
        enrollmentManager.setBrokerTimeStatus(false, remainingLogins);
    } else {
        // Saat sağlıklı ise tolerans sayacını 3 olarak yenile/sıfırla
        OkidiaDatabase::instance().setMetaValue("time_desync_grace_logins", "3");
        enrollmentManager.setBrokerTimeStatus(true, 3);
    }


    // ── Kilit broadcast yardımcısı ───────────────────────────────────────────
    auto broadcastAgentLocks = [&]() {
        okidia::AgentLockStatusBroadcast broadcastMessage;
        QMap<QString, AgentLock> locks = registry->activeLocks();
        for (auto it = locks.begin(); it != locks.end(); ++it) {
            okidia::AgentLockInfo* info = broadcastMessage.add_locks();
            info->set_client_id(it.key().toStdString());
            info->set_master_user_id(it.value().masterUserId.toStdString());
            info->set_master_display_name(it.value().masterDisplayName.toStdString());
            info->set_lock_type(it.value().lockType.toStdString());
            info->set_locked_at(it.value().lockedAt);
        }

        okidia::Envelope env;
        env.set_type(okidia::MessageType::AGENT_LOCK_STATUS_BROADCAST);
        env.set_payload(broadcastMessage.SerializeAsString());

        for (Connection* conn : registry->masterConnections()) {
            conn->sendEnvelope(env);
        }
    };

    QObject::connect(registry, &ClientRegistry::agentLocksChanged, broadcastAgentLocks);

    QObject::connect(registry, &ClientRegistry::clientRevokedBroadcast, [&](const QString& machineId) {
        okidia::Envelope env;
        env.set_type(okidia::MessageType::CLIENT_REVOKED_BROADCAST);
        okidia::ClientRevokedBroadcast msg;
        msg.set_machine_id(machineId.toStdString());
        env.set_payload(msg.SerializeAsString());
        for (Connection* conn : registry->masterConnections()) {
            conn->sendEnvelope(env);
        }
    });

    // ── Tray'den gelen komutlar ───────────────────────────────────────────────
    QObject::connect(ipc, &BrokerIpcServer::cmdReceived,
                     [&](const okidia::BrokerCommand& cmd) {
        if (cmd.type() == okidia::BrokerCommand::STOP) {
            Logger::info(QObject::tr("Broker: Tray'den durdur komutu alındı. Servis durduruluyor..."));
#if defined(Q_OS_WIN)
            PlatformService::stopService();
            QCoreApplication::quit();
#else
            QProcess::startDetached("systemctl", QStringList() << "stop" << "okidia-broker.service");
            // Eğer systemd servisi olarak çalışmıyorsa doğrudan uygulamayı sonlandır
            QTimer::singleShot(1500, &app, &QCoreApplication::quit);
#endif
        } else if (cmd.type() == okidia::BrokerCommand::STATUS) {
            // Tray bağlandığında anlık durum ister
            broadcastStatus();
        } else if (cmd.type() == okidia::BrokerCommand::SET_CONFIG) {
            QString newName = QString::fromStdString(cmd.broker_name());
            quint16 newPort = static_cast<quint16>(cmd.broker_port());
            Logger::info(QObject::tr("Broker: Tray'den yeni yapılandırma komutu alındı → İsim: %1, Port: %2").arg(newName).arg(newPort));
            changeBrokerConfig(newName, newPort);
        }
    });

    // ── Handlers & Context ───────────────────────────────────────────────────
    auto* ctx = new BrokerContext();
    ctx->registry = registry;
    ctx->multicast = multicast;
    ctx->transferManager = transferManager;
    ctx->enrollmentManager = &enrollmentManager;
    ctx->bellScheduler = bellScheduler;
    ctx->broadcastStatus = broadcastStatus;
    ctx->changeBrokerPort = changeBrokerPort;
    ctx->changeBrokerConfig = changeBrokerConfig;
    ctx->purgeLogic = purgeLogic;
    ctx->setupCode = setupCode;
    ctx->chatBatch = &chatBatch;
    ctx->activeRoomMembers = &activeRoomMembers;
    ctx->connToRoomMembers = &connToRoomMembers;
    ctx->agentWindowOpenMap = &agentWindowOpenMap;
    ctx->pushLatestBackupToUser = [registry](const QString& targetUserId) {
        if (targetUserId.isEmpty()) return;
        QString backupDir = OkidiaConfig::dbDir() + "/backups";
        QDir bDir(backupDir);
        QFileInfoList list = bDir.entryInfoList(QStringList() << "*.okbak", QDir::Files, QDir::Time);
        if (list.isEmpty()) {
            Logger::warning(QObject::tr("Broker: Replikasyon için hazır .okbak yedeği bulunamadı"));
            return;
        }
        QFileInfo latestBackup = list.first();
        okidia::BackupReplicaCheck check;
        check.set_filename(latestBackup.fileName().toStdString());
        check.set_total_size(static_cast<uint64_t>(latestBackup.size()));

        okidia::Envelope replicaEnv;
        replicaEnv.set_type(okidia::MessageType::BACKUP_REPLICA_CHECK);
        replicaEnv.set_payload(check.SerializeAsString());

        for (Connection* conn : registry->masterConnections()) {
            if (registry->authenticatedUserId(conn) == targetUserId) {
                const QString connMid = registry->authenticatedMachineId(conn);
                const QString connHost = registry->authenticatedHostname(conn);
                if (OkidiaDatabase::instance().isReplicaDeviceAuthorized(targetUserId, connMid, connHost)) {
                    conn->sendEnvelope(replicaEnv);
                    Logger::info(QObject::tr("Broker: Yetkili YM/Replika cihazına (%1) son yedek anons edildi → %2 (%3, %4 KB)")
                                 .arg(connHost, targetUserId, latestBackup.fileName(), QString::number(latestBackup.size() / 1024)));
                } else {
                    const UserRecord uRec = OkidiaDatabase::instance().userById(targetUserId);
                    Logger::info(QObject::tr("Broker: %1 (%2) tanımlı yedek bilgisayarı (%3) yerine farklı bir bilgisayardan (%4) bağlı; .okbak yedeği bu bilgisayara aktarılmadı.")
                                 .arg(uRec.username, targetUserId, uRec.replicaHostname, connHost));
                }
            }
        }
    };

    auto messageHandler = std::make_shared<MessageHandler>(ctx);
    ctx->reconcileAgentState = [messageHandler](const QString& clientId) {
        messageHandler->reconcileAgentState(clientId);
    };

    // ── Yeni TLS bağlantısı ───────────────────────────────────────────────────
    QObject::connect(server, &TlsServer::newConnection,
                     [ctx, messageHandler](Connection* conn) {

        Logger::info(QObject::tr("Broker: Yeni bağlantı ← ") + conn->peerAddress());
        bool* isMaster = new bool(false);

        QObject::connect(conn, &Connection::envelopeReceived,
                         conn, [conn, ctx, messageHandler, isMaster]
                         (const okidia::Envelope& env) {

            // Yüksek frekanslı periyodik veri mesajlarını (Heartbeat, ekran kareleri, telemetri)
            // log şişmesini ve gürültüyü önlemek amacıyla genel INFO logundan muaf tut
            const okidia::MessageType mType = env.type();
            const bool isHighFrequencyNoise = (
                mType == okidia::MessageType::HEARTBEAT ||
                mType == okidia::MessageType::SCREEN_FRAME ||
                mType == okidia::MessageType::SCREEN_TILE_UPDATE ||
                mType == okidia::MessageType::REMOTE_FRAME ||
                mType == okidia::MessageType::BROADCAST_FRAME ||
                mType == okidia::MessageType::SYSTEM_METRICS_REPORT ||
                mType == okidia::MessageType::TIME_SYNC_REQUEST ||
                mType == okidia::MessageType::TIME_SYNC_RESPONSE
            );

            if (!isHighFrequencyNoise) {
                Logger::info(QObject::tr("Broker: Mesaj tip=") + QString::number(mType)
                             + QObject::tr(" from=") + conn->peerAddress());
            }

            // ── Güvenlik Kontrolü: Kara Liste (Revocation) ──────────────────────
            QString peerId;
            QSslCertificate peerCert = conn->sslSocket() ? conn->sslSocket()->peerCertificate() : QSslCertificate();
            if (!peerCert.isNull()) {
                peerId = peerCert.subjectInfo(QSslCertificate::CommonName).join("");
            }

            if (!peerId.isEmpty() && ctx->registry->isRevoked(peerId)) {
                AgentRecord blacklisted = OkidiaDatabase::instance().blacklistedAgent(peerId);
                if (blacklisted.statusReason == "PENDING_UNINSTALL") {
                    Logger::warning(QObject::tr("Broker: Kara listedeki sertifika UNINSTALL bekliyor → Emir iletilip kesiliyor: ") + peerId);
                    okidia::Envelope unEnv;
                    unEnv.set_type(okidia::MessageType::CLIENT_UNINSTALL);
                    okidia::ClientRevokeCommand cmd;
                    cmd.set_machine_id(peerId.toStdString());
                    cmd.set_uninstall(true);
                    unEnv.set_payload(cmd.SerializeAsString());
                    conn->sendEnvelope(unEnv);
                    
                    // Pusu görevi yerine getirildi; cihaz kendini silecek, kara listeden temizle
                    ctx->registry->unrevokeClient(peerId);

                    // İşlemin başarıyla iletilebilmesi için biraz bekleyip kes
                    QTimer::singleShot(1000, conn, &Connection::disconnect);
                } else {
                    Logger::warning(QObject::tr("Broker: Kara listedeki sertifika ile işlem denemesi → Bağlantı kesiliyor: ") + peerId);
                    conn->disconnect();
                }
                return;
            }

            // Degrade invalid or inactive master certificates to anonymous instead of hard-disconnecting
            if (!peerId.isEmpty() && !peerId.startsWith("okidia-master-")) {
                UserRecord u = OkidiaDatabase::instance().userById(peerId);
                if (u.id.isEmpty() || !u.isActive) {
                    AgentRecord agent = OkidiaDatabase::instance().agent(peerId);
                    if (agent.machineId.isEmpty()) {
                        Logger::warning(QObject::tr("Broker: Kayıtlı olmayan veya pasif kullanıcı/agent sertifikası tespit edildi, anonim muamelesi yapılıyor: ") + peerId);
                        peerId = "";
                    }
                }
            }

            if (ctx->enrollmentManager->process(conn, env, peerId)) {
                return;
            }

            messageHandler->process(env, conn, peerId, isMaster);
        });

        QObject::connect(conn, &Connection::disconnected,
                         conn, [conn, ctx, isMaster]() {
            if (*isMaster) {
                if ((*ctx->connToRoomMembers).contains(conn)) {
                    for (const auto& pair : (*ctx->connToRoomMembers)[conn]) {
                        (*ctx->activeRoomMembers)[pair.first].remove(pair.second);
                        okidia::ChatMemberEvent leaveEvt;
                        leaveEvt.set_room_id(pair.first.toStdString());
                        leaveEvt.set_client_id(pair.second.toStdString());
                        okidia::Envelope leaveEnv;
                        leaveEnv.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
                        leaveEnv.set_payload(leaveEvt.SerializeAsString());
                        for (Connection* mConn : ctx->registry->masterConnections()) {
                            if (mConn != conn) mConn->sendEnvelope(leaveEnv);
                        }
                        if (pair.first != "room_admin") {
                            QString targetClassId = pair.first.startsWith("room_") ? pair.first.mid(5) : "";
                            for (const auto& c : ctx->registry->allClients()) {
                                const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                                if (targetClassId.isEmpty() || rec.classId == targetClassId) {
                                    ctx->registry->forwardToClient(c.clientId, leaveEnv);
                                }
                            }
                        }
                    }
                    (*ctx->connToRoomMembers).remove(conn);
                }
                ctx->registry->removeMasterConnection(conn);
                ctx->broadcastStatus();
            } else {
                for (const auto& c : ctx->registry->allClients()) {
                    if (c.connection == conn) {
                        (*ctx->agentWindowOpenMap).remove(c.clientId);
                        if (ctx->bellScheduler) {
                            ctx->bellScheduler->clearClientUnlockState(c.clientId);
                        }
                        const AgentRecord rec = OkidiaDatabase::instance().agent(c.machineId);
                        if (!rec.classId.isEmpty()) {
                            const QString roomKey = "room_" + rec.classId;
                            (*ctx->activeRoomMembers)[roomKey].remove(c.clientId);
                            if ((*ctx->connToRoomMembers).contains(conn)) {
                                (*ctx->connToRoomMembers).remove(conn);
                            }
                            okidia::ChatMemberEvent leaveEvt;
                            leaveEvt.set_room_id(roomKey.toStdString());
                            leaveEvt.set_client_id(c.clientId.toStdString());
                            leaveEvt.set_hostname(rec.hostname.toStdString());
                            leaveEvt.set_username("AC");
                            okidia::Envelope leaveEnv;
                            leaveEnv.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
                            leaveEnv.set_payload(leaveEvt.SerializeAsString());

                            for (const auto& otherC : ctx->registry->allClients()) {
                                if (otherC.connection != conn) {
                                    const AgentRecord otherRec = OkidiaDatabase::instance().agent(otherC.machineId);
                                    if (otherRec.classId == rec.classId) {
                                        ctx->registry->forwardToClient(otherC.clientId, leaveEnv);
                                    }
                                }
                            }
                            for (Connection* mConn : ctx->registry->masterConnections()) {
                                mConn->sendEnvelope(leaveEnv);
                            }
                        }
                        break;
                    }
                }
                ctx->registry->unregisterByConnection(conn);
                ctx->registry->removePendingByConnection(conn);
                ctx->broadcastStatus();
            }
            delete isMaster;
            conn->deleteLater();
        });
    });

    Logger::info(QObject::tr("Broker: Hazır, bağlantı bekleniyor..."));
    PlatformService::notifyReady();

#if defined(Q_OS_WIN)
    PlatformService::ensureTrayRunning("okidia-broker-tray.exe");
#endif

    // Heartbeat watchdog: 90 sn heartbeat gelmezse bağlantıyı kapat
    // (Agent 15 sn'de bir heartbeat gönderir; 90 sn = 6 kaçırılmış heartbeat)
    registry->startHeartbeatWatchdog(90);

    // Tray'e periyodik durum gönder (tray geç bağlanabilir)
    auto* statusTimer = new QTimer(&app);
    statusTimer->setInterval(3000);
    QObject::connect(statusTimer, &QTimer::timeout, [&]() { broadcastStatus(); });
    statusTimer->start();

    // 28 günden eski logları periyodik temizleme (6 saatte bir kontrol)
    auto* logPurgeTimer = new QTimer(&app);
    logPurgeTimer->setInterval(6 * 3600 * 1000);
    QObject::connect(logPurgeTimer, &QTimer::timeout, []() {
        Logger::purgeOldLogsAsync();
    });
    logPurgeTimer->start();

    // ── Dinamik Ağ Arayüzü İzleyicisi (USB Ethernet Hotplug / Link Change Guard) ──
    // Broker çalışırken sonradan takılan USB Ethernet adaptörlerini veya değişen IP'leri
    // tespit ederek MdnsManager multicast gruplarını dinamik günceller; sökülen bacakları temizler.
    auto* ifaceWatchTimer = new QTimer(&app);
    ifaceWatchTimer->setInterval(4000); // 4 saniyede bir arayüz durumunu tara
    static QString s_lastIfaceSnapshot;

    auto buildIfaceSnapshot = []() -> QString {
        QStringList parts;
        const auto ifaces = QNetworkInterface::allInterfaces();
        for (const auto& iface : ifaces) {
            if (!iface.flags().testFlag(QNetworkInterface::IsUp) ||
                !iface.flags().testFlag(QNetworkInterface::IsRunning) ||
                iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
                continue;
            }
            QString entry = iface.name() + ":";
            for (const auto& addr : iface.addressEntries()) {
                if (addr.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    entry += addr.ip().toString() + ",";
                }
            }
            parts << entry;
        }
        parts.sort();
        return parts.join(";");
    };
    s_lastIfaceSnapshot = buildIfaceSnapshot();

    QObject::connect(ifaceWatchTimer, &QTimer::timeout, [&app, mdns, buildIfaceSnapshot]() {
        QString current = buildIfaceSnapshot();
        if (current != s_lastIfaceSnapshot) {
            Logger::info(QObject::tr("Broker: Ağ arayüzü değişikliği tespit edildi (USB hotplug / IP değişimi) → ") + current);
            s_lastIfaceSnapshot = current;
            mdns->refreshInterfaces();
        }
    });
    ifaceWatchTimer->start();

    int exitCode = app.exec();
    server->stopListening();
    delete server;
    delete ctx;
    return exitCode;
}

int main(int argc, char* argv[]) {
#if defined(Q_OS_WIN)
    if (PlatformService::isRunningAsService()) {
        return PlatformService::runAsService(QStringLiteral("OkidiaBroker"), runBrokerApp, argc, argv) ? 0 : 1;
    }
#endif
    return runBrokerApp(argc, argv);
}
