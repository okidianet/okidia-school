// master/audit/AuditLog.h
#pragma once

#include <QObject>
#include <QDateTime>

struct AuditEvent {
    enum Category {
        Auth, Command, FileTransfer, Chat, System, Security, Power, Deployment, Exam
    };

    QDateTime timestamp;
    Category  category  = Command;
    QString   actor;    // display name (boşsa AuditLog::m_actor kullanılır)
    QString   userId;   // OKIDIA user UUID (boşsa m_userId kullanılır)
    QString   target;
    QString   detail;
};

class AuditLog : public QObject {
    Q_OBJECT

public:
    static AuditLog& instance();

    // Broker DB açıldıktan sonra çağrılır
    bool init(const QString& dbPath = {});

    // Login sonrası setlenir — sonraki tüm kayıtlarda kullanılır
    void setActiveUser(const QString& userId, const QString& username);

    QString dbPath() const { return {}; }  // uyumluluk

    void record(const AuditEvent& event);

    // Kısa yollar — actor/userId boş bırakılınca setActiveUser'dan alınır
    void recordCommand(const QString& actor,
                       const QString& target,
                       const QString& command);
    void recordAuth(const QString& actor,
                    const QString& detail);
    void recordFile(const QString& actor,
                    const QString& target,
                    const QString& filename);
    void recordPower(const QString& actor,
                     const QString& target,
                     const QString& action);
    void recordExam(const QString& actor,
                    const QString& target,
                    const QString& detail);
    void recordDeployment(const QString& actor,
                          const QString& target,
                          const QString& detail);
    void recordChat(const QString& actor,
                    const QString& target,
                    const QString& detail);
    void recordSecurity(const QString& actor,
                        const QString& target,
                        const QString& detail);

    QList<AuditEvent> query(const QDateTime& from,
                             const QDateTime& to,
                             AuditEvent::Category category
                                 = static_cast<AuditEvent::Category>(-1),
                             int limit = 500) const;

    bool exportCsv(const QString& filePath,
                   const QDateTime& from,
                   const QDateTime& to) const;

    void setSessionManager(class SessionManager* session);

    static QString categoryToString(AuditEvent::Category c);
    static AuditEvent::Category stringToCategory(const QString& s);
    static QString categoryDisplayName(AuditEvent::Category c);

private:
    explicit AuditLog(QObject* parent = nullptr);

    bool    m_ready    = false;
    QString m_actor;      // aktif kullanıcı display name
    QString m_userId;     // aktif kullanıcı UUID
    QString m_masterId;   // bu master'ın hostname'i
    class SessionManager* m_session = nullptr;
};

