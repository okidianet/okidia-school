// master/ui/UserManagementPanel/UserManagementPanel.h
#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>

#include "../../../common/db/OkidiaDatabase.h"
#include "../../../common/db/DbRecords.h"

class SessionManager;

class UserManagementPanel : public QWidget {
    Q_OBJECT
public:
    explicit UserManagementPanel(SessionManager* session, QWidget* parent = nullptr);

    // Paneli yenile (dışarıdan tetiklenebilir)
    void refresh();

    // Aktif kullanıcıya göre panel yetkilerini ayarla
    // OM: Kullanıcı Ekle/Sil gizlenir, sadece kendi şifresini değiştirebilir
    void setActiveUser(const UserRecord& user);
    
signals:
    void globalContextMenuRequested(const QPoint& pos);

private slots:
    // Tab 1: Yöneticiler
    void onAddUser();
    void onEditUser();
    void onDeleteUser();
    void onResetPassword();
    void onGenerateJoinCode();
    void onGenerateRecoveryKey();
    void onToggleReplica();
    void onUserSelectionChanged();
    void onUserContextMenu(const QPoint& pos);

    // Tab 2: Öğretmenler (Akıllı Tahta Branş Öğretmenleri)
    void onAddTeacher();
    void onEditTeacher();
    void onDeleteTeacher();
    void onShowTeacherMobilePairQr();
    void onSyncTeachersToSmartboards();
    void onTeacherSelectionChanged();
    void onTeacherContextMenu(const QPoint& pos);

    // Tab 3: Sınıf atamaları
    void onGrantPermission();
    void onRevokePermission();
    void onUserForAssignChanged(int index);
    void onClassListContextMenu(const QPoint& pos);
    void onAssignListContextMenu(const QPoint& pos);

private:
    void setupUserTab();
    void setupTeacherTab();
    void setupAssignTab();
    void loadUsers();
    void loadTeachers();
    void loadClasses();
    void loadAssignments(const QString& userId);

    QString selectedUserId() const;
    QString selectedTeacherId() const;

    // Tab 1: Yöneticiler
    QTableWidget*  m_userTable     = nullptr;
    QPushButton*   m_btnAdd        = nullptr;
    QPushButton*   m_btnEdit       = nullptr;
    QPushButton*   m_btnReset      = nullptr;
    QPushButton*   m_btnDelete     = nullptr;
    QPushButton*   m_btnJoinCode   = nullptr;
    QPushButton*   m_btnRecovery   = nullptr;
    QPushButton*   m_btnReplica    = nullptr;
    QWidget*       m_replicaNoticeWidget = nullptr;
    QLabel*        m_replicaNoticeLabel  = nullptr;

    // Tab 2: Öğretmenler
    QTableWidget*  m_teacherTable       = nullptr;
    QPushButton*   m_btnTeacherAdd      = nullptr;
    QPushButton*   m_btnTeacherEdit     = nullptr;
    QPushButton*   m_btnTeacherDelete   = nullptr;
    QPushButton*   m_btnTeacherMobileQr = nullptr;
    QPushButton*   m_btnTeacherSync     = nullptr;

    // Tab 3: Sınıf Atamaları
    QComboBox*     m_userCombo   = nullptr;
    QListWidget*   m_classList   = nullptr;
    QListWidget*   m_assignList  = nullptr;
    QPushButton*   m_btnGrant    = nullptr;
    QPushButton*   m_btnRevoke   = nullptr;

    QList<UserRecord>    m_users;
    QList<TeacherRecord> m_teachers;
    QList<ClassRecord>   m_classes;
    UserRecord           m_activeUser;  // giriş yapan kullanıcı
    SessionManager*      m_session = nullptr;
};
