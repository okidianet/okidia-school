// master/ssh/SshCredSelectorWidget.h
#pragma once

#include <QWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include "../session/SessionManager.h"
#include "SshClient.h"

class SshCredSelectorWidget : public QWidget {
    Q_OBJECT

public:
    explicit SshCredSelectorWidget(SessionManager* session, QWidget* parent = nullptr);
    ~SshCredSelectorWidget() override = default;

    QString username() const;
    QString secret() const;
    bool isPrivateKey() const;
    bool isSshAgent() const;
    QString currentCredentialId() const;

    SshClient::Config getConfig(const QString& host) const;

    void loadSettings(const QString& groupName);
    void saveSettings(const QString& groupName);
    void setReadOnly(bool readOnly);

public slots:
    void refreshCredentials();

signals:
    void configChanged();

private slots:
    void onCredComboChanged(int index);
    void onAuthComboChanged(int index);
    void onManageCredentials();
    void onBrowseKey();

private:
    void setupUi();

    SessionManager* m_session = nullptr;

    QComboBox*   m_credCombo = nullptr;
    QPushButton* m_manageCredsBtn = nullptr;
    QLineEdit*   m_userEdit = nullptr;
    QComboBox*   m_authCombo = nullptr;
    QLineEdit*   m_passEdit = nullptr;
    QLineEdit*   m_keyPathEdit = nullptr;
    QPushButton* m_keyBrowseBtn = nullptr;
    QWidget*     m_keyRowWidget = nullptr;
    QLabel*      m_passLabel = nullptr;
    QLabel*      m_keyLabel = nullptr;
};
