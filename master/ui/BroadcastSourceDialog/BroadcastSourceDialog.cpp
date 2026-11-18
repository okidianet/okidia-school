// master/ui/BroadcastSourceDialog/BroadcastSourceDialog.cpp
#include "BroadcastSourceDialog.h"
#include "ui_BroadcastSourceDialog.h"

#include <QScreen>
#include <QGuiApplication>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPainter>

BroadcastSourceDialog::BroadcastSourceDialog(int targetCount, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::BroadcastSourceDialog) {
    ui->setupUi(this);
    setWindowTitle(tr("Yayın Kaynağı Seç"));
    resize(560, 480);

    connect(ui->tabWidget, &QTabWidget::currentChanged,
            this, &BroadcastSourceDialog::onTabChanged);

    // Kalite slider — anlık değer göster
    connect(ui->sliderQuality, &QSlider::valueChanged, [this](int v) {
        ui->lblQualityVal->setText(QString::number(v));
    });

    // FPS Önerisi: Hedef istemci sayısına göre otomatik öneri
    if (targetCount >= 10) {
        // 10 veya daha fazla agent: Ağ krizi olmaması için 10-15 FPS öner
        ui->comboFps->setCurrentText("10 FPS");
        ui->lblFpsHint->setText(tr("(Yüksek istemci sayısı: 10-15 FPS önerilir)"));
    } else if (targetCount > 3) {
        ui->comboFps->setCurrentText("15 FPS");
        ui->lblFpsHint->setText(tr("(Önerilen: 15 FPS)"));
    } else if (targetCount > 0) {
        ui->comboFps->setCurrentText("20 FPS");
        ui->lblFpsHint->setText(tr("(Az sayıda istemci: Akıcı)"));
    } else {
        ui->comboFps->setCurrentText("10 FPS");
    }

    connect(ui->monitorList, &QListWidget::currentRowChanged,
            this, &BroadcastSourceDialog::onMonitorSelected);

    populateMonitors();
    updateAllPreview();

    // Varsayılan: Monitörler sekmesi
    ui->tabWidget->setCurrentIndex(0);
    if (ui->monitorList->count() > 0) {
        onMonitorSelected(0);
    } else {
        m_result.type  = BroadcastSource::AllMonitors;
        m_result.label = tr("Tüm Ekranlar");
    }
}

BroadcastSourceDialog::~BroadcastSourceDialog() {
    delete ui;
}

BroadcastSource BroadcastSourceDialog::selectedSource() const {
    BroadcastSource result = m_result;
    result.quality = ui->sliderQuality->value();
    // FPS metni: "10 FPS" → 10
    const QString fpsText = ui->comboFps->currentText();
    result.fps = fpsText.left(fpsText.indexOf(' ')).toInt();
    if (result.fps <= 0) result.fps = 10;
    result.targetStudents = ui->chkTargetStudents->isChecked();
    result.targetSmartboards = ui->chkTargetSmartboards->isChecked();
    return result;
}

// ---------------------------------------------------------------------------
void BroadcastSourceDialog::populateMonitors() {
    ui->monitorList->clear();
    const auto screens = QGuiApplication::screens();
    QScreen*   primary = QGuiApplication::primaryScreen();

    for (int i = 0; i < screens.size(); ++i) {
        QScreen* s = screens[i];
        QString label = QString(tr("Ekran %1: %2\n%3 × %4"))
            .arg(i + 1)
            .arg(s->name())
            .arg(s->geometry().width())
            .arg(s->geometry().height());
        if (s == primary) label += tr("\n(Birincil)");

        // Küçük önizleme ikonu
        QPixmap preview = grabMonitor(i).scaled(
            48, 27, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        auto* item = new QListWidgetItem(QIcon(preview), label);
        item->setData(Qt::UserRole, i);
        ui->monitorList->addItem(item);
    }

    if (ui->monitorList->count() > 0)
        ui->monitorList->setCurrentRow(0);
}

// ---------------------------------------------------------------------------
void BroadcastSourceDialog::onTabChanged(int idx) {
    switch (idx) {
    case 0:  // Monitörler
        onMonitorSelected(ui->monitorList->currentRow());
        break;
    case 1:  // Tüm Ekranlar
        m_result.type  = BroadcastSource::AllMonitors;
        m_result.label = tr("Tüm Ekranlar");
        updateAllPreview();
        break;
    }
}

void BroadcastSourceDialog::onMonitorSelected(int row) {
    if (row < 0) return;
    auto* item = ui->monitorList->item(row);
    if (!item) return;

    int idx = item->data(Qt::UserRole).toInt();
    m_result.type         = BroadcastSource::Monitor;
    m_result.monitorIndex = idx;

    const auto screens = QGuiApplication::screens();
    QString name = (idx < screens.size())
        ? screens[idx]->name()
        : QString::number(idx);
    m_result.label = QString(tr("Ekran %1: %2")).arg(idx + 1).arg(name);

    // Önizleme güncelle
    QPixmap preview = grabMonitor(idx).scaled(
        ui->monitorPreview->width() - 4,
        ui->monitorPreview->height() - 4,
        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->monitorPreview->setPixmap(preview);

    if (idx < screens.size()) {
        auto g = screens[idx]->geometry();
        ui->monitorInfoLabel->setText(
            QString("%1 × %2  @  (%3, %4)")
                .arg(g.width()).arg(g.height())
                .arg(g.x()).arg(g.y()));
    }
}

void BroadcastSourceDialog::updateAllPreview() {
    // Tüm monitörlerin birleşik önizlemesi
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) return;

    QRect total;
    for (QScreen* s : screens)
        total = total.united(s->geometry());

    QPixmap canvas(total.size());
    canvas.fill(Qt::black);
    QPainter p(&canvas);

    for (QScreen* s : screens) {
        QPixmap px = s->grabWindow(0);
        if (px.isNull()) continue;
        QRect rel = s->geometry().translated(-total.topLeft());
        p.drawPixmap(rel, px);

        // Monitör sınırlarını çiz
        p.setPen(QPen(QColor("#89b4fa"), 3));
        p.drawRect(rel);
        p.setPen(QColor("#cdd6f4"));
        p.drawText(rel.adjusted(4, 4, 0, 0), s->name());
    }
    p.end();

    ui->allPreview->setPixmap(
        canvas.scaled(ui->allPreview->width() - 4,
                      ui->allPreview->height() - 4,
                      Qt::KeepAspectRatio,
                      Qt::SmoothTransformation));

    ui->allInfoLabel->setText(
        QString(tr("%1 monitör  —  %2 × %3 toplam alan"))
            .arg(screens.size())
            .arg(total.width())
            .arg(total.height()));
}

// ---------------------------------------------------------------------------
QPixmap BroadcastSourceDialog::grabMonitor(int idx) const {
    const auto screens = QGuiApplication::screens();
    if (idx >= screens.size())
        idx = 0;
    if (screens.isEmpty()) return {};
    return screens[idx]->grabWindow(0);
}
