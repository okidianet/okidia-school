// master/ssh/SshCredentialDialog.h
#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include "../session/SessionManager.h"
#include "../../common/db/DbRecords.h"
#include <QLabel>

class SshCredentialDialog : public QDialog {
    Q_OBJECT

public:
    explicit SshCredentialDialog(SessionManager* session, QWidget* parent = nullptr);

signals:
    // Bir kimlik başarıyla kaydedildiğinde emit edilir (template/class türleri)
    void credentialSaved(const QString& credId);

private slots:
    void onAdd();
    void onDelete();
    void onSelectionChanged();
    void onSave();
    void refreshList();
    void onKeyToggled(bool checked);
    void onBrowseKey();
    void onBulkDelete();
    void onDetailedSelectionChanged();

private:
    void setupUi();

    SessionManager* m_session;
    
    QTabWidget* m_tabWidget;
    QWidget* m_tabGlobal;
    QWidget* m_tabDetailed;

    QTableWidget*   m_table;
    QTableWidget*   m_detailedTable;
    
    // Editor fields
    QLineEdit* m_nameEdit;
    QLineEdit* m_userEdit;
    QLineEdit* m_secretEdit;
    QCheckBox* m_isKeyCheck;
    QCheckBox* m_isSharedCheck;
    
    QPushButton* m_addBtn;
    QPushButton* m_saveBtn;
    QPushButton* m_deleteBtn;
    QPushButton* m_browseBtn;
    QPushButton* m_bulkDeleteBtn;
    QPushButton* m_editBtn;
    QLabel*      m_secretLabel;
    QLabel*      m_targetIdLabel;
    QLineEdit*   m_targetIdEdit;
    
    QString m_currentId;
    QString m_currentTargetId;
    QString m_currentTargetType;
    QString m_currentMachineId;
};
