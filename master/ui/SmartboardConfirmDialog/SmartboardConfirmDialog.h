// master/ui/SmartboardConfirmDialog/SmartboardConfirmDialog.h
#pragma once

#include <QDialog>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QMap>

/**
 * Akıllı Tahta yayın hedefi onaylama diyaloğu.
 * Öğretmenin yanlışlıkla başka sınıftaki akıllı tahtaya yayın yapmasını
 * önlemek amacıyla tahtaların listesini checkbox ile onaylatır.
 */
class SmartboardConfirmDialog : public QDialog {
    Q_OBJECT

public:
    // boards: clientId -> hostname/displayName map
    explicit SmartboardConfirmDialog(const QMap<QString, QString>& boards, QWidget* parent = nullptr);

    // Seçili bırakılan (işaretli) clientId listesi
    QStringList selectedClientIds() const;

private slots:
    void onSelectAll(bool checked);

private:
    void setupUi(int count);

    QMap<QString, QString> m_boards;
    QListWidget*           m_listWidget = nullptr;
    QPushButton*           m_btnConfirm = nullptr;
    QPushButton*           m_btnCancel  = nullptr;
};
