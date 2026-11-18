// master/ui/ReportIssueDialog/ReportIssueDialog.h
#pragma once

#include <QDialog>
#include <QComboBox>
#include <QTextEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class SessionManager;

/**
 * @brief Öğretmen ve sistem yöneticisinin Master üzerinden 3 tıkla
 * teknik hata ve geri bildirim paketi göndermesini sağlayan arayüz.
 */
class ReportIssueDialog : public QDialog {
    Q_OBJECT

public:
    explicit ReportIssueDialog(SessionManager* session, QWidget* parent = nullptr);
    ~ReportIssueDialog() override = default;

private slots:
    void onGonderClicked();
    void onMasaustuneKaydetClicked();
    void onUploadFinished();
    void onUploadProgress(qint64 bytesSent, qint64 bytesTotal);

private:
    void setupUi();
    bool preparePackage(QString& outZipPath, QString& outIncidentId);
    void setUiBusy(bool busy, const QString& statusText = QString());

    SessionManager*       m_session   = nullptr;
    QNetworkAccessManager m_netManager;
    QNetworkReply*        m_activeReply = nullptr;

    QComboBox*    m_categoryCombo     = nullptr;
    QComboBox*    m_timeWindowCombo   = nullptr;
    QTextEdit*    m_commentsEdit      = nullptr;
    QCheckBox*    m_chkScreenshot     = nullptr;

    QLabel*       m_statusLbl         = nullptr;
    QProgressBar* m_progressBar       = nullptr;
    QPushButton*  m_btnInspect        = nullptr;
    QPushButton*  m_btnSend           = nullptr;
    QPushButton*  m_btnCancel         = nullptr;

    QString       m_lastZipPath;
    QString       m_lastIncidentId;
};
