// master/ui/CustomProfilesDialog/CustomProfilesDialog.h
#pragma once

#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include "../../../common/db/DbRecords.h"

class SessionManager;

class CustomProfilesDialog : public QDialog {
    Q_OBJECT

public:
    explicit CustomProfilesDialog(SessionManager* session, QWidget* parent = nullptr);
    ~CustomProfilesDialog() override = default;

private slots:
    void onProfileSelected(int row);
    void onAddNewClicked();
    void onSaveClicked();
    void onDeleteClicked();
    void refreshProfileList(int selectId = -1);

private:
    void setupUi();

    SessionManager*             m_session = nullptr;
    QList<CustomProfileRecord>  m_profiles;
    int                         m_currentProfileId = 0; // 0 = yeni ekleme

    QListWidget* m_listProfiles     = nullptr;
    QPushButton* m_btnAdd           = nullptr;
    QPushButton* m_btnDelete        = nullptr;

    QGroupBox*   m_detailGroup      = nullptr;
    QLineEdit*   m_txtName          = nullptr;
    QCheckBox*   m_chkPlayBell      = nullptr;
    QCheckBox*   m_chkRecessLock    = nullptr;
    QCheckBox*   m_chkChatEnabled   = nullptr;
    QLabel*      m_lblAssignedCount = nullptr;
    QLabel*      m_lblStatus        = nullptr;
    QPushButton* m_btnSave          = nullptr;
};

