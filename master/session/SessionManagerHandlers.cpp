#include "SessionManager.h"
#include "../audit/AuditLog.h"
#include "../chat/RoomManager.h"
#include "../transfer/MasterUpdater.h"
#include "../common/utils/Logger.h"
#include "../../common/crypto/AesGcm.h"
#include "../../common/crypto/BackupPackage.h"
#include <QPixmap>
#include <QDir>
#include <QDateTime>
#include <QSettings>
#include <QFile>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QTimer>
#include "../../common/OkidiaConfig.h"
#include "../../common/utils/OkidiaUtils.h"

void SessionManager::onMessageReceived(
    const okidia::Envelope& envelope) {

    // Master için dosya aktarımı (Master OTA İndirme)
    if (envelope.type() == okidia::MessageType::FILE_TRANSFER_BEGIN ||
        envelope.type() == okidia::MessageType::FILE_TRANSFER_CHUNK ||
        envelope.type() == okidia::MessageType::FILE_TRANSFER_END) {
        if (m_masterUpdater && m_masterUpdater->handleEnvelope(envelope)) {
            return;
        }
    }

    // Broker'dan Master güncelleme bildirimi
    if (envelope.type() == okidia::MessageType::MASTER_UPDATE_AVAILABLE) {
        okidia::MasterUpdateAvailable msg;
        if (msg.ParseFromString(envelope.payload())) {
            const QString pkgVer = QString::fromStdString(msg.version());
            const QString pkgFile = QString::fromStdString(msg.filename());
            const QString pkgOsTag = OkidiaUtils::normalizeOsTag(QString::fromStdString(msg.os_tag()));
            const QString pkgArch = OkidiaUtils::normalizeArch(QString::fromStdString(msg.os_arch()));

            const QString myOsTag = OkidiaUtils::detectLocalOsTag();
            const QString myArch = OkidiaUtils::detectLocalOsArch();
            const QString myVer = Okidia::Version::string();

            // Yalnızca bu Master bilgisayarın işletim sistemi ve mimarisiyle birebir uyumlu
            // olan paketleri kabul et; uyumsuz paketlerin mevcut bildirimi ezmesini engelle.
            if (!OkidiaUtils::isOsPackageCompatible(pkgOsTag, myOsTag) ||
                (!pkgArch.isEmpty() && !myArch.isEmpty() && pkgArch != myArch)) {
                Logger::info(QObject::tr("SessionManager: Bu bilgisayarla (%1/%2) uyumsuz Master güncelleme paketi (%3/%4 - %5) yoksayıldı.")
                             .arg(myOsTag, myArch, pkgOsTag, pkgArch, pkgFile));
                return;
            }

            if (OkidiaUtils::compareVersions(pkgVer, myVer) <= 0) {
                return;
            }

            if (m_availableMasterUpdate.available &&
                OkidiaUtils::compareVersions(pkgVer, m_availableMasterUpdate.version) < 0) {
                return;
            }

            m_availableMasterUpdate.version = pkgVer;
            m_availableMasterUpdate.filename = pkgFile;
            m_availableMasterUpdate.size = msg.file_size();
            m_availableMasterUpdate.osTag = pkgOsTag;
            m_availableMasterUpdate.osArch = pkgArch;
            m_availableMasterUpdate.signature = QByteArray(msg.signature().data(), msg.signature().size());
            m_availableMasterUpdate.fileHash = QByteArray(msg.file_hash().data(), msg.file_hash().size());
            m_availableMasterUpdate.masterCert = QByteArray(msg.master_cert().data(), msg.master_cert().size());
            m_availableMasterUpdate.available = true;

            Logger::info(QObject::tr("SessionManager: Yeni Master güncellemesi mevcut -> v") + m_availableMasterUpdate.version + " (" + m_availableMasterUpdate.filename + ")");
            emit masterUpdateAvailableReceived(m_availableMasterUpdate.version,
                                               m_availableMasterUpdate.filename,
                                               m_availableMasterUpdate.size,
                                               m_availableMasterUpdate.osTag,
                                               m_availableMasterUpdate.osArch);
        }
        return;
    }

    // Master senkronizasyon sinyali (Debounce ile birleştirilir)
    if (envelope.type() == okidia::MessageType::REFRESH_REQUEST &&
        envelope.target_id() == "__sync_masters__") {
        if (!m_dbSyncDebounceTimer) {
            m_dbSyncDebounceTimer = new QTimer(this);
            m_dbSyncDebounceTimer->setSingleShot(true);
            m_dbSyncDebounceTimer->setInterval(2500); // 2.5 saniye debounce
            connect(m_dbSyncDebounceTimer, &QTimer::timeout, this, [this]() {
                Logger::info(QObject::tr("SessionManager: DB eşitleme sinyalleri birleştirildi, güncel DB indiriliyor..."));
                requestDbBackup();
            });
        }
        m_dbSyncDebounceTimer->start();
        return;
    }

    // Broker Saat / BIOS Pili Uyarısı (BROKER_TIME_WARNING)
    if (envelope.type() == okidia::MessageType::BROKER_TIME_WARNING) {
        okidia::BrokerTimeWarning warn;
        if (warn.ParseFromString(envelope.payload())) {
            Logger::warning(QObject::tr("SessionManager: BROKER_TIME_WARNING alındı (Kalan tolerans: %1, Kilitli: %2)")
                            .arg(warn.remaining_logins()).arg(warn.is_locked() ? "Evet" : "Hayır"));
            emit brokerTimeWarningReceived(warn.is_desync(),
                                           warn.remaining_logins(),
                                           QString::fromStdString(warn.warning_message()),
                                           warn.is_locked());
        }
        return;
    }

    // Broker Depolama / Disk Alanı Uyarısı (BROKER_STORAGE_WARNING)
    if (envelope.type() == okidia::MessageType::BROKER_STORAGE_WARNING) {
        okidia::BrokerStorageWarning warn;
        if (warn.ParseFromString(envelope.payload())) {
            Logger::error(QObject::tr("SessionManager: BROKER_STORAGE_WARNING alındı -> ")
                          + QString::fromStdString(warn.warning_message()));
            emit brokerStorageWarningReceived(warn.available_bytes(),
                                           warn.required_bytes(),
                                           QString::fromStdString(warn.warning_message()));
        }
        return;
    }

    // İstemci Donanım / BIOS Pili Arızası (CLIENT_HARDWARE_ALERT)
    if (envelope.type() == okidia::MessageType::CLIENT_HARDWARE_ALERT) {
        okidia::ClientHardwareAlert alert;
        if (alert.ParseFromString(envelope.payload())) {
            QString machineId = QString::fromStdString(alert.machine_id());
            QString hostname = QString::fromStdString(alert.hostname());
            QString alertType = QString::fromStdString(alert.alert_type());
            int count = alert.occurrence_count();
            QString details = QString::fromStdString(alert.details());

            Logger::warning(QObject::tr("SessionManager: CLIENT_HARDWARE_ALERT alındı: %1 (%2) -> %3 (Adet: %4)")
                            .arg(hostname.isEmpty() ? machineId : hostname)
                            .arg(machineId).arg(alertType).arg(count));

            emit clientHardwareAlertReceived(machineId, hostname, alertType, count, details);
        }
        return;
    }

    // Akıllı Tahta Dev/Test PIN Telemetrisi
    if (envelope.type() == okidia::MessageType::SMARTBOARD_LOCK_PIN_REPORT) {
        okidia::SmartboardLockPinReport rep;
        if (rep.ParseFromString(envelope.payload())) {
            QString cId = QString::fromStdString(envelope.target_id());
            if (cId.isEmpty()) cId = QString::fromStdString(rep.client_id());
            QString pin = QString::fromStdString(rep.current_pin());
            int remSec = static_cast<int>(rep.remaining_seconds());

            Logger::info(QObject::tr("SessionManager: Akıllı Tahta Dev PIN alındı (%1) -> %2 (Kalan: %3s)")
                         .arg(cId).arg(pin).arg(remSec));

            emit smartboardLockPinReceived(cId, pin, remSec);
        }
        return;
    }

    // Akıllı Tahta PIN ile Açılma Bildirimi
    if (envelope.type() == okidia::MessageType::SMARTBOARD_UNLOCK_NOTIFY) {
        okidia::SmartboardUnlockNotify notif;
        if (notif.ParseFromString(envelope.payload())) {
            QString cId = QString::fromStdString(envelope.target_id());
            QString username = QString::fromStdString(notif.username());
            QString displayName = QString::fromStdString(notif.display_name());

            Logger::info(QObject::tr("SessionManager: Akıllı Tahta kilidi PIN ile açıldı: %1 (%2)").arg(cId, username));

            if (m_clientStates.contains(cId)) {
                if (!username.isEmpty()) m_clientStates[cId].username = username;
                if (!displayName.isEmpty()) m_clientStates[cId].userDisplayName = displayName;
                if (m_thumbnails.contains(cId)) {
                    m_thumbnails[cId]->updateState(m_clientStates[cId]);
                }
            }

            // Durum bayraklarını güncelle
            setClientScreenLocked(cId, false);
            setClientInputLocked(cId, false);

            emit smartboardUnlockedReceived(cId, username, displayName);
        }
        return;
    }

    // Uzaktan log yanıtı (LOG_TAIL_RES)
    if (envelope.type() == okidia::MessageType::LOG_TAIL_RES) {
        okidia::LogTailResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            QString sourceId = QString::fromStdString(envelope.target_id());
            emit logTailReceived(sourceId, resp);
        }
        return;
    }

    // Kullanıcı rolü / yetki değişikliği bildirimi (USER_ROLE_CHANGED_NOTIFY)
    if (envelope.type() == okidia::MessageType::USER_ROLE_CHANGED_NOTIFY) {
        okidia::UserRoleChangedNotify notif;
        if (notif.ParseFromString(envelope.payload())) {
            QString newRole = QString::fromStdString(notif.new_role());
            QString displayName = QString::fromStdString(notif.display_name());
            QString msg = QString::fromStdString(notif.message());

            Logger::info(QObject::tr("SessionManager: Kullanıcı rolü değiştirildi bildirimi alındı -> Yeni Rol: %1").arg(newRole));

            // Aktif kullanıcımızın rolünü güncelle
            m_activeUser.role = newRole;
            refreshPermissions();

            emit userRoleChangedReceived(newRole, displayName, msg);
        }
        return;
    }

    // CLIENT_INFO gelince clientId'yi öğreniriz
    if (envelope.type() == okidia::MessageType::CLIENT_INFO) {
        handleClientInfo(envelope);
        return;
    }

    if (envelope.type() == okidia::MessageType::DISCOVERY_REQUEST) {
        okidia::DiscoveryRequest req;
        if (!req.ParseFromString(envelope.payload())) return;
        const QString machineId = QString::fromStdString(req.machine_id());
        
        // Prevent empty IDs from crashing map systems down the line
        if (machineId.isEmpty()) return;
        
        if (m_ignoredMachineIds.contains(machineId)) return;
        const QString targetId = envelope.target_id().empty()
            ? machineId
            : QString::fromStdString(envelope.target_id());
        m_pendingDiscoveryData[targetId] = req;  // approve'da DB'ye yazacağız
        emit discoveryRequest(targetId, req);
        return;
    }

    if (envelope.type() == okidia::MessageType::DISCOVERY_RESOLVED_BROADCAST) {
        okidia::DiscoveryResolvedBroadcast res;
        if (res.ParseFromString(envelope.payload())) {
            const QString targetId   = QString::fromStdString(res.target_id());
            const QString resolvedBy = QString::fromStdString(res.resolved_by());
            const QString action     = QString::fromStdString(res.action());
            const uint32_t agentType = res.agent_type();
            const QString hostname   = QString::fromStdString(res.hostname());

            m_pendingDiscoveryData.remove(targetId);

            Logger::info(QObject::tr("SessionManager: DISCOVERY_RESOLVED_BROADCAST alındı → Hedef: %1, Yapan: %2, Eylem: %3, Tip: %4")
                         .arg(targetId, resolvedBy, action).arg(agentType));

            if (action == "approve") {
                if (!m_dbSyncDebounceTimer) {
                    m_dbSyncDebounceTimer = new QTimer(this);
                    m_dbSyncDebounceTimer->setSingleShot(true);
                    m_dbSyncDebounceTimer->setInterval(1000);
                    connect(m_dbSyncDebounceTimer, &QTimer::timeout, this, [this]() {
                        requestDbBackup();
                    });
                }
                m_dbSyncDebounceTimer->start(1000);
            }

            emit discoveryResolved(targetId, resolvedBy, action, agentType, hostname);
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::DB_BACKUP_RESPONSE) {
        okidia::DbBackupResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            if (m_isSyncingDb) {
                m_isSyncingDb = false;
                if (resp.success()) {
                    QByteArray encrypted = QByteArray::fromStdString(resp.encrypted_db());
                    QByteArray rawKey = QByteArray::fromStdString(resp.broker_key());
                    m_brokerKey = rawKey;
                    QByteArray keyData = QCryptographicHash::hash(rawKey, QCryptographicHash::Sha256);
                    QByteArray decrypted = AesGcm::decrypt(encrypted, keyData);

                    if (!decrypted.isEmpty()) {
                        QString dbPath;
                        if (!m_activeUser.id.isEmpty()) {
                            dbPath = OkidiaConfig::masterDbPath(m_activeUser.id);
                        } else if (!OkidiaDatabase::instance().path().isEmpty()) {
                            dbPath = OkidiaDatabase::instance().path();
                        } else {
                            dbPath = OkidiaConfig::masterDbPath();
                        }
                        
                        // Önce veritabanını kapat (overwrite yaparken bozulmaması için)
                        OkidiaDatabase::instance().close();
                        
                        // WAL modunda çalıştığı için eski önbellek dosyalarını temizle
                        QFile::remove(dbPath + "-wal");
                        QFile::remove(dbPath + "-shm");

                        // Atomik geçici dosya yazımı (klasör izinlerinden yararlanarak yazma)
                        QString tmpPath = dbPath + ".tmp";
                        QFile::remove(tmpPath);
                        bool written = false;

                        QFile fTmp(tmpPath);
                        if (fTmp.open(QIODevice::WriteOnly)) {
                            if (fTmp.write(decrypted) == decrypted.size()) {
                                fTmp.close();
                                // Hedef dosyayı kaldır ve geçici dosyayı yerine taşı
                                QFile::remove(dbPath);
                                if (QFile::rename(tmpPath, dbPath)) {
                                    written = true;
                                }
                            } else {
                                fTmp.close();
                                QFile::remove(tmpPath);
                            }
                        }

                        // Eğer rename başarısız olduysa doğrudan yazmayı dene
                        if (!written) {
                            QFile fDirect(dbPath);
                            if (fDirect.open(QIODevice::WriteOnly)) {
                                fDirect.write(decrypted);
                                fDirect.close();
                                written = true;
                            }
                        }

                        if (written) {
                            // Dosya izinlerini grup yazabilir yap
                            QFile::setPermissions(dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                          QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                                          QFileDevice::ReadOther);
                            Logger::info(QObject::tr("SessionManager: Senkronize edilen veritabanı dosyaya yazildi -> ") + dbPath);

                            // Dosya yazıldıktan sonra yeniden aç
                            if (OkidiaDatabase::instance().open(dbPath)) {
                                // DB'yi yükle (m_clientStates'i günceller)
                                loadFromDatabase();
                                refreshPermissions();
                                Logger::info(QObject::tr("SessionManager: Senkronize veritabanı başarıyla açıldı ve yüklendi"));

                                emit databaseReloaded(); // UI treeview + thumbnail panelini tek noktadan senkronize et
                                fetchSshCredentials();   // Broker'dan güncel kimlikleri tazele

                                QString dbSchool = OkidiaDatabase::instance().metaValue("school_name").trimmed();
                                if (!dbSchool.isEmpty() && dbSchool != m_schoolName) {
                                    m_schoolName = dbSchool;
                                    QSettings s("okidia", "master");
                                    s.setValue("school/name", dbSchool);
                                    Logger::info(QObject::tr("SessionManager: Senkronize DB'den okul adı güncellendi → ") + dbSchool);
                                    emit schoolNameChanged(m_schoolName);
                                }

                            } else {
                                Logger::error(QObject::tr("SessionManager: Senkronize veritabanı açılamadı!"));
                            }
                        } else {
                            Logger::error(QObject::tr("SessionManager: Senkronize veritabanı dosyası yazılamadı -> ") + dbPath);
                            // Veritabanı yazılamasa dahi mevcut veritabanını tekrar açarak sistemin açık kalmasını sağla
                            if (!OkidiaDatabase::instance().open(dbPath)) {
                                Logger::error(QObject::tr("SessionManager: Mevcut veritabanı da yeniden açılamadı -> ") + dbPath);
                            }
                        }
                    } else {
                        Logger::error(QObject::tr("SessionManager: Veritabanı deşifre edilemedi!"));
                    }
                } else {
                    Logger::error(QObject::tr("SessionManager: Senkronizasyon için DB yedek alınamadı -> ") + QString::fromStdString(resp.error_msg()));
                }
                return;
            }

            if (m_isManualBackup) {
                m_isManualBackup = false;
                if (resp.success()) {
                    QByteArray encrypted = QByteArray::fromStdString(resp.encrypted_db());
                    QByteArray rawKey = QByteArray::fromStdString(resp.broker_key());
                    QByteArray keyData = QCryptographicHash::hash(rawKey, QCryptographicHash::Sha256);
                    QByteArray decryptedDb = AesGcm::decrypt(encrypted, keyData);

                    if (decryptedDb.isEmpty()) {
                        Logger::error(QObject::tr("SessionManager: Sunucudan gelen veritabanı şifresi çözülemedi!"));
                        emit systemBackupFinished(false, QString(), tr("Veritabanı çözülemedi."));
                        return;
                    }

                    BackupContent content;
                    content.meta.version = qApp->applicationVersion();
                    content.meta.timestamp = OkidiaUtils::nowSecs();
                    content.meta.creator = m_activeUser.username.isEmpty() ? m_activeUser.id : m_activeUser.username;
                    content.meta.description = tr("Master Konsolu Sistem Yedeği");
                    content.databaseData = decryptedDb;

                    if (resp.has_audit()) {
                        QByteArray encAudit = QByteArray::fromStdString(resp.encrypted_audit_db());
                        QByteArray decAudit = AesGcm::decrypt(encAudit, keyData);
                        if (!decAudit.isEmpty()) {
                            content.auditData = decAudit;
                        }
                    }

                    if (resp.has_chat()) {
                        QByteArray encChat = QByteArray::fromStdString(resp.encrypted_chat_db());
                        QByteArray decChat = AesGcm::decrypt(encChat, keyData);
                        if (!decChat.isEmpty()) {
                            content.chatData = decChat;
                        }
                    }

                    for (int i = 0; i < resp.identity_bundle_size(); ++i) {
                        const auto& idFile = resp.identity_bundle(i);
                        content.certFiles[QString::fromStdString(idFile.name())] = QByteArray::fromStdString(idFile.content());
                    }

                    QString targetDir = m_manualBackupDir;
                    if (targetDir.isEmpty()) {
                        targetDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/OKIDIA-Yedekler";
                    }
                    QDir().mkpath(targetDir);

                    QString filename = QString("okidia_%1.okbak").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
                    QString fullPath = targetDir + "/" + filename;

                    QString err;
                    if (BackupPackage::packToFile(fullPath, m_manualBackupPassword, content, &err)) {
                        Logger::info(QObject::tr("SessionManager: Sistem yedeği başarıyla kaydedildi → ") + fullPath);

                        // Kullanıcının tercih ettiği saklama adedine göre eski yedekleri temizle
                        QSettings settings("okidia", "master");
                        int retention = settings.value("backup/retentionCount", 7).toInt();
                        if (retention > 0) {
                            QDir bDir(targetDir);
                            QStringList filters;
                            filters << "*.okbak";
                            QFileInfoList files = bDir.entryInfoList(filters, QDir::Files, QDir::Time);
                            if (files.size() > retention) {
                                for (int i = retention; i < files.size(); ++i) {
                                    QFile::remove(files[i].filePath());
                                    Logger::info(QObject::tr("SessionManager: Eski sistem yedeği silindi → ") + files[i].fileName());
                                }
                            }
                        }

                        emit systemBackupFinished(true, fullPath, QString());
                    } else {
                        Logger::error(QObject::tr("SessionManager: Yedek paketi yazılamadı → ") + err);
                        emit systemBackupFinished(false, QString(), err);
                    }
                } else {
                    QString err = QString::fromStdString(resp.error_msg());
                    Logger::error(QObject::tr("SessionManager: Broker yedekleme hatası → ") + err);
                    emit systemBackupFinished(false, QString(), err);
                }
                return;
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::BACKUP_REPLICA_CHECK) {
        okidia::BackupReplicaCheck check;
        if (check.ParseFromString(envelope.payload())) {
            QString filename = QString::fromStdString(check.filename());
            quint64 totalSize = check.total_size();
            if (filename.isEmpty()) return;

            const QString localHost = QSysInfo::machineHostName().trimmed();
            const UserRecord dbUser = OkidiaDatabase::instance().userById(m_activeUser.id);
            const QString boundHost = !dbUser.replicaHostname.trimmed().isEmpty()
                                      ? dbUser.replicaHostname.trimmed()
                                      : m_activeUser.replicaHostname.trimmed();
            if (!boundHost.isEmpty() && !localHost.isEmpty() &&
                localHost.compare(boundHost, Qt::CaseInsensitive) != 0) {
                Logger::info(QObject::tr("SessionManager: Bu bilgisayar (%1) kullanıcının tanımlı yedek cihazı (%2) olmadığı için .okbak replika indirmesi atlandı.")
                             .arg(localHost, boundHost));
                return;
            }

            QString targetDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
            QDir().mkpath(targetDir);
            QString fullPath = targetDir + "/" + filename;

            QFileInfo fi(fullPath);
            if (fi.exists() && static_cast<quint64>(fi.size()) == totalSize && totalSize > 0) {
                Logger::info(QObject::tr("SessionManager: Replika yedeği zaten güncel (%1, %2 KB), indirme atlandı.")
                             .arg(filename).arg(totalSize / 1024));
                return;
            }

            Logger::info(QObject::tr("SessionManager: Yeni replika yedeği anons edildi, parça parça çekiliyor → %1 (%2 KB)")
                         .arg(filename).arg(totalSize / 1024));

            okidia::BackupReplicaPull pull;
            pull.set_filename(check.filename());
            pull.set_offset(0);

            okidia::Envelope pullEnv;
            pullEnv.set_type(okidia::MessageType::BACKUP_REPLICA_PULL);
            pullEnv.set_payload(pull.SerializeAsString());
            if (m_conn) {
                m_conn->sendEnvelope(pullEnv);
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::BACKUP_REPLICA_PUSH) {
        okidia::BackupReplicaPush push;
        if (push.ParseFromString(envelope.payload())) {
            QString filename = QString::fromStdString(push.filename());
            if (filename.isEmpty()) {
                filename = QString("okidia_replica_%1.okbak").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
            }
            QString targetDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
            QDir().mkpath(targetDir);
            QString fullPath = targetDir + "/" + filename;
            QString partPath = fullPath + ".part";

            bool isChunked = (push.total_size() > 0);
            if (!isChunked) {
                // Eski monolitik yedek aktarım uyumluluğu
                QFile f(fullPath);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(QByteArray::fromStdString(push.package_data()));
                    f.close();
                    Logger::info(QObject::tr("SessionManager: İkincil yedek (replika) başarıyla kaydedildi → ") + fullPath);
                    emit backupReplicaReceived(fullPath);
                }
                return;
            }

            // Chunked parça yazımı
            QFile partFile(partPath);
            QIODevice::OpenMode mode = (push.offset() == 0)
                                       ? (QIODevice::WriteOnly | QIODevice::Truncate)
                                       : (QIODevice::WriteOnly | QIODevice::Append);

            if (partFile.open(mode)) {
                if (push.offset() > 0 && partFile.pos() != static_cast<qint64>(push.offset())) {
                    partFile.seek(static_cast<qint64>(push.offset()));
                }
                partFile.write(QByteArray::fromStdString(push.package_data()));
                partFile.close();

                if (push.is_last_chunk()) {
                    QFile::remove(fullPath);
                    if (QFile::rename(partPath, fullPath)) {
                        Logger::info(QObject::tr("SessionManager: İkincil yedek (replika) tüm parçalar alınarak birleştirildi → ") + fullPath);

                        QSettings settings("okidia", "master");
                        int retention = settings.value("backup/replicaRetentionCount", 7).toInt();
                        if (retention > 0) {
                            QDir bDir(targetDir);
                            QStringList filters;
                            filters << "*.okbak";
                            QFileInfoList files = bDir.entryInfoList(filters, QDir::Files, QDir::Time);
                            if (files.size() > retention) {
                                for (int i = retention; i < files.size(); ++i) {
                                    QFile::remove(files[i].filePath());
                                    Logger::info(QObject::tr("SessionManager: Eski replika yedeği silindi → ") + files[i].fileName());
                                }
                            }
                        }

                        emit backupReplicaReceived(fullPath);
                    } else {
                        Logger::error(QObject::tr("SessionManager: Geçici replika dosyası asıl dosyaya dönüştürülemedi → ") + partPath);
                    }
                }
            } else {
                Logger::error(QObject::tr("SessionManager: İkincil yedek parçası yazılamadı → ") + partPath);
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::DB_USER_SET_REPLICA_RES) {
        okidia::DbUserSetReplicaResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            if (!resp.success()) {
                Logger::warning(QObject::tr("SessionManager: Kullanıcı replika yetkilendirme hatası → ") + QString::fromStdString(resp.error_msg()));
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::DB_RESTORE_RESPONSE) {
        okidia::DbRestoreResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit dbRestoreResult(resp.success(), QString::fromStdString(resp.error_msg()));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::LOGIN_RESPONSE) {
        okidia::LoginResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit loginResult(resp.success(), 
                             QString::fromStdString(resp.user_id()),
                             QString::fromStdString(resp.role()),
                             QString::fromStdString(resp.display_name()),
                             QString::fromStdString(resp.error_msg()),
                             resp.must_change_pw());

            if (m_reauthWatchdogTimer && m_reauthWatchdogTimer->isActive()) {
                m_reauthWatchdogTimer->stop();
            }
            m_reauthRetryAttempted = false;

            if (resp.success()) {
                m_isAuthenticated = true;
                m_activeUser.id = QString::fromStdString(resp.user_id());
                m_activeUser.username = !m_username.isEmpty() ? m_username : m_activeUser.username;
                m_activeUser.role = QString::fromStdString(resp.role());
                m_activeUser.displayName = QString::fromStdString(resp.display_name());
                
                // Giriş başarılı olduysa, kullandığımız güncel şifreyi aktif kullanıcıya kopyala
                if (!m_passwordHash.isEmpty()) {
                    m_activeUser.passwordHash = m_passwordHash;
                }
                m_brokerVersion = QString::fromStdString(resp.broker_version());
                m_brokerOs = QString::fromStdString(resp.broker_os());
                m_brokerOsTag = QString::fromStdString(resp.broker_os_tag());
                m_brokerArch = QString::fromStdString(resp.broker_arch());
                m_brokerAllIps.clear();
                for (int i = 0; i < resp.broker_ips_size(); ++i) {
                    m_brokerAllIps.append(QString::fromStdString(resp.broker_ips(i)));
                }

                QString masterVer = Okidia::Version::string();
                if (!m_brokerVersion.isEmpty()) {
                    if (OkidiaUtils::compareVersions(masterVer, m_brokerVersion) > 0) {
                        m_isBrokerOutdated = true;
                        if (m_activeUser.role == "YM") {
                            Logger::warning(QObject::tr("SessionManager: Broker sürümü (v") + m_brokerVersion + QObject::tr(") Master sürümünden (v") + masterVer + QObject::tr(") daha eski! YM için uyarı tetikleniyor."));
                            emit brokerOutdatedWarning(m_brokerVersion, masterVer);
                        }
                    } else {
                        m_isBrokerOutdated = false;
                    }
                }

                if (m_isOfflineMode) {
                    m_isOfflineMode = false;
                    Logger::info(QObject::tr("SessionManager: Çevrimdışı moddan çıkıldı, başarılı giriş."));
                    emit switchedToOnlineMode();
                    emit connectionModeChanged(connectionMode());
                }

                QString brokerSchool = QString::fromStdString(resp.school_name()).trimmed();
                if (!brokerSchool.isEmpty()) {
                    m_schoolName = brokerSchool;
                    QSettings s("okidia", "master");
                    s.setValue("school/name", brokerSchool);
                    Logger::info(QObject::tr("SessionManager: Broker okul adı alındı ve eşitlendi → ") + brokerSchool);
                    emit schoolNameChanged(m_schoolName);
                }

                Logger::info(QObject::tr("SessionManager: Ag login basarili, otomatik DB senkronizasyonu baslatiliyor..."));
                requestDbBackup();
                syncPendingSshCredentials();
                fetchSshCredentials();
                sendRefreshBroadcast();

                // Broker'da oturumumuz artık doğrulandı; görünür istemci aboneliklerini ve watchdog'u güvenle başlat
                m_lastSentIntervalMs = -1;
                if (!m_visibleClients.isEmpty()) {
                    setVisibleClients(m_visibleClients);
                }
                if (m_blankThumbWatchdog && !m_blankThumbWatchdog->isActive()) {
                    m_blankThumbWatchdog->start();
                }
            } else {
                m_isAuthenticated = false;
                if (m_blankThumbWatchdog && m_blankThumbWatchdog->isActive()) {
                    m_blankThumbWatchdog->stop();
                }
                if (m_staggerRefreshTimer && m_staggerRefreshTimer->isActive()) {
                    m_staggerRefreshTimer->stop();
                }
                m_staggerRefreshQueue.clear();
                if (m_isOfflineMode) {
                    Logger::warning(QObject::tr("SessionManager: Çevrimdışı modda iken yapılan arka plan girişi reddedildi!"));
                    emit offlineLoginFailed(QString::fromStdString(resp.error_msg()));
                }
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::BROKER_PORT_SET_RES) {
        okidia::BrokerPortSetResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            Logger::info(QObject::tr("SessionManager: Broker port yanıtı alındı (success=%1, port=%2)")
                         .arg(resp.success()).arg(resp.new_port()));
            emit brokerPortChanged(resp.success(), static_cast<quint16>(resp.new_port()), QString::fromStdString(resp.error_msg()));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::AGENT_UNINSTALL_PASS_RES) {
        okidia::AgentUninstallPassResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            Logger::info(QObject::tr("SessionManager: İstemci kaldırma parolası yanıtı alındı (success=%1)").arg(resp.success()));
            emit agentUninstallPassResult(resp.success(), QString::fromStdString(resp.error_msg()));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::AGENT_UNINSTALL_PASS_SET) {
        okidia::AgentUninstallPassSet msg;
        QString hash;
        if (msg.ParseFromString(envelope.payload())) {
            hash = QString::fromStdString(msg.password_hash()).trimmed().toLower();
        } else {
            hash = QString::fromUtf8(envelope.payload().c_str()).trimmed().toLower();
        }
        if (!hash.isEmpty() && OkidiaDatabase::instance().isOpen()) {
            OkidiaDatabase::instance().setUninstallPasswordHash(hash);
            Logger::info(QObject::tr("SessionManager: Broker'dan gelen istemci kaldırma parolası senkronize edildi"));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::DB_USER_PW_RESET_RES) {
        okidia::DbUserPwResetResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit dbUserPwResetResult(resp.success(), QString::fromStdString(resp.error_msg()));
            if (resp.success()) {
                requestDbBackup();
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::DB_USER_GRANT_RES) {
        okidia::DbUserGrantResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit dbUserGrantResult(resp.success(), QString::fromStdString(resp.error_msg()));
            if (resp.success()) {
                requestDbBackup();
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::DB_USER_REVOKE_RES) {
        okidia::DbUserRevokeResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit dbUserRevokeResult(resp.success(), QString::fromStdString(resp.error_msg()));
            if (resp.success()) {
                requestDbBackup();
            }
        }
        return;
    }

    // ── CUSTOM_PROFILE_UPSERT_RES ─────────────────────────────────────
    if (envelope.type() == okidia::MessageType::CUSTOM_PROFILE_UPSERT_RES) {
        okidia::CustomProfileUpsertRes resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit customProfileUpsertResult(resp.success(), QString::fromStdString(resp.error_msg()), resp.id());
            if (resp.success()) {
                requestDbBackup();
            }
        }
        return;
    }

    // ── CUSTOM_PROFILE_DELETE_RES ─────────────────────────────────────
    if (envelope.type() == okidia::MessageType::CUSTOM_PROFILE_DELETE_RES) {
        okidia::CustomProfileDeleteRes resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit customProfileDeleteResult(resp.success(), QString::fromStdString(resp.error_msg()));
            if (resp.success()) {
                requestDbBackup();
            }
        }
        return;
    }

    // ── DB_MASTER_DEVICE_DEL_RES ──────────────────────────────────────
    if (envelope.type() == okidia::MessageType::DB_MASTER_DEVICE_DEL_RES) {
        okidia::DbMasterDeviceDeleteRes resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit dbMasterDeviceDeleteResult(resp.success(), QString::fromStdString(resp.error_msg()));
            if (resp.success()) {
                requestDbBackup();
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::SESSION_RESTRICT_MODE) {
        if (m_activeUser.id.isEmpty()) {
            Logger::warning(QObject::tr("SessionManager: Oturum açılmadan kısıtlama mesajı alındı, yok sayılıyor."));
            return;
        }
        Logger::warning(QObject::tr("SessionManager: Baska bir cihazdan oturum acildi, kisitli moda geciliyor."));
        m_isKickedToOffline = true;
        m_isOfflineMode = true;
        if (m_tls) {
            m_tls->setAutoReconnect(false);
            m_tls->disconnectFromServer();
        }
        emit kickedToOfflineMode();
        emit connectionModeChanged(connectionMode());
        return;
    }


    if (envelope.type() == okidia::MessageType::ENROLL_RESPONSE) {
        okidia::EnrollResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit enrollResult(resp.success(),
                              QString::fromStdString(resp.master_cert()),
                              QString::fromStdString(resp.ca_cert()),
                              QString::fromStdString(resp.error_msg()));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::PASSWORD_RESET_RES) {
        okidia::PasswordResetResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit passwordResetResult(resp.success(), 
                                     QString::fromStdString(resp.error_msg()));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::JOIN_CODE_RESPONSE) {
        okidia::JoinCodeResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit joinCodeReceived(QString::fromStdString(resp.code()), 
                                  resp.success(), 
                                  QString::fromStdString(resp.error_msg()));
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::CLIENT_REVOKED_BROADCAST) {
        okidia::ClientRevokedBroadcast msg;
        if (msg.ParseFromString(envelope.payload())) {
            QString mid = QString::fromStdString(msg.machine_id());
            // Eğer bu machineId bizde m_clientStates içinde varsa, temizle
            QString clientIdToRemove;
            for (auto it = m_clientStates.begin(); it != m_clientStates.end(); ++it) {
                if (it.value().machineId == mid || it.key() == mid) {
                    clientIdToRemove = it.key();
                    break;
                }
            }
            if (!clientIdToRemove.isEmpty()) {
                m_clientStates.remove(clientIdToRemove);
                m_hashToClientId.remove(qHash(clientIdToRemove));
                if (m_thumbnails.contains(clientIdToRemove))
                    unregisterThumbnail(clientIdToRemove);
                emit clientDisconnected(clientIdToRemove);
                // Tree ve Chat güncellemeleri için ayrı bir sinyal tetiklenebilir 
                // ya da clientDisconnected zaten thumbnail ve treeview'dan düşürür.
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::SSH_CRED_LIST_RES) {
        handleSshCredList(envelope);
        return;
    }

    if (envelope.type() == okidia::MessageType::AUDIT_LOG_QUERY_RES) {
        okidia::AuditLogQueryResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            QList<AuditEvent> results;
            for (int i = 0; i < resp.records_size(); ++i) {
                const auto& r = resp.records(i);
                AuditEvent e;
                e.timestamp = QDateTime::fromSecsSinceEpoch(r.timestamp());
                e.category  = AuditLog::stringToCategory(QString::fromStdString(r.category()));
                e.actor     = QString::fromStdString(r.actor());
                e.target    = QString::fromStdString(r.target());
                e.detail    = QString::fromStdString(r.detail());
                e.userId    = QString::fromStdString(r.user_id());
                results << e;
            }
            int totalCount = resp.total_count() > 0 ? resp.total_count() : results.size();
            emit auditLogQueryReceived(results, totalCount);
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::CHAT_HISTORY_RES) {
        okidia::ChatHistoryResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            QString roomId = QString::fromStdString(resp.room_id());
            QList<RoomMessage> loaded;
            for (int i = 0; i < resp.records_size(); ++i) {
                const auto& rec = resp.records(i);
                RoomMessage msg;
                msg.id          = QString::fromStdString(rec.id());
                msg.roomId      = QString::fromStdString(rec.room_id());
                msg.fromHost    = QString::fromStdString(rec.from_host());
                msg.fromDisplay = QString::fromStdString(rec.from_host());
                msg.senderRole  = rec.sender_role();
                msg.content     = QString::fromStdString(rec.content());
                msg.timestamp   = static_cast<qint64>(rec.timestamp());
                msg.isSystem    = rec.is_system();
                loaded << msg;
            }
            emit chatHistoryReceived(roomId, loaded);
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::AGENT_LOCK_STATUS_BROADCAST) {
        okidia::AgentLockStatusBroadcast broadcast;
        if (broadcast.ParseFromString(envelope.payload())) {
            // clear lock states for all clients
            for (auto it = m_clientStates.begin(); it != m_clientStates.end(); ++it) {
                it->lockType = "";
                it->lockedBy = "";
                it->lockedByDisplayName = "";
                it->screenLocked = false;
                it->inputLocked = false;
                it->examActive = false;
            }

            for (int i = 0; i < broadcast.locks_size(); ++i) {
                const auto& lock = broadcast.locks(i);
                QString cId = QString::fromStdString(lock.client_id());
                if (m_clientStates.contains(cId)) {
                    m_clientStates[cId].lockType = QString::fromStdString(lock.lock_type());
                    m_clientStates[cId].lockedBy = QString::fromStdString(lock.master_user_id());
                    m_clientStates[cId].lockedByDisplayName = QString::fromStdString(lock.master_display_name());

                    // Set client state flags based on lock
                    if (m_clientStates[cId].lockType == "ScreenLock") {
                        m_clientStates[cId].screenLocked = true;
                    } else if (m_clientStates[cId].lockType == "InputLock") {
                        m_clientStates[cId].inputLocked = true;
                    } else if (m_clientStates[cId].lockType == "RemoteControl") {
                        m_clientStates[cId].inputLocked = true;
                    } else if (m_clientStates[cId].lockType == "ScreenView") {
                        // Sadece izleme, input kilidi yok
                    } else if (m_clientStates[cId].lockType == "Exam") {
                        m_clientStates[cId].examActive = true;
                    }
                }
            }

            // notify UI to update thumbnail icons or client states
            for (const QString& cId : m_clientStates.keys()) {
                if (m_thumbnails.contains(cId)) {
                    m_thumbnails[cId]->updateState(m_clientStates[cId]);
                }
                emit clientStateChanged(cId, m_clientStates[cId]);
            }
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::CHAT_MEMBER_JOINED) {
        okidia::ChatMemberEvent evt;
        if (evt.ParseFromString(envelope.payload())) {
            const QString cid = QString::fromStdString(evt.client_id());
            if (cid.startsWith("master:")) {
                m_knownOnlineMasters[cid] = true;
            }
            emit chatMemberJoinedReceived(
                QString::fromStdString(evt.room_id()),
                cid,
                QString::fromStdString(evt.hostname()),
                QString::fromStdString(evt.username())
            );
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::CHAT_MEMBER_LEFT) {
        okidia::ChatMemberEvent evt;
        if (evt.ParseFromString(envelope.payload())) {
            const QString cid = QString::fromStdString(evt.client_id());
            if (cid.startsWith("master:")) {
                m_knownOnlineMasters[cid] = false;
            }
            emit chatMemberLeftReceived(
                QString::fromStdString(evt.room_id()),
                cid
            );
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::CHAT_LOCK_STATE) {
        okidia::ChatLockState cls;
        if (cls.ParseFromString(envelope.payload())) {
            const QString classId = QString::fromStdString(cls.class_id());
            emit chatLockStateReceived(classId, cls.chat_locked(), cls.private_locked());
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::CHAT_MUTE_STATE) {
        okidia::ChatMuteState cms;
        if (cms.ParseFromString(envelope.payload())) {
            emit chatMuteStateReceived(
                QString::fromStdString(cms.client_id()),
                QString::fromStdString(cms.username()),
                cms.muted(),
                static_cast<qint64>(cms.muted_until()),
                static_cast<int>(cms.violation_level()),
                QString::fromStdString(cms.reason())
            );
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::ROOM_JOIN_REQUEST) {
        okidia::RoomJoinRequest req;
        if (req.ParseFromString(envelope.payload())) {
            emit roomJoinRequestReceived(
                QString::fromStdString(req.request_id()),
                QString::fromStdString(req.room_id()),
                QString::fromStdString(req.room_name()),
                QString::fromStdString(req.from_username()),
                QString::fromStdString(req.from_display()),
                QString::fromStdString(req.target_username())
            );
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::ROOM_JOIN_RESPONSE) {
        okidia::RoomJoinResponse res;
        if (res.ParseFromString(envelope.payload())) {
            emit roomJoinResponseReceived(
                QString::fromStdString(res.request_id()),
                QString::fromStdString(res.room_id()),
                QString::fromStdString(res.from_username()),
                QString::fromStdString(res.target_username()),
                res.accepted(),
                QString::fromStdString(res.reason())
            );
        }
        return;
    }

    // Diğer mesajlarda broker target_id'ye clientId, sequence_id'ye ise qHash(clientId) yazar.
    // qHash seed'i işlemden işleme değiştiği için target_id (string) ile eşleme yapmak en güvenlisidir.
    QString clientId = QString::fromStdString(envelope.target_id());
    if (clientId.isEmpty()) {
        uint hash = static_cast<uint>(envelope.sequence_id());
        clientId = m_hashToClientId.value(hash, "");
    }

    // CHAT_MESSAGE hem agent'lardan hem de doğrudan broker/diğer master'lardan gelebilir (clientId boş olabilir)
    if (envelope.type() == okidia::MessageType::CHAT_MESSAGE) {
        handleChatMessage(envelope, clientId);
        return;
    }

    if (envelope.type() == okidia::MessageType::BROKER_UPDATES_LIST_RES) {
        okidia::BrokerUpdatesListResponse res;
        if (res.ParseFromString(envelope.payload())) {
            Logger::info(QObject::tr("SessionManager: BROKER_UPDATES_LIST_RES alindi, paket sayisi: ") + QString::number(res.packages_size()));
            QVariantList packageList;
            for (int i = 0; i < res.packages_size(); ++i) {
                const auto& pkg = res.packages(i);
                QVariantMap map;
                map["filename"] = QString::fromStdString(pkg.filename());
                map["version"] = QString::fromStdString(pkg.version());
                map["size"] = static_cast<qulonglong>(pkg.size());
                map["timestamp"] = static_cast<qulonglong>(pkg.timestamp());
                packageList.append(map);
            }
            emit brokerUpdatesListReceived(packageList);
        }
        return;
    }

    if (envelope.type() == okidia::MessageType::BROKER_UPDATES_DELETE_RES) {
        okidia::BrokerUpdatesDeleteResponse res;
        if (res.ParseFromString(envelope.payload())) {
            QStringList deleted;
            for (int i = 0; i < res.deleted_filenames_size(); ++i) {
                deleted.append(QString::fromStdString(res.deleted_filenames(i)));
            }
            emit brokerUpdatesDeleteResult(res.success(), QString::fromStdString(res.error_msg()), deleted);
        }
        return;
    }

    if (clientId.isEmpty()) {
        // Henüz ClientInfo gelmemiş, bekle
        return;
    }

    switch (envelope.type()) {

    case okidia::MessageType::SCREEN_FRAME:
        handleScreenFrame(envelope, clientId);
        break;

    case okidia::MessageType::APP_LIST_RESPONSE: {
        okidia::AppListResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            QList<QPair<QString,QString>> apps;
            for (int i = 0; i < resp.apps_size(); ++i)
                apps.append({
                    QString::fromStdString(resp.apps(i).name()),
                    QString::fromStdString(resp.apps(i).wid() > 0
                        ? QString::number(resp.apps(i).wid()).toStdString()
                        : resp.apps(i).pid())
                });
            emit appListReceived(clientId, apps);
        }
        break;
    }

    case okidia::MessageType::QUERY_LOCAL_USERS_RES: {
        okidia::QueryLocalUsersRes res;
        if (res.ParseFromString(envelope.payload())) {
            QString resolvedId = clientId.isEmpty() ? QString::fromStdString(res.client_id()) : clientId;
            QList<QPair<QString,QString>> users;
            for (int i = 0; i < res.users_size(); ++i) {
                users.append({
                    QString::fromStdString(res.users(i).username()),
                    QString::fromStdString(res.users(i).display_name())
                });
            }
            Logger::info(QObject::tr("SessionManager: Yerel kullanıcı listesi alındı (%1) -> %2 adet")
                         .arg(resolvedId).arg(users.size()));
            emit localUsersReceived(resolvedId, res.success(), QString::fromStdString(res.error_msg()), users);
        }
        break;
    }

    case okidia::MessageType::REMOTE_FRAME: {
        // Uzak kontrol görüntüsü — thumbnail'den bağımsız
        okidia::ScreenFrame rf;
        if (rf.ParseFromString(envelope.payload())) {
            QByteArray jpegData = QByteArray::fromStdString(rf.jpeg_data());
            emit remoteFrameReceived(clientId, jpegData,
                static_cast<int>(rf.width()),
                static_cast<int>(rf.height()),
                rf.has_cursor() ? static_cast<int>(rf.cursor_x()) : -1,
                rf.has_cursor() ? static_cast<int>(rf.cursor_y()) : -1,
                rf.has_cursor());
        }
        break;
    }
    case okidia::MessageType::SCREEN_TILE_UPDATE:
        handleTileUpdate(envelope, clientId);
        break;
    case okidia::MessageType::SYSTEM_METRICS_REPORT: {
        okidia::SystemMetrics metrics;
        if (metrics.ParseFromString(envelope.payload())) {
            if (m_clientStates.contains(clientId)) {
                auto& state = m_clientStates[clientId];
                state.cpuUsage = metrics.cpu_usage();
                state.ramUsage = metrics.ram_usage();
                state.diskUsage = metrics.disk_usage();
                if (m_thumbnails.contains(clientId)) {
                    m_thumbnails[clientId]->updateState(state);
                }
                emit clientStateChanged(clientId, state);
            }
        }
        break;
    }
    case okidia::MessageType::HEARTBEAT:
        handleHeartbeat(envelope, clientId);
        break;
    case okidia::MessageType::SCREEN_CAPTURE_RESULT:
        handleScreenCaptureResult(envelope, clientId);
        break;
    case okidia::MessageType::HAND_RAISE:
        handleHandRaise(envelope, clientId);
        break;
    case okidia::MessageType::OTA_BROKER_REPORT: {
        okidia::OtaBrokerReport report;
        if (report.ParseFromString(envelope.payload())) {
            emit otaBrokerReportReceived(QString::fromStdString(report.message()), report.success());
        }
        break;
    }
    case okidia::MessageType::HAND_LOWER:
        handleHandLower(clientId);
        break;
    case okidia::MessageType::SCREEN_INFO_RESPONSE:
        handleScreenInfo(envelope, clientId);
        break;
    case okidia::MessageType::REMOTE_CONTROL_RESPONSE: {
        okidia::RemoteControlResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            emit remoteControlResponseReceived(clientId, resp.accepted(), QString::fromStdString(resp.reason()));
        }
        break;
    }
    // Özel sohbet
    case okidia::MessageType::PRIVATE_CHAT_REQUEST:
        handlePrivateChatRequest(envelope, clientId);
        break;
    case okidia::MessageType::PRIVATE_CHAT_ACCEPT:
        handlePrivateChatAccept(envelope, clientId);
        break;
    case okidia::MessageType::PRIVATE_CHAT_REJECT:
        handlePrivateChatReject(envelope, clientId);
        break;
    case okidia::MessageType::PRIVATE_CHAT_OPEN:
        handlePrivateChatOpen(envelope, clientId);
        break;
    case okidia::MessageType::PRIVATE_CHAT_MESSAGE:
        handlePrivateChatMessage(envelope, clientId);
        break;
    case okidia::MessageType::PRIVATE_CHAT_CLOSE:
        handlePrivateChatClose(envelope, clientId);
        break;
    // Agent pencere durumu
    case okidia::MessageType::CHAT_WINDOW_STATE:
        handleChatWindowState(envelope, clientId);
        break;
    case okidia::MessageType::AGENT_TYPE_SET_RESULT: {
        okidia::AgentTypeSetResult res;
        if (res.ParseFromString(envelope.payload())) {
            bool typeChanged = true;
            if (res.approved()) {
                AgentRecord dbRec = OkidiaDatabase::instance().agent(clientId);
                if (!dbRec.machineId.isEmpty()) {
                    dbRec.agentType = res.requested_type();
                    OkidiaDatabase::instance().upsertAgent(dbRec);
                }
                if (m_clientStates.contains(clientId)) {
                    if (m_clientStates[clientId].agentType == res.requested_type()) {
                        typeChanged = false;
                    } else {
                        m_clientStates[clientId].agentType = res.requested_type();
                        if (m_thumbnails.contains(clientId))
                            m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
                    }
                }
            }

            // Sadece gerçekten tip değiştiyse veya reddedildiyse UI'a bildir
            if (typeChanged || !res.approved()) {
                emit agentTypeSetResult(clientId, res.approved(), 
                                        QString::fromStdString(res.reason()));
            }
        }
        break;
    }
    case okidia::MessageType::AGENT_ALIAS_SET: {
        okidia::AgentAliasSet msg;
        if (msg.ParseFromString(envelope.payload())) {
            QString machineId = QString::fromStdString(msg.machine_id());
            QString aliasStr  = QString::fromStdString(msg.alias()).trimmed();

            AgentRecord dbRec = OkidiaDatabase::instance().agent(machineId);
            if (!dbRec.machineId.isEmpty()) {
                dbRec.alias = aliasStr;
                OkidiaDatabase::instance().upsertAgent(dbRec);
            }

            for (auto it = m_clientStates.begin(); it != m_clientStates.end(); ++it) {
                if (it.value().machineId == machineId || it.key() == machineId) {
                    it.value().alias = aliasStr;
                    if (m_thumbnails.contains(it.key())) {
                        m_thumbnails[it.key()]->updateState(it.value());
                    }
                    emit clientStateChanged(it.key(), it.value());
                    break;
                }
            }
        }
        break;
    }
    case okidia::MessageType::CURSOR_SHAPE_CHANGE:
        handleCursorShapeChange(envelope, clientId);
        break;
    case okidia::MessageType::ROLLBACK_TO_STABLE_RES: {
        okidia::RollbackToStableResponse resp;
        if (resp.ParseFromString(envelope.payload())) {
            Logger::info(QObject::tr("SessionManager: ROLLBACK_TO_STABLE_RES alındı. İstemci: %1, Başarılı: %2, Hata: %3, Sürüm: %4")
                         .arg(clientId).arg(resp.success() ? "Evet" : "Hayır")
                         .arg(QString::fromStdString(resp.error_msg()))
                         .arg(QString::fromStdString(resp.restored_version())));
            emit rollbackCompleted(clientId, resp.success(),
                                   QString::fromStdString(resp.error_msg()),
                                   QString::fromStdString(resp.restored_version()));
        }
        break;
    }
    case okidia::MessageType::COMMAND_RESULT: {
        okidia::CommandResult cmdRes;
        if (cmdRes.ParseFromString(envelope.payload())) {
            Logger::info(QObject::tr("SessionManager: COMMAND_RESULT alındı. İstemci: %1, Başarılı: %2, Mesaj: %3")
                         .arg(clientId).arg(cmdRes.success() ? "Evet" : "Hayır")
                         .arg(QString::fromStdString(cmdRes.error_message())));
            emit commandResultReceived(clientId, cmdRes.success(), QString::fromStdString(cmdRes.error_message()));
        }
        break;
    }
    default:
        break;
    }

}

void SessionManager::handleClientInfo(
    const okidia::Envelope& env) {

    okidia::ClientInfo info;
    if (!info.ParseFromString(env.payload())) return;

    QString clientId = QString::fromStdString(info.machine_id());
    if (clientId.isEmpty())
        clientId = QString::fromStdString(info.hostname());

    // Broker bağlantı kopunca __disconnected__ hostname ile bildirir
    if (QString::fromStdString(info.hostname()) == "__disconnected__") {
        if (m_clientStates.contains(clientId)) {
            ClientState& st = m_clientStates[clientId];
            bool wasConnected = st.connected;
            st.connected = false;
            if (m_thumbnails.contains(clientId)) {
                m_thumbnails[clientId]->updateState(st);
                m_thumbnails[clientId]->updateFrame(QByteArray()); // siyah ekran
            }
            Logger::info(QObject::tr("SessionManager: Client çevrimdışı → ") + clientId);
            emit clientDisconnected(clientId);
            if (wasConnected) {
                updateAdaptiveFps();
            }
        }
        return;
    }

    bool wasConnected = m_clientStates.value(clientId).connected;
    const QString prevUsername = m_clientStates.value(clientId).username;
    ClientState state = m_clientStates.value(clientId);
    state.clientId   = clientId;
    state.hostname   = QString::fromStdString(info.hostname());
    state.username   = QString::fromStdString(info.username());
    state.userDisplayName = QString::fromStdString(info.display_name());
    state.connected  = true;
    state.machineId  = clientId;
    state.agentType  = info.agent_type();
    state.alias      = QString::fromStdString(info.alias());
    if (state.alias.isEmpty()) {
        const AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
        if (!rec.alias.isEmpty()) {
            state.alias = rec.alias;
        }
    }
    state.osPrettyName = QString::fromStdString(info.os_version());
    QString rawOsTag = QString::fromStdString(info.os_tag());
    QString prettyDerivedTag = OkidiaUtils::normalizeOsTag(state.osPrettyName);
    if (prettyDerivedTag == "debian12" || prettyDerivedTag == "debian13" ||
        prettyDerivedTag == "debian11" || prettyDerivedTag == "debian10" ||
        prettyDerivedTag == "ubuntu24" || prettyDerivedTag == "ubuntu22" ||
        prettyDerivedTag == "ubuntu20" || prettyDerivedTag == "windows") {
        state.osTag = prettyDerivedTag;
    } else {
        state.osTag = OkidiaUtils::normalizeOsTag(rawOsTag);
    }
    state.osArch     = OkidiaUtils::normalizeArch(QString::fromStdString(info.os_arch()));
    state.agentVersion = QString::fromStdString(info.agent_version());
    state.screenWidth = info.screen_width();
    state.screenHeight = info.screen_height();
    state.screens = QString::fromStdString(info.screens());
    state.rtcBatteryLow = info.rtc_battery_low();
    if (state.screens.isEmpty() && state.screenWidth > 0 && state.screenHeight > 0) {
        state.screens = QString("%1x%2").arg(state.screenWidth).arg(state.screenHeight);
    }
    // IP: proto'dan al, yoksa DB'deki agents kaydından bak
    QString oldIp = state.ip;
    state.ip = QString::fromStdString(info.ip_address());
    state.mac = !info.mac_wired().empty() ? QString::fromStdString(info.mac_wired()) : QString::fromStdString(info.mac_wifi());
    if (state.ip.isEmpty() || state.mac.isEmpty() || oldIp.isEmpty()) {
        const AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
        if (oldIp.isEmpty()) oldIp = rec.ip;
        if (state.ip.isEmpty()) state.ip = rec.ip;
        if (state.mac.isEmpty()) state.mac = rec.mac;
    }

    if (!state.ip.isEmpty() && !state.osTag.isEmpty()) {
        setHostOs(state.ip, state.osTag);
    }

    // Veritabanındaki kayıtları güncelle
    if (OkidiaDatabase::instance().isOpen()) {
        AgentRecord rec = OkidiaDatabase::instance().agent(clientId);
        if (!rec.machineId.isEmpty()) {
            bool changed = false;
            if (rec.hostname != state.hostname) {
                rec.hostname = state.hostname;
                changed = true;
            }
            if (!state.ip.isEmpty() && rec.ip != state.ip) {
                rec.ip = state.ip;
                changed = true;
            }
            if (rec.agentType != state.agentType) {
                rec.agentType = state.agentType;
                changed = true;
            }
            if (!state.osTag.isEmpty() && rec.osInfo != state.osTag) {
                rec.osInfo = state.osTag;
                changed = true;
            }
            if (!state.osPrettyName.isEmpty() && rec.osPrettyName != state.osPrettyName) {
                rec.osPrettyName = state.osPrettyName;
                changed = true;
            }
            if (!state.osArch.isEmpty() && rec.osArch != state.osArch) {
                rec.osArch = state.osArch;
                changed = true;
            }
            if (!state.agentVersion.isEmpty() && rec.agentVer != state.agentVersion) {
                rec.agentVer = state.agentVersion;
                changed = true;
            }
            if (changed) {
                OkidiaDatabase::instance().upsertAgent(rec);
                Logger::info(QObject::tr("SessionManager: Agent veritabanı kaydı güncellendi → ") + clientId);
            }
        }
    }

    // SSH kimliklerini IP ve Hostname değişikliklerine karşı senkronize et
    if (!state.ip.isEmpty()) {
        for (auto cred : m_sshCredentials) {
            if (cred.targetType == "agent") {
                bool isMatch = (!cred.machineId.isEmpty() && cred.machineId == clientId) ||
                               (!oldIp.isEmpty() && cred.targetId == oldIp);
                if (isMatch) {
                    bool credChanged = false;
                    if (cred.targetId != state.ip) {
                        cred.targetId = state.ip;
                        credChanged = true;
                    }
                    if (cred.machineId != clientId) {
                        cred.machineId = clientId;
                        credChanged = true;
                    }
                    if (!state.hostname.isEmpty() && state.hostname != "Linux PC" && state.hostname != "Windows PC" &&
                        state.hostname != "Bilinmeyen Cihaz" && state.hostname != tr("Bilinmeyen Cihaz")) {
                        int colonIdx = cred.name.lastIndexOf(": ");
                        QString prefix = (colonIdx != -1) ? cred.name.left(colonIdx + 2) : "Manuel: ";
                        QString expectedName = prefix + state.hostname;
                        if (cred.name != expectedName) {
                            cred.name = expectedName;
                            credChanged = true;
                        }
                    }
                    if (credChanged) {
                        addSshCredential(cred);
                        Logger::info(QObject::tr("SessionManager: Agent IP/Ad değişikliği sonrası SSH kimliği güncellendi → ")
                                     + state.hostname + " (" + state.ip + ")");
                    }
                }
            }
        }
    }

    m_clientStates[clientId] = state;

    // Ekran boyutunu thumbnail'a ilet
    if (m_thumbnails.contains(clientId)) {
        m_thumbnails[clientId]->setFrameSize(
            info.screen_width(),
            info.screen_height());
    }

    m_hashToClientId[qHash(clientId)] = clientId;

    // Thumbnail güncelle
    if (m_thumbnails.contains(clientId)) {
        m_thumbnails[clientId]->updateState(state);
    }

    Logger::info(QObject::tr("SessionManager: ClientInfo → ") +
                 state.hostname + " / " + state.username);

    emit clientConnected(clientId, state);
    if (!wasConnected) {
        if (m_conn && m_conn->isConnected() && m_isAuthenticated && !m_isOfflineMode &&
            m_visibleClients.contains(clientId) && !m_streamingClients.contains(clientId)) {
            int intervalMs = calculateAdaptiveFps(m_visibleClients.size());
            okidia::ScreenConfig cfg;
            cfg.set_interval_ms(static_cast<uint32_t>(intervalMs));
            cfg.set_jpeg_quality(60);
            cfg.set_max_width(360);
            okidia::Envelope fpsEnv;
            fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
            fpsEnv.set_payload(cfg.SerializeAsString());
            fpsEnv.set_target_id(clientId.toStdString());
            m_conn->sendEnvelope(fpsEnv);
            Logger::info(QObject::tr("SessionManager: Yeniden bağlanan görünür istemciye doğrudan SCREEN_FPS_SET gönderildi → %1 (%2 ms)").arg(clientId).arg(intervalMs));
        }
        updateAdaptiveFps();
    } else if (prevUsername != state.username && !m_isStreamPaused &&
               m_visibleClients.contains(clientId) && !m_streamingClients.contains(clientId) &&
               state.agentType != 2) {
        // Kullanıcı oturum açma/kapama geçişinde yeni masaüstünün tam karesini 600ms sonra otomatik talep et
        QTimer::singleShot(600, this, [this, clientId]() {
            if (!m_conn || !m_conn->isConnected() || !m_isAuthenticated || m_isOfflineMode || m_isStreamPaused) return;
            if (!m_visibleClients.contains(clientId) || m_streamingClients.contains(clientId)) return;
            const int intervalMs = calculateAdaptiveFps(m_visibleClients.size());
            if (intervalMs <= 0) return;
            okidia::ScreenConfig cfg;
            cfg.set_interval_ms(static_cast<uint32_t>(intervalMs));
            cfg.set_jpeg_quality(60);
            cfg.set_max_width(360);
            okidia::Envelope fpsEnv;
            fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
            fpsEnv.set_payload(cfg.SerializeAsString());
            fpsEnv.set_target_id(clientId.toStdString());
            m_conn->sendEnvelope(fpsEnv);
        });
    }
}

void SessionManager::handleScreenFrame(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::ScreenFrame frame;
    if (!frame.ParseFromString(env.payload())) return;

    // B2: Protobuf internal string referansından doğrudan erişim (zero-copy)
    const auto& jpegStr = frame.jpeg_data();
    QByteArray jpegData = QByteArray::fromRawData(jpegStr.data(), static_cast<int>(jpegStr.size()));

    // Her tam frame thumbnail'i güncelle
    // HQ frame (is_full_frame=true, büyük boyut) de thumbnail'e gitsin —
    // thumbnail zaten scaled gösteriyor
    if (m_thumbnails.contains(clientId)) {
        m_thumbnails[clientId]->updateFrame(jpegData);
    } else {
        // MainWindow henüz thumbnail oluşturmamışsa, bu kareyi sakla
        if (m_clientStates.contains(clientId)) {
            m_clientStates[clientId].initialFrame = jpegData;
            m_clientStates[clientId].initialFrame.detach(); // Derin kopyalamayı zorla çünkü jpegData geçici raw veriye işaret ediyor
        }
    }

    emit clientFrameReceived(clientId, jpegData);
}

void SessionManager::handleTileUpdate(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::ScreenTileUpdate update;
    if (!update.ParseFromString(env.payload())) return;

    if (update.screen_width() > 0 && update.screen_height() > 0) {
        emit clientStreamSizeReceived(clientId,
                                      static_cast<int>(update.screen_width()),
                                      static_cast<int>(update.screen_height()));
    }

    // Thumbnail güncelle — batch modda (showNow=false)
    if (m_thumbnails.contains(clientId)) {
        ClientThumbnail* thumb = m_thumbnails[clientId];
        if (update.screen_width() > 0 && update.screen_height() > 0) {
            thumb->setFrameSize(
                static_cast<int>(update.screen_width()),
                static_cast<int>(update.screen_height()));
        }
        for (int i = 0; i < update.tiles_size(); ++i) {
            const auto& tile = update.tiles(i);
            const auto& tileJpeg = tile.jpeg_data();  // B2: doğrudan referans
            QByteArray tileData = QByteArray::fromRawData(tileJpeg.data(), static_cast<int>(tileJpeg.size()));
            thumb->updateTile(tile.x(), tile.y(),
                              tile.width(), tile.height(),
                              tileData,
                              false);  // batch: render atla
        }
        // Tüm tile'lar buffer'a yazıldı — tek seferde ekrana yansıt
        thumb->refreshDisplay();
    }

    // RemoteControlView'a thumbnail ile aynı ölçekte ilet
    for (int i = 0; i < update.tiles_size(); ++i) {
        const auto& tile = update.tiles(i);
        const auto& tileJpeg2 = tile.jpeg_data();  // B2: doğrudan referans
        QByteArray tileData = QByteArray::fromRawData(tileJpeg2.data(), static_cast<int>(tileJpeg2.size()));
        emit clientTileReceived(clientId,
                                static_cast<int>(tile.x()),
                                static_cast<int>(tile.y()),
                                static_cast<int>(tile.width()),
                                static_cast<int>(tile.height()),
                                tileData);
    }
    
    // Tüm tile'lar gönderildikten sonra RemoteControlView'ın tek seferde render alması için sinyal gönder
    emit clientTileBatchFinished(clientId);
}

void SessionManager::handleHeartbeat(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::Heartbeat hb;
    if (!hb.ParseFromString(env.payload())) return;

    Logger::info(QObject::tr("SessionManager: Heartbeat ← ") +
                 clientId +
                 QObject::tr(" CPU:%") + QString::number(hb.cpu_usage(), 'f', 1) +
                 QObject::tr(" RAM:%") + QString::number(hb.mem_usage(), 'f', 1));
}

// ============================================================================
// Faz 4b — SCREEN_CAPTURE_RESULT
// ============================================================================

void SessionManager::handleScreenCaptureResult(
    const okidia::Envelope& env,
    const QString& clientId) {

    okidia::ScreenCaptureResult result;
    if (!result.ParseFromString(env.payload())) return;

    QByteArray pngData = QByteArray::fromStdString(result.png_data());
    if (pngData.isEmpty()) return;

    QPixmap pixmap;
    if (!pixmap.loadFromData(pngData, "PNG")) return;

    Logger::info(QObject::tr("SessionManager: Ekran görüntüsü alındı ← ") + clientId +
                 " (" + QString::number(pixmap.width()) +
                 QObject::tr("x") + QString::number(pixmap.height()) + ")");

    emit screenCaptureReceived(clientId, pixmap,
                                static_cast<qint64>(result.timestamp()));
}

void SessionManager::handleSshCredList(const okidia::Envelope& env) {
    okidia::SshCredListResponse resp;
    if (!resp.ParseFromString(env.payload())) return;

    m_sshCredentials.clear();
    for (int i = 0; i < resp.credentials_size(); ++i) {
        const auto& c = resp.credentials(i);
        SshCredentialRecord rec;
        rec.id = QString::fromStdString(c.id());
        rec.name = QString::fromStdString(c.name());
        rec.username = QString::fromStdString(c.username());
        
        // Şifreyi çöz
        QByteArray encrypted = QByteArray::fromStdString(c.secret());
        rec.secret = AesGcm::decrypt(encrypted, AesGcm::defaultSystemKey());
        
        rec.isKey = c.is_key();
        rec.ownerUserId = QString::fromStdString(c.owner_id());
        rec.targetId = QString::fromStdString(c.target_id());
        rec.targetType = QString::fromStdString(c.target_type());
        rec.machineId = QString::fromStdString(c.machine_id());
        rec.createdAt = c.created_at();
        m_sshCredentials.append(rec);

        // Disaster Recovery: Yerel SQLite veritabanına da kalıcı olarak yaz
        if (OkidiaDatabase::instance().isOpen()) {
            OkidiaDatabase::instance().upsertSshCredential(rec);
        }
    }
    emit sshCredentialsLoaded();
}

void SessionManager::handleJoinCodeResponse(const okidia::Envelope& env) {
    okidia::JoinCodeResponse resp;
    if (resp.ParseFromString(env.payload())) {
        emit joinCodeReceived(QString::fromStdString(resp.code()), 
                              resp.success(), 
                              QString::fromStdString(resp.error_msg()));
    }
}

void SessionManager::sendChatMuteState(const QString& clientId, const QString& username,
                                       bool muted, int durationSecs, const QString& reason) {
    if (clientId.isEmpty()) return;
    okidia::ChatMuteState proto;
    proto.set_client_id(clientId.toStdString());
    proto.set_username(username.toStdString());
    proto.set_muted(muted);
    proto.set_duration_secs(durationSecs > 0 ? static_cast<uint32_t>(durationSecs) : 0);
    proto.set_reason(reason.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_MUTE_STATE);
    env.set_target_id(clientId.toStdString());
    env.set_payload(proto.SerializeAsString());
    broadcastCommand(env);
}

