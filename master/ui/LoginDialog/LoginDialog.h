// master/ui/LoginDialog/LoginDialog.h
#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QInputDialog>
#include <QPushButton>
#include <QLabel>
#include <QStackedWidget>

#include "../../../common/db/OkidiaDatabase.h"
#include "../../../common/db/DbRecords.h"

class SessionManager;

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);
    void setSessionManager(SessionManager* session);

    // Giriş yapan kullanıcı (accept() sonrası dolu)
    UserRecord loggedInUser() const { return m_user; }
    bool rememberMe() const;
    bool isOfflineSshMode() const { return m_isOfflineSshMode; }

    void setError(const QString& msg);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onLogin();
    void onChangePassword();
    void onForgotPassword();
    void onNetworkSettingsClicked();

private:
    void showChangePasswordPage();
    void showLoginPage();
    void updateSchoolLabel();

    // Login sayfası
    QWidget*     m_loginPage     = nullptr;
    QLabel*      m_schoolLabel   = nullptr;
    QLineEdit*   m_usernameEdit  = nullptr;
    QLineEdit*   m_passwordEdit  = nullptr;
    QLabel*      m_errorLabel    = nullptr;
    QCheckBox*   m_rememberMe    = nullptr;
    QPushButton* m_btnLogin      = nullptr;

    // Şifre değişim sayfası
    QWidget*     m_changePwPage  = nullptr;
    QLineEdit*   m_newPwEdit     = nullptr;
    QLineEdit*   m_newPw2Edit    = nullptr;
    QLabel*      m_changePwError = nullptr;
    QPushButton* m_btnChangePw   = nullptr;

    QStackedWidget* m_stack = nullptr;

    UserRecord m_user;  // doğrulanan kullanıcı
    bool m_isOfflineSshMode = false;
    QPushButton* m_btnOfflineSsh = nullptr;
    SessionManager* m_session = nullptr;
    QString m_pendingPasswordHash;

    QTimer* m_loginTimeoutTimer = nullptr;
    void startLoginTimeout();
    void cancelLoginTimeout();
};

// ─────────────────────────────────────────────────────────────────────────────
// İki Aşamalı Şifre Kurtarma Dialogu
// ─────────────────────────────────────────────────────────────────────────────
class ForgotPasswordDialog : public QDialog {
    Q_OBJECT
public:
    explicit ForgotPasswordDialog(SessionManager* session, QWidget* parent = nullptr);
    ~ForgotPasswordDialog() override = default;

private slots:
    void onVerifyClicked();
    void onSaveClicked();
    void onResetResult(bool success, const QString& errorMsg);
    void onCodeTextChanged(const QString& text);

private:
    SessionManager* m_session = nullptr;
    QStackedWidget* m_stack = nullptr;

    // Aşama 1: Kurtarma Kodu Doğrulama
    QWidget*     m_verifyPage = nullptr;
    QLineEdit*   m_codeEdit = nullptr;
    QPushButton* m_btnVerify = nullptr;
    QLabel*      m_verifyErrorLabel = nullptr;

    // Aşama 2: Yeni Şifre Belirleme
    QWidget*     m_resetPage = nullptr;
    QLineEdit*   m_newPwEdit = nullptr;
    QLineEdit*   m_newPwConfirmEdit = nullptr;
    QPushButton* m_btnSave = nullptr;
    QLabel*      m_resetErrorLabel = nullptr;

    QString m_verifiedKey;
    bool m_isVerifying = false;
};
