// master/ui/Widgets/MasterUpdateDialog.cpp
#include "MasterUpdateDialog.h"
#include "../session/SessionManager.h"
#include "../transfer/MasterUpdater.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

MasterUpdateDialog::MasterUpdateDialog(SessionManager* session,
                                       const QString& newVersion,
                                       const QString& filename,
                                       quint64 fileSize,
                                       const QString& osTag,
                                       const QString& osArch,
                                       QWidget* parent)
    : QDialog(parent),
      m_session(session),
      m_newVersion(newVersion),
      m_filename(filename),
      m_fileSize(fileSize),
      m_osTag(osTag),
      m_osArch(osArch)
{
    setWindowTitle(tr("OKIDIA Master Güncellemesi"));
    resize(480, 320);
    setupUi();

    if (m_session && m_session->masterUpdater()) {
        connect(m_session->masterUpdater(), &MasterUpdater::downloadProgress,
                this, &MasterUpdateDialog::onDownloadProgress);
        connect(m_session->masterUpdater(), &MasterUpdater::downloadFinished,
                this, &MasterUpdateDialog::onDownloadFinished);
    }
}

void MasterUpdateDialog::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setSpacing(15);
    mainLay->setContentsMargins(20, 20, 20, 20);

    // Başlık ve İkon
    auto* headerLay = new QHBoxLayout();
    auto* iconLbl = new QLabel(this);
    iconLbl->setPixmap(ThemeManager::icon(":/ui/refresh.svg").pixmap(48, 48));
    headerLay->addWidget(iconLbl);

    auto* titleLay = new QVBoxLayout();
    auto* titleLbl = new QLabel(tr("<h2>Yeni Bir Master Sürümü Mevcut!</h2>"), this);
    titleLay->addWidget(titleLbl);

    QString curVer = Okidia::Version::string();

    auto* verLbl = new QLabel(QString(tr("Mevcut Sürüm: <b>v%1</b> → Yeni Sürüm: <font color='#2ecc71'><b>v%2</b></font>"))
                                  .arg(curVer, m_newVersion), this);
    titleLay->addWidget(verLbl);
    headerLay->addLayout(titleLay);
    headerLay->addStretch();
    mainLay->addLayout(headerLay);

    // Bilgi Kutusu
    QString sizeStr = OkidiaUtils::formatSize(m_fileSize);
    QString infoText = QString(tr("Paket: <b>%1</b> (%2)<br>"
                                  "Güncelleme paketi okulun yerel Broker sunucusundan doğrudan güvenli TLS kanalı üzerinden indirilecektir."))
                           .arg(m_filename, sizeStr);
    m_infoLabel = new QLabel(infoText, this);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setStyleSheet("background-color: rgba(255,255,255,0.05); padding: 10px; border-radius: 6px;");
    mainLay->addWidget(m_infoLabel);

    // İlerleme Çubuğu ve Durum
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    mainLay->addWidget(m_progressBar);

    m_statusLabel = new QLabel("", this);
    m_statusLabel->setWordWrap(true);
    mainLay->addWidget(m_statusLabel);

    mainLay->addStretch();

    // Butonlar
    auto* btnLay = new QHBoxLayout();
    m_closeBtn = new QPushButton(tr("Daha Sonra"), this);
    m_closeBtn->setProperty("class", "secondary");
    btnLay->addWidget(m_closeBtn);

    btnLay->addStretch();

    m_actionBtn = new QPushButton(tr(" Şimdi İndir ve Kur"), this);
    m_actionBtn->setIcon(ThemeManager::icon(":/ui/download.svg"));
    m_actionBtn->setProperty("class", "primary");
    m_actionBtn->setMinimumHeight(38);
    btnLay->addWidget(m_actionBtn);

    mainLay->addLayout(btnLay);

    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_actionBtn, &QPushButton::clicked, this, &MasterUpdateDialog::onStartDownloadClicked);
}

void MasterUpdateDialog::onStartDownloadClicked() {
    if (!m_downloadedDebPath.isEmpty()) {
        // İndirme zaten tamamlanmış, kur ve yeniden başlat
        m_statusLabel->setText(tr("Kurulum yetkisi bekleniyor..."));
        MasterUpdater::installAndRestart(m_downloadedDebPath, this);
        return;
    }

    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Hata"), tr("Broker bağlantısı yok!"));
        return;
    }

    m_actionBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_statusLabel->setText(tr("Broker'dan paket indiriliyor..."));

    m_session->requestMasterUpdateDownload(m_newVersion, m_osTag, m_osArch);
}

void MasterUpdateDialog::onDownloadProgress(quint64 received, quint64 total, int percent) {
    m_progressBar->setVisible(true);
    m_progressBar->setMaximum(total);
    m_progressBar->setValue(received);
    m_statusLabel->setText(QString(tr("İndiriliyor: %1 / %2 (%3%)"))
                               .arg(OkidiaUtils::formatSize(received),
                                    OkidiaUtils::formatSize(total),
                                    QString::number(percent)));
}

void MasterUpdateDialog::onDownloadFinished(bool success, const QString& pathOrError) {
    if (success) {
        m_downloadedDebPath = pathOrError;
        m_progressBar->setValue(m_progressBar->maximum());
        m_statusLabel->setText(tr("<font color='#2ecc71'><b>İndirme tamamlandı!</b></font> Kuruluma geçmek için butona tıklayın."));
        m_actionBtn->setText(tr(" Şimdi Kur ve Yeniden Başlat"));
        m_actionBtn->setIcon(ThemeManager::icon(":/ui/root_drive.svg"));
        m_actionBtn->setProperty("class", "danger");
        m_actionBtn->setEnabled(true);
    } else {
        m_statusLabel->setText(QString(tr("<font color='#e74c3c'>İndirme başarısız: %1</font>")).arg(pathOrError));
        m_actionBtn->setText(tr(" Yeniden Dene"));
        m_actionBtn->setEnabled(true);
    }
}
