#include "AgentIpcServer.h"
#include "../../common/network/TlsClient.h"
#include "../../common/OkidiaConfig.h"
#include "../../common/utils/Logger.h"
#include "../system/PersistentStateManager.h"
#include "../system/SessionSwitchManager.h"
#include "../OtaUpdater.h"

#include <QDir>
#include <QTimer>
#include <QDataStream>

AgentIpcServer::AgentIpcServer(TlsClient* tls, QObject* parent)
    : QObject(parent), m_tls(tls) {
}

bool AgentIpcServer::start() {
    const QString socketPath = OkidiaConfig::runDir() + "/agent.sock";
    QDir().mkpath(OkidiaConfig::runDir());

    m_localServer = new LocalServer(socketPath, this);
    if (!m_localServer->listen()) {
        Logger::error(QObject::tr("Agent: LocalServer başlatılamadı: ") + socketPath);
        return false;
    }

    connect(m_localServer, &LocalServer::clientConnected, this, [this](LocalConnection* conn) {
        okidia::AgentStatus st;
        st.set_broker_connected(m_tls->isConnected());
        okidia::Envelope stEnv;
        stEnv.set_type(okidia::MessageType::AGENT_STATUS);
        stEnv.set_payload(st.SerializeAsString());
        conn->sendEnvelope(stEnv);
    });

    connect(m_localServer, &LocalServer::clientDisconnected, this, [this](LocalConnection* conn) {
        m_authenticatedClients.remove(conn);
        m_replayedClients.remove(conn);
    });

    connect(m_localServer, &LocalServer::envelopeReceived, this, &AgentIpcServer::handleEnvelopeFromTray);
    return true;
}

void AgentIpcServer::sendToActive(const okidia::Envelope& env) {
    if (m_localServer) {
        m_localServer->sendToActive(env);
    }
}

void AgentIpcServer::broadcast(const okidia::Envelope& env) {
    if (m_localServer) {
        m_localServer->broadcast(env);
    }
}

void AgentIpcServer::clearRemoteControlEnv() {
    m_remoteControlActive = false;
    m_lastTakeControlEnv.Clear();
    m_lastFpsSetEnv.Clear();
    m_lastScreenSelectEnv.Clear();
}

void AgentIpcServer::replayActiveStateToTray(LocalConnection* conn) {
    if (!conn || !conn->isConnected()) return;

    if (PersistentStateManager::isScreenLocked()) {
        uint32_t pol = PersistentStateManager::screenLockPolicy();
        // lock_policy == 3 (Oturum Kapatma) kesinlikle DM tray'e veya yeni oturuma re-play edilmez!
        if (pol != 3) {
            okidia::ScreenLockCommand cmd;
            cmd.set_message(PersistentStateManager::screenLockMessage().toStdString());
            cmd.set_lock_policy(pol);
            okidia::Envelope lockEnv;
            lockEnv.set_type(okidia::MessageType::SCREEN_LOCK);
            lockEnv.set_payload(cmd.SerializeAsString());
            conn->sendEnvelope(lockEnv);
            Logger::info(QObject::tr("Agent: Kalıcı ekran kilidi tray'e re-play edildi (Policy: %1).")
                         .arg(cmd.lock_policy()));
        }
    }
    if (PersistentStateManager::isInputLocked()) {
        okidia::Envelope lockEnv;
        lockEnv.set_type(okidia::MessageType::INPUT_LOCK);
        conn->sendEnvelope(lockEnv);
        Logger::info(QObject::tr("Agent: Kalıcı klavye/fare kilidi tray'e re-play edildi."));
    }

    if (!m_lastScreenSelectEnv.payload().empty()) {
        conn->sendEnvelope(m_lastScreenSelectEnv);
    }
    if (!m_lastFpsSetEnv.payload().empty()) {
        conn->sendEnvelope(m_lastFpsSetEnv);
    }
    if (m_remoteControlActive) {
        Logger::info(QObject::tr("Agent: Uzak kontrol state'i tray'e re-play ediliyor (CLIENT_INFO sonrası)..."));
        conn->sendEnvelope(m_lastTakeControlEnv);
    } else if (!m_lastFpsSetEnv.payload().empty()) {
        Logger::info(QObject::tr("Agent: Aktif ekran akış ayarı (FPS) yeni tray'e re-play edildi."));
    }

    // Anti-Sabotaj: Aktif ekran yayını varsa yeni tray açılır açılmaz anında yayına sok
    if (!m_lastBroadcastBeginEnv.payload().empty()) {
        Logger::info(QObject::tr("Agent (Anti-Sabotaj): Aktif ekran yayını oturumu yeni tray'e re-play edildi."));
        conn->sendEnvelope(m_lastBroadcastBeginEnv);
    }

    // Anti-Sabotaj: Aktif sınav oturumu varsa yeni tray açılır açılmaz anında sınava sok
    if (!m_lastExamStartEnv.payload().empty()) {
        Logger::info(QObject::tr("Agent (Anti-Sabotaj): Aktif sınav oturumu yeni tray'e re-play edildi."));
        conn->sendEnvelope(m_lastExamStartEnv);
    }
}

void AgentIpcServer::handleEnvelopeFromTray(LocalConnection* sender, const okidia::Envelope& env) {
    // ── Kriptografik IPC Token Doğrulaması (Windows/Linux Güvenliği) ─────────
    if (!m_expectedIpcToken.isEmpty()) {
        if (env.type() == okidia::MessageType::AGENT_IPC_HANDSHAKE) {
            QString token = QString::fromUtf8(env.payload().data(), env.payload().size()).trimmed();
            if (token == m_expectedIpcToken) {
                m_authenticatedClients.insert(sender);
                Logger::info(QObject::tr("Agent Servis: Yerel Tray IPC Token doğrulaması başarılı."));
            } else {
                Logger::warning(QObject::tr("Agent Servis: Hatalı IPC Token! Gelen:[%1...] Beklenen:[%2...] Yetkisiz yerel müdahale engellendi.")
                                .arg(token.left(8))
                                .arg(m_expectedIpcToken.left(8)));
                sender->disconnectFromServer();
            }
            return;
        }

        // Token henüz doğrulanmadıysa soketten gelen hiçbir komut veya veri işleme alınmaz
        if (!m_authenticatedClients.contains(sender)) {
            Logger::warning(QObject::tr("Agent Servis: Doğrulanmamış yerel soketten veri alındı (Tip: %1), bağlantı kesiliyor.")
                            .arg(static_cast<int>(env.type())));
            sender->disconnectFromServer();
            return;
        }
    }

    if (env.type() == okidia::MessageType::SCREEN_TILE_UPDATE ||
        env.type() == okidia::MessageType::SCREEN_FRAME ||
        env.type() == okidia::MessageType::REMOTE_FRAME) {
        
        qint64 bufferLimit = m_remoteControlActive ? (2 * 1024 * 1024) : (512 * 1024);
        if (m_tls->isConnected() && m_tls->connection() && 
            m_tls->connection()->bytesToWrite() > bufferLimit) {
            return;
        }
    }

    if (m_tls->isAnonymous()) return;

    if (env.type() == okidia::MessageType::CLIENT_INFO) {
        okidia::ClientInfo ci;
        if (ci.ParseFromString(env.payload())) {
            const QString user = QString::fromStdString(ci.username());
            m_localServer->setUserForConnection(sender, user);
            if (ci.screen_width() > 0 && ci.screen_height() > 0) {
                m_lastKnownScreenW = ci.screen_width();
                m_lastKnownScreenH = ci.screen_height();
            }
            Logger::info(QObject::tr("Agent: Tray kullanıcısı eşlendi → ") + user + 
                         QString(" (%1x%2)").arg(m_lastKnownScreenW).arg(m_lastKnownScreenH));

            if (!m_replayedClients.contains(sender)) {
                m_replayedClients.insert(sender);
                QTimer::singleShot(300, sender, [this, sender]() {
                    replayActiveStateToTray(sender);
                });
            }
        }
    }

    if (env.type() == okidia::MessageType::SCREEN_TILE_UPDATE ||
        env.type() == okidia::MessageType::SCREEN_FRAME ||
        env.type() == okidia::MessageType::REMOTE_FRAME ||
        env.type() == okidia::MessageType::CLIENT_INFO) {

        LocalConnection* active = m_localServer->activeConnection();
        if (active != nullptr && sender != active) {
            return;
        }
    }

    if (env.type() == okidia::MessageType::OTA_UPDATE_INSTALL) {
        QString debPath = QString::fromStdString(env.payload());
        
        QString otaMetaPath = QDir::tempPath() + "/okidia_ota.meta";
        QByteArray signature, fileHash, masterCert;
        QFile metaFile(otaMetaPath);
        if (metaFile.open(QIODevice::ReadOnly)) {
            QDataStream in(&metaFile);
            in.setVersion(QDataStream::Qt_5_15);
            in >> signature >> fileHash >> masterCert;
            metaFile.close();
            QFile::remove(otaMetaPath);
        } else {
            Logger::error(QObject::tr("Agent: OTA güvenlik bilgileri bulunamadı! Güncelleme reddedildi."));
            return;
        }

        OtaUpdater updater;
        updater.installUpdate(debPath, signature, fileHash, masterCert);
        return;
    }

    if (env.type() == okidia::MessageType::SMARTBOARD_UNLOCK_NOTIFY) {
        PersistentStateManager::clearScreenLock();
        PersistentStateManager::clearInputLock();
        Logger::info(QObject::tr("Agent: Akıllı Tahta PIN ile açıldı, yerel kilit durumu temizlendi ve Broker'a iletiliyor."));

        okidia::SmartboardUnlockNotify notify;
        if (notify.ParseFromString(env.payload())) {
            QString username = QString::fromStdString(notify.username());
            QString displayName = QString::fromStdString(notify.display_name());
            QString password = QString::fromStdString(notify.password());
            if (!username.isEmpty()) {
                Logger::info(QObject::tr("Agent (Root): Akıllı Tahta oturum geçişi işletiliyor → Kullanıcı: %1 (%2)")
                             .arg(username).arg(displayName));
                // 1. Sistem kullanıcısını garantiye al (useradd + nopasswdlogin + isteğe bağlı parola)
                SessionSwitchManager::instance().ensureSystemUser(username, displayName, password);
                // 2. Kullanıcı oturumuna geç (farklı oturumu kapat, yeni kullanıcıyı aç)
                SessionSwitchManager::instance().switchToUserSession(username);
            }
        }
    }

    if (m_tls->isConnected() && m_tls->connection())
        m_tls->connection()->sendEnvelope(env);

    if (env.type() == okidia::MessageType::AGENT_TYPE_SET_RESULT) {
        okidia::AgentTypeSetResult res;
        if (res.ParseFromString(env.payload()) && res.approved()) {
            emit agentTypeUpdated(res.requested_type());
            Logger::info(QObject::tr("Agent: Servis tipi güncellendi (Kullanıcı onaylı) → ") + QString::number(res.requested_type()));
        }
    }
}
