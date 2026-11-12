// agent/broadcast/ScreenBroadcastReceiver.cpp
#include "ScreenBroadcastReceiver.h"
#include "../../common/utils/Logger.h"

#include <QKeyEvent>
#include <QCloseEvent>
#include <QPainter>
#include <QLabel>
#include <QVBoxLayout>
#include <QApplication>
#include <QScreen>

// ---------------------------------------------------------------------------
ScreenBroadcastReceiver& ScreenBroadcastReceiver::instance() {
    static ScreenBroadcastReceiver s;
    return s;
}

ScreenBroadcastReceiver::ScreenBroadcastReceiver(QWidget* parent)
    : QWidget(parent,
              Qt::Window
            | Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
            | Qt::X11BypassWindowManagerHint) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setStyleSheet("background: #000000;");
    setFocusPolicy(Qt::StrongFocus);
}

// ---------------------------------------------------------------------------
void ScreenBroadcastReceiver::handleEnvelope(
    const okidia::Envelope& envelope) {
    switch (envelope.type()) {
    case okidia::MessageType::BROADCAST_BEGIN:
        handleBegin();
        break;
    case okidia::MessageType::BROADCAST_FRAME: {
        okidia::BroadcastFrame msg;
        if (msg.ParseFromString(envelope.payload()))
            handleFrame(msg);
        break;
    }
    case okidia::MessageType::BROADCAST_END:
        handleEnd();
        break;
    default:
        break;
    }
}

void ScreenBroadcastReceiver::handleBegin() {
    m_isBroadcasting = true;
    Logger::info(QObject::tr("ScreenBroadcastReceiver: Ekran yayını başladı"));
    QRect geom = QApplication::primaryScreen()->geometry();
    setGeometry(geom);
    showFullScreen();
    raise();
    activateWindow();
    setFocus();
    emit broadcastStarted();
}

void ScreenBroadcastReceiver::handleFrame(
    const okidia::BroadcastFrame& msg) {
    const auto& jpegStr = msg.jpeg_data();
    if (jpegStr.empty()) return;

    // Latest-Frame-Only drop: Eğer GUI iş parçacığı henüz önceki kareyi
    // çizmekle meşgulse (renderPending), ara kareleri decode etmeden atla;
    // doğrudan en son gelen baytları sakla.
    m_pendingJpegData = QByteArray(jpegStr.data(), static_cast<int>(jpegStr.size()));
    if (!m_renderPending) {
        m_renderPending = true;
        // Bir sonraki event loop döngüsünde decode & paint
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_pendingJpegData.isEmpty()) {
                QPixmap px;
                if (px.loadFromData(m_pendingJpegData, "JPEG")) {
                    m_frame = px;
                    update(); // paintEvent tetikle
                }
                m_pendingJpegData.clear();
            }
            m_renderPending = false;
        }, Qt::QueuedConnection);
    }
}

void ScreenBroadcastReceiver::handleEnd() {
    if (!m_isBroadcasting && isHidden()) return;
    m_isBroadcasting = false;
    Logger::info(QObject::tr("ScreenBroadcastReceiver: Ekran yayını bitti"));
    m_frame = QPixmap();
    m_pendingJpegData.clear();
    m_renderPending = false;
    hide();
    emit broadcastEnded();
}

// ---------------------------------------------------------------------------
void ScreenBroadcastReceiver::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (!m_frame.isNull()) {
        QPixmap scaled = m_frame.scaled(
            size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        int x = (width()  - scaled.width())  / 2;
        int y = (height() - scaled.height()) / 2;
        p.drawPixmap(x, y, scaled);
    } else {
        p.setPen(QColor("#6c7086"));
        p.setFont(QFont("sans", 16));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("Öğretmenin ekranı bekleniyor..."));
    }
}

void ScreenBroadcastReceiver::keyPressEvent(QKeyEvent* event) {
    // Yayın sırasında tüm klavye girişlerini yut (Öğrenci kaçamaz)
    event->accept();
}

void ScreenBroadcastReceiver::closeEvent(QCloseEvent* event) {
    event->ignore(); // Öğrenci kapatamaz
}
