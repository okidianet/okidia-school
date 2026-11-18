#include "GitHubFetcher.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QUrl>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QSettings>

GitHubFetcher::GitHubFetcher(QWidget* parent)
    : QDialog(parent), m_manager(new QNetworkAccessManager(this)), m_currentReply(nullptr)
{
    setWindowTitle(tr("GitHub'dan Agent Sürümlerini Çek"));
    setMinimumSize(500, 350);

    auto* mainLayout = new QVBoxLayout(this);

    m_statusLabel = new QLabel(tr("Sürümler aranıyor..."), this);
    mainLayout->addWidget(m_statusLabel);

    auto* tokenLayout = new QHBoxLayout();
    tokenLayout->addWidget(new QLabel(tr("GitHub Token (Private repo için):"), this));
    m_tokenInput = new QLineEdit(this);
    m_tokenInput->setEchoMode(QLineEdit::Password);
    m_tokenInput->setPlaceholderText("github_pat_XXXXXX...");
    
    QSettings settings;
    m_tokenInput->setText(settings.value("github_pat").toString());
    tokenLayout->addWidget(m_tokenInput);
    
    m_refreshBtn = new QPushButton(tr("Yenile"), this);
    tokenLayout->addWidget(m_refreshBtn);
    
    mainLayout->addLayout(tokenLayout);

    m_assetList = new QListWidget(this);
    m_assetList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mainLayout->addWidget(m_assetList);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    mainLayout->addWidget(m_progress);

    auto* btnLayout = new QHBoxLayout();
    m_cancelBtn = new QPushButton(tr("İptal"), this);
    m_downloadBtn = new QPushButton(tr("İndir"), this);
    m_downloadBtn->setEnabled(false);

    btnLayout->addStretch();
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_downloadBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_cancelBtn, &QPushButton::clicked, this, &GitHubFetcher::reject);
    connect(m_downloadBtn, &QPushButton::clicked, this, &GitHubFetcher::onDownloadButtonClicked);
    connect(m_refreshBtn, &QPushButton::clicked, this, &GitHubFetcher::showDialogAndFetch);
    connect(m_tokenInput, &QLineEdit::returnPressed, this, &GitHubFetcher::showDialogAndFetch);
    connect(m_assetList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_downloadBtn->setEnabled(!m_assetList->selectedItems().isEmpty());
    });
}

GitHubFetcher::~GitHubFetcher() {
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
}

void GitHubFetcher::showDialogAndFetch() {
    m_statusLabel->setText(tr("GitHub API üzerinden en güncel sürümler sorgulanıyor..."));
    m_assetList->clear();
    m_assets.clear();
    m_progress->setVisible(false);
    m_downloadBtn->setEnabled(false);

    QNetworkRequest request(QUrl("https://api.github.com/repos/onurtamer79/okidia/releases/latest"));
    request.setRawHeader("User-Agent", "Okidia-Master-App");
    request.setRawHeader("Accept", "application/vnd.github.v3+json");
    
    QString token = m_tokenInput->text().trimmed();
    if (token.isEmpty()) {
        m_statusLabel->setText(tr("Private repo erişimi için GitHub Token girin ve Yenile'ye basın."));
        show();
        return;
    }
    
    request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    QSettings().setValue("github_pat", token); // Token'ı kaydet

    QNetworkReply* reply = m_manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onReleasesFetched(reply);
    });

    show();
}

void GitHubFetcher::onReleasesFetched(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        m_statusLabel->setText("Hata: " + reply->errorString());
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
    if (!jsonDoc.isObject()) {
        m_statusLabel->setText(tr("Hata: Geçersiz JSON yanıtı."));
        return;
    }

    QJsonObject root = jsonDoc.object();
    QString tagName = root["tag_name"].toString();
    QJsonArray assets = root["assets"].toArray();

    m_statusLabel->setText(QString(tr("En son sürüm: %1. İndirilecek paketi seçin:")).arg(tagName));

    for (int i = 0; i < assets.size(); ++i) {
        QJsonObject assetObj = assets[i].toObject();
        QString name = assetObj["name"].toString();
        if (name.endsWith(".deb") || name.endsWith(".exe")) {
            AssetInfo info;
            info.name = name;
            info.downloadUrl = assetObj["browser_download_url"].toString();
            info.apiUrl = assetObj["url"].toString();
            info.size = assetObj["size"].toInt();
            m_assets.append(info);

            double sizeMb = info.size / (1024.0 * 1024.0);
            QListWidgetItem* item = new QListWidgetItem(QString("%1 (%2 MB)").arg(info.name).arg(sizeMb, 0, 'f', 1));
            item->setData(Qt::UserRole, m_assets.size() - 1);
            m_assetList->addItem(item);
        }
    }

    if (m_assets.isEmpty()) {
        m_statusLabel->setText(tr("Bu sürümde uyumlu kurulum paketi (.deb / .exe) bulunamadı."));
    }
}

void GitHubFetcher::onDownloadButtonClicked() {
    auto items = m_assetList->selectedItems();
    if (items.isEmpty()) return;

    m_downloadQueue.clear();
    m_downloadedPaths.clear();

    for (auto* item : items) {
        int idx = item->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < m_assets.size()) {
            m_downloadQueue.enqueue(m_assets[idx]);
        }
    }

    if (m_downloadQueue.isEmpty()) return;

    m_downloadBtn->setEnabled(false);
    m_assetList->setEnabled(false);
    m_progress->setVisible(true);

    startNextDownload();
}

void GitHubFetcher::startNextDownload() {
    if (m_downloadQueue.isEmpty()) {
        if (!m_downloadedPaths.isEmpty()) {
            emit packageDownloaded(m_downloadedPaths.first()); // backwards compatibility
            emit packagesDownloaded(m_downloadedPaths);
        }
        accept();
        return;
    }

    const AssetInfo& asset = m_downloadQueue.head();

    QDir tempDir("/tmp/okidia_updates");
    if (!tempDir.exists()) {
        tempDir.mkpath(".");
    }

    m_downloadPath = tempDir.absoluteFilePath(asset.name);

    QString token = m_tokenInput->text().trimmed();
    
    QNetworkRequest request;
    if (!token.isEmpty()) {
        // Private repo: API üzerinden octet-stream isteği yapılmalı
        request.setUrl(QUrl(asset.apiUrl));
        request.setRawHeader("Accept", "application/octet-stream");
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    } else {
        // Public repo: Doğrudan browser linki
        request.setUrl(QUrl(asset.downloadUrl));
    }
    
    request.setRawHeader("User-Agent", "Okidia-Master-App");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_manager->get(request);
    
    m_progress->setValue(0);
    m_statusLabel->setText(QString(tr("İndiriliyor: %1 (Kalan: %2)")).arg(asset.name).arg(m_downloadQueue.size()));

    connect(m_currentReply, &QNetworkReply::downloadProgress, this, &GitHubFetcher::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::finished, this, &GitHubFetcher::onDownloadFinished);
}

void GitHubFetcher::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        m_progress->setMaximum(bytesTotal);
        m_progress->setValue(bytesReceived);
    }
}

void GitHubFetcher::onDownloadFinished() {
    if (!m_currentReply) return;
    
    m_currentReply->deleteLater();
    QNetworkReply* reply = m_currentReply;
    m_currentReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        m_statusLabel->setText(tr("İndirme hatası: ") + reply->errorString());
        m_progress->setVisible(false);
        m_downloadBtn->setEnabled(true);
        m_assetList->setEnabled(true);
        return;
    }

    QFile file(m_downloadPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(reply->readAll());
        file.close();
        
        m_downloadedPaths.append(m_downloadPath);
        m_downloadQueue.dequeue();
        startNextDownload();
    } else {
        m_statusLabel->setText(tr("Dosya yazılamadı: ") + m_downloadPath);
        m_progress->setVisible(false);
        m_downloadBtn->setEnabled(true);
        m_assetList->setEnabled(true);
    }
}
