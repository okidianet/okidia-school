#pragma once

#include "BrokerContext.h"
#include "../../common/network/Connection.h"
#include "messages.pb.h"
#include <QHash>
#include <QList>
#include <QString>

class ChatHandler {
public:
    explicit ChatHandler(BrokerContext* ctx);
    bool handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster);
    void syncMuteStateForClient(const QString& clientId);

private:
    struct SpamTracker {
        QList<qint64> recentTimestamps;
        QString       lastNormalizedMsg;
        int           consecutiveSameCount = 0;
        qint64        lastSameMsgAt        = 0;
        int           violationLevel       = 0; // 0, 1 (1 dk), 2 (5 dk), 3 (10 dk)
        qint64        lastViolationAt      = 0;
        qint64        mutedUntil           = 0;
        QString       muteReason;
    };

    static QString makeUserKey(const QString& machineId, const QString& username);
    static QString formatStudentDisplayName(const QString& userDisplayName,
                                            const QString& username,
                                            const QString& hostname,
                                            const QString& fallbackId);
    void sendMuteState(const QString& clientId,
                       const QString& username,
                       bool muted,
                       qint64 mutedUntil,
                       int durationSecs,
                       int violationLevel,
                       const QString& reason);

    BrokerContext* m_ctx;
    QHash<QString, SpamTracker> m_spamTrackers;   // "<machineId>:<username>" -> state
    QHash<QString, qint64>      m_lastHandRaise;  // "<machineId>:<username>" -> unix secs
};
