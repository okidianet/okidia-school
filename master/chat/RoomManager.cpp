// master/chat/RoomManager.cpp
#include "RoomManager.h"
#include "../tree/TreeManager.h"
#include "../session/SessionManager.h"
#include "../audit/AuditLog.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QDateTime>
#include <QHostInfo>
#include <QUuid>
#include <QMessageBox>

const char RoomManager::kAdminRoomId[] = "room_admin";

// ── Kurucu ────────────────────────────────────────────────────────────────────

RoomManager::RoomManager(TreeManager* tree, SessionManager* session, QObject* parent)
    : QObject(parent), m_tree(tree), m_session(session)
{
    // Yönetici Odası — kalıcı, tree'ye bağlı değil
    createRoom(kAdminRoomId, tr("Öğretmenler Odası"), RoomType::AdminRoom,
               {}, {}, {});

    // TreeManager sinyalleri
    connect(m_tree, &TreeManager::structureChanged,
            this, &RoomManager::syncRoomsFromTree);
    connect(m_tree, &TreeManager::clientMoved,
            this, &RoomManager::onClientMoved);

    if (m_session) {
        connect(m_session, &SessionManager::chatHistoryReceived,
                this, &RoomManager::onChatHistoryReceived);
        connect(m_session, &SessionManager::chatMemberJoinedReceived,
                this, &RoomManager::onChatMemberJoined);
        connect(m_session, &SessionManager::chatMemberLeftReceived,
                this, &RoomManager::onChatMemberLeft);
        connect(m_session, &SessionManager::chatLockStateReceived,
                this, &RoomManager::onChatLockStateReceived);
    }

    syncRoomsFromTree();
}

// ── Sorgular ──────────────────────────────────────────────────────────────────

QList<Room*> RoomManager::allRooms() const   { return m_rooms.values(); }
Room*        RoomManager::room(const QString& id) const { return m_rooms.value(id); }

QString RoomManager::roomDisplayName(const QString& id) const {
    if (Room* r = m_rooms.value(id)) {
        return r->name;
    }
    if (id == kAdminRoomId) {
        return tr("Yönetici Odası");
    }
    if (id.startsWith("room_")) {
        QString nId = id.mid(5);
        if (OkidiaDatabase::instance().isOpen()) {
            ClassRecord cr = OkidiaDatabase::instance().classById(nId);
            if (!cr.name.isEmpty()) return cr.name;
        }
    } else if (OkidiaDatabase::instance().isOpen()) {
        ClassRecord cr = OkidiaDatabase::instance().classById(id);
        if (!cr.name.isEmpty()) return cr.name;
    }
    return id;
}

QString RoomManager::roomIdForNode(const QString& nodeId) const {
    return m_nodeToRoom.value(nodeId);
}

QStringList RoomManager::roomIdsForClient(const QString& clientId) const {
    QStringList result;
    for (const Room* r : m_rooms) {
        for (const RoomMember& m : r->members)
            if (m.clientId == clientId) { result << r->id; break; }
    }
    return result;
}

// ── Master bilgisi ────────────────────────────────────────────────────────────

void RoomManager::setMasterInfo(const QString& username,
                                 const QString& displayName,
                                 const QString& role) {
    m_masterUsername    = username;
    m_masterDisplayName = displayName.isEmpty() ? username : displayName;
    m_masterRole        = role;
    m_masterRoleLabel   = (role == "YM") ? tr("Yönetici")
                        : (role == "OBSERVER") ? tr("Gözlemci")
                        : tr("Öğretmen");

    const int roleEnum = (role == "YM") ? 1 : (role == "OBSERVER" ? 4 : 2);
    const QString masterId = "master:" + username;

    // 1. Öğretmenler Odası'ndaki tüm kullanıcıları DB'den doldur
    QList<UserRecord> allUsers = OkidiaDatabase::instance().allUsers();
    Room* adminRoom = m_rooms.value(kAdminRoomId);
    for (const auto& u : allUsers) {
        if (u.role != "YM" && u.role != "OM" && u.role != "OBSERVER") continue;
        const QString uId = "master:" + u.username;

        bool isOnline = (u.username == username);
        bool isWindowOpen = (u.username == username);
        if (adminRoom) {
            for (const RoomMember& am : adminRoom->members) {
                if (am.clientId == uId) {
                    isOnline = isOnline || am.online;
                    isWindowOpen = isWindowOpen || am.windowOpen;
                    break;
                }
            }
        }

        RoomMember mb;
        mb.clientId    = uId;
        mb.displayName = u.displayName.isEmpty() ? u.username : u.displayName;
        mb.role        = (u.role == "YM") ? 1 : (u.role == "OBSERVER" ? 4 : 2);
        mb.online      = isOnline;
        mb.windowOpen  = isWindowOpen;
        addMember(kAdminRoomId, mb);
    }

    // 2. Kendi hesabımızı kesin olarak ekle/güncelle
    RoomMember masterMember;
    masterMember.clientId    = masterId;
    masterMember.displayName = m_masterDisplayName;
    masterMember.role        = roleEnum;
    masterMember.online      = true;
    masterMember.windowOpen  = true;
    addMember(kAdminRoomId, masterMember);

    // 3. YM/OM → kendi sınıf odalarına da ekle
    for (Room* r : m_rooms) {
        if (r->type == RoomType::AdminRoom) continue;
        const bool hasPermission = (role == "YM")
            || (m_session && m_session->canManageClass(r->classNodeId));
        if (!hasPermission) continue;
        addMember(r->id, masterMember);
    }

    // 4. Kilit durumlarını DB'den yükle
    m_lockStates.clear();
    for (const auto& rec : OkidiaDatabase::instance().allChatLockStates())
        m_lockStates[rec.classId] = rec;
}

// ── Join / Leave (YM gözlemci modu) ──────────────────────────────────────────

bool RoomManager::isPermanentMember(const QString& roomId, const QString& clientIdOrUsername) const {
    if (roomId == kAdminRoomId) return true; // Öğretmenler Odası'nda tüm YM ve OM'ler daimidir
    Room* r = m_rooms.value(roomId);
    if (!r || r->classNodeId.isEmpty()) return false;

    QString uname = clientIdOrUsername.isEmpty() ? m_masterUsername : clientIdOrUsername;
    if (uname.startsWith("master:")) {
        uname = uname.mid(7);
    }
    if (uname.isEmpty()) return false;

    UserRecord u = OkidiaDatabase::instance().userByUsername(uname);
    if (u.id.isEmpty()) return false;

    return OkidiaDatabase::instance().classIdsForUser(u.id).contains(r->classNodeId);
}

void RoomManager::joinRoom(const QString& roomId) {
    Room* r = m_rooms.value(roomId);
    if (!r || r->type == RoomType::AdminRoom) return;
    const QString masterId = "master:" + m_masterUsername;

    const bool isPermanent = isPermanentMember(roomId);

    const int roleEnum = (m_masterRole == "YM") ? 1 : (m_masterRole == "OBSERVER" ? 4 : 2);
    RoomMember member;
    member.clientId    = masterId;
    member.displayName = m_masterDisplayName;
    member.role        = roleEnum;
    member.online      = true;
    member.windowOpen  = true;
    addMember(roomId, member);

    // Broker ve odadaki diğer katılımcılara CHAT_MEMBER_JOINED yayınla
    if (m_session) {
        okidia::ChatMemberEvent evt;
        evt.set_room_id(roomId.toStdString());
        evt.set_client_id(masterId.toStdString());
        evt.set_hostname(m_masterDisplayName.toStdString());
        evt.set_username(m_masterRole.toStdString());
        okidia::Envelope env;
        env.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
        env.set_payload(evt.SerializeAsString());
        m_session->broadcastCommand(env);
    }

    // Geçmiş yükle
    loadHistory(roomId);

    // Misafir katılım ise sistem mesajı yayınla
    if (!isPermanent) {
        QString sysMsg = (m_masterRole == "YM" ? tr("Yönetici ") : (m_masterRole == "OBSERVER" ? tr("Gözlemci ") : tr("Öğretmen "))) + m_masterDisplayName + tr(" sohbete katıldı");
        broadcastSystemMessage(roomId, sysMsg);
    }
}

void RoomManager::leaveRoom(const QString& roomId) {
    Room* r = m_rooms.value(roomId);
    if (!r || r->type == RoomType::AdminRoom) return;
    const QString masterId = "master:" + m_masterUsername;

    // Daimi üye ise odadan tamamen çıkmaz, sadece sekmesi kapanır (online kalmaya devam eder)
    if (isPermanentMember(roomId)) return;

    // Sistem mesajı önce (üye hâlâ listede)
    QString sysMsg = (m_masterRole == "YM" ? tr("Yönetici ") : (m_masterRole == "OBSERVER" ? tr("Gözlemci ") : tr("Öğretmen "))) + m_masterDisplayName + tr(" sohbetten ayrıldı");
    broadcastSystemMessage(roomId, sysMsg);
    removeMember(roomId, masterId);

    // Broker ve odadaki diğer katılımcılara CHAT_MEMBER_LEFT yayınla
    if (m_session) {
        okidia::ChatMemberEvent evt;
        evt.set_room_id(roomId.toStdString());
        evt.set_client_id(masterId.toStdString());
        okidia::Envelope env;
        env.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
        env.set_payload(evt.SerializeAsString());
        m_session->broadcastCommand(env);
    }
}

// ── Kilit durumu ──────────────────────────────────────────────────────────────

void RoomManager::setChatLock(const QString& classNodeId,
                               bool chatLocked, bool privateLocked) {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(m_tree->treeWidget(), tr("Bağlantı Yok"),
            tr("Broker bağlantısı aktif değil. Sohbet kilitlemek için lütfen sunucuya bağlanın."));
        return;
    }

    // Sohbet kilitlenince özel sohbet de kilitlenir
    if (chatLocked) privateLocked = true;

    // DB'ye yaz
    OkidiaDatabase::instance().setChatLockState(classNodeId, chatLocked, privateLocked);

    // Önbellekte güncelle
    m_lockStates[classNodeId].classId       = classNodeId;
    m_lockStates[classNodeId].chatLocked    = chatLocked;
    m_lockStates[classNodeId].privateLocked = privateLocked;

    // Agent'lara bildir (SessionManager üzerinden)
    if (m_session) {
        okidia::ChatLockState proto;
        proto.set_class_id(classNodeId.toStdString());
        proto.set_chat_locked(chatLocked);
        proto.set_private_locked(privateLocked);
        okidia::Envelope env;
        env.set_type(okidia::MessageType::CHAT_LOCK_STATE);
        env.set_payload(proto.SerializeAsString());
        // O sınıftaki tüm client'lara gönder
        const QString roomId = m_nodeToRoom.value(classNodeId);
        if (Room* r = m_rooms.value(roomId)) {
            for (const RoomMember& m : r->members) {
                if (!m.clientId.startsWith("master:"))
                    m_session->sendCommand(m.clientId, env);
            }
        }
    }

    emit lockStateChanged(classNodeId, chatLocked, privateLocked);

    QString lockDetail = chatLocked
        ? tr("Sohbet kilitlendi (Özel sohbet dahil)")
        : (privateLocked ? tr("Genel sohbet açık, özel sohbet kilitlendi") : tr("Sohbet kilidi açıldı"));
    const QString targetDisplay = roomDisplayName(m_nodeToRoom.value(classNodeId, classNodeId));
    AuditLog::instance().recordChat({}, targetDisplay, lockDetail);
}

ChatLockRecord RoomManager::chatLockState(const QString& classNodeId) const {
    return m_lockStates.value(classNodeId);
}

void RoomManager::reloadLockStates() {
    m_lockStates.clear();
    for (const auto& rec : OkidiaDatabase::instance().allChatLockStates()) {
        m_lockStates[rec.classId] = rec;
    }
}

bool RoomManager::canManageClass(const QString& classNodeId) const {
    if (m_masterRole == "YM") return true;
    if (m_session) return m_session->canManageClass(classNodeId);
    return false;
}

void RoomManager::onChatLockStateReceived(const QString& classNodeId, bool chatLocked, bool privateLocked) {
    m_lockStates[classNodeId].classId       = classNodeId;
    m_lockStates[classNodeId].chatLocked    = chatLocked;
    m_lockStates[classNodeId].privateLocked = privateLocked;
    emit lockStateChanged(classNodeId, chatLocked, privateLocked);
}

// ── Mesaj gönderme ────────────────────────────────────────────────────────────

void RoomManager::sendMessage(const QString& roomId, const QString& content) {
    Room* r = m_rooms.value(roomId);
    if (!r || content.trimmed().isEmpty()) return;

    RoomMessage msg;
    msg.id          = OkidiaUtils::generateUuid();
    msg.roomId      = roomId;
    msg.fromHost    = QHostInfo::localHostName();
    msg.fromDisplay = m_masterDisplayName.isEmpty()
                      ? QHostInfo::localHostName()
                      : m_masterDisplayName;
    msg.senderRole  = (m_masterRole == "YM") ? 1 : (m_masterRole == "OBSERVER" ? 4 : 2);
    msg.content     = content.trimmed();
    msg.timestamp   = OkidiaUtils::nowSecs();

    r->messages.append(msg);
    emit messageReceived(roomId, msg);

    // Proto mesajı oluştur ve broker'a gönder — broker dağıtır ve kaydeder
    okidia::ChatMessage proto;
    proto.set_room_id(roomId.toStdString());
    proto.set_from(msg.fromDisplay.toStdString());
    proto.set_content(content.trimmed().toStdString());
    proto.set_timestamp(static_cast<uint64_t>(msg.timestamp));
    proto.set_sender_role(static_cast<okidia::SenderRole>(msg.senderRole));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_MESSAGE);
    env.set_payload(proto.SerializeAsString());

    if (m_session) m_session->broadcastCommand(env);
}

// ── Geçmiş yükleme ────────────────────────────────────────────────────────────

void RoomManager::loadHistory(const QString& roomId, qint64 beforeTimestamp) {
    Room* r = m_rooms.value(roomId);
    if (!r) return;

    if (m_session) {
        m_session->sendChatHistoryRequest(roomId, 50, beforeTimestamp);
    }
}

void RoomManager::onChatHistoryReceived(const QString& roomId, const QList<RoomMessage>& messages) {
    Room* r = m_rooms.value(roomId);
    if (!r) return;

    r->historyLoaded = true;
    emit historyLoaded(roomId, messages);
}

// ── Gelen mesaj ───────────────────────────────────────────────────────────────

void RoomManager::onIncomingMessage(const QString& roomId,
                                     const QString& fromHost,
                                     const QString& fromDisplay,
                                     int senderRole,
                                     const QString& content,
                                     qint64 timestamp,
                                     bool isSystem) {
    Room* r = m_rooms.value(roomId);
    if (!r) return;

    RoomMessage msg;
    msg.id          = OkidiaUtils::generateUuid();
    msg.roomId      = roomId;
    msg.fromHost    = fromHost;
    msg.fromDisplay = fromDisplay.isEmpty() ? fromHost : fromDisplay;
    msg.senderRole  = senderRole;
    msg.content     = content;
    msg.timestamp   = timestamp;
    msg.isSystem    = isSystem;

    r->messages.append(msg);
    emit messageReceived(roomId, msg);
}

void RoomManager::onChatMemberJoined(const QString& roomId, const QString& clientId, const QString& displayName, const QString& roleStr) {
    int roleEnum = 3;
    if (roleStr == "YM") roleEnum = 1;
    else if (roleStr == "OBSERVER") roleEnum = 4;
    else if (roleStr == "OM") roleEnum = 2;

    RoomMember mb;
    mb.clientId    = clientId;
    QString effDisplay = displayName;
    if (roleEnum == 3 && m_session) {
        const ClientState cs = m_session->clientState(clientId);
        QString formatted = formatStudentDisplayName(cs.userDisplayName, cs.username, cs.hostname);
        if (!formatted.isEmpty()) {
            effDisplay = formatted;
        }
    }
    mb.displayName = effDisplay.isEmpty() ? clientId : effDisplay;
    mb.role        = roleEnum;
    mb.online      = true;
    mb.windowOpen  = true;

    if (Room* r = m_rooms.value(roomId)) {
        addMember(roomId, mb);
    } else {
        // Oda henüz ağaçtan oluşturulmadıysa bekleyenler kuyruğuna kaydet
        m_pendingMembers[roomId][clientId] = mb;
    }
    AuditLog::instance().recordChat(mb.displayName, roomDisplayName(roomId), tr("Sohbet odasına katıldı"));

    // Eğer Master bağlandıysa, diğer üye olduğu odalarda da online durumunu güncelle
    if (clientId.startsWith("master:")) {
        for (Room* otherRoom : m_rooms) {
            if (otherRoom->id == roomId) continue;
            for (RoomMember& m : otherRoom->members) {
                if (m.clientId == clientId) {
                    m.online = true;
                    m.windowOpen = true;
                    emit memberOnlineChanged(otherRoom->id, clientId, true);
                    emit memberWindowChanged(otherRoom->id, clientId, true);
                    break;
                }
            }
        }
    }
}

void RoomManager::onChatMemberLeft(const QString& roomId, const QString& clientId) {
    QString actorDisplay = clientId;
    if (Room* r = m_rooms.value(roomId)) {
        for (const RoomMember& m : r->members) {
            if (m.clientId == clientId && !m.displayName.isEmpty()) {
                actorDisplay = m.displayName;
                break;
            }
        }
    }
    AuditLog::instance().recordChat(actorDisplay, roomDisplayName(roomId), tr("Sohbet odasından ayrıldı"));
    if (Room* r = m_rooms.value(roomId)) {
        // Eğer ayrılan üye daimi bir üye değilse (misafir ise), odadan tamamen sil
        if (!isPermanentMember(roomId, clientId) && clientId.startsWith("master:")) {
            removeMember(roomId, clientId);
        } else {
            for (RoomMember& m : r->members) {
                if (m.clientId == clientId) {
                    m.online     = false;
                    m.windowOpen = false;
                    emit memberOnlineChanged(roomId, clientId, false);
                    emit memberWindowChanged(roomId, clientId, false);
                    break;
                }
            }
        }
    } else {
        if (m_pendingMembers.contains(roomId)) {
            m_pendingMembers[roomId].remove(clientId);
        }
    }

    if (roomId == kAdminRoomId && clientId.startsWith("master:")) {
        // Diğer odalarda da offline yap
        for (Room* otherRoom : m_rooms) {
            if (otherRoom->id == kAdminRoomId) continue;
            for (RoomMember& m : otherRoom->members) {
                if (m.clientId == clientId) {
                    m.online     = false;
                    m.windowOpen = false;
                    emit memberOnlineChanged(otherRoom->id, clientId, false);
                    emit memberWindowChanged(otherRoom->id, clientId, false);
                    break;
                }
            }
        }
    }
}

// ── Agent bağlantı bildirimleri ───────────────────────────────────────────────

void RoomManager::onClientConnected(const QString& clientId,
                                      const QString& displayName,
                                      const QString& /*ip*/) {
    // Agent'ın bulunduğu tüm odaları bul ve online durumunu güncelle
    QStringList rooms = roomIdsForClient(clientId);
    if (rooms.isEmpty()) {
        syncRoomsFromTree();
        rooms = roomIdsForClient(clientId);
    }
    for (const QString& rId : rooms) {
        if (Room* r = m_rooms.value(rId)) {
            for (RoomMember& m : r->members) {
                if (m.clientId == clientId) {
                    bool changed = (!m.online) || (!displayName.isEmpty() && m.displayName != displayName);
                    if (!displayName.isEmpty()) m.displayName = displayName;
                    m.online = true;
                    if (changed) {
                        emit memberOnlineChanged(rId, clientId, true);
                    }
                    break;
                }
            }
            // Agent'a oda bilgisini gönder — pencere açıksa sekme oluşturabilsin
            pushRoomInfoToClient(r, clientId);
        }
    }
}

void RoomManager::onClientDisconnected(const QString& clientId) {
    const QStringList rooms = roomIdsForClient(clientId);
    for (const QString& rId : rooms) {
        if (Room* r = m_rooms.value(rId)) {
            for (RoomMember& m : r->members) {
                if (m.clientId == clientId) {
                    m.online     = false;
                    m.windowOpen = false;
                    emit memberOnlineChanged(rId, clientId, false);
                    emit memberWindowChanged(rId, clientId, false);
                    break;
                }
            }
        }
    }
}

void RoomManager::onAgentWindowState(const QString& clientId, bool open) {
    const QStringList rooms = roomIdsForClient(clientId);
    for (const QString& rId : rooms) {
        if (Room* r = m_rooms.value(rId)) {
            for (RoomMember& m : r->members) {
                if (m.clientId == clientId) {
                    m.windowOpen = open;
                    emit memberWindowChanged(rId, clientId, open);
                    break;
                }
            }
        }
    }
}

// ── Oda yönetimi (iç) ────────────────────────────────────────────────────────

Room* RoomManager::createRoom(const QString& id,
                               const QString& name,
                               RoomType type,
                               const QString& nodeId,
                               const QString& classNodeId,
                               const QString& parentRoomId) {
    if (m_rooms.contains(id)) return m_rooms[id];

    auto* r = new Room();
    r->id           = id;
    r->name         = name;
    r->type         = type;
    r->nodeId       = nodeId;
    r->classNodeId  = classNodeId;
    r->parentRoomId = parentRoomId;
    r->permanent    = (type == RoomType::AdminRoom);

    m_rooms[id] = r;
    if (!nodeId.isEmpty()) m_nodeToRoom[nodeId] = id;

    // Bekleyen üyelik kuyruğundaki üyeleri odaya aktar
    if (m_pendingMembers.contains(id)) {
        for (const RoomMember& pm : m_pendingMembers.value(id)) {
            addMember(id, pm);
        }
    }

    emit roomAdded(id);
    Logger::info(QObject::tr("RoomManager: Oda oluşturuldu → ") + name + QObject::tr(" (ID: ") + id + QObject::tr(", Parent: ") + parentRoomId + ")");
    return r;
}

void RoomManager::destroyRoom(const QString& roomId) {
    if (!m_rooms.contains(roomId)) return;
    Room* r = m_rooms.take(roomId);
    if (!r->nodeId.isEmpty()) {
        m_nodeToRoom.remove(r->nodeId);
    }
    m_nodeToRoom.remove(r->classNodeId);
    emit roomRemoved(roomId);
    delete r;
}

void RoomManager::addMember(const QString& roomId, const RoomMember& member) {
    Room* r = m_rooms.value(roomId);
    if (!r) return;
    for (RoomMember& m : r->members) {
        if (m.clientId == member.clientId) {
            bool onlineChanged = (m.online != member.online);
            bool windowChanged = (m.windowOpen != member.windowOpen);
            bool nameChanged   = (!member.displayName.isEmpty() && m.displayName != member.displayName);
            m.displayName = member.displayName.isEmpty() ? m.displayName : member.displayName;
            m.role = member.role;
            m.online = member.online;
            m.windowOpen = member.windowOpen;
            if (onlineChanged || nameChanged) emit memberOnlineChanged(roomId, m.clientId, m.online);
            if (windowChanged) emit memberWindowChanged(roomId, m.clientId, m.windowOpen);
            return;
        }
    }
    r->members.append(member);
    emit memberJoined(roomId, member);
    // Agent'a oda bilgisini ilet (pencere açıksa mesaj alabilsin)
    pushRoomInfoToClient(r, member.clientId);
}

void RoomManager::removeMember(const QString& roomId, const QString& clientId) {
    Room* r = m_rooms.value(roomId);
    if (!r) return;
    for (int i = 0; i < r->members.size(); ++i) {
        if (r->members[i].clientId == clientId) {
            r->members.removeAt(i);
            emit memberLeft(roomId, clientId);
            return;
        }
    }
}

void RoomManager::pushRoomInfoToClient(Room* r, const QString& clientId) {
    if (!m_session || !r || clientId.startsWith("master:")) return;
    okidia::ChatRoomCreated proto;
    proto.set_room_id(r->id.toStdString());
    proto.set_room_name(r->name.toStdString());
    proto.set_parent_room_id(r->parentRoomId.toStdString());
    for (const RoomMember& m : r->members) {
        auto* mb = proto.add_members();
        mb->set_client_id(m.clientId.toStdString());
        // display = "hostname · username"
        mb->set_hostname(m.displayName.toStdString());
        mb->set_username(m.role == 1 ? "YM" : (m.role == 4 ? "OBSERVER" : (m.role == 2 ? "OM" : "AC")));
        mb->set_online(m.online);
        mb->set_window_open(m.windowOpen);
    }
    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_ROOM_CREATED);
    env.set_payload(proto.SerializeAsString());
    m_session->sendCommand(clientId, env);
}

void RoomManager::addSystemMessage(const QString& roomId, const QString& text) {
    Room* r = m_rooms.value(roomId);
    if (!r) return;
    RoomMessage msg;
    msg.id        = OkidiaUtils::generateUuid();
    msg.roomId    = roomId;
    msg.content   = text;
    msg.timestamp = OkidiaUtils::nowSecs();
    msg.isSystem  = true;
    r->messages.append(msg);
    emit messageReceived(roomId, msg);
}

void RoomManager::broadcastSystemMessage(const QString& roomId, const QString& text) {
    Room* r = m_rooms.value(roomId);
    if (!r || text.trimmed().isEmpty()) return;

    // Locally add the system message
    addSystemMessage(roomId, text.trimmed());

    // Broadcast to others
    okidia::ChatMessage proto;
    proto.set_room_id(roomId.toStdString());
    proto.set_from("Sistem");
    proto.set_content(text.trimmed().toStdString());
    proto.set_timestamp(static_cast<uint64_t>(OkidiaUtils::nowSecs()));
    proto.set_msg_type(okidia::CHAT_MSG_SYSTEM);
    proto.set_sender_role(okidia::ROLE_UNKNOWN);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CHAT_MESSAGE);
    env.set_payload(proto.SerializeAsString());

    if (m_session) m_session->broadcastCommand(env);
}

// ── Tree senkronizasyonu ──────────────────────────────────────────────────────

void RoomManager::syncRoomsFromTree() {
    if (!m_tree || m_tree->viewMode() != TreeManager::ClassView) return;

    QTreeWidget* tw = m_tree->treeWidget();
    if (!tw) return;

    QSet<QString> activeNodeIds;

    std::function<void(QTreeWidgetItem*, const QString&, const QString&)> traverse =
        [&](QTreeWidgetItem* item, const QString& parentRoomId, const QString& parentClassNodeId) {
        const QString type   = m_tree->nodeType(item);
        const bool isClass   = (type == TreeManager::kTypeClass);
        const bool isGroup   = (type == TreeManager::kTypeGroup);
        if (!isClass && !isGroup) return;

        const QString nodeId = m_tree->publicStableId(item);
        if (nodeId.isEmpty()) return;
        activeNodeIds.insert(nodeId);

        const QString currentClassNodeId = isClass ? nodeId : parentClassNodeId;

        Room* r = nullptr;
        if (!m_nodeToRoom.contains(nodeId)) {
            // Yeni oda oluştur
            const QString rId   = "room_" + nodeId;
            const QString rName = m_tree->displayName(item);
            const QString prId  = isGroup ? parentRoomId : QString{};
            r = createRoom(rId, rName,
                           isClass ? RoomType::ClassRoom : RoomType::GroupRoom,
                           nodeId, currentClassNodeId, prId);
        } else {
            // İsim veya üst oda ilişkisi değişmiş olabilir
            const QString existingId = m_nodeToRoom.value(nodeId);
            r = m_rooms.value(existingId);
            if (r) {
                const QString newName = m_tree->displayName(item);
                const QString prId    = isGroup ? parentRoomId : QString{};
                if (r->parentRoomId != prId) {
                    r->parentRoomId = prId;
                }
                if (r->name != newName) {
                    r->name = newName;
                    emit roomRenamed(r->id, newName);
                }
            }
        }

        if (r) {
            Room* adminRoom = m_rooms.value(kAdminRoomId);

            // 1. Bu sınıfa atanmış YM yöneticileri ve OM öğretmenleri DB'den daimi üye olarak ekle
            const QList<UserRecord> allDbUsers = OkidiaDatabase::instance().allUsers();
            for (const auto& u : allDbUsers) {
                if (u.role != "YM" && u.role != "OM" && u.role != "OBSERVER") continue;
                
                // YM ise her odaya ekle, OM ise sadece yetkili olduğu sınıf/gruplara ekle
                if (u.role != "YM") {
                    if (!OkidiaDatabase::instance().classIdsForUser(u.id).contains(currentClassNodeId)) continue;
                }

                const QString managerMasterId = "master:" + u.username;
                bool isSelf = (u.username == m_masterUsername);
                bool isOnline = isSelf || (m_session && m_session->isMasterOnline(managerMasterId));
                if (adminRoom) {
                    for (const RoomMember& am : adminRoom->members) {
                        if (am.clientId == managerMasterId) {
                            isOnline = isOnline || am.online;
                            break;
                        }
                    }
                }
                if (m_pendingMembers.contains(r->id) && m_pendingMembers[r->id].contains(managerMasterId)) {
                    const RoomMember& pm = m_pendingMembers[r->id][managerMasterId];
                    isOnline = isOnline || pm.online;
                }

                // YM veya bu sınıfa atanmış OM için: login olduğu sürece odalarında online ve windowOpen true kalır
                bool isWindowOpen = isOnline;

                RoomMember tm;
                tm.clientId    = managerMasterId;
                tm.displayName = u.displayName.isEmpty() ? u.username : u.displayName;
                tm.role        = (u.role == "YM") ? 1 : (u.role == "OBSERVER" ? 4 : 2);
                tm.online      = isOnline;
                tm.windowOpen  = isWindowOpen;
                addMember(r->id, tm);
            }

            // 2. Eğer şu anki Master bu odaya misafir olarak girmişse (aktif üyeyse), üyeliğini koru
            if (!m_masterUsername.isEmpty()) {
                const QString masterId = "master:" + m_masterUsername;
                for (const RoomMember& existing : r->members) {
                    if (existing.clientId == masterId && existing.online) {
                        addMember(r->id, existing);
                        break;
                    }
                }
            }

            // Bu node altındaki tüm client (öğrenci) üyeleri topla
            std::function<void(QTreeWidgetItem*)> collectClients = [&](QTreeWidgetItem* parentItem) {
                for (int c = 0; c < parentItem->childCount(); ++c) {
                    QTreeWidgetItem* ch = parentItem->child(c);
                    if (m_tree->nodeType(ch) == TreeManager::kTypeClient) {
                        const QString cid = ch->data(0, Qt::UserRole + 1).toString();
                        if (!cid.isEmpty()) {
                            const ClientState cs = m_session ? m_session->clientState(cid) : ClientState{};
                            bool isOnline = cs.connected;
                            bool isWindowOpen = m_session ? m_session->isAgentWindowOpen(cid) : false;
                            for (const RoomMember& existing : r->members) {
                                if (existing.clientId == cid) {
                                    isOnline = isOnline || existing.online;
                                    isWindowOpen = isWindowOpen || existing.windowOpen;
                                    break;
                                }
                            }
                            if (m_pendingMembers.contains(r->id) && m_pendingMembers[r->id].contains(cid)) {
                                const RoomMember& pm = m_pendingMembers[r->id][cid];
                                isOnline = isOnline || pm.online;
                                isWindowOpen = isWindowOpen || pm.windowOpen;
                            }
                            RoomMember cm;
                            cm.clientId    = cid;
                            cm.displayName = formatStudentDisplayName(cs.userDisplayName, cs.username, cs.hostname, cid);
                            cm.role        = 3; // Öğrenci
                            cm.online      = isOnline;
                            cm.windowOpen  = isWindowOpen;
                            addMember(r->id, cm);
                            if (cm.online) {
                                pushRoomInfoToClient(r, cid);
                            }
                        }
                    } else {
                        collectClients(ch);
                    }
                }
            };
            collectClients(item);

            // 3. Varsa diğer bekleyen üyelikleri odaya uygula
            if (m_pendingMembers.contains(r->id)) {
                for (const RoomMember& pm : m_pendingMembers[r->id]) {
                    addMember(r->id, pm);
                }
            }
        }

        // Alt grupları işle
        const QString thisRoomId = m_nodeToRoom.value(nodeId);
        for (int i = 0; i < item->childCount(); ++i) {
            traverse(item->child(i), thisRoomId, currentClassNodeId);
        }
    };

    for (int i = 0; i < tw->topLevelItemCount(); ++i)
        traverse(tw->topLevelItem(i), {}, {});

    // Artık olmayan node odalarını sil (Yönetici Odası hariç)
    QStringList toRemove;
    for (auto it = m_nodeToRoom.constBegin(); it != m_nodeToRoom.constEnd(); ++it) {
        if (!activeNodeIds.contains(it.key()))
            toRemove << it.value();
    }
    for (const QString& rId : toRemove) {
        if (rId != QString(kAdminRoomId))
            destroyRoom(rId);
    }
}

void RoomManager::onClientMoved(const QString& clientId,
                                  QTreeWidgetItem* newNode) {
    if (!newNode) return;

    // Eski odalardan çıkar (Yönetici Odası hariç)
    for (Room* r : m_rooms) {
        if (r->id == kAdminRoomId) continue;
        bool wasInRoom = false;
        for (const RoomMember& m : r->members) {
            if (m.clientId == clientId) {
                wasInRoom = true;
                break;
            }
        }
        if (wasInRoom) {
            removeMember(r->id, clientId);
            if (m_session) {
                okidia::ChatMemberEvent evt;
                evt.set_room_id(r->id.toStdString());
                evt.set_client_id(clientId.toStdString());
                okidia::Envelope env;
                env.set_type(okidia::MessageType::CHAT_MEMBER_LEFT);
                env.set_payload(evt.SerializeAsString());
                m_session->broadcastCommand(env);
            }
        }
    }

    // Yeni node'un odalarına (hedef grup ve üst sınıf) ekle
    const auto& clientState = m_session ? m_session->clientState(clientId) : ClientState{};
    const bool isWindowOpen = m_session ? m_session->isAgentWindowOpen(clientId) : false;

    auto* cur = newNode;
    while (cur) {
        const QString curType = m_tree->nodeType(cur);
        if (curType == TreeManager::kTypeClass || curType == TreeManager::kTypeGroup) {
            const QString pNodeId = m_tree->publicStableId(cur);
            const QString pRoomId = m_nodeToRoom.value(pNodeId);
            if (!pRoomId.isEmpty() && m_rooms.contains(pRoomId)) {
                RoomMember member;
                member.clientId    = clientId;
                member.displayName = formatStudentDisplayName(clientState.userDisplayName, clientState.username, clientState.hostname, clientId);
                member.role        = 3;
                member.online      = clientState.connected;
                member.windowOpen  = isWindowOpen;
                addMember(pRoomId, member);
                
                if (Room* newRoom = m_rooms.value(pRoomId)) {
                    if (member.online) {
                        pushRoomInfoToClient(newRoom, clientId);
                    }
                    if (m_session && member.online) {
                        okidia::ChatMemberEvent evt;
                        evt.set_room_id(pRoomId.toStdString());
                        evt.set_client_id(clientId.toStdString());
                        evt.set_hostname(member.displayName.toStdString());
                        evt.set_username("AC");
                        okidia::Envelope env;
                        env.set_type(okidia::MessageType::CHAT_MEMBER_JOINED);
                        env.set_payload(evt.SerializeAsString());
                        m_session->broadcastCommand(env);
                    }
                }
            }
        }
        cur = cur->parent();
    }
}
