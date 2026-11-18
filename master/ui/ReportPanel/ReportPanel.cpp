// master/ui/ReportPanel/ReportPanel.cpp
#include "ReportPanel.h"
#include "ui_ReportPanel.h"
#include "../../session/SessionManager.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../../common/db/DbRecords.h"

static QString friendlyTargetName(const QString& rawTarget) {
    if (rawTarget.isEmpty()) return QString();
    if (rawTarget == "room_admin") return QObject::tr("Yönetici Odası");

    if (rawTarget.startsWith("room_")) {
        QString classId = rawTarget.mid(5);
        if (OkidiaDatabase::instance().isOpen()) {
            ClassRecord cr = OkidiaDatabase::instance().classById(classId);
            if (!cr.name.isEmpty()) return cr.name;
        }
    }
    // Eğer doğrudan classId (UUID) olarak kaydedilmişse ve DB'de sınıf ise
    if (rawTarget.length() >= 8 && !rawTarget.contains(' ') && OkidiaDatabase::instance().isOpen()) {
        ClassRecord cr = OkidiaDatabase::instance().classById(rawTarget);
        if (!cr.name.isEmpty()) return cr.name;
    }
    return rawTarget;
}

#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QDir>
#include <QClipboard>
#include <QGuiApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPdfWriter>
#include <QPageSize>
#include <QPageLayout>
#include <QTextDocument>

ReportPanel::ReportPanel(SessionManager* session, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , ui(new Ui::ReportPanel)
    , m_session(session) {
    ui->setupUi(this);
    setWindowTitle(tr("Raporlama ve Denetim Panosu — OKIDIA"));
    resize(1080, 680);

    // Varsayılan aralık: Son 24 Saat
    QDateTime now = QDateTime::currentDateTime();
    ui->fromEdit->setDateTime(now.addDays(-1));
    ui->toEdit->setDateTime(now);
    ui->quickRangeCombo->setCurrentIndex(0);

    // Kategori simgeleri
    ui->categoryCombo->setItemIcon(0, ThemeManager::icon(":/ui/chart.svg"));
    ui->categoryCombo->setItemIcon(1, ThemeManager::icon(":/ui/key.svg"));
    ui->categoryCombo->setItemIcon(2, ThemeManager::icon(":/ui/play_cmd.svg"));
    ui->categoryCombo->setItemIcon(3, ThemeManager::icon(":/ui/file.svg"));
    ui->categoryCombo->setItemIcon(4, ThemeManager::icon(":/ui/chat.svg"));
    ui->categoryCombo->setItemIcon(5, ThemeManager::icon(":/ui/home.svg"));
    ui->categoryCombo->setItemIcon(6, ThemeManager::icon(":/ui/lock.svg"));
    ui->categoryCombo->setItemIcon(7, ThemeManager::icon(":/ui/power.svg"));
    ui->categoryCombo->setItemIcon(8, ThemeManager::icon(":/ui/network.svg"));
    ui->categoryCombo->setItemIcon(9, ThemeManager::icon(":/ui/graduation.svg"));

    // Buton ikonları
    ui->btnFilter->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    ui->btnReset->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    ui->btnExportCsv->setIcon(ThemeManager::icon(":/ui/file.svg"));
    ui->btnExportPdf->setIcon(ThemeManager::icon(":/ui/save.svg"));
    ui->btnExportHtml->setIcon(ThemeManager::icon(":/ui/monitor.svg"));

    // Tablo ayarları
    OkidiaUtils::setupTableHeaders(ui->eventTable, "report_event_table", {150, 110, 130, 140, 200}, true);
    ui->eventTable->setSortingEnabled(false);
    ui->eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->eventTable->verticalHeader()->setVisible(false);

    ui->dbPathLabel->setText(tr("Veritabanı: Merkezi Sunucu (Broker)"));

    // Sinyal - Slot Bağlantıları
    connect(ui->quickRangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ReportPanel::onQuickRangeChanged);
    connect(ui->btnFilter, &QPushButton::clicked,
            this, &ReportPanel::onFilter);
    connect(ui->btnReset, &QPushButton::clicked,
            this, &ReportPanel::onReset);
    connect(ui->searchEdit, &QLineEdit::returnPressed,
            this, &ReportPanel::onFilter);

    connect(ui->btnFirstPage, &QPushButton::clicked,
            this, &ReportPanel::onFirstPage);
    connect(ui->btnPrevPage, &QPushButton::clicked,
            this, &ReportPanel::onPrevPage);
    connect(ui->btnNextPage, &QPushButton::clicked,
            this, &ReportPanel::onNextPage);
    connect(ui->btnLastPage, &QPushButton::clicked,
            this, &ReportPanel::onLastPage);
    connect(ui->pageSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ReportPanel::onPageSizeChanged);

    connect(ui->btnExportCsv, &QPushButton::clicked,
            this, &ReportPanel::onExportCsv);
    connect(ui->btnExportPdf, &QPushButton::clicked,
            this, &ReportPanel::onExportPdf);
    connect(ui->btnExportHtml, &QPushButton::clicked,
            this, &ReportPanel::onExportHtml);

    connect(ui->eventTable, &QTableWidget::cellDoubleClicked,
            this, &ReportPanel::onRowDoubleClicked);

    if (m_session) {
        connect(m_session, &SessionManager::auditLogQueryReceived,
                this, &ReportPanel::onEventsReceived);
    }

    updatePaginationUi();
}

ReportPanel::~ReportPanel() {
    delete ui;
}

void ReportPanel::showAndRefresh() {
    show();
    raise();
    activateWindow();
    onFilter();
}

void ReportPanel::onQuickRangeChanged(int index) {
    QDateTime now = QDateTime::currentDateTime();
    switch (index) {
    case 0: // Son 24 Saat
        ui->fromEdit->setDateTime(now.addDays(-1));
        ui->toEdit->setDateTime(now);
        break;
    case 1: // Bugün
        ui->fromEdit->setDateTime(QDateTime(now.date(), QTime(0, 0, 0)));
        ui->toEdit->setDateTime(QDateTime(now.date(), QTime(23, 59, 59)));
        break;
    case 2: // Dün
        ui->fromEdit->setDateTime(QDateTime(now.date().addDays(-1), QTime(0, 0, 0)));
        ui->toEdit->setDateTime(QDateTime(now.date().addDays(-1), QTime(23, 59, 59)));
        break;
    case 3: // Bu Hafta
        ui->fromEdit->setDateTime(QDateTime(now.date().addDays(-(now.date().dayOfWeek() - 1)), QTime(0, 0, 0)));
        ui->toEdit->setDateTime(now);
        break;
    case 4: // Bu Ay
        ui->fromEdit->setDateTime(QDateTime(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0, 0)));
        ui->toEdit->setDateTime(now);
        break;
    case 5: // Özel Aralık
        return; // Kullanıcı serbestçe değiştirir
    }
    m_currentPage = 1;
    loadEvents();
}

void ReportPanel::onPageSizeChanged(int /*index*/) {
    m_pageSize = ui->pageSizeCombo->currentText().toInt();
    if (m_pageSize <= 0) m_pageSize = 50;
    m_currentPage = 1;
    loadEvents();
}

void ReportPanel::onFilter() {
    m_currentPage = 1;
    loadEvents();
}

void ReportPanel::onReset() {
    ui->quickRangeCombo->blockSignals(true);
    ui->quickRangeCombo->setCurrentIndex(0);
    ui->quickRangeCombo->blockSignals(false);

    QDateTime now = QDateTime::currentDateTime();
    ui->fromEdit->setDateTime(now.addDays(-1));
    ui->toEdit->setDateTime(now);
    ui->categoryCombo->setCurrentIndex(0);
    ui->searchEdit->clear();

    m_currentPage = 1;
    loadEvents();
}

void ReportPanel::onFirstPage() {
    if (m_currentPage != 1) {
        m_currentPage = 1;
        loadEvents();
    }
}

void ReportPanel::onPrevPage() {
    if (m_currentPage > 1) {
        --m_currentPage;
        loadEvents();
    }
}

void ReportPanel::onNextPage() {
    if (m_currentPage < m_totalPages) {
        ++m_currentPage;
        loadEvents();
    }
}

void ReportPanel::onLastPage() {
    if (m_currentPage != m_totalPages) {
        m_currentPage = m_totalPages;
        loadEvents();
    }
}

void ReportPanel::loadEvents() {
    if (!m_session) return;

    int categoryIndex = ui->categoryCombo->currentIndex();
    QString categoryStr;
    if (categoryIndex > 0) {
        AuditEvent::Category cat = static_cast<AuditEvent::Category>(categoryIndex - 1);
        categoryStr = AuditLog::categoryToString(cat);
    }

    const QDateTime from = ui->fromEdit->dateTime();
    const QDateTime to   = ui->toEdit->dateTime();
    const QString searchQuery = ui->searchEdit->text().trimmed();
    int offset = (m_currentPage - 1) * m_pageSize;

    ui->statusLabel->setText(tr("Sorgulanıyor..."));
    m_session->sendAuditLogQuery(
        from.toSecsSinceEpoch(),
        to.toSecsSinceEpoch(),
        categoryStr,
        m_pageSize,
        offset,
        searchQuery);
}

void ReportPanel::onEventsReceived(const QList<AuditEvent>& events, int totalCount) {
    m_events = events;
    m_totalCount = totalCount;
    m_totalPages = std::max(1, (m_totalCount + m_pageSize - 1) / m_pageSize);

    if (m_currentPage > m_totalPages) {
        m_currentPage = m_totalPages;
    }

    ui->eventTable->setRowCount(0);

    for (const auto& e : events) {
        int row = ui->eventTable->rowCount();
        ui->eventTable->insertRow(row);

        auto* tsItem = new OkidiaTableItem(
            e.timestamp.toString("dd.MM.yyyy HH:mm:ss"));
        auto* catItem = new OkidiaTableItem(
            AuditLog::categoryDisplayName(e.category));
        catItem->setIcon(categoryIcon(e.category));
        auto* actorItem  = new OkidiaTableItem(e.actor);
        auto* targetItem = new OkidiaTableItem(friendlyTargetName(e.target));
        auto* detailItem = new OkidiaTableItem(e.detail);

        QColor catColor = categoryColor(e.category);
        catItem->setForeground(catColor);

        if (e.category == AuditEvent::Security) {
            for (auto* item : {tsItem, catItem, actorItem, targetItem, detailItem}) {
                item->setForeground(QColor("#f38ba8"));
            }
        }

        ui->eventTable->setItem(row, 0, tsItem);
        ui->eventTable->setItem(row, 1, catItem);
        ui->eventTable->setItem(row, 2, actorItem);
        ui->eventTable->setItem(row, 3, targetItem);
        ui->eventTable->setItem(row, 4, detailItem);
    }

    updatePaginationUi();
}

void ReportPanel::updatePaginationUi() {
    ui->pageLabel->setText(
        QString(tr("Sayfa %1 / %2 (Toplam %3 olay)"))
            .arg(m_currentPage)
            .arg(m_totalPages)
            .arg(m_totalCount));

    ui->btnFirstPage->setEnabled(m_currentPage > 1);
    ui->btnPrevPage->setEnabled(m_currentPage > 1);
    ui->btnNextPage->setEnabled(m_currentPage < m_totalPages);
    ui->btnLastPage->setEnabled(m_currentPage < m_totalPages);

    ui->statusLabel->setText(
        QString(tr("Hazır (%1 gösteriliyor)")).arg(m_events.size()));
}

void ReportPanel::onRowDoubleClicked(int row, int /*column*/) {
    if (row < 0 || row >= m_events.size()) return;
    showEventDetailDialog(m_events.at(row));
}

void ReportPanel::showEventDetailDialog(const AuditEvent& event) {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Denetim Olayı Detayı — OKIDIA"));
    dlg.resize(560, 420);

    auto* mainLay = new QVBoxLayout(&dlg);
    mainLay->setSpacing(12);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    auto* tsLbl = new QLabel(event.timestamp.toString("dd.MM.yyyy HH:mm:ss"), &dlg);
    auto* catLbl = new QLabel(AuditLog::categoryDisplayName(event.category), &dlg);
    catLbl->setStyleSheet(QString("font-weight: bold; color: %1;").arg(categoryColor(event.category).name()));

    auto* actorLbl = new QLabel(event.actor.isEmpty() ? "-" : event.actor, &dlg);
    QString dispTarget = friendlyTargetName(event.target);
    auto* targetLbl = new QLabel(dispTarget.isEmpty() ? "-" : dispTarget, &dlg);

    form->addRow(tr("Zaman:"), tsLbl);
    form->addRow(tr("Kategori:"), catLbl);
    form->addRow(tr("Aktör:"), actorLbl);
    form->addRow(tr("Hedef:"), targetLbl);

    if (!event.userId.isEmpty()) {
        auto* uidLbl = new QLabel(event.userId, &dlg);
        form->addRow(tr("Kullanıcı ID:"), uidLbl);
    }

    mainLay->addLayout(form);

    auto* detailLbl = new QLabel(tr("Açıklama / Parametreler:"), &dlg);
    detailLbl->setStyleSheet("font-weight: 500;");
    mainLay->addWidget(detailLbl);

    auto* detailText = new QPlainTextEdit(&dlg);
    detailText->setPlainText(event.detail);
    detailText->setReadOnly(true);
    mainLay->addWidget(detailText);

    auto* btnRow = new QHBoxLayout();
    auto* copyBtn = new QPushButton(tr("Detayı Kopyala"), &dlg);
    copyBtn->setIcon(ThemeManager::icon(":/ui/edit.svg"));
    connect(copyBtn, &QPushButton::clicked, &dlg, [event, &dlg]() {
        QGuiApplication::clipboard()->setText(event.detail);
        QMessageBox::information(&dlg, tr("Kopyalandı"), tr("Olay açıklaması panoya kopyalandı."));
    });

    auto* closeBtn = new QPushButton(tr("Kapat"), &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    btnRow->addWidget(copyBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);

    mainLay->addLayout(btnRow);
    dlg.exec();
}

void ReportPanel::onExportCsv() {
    QString defaultName = QDir::homePath() + "/okidia_denetim_" +
                          QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".csv";
    QString path = OkidiaUtils::getSaveFileName(
        this, tr("CSV / Excel Olarak Kaydet"),
        "reports",
        defaultName,
        tr("CSV Dosyaları (*.csv)"));
    if (path.isEmpty()) return;

    if (exportToCsv(path)) {
        QMessageBox::information(this, tr("Dışa Aktarıldı"),
            tr("Rapor CSV / Excel formatında başarıyla kaydedildi:\n") + path);
    } else {
        QMessageBox::warning(this, tr("Hata"),
            tr("Dosya kaydedilemedi:\n") + path);
    }
}

void ReportPanel::onExportPdf() {
    QString defaultName = QDir::homePath() + "/okidia_denetim_" +
                          QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".pdf";
    QString path = OkidiaUtils::getSaveFileName(
        this, tr("PDF Raporu Olarak Kaydet"),
        "reports",
        defaultName,
        tr("PDF Belgeleri (*.pdf)"));
    if (path.isEmpty()) return;

    if (exportToPdf(path)) {
        QMessageBox::information(this, tr("Dışa Aktarıldı"),
            tr("Rapor vektörel PDF olarak başarıyla oluşturuldu:\n") + path);
    } else {
        QMessageBox::warning(this, tr("Hata"),
            tr("PDF dosyası oluşturulamadı:\n") + path);
    }
}

void ReportPanel::onExportHtml() {
    QString defaultName = QDir::homePath() + "/okidia_denetim_" +
                          QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".html";
    QString path = OkidiaUtils::getSaveFileName(
        this, tr("HTML Raporu Olarak Kaydet"),
        "reports",
        defaultName,
        tr("HTML Belgeleri (*.html)"));
    if (path.isEmpty()) return;

    if (exportToHtml(path)) {
        QMessageBox::information(this, tr("Dışa Aktarıldı"),
            tr("Rapor HTML formatında başarıyla kaydedildi:\n") + path);
    } else {
        QMessageBox::warning(this, tr("Hata"),
            tr("HTML dosyası kaydedilemedi:\n") + path);
    }
}

bool ReportPanel::exportToCsv(const QString& filePath) const {
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    // Excel Türkçe ve UTF-8 BOM desteği
    out << "\xEF\xBB\xBF";
    out << tr("Zaman;Kategori;Aktör;Hedef;Açıklama\n");

    for (const auto& e : m_events) {
        QString ts  = e.timestamp.toString("dd.MM.yyyy HH:mm:ss");
        QString cat = AuditLog::categoryDisplayName(e.category);
        QString act = QString(e.actor).replace(';', ',');
        QString tgt = QString(friendlyTargetName(e.target)).replace(';', ',');
        QString det = QString(e.detail).replace(';', ',');
        det.replace('\n', ' ');
        det.replace('\r', ' ');

        out << ts << ";" << cat << ";\"" << act << "\";\"" << tgt << "\";\"" << det << "\"\n";
    }
    return true;
}

QString ReportPanel::generateHtmlReport(bool forPrint) const {
    QString html;
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<title>" + tr("OKIDIA Denetim ve Faaliyet Raporu") + "</title>";
    html += "<style>";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; margin: " + QString(forPrint ? "15px" : "30px") + "; color: #333; }";
    html += "h1 { color: #1e88e5; font-size: 22px; margin-bottom: 4px; }";
    html += ".meta { font-size: 12px; color: #666; margin-bottom: 20px; }";
    html += "table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 12px; }";
    html += "th { background-color: #f1f5f9; color: #334155; text-align: left; padding: 8px 10px; border-bottom: 2px solid #cbd5e1; }";
    html += "td { padding: 8px 10px; border-bottom: 1px solid #e2e8f0; vertical-align: top; }";
    html += "tr:nth-child(even) { background-color: #f8fafc; }";
    html += ".badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-weight: bold; font-size: 11px; }";
    html += ".sec { color: #e11d48; font-weight: bold; }";
    html += ".footer { margin-top: 25px; font-size: 11px; color: #94a3b8; text-align: center; }";
    html += "</style></head><body>";

    html += "<h1>" + tr("OKIDIA Denetim ve Faaliyet Raporu") + "</h1>";
    html += "<div class='meta'>";
    html += "<b>" + tr("Rapor Tarihi:") + "</b> " + QDateTime::currentDateTime().toString("dd.MM.yyyy HH:mm:ss") + " &nbsp;|&nbsp; ";
    html += "<b>" + tr("Filtre:") + "</b> " + ui->fromEdit->dateTime().toString("dd.MM.yyyy HH:mm") + " - " + ui->toEdit->dateTime().toString("dd.MM.yyyy HH:mm") + " &nbsp;|&nbsp; ";
    html += "<b>" + tr("Kayıt Sayısı:") + "</b> " + QString::number(m_events.size());
    if (m_totalCount > m_events.size()) {
        html += " (" + QString(tr("Toplam %1 kayıttan")).arg(m_totalCount) + ")";
    }
    html += "</div>";

    html += "<table><thead><tr>";
    html += "<th>" + tr("Zaman") + "</th>";
    html += "<th>" + tr("Kategori") + "</th>";
    html += "<th>" + tr("Aktör") + "</th>";
    html += "<th>" + tr("Hedef") + "</th>";
    html += "<th>" + tr("Açıklama") + "</th>";
    html += "</tr></thead><tbody>";

    for (const auto& e : m_events) {
        bool isSec = (e.category == AuditEvent::Security);
        html += isSec ? "<tr class='sec'>" : "<tr>";
        html += "<td>" + e.timestamp.toString("dd.MM.yyyy HH:mm:ss") + "</td>";
        html += "<td>" + AuditLog::categoryDisplayName(e.category) + "</td>";
        html += "<td>" + e.actor.toHtmlEscaped() + "</td>";
        html += "<td>" + friendlyTargetName(e.target).toHtmlEscaped() + "</td>";
        html += "<td>" + e.detail.toHtmlEscaped() + "</td>";
        html += "</tr>";
    }

    html += "</tbody></table>";
    html += "<div class='footer'>" + tr("Bu rapor OKIDIA Merkezi Yönetim Konsolu tarafından otomatik olarak üretilmiştir.") + "</div>";
    html += "</body></html>";
    return html;
}

bool ReportPanel::exportToHtml(const QString& filePath) const {
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << generateHtmlReport(false);
    return true;
}

bool ReportPanel::exportToPdf(const QString& filePath) const {
    QPdfWriter writer(filePath);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageOrientation(QPageLayout::Landscape);
    writer.setResolution(150);

    QTextDocument doc;
    doc.setHtml(generateHtmlReport(true));
    doc.print(&writer);
    return true;
}

QIcon ReportPanel::categoryIcon(AuditEvent::Category c) const {
    switch (c) {
    case AuditEvent::Auth:         return ThemeManager::icon(":/ui/key.svg");
    case AuditEvent::Command:      return ThemeManager::icon(":/ui/play_cmd.svg");
    case AuditEvent::FileTransfer: return ThemeManager::icon(":/ui/file.svg");
    case AuditEvent::Chat:         return ThemeManager::icon(":/ui/chat.svg");
    case AuditEvent::System:       return ThemeManager::icon(":/ui/home.svg");
    case AuditEvent::Security:     return ThemeManager::icon(":/ui/lock.svg");
    case AuditEvent::Power:        return ThemeManager::icon(":/ui/power.svg");
    case AuditEvent::Deployment:   return ThemeManager::icon(":/ui/network.svg");
    case AuditEvent::Exam:         return ThemeManager::icon(":/ui/graduation.svg");
    default:                       return ThemeManager::icon(":/ui/chart.svg");
    }
}

QColor ReportPanel::categoryColor(AuditEvent::Category c) const {
    switch (c) {
    case AuditEvent::Auth:         return QColor("#f9e2af"); // Sarı/Gold
    case AuditEvent::Command:      return QColor("#89b4fa"); // Mavi
    case AuditEvent::FileTransfer: return QColor("#a6e3a1"); // Yeşil
    case AuditEvent::Chat:         return QColor("#94e2d5"); // Camgöbeği
    case AuditEvent::System:       return QColor("#bac2de"); // Gri/Mavimsi
    case AuditEvent::Security:     return QColor("#f38ba8"); // Kırmızı
    case AuditEvent::Power:        return QColor("#fab387"); // Turuncu
    case AuditEvent::Deployment:   return QColor("#b4befe"); // Lavanta
    case AuditEvent::Exam:         return QColor("#cba6f7"); // Mor
    default:                       return QColor("#cdd6f4");
    }
}
