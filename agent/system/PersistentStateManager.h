#pragma once

#include <QString>
#include <QJsonObject>
#include <QStringList>

class PersistentStateManager {
public:
    static void setScreenLock(const QString& message, uint32_t lockPolicy = 0, qint64 ttlSecs = 10800); // 3 hours
    static void clearScreenLock();
    static bool isScreenLocked();
    static QString screenLockMessage();
    static uint32_t screenLockPolicy();

    static void setInputLock(qint64 ttlSecs = 10800);
    static void clearInputLock();
    static bool isInputLocked();

    static void setWebBlockAll(const QString& reason = QString(), qint64 ttlSecs = 10800);
    static void clearWebBlock();
    static bool hasWebBlock();
    static QJsonObject webBlockData(); // Returns JSON with "type" ("block_all"), "reason"

private:
    static QJsonObject loadState();
    static void saveState(const QJsonObject& obj);
    static bool isExpired(const QJsonObject& obj, const QString& key);
};
