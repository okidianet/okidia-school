// agent/chat/ChatHandler.cpp
#include "ChatHandler.h"
#include "../ui/AgentChatWindow/AgentChatWindow.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/crypto/KeyManager.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <openssl/evp.h>
#include <openssl/rand.h>


ChatHandler::ChatHandler(QObject* parent)
    : QObject(parent) {}

ChatHandler::~ChatHandler() {
    for (EVP_PKEY* pkey : m_pendingEcdhKeys) {
        if (pkey) EVP_PKEY_free(pkey);
    }
    m_pendingEcdhKeys.clear();
}

void ChatHandler::setMyInfo(const QString& clientId,
                              const QString& hostname,
                              const QString& username) {
    m_myClientId = clientId;
    m_myHostname = hostname;
    m_myUsername = username;
    m_myDisplay  = hostname + (username.isEmpty() ? "" : " · " + username);
}

void ChatHandler::updateChatAvailability() {
    if (m_cachedRooms.isEmpty()) {
        emit chatAvailabilityChanged(false);
        return;
    }
    bool anyUnlocked = false;
    for (auto it = m_classLockStates.constBegin(); it != m_classLockStates.constEnd(); ++it) {
        if (!it.value().first) {
            anyUnlocked = true;
            break;
        }
    }
    emit chatAvailabilityChanged(anyUnlocked || m_classLockStates.isEmpty());
}

void ChatHandler::showChatWindow(bool allowEmpty) {
    if (!allowEmpty && m_cachedRooms.isEmpty()) {
        Logger::info(QObject::tr("ChatHandler: Herhangi bir sınıfa/odaya dahil olunmadığı için sohbet penceresi açılmadı."));
        return;
    }

    if (!m_window) {
        m_window = new AgentChatWindow(m_myClientId, m_myDisplay);
        connect(m_window, &AgentChatWindow::messageSent,
                this, &ChatHandler::onMessageSent);
        connect(m_window, &AgentChatWindow::privateMessageSent,
                this, &ChatHandler::onPrivateMessageSent);
        connect(m_window, &AgentChatWindow::privateRequested,
                this, &ChatHandler::onPrivateRequested);
        connect(m_window, &AgentChatWindow::privateAccepted,
                this, &ChatHandler::onPrivateAccepted);
        connect(m_window, &AgentChatWindow::privateRejected,
                this, &ChatHandler::onPrivateRejected);
        connect(m_window, &AgentChatWindow::privateSessionCloseRequested,
                this, &ChatHandler::onPrivateClosedByUser);
        connect(m_window, &AgentChatWindow::handRaiseRequested,
                this, &ChatHandler::onHandRaiseRequested);
        connect(m_window, &AgentChatWindow::windowClosed,
                this, &ChatHandler::onWindowClosed);

        // Pencere açılmadan önce gelen odaları yükle
        for (const auto& cached : m_cachedRooms) {
            m_window->addRoom(cached.info);
            for (const AgentMemberInfo& member : cached.members)
                m_window->addMember(cached.info.id, member);
        }

        // Önbellekteki kilit durumlarını uygula
        for (auto it = m_classLockStates.constBegin(); it != m_classLockStates.constEnd(); ++it) {
            for (auto rIt = m_cachedRooms.constBegin(); rIt != m_cachedRooms.constEnd(); ++rIt) {
                m_window->setChatLocked(rIt.key(), it.value().first);
            }
            m_window->setPrivateLocked(it.value().second);
        }
    }
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
    if (!m_windowOpen) {
        m_windowOpen = true;
        sendWindowState(true);
    }
}

// ── Broker'dan gelen mesajlar ─────────────────────────────────────────────────

void ChatHandler::handleEnvelope(const okidia::Envelope& env) {
    switch (env.type()) {
    case okidia::MessageType::CHAT_ROOM_CREATED:
        handleRoomCreated(env); break;
    case okidia::MessageType::CHAT_MESSAGE:
        handleChatMessage(env); break;
    case okidia::MessageType::CHAT_MEMBER_JOINED:
        handleMemberJoined(env); break;
    case okidia::MessageType::CHAT_MEMBER_LEFT:
        handleMemberLeft(env); break;
    case okidia::MessageType::PRIVATE_CHAT_REQUEST:
        handlePrivateRequest(env); break;
    case okidia::MessageType::PRIVATE_CHAT_ACCEPT:
        handlePrivateAccept(env); break;
    case okidia::MessageType::PRIVATE_CHAT_REJECT:
        handlePrivateReject(env); break;
    case okidia::MessageType::PRIVATE_CHAT_OPEN:
        handlePrivateOpen(env); break;
    case okidia::MessageType::PRIVATE_CHAT_MESSAGE:
        handlePrivateMessage(env); break;
    case okidia::MessageType::PRIVATE_CHAT_CLOSE:
        handlePrivateClose(env); break;
    case okidia::MessageType::CHAT_LOCK_STATE:
        handleChatLockState(env); break;
    case okidia::MessageType::CHAT_WINDOW_STATE:
        handleChatWindowState(env); break;
    default: break;
    }
}

void ChatHandler::handleRoomCreated(const okidia::Envelope& env) {
    okidia::ChatRoomCreated msg;
    if (!msg.ParseFromString(env.payload())) return;

    AgentRoomInfo room;
    room.id           = QString::fromStdString(msg.room_id());
    room.name         = QString::fromStdString(msg.room_name());
    room.parentRoomId = QString::fromStdString(msg.parent_room_id());

    // Her zaman önbelleğe al
    CachedRoom& cached = m_cachedRooms[room.id];
    cached.info = room;

    // Mevcut önbellekteki online ve windowOpen durumlarını koru!
    QMap<QString, bool> prevOnline;
    QMap<QString, bool> prevWindow;
    for (const auto& m : cached.members) {
        prevOnline[m.clientId] = m.online;
        prevWindow[m.clientId] = m.windowOpen;
    }

    cached.members.clear();
    for (const auto& m : msg.members()) {
        AgentMemberInfo member;
        member.clientId    = QString::fromStdString(m.client_id());
        member.displayName = QString::fromStdString(m.hostname());
        if (m.username() == "YM") member.role = 1;
        else if (m.username() == "OM") member.role = 2;
        else member.role = 3;
        const bool isSelf = (member.clientId == m_myClientId);
        bool isOnline = isSelf ? true : (m.online() || prevOnline.value(member.clientId, false));
        bool isWindowOpen = isSelf ? (m_window && m_window->isVisible()) : (m.window_open() || prevWindow.value(member.clientId, false));
        if (member.role == 1 || member.role == 2) {
            if (isOnline) isWindowOpen = true;
        }
        member.online      = isOnline;
        member.windowOpen  = isWindowOpen;
        cached.members.append(member);
    }

    // Pencere açıksa direkt ekle
    if (m_window) {
        m_window->addRoom(room);
        for (const AgentMemberInfo& member : cached.members)
            m_window->addMember(room.id, member);
    }
    updateChatAvailability();
    Logger::info(QObject::tr("ChatHandler: Oda alındı → ") + room.name);
}

void ChatHandler::handleChatMessage(const okidia::Envelope& env) {
    okidia::ChatMessage msg;
    if (!msg.ParseFromString(env.payload())) return;

    const QString roomId = QString::fromStdString(msg.room_id());
    const QString from   = QString::fromStdString(msg.from());
    const QString content= QString::fromStdString(msg.content());
    const qint64  ts     = msg.timestamp() > 0
        ? static_cast<qint64>(msg.timestamp())
        : OkidiaUtils::nowSecs();

    AgentMessage agMsg;
    agMsg.fromDisplay = from.isEmpty() ? "?" : from;
    agMsg.content     = content;
    agMsg.timestamp   = ts;
    agMsg.fromMe      = (from == m_myDisplay);
    agMsg.isSystem    = (msg.msg_type() == okidia::CHAT_MSG_SYSTEM);
    agMsg.senderRole  = static_cast<int>(msg.sender_role());

    if (m_window) {
        m_window->appendMessage(roomId, agMsg);
    }
}

void ChatHandler::handleMemberJoined(const okidia::Envelope& env) {
    okidia::ChatMemberEvent msg;
    if (!msg.ParseFromString(env.payload())) return;
    const QString roomId   = QString::fromStdString(msg.room_id());
    const QString clientId = QString::fromStdString(msg.client_id());
    AgentMemberInfo member;
    member.clientId    = clientId;
    member.displayName = QString::fromStdString(msg.hostname());
    if (msg.username() == "YM") member.role = 1;
    else if (msg.username() == "OM") member.role = 2;
    else member.role = 3;
    member.online      = true;
    member.windowOpen  = true;

    // Önbelleği güncelle
    if (m_cachedRooms.contains(roomId)) {
        auto& cached = m_cachedRooms[roomId];
        bool found = false;
        for (auto& m : cached.members) {
            if (m.clientId == clientId) {
                m.displayName = member.displayName.isEmpty() ? m.displayName : member.displayName;
                m.role = member.role;
                m.online = true;
                m.windowOpen = true;
                found = true;
                break;
            }
        }
        if (!found) cached.members.append(member);
    }

    if (m_window) m_window->addMember(roomId, member);
}

void ChatHandler::handleMemberLeft(const okidia::Envelope& env) {
    okidia::ChatMemberEvent msg;
    if (!msg.ParseFromString(env.payload())) return;
    const QString roomId   = QString::fromStdString(msg.room_id());
    const QString clientId = QString::fromStdString(msg.client_id());

    if (clientId == m_myClientId || clientId == "__room_closed__") {
        m_cachedRooms.remove(roomId);
        if (m_window) {
            m_window->removeRoom(roomId);
        }
        updateChatAvailability();
        return;
    }

    // Önbelleği güncelle (silmek yerine offline yap)
    if (m_cachedRooms.contains(roomId)) {
        auto& cached = m_cachedRooms[roomId];
        for (auto& m : cached.members) {
            if (m.clientId == clientId) {
                m.online = false;
                m.windowOpen = false;
                break;
            }
        }
    }

    if (m_window) m_window->setMemberOnline(roomId, clientId, false);
}

void ChatHandler::handlePrivateRequest(const okidia::Envelope& env) {
    okidia::PrivateChatRequest msg;
    if (!msg.ParseFromString(env.payload())) return;
    const QString reqId   = QString::fromStdString(msg.request_id());
    const QString display = QString::fromStdString(msg.from_display());
    
    // E2EE public key saptandıysa önbelleğe kaydet
    if (!msg.ecdh_public_key().empty()) {
        QByteArray peerPubKey = QByteArray::fromBase64(QByteArray::fromStdString(msg.ecdh_public_key()));
        m_pendingPeerKeys[reqId] = peerPubKey;
        Logger::info(QObject::tr("ChatHandler: E2EE peer public key stored for request ") + reqId);
    }

    if (m_window) m_window->showPrivateRequest(reqId, display);
    // Tray'e bildir (main_tray.cpp bunu halleder)
}

void ChatHandler::handlePrivateAccept(const okidia::Envelope& env) {
    okidia::PrivateChatAccept msg;
    if (!msg.ParseFromString(env.payload())) return;

    const QString reqId = QString::fromStdString(msg.request_id());
    const QString sessionId = QString::fromStdString(msg.session_id());

    // E2EE anahtar türetme işlemi tetiklenmiş mi?
    if (m_pendingEcdhKeys.contains(reqId) && !msg.ecdh_public_key().empty()) {
        EVP_PKEY* myPrivKey = m_pendingEcdhKeys.take(reqId);
        QByteArray peerPubKeyBytes = QByteArray::fromBase64(QByteArray::fromStdString(msg.ecdh_public_key()));

        QByteArray sharedKey = KeyManager::deriveSharedSecret(myPrivKey, peerPubKeyBytes);
        if (!sharedKey.isEmpty()) {
            m_sessionKeys[sessionId] = sharedKey;
            Logger::info(QObject::tr("ChatHandler: E2EE Session Key derived for initiator on session ") + sessionId);
        }
        EVP_PKEY_free(myPrivKey);
    } else {
        if (m_pendingEcdhKeys.contains(reqId)) {
            EVP_PKEY* myPrivKey = m_pendingEcdhKeys.take(reqId);
            EVP_PKEY_free(myPrivKey);
        }
    }
}

void ChatHandler::handlePrivateReject(const okidia::Envelope& env) {
    okidia::PrivateChatReject msg;
    if (!msg.ParseFromString(env.payload())) return;
    // Ret bildirimi — cooldown ui tarafında gösterilir
}

void ChatHandler::handlePrivateOpen(const okidia::Envelope& env) {
    okidia::PrivateChatOpen msg;
    if (!msg.ParseFromString(env.payload())) return;
    const QString sessionId  = QString::fromStdString(msg.session_id());
    const QString fromHost   = QString::fromStdString(msg.from_host());
    const QString fromDisplay= QString::fromStdString(msg.from_display());

    // Kim gönderdi → peer olarak kaydet (from_host = machineId veya hostname)
    // Broker'ın ilettiği env'de sender bilgisi target_id'den çıkarılamaz,
    // master clientId'yi session ile eşliyor — from_host'u peer olarak kullan
    m_sessionToPeer[sessionId] = fromHost;

    showChatWindow(true);
    if (m_window)
        m_window->openPrivateTab(sessionId, fromDisplay);
}

void ChatHandler::handlePrivateMessage(const okidia::Envelope& env) {
    okidia::PrivateChatMessage msg;
    if (!msg.ParseFromString(env.payload())) return;
    
    showChatWindow(true);
    if (!m_window) return;

    QString sessionId = QString::fromStdString(msg.session_id());
    QString rawContent = QString::fromStdString(msg.content());
    QString decryptedContent = rawContent;
    bool isDecrypted = false;

    // Mesajın E2EE JSON formatında olup olmadığını denetle
    if (rawContent.startsWith("{\"e2ee\":")) {
        QJsonDocument doc = QJsonDocument::fromJson(rawContent.toUtf8());
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject json = doc.object();
            if (json.value("e2ee").toBool() && m_sessionKeys.contains(sessionId)) {
                QByteArray key = m_sessionKeys.value(sessionId);
                QByteArray iv = QByteArray::fromBase64(json.value("iv").toString().toLatin1());
                QByteArray tag = QByteArray::fromBase64(json.value("tag").toString().toLatin1());
                QByteArray ciphertext = QByteArray::fromBase64(json.value("ciphertext").toString().toLatin1());

                QByteArray decryptedBytes;
                if (KeyManager::decryptAesGcm(ciphertext, key, iv, tag, decryptedBytes)) {
                    decryptedContent = QString::fromUtf8(decryptedBytes);
                    isDecrypted = true;
                } else {
                    decryptedContent = tr("[HATA: Uçtan Uca Şifreli Mesaj Çözülemedi!]");
                    Logger::error(QObject::tr("ChatHandler: E2EE GCM Decryption failed"));
                }
            }
        }
    }

    QString displayContent = decryptedContent;
    if (isDecrypted) {
        displayContent = "🔒 " + decryptedContent;
    }

    m_window->appendPrivateMsg(
        sessionId,
        displayContent,
        false,
        static_cast<qint64>(msg.timestamp()));

    if (!m_window->isVisible() || m_window->isMinimized()) {
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
    }
}

void ChatHandler::handlePrivateClose(const okidia::Envelope& env) {
    if (!m_window) return;
    okidia::PrivateChatClose msg;
    if (!msg.ParseFromString(env.payload())) return;
    const QString sessionId = QString::fromStdString(msg.session_id());
    m_sessionToPeer.remove(sessionId);
    m_sessionKeys.remove(sessionId); // Oturum anahtarını temizle
    m_window->closePrivateTab(sessionId);
}

// ── Pencere sinyalleri ────────────────────────────────────────────────────────

void ChatHandler::onMessageSent(const QString& roomId, const QString& content) {
    if (!m_conn) return;
    okidia::ChatMessage proto;
    proto.set_room_id(roomId.toStdString());
    proto.set_from(m_myDisplay.toStdString());
    proto.set_content(content.toStdString());
    proto.set_timestamp(OkidiaUtils::nowSecsU64());
    proto.set_sender_role(okidia::SenderRole::ROLE_CLIENT);
    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_MESSAGE);
    env.set_payload(proto.SerializeAsString());
    m_conn->sendEnvelope(env);

    // Kendi mesajımızı da penceреye ekle
    if (m_window) {
        AgentMessage msg;
        msg.fromDisplay = m_myDisplay;
        msg.content     = content;
        msg.timestamp   = OkidiaUtils::nowSecs();
        msg.fromMe      = true;
        m_window->appendMessage(roomId, msg);
    }
}

void ChatHandler::onPrivateMessageSent(const QString& sessionId,
                                        const QString& content) {
    if (!m_conn) return;

    QString finalContent = content;
    bool isEncrypted = false;

    // E2EE aktif mi?
    if (m_sessionKeys.contains(sessionId)) {
        QByteArray key = m_sessionKeys.value(sessionId);
        
        // Generate random 12-byte IV
        QByteArray iv(12, 0);
        RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), 12);

        QByteArray ciphertext, tag;
        if (KeyManager::encryptAesGcm(content.toUtf8(), key, iv, ciphertext, tag)) {
            QJsonObject json;
            json["e2ee"] = true;
            json["iv"] = QString::fromLatin1(iv.toBase64());
            json["tag"] = QString::fromLatin1(tag.toBase64());
            json["ciphertext"] = QString::fromLatin1(ciphertext.toBase64());
            
            QJsonDocument doc(json);
            finalContent = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
            isEncrypted = true;
            Logger::info(QObject::tr("ChatHandler: Message encrypted successfully via AES-256-GCM"));
        }
    }

    okidia::PrivateChatMessage proto;
    proto.set_session_id(sessionId.toStdString());
    proto.set_content(finalContent.toStdString());
    proto.set_timestamp(OkidiaUtils::nowSecsU64());
    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_MESSAGE);
    env.set_payload(proto.SerializeAsString());
    const QString peerId = m_sessionToPeer.value(sessionId);
    if (!peerId.isEmpty()) env.set_target_id(peerId.toStdString());
    m_conn->sendEnvelope(env);

    if (m_window) {
        QString displayMsg = content;
        if (isEncrypted) displayMsg = "🔒 " + content;
        m_window->appendPrivateMsg(sessionId, displayMsg, true,
                                    OkidiaUtils::nowSecs());
    }
}

void ChatHandler::onPrivateRequested(const QString& peerId,
                                      const QString& /*peerDisplay*/) {
    if (!m_conn) return;

    QString reqId = OkidiaUtils::generateUuid();

    // Ephemeral X25519 keypair üret
    EVP_PKEY* myPrivKey = nullptr;
    QByteArray myPubKeyBytes;
    if (KeyManager::generateEcdhKeyPair(&myPrivKey, myPubKeyBytes)) {
        m_pendingEcdhKeys[reqId] = myPrivKey;
        Logger::info(QObject::tr("ChatHandler: E2EE X25519 ephemeral keypair generated for request ") + reqId);
    }

    okidia::PrivateChatRequest proto;
    proto.set_request_id(reqId.toStdString());
    proto.set_from_host(m_myHostname.toStdString());
    proto.set_from_display(m_myDisplay.toStdString());
    proto.set_from_user(m_myUsername.toStdString());
    if (!myPubKeyBytes.isEmpty()) {
        proto.set_ecdh_public_key(myPubKeyBytes.toBase64().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_REQUEST);
    env.set_target_id(peerId.toStdString());
    env.set_payload(proto.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void ChatHandler::onPrivateAccepted(const QString& requestId) {
    if (!m_conn) return;

    QString sessionId = OkidiaUtils::generateUuid();
    QByteArray myPubKeyBytes;

    // Ephemeral B keypair üret ve shared key'i türet
    if (m_pendingPeerKeys.contains(requestId)) {
        QByteArray peerPubKeyBytes = m_pendingPeerKeys.take(requestId);
        EVP_PKEY* myPrivKey = nullptr;

        if (KeyManager::generateEcdhKeyPair(&myPrivKey, myPubKeyBytes)) {
            QByteArray sharedKey = KeyManager::deriveSharedSecret(myPrivKey, peerPubKeyBytes);
            if (!sharedKey.isEmpty()) {
                m_sessionKeys[sessionId] = sharedKey;
                Logger::info(QObject::tr("ChatHandler: E2EE Session Key derived for acceptor on session ") + sessionId);
            }
            EVP_PKEY_free(myPrivKey);
        }
    }

    okidia::PrivateChatAccept proto;
    proto.set_request_id(requestId.toStdString());
    proto.set_session_id(sessionId.toStdString());
    if (!myPubKeyBytes.isEmpty()) {
        proto.set_ecdh_public_key(myPubKeyBytes.toBase64().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_ACCEPT);
    env.set_payload(proto.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void ChatHandler::onPrivateRejected(const QString& requestId) {
    m_pendingPeerKeys.remove(requestId);
    if (!m_conn) return;
    okidia::PrivateChatReject proto;
    proto.set_request_id(requestId.toStdString());
    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_REJECT);
    env.set_payload(proto.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void ChatHandler::onHandRaiseRequested(const QString& note) {
    if (!m_conn || !m_conn->isConnected()) {
        Logger::info(QObject::tr("ChatHandler: El kaldırma gönderilemedi — servis bağlantısı yok"));
        return;
    }
    okidia::HandRaiseEvent evt;
    evt.set_message(note.toStdString());
    evt.set_timestamp(OkidiaUtils::nowSecsU64());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::HAND_RAISE);
    env.set_payload(evt.SerializeAsString());

    m_conn->sendEnvelope(env);
    Logger::info(QObject::tr("ChatHandler: Yardım talebi gönderildi → ") + note);
    emit handRaiseSent(note);
}

void ChatHandler::onWindowClosed() {
    m_windowOpen = false;
    sendWindowState(false);
}

void ChatHandler::onPrivateClosedByUser(const QString& sessionId) {
    // Kullanıcı sekmeyi kapattı — karşı tarafa kapat bildirimi gönder ve anahtarları sil
    m_sessionToPeer.remove(sessionId);
    m_sessionKeys.remove(sessionId);
    if (!m_conn) return;
    okidia::PrivateChatClose proto;
    proto.set_session_id(sessionId.toStdString());
    okidia::Envelope env;
    env.set_type(okidia::MessageType::PRIVATE_CHAT_CLOSE);
    env.set_payload(proto.SerializeAsString());
    m_conn->sendEnvelope(env);
}


void ChatHandler::sendWindowState(bool open) {
    if (!m_conn) return;
    okidia::ChatWindowState proto;
    proto.set_open(open);
    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_WINDOW_STATE);
    env.set_payload(proto.SerializeAsString());
    m_conn->sendEnvelope(env);
    Logger::info(QObject::tr("ChatHandler: Pencere durumu → ") +
                 (open ? tr("açık") : tr("kapalı")));
}

void ChatHandler::handleChatLockState(const okidia::Envelope& env) {
    okidia::ChatLockState msg;
    if (!msg.ParseFromString(env.payload())) return;

    const QString classId    = QString::fromStdString(msg.class_id());
    const bool chatLocked    = msg.chat_locked();
    const bool privateLocked = msg.private_locked();

    m_classLockStates[classId] = {chatLocked, privateLocked};

    if (m_window) {
        // Önbellekteki oda id'leri içinde bu classId geçiyorsa kilitle
        for (auto it = m_cachedRooms.constBegin(); it != m_cachedRooms.constEnd(); ++it)
            m_window->setChatLocked(it.key(), chatLocked);
        m_window->setPrivateLocked(privateLocked);
    }

    updateChatAvailability();
}

void ChatHandler::handleChatWindowState(const okidia::Envelope& env) {
    okidia::ChatWindowState msg;
    if (!msg.ParseFromString(env.payload())) return;
    const QString senderId = QString::fromStdString(env.target_id());
    if (senderId.isEmpty()) return;

    for (auto it = m_cachedRooms.begin(); it != m_cachedRooms.end(); ++it) {
        for (auto& m : it.value().members) {
            if (m.clientId == senderId) {
                m.windowOpen = msg.open();
                if (msg.open()) m.online = true;
                if (m_window) {
                    m_window->setMemberWindowState(it.key(), senderId, msg.open());
                }
                break;
            }
        }
    }
}
