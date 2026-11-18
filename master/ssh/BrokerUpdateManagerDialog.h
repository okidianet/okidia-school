#ifndef BROKER_UPDATE_MANAGER_DIALOG_H
#define BROKER_UPDATE_MANAGER_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "../session/SessionManager.h"

class BrokerUpdateManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit BrokerUpdateManagerDialog(SessionManager* session, QWidget* parent = nullptr);

private slots:
    void onBrokerUpdatesListReceived(const QVariantList& packages);
    void onBrokerUpdatesDeleteResult(bool success, const QString& errorMsg, const QStringList& deletedFiles);
    
    void onRefreshClicked();
    void onDeleteSelectedClicked();
    void onDeleteAllClicked();
    void onKeepLatestOnlyClicked();
    void onSelectionChanged();

private:
    SessionManager* m_session;
    QTableWidget*   m_table;
    QPushButton*    m_refreshBtn;
    QPushButton*    m_deleteSelectedBtn;
    QPushButton*    m_deleteAllBtn;
    QPushButton*    m_keepLatestBtn;
    QLabel*         m_statusLabel;
    
    void requestList();
    void updateButtons();
    QString formatSize(qulonglong size) const;
    QString formatTime(qulonglong ts) const;
};

#endif // BROKER_UPDATE_MANAGER_DIALOG_H
