// master/chat/RoomManager.h
// Sohbet oda yönetimi — yeni mimari
#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QSet>
#include <QString>
#include <QTreeWidgetItem>
#include "../../../common/db/DbRecords.h"

class TreeManager;
class SessionManager;

// ── Veri yapıları ─────────────────────────────────────────────────────────────

struct RoomMessage {
    QString id;
    QString roomId;
    QString fromHost;     // hostname (agent) veya username (master)
    QString fromDisplay;  // gösterim adı
    int     senderRole;   // 0=agent, 1=YM/OM
    QString content;
    qint64  timestamp   = 0;
    bool    isSystem    = false;
};

struct RoomMember {
    QString clientId;    // machineId veya "master:<hostname>"
    QString displayName; // "hostname · username" veya "Öğretmen Adı"
    int     role        = 3; // 1=YM (Yönetici), 2=OM (Öğretmen), 3=AC (Öğrenci)
    bool    online      = false;
    bool    windowOpen  = false; // agent sohbet penceresi açık mı
};

enum class RoomType {
    AdminRoom,  // Yönetici Odası — sadece YM/OM
    ClassRoom,  // Sınıf odası
    GroupRoom,  // Alt grup odası
};

struct Room {
    QString         id;
    QString         name;
    RoomType        type        = RoomType::ClassRoom;
    QString         nodeId;         // bağlı ağaç düğümü UUID (sınıf veya grup)
    QString         classNodeId;    // bağlı ana sınıf UUID (yetki ve kilit için)
    QString         parentRoomId;   // grup odaları için
    bool            permanent   = false;  // Yönetici Odası kalıcı
    QList<RoomMember>  members;
    QList<RoomMessage> messages;   // bellekteki son mesajlar
    bool            historyLoaded = false;
};

// ── RoomManager ───────────────────────────────────────────────────────────────

class RoomManager : public QObject {
    Q_OBJECT
public:
    static const char kAdminRoomId[];

    explicit RoomManager(TreeManager*    tree,
                         SessionManager* session,
                         QObject*        parent = nullptr);
    ~RoomManager() override = default;

    static QString formatStudentDisplayName(const QString& userDisplayName,
                                           const QString& username,
                                           const QString& hostname,
                                           const QString& fallbackId = QString()) {
        QString namePart = !userDisplayName.trimmed().isEmpty() ? userDisplayName.trimmed() : username.trimmed();
        QString hostPart = hostname.trimmed();
        if (!namePart.isEmpty() && !hostPart.isEmpty()) {
            return QString("%1 - %2").arg(namePart, hostPart);
        } else if (!namePart.isEmpty()) {
            return namePart;
        } else if (!hostPart.isEmpty()) {
            return hostPart;
        }
        return fallbackId;
    }

    // ── Sorgular ──────────────────────────────────────────────────────────────
    QList<Room*>  allRooms()  const;
    Room*         room(const QString& roomId) const;
    QString       roomDisplayName(const QString& roomId) const;
    QString       roomIdForNode(const QString& classNodeId) const;
    QStringList   roomIdsForClient(const QString& clientId) const;

    // ── Mesaj gönderme (master → oda) ─────────────────────────────────────────
    void sendMessage(const QString& roomId, const QString& content);

    // ── YM gözlemci join/leave (kendi odası dışındaki odalar) ─────────────────
    void joinRoom (const QString& roomId);
    void leaveRoom(const QString& roomId);

    // ── Kilit durumu ──────────────────────────────────────────────────────────
    void setChatLock(const QString& classNodeId, bool chatLocked, bool privateLocked);
    ChatLockRecord chatLockState(const QString& classNodeId) const;
    QMap<QString, ChatLockRecord> allLockStates() const { return m_lockStates; }
    void reloadLockStates();
    bool canManageClass(const QString& classNodeId) const;
    bool isPermanentMember(const QString& roomId, const QString& clientIdOrUsername = QString()) const;

    // ── Geçmiş yükleme (YM/OM için scroll yukarı) ────────────────────────────
    // before: bu timestamp'ten önceki 50 mesajı yükle
    // Yükleme tamamlanınca historyLoaded(roomId) sinyali emit edilir
    void broadcastSystemMessage(const QString& roomId, const QString& text);
    
    void loadHistory(const QString& roomId, qint64 beforeTimestamp = 0);

    // ── Master bilgisi ────────────────────────────────────────────────────────
    // displayName: "Ad Soyad" (users.display_name), role: "YM" | "OM"
    void setMasterInfo(const QString& username,
                       const QString& displayName,
                       const QString& role);
    QString masterName() const { return m_masterDisplayName; }
    QString masterUsername() const { return m_masterUsername; }
    QString masterRole() const { return m_masterRole; }

    // ── Agent bağlantı bildirimleri ───────────────────────────────────────────
    void onClientConnected   (const QString& clientId, const QString& displayName, const QString& ip);
    void onClientDisconnected(const QString& clientId);

    // ── Gelen mesaj (SessionManager'dan) ─────────────────────────────────────
    void onIncomingMessage(const QString& roomId,
                           const QString& fromHost,
                           const QString& fromDisplay,
                           int senderRole,
                           const QString& content,
                           qint64 timestamp,
                           bool isSystem = false);

    // ── Agent pencere durumu bildirimi ────────────────────────────────────────
    // Broker bu bilgiyi master'a iletir: agent pencereyi açtı/kapattı
    void onAgentWindowState(const QString& clientId, bool open);

signals:
    // Oda yapısı
    void roomAdded  (const QString& roomId);
    void roomRemoved(const QString& roomId);
    void roomRenamed(const QString& roomId, const QString& newName);

    // Üyeler
    void memberJoined       (const QString& roomId, const RoomMember& member);
    void memberLeft         (const QString& roomId, const QString& clientId);
    void memberWindowChanged(const QString& roomId, const QString& clientId, bool open);
    void memberOnlineChanged(const QString& roomId, const QString& clientId, bool online);

    // Mesajlar
    void messageReceived(const QString& roomId, const RoomMessage& msg);
    void historyLoaded  (const QString& roomId, const QList<RoomMessage>& messages);

    // Kilit durumu değişti — ChatWindow ve TreeManager günceller
    void lockStateChanged(const QString& classNodeId, bool chatLocked, bool privateLocked);

public slots:
    void syncRoomsFromTree();

private slots:
    void onClientMoved(const QString& clientId, QTreeWidgetItem* newNode);
    void onChatHistoryReceived(const QString& roomId, const QList<RoomMessage>& messages);
    void onChatMemberJoined(const QString& roomId, const QString& clientId, const QString& displayName, const QString& roleStr);
    void onChatMemberLeft(const QString& roomId, const QString& clientId);
    void onChatLockStateReceived(const QString& classNodeId, bool chatLocked, bool privateLocked);

private:
    Room* createRoom(const QString& id,
                     const QString& name,
                     RoomType       type,
                     const QString& nodeId       = {},
                     const QString& classNodeId  = {},
                     const QString& parentRoomId = {});
    void  destroyRoom(const QString& roomId);

    void addMember   (const QString& roomId, const RoomMember& member);
    void removeMember(const QString& roomId, const QString& clientId);

    // Agent'a oda bilgisi gönder (CHAT_ROOM_CREATED proto)
    void pushRoomInfoToClient(Room* room, const QString& clientId);
    // Sistem mesajı ekle (üye katıldı/ayrıldı vb.)
    void addSystemMessage(const QString& roomId, const QString& text);

    TreeManager*    m_tree;
    SessionManager* m_session;

    QString m_masterUsername;
    QString m_masterDisplayName; // "Ad Soyad" (users.display_name)
    QString m_masterRole;        // "YM" | "OM"
    QString m_masterRoleLabel;   // "Yönetici" | "Öğretmen"

    QMap<QString, Room*>    m_rooms;      // roomId → Room
    QMap<QString, QString>  m_nodeToRoom; // nodeId → roomId
    QMap<QString, ChatLockRecord> m_lockStates; // classNodeId → kilit durumu
    QMap<QString, QMap<QString, RoomMember>> m_pendingMembers; // roomId → (clientId → RoomMember)
};
