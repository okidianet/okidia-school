// broker/db/OkidiaDatabase.h
// Merkezi SQLite veritabanı — tüm kalıcı veri buradadır.
// Konum: /var/lib/okidia/okidia.db (üretim) | ~/.local/share/OKIDIA/okidia.db (geliştirme)
//
// Singleton. Tüm bileşenler OkidiaDatabase::instance() üzerinden erişir.
// ChatRoomManager ve AuditLog kendi query'leri için db() ile ham bağlantı alabilir.
#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QList>
#include <QString>
#include "../../common/db/DbRecords.h"

class OkidiaDatabase : public QObject {
    Q_OBJECT

public:
    static OkidiaDatabase& instance();

    // ── Yaşam döngüsü ────────────────────────────────────────────────────
    // path: tam dosya yolu. Klasör yoksa otomatik oluşturulur.
    bool open(const QString& path);
    void close();
    bool switchDatabase(const QString& newPath);
    bool isOpen() const;
    QString path() const { return m_path; }
    bool vacuumInto(const QString& targetPath);
    bool restoreFrom(const QString& sourcePath);

    // Ham bağlantı
    QSqlDatabase& db() { return m_db; }

    // ── Schema versiyonu ─────────────────────────────────────────────────
    int  schemaVersion() const;

    // ── Meta ─────────────────────────────────────────────────────────────
    QString metaValue(const QString& key) const;
    bool    setMetaValue(const QString& key, const QString& value);

    // ── Windows İstemci Kaldırma Koruması ─────────────────────────────────
    static const QString DEFAULT_UNINSTALL_PASS;      // "www.okidia.net"
    static const QString DEFAULT_UNINSTALL_PASS_HASH; // SHA-256 hash
    QString uninstallPasswordHash() const;
    bool    isDefaultUninstallPassword() const;
    bool    setUninstallPasswordHash(const QString& hash);

    // ── Agents ───────────────────────────────────────────────────────────
    bool            upsertAgent(const AgentRecord& agent);
    bool            removeAgent(const QString& machineId);
    AgentRecord     agent(const QString& machineId) const;
    QList<AgentRecord> allAgents() const;
    bool            updateAgentLastSeen(const QString& machineId,
                                        qint64 timestamp = 0);
    bool            updateAgentClass(const QString& machineId,
                                     const QString& classId);
    bool            updateAgentAlias(const QString& machineId,
                                     const QString& alias);
    bool            updateAgentType(const QString& machineId,
                                    int type);

    // ── Blacklist (agents table via status) ────────────────────────────────
    bool            addToBlacklist(const AgentRecord& rec);
    bool            removeFromBlacklist(const QString& machineId);
    bool            isBlacklisted(const QString& machineId) const;
    AgentRecord     blacklistedAgent(const QString& machineId) const;
    QList<AgentRecord> blacklist() const;

    // ── Users ─────────────────────────────────────────────────────────────
    bool            upsertUser(const UserRecord& user);
    bool            removeUser(const QString& userId);
    bool            setUserPassword(const QString& userId,
                                    const QString& hash,
                                    bool mustChange = false);
    bool            updateLastLogin(const QString& userId,
                                    const QString& version = QString(),
                                    const QString& osPrettyName = QString(),
                                    const QString& osTag = QString(),
                                    const QString& arch = QString());
    UserRecord      userById(const QString& userId) const;
    UserRecord      userByUsername(const QString& username) const;
    QList<UserRecord> allUsers() const;
    bool            setUserBackupReplica(const QString& userId,
                                         bool isReplica,
                                         const QString& replicaMachineId = QString(),
                                         const QString& replicaHostname = QString());
    bool            isUserBackupReplica(const QString& userId) const;
    bool            isReplicaDeviceAuthorized(const QString& userId,
                                              const QString& machineId,
                                              const QString& hostname) const;
    QList<QString>  backupReplicaUserIds() const;
    bool            demoteOtherYmUsers(const QString& exceptUserId);

    // ── Master Devices (Fiziksel Master Bilgisayarları) ───────────────────
    bool                 upsertMasterDevice(const MasterDeviceRecord& dev);
    MasterDeviceRecord   masterDeviceById(const QString& machineId) const;
    QList<MasterDeviceRecord> allMasterDevices() const;

    // ── Teachers (Akıllı Tahta Branş Öğretmenleri) ────────────────────────
    bool            upsertTeacher(const TeacherRecord& teacher);
    bool            removeTeacher(const QString& teacherId);
    TeacherRecord   teacherById(const QString& teacherId) const;
    TeacherRecord   teacherByUsername(const QString& username) const;
    QList<TeacherRecord> allTeachers() const;

    // ── Classes ───────────────────────────────────────────────────────────
    bool            upsertClass(const ClassRecord& cls);
    bool            removeClass(const QString& classId);
    ClassRecord     classById(const QString& classId) const;
    QList<ClassRecord> allClasses() const;
    QList<ClassRecord> childClasses(const QString& parentId) const;

    // ── User-Class permissions ────────────────────────────────────────────
    bool            grantPermission(const QString& userId,
                                    const QString& classId,
                                    const QString& permission = "full",
                                    const QString& grantedBy = {});
    bool            revokePermission(const QString& userId,
                                     const QString& classId);
    bool            revokeAllPermissions(const QString& userId);
    QList<UserClassRecord> permissionsForUser(const QString& userId) const;
    QStringList     classIdsForUser(const QString& userId) const;
    QStringList     userIdsForClass(const QString& classId) const;
    bool            isUserAuthorizedForClass(const QString& userId, const QString& classId) const;
    bool            isUserAuthorizedForClient(const QString& userId, const QString& clientId) const;

    // ── Delegations ───────────────────────────────────────────────────────
    bool            addDelegation(const DelegationRecord& del);
    bool            removeDelegation(const QString& delegationId);
    QList<DelegationRecord> activeDelegationsForUser(
                                const QString& toUserId) const;

    // ── Chat lock state ───────────────────────────────────────────────────
    bool            setChatLockState(const QString& classId,
                                     bool chatLocked, bool privateLocked);
    ChatLockRecord  chatLockState(const QString& classId) const;
    QList<ChatLockRecord> allChatLockStates() const;

    // ── Pending commands (offline agent için) ─────────────────────────────
    bool            addPendingCommand(const QString& machineId,
                                      int msgType,
                                      const QByteArray& payload,
                                      int ttlSeconds = 86400);  // default 24 saat
    QList<PendingCommandRecord> pendingCommands(const QString& machineId) const;
    bool            deletePendingCommand(qint64 id);
    void            purgePendingCommands();  // süresi dolmuş olanları sil
    // ── Broker (SB/MB kaydı) ─────────────────────────────────────────────────
    bool            upsertBroker(const BrokerRecord& broker);
    QList<BrokerRecord> allBrokers() const;

    // ── SSH Kimlikleri ───────────────────────────────────────────────────────
    bool            upsertSshCredential(const SshCredentialRecord& cred);
    bool            deleteSshCredential(const QString& id);
    SshCredentialRecord sshCredentialById(const QString& id) const;
    QList<SshCredentialRecord> sshCredentials(const QString& ownerUserId = {}) const; // Boş = sadece ortakları getir
    
    // ── Join Codes (OM Davet) ────────────────────────────────────────────────
    QString         createJoinCode(const QString& userId, int ttlHours = 24);
    JoinCodeRecord  validateJoinCode(const QString& code) const;
    bool            consumeJoinCode(const QString& code);
    void            purgeJoinCodes();

    // ── Özel Ajan Profilleri (Custom Profiles) ──────────────────────────────
    bool                       upsertCustomProfile(const CustomProfileRecord& prof);
    bool                       deleteCustomProfile(int id);
    CustomProfileRecord        customProfile(int id) const;
    QList<CustomProfileRecord> allCustomProfiles() const;
    int                        countAgentsWithCustomProfile(int profileId) const;

signals:
    void agentAdded(const QString& machineId);
    void agentRemoved(const QString& machineId);
    void agentUpdated(const QString& machineId);
    void blacklistChanged();
    void userChanged(const QString& userId);
    void teacherChanged(const QString& teacherId);
    void classChanged(const QString& classId);
    void customProfilesChanged();

private:
    explicit OkidiaDatabase(QObject* parent = nullptr);
    OkidiaDatabase(const OkidiaDatabase&)            = delete;
    OkidiaDatabase& operator=(const OkidiaDatabase&) = delete;

    bool createSchema();
    bool runMigrations();

    bool execQuery(const QString& sql) const;
    bool execQuery(QSqlQuery& q, const QString& context = {}) const;

    AgentRecord         rowToAgent(const QSqlQuery& q) const;
    UserRecord          rowToUser(const QSqlQuery& q) const;
    MasterDeviceRecord  rowToMasterDevice(const QSqlQuery& q) const;
    TeacherRecord       rowToTeacher(const QSqlQuery& q) const;
    ClassRecord         rowToClass(const QSqlQuery& q) const;
    UserClassRecord     rowToUserClass(const QSqlQuery& q) const;
    DelegationRecord    rowToDelegation(const QSqlQuery& q) const;
    CustomProfileRecord rowToCustomProfile(const QSqlQuery& q) const;

    QSqlDatabase m_db;
    QString      m_path;
    bool         m_open = false;

    static constexpr int kCurrentSchemaVersion = 1;
};
