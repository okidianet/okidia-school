// master/ui/ChatWindow/ChatWindow.h
#pragma once

#include <QMainWindow>
#include <QMap>
#include <QTabBar>
#include <QTreeWidgetItem>
#include <QTimer>
#include "../../chat/RoomManager.h"
#include "../../chat/PrivateChatManager.h"

class QTreeWidget;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QSplitter;
class QHeaderView;
class QStackedWidget;
class QComboBox;
class SessionManager;

namespace ui {
namespace chat {
class ChatWidget;
class ChatModel;
struct ChatMessage;
}
}

class ChatWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ChatWindow(RoomManager* rooms, PrivateChatManager* priv,
                        SessionManager* session = nullptr,
                        QWidget* parent = nullptr);
    ~ChatWindow() override = default;

    void showAndRaise();
    void buildRoomTree();
    void selectRoom(const QString& roomId);
    void switchToTab(const QString& id);
    void updateRoomLockEmojis(const QString& roomId,
                               bool chatLocked, bool privateLocked);
    void setBlockedByLock(bool blocked);
    bool isBlockedByLock() const { return m_blockedByLock; }

signals:
    void unreadMessageArrived();  // toolbar badge için
    void chatWindowOpened();      // badge sıfırla

protected:
    void closeEvent (QCloseEvent* ev) override;
    void showEvent  (QShowEvent*  ev) override;
    void changeEvent(QEvent*      ev) override;

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onFlashTimerTick();

    void onRoomFilterChanged(int index);
    void onRoomSelected();
    void onRoomDoubleClicked(QTreeWidgetItem* item, int col);
    void onRoomItemClicked  (QTreeWidgetItem* item, int col);
    void onLockStateChanged (const QString& classNodeId, bool chatLocked, bool privateLocked);
    void onRoomAdded   (const QString& roomId);
    void onRoomRemoved (const QString& roomId);
    void onRoomRenamed (const QString& roomId, const QString& name);
    void onMessageReceived(const QString& roomId, const RoomMessage& msg);
    void onHistoryLoaded  (const QString& roomId, const QList<RoomMessage>& msgs);
    void onMemberJoined       (const QString& roomId, const RoomMember& member);
    void onMemberLeft         (const QString& roomId, const QString& clientId);
    void onMemberOnlineChanged(const QString& roomId, const QString& clientId, bool online);
    void onMemberWindowChanged(const QString& roomId, const QString& clientId, bool open);
    void onMemberDoubleClicked(QListWidgetItem* item);
    void onMemberContextMenuRequested(const QPoint& pos);

    void onRoomJoinRequestReceived(const QString& requestId, const QString& roomId,
                                   const QString& roomName, const QString& fromUsername,
                                   const QString& fromDisplay, const QString& targetUsername);
    void onRoomJoinResponseReceived(const QString& requestId, const QString& roomId,
                                    const QString& fromUsername, const QString& targetUsername,
                                    bool accepted, const QString& reason);

    void onPrivateSessionOpened (const QString& sessionId);
    void onPrivateSessionClosed (const QString& sessionId);
    void onPrivateMessageReceived(const QString& sessionId, const PrivateMessage& msg);
    void onPrivateRequestReceived(const PendingRequest& req);
    void onHandRaiseReceived     (const QString& clientId, const QString& message);
    void onChatMuteStateReceived (const QString& clientId, const QString& username,
                                  bool muted, qint64 mutedUntil, int violationLevel, const QString& reason);

    void onChatMessageReadyToSend(const QString& text);
    void onChatContextMenuRequested(const QString& msgId, const QPoint& pos);

private:
    struct TabItem {
        QString id;          // roomId veya sessionId
        bool    isPrivate = false;
        QString title;       // Temel başlık
        int     unread = 0;
        ui::chat::ChatModel*  model  = nullptr;
        ui::chat::ChatWidget* widget = nullptr;
    };

    QTreeWidgetItem* roomItem(const QString& roomId) const;
    QTreeWidgetItem* makeRoomItem(const Room* r) const;
    
    void openRoomTab(const QString& roomId, bool activate = false);
    void openPrivateTab(const QString& sessionId, bool activate = true);
    void closeTabById(const QString& id);

    void refreshMemberList(const QString& roomId);
    void appendMessage    (const QString& roomId, const RoomMessage& msg);
    void rebuildMessages  (const QString& roomId);
    
    void notifyUnread(const QString& id);
    void clearUnread (const QString& id);
    void updateTabDisplay(int tabIndex);

    void setInputEnabled  (bool enabled);
    QString activeId() const;
    bool isActivePrivate() const;

    RoomManager*        m_rooms;
    PrivateChatManager* m_priv;
    SessionManager*     m_session = nullptr;

    QTabBar*        m_tabBar       = nullptr;
    QSplitter*      m_splitter     = nullptr;
    QWidget*        m_leftW        = nullptr;
    QComboBox*      m_roomFilterCombo = nullptr;
    QTreeWidget*    m_roomTree     = nullptr;
    QStackedWidget* m_stack        = nullptr;
    QLabel*         m_roomTitle    = nullptr;
    QWidget*        m_rightW       = nullptr;
    QListWidget*    m_memberList   = nullptr;

    QMap<QString, TabItem*> m_tabs; // id -> TabItem
    QString                 m_activeId;
    bool                    m_filterOnlyMyRooms = false;
    QMap<QString, QString>  m_pendingJoinRequests; // requestId -> roomId

    QTimer* m_flashTimer = nullptr;
    bool    m_flashState = false;
    QTimer* m_memberRefreshTimer = nullptr;

    bool m_blockedByLock = false;
    bool m_wantsShowAfterUnlock = false;
    QMap<QString, qint64> m_mutedUsers; // "<clientId>:<username>" -> mutedUntil unix secs
};
