// master/ui/LogViewerDialog/LogViewerDialog.cpp
#include "LogViewerDialog.h"
#include "../../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QFontDatabase>

// ── LogSyntaxHighlighter ─────────────────────────────────────────────────────
LogSyntaxHighlighter::LogSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    HighlightingRule rule;

    // Zaman Damgası: yyyy-MM-dd hh:mm:ss
    QTextCharFormat timeFormat;
    timeFormat.setForeground(QColor("#00a896"));
    rule.pattern = QRegularExpression("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}");
    rule.format = timeFormat;
    m_rules.append(rule);

    // [ERROR]
    QTextCharFormat errorFormat;
    errorFormat.setForeground(QColor("#e63946"));
    errorFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("\\[ERROR\\]");
    rule.format = errorFormat;
    m_rules.append(rule);

    // [WARN]
    QTextCharFormat warnFormat;
    warnFormat.setForeground(QColor("#f4a261"));
    warnFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("\\[WARN\\s*\\]");
    rule.format = warnFormat;
    m_rules.append(rule);

    // [INFO]
    QTextCharFormat infoFormat;
    infoFormat.setForeground(QColor("#457b9d"));
    rule.pattern = QRegularExpression("\\[INFO\\s*\\]");
    rule.format = infoFormat;
    m_rules.append(rule);

    // [AUDIT]
    QTextCharFormat auditFormat;
    auditFormat.setForeground(QColor("#9d4edd"));
    auditFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("\\[AUDIT\\]");
    rule.format = auditFormat;
    m_rules.append(rule);

    // [DEBUG]
    QTextCharFormat debugFormat;
    debugFormat.setForeground(QColor("#8d99ae"));
    rule.pattern = QRegularExpression("\\[DEBUG\\]");
    rule.format = debugFormat;
    m_rules.append(rule);

    // Başlangıç / Ayrım satırları
    QTextCharFormat headerFormat;
    headerFormat.setForeground(QColor("#2a9d8f"));
    headerFormat.setFontWeight(QFont::Bold);
    rule.pattern = QRegularExpression("={5,}.*={5,}");
    rule.format = headerFormat;
    m_rules.append(rule);
}

void LogSyntaxHighlighter::highlightBlock(const QString& text) {
    for (const auto& rule : m_rules) {
        auto matchIterator = rule.pattern.globalMatch(text);
        while (matchIterator.hasNext()) {
            auto match = matchIterator.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }
}

// ── LogViewerDialog ──────────────────────────────────────────────────────────
LogViewerDialog::LogViewerDialog(SessionManager* session, QWidget* parent)
    : QDialog(parent), m_session(session), m_currentTargetId("local") {
    setupUi();

    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(300);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, &LogViewerDialog::onFilterChanged);

    if (m_session) {
        connect(m_session, &SessionManager::logTailReceived, this, &LogViewerDialog::onRemoteLogReceived);
    }

    loadSources();
    loadLocalFiles();
    onRefresh();
}

void LogViewerDialog::setupUi() {
    setWindowTitle(tr("Sistem Günlükleri"));
    setWindowIcon(ThemeManager::icon(":/ui/terminal.svg"));
    resize(1000, 680);
    setMinimumSize(800, 500);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    // ── 1. Filtre Alanı ───────────────────────────────────────────────────────
    auto* filterBox = new QGroupBox(tr("Filtreler ve Seçenekler"), this);
    auto* filterLayout = new QGridLayout(filterBox);
    filterLayout->setContentsMargins(10, 10, 10, 10);
    filterLayout->setHorizontalSpacing(12);
    filterLayout->setVerticalSpacing(8);

    // Satır 1: Kaynak, Dosya, Seviye, Satır Limiti
    auto* lblSource = new QLabel(tr("Kaynak:"), filterBox);
    m_sourceCombo = new QComboBox(filterBox);
    m_sourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    auto* lblFile = new QLabel(tr("Dosya:"), filterBox);
    m_fileCombo = new QComboBox(filterBox);
    m_fileCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    auto* lblLevel = new QLabel(tr("Seviye:"), filterBox);
    m_levelCombo = new QComboBox(filterBox);
    m_levelCombo->addItem(tr("Tümü"), "");
    m_levelCombo->addItem(tr("DEBUG"), "DEBUG");
    m_levelCombo->addItem(tr("INFO"), "INFO");
    m_levelCombo->addItem(tr("WARN"), "WARN");
    m_levelCombo->addItem(tr("ERROR"), "ERROR");
    m_levelCombo->addItem(tr("AUDIT"), "AUDIT");

    auto* lblLines = new QLabel(tr("Satır Limiti:"), filterBox);
    m_lineCountCombo = new QComboBox(filterBox);
    m_lineCountCombo->addItem(tr("100 Satır"), 100);
    m_lineCountCombo->addItem(tr("200 Satır"), 200);
    m_lineCountCombo->addItem(tr("500 Satır (Varsayılan)"), 500);
    m_lineCountCombo->addItem(tr("1000 Satır"), 1000);
    m_lineCountCombo->addItem(tr("2000 Satır"), 2000);
    m_lineCountCombo->addItem(tr("Tümü (Sınırsız)"), 0);
    m_lineCountCombo->setCurrentIndex(2); // 500 satır

    filterLayout->addWidget(lblSource, 0, 0);
    filterLayout->addWidget(m_sourceCombo, 0, 1);
    filterLayout->addWidget(lblFile, 0, 2);
    filterLayout->addWidget(m_fileCombo, 0, 3);
    filterLayout->addWidget(lblLevel, 0, 4);
    filterLayout->addWidget(m_levelCombo, 0, 5);
    filterLayout->addWidget(lblLines, 0, 6);
    filterLayout->addWidget(m_lineCountCombo, 0, 7);

    // Satır 2: Zaman Aralığı & Canlı Arama
    m_timeFilterCheck = new QCheckBox(tr("Saat Aralığı:"), filterBox);
    m_timeFilterCheck->setChecked(false);

    m_timeFromEdit = new QTimeEdit(QTime(0, 0, 0), filterBox);
    m_timeFromEdit->setDisplayFormat("hh:mm:ss");
    m_timeFromEdit->setEnabled(false);

    auto* lblTo = new QLabel(tr("-"), filterBox);

    m_timeToEdit = new QTimeEdit(QTime(23, 59, 59), filterBox);
    m_timeToEdit->setDisplayFormat("hh:mm:ss");
    m_timeToEdit->setEnabled(false);

    connect(m_timeFilterCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_timeFromEdit->setEnabled(checked);
        m_timeToEdit->setEnabled(checked);
        onFilterChanged();
    });

    auto* timeBox = new QHBoxLayout();
    timeBox->setSpacing(4);
    timeBox->addWidget(m_timeFilterCheck);
    timeBox->addWidget(m_timeFromEdit);
    timeBox->addWidget(lblTo);
    timeBox->addWidget(m_timeToEdit);

    auto* lblSearch = new QLabel(tr("Arama:"), filterBox);
    m_searchEdit = new QLineEdit(filterBox);
    m_searchEdit->setPlaceholderText(tr("Log içeriğinde canlı ara..."));
    m_searchEdit->setClearButtonEnabled(true);

    m_btnRefresh = new QPushButton(ThemeManager::icon(":/ui/refresh.svg"), tr("Yenile"), filterBox);

    filterLayout->addLayout(timeBox, 1, 0, 1, 4);
    filterLayout->addWidget(lblSearch, 1, 4);
    filterLayout->addWidget(m_searchEdit, 1, 5, 1, 2);
    filterLayout->addWidget(m_btnRefresh, 1, 7);

    mainLayout->addWidget(filterBox);

    // ── 2. Log Metin Alanı ────────────────────────────────────────────────────
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(10);
    m_logView->setFont(monoFont);
    m_highlighter = new LogSyntaxHighlighter(m_logView->document());
    mainLayout->addWidget(m_logView, 1);

    // ── 3. Durum Çubuğu ───────────────────────────────────────────────────────
    auto* statusLayout = new QHBoxLayout();
    m_statusLabel = new QLabel(tr("Hazır"), this);
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0);
    m_progressBar->setMaximumWidth(150);
    m_progressBar->setMaximumHeight(14);
    m_progressBar->setTextVisible(false);
    m_progressBar->hide();

    statusLayout->addWidget(m_statusLabel, 1);
    statusLayout->addWidget(m_progressBar);
    mainLayout->addLayout(statusLayout);

    // ── 4. Alt İşlem Düğmeleri ───────────────────────────────────────────────
    auto* bottomLayout = new QHBoxLayout();
    m_btnOpenFolder = new QPushButton(ThemeManager::icon(":/ui/folder.svg"), tr("Log Klasörünü Aç"), this);
    m_btnClear = new QPushButton(ThemeManager::icon(":/ui/trash.svg"), tr("Aktif Logu Sıfırla"), this);
    m_btnExport = new QPushButton(ThemeManager::icon(":/ui/save.svg"), tr("Dışa Aktar..."), this);
    m_btnClose = new QPushButton(tr("Kapat"), this);

    bottomLayout->addWidget(m_btnOpenFolder);
    bottomLayout->addWidget(m_btnClear);
    bottomLayout->addStretch(1);
    bottomLayout->addWidget(m_btnExport);
    bottomLayout->addWidget(m_btnClose);
    mainLayout->addLayout(bottomLayout);

    // Bağlantılar
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LogViewerDialog::onSourceChanged);
    connect(m_fileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LogViewerDialog::onFileChanged);
    connect(m_levelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LogViewerDialog::onFilterChanged);
    connect(m_lineCountCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LogViewerDialog::onFilterChanged);
    connect(m_timeFromEdit, &QTimeEdit::timeChanged, this, &LogViewerDialog::onFilterChanged);
    connect(m_timeToEdit, &QTimeEdit::timeChanged, this, &LogViewerDialog::onFilterChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, [this]() {
        m_searchDebounceTimer->start();
    });

    connect(m_btnRefresh, &QPushButton::clicked, this, &LogViewerDialog::onRefresh);
    connect(m_btnOpenFolder, &QPushButton::clicked, this, &LogViewerDialog::onOpenLogFolder);
    connect(m_btnClear, &QPushButton::clicked, this, &LogViewerDialog::onClearCurrentLog);
    connect(m_btnExport, &QPushButton::clicked, this, &LogViewerDialog::onExportLogs);
    connect(m_btnClose, &QPushButton::clicked, this, &QDialog::accept);
}

void LogViewerDialog::loadSources() {
    m_sourceCombo->blockSignals(true);
    m_sourceCombo->clear();

    // 1. Yerel (Master)
    m_sourceCombo->addItem(ThemeManager::icon(":/ui/monitor.svg"),
                           tr("Yerel (Master - Bu Bilgisayar)"), "local");

    // 2. Broker (Sunucu)
    m_sourceCombo->addItem(ThemeManager::icon(":/ui/network.svg"),
                           tr("Broker (Merkezi Sunucu)"), "broker");

    // 3. Online İstemciler
    if (m_session) {
        const auto states = m_session->clientStates();
        for (auto it = states.begin(); it != states.end(); ++it) {
            const auto& cs = it.value();
            if (cs.connected) {
                QString label = QString("%1 (%2)").arg(cs.hostname.isEmpty() ? cs.clientId : cs.hostname, cs.ip);
                m_sourceCombo->addItem(ThemeManager::icon(":/ui/student.svg"), label, cs.clientId);
            }
        }
    }

    m_sourceCombo->blockSignals(false);
}

void LogViewerDialog::loadLocalFiles() {
    m_fileCombo->blockSignals(true);
    m_fileCombo->clear();

    QString logDir = Logger::logDir();
    auto files = Logger::availableLogFiles(logDir);

    m_fileCombo->addItem(tr("Aktif Log (okidia-master.log)"), Logger::logFilePath());

    for (const auto& fi : files) {
        if (fi.filePath != Logger::logFilePath()) {
            QString label = QString("%1 (%2)")
                .arg(fi.fileName, OkidiaUtils::formatSize(fi.sizeBytes));
            m_fileCombo->addItem(label, fi.filePath);
        }
    }

    m_fileCombo->blockSignals(false);
}

void LogViewerDialog::setTarget(const QString& targetId) {
    int idx = m_sourceCombo->findData(targetId);
    if (idx >= 0) {
        m_sourceCombo->setCurrentIndex(idx);
    } else if (targetId != "local" && targetId != "broker") {
        // İstemci henüz listede yoksa ekle
        m_sourceCombo->addItem(ThemeManager::icon(":/ui/student.svg"),
                               tr("İstemci: %1").arg(targetId), targetId);
        m_sourceCombo->setCurrentIndex(m_sourceCombo->count() - 1);
    }
}

void LogViewerDialog::onSourceChanged(int index) {
    if (index < 0) return;
    m_currentTargetId = m_sourceCombo->currentData().toString();

    const bool isLocal = (m_currentTargetId == "local");
    m_btnOpenFolder->setEnabled(isLocal);
    m_btnClear->setEnabled(isLocal);

    if (isLocal) {
        loadLocalFiles();
        loadLocalLogs();
    } else {
        m_fileCombo->blockSignals(true);
        m_fileCombo->clear();
        m_fileCombo->addItem(tr("Aktif Log Dosyası"), "");
        m_fileCombo->blockSignals(false);
        requestRemoteLogs();
    }
}

void LogViewerDialog::onFileChanged(int index) {
    Q_UNUSED(index);
    onRefresh();
}

void LogViewerDialog::onFilterChanged() {
    onRefresh();
}

void LogViewerDialog::onRefresh() {
    if (m_currentTargetId == "local") {
        loadLocalLogs();
    } else {
        requestRemoteLogs();
    }
}

void LogViewerDialog::loadLocalLogs() {
    updateStatus(tr("Yerel loglar okunuyor..."), true);

    QString selectedFile = m_fileCombo->currentData().toString();
    if (selectedFile.isEmpty()) {
        selectedFile = Logger::logFilePath();
    }

    int maxLines = m_lineCountCombo->currentData().toInt();
    QString levelFilter = m_levelCombo->currentData().toString();
    QString textFilter = m_searchEdit->text().trimmed();

    QDateTime fromTime, toTime;
    if (m_timeFilterCheck->isChecked()) {
        QDate logDate = QDate::currentDate();
        static const QRegularExpression dateRx("(\\d{4}-\\d{2}-\\d{2})");
        auto m = dateRx.match(selectedFile);
        if (m.hasMatch()) {
            QDate parsed = QDate::fromString(m.captured(1), "yyyy-MM-dd");
            if (parsed.isValid()) logDate = parsed;
        }
        fromTime = QDateTime(logDate, m_timeFromEdit->time());
        toTime = QDateTime(logDate, m_timeToEdit->time());
    }

    QStringList lines = Logger::readLogLines(selectedFile, maxLines, fromTime, toTime, levelFilter, textFilter);
    m_logView->setPlainText(lines.join("\n"));
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());

    QFileInfo fi(selectedFile);
    qint64 sz = fi.size();
    updateStatus(tr("%1 satır listelendi | Dosya Boyutu: %2")
        .arg(lines.size())
        .arg(OkidiaUtils::formatSize(sz)), false);
}

void LogViewerDialog::requestRemoteLogs() {
    if (!m_session || !m_session->isConnected()) {
        updateStatus(tr("Broker bağlantısı yok!"), false);
        m_logView->setPlainText(tr("--- Broker bağlantısı çevrimdışı. Uzak loglar alınamıyor. ---"));
        return;
    }

    updateStatus(tr("Uzak sunucudan/cihazdan loglar alınıyor..."), true);

    QString targetFile = m_fileCombo->currentData().toString();
    int maxLines = m_lineCountCombo->currentData().toInt();
    QString levelFilter = m_levelCombo->currentData().toString();
    QString textFilter = m_searchEdit->text().trimmed();

    QString fromTimeStr, toTimeStr;
    if (m_timeFilterCheck->isChecked()) {
        fromTimeStr = m_timeFromEdit->time().toString("hh:mm:ss");
        toTimeStr = m_timeToEdit->time().toString("hh:mm:ss");
    }

    bool sent = m_session->sendLogTailRequest(m_currentTargetId,
                                             maxLines,
                                             fromTimeStr,
                                             toTimeStr,
                                             levelFilter,
                                             textFilter,
                                             targetFile);
    if (!sent) {
        updateStatus(tr("İstek gönderilemedi!"), false);
    }
}

void LogViewerDialog::onRemoteLogReceived(const QString& sourceId, const okidia::LogTailResponse& resp) {
    // Kaynak eşleşmesi kontrolü: boş veya "broker"
    bool matches = (sourceId == m_currentTargetId);
    if (!matches && (m_currentTargetId == "broker" && (sourceId.isEmpty() || sourceId == "broker"))) {
        matches = true;
    }

    if (!matches) {
        return;
    }

    if (!resp.success()) {
        updateStatus(tr("Hata: %1").arg(QString::fromStdString(resp.error_message())), false);
        m_logView->setPlainText(tr("--- Log okunamadı: %1 ---").arg(QString::fromStdString(resp.error_message())));
        return;
    }

    // Dosya listesini güncelle
    m_fileCombo->blockSignals(true);
    for (int i = 0; i < resp.available_files_size(); ++i) {
        QString f = QString::fromStdString(resp.available_files(i));
        if (m_fileCombo->findData(f) == -1) {
            m_fileCombo->addItem(f, f);
        }
    }
    m_fileCombo->blockSignals(false);

    QStringList lines;
    lines.reserve(resp.lines_size());
    for (int i = 0; i < resp.lines_size(); ++i) {
        lines.append(QString::fromStdString(resp.lines(i)));
    }

    m_logView->setPlainText(lines.join("\n"));
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());

    updateStatus(tr("%1 satır listelendi (%2) | Dosya Boyutu: %3")
        .arg(lines.size())
        .arg(QString::fromStdString(resp.file_name()))
        .arg(OkidiaUtils::formatSize(resp.file_size())), false);
}

void LogViewerDialog::updateStatus(const QString& msg, bool isBusy) {
    m_statusLabel->setText(msg);
    if (isBusy) {
        m_progressBar->show();
    } else {
        m_progressBar->hide();
    }
}

void LogViewerDialog::onOpenLogFolder() {
    QString dir = Logger::logDir();
    if (!dir.isEmpty() && QDir(dir).exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
}

void LogViewerDialog::onClearCurrentLog() {
    if (m_currentTargetId != "local") {
        QMessageBox::information(this, tr("Bilgi"), tr("Yalnızca yerel log dosyası doğrudan temizlenebilir."));
        return;
    }

    int ret = QMessageBox::question(this, tr("Günlüğü Temizle"),
        tr("Mevcut aktif log dosyasını sıfırlamak istediğinize emin misiniz?\n\n"
           "Not: Eski loglarınız 28 gün boyunca arşivde saklanmaya devam edecektir."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        if (Logger::clearCurrentLog()) {
            loadLocalLogs();
            QMessageBox::information(this, tr("Başarılı"), tr("Aktif log dosyası başarıyla temizlendi."));
        } else {
            QMessageBox::warning(this, tr("Hata"), tr("Log dosyası temizlenemedi."));
        }
    }
}

void LogViewerDialog::onExportLogs() {
    QString defaultName = QString("okidia_log_%1.txt").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString savePath = OkidiaUtils::getSaveFileName(this, tr("Logları Dışa Aktar"),
                                                    "logs",
                                                    defaultName,
                                                    tr("Metin Dosyaları (*.txt *.log);;Tüm Dosyalar (*.*)"));
    if (savePath.isEmpty()) return;

    QFile outFile(savePath);
    if (outFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&outFile);
        out << m_logView->toPlainText();
        outFile.close();
        QMessageBox::information(this, tr("Başarılı"), tr("Loglar kaydedildi:\n%1").arg(savePath));
    } else {
        QMessageBox::warning(this, tr("Hata"), tr("Dosya kaydedilemedi: %1").arg(savePath));
    }
}
