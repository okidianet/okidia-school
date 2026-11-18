// master/ui/Widgets/MasterUpdateDialog.h
#pragma once

#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

class SessionManager;

class MasterUpdateDialog : public QDialog {
    Q_OBJECT

public:
    explicit MasterUpdateDialog(SessionManager* session,
                                const QString& newVersion,
                                const QString& filename,
                                quint64 fileSize,
                                const QString& osTag,
                                const QString& osArch,
                                QWidget* parent = nullptr);
    ~MasterUpdateDialog() override = default;

private slots:
    void onStartDownloadClicked();
    void onDownloadProgress(quint64 received, quint64 total, int percent);
    void onDownloadFinished(bool success, const QString& pathOrError);

private:
    void setupUi();

    SessionManager* m_session;
    QString m_newVersion;
    QString m_filename;
    quint64 m_fileSize;
    QString m_osTag;
    QString m_osArch;
    QString m_downloadedDebPath;

    QLabel* m_infoLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QPushButton* m_actionBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
};
