// master/ui/BroadcastOverlay/BroadcastOverlayWidget.cpp
#include "BroadcastOverlayWidget.h"
#include "../../../common/ui/ThemeManager.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QEnterEvent>

BroadcastOverlayWidget::BroadcastOverlayWidget(QWidget* parent)
    : QWidget(parent) {
    setupUi();

    m_collapseTimer = new QTimer(this);
    m_collapseTimer->setSingleShot(true);
    m_collapseTimer->setInterval(600); // 600 ms tolerans
    connect(m_collapseTimer, &QTimer::timeout, this, &BroadcastOverlayWidget::onCollapseTimeout);
}

void BroadcastOverlayWidget::setupUi() {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    m_mainLayout = new QHBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    m_container = new QWidget(this);
    m_container->setObjectName("broadcastOverlayContainer");
    m_container->setStyleSheet(
        "#broadcastOverlayContainer {"
        "  background-color: rgba(30, 30, 46, 0.95);"
        "  border: 1px solid #45475a;"
        "  border-top: none;"
        "  border-bottom-left-radius: 10px;"
        "  border-bottom-right-radius: 10px;"
        "}"
    );

    auto* containerLayout = new QHBoxLayout(m_container);
    containerLayout->setContentsMargins(10, 4, 10, 6);
    containerLayout->setSpacing(8);

    // --- 1. Daraltılmış (Collapsed) Durum ---
    m_collapsedBar = new QWidget(m_container);
    auto* collLayout = new QHBoxLayout(m_collapsedBar);
    collLayout->setContentsMargins(4, 0, 4, 0);
    collLayout->setSpacing(4);

    m_lblCollapsed = new QLabel(tr("● YAYINDA ▼"), m_collapsedBar);
    m_lblCollapsed->setStyleSheet("color: #f38ba8; font-weight: bold; font-size: 11px;");
    m_lblCollapsed->setAlignment(Qt::AlignCenter);
    collLayout->addWidget(m_lblCollapsed);

    containerLayout->addWidget(m_collapsedBar);

    // --- 2. Genişletilmiş (Expanded) Durum ---
    m_expandedBar = new QWidget(m_container);
    auto* expLayout = new QHBoxLayout(m_expandedBar);
    expLayout->setContentsMargins(0, 0, 0, 0);
    expLayout->setSpacing(10);

    // Canlı Yayın Rozeti
    m_lblLive = new QLabel(tr("● CANLI"), m_expandedBar);
    m_lblLive->setStyleSheet("color: #f38ba8; font-weight: bold; font-size: 11px;");
    expLayout->addWidget(m_lblLive);

    // Ayırıcı çizgi
    auto* sep1 = new QFrame(m_expandedBar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    sep1->setStyleSheet("color: #45475a;");
    expLayout->addWidget(sep1);

    // Kalite Başlığı
    m_lblQualityTitle = new QLabel(tr("Kalite:"), m_expandedBar);
    m_lblQualityTitle->setStyleSheet("color: #a6adc8; font-size: 11px;");
    expLayout->addWidget(m_lblQualityTitle);

    // Kalite Slider
    m_sliderQuality = new QSlider(Qt::Horizontal, m_expandedBar);
    m_sliderQuality->setRange(20, 95);
    m_sliderQuality->setValue(75);
    m_sliderQuality->setFixedWidth(110);
    m_sliderQuality->setToolTip(tr("Yayın kalitesini anlık ayarla"));
    connect(m_sliderQuality, &QSlider::valueChanged, this, &BroadcastOverlayWidget::onSliderValueChanged);
    connect(m_sliderQuality, &QSlider::sliderPressed, this, [this]() {
        m_isSliderDragging = true;
        m_collapseTimer->stop();
    });
    connect(m_sliderQuality, &QSlider::sliderReleased, this, [this]() {
        m_isSliderDragging = false;
        if (!rect().contains(mapFromGlobal(QCursor::pos())) && !m_isPinned) {
            m_collapseTimer->start();
        }
    });
    expLayout->addWidget(m_sliderQuality);

    // Kalite Değeri ve Mod Bilgisi (Örn: 75 Net Metin)
    m_lblQualityVal = new QLabel(m_expandedBar);
    m_lblQualityVal->setStyleSheet("color: #89b4fa; font-weight: bold; font-size: 11px;");
    m_lblQualityVal->setMinimumWidth(85);
    expLayout->addWidget(m_lblQualityVal);

    // Ayırıcı çizgi
    auto* sep2 = new QFrame(m_expandedBar);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setFrameShadow(QFrame::Sunken);
    sep2->setStyleSheet("color: #45475a;");
    expLayout->addWidget(sep2);

    // Sabitleme (Pin) Butonu
    m_btnPin = new QPushButton(m_expandedBar);
    m_btnPin->setCheckable(true);
    m_btnPin->setText(tr("Sabitle"));
    m_btnPin->setToolTip(tr("Menüyü ekranda açık tut"));
    m_btnPin->setStyleSheet(
        "QPushButton {"
        "  background: transparent; color: #a6adc8; border: 1px solid #45475a;"
        "  border-radius: 4px; padding: 2px 6px; font-size: 10px;"
        "}"
        "QPushButton:checked {"
        "  background: #45475a; color: #89b4fa; font-weight: bold;"
        "}"
    );
    connect(m_btnPin, &QPushButton::toggled, this, &BroadcastOverlayWidget::onPinToggled);
    expLayout->addWidget(m_btnPin);

    // Durdur Butonu
    m_btnStop = new QPushButton(m_expandedBar);
    m_btnStop->setText(tr("Durdur"));
    m_btnStop->setIcon(ThemeManager::icon(":/ui/stop.svg"));
    m_btnStop->setToolTip(tr("Ekran yayınını durdur"));
    m_btnStop->setStyleSheet(
        "QPushButton {"
        "  background: #f38ba8; color: #11111b; font-weight: bold;"
        "  border-radius: 5px; padding: 3px 10px; font-size: 11px;"
        "}"
        "QPushButton:hover { background: #eba0ac; }"
    );
    connect(m_btnStop, &QPushButton::clicked, this, &BroadcastOverlayWidget::stopRequested);
    expLayout->addWidget(m_btnStop);

    containerLayout->addWidget(m_expandedBar);
    m_mainLayout->addWidget(m_container);

    updateQualityLabel(m_sliderQuality->value());

    // Varsayılan olarak daraltılmış (collapsed) durum ile başla
    m_expandedBar->hide();
    m_collapsedBar->show();
    m_isExpanded = false;
    resize(120, 22);
}

void BroadcastOverlayWidget::setTargetScreen(QScreen* screen) {
    m_targetScreen = screen ? screen : QGuiApplication::primaryScreen();
    reposition();
}

void BroadcastOverlayWidget::setInitialQuality(int quality) {
    m_sliderQuality->blockSignals(true);
    m_sliderQuality->setValue(qBound(20, quality, 95));
    m_sliderQuality->blockSignals(false);
    updateQualityLabel(m_sliderQuality->value());
}

void BroadcastOverlayWidget::showOverlay() {
    collapse();
    show();
    raise();
    reposition();
}

void BroadcastOverlayWidget::hideOverlay() {
    m_collapseTimer->stop();
    m_isPinned = false;
    m_btnPin->setChecked(false);
    hide();
}

void BroadcastOverlayWidget::expand() {
    if (m_isExpanded) return;
    m_isExpanded = true;
    m_collapsedBar->hide();
    m_expandedBar->show();
    resize(460, 44);
    reposition();
}

void BroadcastOverlayWidget::collapse() {
    if (m_isPinned) return;
    m_isExpanded = false;
    m_expandedBar->hide();
    m_collapsedBar->show();
    resize(120, 22);
    reposition();
}

void BroadcastOverlayWidget::reposition() {
    QScreen* s = m_targetScreen ? m_targetScreen : QGuiApplication::primaryScreen();
    if (!s) return;

    QRect geom = s->geometry();
    int x = geom.x() + (geom.width() - width()) / 2;
    int y = geom.y(); // Ekranın tam üst kenarına yapışık
    move(x, y);
}

void BroadcastOverlayWidget::enterEvent(QEnterEvent*) {
    m_collapseTimer->stop();
    expand();
}

void BroadcastOverlayWidget::leaveEvent(QEvent*) {
    if (!m_isPinned && !m_isSliderDragging) {
        m_collapseTimer->start();
    }
}

void BroadcastOverlayWidget::onSliderValueChanged(int value) {
    updateQualityLabel(value);
    emit qualityChanged(value);
}

void BroadcastOverlayWidget::onPinToggled(bool checked) {
    m_isPinned = checked;
    if (m_isPinned) {
        m_collapseTimer->stop();
    } else {
        if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
            m_collapseTimer->start();
        }
    }
}

void BroadcastOverlayWidget::onCollapseTimeout() {
    if (!m_isPinned && !m_isSliderDragging) {
        collapse();
    }
}

void BroadcastOverlayWidget::updateQualityLabel(int value) {
    QString mode;
    if (value >= 70) {
        mode = tr("Net");
    } else if (value >= 50) {
        mode = tr("Dengeli");
    } else {
        mode = tr("Hızlı");
    }
    m_lblQualityVal->setText(QString("%1 (%2)").arg(value).arg(mode));
}

