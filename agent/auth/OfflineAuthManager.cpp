// agent/auth/OfflineAuthManager.cpp
#include "OfflineAuthManager.h"
#include "../../common/crypto/KeyManager.h"
#include "../../common/utils/Logger.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QRandomGenerator>

static const QString kTeachersStorePath = QStringLiteral("/etc/okidia/teachers.dat");
static const qint64  kWindowDurationSeconds = 60; // Her QR 60 saniye geçerli

OfflineAuthManager& OfflineAuthManager::instance() {
    static OfflineAuthManager s_instance;
    return s_instance;
}

OfflineAuthManager::OfflineAuthManager(QObject* parent)
    : QObject(parent) {
    loadStore();
}

void OfflineAuthManager::loadStore() {
    QMutexLocker locker(&m_mutex);
    m_teachers.clear();

    QFile file(kTeachersStorePath);
    if (!file.exists()) {
        // Geliştirme ve saha testi için varsayılan bir demo öğretmen tohumu oluştur
        TeacherInfo defaultTeacher;
        defaultTeacher.teacherId = QStringLiteral("teacher-demo-01");
        defaultTeacher.displayName = QStringLiteral("Öğretmen (Demo)");
        defaultTeacher.username = QStringLiteral("ogretmen");
        // Sabit 32-byte demo seed
        defaultTeacher.seedBytes = QByteArray::fromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        m_teachers[defaultTeacher.teacherId] = defaultTeacher;
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        Logger::warning(QObject::tr("OfflineAuthManager: Öğretmen deposu açılamadı: ") + kTeachersStorePath);
        return;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray()) return;

    QJsonArray arr = doc.array();
    for (const QJsonValue& val : arr) {
        QJsonObject obj = val.toObject();
        TeacherInfo t;
        t.teacherId = obj.value(QStringLiteral("id")).toString();
        t.displayName = obj.value(QStringLiteral("name")).toString();
        t.username = obj.value(QStringLiteral("user")).toString();
        t.password = obj.value(QStringLiteral("password")).toString();
        t.seedBytes = QByteArray::fromHex(obj.value(QStringLiteral("seed")).toString().toUtf8());

        if (!t.teacherId.isEmpty() && !t.seedBytes.isEmpty()) {
            m_teachers[t.teacherId] = t;
        }
    }
    Logger::info(QObject::tr("OfflineAuthManager: %1 öğretmen tohumu yüklendi.").arg(m_teachers.size()));
}

void OfflineAuthManager::saveStore() {
    QMutexLocker locker(&m_mutex);
    QJsonArray arr;
    for (const TeacherInfo& t : m_teachers) {
        QJsonObject obj;
        obj[QStringLiteral("id")] = t.teacherId;
        obj[QStringLiteral("name")] = t.displayName;
        obj[QStringLiteral("user")] = t.username;
        obj[QStringLiteral("password")] = t.password;
        obj[QStringLiteral("seed")] = QString::fromUtf8(t.seedBytes.toHex());
        arr.append(obj);
    }

    QDir().mkpath(QStringLiteral("/etc/okidia"));
    QFile file(kTeachersStorePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        file.flush();
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    }
}

void OfflineAuthManager::upsertTeacher(const QString& teacherId, const QString& displayName,
                                      const QString& username, const QString& seedHex,
                                      const QString& password) {
    QMutexLocker locker(&m_mutex);
    TeacherInfo t;
    t.teacherId = teacherId;
    t.displayName = displayName;
    t.username = username.isEmpty() ? QStringLiteral("ogretmen") : username;
    t.password = password;
    t.seedBytes = QByteArray::fromHex(seedHex.toUtf8());

    m_teachers[teacherId] = t;
    locker.unlock();
    saveStore();
}

void OfflineAuthManager::removeTeacher(const QString& teacherId) {
    QMutexLocker locker(&m_mutex);
    m_teachers.remove(teacherId);
    locker.unlock();
    saveStore();
}

QList<TeacherInfo> OfflineAuthManager::allTeachers() const {
    QMutexLocker locker(&m_mutex);
    return m_teachers.values();
}

QString OfflineAuthManager::startLockSession(const QString& agentUuid) {
    QMutexLocker locker(&m_mutex);
    m_agentUuid = agentUuid;
    m_failedAttempts = 0;

    // 8-bayt random nonce
    quint64 r1 = QRandomGenerator::global()->generate64();
    m_currentNonce = QString::asprintf("%016llx", static_cast<unsigned long long>(r1));

    qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    m_currentWindow = nowSec / kWindowDurationSeconds;

    // QR formatı: OKIDIA_LOCK:<AGENT_UUID>:<WINDOW>:<NONCE>
    m_currentQrData = QStringLiteral("OKIDIA_LOCK:%1:%2:%3")
                          .arg(m_agentUuid)
                          .arg(m_currentWindow)
                          .arg(m_currentNonce);

    QString testPin = currentTestPin();
    int remSec = remainingSeconds();
    locker.unlock();

    emit lockSessionRefreshed(m_currentQrData, testPin, remSec);
    return m_currentQrData;
}

void OfflineAuthManager::endLockSession() {
    QMutexLocker locker(&m_mutex);
    m_currentNonce.clear();
    m_currentQrData.clear();
    m_failedAttempts = 0;
}

QString OfflineAuthManager::currentTestPin() const {
    if (m_teachers.isEmpty()) return QStringLiteral("123456");
    // İlk öğretmenin tohumunu kullanarak geçerli PIN'i hesapla
    TeacherInfo first = m_teachers.first();
    return KeyManager::generatePinFromSeed(first.seedBytes, m_currentNonce, m_currentWindow);
}

int OfflineAuthManager::remainingSeconds() const {
    qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    qint64 elapsedInWindow = nowSec % kWindowDurationSeconds;
    return static_cast<int>(kWindowDurationSeconds - elapsedInWindow);
}

bool OfflineAuthManager::verifyPin(const QString& enteredPin, QString* outUsername,
                                   QString* outDisplayName, QString* outPassword) {
    QMutexLocker locker(&m_mutex);
    QString cleanPin = enteredPin.trimmed();
    if (cleanPin.length() != 6) return false;

    // Brute force rate limiting (5 hatalı denemede nonce yenilenir)
    if (m_failedAttempts >= 5) {
        locker.unlock();
        startLockSession(m_agentUuid);
        emit authFailed(0);
        return false;
    }

    qint64 currentW = QDateTime::currentSecsSinceEpoch() / kWindowDurationSeconds;

    // Saat toleransı için hem o anki pencereyi hem de bir önceki pencereyi dene (±30s tolerans)
    QList<qint64> windows = { currentW, currentW - 1 };

    for (const TeacherInfo& teacher : m_teachers) {
        for (qint64 w : windows) {
            QString expectedPin = KeyManager::generatePinFromSeed(teacher.seedBytes, m_currentNonce, w);
            if (expectedPin == cleanPin) {
                Logger::info(QObject::tr("OfflineAuthManager: PIN doğrulandı! Öğretmen: ") + teacher.displayName);
                m_failedAttempts = 0;
                if (outUsername) *outUsername = teacher.username;
                if (outDisplayName) *outDisplayName = teacher.displayName;
                if (outPassword) *outPassword = teacher.password;
                locker.unlock();
                emit authSucceeded(teacher.username, teacher.displayName);
                return true;
            }
        }
    }

    m_failedAttempts++;
    int remaining = 5 - m_failedAttempts;
    Logger::warning(QObject::tr("OfflineAuthManager: Hatalı PIN girildi (%1). Kalan deneme: %2")
                    .arg(cleanPin).arg(remaining));
    locker.unlock();
    emit authFailed(remaining);
    return false;
}
