#ifndef DOWNLOAD_MANAGER_DIALOG_H
#define DOWNLOAD_MANAGER_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QRadioButton>
#include <QButtonGroup>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QQueue>
#include "../network/ReleaseManager.h"
#include "../ssh/PackageRequirementSolver.h"

class DownloadManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadManagerDialog(
        const QList<PackageRequirement>& initialRequirements,
        QWidget* parent = nullptr
    );
    ~DownloadManagerDialog() override;

    QStringList downloadedFiles() const { return m_downloadedFiles; }

signals:
    void packagesReady(const QStringList& filePaths);

private slots:
    void onRefreshClicked();
    void onStartDownloadClicked();
    void onCancelClicked();
    void onReleasesFetched(const QList<ReleaseItem>& releases, const QList<ReleaseCategory>& categories);
    void onFetchFailed(const QString& errorMessage);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();

private:
    void setupUi();
    void updateTable();
    void startNextInQueue();
    void processRequirementDownloaded(int reqIndex, const QString& tempFilePath);

    QList<PackageRequirement> m_requirements;
    ReleaseManager* m_releaseManager;
    QNetworkAccessManager* m_downloadNetManager;
    QNetworkReply* m_currentReply = nullptr;
    QFile* m_currentOutputFile = nullptr;

    // UI bileşenleri
    QRadioButton* m_radioOkidiaNet = nullptr;
    QRadioButton* m_radioGitHub = nullptr;
    QButtonGroup* m_sourceGroup = nullptr;

    QTableWidget* m_table = nullptr;
    QLabel* m_statusLabel = nullptr;
    QProgressBar* m_overallProgress = nullptr;
    QPushButton* m_startBtn = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;

    // İndirme kuyruğu
    QQueue<int> m_downloadQueue;
    int m_currentQueueIndex = -1;
    QStringList m_downloadedFiles;
};

#endif // DOWNLOAD_MANAGER_DIALOG_H
