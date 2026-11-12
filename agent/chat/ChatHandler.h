// agent/chat/ChatHandler.h
#pragma once

#include <QObject>
#include <QList>
#include <QMap>
#include <QPair>
#include "../../common/network/LocalConnection.h"
#include "../ui/AgentChatWindow/AgentChatWindow.h"

class ChatHandler : public QObject {
    Q_OBJECT
public:
    explicit ChatHandler(QObject* parent = nullptr);

    ~ChatHandler();

    void setConnection(LocalConnection* conn) { m_conn = conn; }
    void setMyInfo(const QString& clientId, const QString& hostname,
                   const QString& username);

    void handleEnvelope(const okidia::Envelope& envelope);
    void showChatWindow(bool allowEmpty = false);
    void updateChatAvailability();

signals:
    void chatAvailabilityChanged(bool available);  // tray sohbet butonu
    void handRaiseSent          (const QString& note);

private slots:
    void onMessageSent          (const QString& roomId,    const QString& content);
    void onPrivateMessageSent   (const QString& sessionId, const QString& content);
    void onPrivateRequested     (const QString& peerId,    const QString& peerDisplay);
    void onPrivateAccepted      (const QString& requestId);
    void onPrivateRejected      (const QString& requestId);
    void onPrivateClosedByUser  (const QString& sessionId);
    void onHandRaiseRequested   (const QString& note);
    void onWindowClosed         ();

private:
    void handleRoomCreated   (const okidia::Envelope& env);
    void handleChatMessage   (const okidia::Envelope& env);
    void handleMemberJoined  (const okidia::Envelope& env);
    void handleMemberLeft    (const okidia::Envelope& env);
    void handlePrivateRequest(const okidia::Envelope& env);
    void handlePrivateAccept (const okidia::Envelope& env);
    void handlePrivateReject (const okidia::Envelope& env);
    void handlePrivateOpen   (const okidia::Envelope& env);
    void handlePrivateMessage(const okidia::Envelope& env);
    void handlePrivateClose  (const okidia::Envelope& env);
    void handleChatLockState (const okidia::Envelope& env);
    void handleChatWindowState(const okidia::Envelope& env);
    void sendWindowState(bool open);

    LocalConnection*  m_conn    = nullptr;
    AgentChatWindow*  m_window  = nullptr;

    QString m_myClientId;
    QString m_myHostname;
    QString m_myUsername;
    QString m_myDisplay;

    bool m_windowOpen = false;

    struct CachedRoom {
        AgentRoomInfo          info;
        QList<AgentMemberInfo> members;
    };
    QMap<QString, CachedRoom>   m_cachedRooms;
    QMap<QString, QString>      m_sessionToPeer;

    // classNodeId → {chatLocked, privateLocked}
    QMap<QString, QPair<bool,bool>> m_classLockStates;

    // E2EE Kripto Saklama Alanları
    QMap<QString, struct evp_pkey_st*> m_pendingEcdhKeys; // requestId -> ephemeral private key
    QMap<QString, QByteArray>          m_sessionKeys;     // sessionId -> derived key
    QMap<QString, QByteArray>          m_pendingPeerKeys; // requestId -> peer public key (B'nin isteği aldığında A'nın public key'ini saklaması için)
};
