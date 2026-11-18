// master/ui/ReportIssueDialog/ReportIssueDialog.cpp
#include "ReportIssueDialog.h"
#include "../../diagnostics/DiagnosticCollector.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/version/OkidiaVersion.h"
#include "../../session/SessionManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScreen>
#include <QGuiApplication>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>

ReportIssueDialog::ReportIssueDialog(SessionManager* session, QWidget* parent)
    : QDialog(parent)
    , m_session(session)
{
    setupUi();
}

void ReportIssueDialog::setupUi() {
    setWindowTitle(tr("Hata ve Geri Bildirim Bildir"));
    setMinimumWidth(560);
    setMinimumHeight(440);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* mainLay = new QVBoxLayout(this);
    mainLay->setSpacing(14);
    mainLay->setContentsMargins(20, 20, 20, 20);

    // Başlık ve Açıklama
    auto* headerLbl = new QLabel(tr("<b>Teknik Teşhis ve Hata Bildirimi</b>"), this);
    headerLbl->setStyleSheet("font-size: 15px; font-weight: bold;");
    mainLay->addWidget(headerLbl);

    auto* descLbl = new QLabel(
        tr("Karşılaştığınız sorunu bildirerek Okidia'nın gelişmesine katkıda bulunabilirsiniz.\n"
           "Teknik sistem künyesi ve filtrelenmiş hata günlükleri otomatik olarak paketlenecektir."),
        this);
    descLbl->setWordWrap(true);
    descLbl->setStyleSheet("color: gray; font-size: 12px;");
    mainLay->addWidget(descLbl);

    // Form Grubu
    auto* formGroup = new QGroupBox(tr("Sorun Detayları"), this);
    auto* formLay = new QFormLayout(formGroup);
    formLay->setSpacing(10);
    formLay->setContentsMargins(14, 14, 14, 14);

    // Kategori
    m_categoryCombo = new QComboBox(formGroup);
    m_categoryCombo->addItem(tr("Ses Yayını & Anons"), "audio_broadcast");
    m_categoryCombo->addItem(tr("Ekran Yayını & İzleme"), "screen_broadcast");
    m_categoryCombo->addItem(tr("Kilit Ekranı & Güvenlik"), "lock_screen");
    m_categoryCombo->addItem(tr("Uzak Masaüstü Kontrolü"), "remote_control");
    m_categoryCombo->addItem(tr("Dosya Dağıtımı / Transfer"), "file_transfer");
    m_categoryCombo->addItem(tr("Ağ & Broker Bağlantısı"), "network_connection");
    m_categoryCombo->addItem(tr("Genel Sistem / Diğer"), "other");
    formLay->addRow(tr("Kategori:"), m_categoryCombo);

    // Zaman Penceresi
    m_timeWindowCombo = new QComboBox(formGroup);
    m_timeWindowCombo->addItem(tr("Son 15 Dakika (Önerilen)"), 15);
    m_timeWindowCombo->addItem(tr("Son 1 Saat"), 60);
    m_timeWindowCombo->addItem(tr("Son 24 Saat / Tüm Gün"), 1440);
    formLay->addRow(tr("Olay Zamanı:"), m_timeWindowCombo);

    // Kullanıcı Açıklaması
    m_commentsEdit = new QTextEdit(formGroup);
    m_commentsEdit->setPlaceholderText(
        tr("Lütfen ne yaparken bu hatayla karşılaştığınızı kısaca anlatınız...\n"
           "(Örnek: 10-A sınıfına ses dosyası oynatırken buton asılı kaldı ve ses gitmedi.)"));
    m_commentsEdit->setMinimumHeight(100);
    formLay->addRow(tr("Açıklama:"), m_commentsEdit);

    // Ekran Görüntüsü Onayı
    m_chkScreenshot = new QCheckBox(tr("Mevcut Master ekran görüntüsünü teşhis paketine iliştir"), formGroup);
    m_chkScreenshot->setChecked(true);
    formLay->addRow(QString(), m_chkScreenshot);

    mainLay->addWidget(formGroup);

    // Durum ve İlerleme
    m_statusLbl = new QLabel(this);
    m_statusLbl->setStyleSheet("font-size: 12px; font-weight: 500;");
    m_statusLbl->hide();
    mainLay->addWidget(m_statusLbl);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->hide();
    mainLay->addWidget(m_progressBar);

    // Alt Butonlar
    auto* btnLay = new QHBoxLayout;
    btnLay->setSpacing(10);

    m_btnInspect = new QPushButton(ThemeManager::icon(":/ui/save.svg"), tr("Paketi İncele / Masaüstüne Kaydet"), this);
    m_btnInspect->setToolTip(tr("Paketi göndermeden önce incelemek veya masaüstüne kaydetmek için tıklayın."));
    connect(m_btnInspect, &QPushButton::clicked, this, &ReportIssueDialog::onMasaustuneKaydetClicked);
    btnLay->addWidget(m_btnInspect);

    btnLay->addStretch();

    m_btnCancel = new QPushButton(tr("İptal"), this);
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLay->addWidget(m_btnCancel);

    m_btnSend = new QPushButton(tr("Gönder"), this);
    m_btnSend->setDefault(true);
    m_btnSend->setStyleSheet("font-weight: bold; min-width: 90px;");
    connect(m_btnSend, &QPushButton::clicked, this, &ReportIssueDialog::onGonderClicked);
    btnLay->addWidget(m_btnSend);

    mainLay->addLayout(btnLay);
}

bool ReportIssueDialog::preparePackage(QString& outZipPath, QString& outIncidentId) {
    setUiBusy(true, tr("Teşhis paketi hazırlanıyor..."));

    IncidentReportRequest req;
    req.category = m_categoryCombo->currentData().toString();
    req.userComments = m_commentsEdit->toPlainText().trimmed();
    req.timeWindowMinutes = m_timeWindowCombo->currentData().toInt();
    req.includeScreenshot = m_chkScreenshot->isChecked();
    req.session = m_session;

    if (req.includeScreenshot) {
        if (parentWidget()) {
            req.screenshot = parentWidget()->grab();
        } else if (QGuiApplication::primaryScreen()) {
            req.screenshot = QGuiApplication::primaryScreen()->grabWindow(0);
        }
    }

    IncidentReportResult res = DiagnosticCollector::collect(req);
    if (!res.success) {
        setUiBusy(false);
        QMessageBox::critical(this, tr("Hata"), tr("Teşhis paketi oluşturulamadı:\n%1").arg(res.errorMessage));
        return false;
    }

    outZipPath = res.zipFilePath;
    outIncidentId = res.incidentId;
    m_lastZipPath = res.zipFilePath;
    m_lastIncidentId = res.incidentId;
    return true;
}

void ReportIssueDialog::setUiBusy(bool busy, const QString& statusText) {
    m_btnSend->setEnabled(!busy);
    m_btnInspect->setEnabled(!busy);
    m_categoryCombo->setEnabled(!busy);
    m_timeWindowCombo->setEnabled(!busy);
    m_commentsEdit->setEnabled(!busy);
    m_chkScreenshot->setEnabled(!busy);

    if (busy) {
        m_statusLbl->setText(statusText);
        m_statusLbl->setStyleSheet("color: #3b82f6; font-size: 12px; font-weight: 500;");
        m_statusLbl->show();
        m_progressBar->show();
        m_progressBar->setValue(0);
    } else {
        m_progressBar->hide();
    }
}

void ReportIssueDialog::onMasaustuneKaydetClicked() {
    QString zipPath, incidentId;
    if (!preparePackage(zipPath, incidentId)) {
        return;
    }

    const QString desktopDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString targetDesktopPath = desktopDir + QString("/okidia_incident_%1.zip").arg(incidentId);

    QFile::remove(targetDesktopPath);
    if (QFile::copy(zipPath, targetDesktopPath)) {
        setUiBusy(false);
        m_statusLbl->setText(tr("Paket masaüstüne kaydedildi: %1").arg(QFileInfo(targetDesktopPath).fileName()));
        m_statusLbl->setStyleSheet("color: #10b981; font-size: 12px; font-weight: bold;");
        m_statusLbl->show();

        // Masaüstü klasörünü dosya yöneticisinde göster
        QDesktopServices::openUrl(QUrl::fromLocalFile(desktopDir));

        QMessageBox::information(
            this, tr("Paket Kaydedildi"),
            tr("Teşhis paketi masaüstünüze başarıyla kaydedildi:\n\n%1\n\n"
               "Paket içeriğini (diagnostics.json, loglar ve ekran görüntüsü) inceleyebilirsiniz.")
                .arg(targetDesktopPath));
    } else {
        setUiBusy(false);
        QMessageBox::warning(this, tr("Kayıt Hatası"), tr("Paket masaüstüne kopyalanamadı: %1").arg(targetDesktopPath));
    }
}

void ReportIssueDialog::onGonderClicked() {
    if (m_commentsEdit->toPlainText().trimmed().isEmpty()) {
        int ans = QMessageBox::question(
            this, tr("Açıklama Boş"),
            tr("Sorun açıklamasını boş bıraktınız. Yine de yalnızca sistem loglarını ve teknik künyeyi göndermek istiyor musunuz?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes) {
            m_commentsEdit->setFocus();
            return;
        }
    }

    QString zipPath, incidentId;
    if (!preparePackage(zipPath, incidentId)) {
        return;
    }

    setUiBusy(true, tr("Sunucuya iletiliyor (%1)...").arg(incidentId));

    auto* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    // incident_id
    QHttpPart idPart;
    idPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"incident_id\""));
    idPart.setBody(incidentId.toUtf8());
    multiPart->append(idPart);

    // category
    QHttpPart catPart;
    catPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"category\""));
    catPart.setBody(m_categoryCombo->currentData().toString().toUtf8());
    multiPart->append(catPart);

    // user_comments
    QHttpPart commentPart;
    commentPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"user_comments\""));
    commentPart.setBody(m_commentsEdit->toPlainText().trimmed().toUtf8());
    multiPart->append(commentPart);

    // file
    QFile* file = new QFile(zipPath);
    if (!file->open(QIODevice::ReadOnly)) {
        setUiBusy(false);
        delete multiPart;
        delete file;
        QMessageBox::critical(this, tr("Hata"), tr("Paket dosyası okunamadı: %1").arg(zipPath));
        return;
    }

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/zip"));
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QVariant(QString("form-data; name=\"incident_file\"; filename=\"%1\"")
                               .arg(QFileInfo(zipPath).fileName())));
    filePart.setBodyDevice(file);
    file->setParent(multiPart); // multiPart silindiğinde dosya da kapansın ve silinsin
    multiPart->append(filePart);

    QUrl url("https://okidia.net/api/v1/report_incident.php");
    QNetworkRequest netReq(url);
    netReq.setRawHeader("User-Agent", "OkidiaMaster/" + Okidia::Version::string().toUtf8());

    m_activeReply = m_netManager.post(netReq, multiPart);
    multiPart->setParent(m_activeReply); // reply bittiğinde multiPart temizlensin

    connect(m_activeReply, &QNetworkReply::uploadProgress, this, &ReportIssueDialog::onUploadProgress);
    connect(m_activeReply, &QNetworkReply::finished, this, &ReportIssueDialog::onUploadFinished);
}

void ReportIssueDialog::onUploadProgress(qint64 bytesSent, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        int pct = static_cast<int>((bytesSent * 100) / bytesTotal);
        m_progressBar->setValue(pct);
        m_statusLbl->setText(tr("Yükleniyor: %1%").arg(pct));
    }
}

void ReportIssueDialog::onUploadFinished() {
    if (!m_activeReply) return;

    const int statusCode = m_activeReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseData = m_activeReply->readAll();
    const QNetworkReply::NetworkError netErr = m_activeReply->error();
    m_activeReply->deleteLater();
    m_activeReply = nullptr;

    setUiBusy(false);

    bool isSuccess = false;
    QString serverMsg;

    if (netErr == QNetworkReply::NoError && (statusCode == 200 || statusCode == 201)) {
        QJsonParseError jerr;
        QJsonDocument jdoc = QJsonDocument::fromJson(responseData, &jerr);
        if (!jdoc.isNull() && jdoc.isObject()) {
            QJsonObject jobj = jdoc.object();
            if (jobj["success"].toBool()) {
                isSuccess = true;
                serverMsg = jobj["incident_id"].toString(m_lastIncidentId);
            } else {
                serverMsg = jobj["message"].toString(tr("Sunucu hatası"));
            }
        }
    }

    if (isSuccess) {
        m_statusLbl->setText(tr("Rapor başarıyla iletildi (#%1)").arg(serverMsg));
        m_statusLbl->setStyleSheet("color: #10b981; font-size: 12px; font-weight: bold;");
        m_statusLbl->show();

        QMessageBox::information(
            this, tr("Teşekkür Ederiz"),
            tr("Hata ve geri bildirim raporunuz başarıyla iletildi.\n\n"
               "Takip Kodu: #%1\n\n"
               "Teknik inceleme sonrasında gerekli güncellemeler yayınlanacaktır.")
                .arg(serverMsg));
        accept();
    } else {
        // Sunucuya ulaşılamadı (MEB engeli, internet yokluğu vb.) -> Zarafetle Masaüstüne Kaydet Fallback'i
        const QString desktopDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        const QString fallbackPath = desktopDir + QString("/okidia_incident_%1.zip").arg(m_lastIncidentId);
        QFile::copy(m_lastZipPath, fallbackPath);

        m_statusLbl->setText(tr("Sunucuya ulaşılamadı. Paket masaüstüne kaydedildi."));
        m_statusLbl->setStyleSheet("color: #f59e0b; font-size: 12px; font-weight: bold;");
        m_statusLbl->show();

        QMessageBox::warning(
            this, tr("Sunucuya Ulaşılamadı"),
            tr("Okidia sunucusuna bağlanılamadı (İnternet bağlantınızı veya MEB ağ filtresini kontrol ediniz).\n\n"
               "Oluşturulan teşhis paketi masaüstünüze güvenle kaydedildi:\n%1\n\n"
               "Lütfen bu dosyayı interneti olan bir cihazdan destek@okidia.net adresine iletiniz.")
                .arg(fallbackPath));
    }
}
