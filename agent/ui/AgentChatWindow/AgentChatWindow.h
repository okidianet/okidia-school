// agent/ui/AgentChatWindow/AgentChatWindow.h
#pragma once

#include <QMainWindow>
#include <QMap>
#include <QList>
#include <QTimer>

class QTabBar;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QStackedWidget;
class QWidget;
class QVBoxLayout;
class QLabel;

namespace ui {
namespace chat {
class ChatWidget;
class ChatModel;
struct ChatMessage;
}
}

struct AgentRoomInfo {
    QString id;
    QString name;
    QString parentRoomId;
};

struct AgentMemberInfo {
    QString clientId;
    QString displayName;
    int     role = 3; // 1=YM, 2=OM, 3=AC
    bool    online = true;
    bool    windowOpen = true;
};

struct AgentMessage {
    QString fromDisplay;
    QString content;
    qint64  timestamp = 0;
    bool    fromMe    = false;
    bool    isSystem  = false;
    int     senderRole= 0;
};

class AgentChatWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AgentChatWindow(const QString& myClientId,
                             const QString& myDisplay,
                             QWidget* parent = nullptr);
    ~AgentChatWindow() override = default;

    void addRoom   (const AgentRoomInfo& room);
    void removeRoom(const QString& roomId);

    void addMember   (const QString& roomId, const AgentMemberInfo& member);
    void removeMember(const QString& roomId, const QString& clientId);
    void setMemberOnline(const QString& roomId, const QString& clientId, bool online);
    void setMemberWindowState(const QString& roomId, const QString& clientId, bool windowOpen);

    void appendMessage(const QString& roomId, const AgentMessage& msg);

    void openPrivateTab  (const QString& sessionId, const QString& peerDisplay);
    void closePrivateTab (const QString& sessionId);
    void appendPrivateMsg(const QString& sessionId, const QString& content,
                          bool fromMe, qint64 timestamp);
    void showPrivateRequest(const QString& requestId, const QString& fromDisplay);

    void setChatLocked   (const QString& roomId, bool locked);
    void setPrivateLocked(bool locked);

signals:
    void messageSent          (const QString& roomId,    const QString& content);
    void privateMessageSent   (const QString& sessionId, const QString& content);
    void privateRequested     (const QString& peerId,    const QString& peerDisplay);
    void privateAccepted      (const QString& requestId);
    void privateRejected      (const QString& requestId);
    void privateSessionCloseRequested(const QString& sessionId);
    void handRaiseRequested   (const QString& note);
    void windowClosed         ();

protected:
    void closeEvent (QCloseEvent* ev) override;
    void showEvent  (QShowEvent*  ev) override;
    void changeEvent(QEvent*      ev) override;

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onFlashTimerTick();
    void onHelpButtonClicked();

    void onMemberDoubleClicked(QListWidgetItem* item);
    void onChatMessageReadyToSend(const QString& text);
    void onChatContextMenuRequested(const QString& msgId, const QPoint& pos);

private:
    struct TabItem {
        QString id;
        bool    isPrivate = false;
        QString title;
        int     unread = 0;
        ui::chat::ChatModel*  model  = nullptr;
        ui::chat::ChatWidget* widget = nullptr;
        QMap<QString, AgentMemberInfo> memberMap;
    };

    void activateTab(const QString& id);
    void rebuildMemberList(TabItem* tab);
    void notifyUnread(const QString& id);
    void clearUnread (const QString& id);
    void updateTabDisplay(int tabIndex);

    QString m_myClientId;
    QString m_myDisplay;

    QTabBar*        m_tabBar      = nullptr;
    QSplitter*      m_splitter    = nullptr;
    QStackedWidget* m_stack       = nullptr;
    QWidget*        m_memberPanel = nullptr;
    QListWidget*    m_memberList  = nullptr;
    QLabel*         m_memberTitle = nullptr;

    QString m_activeId;

    QMap<QString, TabItem*> m_tabs; // id -> TabItem

    QTimer* m_flashTimer = nullptr;
    bool    m_flashState = false;
};
