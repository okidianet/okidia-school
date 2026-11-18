// master/chat/PrivateChatManager.h
// Özel sohbet yönetimi
//
// Kural özeti:
//   - YM/OM → herhangi birine onaysız açar
//   - Agent → agent/master'a istek gönderir, onay gerekir
//   - Pencere kapanınca sohbet sonlanır, geçmiş görüntülenemez
//   - Mesajlar bellekte tutulur (şifreli depolama sonraya)
//   - Ret durumunda: 1dk → 5dk → 40dk bekleme
//   - Broker'a sadece meta log gönderilir (private_chat_log)

#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QDateTime>

class SessionManager;

struct PrivateMessage {
    QString id;
    QString content;
    qint64  timestamp = 0;
    bool    fromMe    = false;
};

struct PrivateSession {
    QString id;              // oturum UUID
    QString peerId;          // karşı tarafın clientId/machineId
    QString peerDisplay;     // gösterim adı
    QString peerHost;
    QString peerUser;
    bool    isOpen    = false;
    bool    isMaster  = false; // karşı taraf master mı (YM/OM)
    QList<PrivateMessage> messages;
    qint64  startedAt = 0;
};

struct PendingRequest {
    QString requestId;
    QString fromId;
    QString fromDisplay;
    QString fromHost;
    qint64  sentAt = 0;
};

class PrivateChatManager : public QObject {
    Q_OBJECT
public:
    explicit PrivateChatManager(SessionManager* session, QObject* parent = nullptr);
    ~PrivateChatManager() override = default;

    // ── Sorgular ──────────────────────────────────────────────────────────────
    QList<PrivateSession*> activeSessions() const;
    QList<PendingRequest>  pendingRequests() const;
    PrivateSession*        session(const QString& sessionId) const;
    QString                sessionIdForPeer(const QString& peerId) const;

    // ── Master bilgisi ────────────────────────────────────────────────────────
    void setMasterInfo(const QString& username,
                       const QString& displayName,
                       const QString& role);
    QString masterName() const { return m_masterDisplayName; }
    QString masterUsername() const { return m_masterUsername; }
    QString masterRole() const { return m_masterRole; }

    // ── YM/OM: onaysız başlat ─────────────────────────────────────────────────
    void openDirect(const QString& peerId, const QString& peerDisplay,
                    const QString& peerHost, const QString& peerUser);

    // ── Agent → istek gönder ──────────────────────────────────────────────────
    // Daha önce reddedilmişse bekleme süresi kontrolü yapılır
    bool sendRequest(const QString& peerId, const QString& peerDisplay);

    // ── İstek yanıtı ─────────────────────────────────────────────────────────
    void acceptRequest(const QString& requestId);
    void rejectRequest(const QString& requestId);

    // ── Mesaj gönder ──────────────────────────────────────────────────────────
    void sendMessage(const QString& sessionId, const QString& content);
    void appendSystemMessage(const QString& sessionId, const QString& content);

    // ── Oturumu kapat ────────────────────────────────────────────────────────
    void closeSession(const QString& sessionId);

    // ── Gelen proto mesajları (SessionManager'dan) ────────────────────────────
    void onIncomingRequest(const QString& fromId, const QString& fromDisplay,
                           const QString& fromHost, const QString& fromUser,
                           const QString& requestId);
    void onIncomingOpen(const QString& sessionId, const QString& fromId,
                        const QString& fromDisplay, const QString& fromHost);
    void onRequestAccepted(const QString& requestId, const QString& sessionId);
    void onRequestRejected(const QString& requestId);
    void onIncomingMessage(const QString& sessionId, const QString& content,
                           qint64 timestamp);
    void onSessionClosed  (const QString& sessionId);

    // Ret bekleme süresi (dk): 1 → 5 → 40
    int   cooldownMinutes(const QString& peerId) const;
    qint64 cooldownEndsAt(const QString& peerId) const;

signals:
    void requestReceived (const PendingRequest& req);  // tray bildirimi
    void requestExpired  (const QString& requestId);
    void sessionOpened   (const QString& sessionId);
    void sessionClosed   (const QString& sessionId);
    void messageReceived (const QString& sessionId, const PrivateMessage& msg);
    void cooldownChanged (const QString& peerId, qint64 endsAt);

private:
    SessionManager* m_session;

    QMap<QString, PrivateSession*> m_sessions;   // sessionId → session
    QList<PendingRequest>          m_pending;

    // Ret geçmişi: peerId → {retAt, retCount}
    struct RejectRecord { qint64 lastAt; int count; };
    QMap<QString, RejectRecord> m_rejectHistory;

    QString m_masterUsername;
    QString m_masterDisplayName;
    QString m_masterRole;

    QString newId() const;
};
