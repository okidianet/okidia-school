#include "AgentApp.h"
#include "AgentIpcServer.h"
#include "AgentDiscoveryCoordinator.h"
#include "LocalScheduleWatchdog.h"
#include "../network/DiscoveryService.h"
#include "../system/SystemInfo.h"
#include "../system/PersistentStateManager.h"
#include "../web/WebFilter.h"
#include "../transfer/FileReceiver.h"
#include "../auth/OfflineAuthManager.h"
#include "../OtaUpdater.h"
#include "../../common/crypto/KeyManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/utils/PlatformService.h"
#include "../../common/OkidiaConfig.h"
#include "../system/SessionSwitchManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>
#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <QSslSocket>
#endif

AgentApp::AgentApp(QObject* parent) : QObject(parent) {
}

AgentApp::~AgentApp() {
#if defined(Q_OS_WIN)
    if (m_hUserTrayWait) {
        UnregisterWait(reinterpret_cast<HANDLE>(m_hUserTrayWait));
        m_hUserTrayWait = nullptr;
    }
    if (m_hUserTrayProc) {
        CloseHandle(reinterpret_cast<HANDLE>(m_hUserTrayProc));
        m_hUserTrayProc = nullptr;
    }
    if (m_hDmTrayProc) {
        CloseHandle(reinterpret_cast<HANDLE>(m_hDmTrayProc));
        m_hDmTrayProc = nullptr;
    }
#else
    if (m_dmTrayProc) {
        m_dmTrayProc->terminate();
        m_dmTrayProc->waitForFinished(1000);
        delete m_dmTrayProc;
        m_dmTrayProc = nullptr;
    }
#endif
}

bool AgentApp::isUiMessage(okidia::MessageType t) {
    switch (t) {
    case okidia::MessageType::SCREEN_LOCK:
    case okidia::MessageType::SCREEN_UNLOCK:
    case okidia::MessageType::SCREEN_CAPTURE:
    case okidia::MessageType::SCREEN_SELECT:
    case okidia::MessageType::SCREEN_INFO_REQUEST:
    case okidia::MessageType::SCREEN_FPS_SET:
    case okidia::MessageType::SCREEN_STREAM_PAUSE:
    case okidia::MessageType::REFRESH_REQUEST:
    case okidia::MessageType::INPUT_LOCK:
    case okidia::MessageType::INPUT_UNLOCK:
    case okidia::MessageType::INPUT_TAKE_CONTROL:
    case okidia::MessageType::REMOTE_CONTROL_REQUEST:
    case okidia::MessageType::REMOTE_CONTROL_STOP:
    case okidia::MessageType::MOUSE_EVENT:
    case okidia::MessageType::KEY_EVENT:
    case okidia::MessageType::AUDIO_BROADCAST_FILE:
    case okidia::MessageType::AUDIO_BROADCAST_LIVE:
    case okidia::MessageType::AUDIO_BROADCAST_STOP:
    case okidia::MessageType::AUDIO_PLAY_LOCAL:
    case okidia::MessageType::AUDIO_UNMUTE_ONLY:
    case okidia::MessageType::AUDIO_MUTE:
    case okidia::MessageType::AUDIO_UNMUTE:
    case okidia::MessageType::AUDIO_SET_VOLUME:
    case okidia::MessageType::MSG_POPUP:
    case okidia::MessageType::MSG_BROADCAST:
    case okidia::MessageType::APP_LIST_REQUEST:
    case okidia::MessageType::APP_CLOSE:
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
    case okidia::MessageType::FILE_TRANSFER_BEGIN:
    case okidia::MessageType::FILE_TRANSFER_CHUNK:
    case okidia::MessageType::FILE_TRANSFER_END:
    case okidia::MessageType::BROADCAST_BEGIN:
    case okidia::MessageType::BROADCAST_FRAME:
    case okidia::MessageType::BROADCAST_END:
    case okidia::MessageType::EXAM_START:
    case okidia::MessageType::EXAM_COLLECT:
    case okidia::MessageType::EXAM_END:
    case okidia::MessageType::TEACHER_KEY_SYNC_REQ:
        return true;
    default:
        return false;
    }
}

bool AgentApp::init() {
#if defined(Q_OS_WIN)
    qunsetenv("QT_QPA_PLATFORM");
    QLocale::setDefault(QLocale::c());

    QSslSocket::setActiveBackend(QStringLiteral("openssl"));
    Logger::info(QObject::tr("Agent: TLS Backend → %1 (Kütüphane: %2)")
                 .arg(QSslSocket::activeBackend())
                 .arg(QSslSocket::sslLibraryVersionString()));
#endif

    m_tls = new TlsClient(this);
    m_tls->setAutoReconnect(true, 5000);

    m_ipcServer = new AgentIpcServer(m_tls, this);

    // Kriptografik IPC Token yönetimi (Named Pipe / Local Socket güvenliği)
    QDir().mkpath(OkidiaConfig::runDir());
    const QString tokenPath = OkidiaConfig::runDir() + "/ipc_token";

    // Servis yeniden başladığında (restart/crash/reload), mevcut oturumdaki Tray'leri düşürmemek
    // için diskte önceden geçerli bir token varsa onu koru ve yeniden kullan.
    QFile tokenFile(tokenPath);
    if (tokenFile.open(QIODevice::ReadOnly)) {
        QString existing = QString::fromUtf8(tokenFile.readAll()).trimmed();
        tokenFile.close();
        if (!existing.isEmpty() && existing.length() >= 16) {
            m_ipcToken = existing;
            Logger::info(QObject::tr("Agent: Mevcut IPC Token diskten yüklendi."));
        }
    }

    if (m_ipcToken.isEmpty()) {
        m_ipcToken = OkidiaUtils::generateUuid();
        if (tokenFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            tokenFile.write(m_ipcToken.toUtf8());
            tokenFile.close();
#if !defined(Q_OS_WIN)
            QFile::setPermissions(tokenPath,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                QFileDevice::ReadGroup |
                QFileDevice::ReadOther);
#endif
            Logger::info(QObject::tr("Agent: Yeni IPC Token üretildi ve kaydedildi."));
        }
    }
    m_ipcServer->setExpectedIpcToken(m_ipcToken);

    // Agent özel anahtarı varsa izinlerini sertleştir ve bütünlüğünü doğrula
    const QString agentKeyPath = OkidiaConfig::agentCertDir() + "/agent.key";
    if (QFile::exists(agentKeyPath)) {
        OkidiaUtils::securePrivateKeyFile(agentKeyPath, true);
        if (OkidiaUtils::verifyKeyIntegrity(agentKeyPath)) {
            Logger::info(QObject::tr("Agent: agent.key güvenlik izinleri ve bütünlüğü doğrulandı (SHA-256: %1)")
                         .arg(OkidiaUtils::computeFileSha256(agentKeyPath).left(12)));
        } else {
            Logger::warning(QObject::tr("Agent: agent.key bozuk veya geçersiz!"));
        }
    }

    if (!m_ipcServer->start()) {
        return false;
    }

    connect(m_ipcServer, &AgentIpcServer::agentTypeUpdated, this, [this](int newType) {
        m_agentType = newType;
    });

    m_webFilter = new WebFilter(this);
    if (PersistentStateManager::hasWebBlock()) {
        QJsonObject wb = PersistentStateManager::webBlockData();
        okidia::Envelope env;
        if (wb["type"].toString() == "block_all") {
            env.set_type(okidia::MessageType::WEB_BLOCK_ALL);
            okidia::WebFilterCommand cmd;
            cmd.set_reason(wb["reason"].toString().toStdString());
            env.set_payload(cmd.SerializeAsString());
        }
        if (env.type() != 0) {
            m_webFilter->handleEnvelope(env);
            Logger::info(QObject::tr("Agent: Kalıcı web kısıtlaması (persistent state) başlangıçta uygulandı."));
        }
    }

    m_otaReceiver = new FileReceiver(this);
    connect(m_otaReceiver, &FileReceiver::otaUpdateReady, this, [this](const QString& path) {
        Logger::info(QObject::tr("Agent: Otonom OTA paketi başarıyla diske yazıldı: ") + path);
        m_pendingOtaPath = path;
        tryTriggerOta();
    });

    connect(m_tls, &TlsClient::connected, this, &AgentApp::onBrokerConnected);
    connect(m_tls, &TlsClient::disconnected, this, [this]() {
        if (m_scheduleWatchdog) {
            m_scheduleWatchdog->setBrokerConnected(false);
        }
    });

    m_rtcBatteryLow = SystemInfo::isRtcBatteryFaulty();
    if (m_rtcBatteryLow) {
        Logger::warning(QObject::tr("AgentApp: Donanım saati (RTC) / BIOS pili arızalı veya sistem yılı 2024 öncesi!"));
    }

    m_scheduleWatchdog = new LocalScheduleWatchdog(this);
    m_scheduleWatchdog->setRtcBatteryLow(m_rtcBatteryLow);
    m_scheduleWatchdog->setBrokerConnected(m_tls->isConnected());

    connect(m_scheduleWatchdog, &LocalScheduleWatchdog::triggerLocalAudio, this, [this](const QString& audioPath) {
        okidia::AudioPlayLocalCommand cmd;
        cmd.set_anons_path(audioPath.toStdString());
        cmd.set_repeat_count(1);
        cmd.set_is_bell(true);
        okidia::Envelope env;
        env.set_type(okidia::MessageType::AUDIO_PLAY_LOCAL);
        env.set_payload(cmd.SerializeAsString());
        m_ipcServer->broadcast(env);
    });

    connect(m_scheduleWatchdog, &LocalScheduleWatchdog::triggerSmartboardLock, this, [this]() {
        okidia::Envelope env;
        env.set_type(okidia::MessageType::SCREEN_LOCK);
        m_ipcServer->broadcast(env);
    });

    connect(m_scheduleWatchdog, &LocalScheduleWatchdog::triggerSmartboardUnlock, this, [this]() {
        PersistentStateManager::clearScreenLock();
        okidia::Envelope env;
        env.set_type(okidia::MessageType::SCREEN_UNLOCK);
        m_ipcServer->broadcast(env);
    });

    m_metricsTimer = new QTimer(this);
    m_metricsTimer->setInterval(10000);
    connect(m_metricsTimer, &QTimer::timeout, this, &AgentApp::onMetricsTimer);
    // Talep tabanlı telemetri: Varsayılan olarak kapalıdır (start çağrılmaz).
    // Yalnızca Master cihaz detay kartını açtığında (METRICS_STREAM_START) başlar.

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(15000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &AgentApp::onHeartbeatTimer);
    m_heartbeatTimer->start();

    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setInterval(2000);
    connect(m_sessionTimer, &QTimer::timeout, this, &AgentApp::checkSession);
    m_sessionTimer->start();
    checkSession();

#if defined(Q_OS_WIN)
    PlatformService::setSessionChangeCallback([this]() {
        QMetaObject::invokeMethod(this, &AgentApp::checkSession, Qt::QueuedConnection);
    });
#endif

    m_discovery = new AgentDiscoveryCoordinator(m_tls, m_ipcServer->server(), this);
    m_discovery->start();

    return true;
}

void AgentApp::sendClientInfo(const QString& username, uint32_t w, uint32_t h) {
    if (!m_tls->isConnected() || !m_tls->connection()) return;

    SystemStats stats = SystemInfo::readStats();
    okidia::ClientInfo ci;
    ci.set_hostname(stats.hostname.toStdString());
    ci.set_username(username.toStdString());
    ci.set_display_name(SystemInfo::userDisplayName(username).toStdString());
    ci.set_os_version(stats.osVersion.toStdString());
    ci.set_machine_id(SystemInfo::machineId().toStdString());
    ci.set_ip_address(SystemInfo::ipAddress().toStdString());
    ci.set_screen_width(w);
    ci.set_screen_height(h);
    ci.set_screens(QString("%1x%2").arg(w).arg(h).toStdString());
    ci.set_agent_version(stats.agentVersion.toStdString());
    ci.set_os_tag(stats.osTag.toStdString());
    ci.set_os_arch(stats.osArch.toStdString());
    ci.set_agent_type(static_cast<uint32_t>(m_agentType));
    ci.set_rtc_battery_low(m_rtcBatteryLow);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CLIENT_INFO);
    env.set_payload(ci.SerializeAsString());
    m_tls->connection()->sendEnvelope(env);
}

void AgentApp::sendSystemMetrics() {
    if (!m_tls->isConnected() || !m_tls->connection()) return;

    SystemStats stats = SystemInfo::readStats();
    okidia::SystemMetrics sm;
    sm.set_client_id(SystemInfo::machineId().toStdString());
    sm.set_cpu_usage(static_cast<uint32_t>(stats.cpuUsage));
    sm.set_ram_usage(static_cast<uint32_t>(stats.memUsage));
    sm.set_disk_total(stats.diskTotal);
    sm.set_disk_used(stats.diskUsed);
    sm.set_disk_usage(static_cast<uint32_t>(stats.diskUsage));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SYSTEM_METRICS_REPORT);
    env.set_payload(sm.SerializeAsString());
    m_tls->connection()->sendEnvelope(env);
}

void AgentApp::sendBlackFrame() {
    if (!m_tls->isConnected() || !m_tls->connection()) return;

    static const unsigned char blackJpeg[] = {
        0xFF,0xD8,0xFF,0xE0,0x00,0x10,0x4A,0x46,0x49,0x46,0x00,0x01,
        0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0xFF,0xDB,0x00,0x43,
        0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,0x07,0x07,0x07,0x09,
        0x09,0x08,0x0A,0x0C,0x14,0x0D,0x0C,0x0B,0x0B,0x0C,0x19,0x12,
        0x13,0x0F,0x14,0x1D,0x1A,0x1F,0x1E,0x1D,0x1A,0x1C,0x1C,0x20,
        0x24,0x2E,0x27,0x20,0x22,0x2C,0x23,0x1C,0x1C,0x28,0x37,0x29,
        0x2C,0x30,0x31,0x34,0x34,0x34,0x1F,0x27,0x39,0x3D,0x38,0x32,
        0x3C,0x2E,0x33,0x34,0x32,0xFF,0xC0,0x00,0x0B,0x08,0x00,0x01,
        0x00,0x01,0x01,0x01,0x11,0x00,0xFF,0xC4,0x00,0x1F,0x00,0x00,
        0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0A,0x0B,0xFF,0xC4,0x00,0xB5,0x10,0x00,0x02,0x01,0x03,
        0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7D,
        0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,
        0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,
        0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,
        0x82,0xFF,0xDA,0x00,0x08,0x01,0x01,0x00,0x00,0x3F,0x00,0xFB,
        0xD8,0xFF,0xD9
    };

    QByteArray jpeg(reinterpret_cast<const char*>(blackJpeg), sizeof(blackJpeg));
    okidia::ScreenFrame frame;
    frame.set_width(1);
    frame.set_height(1);
    frame.set_jpeg_data(jpeg.constData(), jpeg.size());
    frame.set_is_full_frame(true);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SCREEN_FRAME);
    env.set_sequence_id(2);
    env.set_payload(frame.SerializeAsString());
    m_tls->connection()->sendEnvelope(env);
}

void AgentApp::tryTriggerOta() {
    if (!m_pendingOtaPath.isEmpty() && !m_pendingSig.isEmpty() && !m_pendingHash.isEmpty() && !m_pendingCert.isEmpty()) {
        QString path = m_pendingOtaPath;
        QByteArray sig = m_pendingSig;
        QByteArray hash = m_pendingHash;
        QByteArray cert = m_pendingCert;

        m_pendingOtaPath.clear();
        m_pendingSig.clear();
        m_pendingHash.clear();
        m_pendingCert.clear();

        Logger::info(QObject::tr("Agent: Otonom OTA paketi ve güvenlik bilgileri hazır. Kurulum başlatılıyor: ") + path);
        OtaUpdater updater;
        updater.installUpdate(path, sig, hash, cert);
    }
}

void AgentApp::onMetricsTimer() {
    sendSystemMetrics();
}

void AgentApp::onHeartbeatTimer() {
    if (!m_tls->isConnected() || !m_tls->connection()) return;
    if (m_tls->isAnonymous()) return;

    SystemStats stats = SystemInfo::readStats();
    okidia::Heartbeat hb;
    hb.set_timestamp(OkidiaUtils::nowSecs());
    hb.set_cpu_usage(stats.cpuUsage);
    hb.set_mem_usage(stats.memUsage);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::HEARTBEAT);
    env.set_payload(hb.SerializeAsString());
    m_tls->connection()->sendEnvelope(env);

    Logger::info(QObject::tr("Agent: Heartbeat → CPU:%") +
                 QString::number(stats.cpuUsage, 'f', 1) +
                 QObject::tr(" RAM:%") + QString::number(stats.memUsage, 'f', 1));
}

void AgentApp::onBrokerConnected(Connection* conn) {
    Logger::info(QObject::tr("Agent: Broker'a bağlandı"));
    PlatformService::notifyStatus(QObject::tr("Bağlı"));

    if (m_scheduleWatchdog) {
        m_scheduleWatchdog->setBrokerConnected(true);
    }

    const QString agentCertPath = OkidiaConfig::agentCertDir() + "/agent.crt";
    const qint64 certNotBefore = KeyManager::getCertificateNotBefore(agentCertPath);
    const qint64 localNow = OkidiaUtils::nowSecs();
    const bool rtcSuspected = (certNotBefore > 0 && localNow < certNotBefore);

    if (m_tls->isAnonymous()) {
        Logger::info(QObject::tr("Agent: Anonymous mod — İlk kayıt öncesi zorunlu zaman senkronu isteniyor..."));
        DiscoveryService::sendTimeSyncRequest(conn, rtcSuspected, true);
    } else {
        if (!m_timeSyncedThisSession || rtcSuspected) {
            Logger::info(QObject::tr("Agent: Sertifikalı mod — Zaman senkronu talep ediliyor (RTC Şüpheli: %1)...")
                         .arg(rtcSuspected ? "Evet" : "Hayır"));
            DiscoveryService::sendTimeSyncRequest(conn, rtcSuspected, false);
        }

        okidia::Envelope refreshEnv;
        refreshEnv.set_type(okidia::MessageType::REFRESH_REQUEST);
        m_ipcServer->broadcast(refreshEnv);
        sendClientInfo(SystemInfo::activeDisplayUser());
    }

    okidia::AgentStatus st;
    st.set_broker_connected(true);
    okidia::Envelope stEnv;
    stEnv.set_type(okidia::MessageType::AGENT_STATUS);
    stEnv.set_payload(st.SerializeAsString());
    m_ipcServer->broadcast(stEnv);

    connect(conn, &Connection::envelopeReceived, this, [this, conn](const okidia::Envelope& env) {
        onBrokerEnvelopeReceived(conn, env);
    });

    connect(conn, &Connection::disconnected, conn, &Connection::deleteLater);
}

void AgentApp::onBrokerEnvelopeReceived(Connection* conn, const okidia::Envelope& env) {
    static QSet<uint64_t> s_activeServiceTransferSeqIds;
    if (env.type() == okidia::MessageType::FILE_TRANSFER_BEGIN) {
        okidia::FileTransferBegin beginMsg;
        if (beginMsg.ParseFromString(env.payload())) {
            QString fname = QString::fromStdString(beginMsg.filename());
            bool isOta = (beginMsg.destination() == "tmp" && fname.startsWith("okidia-agent"));
            bool isAsset = (beginMsg.destination() == "assets");
            if (isOta || isAsset) {
                s_activeServiceTransferSeqIds.insert(env.sequence_id());
                Logger::info(QObject::tr("Agent: Servis seviyesi dosya transferi başladı (%1, seqId=%2, dosya=%3)")
                             .arg(isAsset ? QObject::tr("Varlık/Zil/Logo") : QObject::tr("OTA Güncelleme"))
                             .arg(QString::number(env.sequence_id()), fname));
                m_otaReceiver->handleEnvelope(env);
                return;
            }
        }
    } else if (env.type() == okidia::MessageType::FILE_TRANSFER_CHUNK) {
        if (s_activeServiceTransferSeqIds.contains(env.sequence_id())) {
            m_otaReceiver->handleEnvelope(env);
            return;
        }
    } else if (env.type() == okidia::MessageType::FILE_TRANSFER_END) {
        if (s_activeServiceTransferSeqIds.remove(env.sequence_id())) {
            m_otaReceiver->handleEnvelope(env);
            return;
        }
    }

    if (env.type() == okidia::MessageType::LOG_TAIL_REQ) {
        okidia::LogTailRequest req;
        if (req.ParseFromString(env.payload())) {
            okidia::LogTailResponse resp;
            resp.set_success(true);
            QString targetFile = QString::fromStdString(req.target_file());
            QString logDir = Logger::logDir();
            QString fullPath = targetFile.isEmpty() ? Logger::logFilePath() : (logDir + "/" + targetFile);
            
            QFileInfo fi(fullPath);
            if (fi.exists()) {
                resp.set_file_name(fi.fileName().toStdString());
                resp.set_file_size(fi.size());

                QDateTime fromTime;
                if (!req.from_time().empty()) {
                    fromTime = QDateTime::fromString(QString::fromStdString(req.from_time()), "yyyy-MM-dd hh:mm:ss");
                    if (!fromTime.isValid()) {
                        QTime t = QTime::fromString(QString::fromStdString(req.from_time()), "hh:mm:ss");
                        if (t.isValid()) fromTime = QDateTime(QDate::currentDate(), t);
                    }
                }

                QDateTime toTime;
                if (!req.to_time().empty()) {
                    toTime = QDateTime::fromString(QString::fromStdString(req.to_time()), "yyyy-MM-dd hh:mm:ss");
                    if (!toTime.isValid()) {
                        QTime t = QTime::fromString(QString::fromStdString(req.to_time()), "hh:mm:ss");
                        if (t.isValid()) toTime = QDateTime(QDate::currentDate(), t);
                    }
                }

                QString levelFilter = QString::fromStdString(req.level_filter());
                QString textFilter = QString::fromStdString(req.search_query());
                int maxLines = req.max_lines();

                QStringList lines = Logger::readLogLines(fullPath, maxLines, fromTime, toTime, levelFilter, textFilter);
                for (const QString& l : lines) {
                    resp.add_lines(l.toStdString());
                }
            } else {
                resp.set_success(false);
                resp.set_error_message(QObject::tr("Log dosyası bulunamadı: %1").arg(targetFile).toStdString());
            }

            auto fileList = Logger::availableLogFiles(logDir);
            for (const auto& f : fileList) {
                resp.add_available_files(f.fileName.toStdString());
            }

            okidia::Envelope re;
            re.set_type(okidia::MessageType::LOG_TAIL_RES);
            re.set_payload(resp.SerializeAsString());
            conn->sendEnvelope(re);
            return;
        }
    }

    if (env.type() == okidia::MessageType::ROLLBACK_TO_STABLE_REQ) {
        okidia::RollbackToStableRequest req;
        okidia::RollbackToStableResponse resp;
        resp.set_success(false);

        if (req.ParseFromString(env.payload())) {
            Logger::info(QObject::tr("Agent: Kararlı sürüme geri alma (Rollback) isteği alındı. Hedef sürüm: %1")
                         .arg(QString::fromStdString(req.target_version())));

            QString err;
            bool ok = OtaUpdater::executeRollback(&err);
            resp.set_success(ok);
            if (!ok) {
                resp.set_error_msg(err.toStdString());
            } else {
                resp.set_restored_version(req.target_version());
            }
        } else {
            resp.set_error_msg(QObject::tr("Geçersiz Rollback istek paketi").toStdString());
        }

        okidia::Envelope resEnv;
        resEnv.set_type(okidia::MessageType::ROLLBACK_TO_STABLE_RES);
        resEnv.set_payload(resp.SerializeAsString());
        conn->sendEnvelope(resEnv);
        return;
    }

    if (env.type() == okidia::MessageType::INPUT_TAKE_CONTROL) {
        m_ipcServer->setRemoteControlActive(true);
        m_ipcServer->setLastTakeControlEnv(env);
    } else if (env.type() == okidia::MessageType::REMOTE_CONTROL_STOP) {
        m_ipcServer->clearRemoteControlEnv();
    } else if (env.type() == okidia::MessageType::SCREEN_FPS_SET) {
        m_ipcServer->setLastFpsSetEnv(env);
        Logger::info(QObject::tr("Agent: SCREEN_FPS_SET komutu alındı, aktif Tray'e iletiliyor."));
    } else if (env.type() == okidia::MessageType::SCREEN_STREAM_PAUSE) {
        m_ipcServer->clearFpsSetEnv();
        Logger::info(QObject::tr("Agent: SCREEN_STREAM_PAUSE komutu alındı, aktif Tray'e iletiliyor."));
    } else if (env.type() == okidia::MessageType::SCREEN_SELECT) {
        m_ipcServer->setLastScreenSelectEnv(env);
    } else if (env.type() == okidia::MessageType::SCREEN_LOCK) {
        okidia::ScreenLockCommand cmd;
        if (cmd.ParseFromString(env.payload())) {
            // lock_policy == 3: Teneffüste Öğrenci Oturumu Kapatma (Geçici eylem, diske ASLA kalıcı yazılmaz)
            // grace_period_sec > 0: Nezaket süresi devredeyken tahta henüz fiilen kilitlenmemiştir;
            // erken diske yazım oturum/tray geçişlerinde mükerrer re-play kilitlerine yol açmamalıdır.
            if (cmd.lock_policy() != 3 && cmd.grace_period_sec() == 0) {
                PersistentStateManager::setScreenLock(QString::fromStdString(cmd.message()), cmd.lock_policy());
                if (cmd.lock_policy() > 0) {
                    QSettings s("okidia", "agent");
                    s.setValue("smartboard/lockPolicy", cmd.lock_policy());
                }
            } else if (cmd.lock_policy() == 3) {
                Logger::info(QObject::tr("Agent: Teneffüs öğrenci oturum kapatma (lock_policy=3) geçici eylem olarak işlendi, kalıcı kilit yazılmadı."));
            } else {
                Logger::info(QObject::tr("Agent: Nezaket süresi devrede (%1 sn), kalıcı kilit durumu nezaket süresi sonrasına bırakıldı.").arg(cmd.grace_period_sec()));
            }
        }
    } else if (env.type() == okidia::MessageType::SCREEN_UNLOCK) {
        PersistentStateManager::clearScreenLock();
    } else if (env.type() == okidia::MessageType::INPUT_LOCK) {
        PersistentStateManager::setInputLock();
    } else if (env.type() == okidia::MessageType::INPUT_UNLOCK) {
        PersistentStateManager::clearInputLock();
    } else if (env.type() == okidia::MessageType::WEB_BLOCK_ALL) {
        okidia::WebFilterCommand cmd;
        if (cmd.ParseFromString(env.payload())) {
            PersistentStateManager::setWebBlockAll(QString::fromStdString(cmd.reason()));
        }
    } else if (env.type() == okidia::MessageType::WEB_UNBLOCK) {
        PersistentStateManager::clearWebBlock();
    } else if (env.type() == okidia::MessageType::METRICS_STREAM_START) {
        m_detailedMetrics = true;
        if (m_metricsTimer) {
            m_metricsTimer->setInterval(10000);
            if (!m_metricsTimer->isActive()) {
                m_metricsTimer->start();
            }
            sendSystemMetrics();
            Logger::info(QObject::tr("Agent: Canlı telemetri akışı başlatıldı (METRICS_STREAM_START)"));
        }
        return;
    } else if (env.type() == okidia::MessageType::METRICS_STREAM_STOP) {
        m_detailedMetrics = false;
        if (m_metricsTimer) {
            m_metricsTimer->stop();
            Logger::info(QObject::tr("Agent: Telemetri akışı duraklatıldı (METRICS_STREAM_STOP)"));
        }
        return;
    } else if (env.type() == okidia::MessageType::TIME_SYNC_RESPONSE) {
        okidia::TimeSyncResponse syncResp;
        if (syncResp.ParseFromString(env.payload())) {
            qint64 serverTime = syncResp.server_utc_time();
            bool brokerValid = syncResp.broker_time_valid();
            Logger::info(QObject::tr("Agent: TIME_SYNC_RESPONSE alındı (Sunucu UTC: %1, Geçerli: %2)")
                         .arg(serverTime).arg(brokerValid ? "Evet" : "Hayır"));

            if (brokerValid && serverTime > 0) {
                qint64 currentLocal = OkidiaUtils::nowSecs();
                qint64 diff = std::abs(serverTime - currentLocal);
                if (diff >= 2) {
                    Logger::info(QObject::tr("Agent: Sistem saati Broker saatiyle eşitleniyor (Fark: %1 sn)...").arg(diff));
                    PlatformService::setSystemTimeUtc(serverTime);
                }
                m_timeSyncedThisSession = true;
            }

            if (m_tls->isAnonymous()) {
                const QString certPath = OkidiaConfig::agentCertDir() + "/agent.crt";
                const QString keyPath  = OkidiaConfig::agentCertDir() + "/agent.key";
                const QString caPath   = OkidiaConfig::agentCertDir() + "/ca.crt";

                if (QFile::exists(certPath) && QFile::exists(keyPath) && QFile::exists(caPath)) {
                    Logger::info(QObject::tr("Agent: Saat başarıyla senkronize edildi, şimdi sertifikalı mTLS bağlantısına geçiliyor..."));
                    QTimer::singleShot(500, this, [this, certPath, keyPath, caPath]() {
                        m_tls->reconnectWithCert(certPath, keyPath, caPath);
                    });
                } else {
                    Logger::info(QObject::tr("Agent: Saat senkronize edildi. Şimdi DISCOVERY_REQUEST gönderiliyor..."));
                    DiscoveryService::sendDiscoveryRequest(conn);
                }
            }
        }
        return;
    }

    if (m_agentType > 0) {
        bool allowed = true;
        int effectiveType = (m_agentType >= 100) ? 1 : m_agentType;
        if (effectiveType == 1) {
            allowed = true;
        } else if (effectiveType == 2) {
            if (env.type() == okidia::MessageType::WEB_BLOCK_ALL ||
                env.type() == okidia::MessageType::WEB_UNBLOCK ||
                env.type() == okidia::MessageType::POWER_SHUTDOWN ||
                env.type() == okidia::MessageType::POWER_RESTART ||
                env.type() == okidia::MessageType::POWER_LOGOUT ||
                env.type() == okidia::MessageType::POWER_HIBERNATE ||
                env.type() == okidia::MessageType::SCREEN_LOCK ||
                env.type() == okidia::MessageType::SCREEN_UNLOCK ||
                env.type() == okidia::MessageType::INPUT_LOCK ||
                env.type() == okidia::MessageType::INPUT_UNLOCK ||
                env.type() == okidia::MessageType::EXAM_START ||
                env.type() == okidia::MessageType::EXAM_COLLECT ||
                env.type() == okidia::MessageType::EXAM_END ||
                env.type() == okidia::MessageType::APP_CLOSE ||
                env.type() == okidia::MessageType::APP_BLOCK_BYNAME ||
                env.type() == okidia::MessageType::APP_BLOCK_BYHASH ||
                env.type() == okidia::MessageType::AUDIO_PLAY_LOCAL ||
                env.type() == okidia::MessageType::AUDIO_BROADCAST ||
                env.type() == okidia::MessageType::AUDIO_BROADCAST_LIVE ||
                env.type() == okidia::MessageType::AUDIO_BROADCAST_FILE ||
                env.type() == okidia::MessageType::AUDIO_BROADCAST_STOP ||
                env.type() == okidia::MessageType::AUDIO_STOP ||
                env.type() == okidia::MessageType::AUDIO_MUTE ||
                env.type() == okidia::MessageType::AUDIO_UNMUTE ||
                env.type() == okidia::MessageType::AUDIO_UNMUTE_ONLY ||
                env.type() == okidia::MessageType::AUDIO_SET_VOLUME) {
                allowed = false;
            }
        } else if (effectiveType == 3) {
            if (env.type() == okidia::MessageType::EXAM_START ||
                env.type() == okidia::MessageType::EXAM_COLLECT ||
                env.type() == okidia::MessageType::EXAM_END) {
                allowed = false;
            }
        }

        if (!allowed) {
            Logger::warning(QObject::tr("Agent: Komut servis seviyesinde engellendi (Tip ") + 
                            QString::number(m_agentType) + QObject::tr(") → tip:") + 
                            QString::number(env.type()));
            return;
        }
    }

    switch (env.type()) {
    case okidia::MessageType::POWER_SHUTDOWN: {
        SessionSwitchManager::instance().cleanupAutologinConfig();
        auto pCmd = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Shutdown);
        QProcess::startDetached(pCmd.first, pCmd.second);
        break;
    }
    case okidia::MessageType::POWER_RESTART: {
        SessionSwitchManager::instance().cleanupAutologinConfig();
        auto pCmd = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Reboot);
        QProcess::startDetached(pCmd.first, pCmd.second);
        break;
    }
    case okidia::MessageType::POWER_LOGOUT: {
#if defined(Q_OS_WIN)
        auto pCmd = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Logout);
        QProcess::startDetached(pCmd.first, pCmd.second);
#else
        const QString active = m_ipcServer->server() ? m_ipcServer->server()->activeUser() : QString();
        if (!active.isEmpty() && !SystemInfo::isDmUser(active)) {
            QProcess p;
            p.start("loginctl", {"terminate-user", active});
            p.waitForFinished(3000);
        } else {
            auto pCmd = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Logout);
            QProcess::startDetached(pCmd.first, pCmd.second);
        }
#endif
        break;
    }
    case okidia::MessageType::POWER_HIBERNATE: {
        auto pCmd = OkidiaUtils::localPowerCommand(OkidiaUtils::PowerAction::Suspend);
        QProcess::startDetached(pCmd.first, pCmd.second);
        break;
    }
    case okidia::MessageType::APP_BLOCK_BYNAME:
    case okidia::MessageType::APP_BLOCK_BYHASH:
    case okidia::MessageType::APP_WHITELIST_SET:
    case okidia::MessageType::APP_UNBLOCK:
        break;
    case okidia::MessageType::WEB_BLOCK_ALL:
    case okidia::MessageType::WEB_UNBLOCK:
        m_webFilter->handleEnvelope(env);
        break;
    case okidia::MessageType::CERT_PROVISION: {
        okidia::CertProvision prov;
        if (!prov.ParseFromString(env.payload())) break;
        {
            QFile f(OkidiaConfig::agentCertDir() + "/agent.crt");
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QByteArray::fromStdString(prov.cert_pem()));
                f.close();
                QFile::setPermissions(OkidiaConfig::agentCertDir() + "/agent.crt",
                    QFile::ReadOwner | QFile::WriteOwner |
                    QFile::ReadGroup | QFile::ReadOther);
                Logger::info(QObject::tr("Agent: agent.crt kaydedildi"));
            }
        }
        if (!prov.ca_pem().empty()) {
            QFile f(OkidiaConfig::agentCertDir() + "/ca.crt");
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QByteArray::fromStdString(prov.ca_pem()));
                f.close();
                Logger::info(QObject::tr("Agent: ca.crt güncellendi"));
            }
        }
        Logger::info(QObject::tr("Agent: Sertifika alındı, sertifikalı bağlantıya geçiliyor"));
        QTimer::singleShot(500, [this]() {
            m_tls->reconnectWithCert(
                OkidiaConfig::agentCertDir() + "/agent.crt",
                OkidiaConfig::agentCertDir() + "/agent.key",
                OkidiaConfig::agentCertDir() + "/ca.crt");
        });
        break;
    }
    case okidia::MessageType::DISCOVERY_IGNORE:
        Logger::info(QObject::tr("Agent: Discovery isteği yoksayıldı (kalıcı kara liste)."));
        break;
    case okidia::MessageType::DISCOVERY_REJECT:
        Logger::info(QObject::tr("Agent: Discovery isteği reddedildi, broker bağlantıyı kesecek."));
        break;
    case okidia::MessageType::CLIENT_REVOKE:
        Logger::info(QObject::tr("Agent: CLIENT_REVOKE alindi — sertifikalar siliniyor"));
        QFile::remove(OkidiaConfig::agentCertDir() + "/agent.crt");
        QFile::remove(OkidiaConfig::agentCertDir() + "/agent.key");
        QFile::remove(OkidiaConfig::agentCertDir() + "/ca.crt");
        QFile::remove(OkidiaConfig::agentCertDir() + "/agent.csr");
        Logger::info(QObject::tr("Agent: Sertifikalar silindi, anonymous moda geciliyor"));
        QTimer::singleShot(1500, [this]() {
            m_tls->reconnectAnonymous();
        });
        break;
    case okidia::MessageType::CLIENT_UNINSTALL: {
        Logger::info(QObject::tr("Agent: CLIENT_UNINSTALL alindi — Sistemden tamamen silinme (purge) baslatiliyor..."));
        QFile::remove(OkidiaConfig::agentCertDir() + "/agent.crt");
        QFile::remove(OkidiaConfig::agentCertDir() + "/agent.key");
        QFile::remove(OkidiaConfig::agentCertDir() + "/ca.crt");
        QFile::remove(OkidiaConfig::agentCertDir() + "/agent.csr");
#if defined(Q_OS_WIN)
        {
            QString uPath = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../unins000.exe");
            if (!QFile::exists(uPath)) {
                uPath = "C:\\Program Files\\Okidia\\unins000.exe";
            }
            uPath = QDir::toNativeSeparators(uPath);
            Logger::info(QObject::tr("Agent: Uninstaller başlatılıyor → ") + uPath);
            
            QString keyArg;
            QString keyFile = OkidiaConfig::systemBasePath() + "/Agent/uninstall.key";
            QFile kf(keyFile);
            if (kf.open(QIODevice::ReadOnly)) {
                QString hash = QString::fromUtf8(kf.readAll()).trimmed();
                if (!hash.isEmpty()) {
                    keyArg = QString(" /UNINSTALLKEY=\"%1\"").arg(hash);
                }
                kf.close();
            }

            QString uninsCmd = QString("timeout /t 2 /nobreak >nul & \"%1\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART%2").arg(uPath, keyArg);
            QProcess::startDetached("cmd.exe", QStringList() << "/c" << "start" << "cmd.exe" << "/c" << uninsCmd);
        }
#else
        QString uninsCmd = "sleep 1; systemctl stop okidia-agent 2>/dev/null; DEBIAN_FRONTEND=noninteractive apt-get purge -y okidia-agent";
        bool started = QProcess::startDetached("systemd-run", QStringList() << "--description=Okidia Agent Uninstaller" << "bash" << "-c" << uninsCmd);
        if (!started) {
            QProcess::startDetached("nohup", QStringList() << "bash" << "-c" << uninsCmd);
        }
#endif
        QTimer::singleShot(500, qApp, &QCoreApplication::quit);
        break;
    }
    case okidia::MessageType::AGENT_UNINSTALL_PASS_SET: {
#if defined(Q_OS_WIN)
        QString hash;
        okidia::AgentUninstallPassSet passMsg;
        if (passMsg.ParseFromString(env.payload())) {
            hash = QString::fromStdString(passMsg.password_hash()).trimmed();
        } else {
            hash = QString::fromUtf8(env.payload().c_str()).trimmed();
        }

        if (!hash.isEmpty()) {
            QString keyDir = OkidiaConfig::systemBasePath() + "/Agent";
            QDir().mkpath(keyDir);
            QString keyFile = keyDir + "/uninstall.key";
            QFile kf(keyFile);
            if (kf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                kf.write(hash.toUtf8() + "\r\n");
                kf.close();
                Logger::info(QObject::tr("Agent: Windows kaldırma koruma anahtarı güncellendi."));
            } else {
                Logger::warning(QObject::tr("Agent: Kaldırma koruma anahtarı dosyasına yazılamadı: ") + keyFile);
            }
        }
#endif
        break;
    }
    case okidia::MessageType::AGENT_TYPE_SET: {
        Logger::info(QObject::tr("Agent: Tip atama isteği alındı → Tray'e iletiliyor"));
        m_ipcServer->sendToActive(env);
        break;
    }
    case okidia::MessageType::AGENT_ALIAS_SET: {
        okidia::AgentAliasSet aliasMsg;
        if (aliasMsg.ParseFromString(env.payload())) {
            QString aliasStr = QString::fromStdString(aliasMsg.alias()).trimmed();
            OkidiaConfig::setAgentAlias(aliasStr);
            Logger::info(QObject::tr("Agent: Takma ad servise kaydedildi → ") + aliasStr);
        }
        m_ipcServer->sendToActive(env);
        break;
    }
    case okidia::MessageType::AGENT_POLICY_SET: {
        okidia::AgentPolicySet policy;
        if (policy.ParseFromString(env.payload())) {
            SystemInfo::setSwitchUserDisabled(policy.disable_switch_user());
            SystemInfo::setUsbProtectionEnabled(policy.usb_protection_enabled());
            Logger::info(QObject::tr("Agent: Sistem politikaları güncellendi → Switch User: %1, USB Koruma: %2")
                         .arg(policy.disable_switch_user() ? QObject::tr("KAPALI") : QObject::tr("AÇIK"))
                         .arg(policy.usb_protection_enabled() ? QObject::tr("AÇIK") : QObject::tr("KAPALI")));

            if (m_scheduleWatchdog && !policy.school_schedule_json().empty()) {
                m_scheduleWatchdog->updateSchedule(
                    QString::fromStdString(policy.school_schedule_json()),
                    policy.smartboard_lock_policy(),
                    QString::fromStdString(policy.school_timezone()),
                    policy.bell_smartboard_enabled(),
                    policy.bell_student_pc_enabled(),
                    policy.bell_auto_lock_recess(),
                    policy.last_bell_lock_behavior()
                );
            }

            m_ipcServer->sendToActive(env);
        }
        break;
    }
    case okidia::MessageType::OTA_UPDATE_PUSH: {
        okidia::OtaUpdatePush pushMsg;
        if (pushMsg.ParseFromString(env.payload())) {
            m_pendingSig = QByteArray(pushMsg.signature().data(), pushMsg.signature().size());
            m_pendingHash = QByteArray(pushMsg.file_hash().data(), pushMsg.file_hash().size());
            m_pendingCert = QByteArray(pushMsg.master_cert().data(), pushMsg.master_cert().size());

            QString otaMetaPath = QDir::tempPath() + "/okidia_ota.meta";
            QFile metaFile(otaMetaPath);
            if (metaFile.open(QIODevice::WriteOnly)) {
                QDataStream out(&metaFile);
                out.setVersion(QDataStream::Qt_5_15);
                out << m_pendingSig << m_pendingHash << m_pendingCert;
                metaFile.close();
                Logger::info(QObject::tr("Agent: OTA güvenlik bilgileri servise kaydedildi."));
            }

            tryTriggerOta();
        }
        break;
    }
    case okidia::MessageType::TEACHER_KEY_SYNC_REQ: {
        okidia::TeacherKeySyncReq syncReq;
        if (syncReq.ParseFromString(env.payload())) {
            for (int i = 0; i < syncReq.teachers_size(); ++i) {
                const auto& item = syncReq.teachers(i);
                OfflineAuthManager::instance().upsertTeacher(
                    QString::fromStdString(item.teacher_id()),
                    QString::fromStdString(item.display_name()),
                    QString::fromStdString(item.username()),
                    QString::fromStdString(item.seed_hex()),
                    QString::fromStdString(item.password())
                );
            }
            Logger::info(QObject::tr("Agent Servis: %1 adet öğretmen tohumu yerel depoya senkronize edildi.")
                         .arg(syncReq.teachers_size()));

            m_ipcServer->broadcast(env);

            okidia::TeacherKeySyncRes syncRes;
            syncRes.set_success(true);
            syncRes.set_synced_count(syncReq.teachers_size());

            okidia::Envelope resEnv;
            resEnv.set_type(okidia::MessageType::TEACHER_KEY_SYNC_RES);
            resEnv.set_payload(syncRes.SerializeAsString());
            conn->sendEnvelope(resEnv);
        }
        break;
    }
    case okidia::MessageType::QUERY_LOCAL_USERS_REQ: {
        okidia::QueryLocalUsersRes res;
        res.set_client_id(SystemInfo::machineId().toStdString());
        res.set_success(true);

#if defined(Q_OS_LINUX)
        QFile file(QStringLiteral("/etc/passwd"));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                QStringList parts = line.split(':');
                if (parts.size() >= 7) {
                    QString uname = parts[0].trimmed();
                    uint uid = parts[2].toUInt();
                    QString gecos = parts[4].split(',').first().trimmed();
                    QString home = parts[5].trimmed();
                    QString shell = parts[6].trimmed();

                    // Normal kullanıcı kriterleri: UID >= 1000 ve < 60000, nobody hariç, nologin/false hariç
                    if (uid >= 1000 && uid < 60000 && uname != "nobody" &&
                        !shell.endsWith("/nologin") && !shell.endsWith("/false")) {
                        auto* u = res.add_users();
                        u->set_username(uname.toStdString());
                        u->set_display_name(gecos.isEmpty() ? uname.toStdString() : gecos.toStdString());
                        u->set_uid(uid);
                        u->set_home_dir(home.toStdString());
                    }
                }
            }
            file.close();
            Logger::info(QObject::tr("Agent: Yerel kullanıcılar tarandı, %1 adet kullanıcı bulundu.").arg(res.users_size()));
        } else {
            res.set_success(false);
            res.set_error_msg("Yerel /etc/passwd okunamadı.");
        }
#else
        res.set_success(false);
        res.set_error_msg("Bu işletim sisteminde yerel kullanıcı tarama desteklenmiyor.");
#endif

        okidia::Envelope resEnv;
        resEnv.set_type(okidia::MessageType::QUERY_LOCAL_USERS_RES);
        resEnv.set_target_id(env.target_id());
        resEnv.set_payload(res.SerializeAsString());
        conn->sendEnvelope(resEnv);
        break;
    }
    default:
        if (isUiMessage(env.type())) {
            int effectiveType = (m_agentType >= 100) ? 1 : m_agentType;
            if (effectiveType == 2) {
                if (env.type() == okidia::MessageType::BROADCAST_BEGIN ||
                    env.type() == okidia::MessageType::BROADCAST_FRAME ||
                    env.type() == okidia::MessageType::BROADCAST_END ||
                    env.type() == okidia::MessageType::EXAM_START ||
                    env.type() == okidia::MessageType::EXAM_COLLECT ||
                    env.type() == okidia::MessageType::EXAM_END) {
                    return; 
                }
                if (env.type() == okidia::MessageType::APP_LIST_REQUEST) {
                    okidia::AppListResponse emptyResp;
                    okidia::Envelope resEnv;
                    resEnv.set_type(okidia::MessageType::APP_LIST_RESPONSE);
                    resEnv.set_payload(emptyResp.SerializeAsString());
                    if (m_tls && m_tls->connection()) {
                        m_tls->connection()->sendEnvelope(resEnv);
                    }
                    return;
                }
            }
            if (effectiveType == 3) {
                if (env.type() == okidia::MessageType::EXAM_START ||
                    env.type() == okidia::MessageType::EXAM_COLLECT ||
                    env.type() == okidia::MessageType::EXAM_END) {
                    return;
                }
            }

            if (env.type() == okidia::MessageType::REMOTE_CONTROL_REQUEST && 
                (!m_ipcServer->server() || m_ipcServer->server()->clientCount() == 0)) {
                Logger::info(QObject::tr("Agent: Oturum kapali, remote control istegi otomatik reddedildi."));
                okidia::RemoteControlResponse resp;
                resp.set_accepted(false);
                resp.set_reason("Oturum kapalı (Kullanıcı girişi yapılmamış).");
                
                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::REMOTE_CONTROL_RESPONSE);
                resEnv.set_payload(resp.SerializeAsString());
                conn->sendEnvelope(resEnv);
                return;
            }

            if (env.type() == okidia::MessageType::SCREEN_INFO_REQUEST &&
                (!m_ipcServer->server() || m_ipcServer->server()->clientCount() == 0)) {
                Logger::warning(QObject::tr("Agent: Tray bağlı değil, SCREEN_INFO_REQUEST için varsayılan fallback yanıtı üretiliyor..."));
                okidia::ScreenInfoResponse scrResp;
                auto* mon = scrResp.add_monitors();
                mon->set_index(0);
                mon->set_name("Screen-0");
                mon->set_x(0);
                mon->set_y(0);
                uint32_t w = m_ipcServer->lastKnownScreenW();
                uint32_t h = m_ipcServer->lastKnownScreenH();
                if (w == 0 || h == 0) {
                    w = 1920;
                    h = 1080;
                }
                mon->set_width(w);
                mon->set_height(h);
                mon->set_is_primary(true);

                okidia::Envelope resEnv;
                resEnv.set_type(okidia::MessageType::SCREEN_INFO_RESPONSE);
                resEnv.set_payload(scrResp.SerializeAsString());
                conn->sendEnvelope(resEnv);

                // Tray eksikse oturum kontrolünü hemen tetikleyerek tray'in açılmasını sağla
                QTimer::singleShot(100, this, &AgentApp::checkSession);
                return;
            }

            // Anti-Sabotaj: Aktif yayın ve sınav durumlarını AgentIpcServer önbelleğinde güncelle
            if (env.type() == okidia::MessageType::BROADCAST_BEGIN) {
                m_ipcServer->setLastBroadcastBeginEnv(env);
            } else if (env.type() == okidia::MessageType::BROADCAST_END) {
                m_ipcServer->clearBroadcastEnv();
            } else if (env.type() == okidia::MessageType::EXAM_START) {
                m_ipcServer->setLastExamStartEnv(env);
            } else if (env.type() == okidia::MessageType::EXAM_END) {
                m_ipcServer->clearExamEnv();
            }

            if (env.type() == okidia::MessageType::SCREEN_UNLOCK ||
                env.type() == okidia::MessageType::INPUT_UNLOCK) {
                m_ipcServer->broadcast(env);
            } else if (m_ipcServer->server() && m_ipcServer->server()->clientCount() > 0) {
                m_ipcServer->sendToActive(env);
            } else {
                Logger::warning(QObject::tr("Agent: Tray bağlı değil, UI mesajı atlandı → tip:")
                                + QString::number(env.type()));
                // Tray bağlı değilken gelen UI mesajları tray'in henüz başlatılmamış olabileceğinin işaretidir
                QTimer::singleShot(200, this, &AgentApp::checkSession);
            }
        } else {
            Logger::info(QObject::tr("Agent: İşlenmeyen mesaj → tip:")
                         + QString::number(env.type()));
        }
        break;
    }
}

void AgentApp::checkSession() {
    const QString newActive  = SystemInfo::activeDisplayUser();
    const bool    onLogin    = SystemInfo::isDmUser(newActive);

#if defined(Q_OS_WIN)
    bool dmTrayRunning = false;
    if (m_hDmTrayProc) {
        DWORD waitRes = WaitForSingleObject(reinterpret_cast<HANDLE>(m_hDmTrayProc), 0);
        if (waitRes == WAIT_TIMEOUT) {
            dmTrayRunning = true;
        } else {
            CloseHandle(reinterpret_cast<HANDLE>(m_hDmTrayProc));
            m_hDmTrayProc = nullptr;
        }
    }
    if (!dmTrayRunning && m_ipcServer->server() && m_ipcServer->server()->hasUserConnection(QStringLiteral("winlogon"))) {
        dmTrayRunning = true;
    }
    const bool dmTrayMissing = onLogin && !dmTrayRunning;

    bool userTrayMissing = false;
    if (!onLogin && !newActive.isEmpty() && m_ipcServer->server()) {
        userTrayMissing = !m_ipcServer->server()->hasUserConnection(newActive);
        if (!userTrayMissing) {
            m_userTrayCrashCount = 0;
            m_userTrayBackoffUntil = 0;
        }
    }
#else
    if (onLogin && m_dmTrayProc && m_dmTrayProc->state() != QProcess::Running) {
        Logger::warning(QObject::tr("Agent: DM tray beklenmedik şekilde kapandı, yeniden başlatılıyor..."));
        delete m_dmTrayProc;
        m_dmTrayProc = nullptr;
    }
    const bool dmTrayMissing = onLogin && !m_dmTrayProc;

    bool userTrayMissing = false;
    if (!onLogin && !newActive.isEmpty() && m_ipcServer->server()) {
        userTrayMissing = !m_ipcServer->server()->hasUserConnection(newActive);
        if (!userTrayMissing) {
            m_userTrayCrashCount = 0;
            m_userTrayBackoffUntil = 0;
        }
    }
#endif

    const bool userChanged = (newActive != m_currentActive);
    if (!userChanged && !dmTrayMissing && !userTrayMissing) return;

    if (userChanged) {
        Logger::info(QObject::tr("Agent: Aktif kullanıcı değişti → '") + newActive + "'");
        const QString prevActive = m_currentActive;
        m_currentActive = newActive;

        if (m_ipcServer->server()) {
            m_ipcServer->server()->setActiveUser(newActive);
        }

        if (m_tls->isConnected() && !m_tls->isAnonymous()) {
            if (onLogin && !prevActive.isEmpty() && !SystemInfo::isDmUser(prevActive)) {
                sendBlackFrame();
                Logger::info(QObject::tr("Agent: Oturum kapatıldı, thumbnail temizlendi"));
            }
            sendClientInfo(newActive);
        }
    }

    if (onLogin) {
#if defined(Q_OS_WIN)
        if (!dmTrayRunning) {
            QProcess killProc;
            killProc.start("taskkill.exe", {"/F", "/FI", "USERNAME eq SYSTEM", "/IM", "okidia-tray.exe"});
            killProc.waitForFinished(1000);

            QString trayPath = QCoreApplication::applicationDirPath() + "/okidia-tray.exe";
            if (!QFile::exists(trayPath)) {
                trayPath = "C:/Program Files/Okidia/Agent/bin/okidia-tray.exe";
            }
            if (!QFile::exists(trayPath)) {
                trayPath = "C:/Program Files/Okidia/okidia-tray.exe";
            }
            if (QFile::exists(trayPath)) {
                Logger::info(QObject::tr("Agent: Windows Login/Kilit ekranı algılandı, winlogon masaüstünde konsol izleme başlatılıyor..."));
                void* hProc = nullptr;
                QStringList args = {"--username", "winlogon"};
                if (!m_ipcToken.isEmpty()) {
                    args << "--ipc-token" << m_ipcToken;
                }
                if (SystemInfo::launchProcessInConsoleSession(trayPath, args, &hProc)) {
                    Logger::info(QObject::tr("Agent: Windows konsol oturumunda okidia-tray (winlogon) aktif edildi."));
                    m_hDmTrayProc = hProc;
                } else {
                    Logger::error(QObject::tr("Agent: launchProcessInConsoleSession (winlogon) başarısız!"));
                }
            }
        }
#else
        if (!m_dmTrayProc) {
            const QString xauth = SystemInfo::dmXauthority();
            if (xauth.isEmpty()) {
                Logger::info(QObject::tr("Agent: Login ekranı var, XAUTHORITY bekleniyor..."));
                return;
            }

            Logger::info(QObject::tr("Agent: DM tray başlatılıyor → XAUTH:") + xauth);
            m_dmTrayProc = new QProcess();

            QProcessEnvironment penv = QProcessEnvironment::systemEnvironment();
            penv.insert("DISPLAY", ":0");
            penv.insert("XAUTHORITY", xauth);
            penv.remove("XDG_RUNTIME_DIR");
            penv.remove("DBUS_SESSION_BUS_ADDRESS");
            m_dmTrayProc->setProcessEnvironment(penv);

            QStringList args = {"--username", "lightdm"};
            if (!m_ipcToken.isEmpty()) {
                args << "--ipc-token" << m_ipcToken;
            }
            m_dmTrayProc->start("/usr/bin/okidia-tray", args);
        }
#endif
    } else {
#if defined(Q_OS_WIN)
        if (m_hDmTrayProc) {
            Logger::info(QObject::tr("Agent: Kullanıcı girdi, winlogon DM tray kapatılıyor..."));
            TerminateProcess(reinterpret_cast<HANDLE>(m_hDmTrayProc), 0);
            CloseHandle(reinterpret_cast<HANDLE>(m_hDmTrayProc));
            m_hDmTrayProc = nullptr;
            QProcess killProc;
            killProc.start("taskkill.exe", {"/F", "/FI", "USERNAME eq SYSTEM", "/IM", "okidia-tray.exe"});
            killProc.waitForFinished(1000);
        }

        // Önceki izlenen kullanıcı tray süreci kapandıysa veya IPC'ye bağlanamadan asılı kaldıysa temizle
        qint64 now = QDateTime::currentSecsSinceEpoch();
        if (m_hUserTrayProc) {
            DWORD waitRes = WaitForSingleObject(reinterpret_cast<HANDLE>(m_hUserTrayProc), 0);
            if (waitRes != WAIT_TIMEOUT) {
                if (m_hUserTrayWait) {
                    UnregisterWait(reinterpret_cast<HANDLE>(m_hUserTrayWait));
                    m_hUserTrayWait = nullptr;
                }
                CloseHandle(reinterpret_cast<HANDLE>(m_hUserTrayProc));
                m_hUserTrayProc = nullptr;

                if (now - m_lastUserTrayLaunch <= 15) {
                    m_userTrayCrashCount++;
                    if (m_userTrayCrashCount >= 3) {
                        m_userTrayBackoffUntil = now + 60;
                        Logger::warning(QObject::tr("Agent: Kullanıcı tray süreci kısa sürede peş peşe kapandı (%1 kez). 60 saniye beklemeye alınıyor...").arg(m_userTrayCrashCount));
                    }
                }
            } else if (userTrayMissing && (now - m_lastUserTrayLaunch >= 15)) {
                // Süreç 15 saniyedir açık olduğu halde IPC bağlantısı kuramadıysa (örn. modal hata/asılı kalma)
                // üst üste yeni süreçler açmamak için asılı süreci sonlandır ve hata sayacını artır.
                Logger::warning(QObject::tr("Agent: Kullanıcı tray süreci 15 saniye içinde IPC bağlantısı kuramadı, asılı süreç sonlandırılıyor..."));
                if (m_hUserTrayWait) {
                    UnregisterWait(reinterpret_cast<HANDLE>(m_hUserTrayWait));
                    m_hUserTrayWait = nullptr;
                }
                TerminateProcess(reinterpret_cast<HANDLE>(m_hUserTrayProc), 1);
                CloseHandle(reinterpret_cast<HANDLE>(m_hUserTrayProc));
                m_hUserTrayProc = nullptr;

                m_userTrayCrashCount++;
                if (m_userTrayCrashCount >= 3) {
                    m_userTrayBackoffUntil = now + 60;
                    Logger::warning(QObject::tr("Agent: Kullanıcı tray süreci üst üste başarısız oldu (%1 kez). 60 saniye beklemeye alınıyor...").arg(m_userTrayCrashCount));
                }
            }
        }

        if (userTrayMissing && !m_hUserTrayProc) {
            if (now < m_userTrayBackoffUntil) {
                return;
            }
            if (now - m_lastUserTrayLaunch >= 10) {
            m_lastUserTrayLaunch = now;

            QString trayPath = QCoreApplication::applicationDirPath() + "/okidia-tray.exe";
            if (!QFile::exists(trayPath)) {
                trayPath = "C:/Program Files/Okidia/Agent/bin/okidia-tray.exe";
            }
            if (!QFile::exists(trayPath)) {
                trayPath = "C:/Program Files/Okidia/okidia-tray.exe";
            }
            if (QFile::exists(trayPath)) {
                Logger::info(QObject::tr("Agent: Kullanıcı oturumunda okidia-tray başlatılıyor (%1)...").arg(newActive));
                void* hProc = nullptr;
                QStringList args;
                if (!m_ipcToken.isEmpty()) {
                    args << "--ipc-token" << m_ipcToken;
                }
                if (SystemInfo::launchProcessInConsoleSession(trayPath, args, &hProc)) {
                    Logger::info(QObject::tr("Agent: Kullanıcı tray süreci başarıyla başlatıldı."));
                    m_hUserTrayProc = hProc;

                    // ── Win32 Sıfır Gecikmeli Tray Bekçisi (Watchdog) ───────────────
                    // Tray süreci Görev Yöneticisi'nden veya dışarıdan sonlandırıldığı milisaniyede
                    // işletim sistemi kernel seviyesinde callback tetikler ve checkSession'ı çağırır.
                    HANDLE hNewWait = NULL;
                    BOOL waitRegistered = RegisterWaitForSingleObject(
                        &hNewWait,
                        reinterpret_cast<HANDLE>(m_hUserTrayProc),
                        [](PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
                            Q_UNUSED(TimerOrWaitFired);
                            auto* app = reinterpret_cast<AgentApp*>(lpParameter);
                            if (app) {
                                Logger::warning(QObject::tr("Agent Servis [Watchdog]: Kullanıcı Tray süreci kapandı/sonlandırıldı! Milisaniyelik kurtarma tetikleniyor..."));
                                QMetaObject::invokeMethod(app, &AgentApp::checkSession, Qt::QueuedConnection);
                            }
                        },
                        this,
                        INFINITE,
                        WT_EXECUTEONLYONCE
                    );
                    if (waitRegistered) {
                        m_hUserTrayWait = hNewWait;
                    }
                } else {
                    Logger::warning(QObject::tr("Agent: Kullanıcı tray süreci başlatılamadı!"));
                }
            }
        }
    }
#else
        if (m_dmTrayProc) {
            Logger::info(QObject::tr("Agent: Kullanıcı girdi, DM tray kapatılıyor..."));
            m_dmTrayProc->terminate();
            m_dmTrayProc->waitForFinished(3000);
            delete m_dmTrayProc;
            m_dmTrayProc = nullptr;
        }

        qint64 now = QDateTime::currentSecsSinceEpoch();
        if (userTrayMissing && (now - m_lastUserTrayLaunch >= 10)) {
            m_lastUserTrayLaunch = now;

            QString trayPath = "/usr/bin/okidia-tray";
            if (!QFile::exists(trayPath)) {
                trayPath = QCoreApplication::applicationDirPath() + "/okidia-tray";
            }

            if (QFile::exists(trayPath)) {
                // Eğer kullanıcı oturumunda asılı kalmış, yetkilendirilememiş veya eski bir okidia-tray süreci varsa,
                // yeni sürecin tekil örnek kilidine (single-instance socket probe) takılmaması için önce temizle.
                QProcess::execute("pkill", {"-u", newActive, "-x", "okidia-tray"});

                Logger::info(QObject::tr("Agent: Kullanıcı oturumunda okidia-tray başlatılıyor (%1)...").arg(newActive));
                QStringList args;
                if (!m_ipcToken.isEmpty()) {
                    args << "--ipc-token" << m_ipcToken;
                }
                if (SystemInfo::launchProcessInUserSession(newActive, trayPath, args)) {
                    Logger::info(QObject::tr("Agent: Kullanıcı tray süreci başarıyla başlatıldı (%1).").arg(newActive));
                } else {
                    Logger::warning(QObject::tr("Agent: Kullanıcı tray süreci başlatılamadı (%1)!").arg(newActive));
                }
            }
        }
#endif
    }
}
