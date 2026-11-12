// agent/ui/BellToastBanner/BellToastBanner.cpp
#include "BellToastBanner.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/utils/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QTimer>
#include <QMouseEvent>

BellToastBanner* BellToastBanner::s_activeBanner = nullptr;

void BellToastBanner::showBanner(const QString& title, const QString& message, int durationMs) {
    closeActive();

    auto* banner = new BellToastBanner(title, message, durationMs);
    s_activeBanner = banner;
    banner->show();
    banner->raise();
}

void BellToastBanner::closeActive() {
    if (s_activeBanner) {
        s_activeBanner->close();
        s_activeBanner = nullptr;
    }
}

BellToastBanner::BellToastBanner(const QString& title, const QString& message, int durationMs, QWidget* parent)
    : QWidget(parent,
              Qt::FramelessWindowHint |
              Qt::WindowStaysOnTopHint |
              Qt::Tool |
              Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);

    setupUi(title, message);
    positionToTopRight();

    if (durationMs > 0) {
        m_dismissTimer = new QTimer(this);
        m_dismissTimer->setSingleShot(true);
        connect(m_dismissTimer, &QTimer::timeout, this, &QWidget::close);
        m_dismissTimer->start(durationMs);
    }
}

BellToastBanner::~BellToastBanner() {
    if (s_activeBanner == this) {
        s_activeBanner = nullptr;
    }
}

void BellToastBanner::setupUi(const QString& title, const QString& message) {
    setFixedSize(380, 84);

    auto* card = new QWidget(this);
    card->setObjectName("card");
    card->setGeometry(0, 0, 380, 84);

    // Koyu temaya tam uyumlu, zarif kehribar/altın vurgulu cam tasarımı
    card->setStyleSheet(R"(
        QWidget#card {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #222538, stop:1 #161826);
            border: 1.5px solid #f59e0b;
            border-radius: 12px;
        }
        QLabel {
            background: transparent;
        }
    )");

    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(20);
    shadow->setColor(QColor(0, 0, 0, 160));
    shadow->setOffset(0, 4);
    card->setGraphicsEffect(shadow);

    auto* rootLayout = new QHBoxLayout(card);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(14);

    // Zil SVG İkonu
    auto* iconContainer = new QWidget(card);
    iconContainer->setFixedSize(44, 44);
    iconContainer->setStyleSheet(R"(
        background: rgba(245, 158, 11, 0.15);
        border: 1px solid rgba(245, 158, 11, 0.4);
        border-radius: 22px;
    )");

    auto* iconLayout = new QVBoxLayout(iconContainer);
    iconLayout->setContentsMargins(0, 0, 0, 0);
    iconLayout->setAlignment(Qt::AlignCenter);

    auto* ico = new QLabel(iconContainer);
    QIcon bellIcon = ThemeManager::icon(":/ui/bell.svg");
    ico->setPixmap(bellIcon.pixmap(24, 24));
    ico->setAlignment(Qt::AlignCenter);
    iconLayout->addWidget(ico);

    rootLayout->addWidget(iconContainer);

    // Metin Bölümü
    auto* textCol = new QVBoxLayout();
    textCol->setSpacing(3);
    textCol->setAlignment(Qt::AlignVCenter);

    auto* lblTitle = new QLabel(title, card);
    lblTitle->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: bold;");

    auto* lblMessage = new QLabel(message, card);
    lblMessage->setStyleSheet("color: #cbd5e1; font-size: 12px;");
    lblMessage->setWordWrap(true);

    textCol->addWidget(lblTitle);
    textCol->addWidget(lblMessage);

    rootLayout->addLayout(textCol);
    rootLayout->addStretch();
}

void BellToastBanner::positionToTopRight() {
    QScreen* screen = QApplication::primaryScreen();
    QRect geom = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    // Sağdan 24px, yukarıdan 24px mesafe
    move(geom.right() - width() - 24, geom.top() + 24);
}

void BellToastBanner::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    close();
}
