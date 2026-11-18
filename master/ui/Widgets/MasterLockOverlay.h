// master/ui/Widgets/MasterLockOverlay.h
#pragma once

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include "../../../common/db/DbRecords.h"

class SessionManager;

class MasterLockOverlay : public QWidget {
    Q_OBJECT

public:
    explicit MasterLockOverlay(SessionManager* session, QWidget* parent = nullptr);
    ~MasterLockOverlay() override = default;

    void setUser(const UserRecord& user);
    void resetState();

signals:
    void unlocked();
    void logoutRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private slots:
    void onUnlockClicked();
    void onLogoutClicked();
    void onLoginResult(bool success, const QString& userId, const QString& role, const QString& displayName, const QString& errorMsg, bool mustChangePw);

private:
    SessionManager* m_session = nullptr;
    UserRecord m_user;
    bool m_waitingBroker = false;

    QFrame*      m_cardFrame    = nullptr;
    QLabel*      m_iconLabel    = nullptr;
    QLabel*      m_titleLabel   = nullptr;
    QLabel*      m_userLabel    = nullptr;
    QLineEdit*   m_passwordEdit = nullptr;
    QPushButton* m_unlockButton = nullptr;
    QPushButton* m_logoutButton = nullptr;
    QLabel*      m_errorLabel   = nullptr;
};
