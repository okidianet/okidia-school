#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <QSet>
#include <QPair>
#include <functional>
#include <memory>

#include "../ClientRegistry.h"
#include "../MulticastManager.h"
#include "../BrokerTransferManager.h"
#include "../EnrollmentManager.h"
#include "messages.pb.h"

class BellScheduler;

struct ChatBatchItem {
    QString id;
    QString roomId;
    QString fromHost;
    int     senderRole;
    QString content;
    qint64  timestamp;
    bool    isSystem;
};

struct BrokerContext {
    ClientRegistry* registry = nullptr;
    MulticastManager* multicast = nullptr;
    BrokerTransferManager* transferManager = nullptr;
    EnrollmentManager* enrollmentManager = nullptr;
    BellScheduler* bellScheduler = nullptr;

    
    std::function<void()> broadcastStatus;
    std::function<bool(quint16)> changeBrokerPort;
    std::function<bool(const QString&, quint16)> changeBrokerConfig;
    std::function<void()> purgeLogic;
    
    // YM/OM checks
    std::function<bool(Connection*)> isRequesterYM;
    std::function<bool(Connection*)> isRequesterYMOrOM;
    
    // Broadcast helpers
    std::function<void(const QString&, const QString&)> broadcastAgentLeftRoom;
    std::function<void(const QString&, const QString&)> broadcastAgentJoinedRoom;
    std::function<void(const QString&)> broadcastRoomClosed;
    std::function<void()> broadcastDbSyncSignal;
    std::function<void(const QString&)> pushLatestBackupToUser;
    std::function<void(const QString&)> reconcileAgentState;
    
    QString setupCode;
    
    QList<ChatBatchItem>* chatBatch = nullptr;
    QMap<QString, QMap<QString, okidia::ChatMemberEvent>>* activeRoomMembers = nullptr;
    QMap<Connection*, QSet<QPair<QString, QString>>>* connToRoomMembers = nullptr;
    QMap<QString, bool>* agentWindowOpenMap = nullptr;
};
