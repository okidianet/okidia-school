// master/chat/PrivateChatManager.cpp
#include "PrivateChatManager.h"
#include "../session/SessionManager.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../audit/AuditLog.h"

#include <QDateTime>
#include <QHostInfo>
#include <QUuid>

// Ret bekleme basamakları (dakika)
static const QList<int> kCooldowns = {1, 5, 40};

PrivateChatManager::PrivateChatManager(SessionManager* session, QObject* parent)
    : QObject(parent), m_session(session) {}

// ── Sorgular ──────────────────────────────────────────────────────────────────

QList<PrivateSession*> PrivateChatManager::activeSessions() const {
    return m_sessions.values();
}

QList<PendingRequest> PrivateChatManager::pendingRequests() const {
    return m_pending;
}

PrivateSession* PrivateChatManager::session(const QString& id) const {
    return m_sessions.value(id);
}

QString PrivateChatManager::sessionIdForPeer(const QString& peerId) const {
    for (auto* s : m_sessions) {
        if (s && s->isOpen && s->peerId == peerId) return s->id;
    }
    return QString();
}

void PrivateChatManager::setMasterInfo(const QString& username,
                                       const QString& displayName,
                                       const QString& role) {
    m_masterUsername = username;
    m_masterDisplayName = displayName;
    m_masterRole = role;
}

// ── YM/OM: onaysız başlat ─────────────────────────────────────────────────────

void PrivateChatManager::openDirect(const QString& peerId,
                                     const QString& peerDisplay,
                                     const QString& peerHost,
                                     const QString& peerUser) {
    // Halihazırda açık bir oturum varsa yeniden açmak yerine onu öne çıkar
    for (PrivateSession* existing : m_sessions) {
        if (existing && existing->isOpen && existing->peerId == peerId) {
            emit sessionOpened(existing->id);
            return;
        }
    }

    const QString sessionId = newId();
    auto* s = new PrivateSession();
    s->id          = sessionId;
    s->peerId      = peerId;
    s->peerDisplay = peerDisplay;
    s->peerHost    = peerHost;
    s->peerUser    = peerUser;
    s->isOpen      = true;
    s->startedAt   = OkidiaUtils::nowSecs();

    m_sessions[sessionId] = s;

    // Meta log is handled centrally on the Broker

    // Karşı tarafa bildir
    okidia::PrivateChatOpen proto;
    proto.set_session_id(sessionId.toStdString());
    
    // from_host = Master client ID (ör: master:onurtamer), böylece ajan yanıt verirken hedefe ulaşır
    QString senderHost = m_masterUsername.isEmpty() ? QHostInfo::localHostName() : ("master:" + m_masterUsername);
    proto.set_from_host(senderHost.toStdString());

    // Görünen ad formatlaması (ör: "[Yönetici] Onur Tamer" veya "[Öğretmen] Seda")
    QString fullDisplay = m_masterDisplayName.isEmpty() ? (m_masterUsername.isEmpty() ? tr("Öğretmen") : m_masterUsername) : m_masterDisplayName;
    if (m_masterRole == "YM") {
        if (!fullDisplay.contains(tr("Yönetici"))) {
            fullDisplay = tr("[Yönetici] ") + fullDisplay;
        }
    } else if (m_masterRole == "OBSERVER") {
        if (!fullDisplay.contains(tr("Gözlemci"))) {
            fullDisplay = tr("[Gözlemci] ") + fullDisplay;
        }
    } else if (m_masterRole == "OM") {
        if (!fullDisplay.contains(tr("Öğretmen"))) {
            fullDisplay = tr("[Öğretmen] ") + fullDisplay;
        }
    }
    proto.set_from_display(fullDisplay.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_OPEN);
    env.set_payload(proto.SerializeAsString());
    if (m_session) m_session->sendCommand(peerId, env);

    AuditLog::instance().recordChat({}, peerDisplay, tr("Özel sohbet oturumu başlatıldı"));
    emit sessionOpened(sessionId);
    Logger::info(QObject::tr("PrivateChat: Açıldı (onaysız) → ") + peerDisplay);
}

// ── İstek gönder ─────────────────────────────────────────────────────────────

bool PrivateChatManager::sendRequest(const QString& peerId,
                                      const QString& peerDisplay) {
    const qint64 now = OkidiaUtils::nowSecs();
    if (m_rejectHistory.contains(peerId)) {
        const qint64 endsAt = cooldownEndsAt(peerId);
        if (now < endsAt) {
            Logger::info(QObject::tr("PrivateChat: Bekleme süresi dolmadı → ") + peerDisplay);
            return false;
        }
    }

    const QString reqId = newId();
    PendingRequest req;
    req.requestId   = reqId;
    req.fromId      = m_masterUsername.isEmpty() ? "master" : ("master:" + m_masterUsername);
    req.fromDisplay = m_masterDisplayName.isEmpty() ? "Master" : m_masterDisplayName;
    req.sentAt      = now;
    m_pending.append(req);

    okidia::PrivateChatRequest proto;
    proto.set_request_id(reqId.toStdString());
    
    QString senderHost = m_masterUsername.isEmpty() ? QHostInfo::localHostName() : ("master:" + m_masterUsername);
    proto.set_from_host(senderHost.toStdString());

    QString fullDisplay = m_masterDisplayName.isEmpty() ? (m_masterUsername.isEmpty() ? "Master" : m_masterUsername) : m_masterDisplayName;
    if (m_masterRole == "YM") {
        if (!fullDisplay.contains(tr("Yönetici"))) {
            fullDisplay = tr("[Yönetici] ") + fullDisplay;
        }
    } else if (m_masterRole == "OBSERVER") {
        if (!fullDisplay.contains(tr("Gözlemci"))) {
            fullDisplay = tr("[Gözlemci] ") + fullDisplay;
        }
    } else if (m_masterRole == "OM") {
        if (!fullDisplay.contains(tr("Öğretmen"))) {
            fullDisplay = tr("[Öğretmen] ") + fullDisplay;
        }
    }
    proto.set_from_display(fullDisplay.toStdString());
    proto.set_from_user(m_masterUsername.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_REQUEST);
    env.set_payload(proto.SerializeAsString());
    if (m_session) m_session->sendCommand(peerId, env);
    return true;
}

// ── İstek yanıtı ─────────────────────────────────────────────────────────────

void PrivateChatManager::acceptRequest(const QString& requestId) {
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].requestId == requestId) {
            const PendingRequest req = m_pending.takeAt(i);
            // Aynı öğrenciden gelen diğer bekleyen istekleri de temizle (tekrarlı onay penceresi açılmasın)
            for (int j = m_pending.size() - 1; j >= 0; --j) {
                if (m_pending[j].fromId == req.fromId) {
                    m_pending.removeAt(j);
                }
            }
            openDirect(req.fromId, req.fromDisplay, req.fromHost, req.fromUser);
            // Kabul bildirimi
            okidia::PrivateChatAccept proto;
            proto.set_request_id(requestId.toStdString());
            okidia::Envelope env;
            env.set_type(okidia::MessageType::PRIVATE_CHAT_ACCEPT);
            env.set_payload(proto.SerializeAsString());
            if (m_session) m_session->sendCommand(req.fromId, env);
            return;
        }
    }
}

void PrivateChatManager::rejectRequest(const QString& requestId) {
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].requestId == requestId) {
            const PendingRequest req = m_pending.takeAt(i);
            for (int j = m_pending.size() - 1; j >= 0; --j) {
                if (m_pending[j].fromId == req.fromId) {
                    m_pending.removeAt(j);
                }
            }
            // Ret geçmişi güncelle
            auto& rec = m_rejectHistory[req.fromId];
            rec.count++;
            rec.lastAt = OkidiaUtils::nowSecs();
            const int mins = cooldownMinutes(req.fromId);
            emit cooldownChanged(req.fromId, rec.lastAt + mins * 60);

            okidia::PrivateChatReject proto;
            proto.set_request_id(requestId.toStdString());
            proto.set_cooldown_seconds(mins * 60);
            okidia::Envelope env;
            env.set_type(okidia::MessageType::PRIVATE_CHAT_REJECT);
            env.set_payload(proto.SerializeAsString());
            if (m_session) m_session->sendCommand(req.fromId, env);

            AuditLog::instance().recordChat({}, req.fromDisplay, tr("Özel sohbet isteği reddedildi"));
            return;
        }
    }
}

void PrivateChatManager::ignoreRequest(const QString& requestId) {
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].requestId == requestId) {
            const PendingRequest req = m_pending.takeAt(i);
            for (int j = m_pending.size() - 1; j >= 0; --j) {
                if (m_pending[j].fromId == req.fromId) {
                    m_pending.removeAt(j);
                }
            }
            QString effUser = req.fromUser;
            if (effUser.isEmpty() && m_session) {
                effUser = m_session->clientState(req.fromId).username;
            }
            ignoreUser(req.fromId, effUser);

            okidia::PrivateChatReject proto;
            proto.set_request_id(requestId.toStdString());
            proto.set_cooldown_seconds(3600);
            okidia::Envelope env;
            env.set_type(okidia::MessageType::PRIVATE_CHAT_REJECT);
            env.set_payload(proto.SerializeAsString());
            if (m_session) m_session->sendCommand(req.fromId, env);

            AuditLog::instance().recordChat({}, req.fromDisplay, tr("Özel sohbet istekleri yoksayıldı"));
            return;
        }
    }
}

QString PrivateChatManager::makeUserKey(const QString& clientId, const QString& username) {
    const QString normUser = username.trimmed().toLower();
    return clientId + ":" + (normUser.isEmpty() ? QStringLiteral("__unknown__") : normUser);
}

void PrivateChatManager::ignoreUser(const QString& clientId, const QString& username) {
    if (clientId.isEmpty()) return;
    const QString key = makeUserKey(clientId, username);
    if (!m_ignoredUsers.contains(key)) {
        m_ignoredUsers.insert(key);
        Logger::info(QObject::tr("PrivateChat: Kullanıcı istekleri yoksayıldı → ") + key);
        emit ignoredUsersChanged();
    }
}

void PrivateChatManager::unignoreUser(const QString& clientId, const QString& username) {
    if (clientId.isEmpty()) return;
    const QString key = makeUserKey(clientId, username);
    if (m_ignoredUsers.remove(key)) {
        Logger::info(QObject::tr("PrivateChat: Kullanıcı yoksayma kaldırıldı → ") + key);
        emit ignoredUsersChanged();
    }
}

bool PrivateChatManager::isUserIgnored(const QString& clientId, const QString& username) const {
    if (clientId.isEmpty()) return false;
    return m_ignoredUsers.contains(makeUserKey(clientId, username));
}

// ── Mesaj gönder ──────────────────────────────────────────────────────────────

void PrivateChatManager::sendMessage(const QString& sessionId,
                                      const QString& content) {
    PrivateSession* s = m_sessions.value(sessionId);
    if (!s || !s->isOpen) return;

    PrivateMessage msg;
    msg.id        = newId();
    msg.content   = content;
    msg.timestamp = OkidiaUtils::nowSecs();
    msg.fromMe    = true;
    s->messages.append(msg);
    emit messageReceived(sessionId, msg);

    okidia::PrivateChatMessage proto;
    proto.set_session_id(sessionId.toStdString());
    proto.set_content(content.toStdString());
    proto.set_timestamp(static_cast<uint64_t>(msg.timestamp));
    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_MESSAGE);
    env.set_payload(proto.SerializeAsString());
    if (m_session) m_session->sendCommand(s->peerId, env);
}

void PrivateChatManager::appendSystemMessage(const QString& sessionId, const QString& content) {
    PrivateSession* s = m_sessions.value(sessionId);
    if (!s) return;

    PrivateMessage msg;
    msg.id        = newId();
    msg.content   = content;
    msg.timestamp = OkidiaUtils::nowSecs();
    msg.fromMe    = false;
    s->messages.append(msg);
    emit messageReceived(sessionId, msg);
}

// ── Oturumu kapat ─────────────────────────────────────────────────────────────

void PrivateChatManager::closeSession(const QString& sessionId) {
    PrivateSession* s = m_sessions.value(sessionId);
    if (!s) return;
    s->isOpen = false;

    // Meta log is handled centrally on the Broker

    okidia::PrivateChatClose proto;
    proto.set_session_id(sessionId.toStdString());
    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_CLOSE);
    env.set_payload(proto.SerializeAsString());
    if (m_session) m_session->sendCommand(s->peerId, env);

    emit sessionClosed(sessionId);
    m_sessions.remove(sessionId);
    delete s;
}

// ── Gelen proto ───────────────────────────────────────────────────────────────

void PrivateChatManager::onIncomingRequest(const QString& fromId,
                                            const QString& fromDisplay,
                                            const QString& fromHost,
                                            const QString& fromUser,
                                            const QString& requestId) {
    QString effUser = fromUser;
    if (effUser.isEmpty() && m_session) {
        effUser = m_session->clientState(fromId).username;
    }

    // 1. Kullanıcı yoksayılmışsa sessizce engelle
    if (isUserIgnored(fromId, effUser)) {
        Logger::info(QObject::tr("PrivateChat: Yoksayılan kullanıcıdan gelen istek engellendi → ") + fromId);
        return;
    }

    // 2. Zaten açık bir özel sohbet oturumu varsa tekrar onay sorma, mevcut sekmeyi öne çıkar
    const QString existingSessionId = sessionIdForPeer(fromId);
    if (!existingSessionId.isEmpty()) {
        emit sessionOpened(existingSessionId);
        return;
    }

    // 3. Aynı kullanıcıdan halihazırda bekleyen bir istek varsa mükerrer diyalog açma
    for (const PendingRequest& p : m_pending) {
        if (p.fromId == fromId) {
            return;
        }
    }

    // 4. Ret bekleme süresi dolmadıysa sessizce engelle
    const qint64 now = OkidiaUtils::nowSecs();
    if (m_rejectHistory.contains(fromId) && now < cooldownEndsAt(fromId)) {
        return;
    }

    PendingRequest req;
    req.requestId   = requestId;
    req.fromId      = fromId;
    req.fromDisplay = fromDisplay;
    req.fromHost    = fromHost;
    req.fromUser    = effUser;
    req.sentAt      = now;
    m_pending.append(req);
    emit requestReceived(req);
}

void PrivateChatManager::onIncomingOpen(const QString& sessionId, const QString& fromId,
                                        const QString& fromDisplay, const QString& fromHost) {
    if (m_sessions.contains(sessionId)) return;
    
    auto* s = new PrivateSession();
    s->id          = sessionId;
    s->peerId      = fromId;
    s->peerDisplay = fromDisplay;
    s->peerHost    = fromHost;
    s->isOpen      = true;
    s->isMaster    = true; // OPEN direkt master'dan gelir
    s->startedAt   = OkidiaUtils::nowSecs();
    
    m_sessions[sessionId] = s;
    emit sessionOpened(sessionId);
}

void PrivateChatManager::onRequestAccepted(const QString& requestId,
                                            const QString& sessionId) {
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].requestId == requestId) {
            const PendingRequest req = m_pending.takeAt(i);
            auto* s = new PrivateSession();
            s->id          = sessionId;
            s->peerId      = req.fromId;
            s->peerDisplay = req.fromDisplay;
            s->peerHost    = req.fromHost;
            s->isOpen      = true;
            s->startedAt   = OkidiaUtils::nowSecs();
            m_sessions[sessionId] = s;
            emit sessionOpened(sessionId);
            return;
        }
    }
}

void PrivateChatManager::onRequestRejected(const QString& requestId) {
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].requestId == requestId) {
            m_pending.removeAt(i);
            emit requestExpired(requestId);
            return;
        }
    }
}

void PrivateChatManager::onIncomingMessage(const QString& sessionId,
                                            const QString& content,
                                            qint64 timestamp) {
    PrivateSession* s = m_sessions.value(sessionId);
    if (!s) return;
    PrivateMessage msg;
    msg.id        = newId();
    msg.content   = content;
    msg.timestamp = timestamp;
    msg.fromMe    = false;
    s->messages.append(msg);
    emit messageReceived(sessionId, msg);
}

void PrivateChatManager::onSessionClosed(const QString& sessionId) {
    if (!m_sessions.contains(sessionId)) return;
    PrivateSession* s = m_sessions.take(sessionId);
    // Meta log is handled centrally on the Broker
    emit sessionClosed(sessionId);
    delete s;
}

// ── Bekleme süresi ────────────────────────────────────────────────────────────

int PrivateChatManager::cooldownMinutes(const QString& peerId) const {
    if (!m_rejectHistory.contains(peerId)) return 0;
    const int idx = qMin(m_rejectHistory[peerId].count - 1,
                         kCooldowns.size() - 1);
    return kCooldowns[idx];
}

qint64 PrivateChatManager::cooldownEndsAt(const QString& peerId) const {
    if (!m_rejectHistory.contains(peerId)) return 0;
    const auto& rec = m_rejectHistory[peerId];
    return rec.lastAt + cooldownMinutes(peerId) * 60LL;
}

QString PrivateChatManager::newId() const {
    return OkidiaUtils::generateUuid();
}
