#include "DownloadManagerDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/Logger.h"

DownloadManagerDialog::DownloadManagerDialog(
    const QList<PackageRequirement>& initialRequirements,
    QWidget* parent
)
    : QDialog(parent),
      m_requirements(initialRequirements),
      m_releaseManager(new ReleaseManager(this)),
      m_downloadNetManager(new QNetworkAccessManager(this))
{
    setupUi();

    connect(m_releaseManager, &ReleaseManager::releasesFetched, this, &DownloadManagerDialog::onReleasesFetched);
    connect(m_releaseManager, &ReleaseManager::fetchFailed, this, &DownloadManagerDialog::onFetchFailed);

    updateTable();

    // Otomatik olarak güncel sürümleri çek
    onRefreshClicked();
}

DownloadManagerDialog::~DownloadManagerDialog() {
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    if (m_currentOutputFile) {
        if (m_currentOutputFile->isOpen()) {
            m_currentOutputFile->close();
        }
        delete m_currentOutputFile;
        m_currentOutputFile = nullptr;
    }
}

void DownloadManagerDialog::setupUi() {
    setWindowTitle(tr("Akıllı Paket Çözümleyici & İndirme Yöneticisi"));
    resize(860, 520);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // 1. Üst Alan: Kaynak Seçimi (okidia.net vs GitHub)
    auto* sourceLayout = new QHBoxLayout();
    sourceLayout->setSpacing(16);

    auto* srcTitle = new QLabel(tr("İndirme Kaynağı:"), this);
    QFont boldFont = srcTitle->font();
    boldFont.setBold(true);
    srcTitle->setFont(boldFont);
    sourceLayout->addWidget(srcTitle);

    m_radioOkidiaNet = new QRadioButton(tr("OKIDIA Resmi Portalı (okidia.net)"), this);
    m_radioOkidiaNet->setChecked(true);
    sourceLayout->addWidget(m_radioOkidiaNet);

    m_radioGitHub = new QRadioButton(tr("GitHub Releases (github.com)"), this);
    m_radioGitHub->setEnabled(false);
    m_radioGitHub->setToolTip(tr("Yakında hizmetinizde"));
    sourceLayout->addWidget(m_radioGitHub);

    m_sourceGroup = new QButtonGroup(this);
    m_sourceGroup->addButton(m_radioOkidiaNet);
    m_sourceGroup->addButton(m_radioGitHub);

    sourceLayout->addStretch();

    m_refreshBtn = new QPushButton(tr("Listeyi Yenile"), this);
    m_refreshBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    connect(m_refreshBtn, &QPushButton::clicked, this, &DownloadManagerDialog::onRefreshClicked);
    sourceLayout->addWidget(m_refreshBtn);

    mainLayout->addLayout(sourceLayout);

    // 2. Tablo Alanı
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    QStringList headers = {
        tr("Bileşen"),
        tr("Hedef OS & Mimari"),
        tr("Hedef Cihazlar"),
        tr("Paket Dosyası"),
        tr("Boyut"),
        tr("Durum")
    };
    m_table->setHorizontalHeaderLabels(headers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table);

    // 3. Durum ve İlerleme Çubuğu
    m_statusLabel = new QLabel(tr("Sistem gereksinimleri analiz ediliyor..."), this);
    mainLayout->addWidget(m_statusLabel);

    m_overallProgress = new QProgressBar(this);
    m_overallProgress->setRange(0, 100);
    m_overallProgress->setValue(0);
    m_overallProgress->setTextVisible(true);
    m_overallProgress->setFixedHeight(18);
    mainLayout->addWidget(m_overallProgress);

    // 4. Alt Butonlar
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    m_startBtn = new QPushButton(tr("Eksik Paketleri İndir"), this);
    m_startBtn->setIcon(ThemeManager::icon(":/ui/checked.svg"));
    m_startBtn->setDefault(true);
    connect(m_startBtn, &QPushButton::clicked, this, &DownloadManagerDialog::onStartDownloadClicked);
    btnLayout->addWidget(m_startBtn);

    m_closeBtn = new QPushButton(tr("Kapat"), this);
    m_closeBtn->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    connect(m_closeBtn, &QPushButton::clicked, this, &DownloadManagerDialog::onCancelClicked);
    btnLayout->addWidget(m_closeBtn);

    mainLayout->addLayout(btnLayout);
}

void DownloadManagerDialog::updateTable() {
    m_table->setRowCount(m_requirements.size());

    int missingCount = 0;

    for (int row = 0; row < m_requirements.size(); ++row) {
        const auto& req = m_requirements.at(row);

        // Bileşen
        auto* compItem = new QTableWidgetItem(req.component);
        if (req.component.contains("broker")) {
            compItem->setIcon(ThemeManager::icon(":/ui/admin.svg"));
        } else if (req.component.contains("master")) {
            compItem->setIcon(ThemeManager::icon(":/ui/teacher.svg"));
        } else {
            compItem->setIcon(ThemeManager::icon(":/ui/student.svg"));
        }
        m_table->setItem(row, 0, compItem);

        // Hedef OS & Mimari
        QString osLabel = QString("%1 (%2)").arg(req.osTag.isEmpty() ? tr("Genel") : req.osTag, req.arch.isEmpty() ? "all" : req.arch);
        auto* osItem = new QTableWidgetItem(osLabel);
        if (req.osTag.contains("win")) {
            osItem->setIcon(ThemeManager::icon(":/ui/windows.svg"));
        } else {
            osItem->setIcon(ThemeManager::icon(":/ui/linux.svg"));
        }
        m_table->setItem(row, 1, osItem);

        // Hedef Cihazlar
        QString devInfo = QString(tr("%1 cihaz")).arg(req.targetNodes.size());
        QStringList devNames;
        for (const auto& n : req.targetNodes) {
            devNames.append(n.name.isEmpty() ? n.id : n.name);
        }
        auto* devItem = new QTableWidgetItem(devInfo);
        devItem->setToolTip(devNames.join("\n"));
        m_table->setItem(row, 2, devItem);

        // Dosya Adı
        QString fName = req.expectedFilename.isEmpty() ? (req.localFilePath.isEmpty() ? tr("Belirlenemedi") : QFileInfo(req.localFilePath).fileName()) : req.expectedFilename;
        auto* fileItem = new QTableWidgetItem(fName);
        fileItem->setIcon(ThemeManager::icon(":/ui/file.svg"));
        m_table->setItem(row, 3, fileItem);

        // Boyut
        auto* sizeItem = new QTableWidgetItem(req.sizeHuman.isEmpty() ? "-" : req.sizeHuman);
        m_table->setItem(row, 4, sizeItem);

        // Durum
        auto* statusItem = new QTableWidgetItem(req.statusText());
        switch (req.status) {
            case PackageStatus::Ready:
                statusItem->setIcon(ThemeManager::icon(":/ui/check_circle.svg"));
                statusItem->setForeground(ThemeManager::instance().successColor());
                break;
            case PackageStatus::Missing:
                statusItem->setIcon(ThemeManager::icon(":/ui/warning.svg"));
                statusItem->setForeground(ThemeManager::instance().warningColor());
                missingCount++;
                break;
            case PackageStatus::HashMismatch:
                statusItem->setIcon(ThemeManager::icon(":/ui/error_circle.svg"));
                statusItem->setForeground(ThemeManager::instance().errorColor());
                missingCount++;
                break;
            case PackageStatus::Unavailable:
                statusItem->setIcon(ThemeManager::icon(":/ui/forbidden.svg"));
                statusItem->setForeground(ThemeManager::instance().errorColor());
                break;
        }
        m_table->setItem(row, 5, statusItem);
    }

    if (missingCount > 0) {
        m_statusLabel->setText(QString(tr("%1 adet paket eksik veya güncellenmeli.")).arg(missingCount));
        m_startBtn->setEnabled(true);
        m_startBtn->setText(QString(tr("Eksik Paketleri İndir (%1)")).arg(missingCount));
    } else {
        m_statusLabel->setText(tr("Tüm gerekli paketler yerel önbellekte hazır ve doğrulandı."));
        m_startBtn->setEnabled(false);
        m_startBtn->setText(tr("Tüm Paketler Hazır"));
    }
}

void DownloadManagerDialog::onRefreshClicked() {
    m_statusLabel->setText(tr("Resmi sunucudan sürüm listesi sorgulanıyor..."));
    m_refreshBtn->setEnabled(false);
    m_releaseManager->fetchReleases();
}

void DownloadManagerDialog::onReleasesFetched(const QList<ReleaseItem>& releases, const QList<ReleaseCategory>& /*categories*/) {
    m_refreshBtn->setEnabled(true);

    // Taranan gereksinimleri yeniden çözümle
    QList<TargetNode> allNodes;
    for (const auto& req : m_requirements) {
        allNodes.append(req.targetNodes);
    }

    QStringList searchDirs = { PackageRequirementSolver::defaultPackageCacheDir() };
    for (const auto& req : m_requirements) {
        if (!req.localFilePath.isEmpty()) {
            QString pDir = QFileInfo(req.localFilePath).absolutePath();
            if (!searchDirs.contains(pDir)) searchDirs.append(pDir);
        }
    }

    m_requirements = PackageRequirementSolver::solve(allNodes, releases, searchDirs);
    updateTable();
}

void DownloadManagerDialog::onFetchFailed(const QString& errorMessage) {
    m_refreshBtn->setEnabled(true);
    m_statusLabel->setText(tr("Hata: %1").arg(errorMessage));
}

void DownloadManagerDialog::onStartDownloadClicked() {
    m_downloadQueue.clear();
    for (int i = 0; i < m_requirements.size(); ++i) {
        if (m_requirements[i].status == PackageStatus::Missing ||
            m_requirements[i].status == PackageStatus::HashMismatch) {
            if (!m_requirements[i].downloadUrl.isEmpty()) {
                m_downloadQueue.enqueue(i);
            }
        }
    }

    if (m_downloadQueue.isEmpty()) {
        QMessageBox::information(this, tr("Bilgi"), tr("İndirilecek eksik paket bulunmuyor."));
        return;
    }

    m_startBtn->setEnabled(false);
    m_refreshBtn->setEnabled(false);
    startNextInQueue();
}

void DownloadManagerDialog::startNextInQueue() {
    if (m_downloadQueue.isEmpty()) {
        m_currentQueueIndex = -1;
        m_overallProgress->setValue(100);
        m_statusLabel->setText(tr("Tüm indirmeler tamamlandı!"));
        m_startBtn->setEnabled(true);
        m_refreshBtn->setEnabled(true);
        updateTable();

        QStringList readyPaths;
        for (const auto& req : m_requirements) {
            if (req.status == PackageStatus::Ready && !req.localFilePath.isEmpty()) {
                readyPaths.append(req.localFilePath);
            }
        }
        m_downloadedFiles = readyPaths;
        emit packagesReady(readyPaths);
        return;
    }

    m_currentQueueIndex = m_downloadQueue.dequeue();
    const auto& req = m_requirements.at(m_currentQueueIndex);

    QString destDir = PackageRequirementSolver::defaultPackageCacheDir();
    QString finalPath = QDir(destDir).filePath(req.expectedFilename);
    QString partPath = finalPath + ".part";

    if (m_currentOutputFile) {
        delete m_currentOutputFile;
        m_currentOutputFile = nullptr;
    }

    m_currentOutputFile = new QFile(partPath);
    if (!m_currentOutputFile->open(QIODevice::WriteOnly)) {
        Logger::warning(QString("DownloadManager: Dosya yazılamadı: %1").arg(partPath));
        startNextInQueue();
        return;
    }

    m_statusLabel->setText(QString(tr("İndiriliyor: %1...")).arg(req.expectedFilename));
    m_overallProgress->setValue(0);

    QNetworkRequest request(QUrl(req.downloadUrl));
    request.setHeader(QNetworkRequest::UserAgentHeader, "OkidiaMaster/2.13 (DownloadManager)");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_downloadNetManager->get(request);
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, &DownloadManagerDialog::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_currentReply && m_currentOutputFile && m_currentOutputFile->isOpen()) {
            m_currentOutputFile->write(m_currentReply->readAll());
        }
    });
    connect(m_currentReply, &QNetworkReply::finished, this, &DownloadManagerDialog::onDownloadFinished);
}

void DownloadManagerDialog::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        int percent = static_cast<int>((bytesReceived * 100) / bytesTotal);
        m_overallProgress->setValue(percent);
    }
}

void DownloadManagerDialog::onDownloadFinished() {
    if (!m_currentReply) return;

    QNetworkReply* reply = m_currentReply;
    m_currentReply = nullptr;
    reply->deleteLater();

    if (m_currentOutputFile) {
        m_currentOutputFile->close();
    }

    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_requirements.size()) {
        startNextInQueue();
        return;
    }

    auto& req = m_requirements[m_currentQueueIndex];
    QString destDir = PackageRequirementSolver::defaultPackageCacheDir();
    QString finalPath = QDir(destDir).filePath(req.expectedFilename);
    QString partPath = finalPath + ".part";

    if (reply->error() != QNetworkReply::NoError) {
        Logger::warning(QString("DownloadManager indirme hatası: %1").arg(reply->errorString()));
        QFile::remove(partPath);
        startNextInQueue();
        return;
    }

    // Geçici part dosyasını nihai dosyaya taşı
    if (QFile::exists(finalPath)) {
        QFile::remove(finalPath);
    }
    if (!QFile::rename(partPath, finalPath)) {
        Logger::warning(QString("DownloadManager: Yeniden adlandırma başarısız: %1 -> %2").arg(partPath, finalPath));
        startNextInQueue();
        return;
    }

    // SHA-256 bütünlüğünü doğrula
    if (!req.sha256.isEmpty()) {
        QString downloadedSha = PackageRequirementSolver::computeSha256(finalPath);
        if (downloadedSha == req.sha256.toLower()) {
            req.status = PackageStatus::Ready;
            req.localFilePath = finalPath;
            Logger::info(QString("DownloadManager: Paket başarıyla indirildi ve doğrulandı: %1").arg(finalPath));
        } else {
            req.status = PackageStatus::HashMismatch;
            Logger::warning(QString("DownloadManager: İndirilen pakette SHA-256 uyuşmazlığı: %1 (Beklenen: %2, İndirilen: %3)")
                         .arg(finalPath, req.sha256, downloadedSha));
        }
    } else {
        req.status = PackageStatus::Ready;
        req.localFilePath = finalPath;
    }

    updateTable();
    startNextInQueue();
}

void DownloadManagerDialog::onCancelClicked() {
    if (m_currentReply) {
        m_currentReply->abort();
    }
    reject();
}
