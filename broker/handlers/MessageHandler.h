#pragma once

#include "BrokerContext.h"
#include "../../common/network/Connection.h"
#include "messages.pb.h"
#include <memory>

class AuthHandler;
class ClientHandler;
class ClassHandler;
class ChatHandler;
class SystemHandler;

class MessageHandler {
public:
    explicit MessageHandler(BrokerContext* ctx);
    ~MessageHandler();

    void process(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster);
    void reconcileAgentState(const QString& clientId);

private:
    struct ActiveBroadcastSession {
        QString masterUserId;
        QString masterDisplayName;
        QStringList targetClientIds; // Boş ise tüm okul / sınıf
        okidia::Envelope lastBeginEnvelope;
        okidia::Envelope lastFrameEnvelope;
    };

    bool checkAuthorization(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool isMaster);
    bool isAuthorizedForMasterMessage(const okidia::Envelope& env, Connection* conn, const QString& role);
    bool isAllowedAgentMessageType(okidia::MessageType type) const;

    BrokerContext* m_ctx;
    std::unique_ptr<AuthHandler> m_authHandler;
    std::unique_ptr<ClientHandler> m_clientHandler;
    std::unique_ptr<ClassHandler> m_classHandler;
    std::unique_ptr<ChatHandler> m_chatHandler;
    std::unique_ptr<SystemHandler> m_systemHandler;

    // MasterUserId -> Aktif Ekran Yayını Oturumu
    QMap<QString, ActiveBroadcastSession> m_activeBroadcastSessions;
};
