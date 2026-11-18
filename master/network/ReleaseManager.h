#ifndef RELEASE_MANAGER_H
#define RELEASE_MANAGER_H

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>

struct ReleaseCategory {
    QString id;
    QString name;
    QString os;
    QString status;
};

struct ReleaseItem {
    int id = 0;
    QString version;
    QString category;      // "ubuntu24", "debian12", "windows" etc.
    QString component;     // "okidia-agent", "okidia-master", "okidia-broker"
    QString arch;          // "amd64", "arm64", "x64"
    QString filename;
    QString url;           // Relative e.g. "downloads/ubuntu24/..." or absolute
    QString sha256;
    qint64 sizeBytes = 0;
    QString sizeHuman;
    QString releaseDate;
    bool isRegistered = true;

    QString downloadUrl(const QString& baseUrl = "https://okidia.net/") const;
};

class ReleaseManager : public QObject {
    Q_OBJECT
public:
    explicit ReleaseManager(QObject* parent = nullptr);
    ~ReleaseManager() override = default;

    void setBaseUrl(const QString& url);
    QString baseUrl() const { return m_baseUrl; }

    void fetchReleases();

    QList<ReleaseCategory> categories() const { return m_categories; }
    QList<ReleaseItem> allReleases() const { return m_releases; }

    // Çözümleme fonksiyonları
    ReleaseItem findBestMatch(const QString& component, const QString& osTag, const QString& arch, const QString& preferredVersion = QString()) const;

signals:
    void releasesFetched(const QList<ReleaseItem>& releases, const QList<ReleaseCategory>& categories);
    void fetchFailed(const QString& errorMessage);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    QString m_baseUrl;
    QNetworkAccessManager* m_netManager;
    QList<ReleaseCategory> m_categories;
    QList<ReleaseItem> m_releases;
};

#endif // RELEASE_MANAGER_H
