#pragma once

#include "BrokerContext.h"
#include "../../common/network/Connection.h"
#include "messages.pb.h"

class AuthHandler {
public:
    explicit AuthHandler(BrokerContext* ctx);
    bool handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster);

    // Kilit sıfırlama yardımcıları
    void resetAccountLockout(const QString& username);
    void resetIpRateLimit(const QString& peerIp);

private:
    BrokerContext* m_ctx;

    struct IpAttemptInfo {
        int attempts = 0;
        qint64 blockedUntil = 0;
    };

    struct UserAttemptInfo {
        int attempts = 0;
        qint64 lockedUntil = 0;
    };

    QMap<QString, IpAttemptInfo>   m_failedIpAttempts;    // IP -> Deneme bilgisi
    QMap<QString, UserAttemptInfo> m_failedUserAttempts;  // Kullanıcı adı -> Deneme bilgisi

    static constexpr int MAX_IP_ATTEMPTS = 5;
    static constexpr qint64 IP_BLOCK_DURATION_SECS = 300; // 5 dakika

    static constexpr int MAX_USER_ATTEMPTS = 10;
    static constexpr qint64 USER_LOCK_DURATION_SECS = 900; // 15 dakika

    void cleanupExpiredLimits();
};

