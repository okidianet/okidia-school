#include "PersistentStateManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/utils/Logger.h"
#include "../../common/OkidiaConfig.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QObject>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif
#else
#  include <unistd.h>
#  include <fcntl.h>
#endif

static QString stateFilePath() {
    return OkidiaConfig::basePath() + "/agent_state.json";
}

QJsonObject PersistentStateManager::loadState() {
    QFile file(stateFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    QByteArray data = file.readAll();
    file.close();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        return QJsonObject();
    }
    return doc.object();
}

void PersistentStateManager::saveState(const QJsonObject& obj) {
    const QString baseDir = OkidiaConfig::basePath();
    QDir().mkpath(baseDir);
    const QString finalPath = stateFilePath();
    const QString tmpPath = finalPath + ".tmp";

    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("PersistentStateManager: Durum dosyası yazılamadı: %1").arg(tmpPath));
        return;
    }
    QJsonDocument doc(obj);
    file.write(doc.toJson());
    file.flush();
    int fd = file.handle();
    if (fd != -1) {
#if !defined(Q_OS_WIN)
        fsync(fd);
#endif
    }
    file.close();
    
#if defined(Q_OS_WIN)
    QFile::remove(finalPath);
    if (!QFile::rename(tmpPath, finalPath)) {
        Logger::error(QObject::tr("PersistentStateManager: Dosya yeniden adlandırma başarısız!"));
        return;
    }
#else
    if (::rename(tmpPath.toUtf8().constData(), finalPath.toUtf8().constData()) != 0) {
        Logger::error(QObject::tr("PersistentStateManager: rename() başarısız!"));
        return;
    }

    int dirfd = ::open(baseDir.toUtf8().constData(), O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        fsync(dirfd);
        ::close(dirfd);
    }
#endif
}

bool PersistentStateManager::isExpired(const QJsonObject& obj, const QString& key) {
    if (!obj.contains(key)) return true;
    QJsonObject data = obj.value(key).toObject();
    if (!data.contains("expires_at")) return false; // süresiz
    qint64 expiresAt = static_cast<qint64>(data.value("expires_at").toDouble());
    return QDateTime::currentSecsSinceEpoch() > expiresAt;
}

void PersistentStateManager::setScreenLock(const QString& message, uint32_t lockPolicy, qint64 ttlSecs) {
    QJsonObject state = loadState();
    QJsonObject lockObj;
    lockObj["message"] = message;
    lockObj["policy"] = static_cast<double>(lockPolicy);
    if (ttlSecs > 0) {
        lockObj["expires_at"] = static_cast<double>(QDateTime::currentSecsSinceEpoch() + ttlSecs);
    }
    state["screen_lock"] = lockObj;
    saveState(state);
}

void PersistentStateManager::clearScreenLock() {
    QJsonObject state = loadState();
    state.remove("screen_lock");
    saveState(state);
}

bool PersistentStateManager::isScreenLocked() {
    QJsonObject state = loadState();
    if (isExpired(state, "screen_lock")) {
        if (state.contains("screen_lock")) clearScreenLock();
        return false;
    }
    return true;
}

QString PersistentStateManager::screenLockMessage() {
    QJsonObject state = loadState();
    if (isExpired(state, "screen_lock")) {
        if (state.contains("screen_lock")) clearScreenLock();
        return QString();
    }
    return state.value("screen_lock").toObject().value("message").toString();
}

uint32_t PersistentStateManager::screenLockPolicy() {
    QJsonObject state = loadState();
    if (isExpired(state, "screen_lock")) {
        if (state.contains("screen_lock")) clearScreenLock();
        return 0;
    }
    return static_cast<uint32_t>(state.value("screen_lock").toObject().value("policy").toDouble());
}

void PersistentStateManager::setInputLock(qint64 ttlSecs) {
    QJsonObject state = loadState();
    QJsonObject lockObj;
    if (ttlSecs > 0) {
        lockObj["expires_at"] = static_cast<double>(QDateTime::currentSecsSinceEpoch() + ttlSecs);
    }
    state["input_lock"] = lockObj;
    saveState(state);
}

void PersistentStateManager::clearInputLock() {
    QJsonObject state = loadState();
    state.remove("input_lock");
    saveState(state);
}

bool PersistentStateManager::isInputLocked() {
    QJsonObject state = loadState();
    if (isExpired(state, "input_lock")) {
        if (state.contains("input_lock")) clearInputLock();
        return false;
    }
    return true;
}

void PersistentStateManager::setWebBlockAll(const QString& reason, qint64 ttlSecs) {
    QJsonObject state = loadState();
    QJsonObject webObj;
    webObj["type"] = "block_all";
    webObj["reason"] = reason;
    if (ttlSecs > 0) {
        webObj["expires_at"] = static_cast<double>(QDateTime::currentSecsSinceEpoch() + ttlSecs);
    }
    state["web_block"] = webObj;
    saveState(state);
}

void PersistentStateManager::clearWebBlock() {
    QJsonObject state = loadState();
    state.remove("web_block");
    saveState(state);
}

bool PersistentStateManager::hasWebBlock() {
    QJsonObject state = loadState();
    if (isExpired(state, "web_block")) {
        if (state.contains("web_block")) clearWebBlock();
        return false;
    }
    return true;
}

QJsonObject PersistentStateManager::webBlockData() {
    QJsonObject state = loadState();
    if (isExpired(state, "web_block")) {
        if (state.contains("web_block")) clearWebBlock();
        return QJsonObject();
    }
    return state.value("web_block").toObject();
}
