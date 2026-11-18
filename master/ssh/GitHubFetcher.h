#ifndef GITHUB_FETCHER_H
#define GITHUB_FETCHER_H

#include <QDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QQueue>

class GitHubFetcher : public QDialog {
    Q_OBJECT
public:
    explicit GitHubFetcher(QWidget* parent = nullptr);
    ~GitHubFetcher();

    void showDialogAndFetch();

signals:
    void packageDownloaded(const QString& filePath);
    void packagesDownloaded(const QStringList& filePaths);

private slots:
    void onReleasesFetched(QNetworkReply* reply);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();
    void onDownloadButtonClicked();

private:
    void startNextDownload();
    QNetworkAccessManager* m_manager;
    QNetworkReply* m_currentReply;
    
    QListWidget* m_assetList;
    QPushButton* m_downloadBtn;
    QPushButton* m_cancelBtn;
    QProgressBar* m_progress;
    QLabel* m_statusLabel;
    QLineEdit* m_tokenInput;
    QPushButton* m_refreshBtn;

    struct AssetInfo {
        QString name;
        QString downloadUrl;
        QString apiUrl;
        qint64 size;
    };
    QList<AssetInfo> m_assets;
    
    QQueue<AssetInfo> m_downloadQueue;
    QStringList m_downloadedPaths;
    QString m_downloadPath;
};

#endif // GITHUB_FETCHER_H
