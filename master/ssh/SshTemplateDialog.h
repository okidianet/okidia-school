#pragma once
#include <QDialog>
#include <QTableWidget>
#include <QPushButton>

class SshTemplateDialog : public QDialog {
    Q_OBJECT
public:
    explicit SshTemplateDialog(QWidget* parent = nullptr);

signals:
    void templatesChanged();

private:
    void loadTemplates();
    void saveTemplates();
    void onAdd();
    void onEdit();
    void onDelete();
    void onExportTemplates();
    void onImportTemplates();

    QTableWidget* m_table;
    QPushButton* m_addBtn;
    QPushButton* m_editBtn;
    QPushButton* m_delBtn;
    QPushButton* m_exportBtn;
    QPushButton* m_importBtn;
    QPushButton* m_refreshBtn;
    QPushButton* m_closeBtn;
};
