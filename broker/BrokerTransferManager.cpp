#include "BrokerTransferManager.h"
#include "../common/utils/Logger.h"
#include "../common/OkidiaConfig.h"
#include "../common/db/OkidiaDatabase.h"
#include <QRandomGenerator>
#include <QRegularExpression>
#include "../common/utils/OkidiaUtils.h"

BrokerTransferManager::BrokerTransferManager(ClientRegistry* registry, QObject* parent)
    : QObject(parent), m_registry(registry)
{
    m_updatesDir = OkidiaConfig::dbDir() + "/updates";
    QDir dir(m_updatesDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 24 saatte bir çalışan otomatik OTA temizlik zamanlayıcısı (90 günden eski sürümler)
    m_cleanupTimer = new QTimer(this);
    m_cleanupTimer->setInterval(24 * 60 * 60 * 1000);
    connect(m_cleanupTimer, &QTimer::timeout, this, [this]() {
        cleanOldOtaPackages(90);
    });
    m_cleanupTimer->start();

    // 10 dakikada bir bağlı Master'ların güncel olup olmadığını kontrol eden zamanlayıcı
    m_periodicMasterCheckTimer = new QTimer(this);
    m_periodicMasterCheckTimer->setInterval(10 * 60 * 1000);
    connect(m_periodicMasterCheckTimer, &QTimer::timeout, this, [this]() {
        notifyConnectedMastersIfUpdateAvailable();
    });
    m_periodicMasterCheckTimer->start();

    // Servis açıldıktan 5 saniye sonra ilk kontrolleri yap
    QTimer::singleShot(5000, this, [this]() {
        cleanOldOtaPackages(90);
        notifyConnectedMastersIfUpdateAvailable();
    });

    if (m_registry) {
        connect(m_registry, &ClientRegistry::clientRegistered, this, &BrokerTransferManager::onClientRegistered);
    }
}

BrokerTransferManager::~BrokerTransferManager() {
    for (auto* transfer : m_transfers.values()) {
        if (transfer->file) {
            transfer->file->close();
            delete transfer->file;
        }
        delete transfer;
    }
    m_transfers.clear();

    for (auto* at : m_assetTransfers.values()) {
        if (at->file) {
            at->file->close();
            delete at->file;
        }
        delete at;
    }
    m_assetTransfers.clear();
}

bool BrokerTransferManager::handleOtaBegin(Connection* sender, const okidia::OtaUpdateBegin& begin) {
    if (m_transfers.contains(sender)) {
        auto* old = m_transfers.take(sender);
        if (old->file) { old->file->close(); delete old->file; }
        delete old;
    }

    QString filename = QString::fromStdString(begin.filename());
    // Güvenlik: dosya yolundaki '/' ve '\' karakterlerini engelle
    filename = filename.split('/').last().split('\\').last();

    ActiveTransfer* t = new ActiveTransfer;
    t->osTag = QString::fromStdString(begin.os_tag());
    t->version = QString::fromStdString(begin.version());
    t->filename = filename;
    t->expectedSize = begin.file_size();
    t->receivedSize = 0;
    t->signature = QByteArray(begin.signature().data(), begin.signature().size());
    t->file_hash = QByteArray(begin.file_hash().data(), begin.file_hash().size());
    t->master_cert = QByteArray(begin.master_cert().data(), begin.master_cert().size());
    
    QString fullPath = m_updatesDir + "/" + filename;
    t->file = new QFile(fullPath);
    if (!t->file->open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("Broker: OTA dosyası oluşturulamadı: ") + fullPath);
        delete t->file;
        delete t;
        return false;
    }

    m_transfers.insert(sender, t);
    Logger::info(QObject::tr("Broker: OTA aktarımı başladı (") + filename + QObject::tr("), boyut: ") + QString::number(t->expectedSize));
    return true;
}

#include <QProcess>

bool BrokerTransferManager::handleOtaChunk(Connection* sender, const okidia::OtaUpdateChunk& chunk) {
    if (!m_transfers.contains(sender)) return false;
    
    ActiveTransfer* t = m_transfers[sender];
    if (!t->file || !t->file->isOpen()) return false;

    QByteArray data(chunk.data().data(), chunk.data().size());
    qint64 written = t->file->write(data);
    if (written != data.size()) {
        Logger::error(QObject::tr("Broker: OTA chunk yazılamadı."));
        return false;
    }
    t->receivedSize += written;

    if (chunk.is_last()) {
        t->file->close();
        Logger::info(QObject::tr("Broker: OTA dosyası başarıyla alındı (") + t->filename + ")");
        
        // Güvenlik meta verisini kaydet
        QFile metaFile(t->file->fileName() + ".meta");
        if (metaFile.open(QIODevice::WriteOnly)) {
            QDataStream out(&metaFile);
            out.setVersion(QDataStream::Qt_5_15);
            out << t->signature << t->file_hash << t->master_cert;
            metaFile.close();
        } else {
            Logger::error(QObject::tr("Broker: OTA meta dosyası yazılamadı: ") + metaFile.fileName());
        }

        if (t->filename.startsWith("okidia-master")) {
            broadcastMasterUpdateAvailable(t->osTag, t->version, t->filename, t->expectedSize,
                                           t->signature, t->file_hash, t->master_cert);
        } else {
            broadcastOtaPush(t->osTag, t->version, t->expectedSize);
        }
        
        delete t->file;
        delete t;
        m_transfers.remove(sender);
        return true;
    }
    return false;
}

bool BrokerTransferManager::handleOtaEnd(Connection* sender, const okidia::Envelope& /*env*/) {
    if (!m_transfers.contains(sender)) return false;
    
    ActiveTransfer* t = m_transfers.take(sender);
    if (t->file) {
        t->file->close();
        delete t->file;
    }
    delete t;
    Logger::warning(QObject::tr("Broker: OTA transferi erken sonlandırıldı (OTA_UPDATE_END alındı ama last chunk değildi)."));
    return true;
}

bool BrokerTransferManager::handleBrokerOtaBegin(Connection* sender, const okidia::OtaUpdateBegin& begin) {
    if (m_transfers.contains(sender)) {
        auto* old = m_transfers.take(sender);
        if (old->file) { old->file->close(); delete old->file; }
        delete old;
    }

    ActiveTransfer* t = new ActiveTransfer;
    t->osTag = QString::fromStdString(begin.os_tag());
    t->version = QString::fromStdString(begin.version());
#if defined(Q_OS_WIN)
    t->filename = "okidia-broker-setup.exe";
    QString fullPath = QDir::tempPath() + "/okidia-broker-setup.exe";
#else
    t->filename = "okidia-broker-ota.deb";
    QString fullPath = "/tmp/okidia-broker-ota.deb";
#endif
    t->expectedSize = begin.file_size();
    t->receivedSize = 0;
    t->signature = QByteArray(begin.signature().data(), begin.signature().size());
    t->file_hash = QByteArray(begin.file_hash().data(), begin.file_hash().size());
    t->master_cert = QByteArray(begin.master_cert().data(), begin.master_cert().size());
    t->isBrokerSelfUpdate = true;

    t->file = new QFile(fullPath);
    if (!t->file->open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("Broker: Broker OTA dosyası oluşturulamadı: ") + fullPath);
        delete t->file;
        delete t;
        return false;
    }

    m_transfers.insert(sender, t);
    Logger::info(QObject::tr("Broker: Broker OTA aktarımı başladı, boyut: ") + QString::number(t->expectedSize));
    return true;
}

bool BrokerTransferManager::handleBrokerOtaChunk(Connection* sender, const okidia::OtaUpdateChunk& chunk) {
    if (!m_transfers.contains(sender)) return false;

    ActiveTransfer* t = m_transfers[sender];
    if (!t->file || !t->file->isOpen()) return false;

    QByteArray data(chunk.data().data(), chunk.data().size());
    qint64 written = t->file->write(data);
    if (written != data.size()) {
        Logger::error(QObject::tr("Broker: Broker OTA chunk yazılamadı."));
        return false;
    }
    t->receivedSize += written;

    if (chunk.is_last()) {
        t->file->close();
        Logger::info(QObject::tr("Broker: Broker OTA paketi başarıyla alındı (") + t->file->fileName() + ")");

        // Master'a rapor gönder
        okidia::OtaBrokerReport report;
        report.set_message(QObject::tr("Broker OTA paketi başarıyla alındı. Sunucu otonom olarak güncelleniyor ve yeniden başlatılıyor...").toStdString());
        report.set_success(true);

        okidia::Envelope env;
        env.set_type(okidia::MessageType::OTA_BROKER_REPORT);
        env.set_payload(report.SerializeAsString());
        sender->sendEnvelope(env);

#if defined(Q_OS_WIN)
        QString fullPath = t->file->fileName();
        QString batchPath = QDir::tempPath() + "/okidia_broker_ota.bat";
        QString otaLogPath = QDir::toNativeSeparators(QDir::tempPath() + "/okidia_broker_ota.log");
        QString innoLogPath = QDir::toNativeSeparators(QDir::tempPath() + "/okidia_broker_inno.log");
        QFile bf(batchPath);
        if (bf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&bf);
            ts << "@echo off\r\n";
            ts << "setlocal enabledelayedexpansion\r\n";
            ts << "echo [%date% %time%] Broker OTA baslatildi. >> \"" << otaLogPath << "\"\r\n";
            ts << "echo [%date% %time%] Paket: \"" << QDir::toNativeSeparators(fullPath) << "\" >> \"" << otaLogPath << "\"\r\n";
            ts << "echo [%date% %time%] Servis ve surecler durduruluyor... >> \"" << otaLogPath << "\"\r\n";
            ts << "net stop OkidiaBroker /y >> \"" << otaLogPath << "\" 2>&1\r\n";
            ts << "sc stop OkidiaBroker >> \"" << otaLogPath << "\" 2>&1\r\n";
            ts << "taskkill /F /IM okidia-broker.exe >> \"" << otaLogPath << "\" 2>&1\r\n";
            ts << "taskkill /F /IM okidia-broker-tray.exe >> \"" << otaLogPath << "\" 2>&1\r\n";
            ts << "powershell -NoProfile -Command \"Stop-Process -Name okidia-broker, okidia-broker-tray -Force -ErrorAction SilentlyContinue\" >> \"" << otaLogPath << "\" 2>&1\r\n";
            ts << "timeout /t 2 /nobreak >nul\r\n";
            ts << "echo [%date% %time%] Kurulum baslatiliyor (start /wait)... >> \"" << otaLogPath << "\"\r\n";
            ts << "start \"\" /wait \"" << QDir::toNativeSeparators(fullPath) << "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=\"" << innoLogPath << "\"\r\n";
            ts << "set SETUP_EXIT=!errorlevel!\r\n";
            ts << "echo [%date% %time%] Kurulum tamamlandi. Exit code: !SETUP_EXIT! >> \"" << otaLogPath << "\"\r\n";
            ts << "echo [%date% %time%] Servis baslatiliyor... >> \"" << otaLogPath << "\"\r\n";
            ts << "sc start OkidiaBroker >> \"" << otaLogPath << "\" 2>&1\r\n";
            ts << "echo [%date% %time%] Temizlik yapiliyor... >> \"" << otaLogPath << "\"\r\n";
            ts << "del /f /q \"" << QDir::toNativeSeparators(fullPath) << "\"\r\n";
            ts << "del /f /q \"%~f0\"\r\n";
            bf.close();
        }
        // WMI (Win32_Process.Create) ile servisin süreç ağacından bağımsız başlat
        QString nativeBatch = QDir::toNativeSeparators(batchPath);
        QString psScript = QString("Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='cmd.exe /c \"\"%1\"\"'}").arg(nativeBatch);
        bool launched = QProcess::startDetached("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << psScript);
        if (!launched) {
            QProcess::startDetached("cmd.exe", QStringList() << "/C" << nativeBatch);
        }
        Logger::info(QObject::tr("Broker: Windows otonom OTA kurulum betiği tetiklendi: ") + batchPath);
#else
        // Otonom systemd güncelleme betiğini başlat (SSH düzeyinde bağımlılık onarımı ve birim çakışmasız)
        QString cmd = "sleep 2; for i in 1 2 3 4 5; do dpkg -i /tmp/okidia-broker-ota.deb && break; apt-get install -f -y; dpkg -i /tmp/okidia-broker-ota.deb && break; sleep 3; done; systemctl is-active --quiet okidia-broker.service || systemctl restart okidia-broker.service; rm -f /tmp/okidia-broker-ota.deb";
        QProcess::startDetached("systemd-run", QStringList() 
            << "--description=OKIDIA Broker OTA Installer" 
            << "--collect"
            << "bash" << "-c" << cmd);
#endif

        delete t->file;
        delete t;
        m_transfers.remove(sender);
        return true;
    }
    return false;
}

bool BrokerTransferManager::handleBrokerOtaEnd(Connection* sender, const okidia::Envelope& env) {
    return handleOtaEnd(sender, env);
}

void BrokerTransferManager::sendBrokerReportToMasters(const QString& message, bool success) {
    okidia::OtaBrokerReport report;
    report.set_message(message.toStdString());
    report.set_success(success);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::OTA_BROKER_REPORT);
    env.set_payload(report.SerializeAsString());

    for (Connection* mConn : m_registry->masterConnections()) {
        if (mConn && mConn->isConnected()) {
            mConn->sendEnvelope(env);
        }
    }
}

void BrokerTransferManager::onClientRegistered(const QString& clientId) {
    // Not: ClientHandler::handle içinde registerClient çağrıldıktan hemen sonra
    // veritabanı güncellenir ve checkAndSendUpdateToAgent(...) çağrılır.
    // Gerçek sürüm doğrulaması ve aktif OTA yuvasının serbest bırakılması
    // checkAndSendUpdateToAgent içinde yeni sürüm teyit edildiğinde yapılır.
    if (m_activeOtaClients.contains(clientId) && !m_sentAgentUpdates.contains(clientId)) {
        m_activeOtaClients.remove(clientId);
        processOtaQueue();
    }
}

void BrokerTransferManager::onOtaQueueTimeout() {
    processOtaQueue();
}

void BrokerTransferManager::broadcastOtaPush(const QString& osTag, const QString& version, uint64_t /*fileSize*/) {
    const QString targetOsTag = OkidiaUtils::normalizeOsTag(osTag);
    int newlyAdded = 0;

    const auto allAgents = m_registry->allClients();
    for (const auto& client : allAgents) {
        if (client.connection && client.connection->isConnected()) {
            AgentRecord rec = OkidiaDatabase::instance().agent(client.clientId);
            QString agentOsTag = OkidiaUtils::normalizeOsTag(rec.osInfo.isEmpty() ? rec.osPrettyName : rec.osInfo);
            QString agentOsArch = OkidiaUtils::normalizeArch(rec.osArch);
            QString agentVersion = rec.agentVer;

            if (agentVersion.isEmpty()) agentVersion = "0.0.0"; // zorla güncelleme denemesi

            // Güvenlik ve uyumluluk: Eğer istemcinin işletim sistemi veya mimarisi bilinmiyorsa GÜNCELLEME YAPMA!
            if (agentOsTag.isEmpty() || agentOsTag == "unknown") {
                Logger::warning(QObject::tr("Broker: İstemcinin (%1) OS bilgisi bilinmiyor. OTA güncellemesi atlanıyor.").arg(client.hostname));
                continue;
            }
            if (agentOsArch.isEmpty() || agentOsArch == "unknown") {
                Logger::warning(QObject::tr("Broker: İstemcinin (%1) OS Mimari bilgisi bilinmiyor. OTA güncellemesi atlanıyor.").arg(client.hostname));
                continue;
            }

            // Yalnızca yüklenen paketin işletim sistemiyle uyumlu olan istemcileri kuyruğa ekle
            if (!targetOsTag.isEmpty() && targetOsTag != "unknown" &&
                !OkidiaUtils::isOsPackageCompatible(targetOsTag, agentOsTag)) {
                continue;
            }

            // İstemci zaten bu sürümde veya daha yüksek bir sürümdeyse kuyruğa ekleme
            if (!version.isEmpty() && agentVersion != "0.0.0" &&
                OkidiaUtils::compareVersions(agentVersion, version) >= 0) {
                continue;
            }

            bool alreadyQueued = m_activeOtaClients.contains(client.clientId);
            if (!alreadyQueued) {
                for (const auto& qItem : m_pendingOtaQueue) {
                    if (qItem.clientId == client.clientId) {
                        alreadyQueued = true;
                        break;
                    }
                }
            }
            if (!alreadyQueued) {
                m_pendingOtaQueue.append({client.clientId, agentOsTag, agentOsArch, agentVersion, client.hostname});
                newlyAdded++;
            }
        }
    }

    m_totalOtaTargets += newlyAdded;
    Logger::info(QObject::tr("Broker: %1 istemci OTA kuyruğuna eklendi (Toplam hedef: %2). %3'lü gruplar halinde sırayla dağıtılacak. Hedef OS_TAG: %4")
                 .arg(newlyAdded).arg(m_totalOtaTargets).arg(kMaxConcurrentOta).arg(targetOsTag));

    if (newlyAdded > 0) {
        sendBrokerReportToMasters(
            QObject::tr("Broker Bildirimi: %1 istemci (%2) için güncelleme kuyruğu oluşturuldu (%3'lü gruplar halinde dağıtılıyor)...")
            .arg(newlyAdded).arg(targetOsTag).arg(kMaxConcurrentOta),
            true
        );
    } else if (m_totalOtaTargets == 0) {
        sendBrokerReportToMasters(
            QObject::tr("Broker Bildirimi: Paket (%1 - v%2) sunucu güncelleme deposuna kaydedildi. Çevrimdışı istemciler açıldıklarında otonom güncellenecektir.")
            .arg(targetOsTag, version),
            true
        );
    }

    processOtaQueue();
}

void BrokerTransferManager::processOtaQueue() {
    qint64 now = QDateTime::currentSecsSinceEpoch();

    // Zaman aşımına uğrayan aktif gönderimleri temizle (kOtaWaveTimeoutSec kuralı)
    for (auto it = m_activeOtaClients.begin(); it != m_activeOtaClients.end();) {
        if ((now - it.value()) > kOtaWaveTimeoutSec) {
            Logger::warning(QObject::tr("Broker: İstemci %1 için OTA güncelleme zaman aşımı (%2 sn). Sonraki istemciye geçiliyor.")
                            .arg(it.key()).arg(kOtaWaveTimeoutSec));
            it = m_activeOtaClients.erase(it);
        } else {
            ++it;
        }
    }

    // Aktif gönderim yuvalarını (kMaxConcurrentOta = 4) doldur
    while (m_activeOtaClients.size() < kMaxConcurrentOta && !m_pendingOtaQueue.isEmpty()) {
        QueuedOtaTarget target = m_pendingOtaQueue.takeFirst();
        RegisteredClient client = m_registry->client(target.clientId);
        if (!client.connection || !client.connection->isConnected()) {
            Logger::warning(QObject::tr("Broker: Kuyruktaki istemci (%1) çevrimdışı, atlanıyor.").arg(target.hostname));
            continue;
        }

        m_activeOtaClients.insert(target.clientId, now);
        Logger::info(QObject::tr("Broker: Grup OTA paketi aktarılıyor → %1 (%2) [Kalan kuyruk: %3, Aktif grup: %4/%5]")
                     .arg(target.hostname, target.clientId)
                     .arg(m_pendingOtaQueue.size())
                     .arg(m_activeOtaClients.size())
                     .arg(kMaxConcurrentOta));

        checkAndSendUpdateToAgent(client.connection, target.osTag, target.osArch, target.agentVersion, target.clientId, true);
    }

    if (!m_pendingOtaQueue.isEmpty() || !m_activeOtaClients.isEmpty()) {
        if (!m_otaQueueTimer) {
            m_otaQueueTimer = new QTimer(this);
            connect(m_otaQueueTimer, &QTimer::timeout, this, &BrokerTransferManager::onOtaQueueTimeout);
        }
        if (!m_otaQueueTimer->isActive()) {
            m_otaQueueTimer->start(3000); // 3 saniyede bir kuyruk ve zaman aşımı denetimi
        }
    } else {
        if (m_otaQueueTimer && m_otaQueueTimer->isActive()) {
            m_otaQueueTimer->stop();
        }
        if (m_totalOtaTargets > 0) {
            Logger::info(QObject::tr("Broker: Tüm istemcilerin OTA grup dağıtımı başarıyla tamamlandı (Toplam: %1).").arg(m_totalOtaTargets));
            sendBrokerReportToMasters(
                QObject::tr("Broker Bildirimi: Tüm istemcilere OTA paket dağıtımı tamamlandı (Toplam: %1 istemci).").arg(m_totalOtaTargets),
                true
            );
            m_totalOtaTargets = 0;
        }
    }
}

void BrokerTransferManager::checkAndSendUpdateToAgent(Connection* agentConn, const QString& osTag, const QString& osArch, const QString& currentVersion, const QString& clientId, bool force) {
    QString normTag = OkidiaUtils::normalizeOsTag(osTag);
    QString normArch = OkidiaUtils::normalizeArch(osArch);
    if (normTag.isEmpty() || normTag == "unknown" || normArch.isEmpty() || normArch == "unknown" || !agentConn) {
        if (!clientId.isEmpty()) m_activeOtaClients.remove(clientId);
        return;
    }
    
    QDir dir(m_updatesDir);
    QFileInfoList allFiles = dir.entryInfoList(QDir::Files, QDir::Time);
    if (allFiles.isEmpty()) {
        if (!clientId.isEmpty()) m_activeOtaClients.remove(clientId);
        return;
    }
    
    QRegularExpression regex(OkidiaUtils::getAgentPackageRegex(normTag, normArch));
    QFileInfo newest;
    QString latestVer;
    
    for (const QFileInfo& fi : allFiles) {
        if (regex.match(fi.fileName()).hasMatch()) {
            QString candVer = OkidiaUtils::extractPackageVersion(fi.fileName());
            if (candVer.isEmpty()) continue;
            if (!newest.exists() || OkidiaUtils::compareVersions(candVer, latestVer) > 0) {
                newest = fi;
                latestVer = candVer;
            }
        }
    }
    
    if (!newest.exists() || latestVer.isEmpty()) {
        if (!clientId.isEmpty()) m_activeOtaClients.remove(clientId);
        return;
    }
    QString filename = newest.fileName();
    
    QString cur = currentVersion;
    if (cur.startsWith("v") || cur.startsWith("V")) cur = cur.mid(1);
    
    QString clientKey = clientId.isEmpty() ? agentConn->peerAddress() : clientId;

    if (OkidiaUtils::compareVersions(latestVer, cur) <= 0) {
        // İstemci zaten bu sürüme veya daha yenisine geçmiş, varsa eski kayıtları temizle
        if (!clientKey.isEmpty()) {
            m_sentAgentUpdates.remove(clientKey);
        }
        if (!clientId.isEmpty() && m_activeOtaClients.remove(clientId) > 0) {
            Logger::info(QObject::tr("Broker: İstemci %1 güncellendi ve yeni sürümle (%2) bağlandı. Sıradaki grup istemcisi tetikleniyor.").arg(clientId, currentVersion));
            if (!force) {
                processOtaQueue();
            }
        }
        return; // Broker'daki paket eski veya aynı, güncelleme gönderme
    }
    
    // Throttling / Yeniden gönderim sıklık koruması:
    // Eğer aynı istemciye aynı hedef sürüm son 5 dakika (300 sn) içinde zaten gönderildiyse ve zorlama (force) yoksa tekrar gönderme
    if (!force && !clientKey.isEmpty() && m_sentAgentUpdates.contains(clientKey)) {
        const auto& lastSent = m_sentAgentUpdates.value(clientKey);
        if (lastSent.targetVersion == latestVer) {
            qint64 now = QDateTime::currentSecsSinceEpoch();
            qint64 elapsed = now - lastSent.timestampSec;
            if (elapsed < 300) {
                return; // Tekrar gönderimi ve log kirliliğini engelle
            }
        }
    }

    Logger::info(QObject::tr("Broker: İstemci için yeni sürüm bulundu. İstemci: ") + currentVersion + QObject::tr(", Yeni: ") + latestVer + QObject::tr(" -> Dosya gönderiliyor: ") + filename);

    if (!clientKey.isEmpty()) {
        SentUpdateRecord rec;
        rec.targetVersion = latestVer;
        rec.timestampSec = QDateTime::currentSecsSinceEpoch();
        rec.attemptCount = m_sentAgentUpdates.value(clientKey).attemptCount + 1;
        m_sentAgentUpdates[clientKey] = rec;
    }
    
    QFile file(newest.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (!clientId.isEmpty()) m_activeOtaClients.remove(clientId);
        return;
    }
    
    uint64_t fileSize = file.size();
    
    // 1. FILE_TRANSFER_BEGIN
    okidia::FileTransferBegin beginMsg;
    beginMsg.set_filename(filename.toStdString());
    beginMsg.set_file_size(fileSize);
    beginMsg.set_mime_type("application/vnd.debian.binary-package");
    beginMsg.set_read_only(false);
    beginMsg.set_destination("tmp");
    
    okidia::Envelope beginEnv;
    beginEnv.set_type(okidia::MessageType::FILE_TRANSFER_BEGIN);
    uint64_t seqId = QRandomGenerator::global()->generate64();
    beginEnv.set_sequence_id(seqId);
    beginEnv.set_payload(beginMsg.SerializeAsString());
    agentConn->sendEnvelope(beginEnv);
    
    // 2. Chunks (64 KB)
    uint32_t chunkIdx = 0;
    while (!file.atEnd()) {
        QByteArray chunkData = file.read(64 * 1024);
        bool isLast = file.atEnd();
        
        okidia::FileTransferChunk chunkMsg;
        chunkMsg.set_chunk_index(chunkIdx++);
        chunkMsg.set_data(chunkData.constData(), chunkData.size());
        chunkMsg.set_is_last(isLast);
        
        okidia::Envelope chunkEnv;
        chunkEnv.set_type(okidia::MessageType::FILE_TRANSFER_CHUNK);
        chunkEnv.set_sequence_id(seqId);
        chunkEnv.set_payload(chunkMsg.SerializeAsString());
        agentConn->sendEnvelope(chunkEnv);
    }
    file.close();
    
    // 3. FILE_TRANSFER_END
    okidia::Envelope endEnv;
    endEnv.set_type(okidia::MessageType::FILE_TRANSFER_END);
    endEnv.set_sequence_id(seqId);
    agentConn->sendEnvelope(endEnv);

    // 4. OTA_UPDATE_PUSH ile imza/sertifika vb. gönder
    okidia::OtaUpdatePush pushMsg;
    pushMsg.set_os_tag(osTag.toStdString());
    pushMsg.set_version(latestVer.toStdString());
    pushMsg.set_file_size(fileSize);

    QFile metaFile(newest.absoluteFilePath() + ".meta");
    if (metaFile.open(QIODevice::ReadOnly)) {
        QDataStream in(&metaFile);
        in.setVersion(QDataStream::Qt_5_15);
        QByteArray signature, fileHash, masterCert;
        in >> signature >> fileHash >> masterCert;
        
        pushMsg.set_signature(signature.constData(), signature.size());
        pushMsg.set_file_hash(fileHash.constData(), fileHash.size());
        pushMsg.set_master_cert(masterCert.constData(), masterCert.size());
    } else {
        Logger::error(QObject::tr("Broker: Meta dosyası okunamadı (") + metaFile.fileName() + QObject::tr("), istemci kurulum yapamayabilir."));
    }

    okidia::Envelope pushEnv;
    pushEnv.set_type(okidia::MessageType::OTA_UPDATE_PUSH);
    pushEnv.set_payload(pushMsg.SerializeAsString());
    agentConn->sendEnvelope(pushEnv);
}

void BrokerTransferManager::handleUpdatesListReq(Connection* sender, const okidia::Envelope& env) {
    QDir dir(m_updatesDir);
    Logger::info(QObject::tr("Broker: handleUpdatesListReq basladi. Hedef klasör: ") + m_updatesDir);
    
    QFileInfoList allFiles = dir.entryInfoList(QStringList() << "*.deb" << "*.exe" << "*.zip" << "*.tar.gz", QDir::Files, QDir::Time);
    Logger::info(QObject::tr("Broker: Klasorde ") + QString::number(allFiles.size()) + QObject::tr(" adet paket dosyasi bulundu."));
    
    okidia::BrokerUpdatesListResponse res;
    
    for (const QFileInfo& fi : allFiles) {
        okidia::BrokerUpdatePackage* pkg = res.add_packages();
        pkg->set_filename(fi.fileName().toStdString());
        pkg->set_size(fi.size());
        pkg->set_timestamp(fi.lastModified().toSecsSinceEpoch());
        
        // Versiyonu çıkar (örneğin okidia-agent_ubuntu24_0.1.0_amd64.deb, okidia-master_0.1.0_amd64.deb veya okidia-agent-setup_windows_0.1.0_x86_64.exe)
        QString filename = fi.fileName();
        QRegularExpression verRegex(R"((\d+\.\d+(?:\.\d+)?))");
        auto match = verRegex.match(filename);
        QString ver = match.hasMatch() ? match.captured(1) : "Bilinmiyor";
        pkg->set_version(ver.toStdString());
    }
    
    okidia::Envelope resEnv;
    resEnv.set_type(okidia::MessageType::BROKER_UPDATES_LIST_RES);
    resEnv.set_sequence_id(env.sequence_id());
    resEnv.set_payload(res.SerializeAsString());
    
    sender->sendEnvelope(resEnv);
    Logger::info(QObject::tr("Broker: BROKER_UPDATES_LIST_RES yaniti Master'a gonderildi. Paket sayisi: ") + QString::number(res.packages_size()));
}

void BrokerTransferManager::handleUpdatesDeleteReq(Connection* sender, const okidia::Envelope& env) {
    okidia::BrokerUpdatesDeleteRequest req;
    if (!req.ParseFromString(env.payload())) return;
    
    okidia::BrokerUpdatesDeleteResponse res;
    res.set_success(true);
    
    QDir dir(m_updatesDir);
    for (int i = 0; i < req.filenames_size(); ++i) {
        QString filename = QString::fromStdString(req.filenames(i));
        
        // Güvenlik önlemi: dizin dışına çıkmaya karşı
        if (filename.contains("/") || filename.contains("\\") || filename == ".." || filename == ".") {
            continue;
        }
        
        QString filePath = dir.absoluteFilePath(filename);
        QFile f(filePath);
        if (f.exists()) {
            if (f.remove()) {
                res.add_deleted_filenames(filename.toStdString());
            }
        }
        
        // Meta dosyasını da sil
        QString metaPath = filePath + ".meta";
        QFile mf(metaPath);
        if (mf.exists()) {
            mf.remove();
        }
    }
    
    okidia::Envelope resEnv;
    resEnv.set_type(okidia::MessageType::BROKER_UPDATES_DELETE_RES);
    resEnv.set_sequence_id(env.sequence_id());
    resEnv.set_payload(res.SerializeAsString());
    
    sender->sendEnvelope(resEnv);
}

void BrokerTransferManager::cleanOldOtaPackages(int maxAgeDays) {
    QDir dir(m_updatesDir);
    if (!dir.exists()) return;

    QFileInfoList allFiles = dir.entryInfoList(QStringList() << "*.deb" << "*.exe" << "*.zip" << "*.tar.gz", QDir::Files);
    if (allFiles.isEmpty()) return;

    // Hedef mimarilere ve paket türlerine göre grupla (Agent, Broker, Master x Linux/Windows/Pardus vb.)
    QMap<QString, QList<QFileInfo>> groups;
    for (const QFileInfo& fi : allFiles) {
        QString filename = fi.fileName();
        QStringList parts = filename.split('_');
        if (parts.size() >= 4) {
            QString groupKey = parts[0] + "_" + parts[1] + "_" + parts[3];
            groups[groupKey].append(fi);
        } else {
            groups[filename].append(fi);
        }
    }

    int deletedCount = 0;
    QDateTime now = QDateTime::currentDateTime();

    for (auto it = groups.begin(); it != groups.end(); ++it) {
        const QList<QFileInfo>& files = it.value();
        if (files.size() <= 1) continue; // Tek paket varsa kesinlikle koru (en güncel odur)

        // En son sürüm olan dosyayı bul (en yüksek semantik versiyon ve en yeni timestamp)
        QFileInfo latestFi = files[0];
        QString latestVer = "0.0.0";
        {
            QStringList p = latestFi.fileName().split('_');
            if (p.size() >= 3) latestVer = p[2];
        }

        for (int i = 1; i < files.size(); ++i) {
            const QFileInfo& fi = files[i];
            QString ver = "0.0.0";
            QStringList p = fi.fileName().split('_');
            if (p.size() >= 3) ver = p[2];

            if (OkidiaUtils::compareVersions(ver, latestVer) > 0 ||
               (OkidiaUtils::compareVersions(ver, latestVer) == 0 && fi.lastModified() > latestFi.lastModified())) {
                latestFi = fi;
                latestVer = ver;
            }
        }

        // En son sürüm olmayan (eski) paketleri denetle
        for (const QFileInfo& fi : files) {
            if (fi.absoluteFilePath() == latestFi.absoluteFilePath()) {
                continue; // En son sürüm ASLA silinmez!
            }

            qint64 ageDays = fi.lastModified().daysTo(now);
            if (ageDays >= maxAgeDays) {
                QString filename = fi.fileName();
                if (QFile::remove(fi.absoluteFilePath())) {
                    deletedCount++;
                    // Varsa meta dosyasını da temizle
                    QFile::remove(fi.absoluteFilePath() + ".meta");
                    Logger::info(QObject::tr("Broker: Eski OTA paketi otomatik temizlendi (%1 günlük): %2").arg(ageDays).arg(filename));
                }
            }
        }
    }

    if (deletedCount > 0) {
        Logger::info(QObject::tr("Broker: OTA öz-bakım tamamlandı. %1 adet 90 günden eski paket temizlendi.").arg(deletedCount));
    }
}

void BrokerTransferManager::broadcastMasterUpdateAvailable(const QString& osTag, const QString& version, const QString& filename, uint64_t fileSize,
                                                            const QByteArray& sig, const QByteArray& hash, const QByteArray& cert) {
    okidia::MasterUpdateAvailable msg;
    msg.set_version(version.toStdString());
    msg.set_filename(filename.toStdString());
    msg.set_file_size(fileSize);
    msg.set_os_tag(osTag.toStdString());

    QString arch = "amd64";
    if (filename.endsWith(".deb", Qt::CaseInsensitive)) {
        QString base = filename;
        base.chop(4);
        arch = base.split('_').last();
    }
    msg.set_os_arch(arch.toStdString());

    msg.set_signature(sig.constData(), sig.size());
    msg.set_file_hash(hash.constData(), hash.size());
    msg.set_master_cert(cert.constData(), cert.size());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::MASTER_UPDATE_AVAILABLE);
    env.set_payload(msg.SerializeAsString());

    for (Connection* mConn : m_registry->masterConnections()) {
        mConn->sendEnvelope(env);
    }
    Logger::info(QObject::tr("Broker: MASTER_UPDATE_AVAILABLE tüm bağlı Master'lara duyuruldu (v") + version + ")");
}

void BrokerTransferManager::checkAndNotifyMasterUpdate(Connection* masterConn, const QString& masterVersion, const QString& osTag, const QString& osArch) {
    if (!masterConn) return;

    QString tag = OkidiaUtils::normalizeOsTag(osTag);
    if (tag.isEmpty() || tag == "unknown") return;
    QString arch = OkidiaUtils::normalizeArch(osArch.isEmpty() ? "amd64" : osArch);

    QDir dir(m_updatesDir);
    QFileInfoList allFiles = dir.entryInfoList(QDir::Files, QDir::Time);
    if (allFiles.isEmpty()) return;

    QRegularExpression regex(OkidiaUtils::getMasterPackageRegex(tag, arch));
    QFileInfo newest;
    QString latestVer;

    for (const QFileInfo& fi : allFiles) {
        if (regex.match(fi.fileName()).hasMatch()) {
            QString candVer = OkidiaUtils::extractPackageVersion(fi.fileName());
            if (candVer.isEmpty()) continue;
            if (!newest.exists() || OkidiaUtils::compareVersions(candVer, latestVer) > 0) {
                newest = fi;
                latestVer = candVer;
            }
        }
    }

    if (!newest.exists() || latestVer.isEmpty()) return;

    QString filename = newest.fileName();
    QString cur = masterVersion;
    if (cur.startsWith("v") || cur.startsWith("V")) cur = cur.mid(1);

    if (OkidiaUtils::compareVersions(latestVer, cur) <= 0) {
        return; // Depodaki paket eski veya aynı
    }

    Logger::info(QObject::tr("Broker: Master (%1/%2) için yeni sürüm mevcut. Master: %3, Yeni: %4 (%5)")
                 .arg(tag, arch, masterVersion, latestVer, filename));

    okidia::MasterUpdateAvailable msg;
    msg.set_version(latestVer.toStdString());
    msg.set_filename(filename.toStdString());
    msg.set_file_size(newest.size());
    msg.set_os_tag(tag.toStdString());
    msg.set_os_arch(arch.toStdString());

    QFile metaFile(newest.absoluteFilePath() + ".meta");
    if (metaFile.open(QIODevice::ReadOnly)) {
        QDataStream in(&metaFile);
        in.setVersion(QDataStream::Qt_5_15);
        QByteArray sig, hash, cert;
        in >> sig >> hash >> cert;
        msg.set_signature(sig.constData(), sig.size());
        msg.set_file_hash(hash.constData(), hash.size());
        msg.set_master_cert(cert.constData(), cert.size());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::MASTER_UPDATE_AVAILABLE);
    env.set_payload(msg.SerializeAsString());
    masterConn->sendEnvelope(env);
}

void BrokerTransferManager::notifyConnectedMastersIfUpdateAvailable() {
    if (!m_registry) return;

    QDir dir(m_updatesDir);
    QFileInfoList allFiles = dir.entryInfoList(QDir::Files, QDir::Time);
    if (allFiles.isEmpty()) return;

    // Her (osTag, osArch) grubu için en yüksek versiyonlu Master paketini bul
    struct BestMasterPkg {
        QFileInfo fi;
        QString osTag;
        QString osArch;
        QString version;
    };
    QMap<QString, BestMasterPkg> bestByGroup;

    for (const QFileInfo& fi : allFiles) {
        QString fname = fi.fileName();
        if (!fname.startsWith("okidia-master", Qt::CaseInsensitive) || fname.endsWith(".meta", Qt::CaseInsensitive)) {
            continue;
        }
        QString ver = OkidiaUtils::extractPackageVersion(fname);
        if (ver.isEmpty()) continue;

        QString osTag;
        QString osArch = "amd64";
        if (fname.endsWith(".exe", Qt::CaseInsensitive)) {
            osTag = "windows";
        } else if (fname.endsWith(".deb", Qt::CaseInsensitive)) {
            QStringList parts = fname.split('_');
            if (parts.size() < 4) continue;
            osTag = OkidiaUtils::normalizeOsTag(parts[1]);
            osArch = parts[3];
            if (osArch.endsWith(".deb", Qt::CaseInsensitive)) osArch.chop(4);
            osArch = OkidiaUtils::normalizeArch(osArch);
        } else {
            continue;
        }

        QString groupKey = osTag + ":" + osArch;
        if (!bestByGroup.contains(groupKey) ||
            OkidiaUtils::compareVersions(ver, bestByGroup[groupKey].version) > 0) {
            bestByGroup[groupKey] = {fi, osTag, osArch, ver};
        }
    }

    for (auto it = bestByGroup.constBegin(); it != bestByGroup.constEnd(); ++it) {
        const BestMasterPkg& pkg = it.value();
        QByteArray sig, hash, cert;
        QFile metaFile(pkg.fi.absoluteFilePath() + ".meta");
        if (metaFile.open(QIODevice::ReadOnly)) {
            QDataStream in(&metaFile);
            in.setVersion(QDataStream::Qt_5_15);
            in >> sig >> hash >> cert;
        }
        broadcastMasterUpdateAvailable(pkg.osTag, pkg.version, pkg.fi.fileName(), pkg.fi.size(), sig, hash, cert);
    }
}


void BrokerTransferManager::handleMasterDownloadReq(Connection* sender, const okidia::MasterUpdateDownloadRequest& req) {
    if (!sender) return;

    QString osTag = OkidiaUtils::normalizeOsTag(QString::fromStdString(req.os_tag()));
    QString osArch = OkidiaUtils::normalizeArch(QString::fromStdString(req.os_arch()));
    if (osTag.isEmpty() || osTag == "unknown") osTag = "ubuntu24";
    if (osArch.isEmpty() || osArch == "unknown") osArch = "amd64";

    QDir dir(m_updatesDir);
    QFileInfoList allFiles = dir.entryInfoList(QDir::Files, QDir::Time);
    if (allFiles.isEmpty()) return;

    QRegularExpression regex(OkidiaUtils::getMasterPackageRegex(osTag, osArch));
    QFileInfo newest;
    QString latestVer;

    for (const QFileInfo& fi : allFiles) {
        if (regex.match(fi.fileName()).hasMatch()) {
            QString candVer = OkidiaUtils::extractPackageVersion(fi.fileName());
            if (candVer.isEmpty()) continue;
            if (!newest.exists() || OkidiaUtils::compareVersions(candVer, latestVer) > 0) {
                newest = fi;
                latestVer = candVer;
            }
        }
    }

    if (!newest.exists()) {
        Logger::warning(QObject::tr("Broker: Master için indirilecek paket bulunamadı (") + osTag + "/" + osArch + ")");
        return;
    }

    Logger::info(QObject::tr("Broker: Master'a paket aktarımı başlatılıyor: ") + newest.fileName());

    QFile file(newest.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    uint64_t fileSize = file.size();

    // 1. FILE_TRANSFER_BEGIN
    okidia::FileTransferBegin beginMsg;
    beginMsg.set_filename(newest.fileName().toStdString());
    beginMsg.set_file_size(fileSize);
    beginMsg.set_mime_type("application/vnd.debian.binary-package");
    beginMsg.set_read_only(false);
    beginMsg.set_destination("tmp");

    okidia::Envelope beginEnv;
    beginEnv.set_type(okidia::MessageType::FILE_TRANSFER_BEGIN);
    uint64_t seqId = QRandomGenerator::global()->generate64();
    beginEnv.set_sequence_id(seqId);
    beginEnv.set_payload(beginMsg.SerializeAsString());
    sender->sendEnvelope(beginEnv);

    // 2. Chunks (64 KB)
    uint32_t chunkIdx = 0;
    while (!file.atEnd()) {
        QByteArray chunkData = file.read(64 * 1024);
        bool isLast = file.atEnd();

        okidia::FileTransferChunk chunkMsg;
        chunkMsg.set_chunk_index(chunkIdx++);
        chunkMsg.set_data(chunkData.constData(), chunkData.size());
        chunkMsg.set_is_last(isLast);

        okidia::Envelope chunkEnv;
        chunkEnv.set_type(okidia::MessageType::FILE_TRANSFER_CHUNK);
        chunkEnv.set_sequence_id(seqId);
        chunkEnv.set_payload(chunkMsg.SerializeAsString());
        sender->sendEnvelope(chunkEnv);
    }
    file.close();

    // 3. FILE_TRANSFER_END
    okidia::Envelope endEnv;
    endEnv.set_type(okidia::MessageType::FILE_TRANSFER_END);
    endEnv.set_sequence_id(seqId);
    sender->sendEnvelope(endEnv);
}

bool BrokerTransferManager::handleAssetBegin(uint64_t seqId, const okidia::FileTransferBegin& begin) {
    if (m_assetTransfers.contains(seqId)) {
        auto* old = m_assetTransfers.take(seqId);
        if (old->file) {
            old->file->close();
            delete old->file;
        }
        delete old;
    }

    QString filename = QString::fromStdString(begin.filename());
    filename = QFileInfo(filename).fileName();
    if (filename.isEmpty()) filename = "asset_file";

    QString targetDir = OkidiaConfig::assetsDir();
    QDir().mkpath(targetDir);
    QString targetPath = targetDir + "/" + filename;
    QString partPath = targetPath + ".part";

    auto* t = new ActiveAssetTransfer;
    t->filename = filename;
    t->targetPath = targetPath;
    t->expectedSize = begin.file_size();
    t->receivedSize = 0;
    t->file = new QFile(partPath);

    if (!t->file->open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("Broker: Varlık dosyası açılamadı → %1").arg(partPath));
        delete t->file;
        delete t;
        return false;
    }

    m_assetTransfers.insert(seqId, t);
    Logger::info(QObject::tr("Broker: Varlık transferi başladı (seqId=%1, dosya=%2, boyut=%3 KB)")
                 .arg(seqId).arg(filename).arg(t->expectedSize / 1024));
    return true;
}

bool BrokerTransferManager::handleAssetChunk(uint64_t seqId, const okidia::FileTransferChunk& chunk) {
    auto* t = m_assetTransfers.value(seqId, nullptr);
    if (!t || !t->file || !t->file->isOpen()) return false;

    QByteArray data(chunk.data().data(), chunk.data().size());
    qint64 written = t->file->write(data);
    if (written != data.size()) {
        Logger::error(QObject::tr("Broker: Varlık chunk yazılamadı (seqId=%1)").arg(seqId));
        return false;
    }
    t->receivedSize += written;

    if (chunk.is_last()) {
        return handleAssetEnd(seqId);
    }
    return true;
}

bool BrokerTransferManager::handleAssetEnd(uint64_t seqId) {
    auto* t = m_assetTransfers.take(seqId);
    if (!t) return false;

    if (t->file) {
        if (t->file->isOpen()) {
            t->file->close();
        }
        QString partPath = t->file->fileName();
        delete t->file;
        t->file = nullptr;

        QFile::remove(t->targetPath);
        if (QFile::rename(partPath, t->targetPath)) {
            QFile::setPermissions(t->targetPath,
                QFile::ReadOwner | QFile::WriteOwner |
                QFile::ReadGroup | QFile::ReadOther);
            Logger::info(QObject::tr("Broker: Varlık dosyası merkezi depoya başarıyla kaydedildi → %1 (%2 KB)")
                         .arg(t->targetPath).arg(t->receivedSize / 1024));
        } else {
            Logger::error(QObject::tr("Broker: Varlık dosyası atomik taşınamadı → %1").arg(t->targetPath));
        }
    }

    delete t;
    return true;
}

void BrokerTransferManager::syncAssetsToClient(Connection* agentConn, int agentType) {
    if (!agentConn || !agentConn->isConnected()) return;
    if (agentType == 2) return; // İdari PC'ye zil varlığı gönderilmez

    struct AssetPushItem {
        QString fullPath;
        QString targetName;
        QString mimeType;
    };

    QList<AssetPushItem> items = {
        {OkidiaConfig::bellStudentAssetPath(), "bell_student.mp3", "audio/mpeg"},
        {OkidiaConfig::bellTeacherAssetPath(), "bell_teacher.mp3", "audio/mpeg"},
        {OkidiaConfig::bellRecessAssetPath(),  "bell_recess.mp3",  "audio/mpeg"},
        {OkidiaConfig::schoolLogoPath(),        "school_logo.png", "image/png"}
    };

    for (const auto& item : items) {
        if (!QFile::exists(item.fullPath) || QFileInfo(item.fullPath).size() == 0) {
            continue;
        }

        QFile file(item.fullPath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }

        uint64_t fileSize = static_cast<uint64_t>(file.size());
        uint64_t seqId = QRandomGenerator::global()->generate64();

        // 1. FILE_TRANSFER_BEGIN
        okidia::FileTransferBegin beginMsg;
        beginMsg.set_filename(item.targetName.toStdString());
        beginMsg.set_file_size(fileSize);
        beginMsg.set_mime_type(item.mimeType.toStdString());
        beginMsg.set_read_only(true);
        beginMsg.set_destination("assets");

        okidia::Envelope beginEnv;
        beginEnv.set_type(okidia::MessageType::FILE_TRANSFER_BEGIN);
        beginEnv.set_sequence_id(seqId);
        beginEnv.set_payload(beginMsg.SerializeAsString());
        agentConn->sendEnvelope(beginEnv);

        // 2. FILE_TRANSFER_CHUNK (64 KB)
        uint32_t chunkIdx = 0;
        while (!file.atEnd()) {
            QByteArray chunkData = file.read(64 * 1024);
            bool isLast = file.atEnd();

            okidia::FileTransferChunk chunkMsg;
            chunkMsg.set_chunk_index(chunkIdx++);
            chunkMsg.set_data(chunkData.constData(), chunkData.size());
            chunkMsg.set_is_last(isLast);

            okidia::Envelope chunkEnv;
            chunkEnv.set_type(okidia::MessageType::FILE_TRANSFER_CHUNK);
            chunkEnv.set_sequence_id(seqId);
            chunkEnv.set_payload(chunkMsg.SerializeAsString());
            agentConn->sendEnvelope(chunkEnv);
        }
        file.close();

        // 3. FILE_TRANSFER_END
        okidia::Envelope endEnv;
        endEnv.set_type(okidia::MessageType::FILE_TRANSFER_END);
        endEnv.set_sequence_id(seqId);
        agentConn->sendEnvelope(endEnv);

        Logger::info(QObject::tr("Broker: Otonom varlık fısıldandı → %1 (%2 KB, seqId=%3)")
                     .arg(item.targetName).arg(fileSize / 1024).arg(seqId));
    }
}

