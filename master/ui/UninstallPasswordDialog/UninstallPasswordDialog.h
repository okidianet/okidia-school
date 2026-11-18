// master/ui/UninstallPasswordDialog/UninstallPasswordDialog.h
#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>

class UninstallPasswordDialog : public QDialog {
    Q_OBJECT
public:
    explicit UninstallPasswordDialog(QWidget* parent = nullptr);

    QString passwordHash() const { return m_passwordHash; }
    bool isResetToDefault() const { return m_isResetToDefault; }

private slots:
    void onSaveClicked();
    void onResetDefaultClicked();
    void onToggleShowPassword(bool show);

private:
    void setupUi();
    void updateStatusIndicator();

    QLineEdit*   m_txtNewPassword      = nullptr;
    QLineEdit*   m_txtConfirmPassword  = nullptr;
    QCheckBox*   m_chkShowPassword     = nullptr;
    QLabel*      m_lblStatusIcon       = nullptr;
    QLabel*      m_lblStatusText       = nullptr;
    QPushButton* m_btnSave             = nullptr;
    QPushButton* m_btnResetDefault     = nullptr;
    QPushButton* m_btnCancel           = nullptr;

    QString      m_passwordHash;
    bool         m_isResetToDefault    = false;
};
