#include "ScreenBroadcaster.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QApplication>
#include <QScreen>
#include <QPixmap>
#include <QBuffer>
#include <QDateTime>
#include <QPainter>
#include <QCursor>

ScreenBroadcaster::ScreenBroadcaster(Connection* conn, QObject* parent)
    : QObject(parent), m_conn(conn) {
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout,
            this, &ScreenBroadcaster::captureAndSend);
}

void ScreenBroadcaster::start(const BroadcastSource& source, const QStringList& targetClientIds) {
    if (!m_conn || m_timer->isActive()) return;
    m_source = source;
    m_targetClientIds = targetClientIds;
    m_jpegQuality = qBound(20, source.quality, 95);

    int fps = qBound(1, source.fps, 30);
    sendBegin();
    m_timer->start(1000 / fps);

    Logger::info(QObject::tr("ScreenBroadcaster: Yayın başladı @ ") +
                 QString::number(fps) + QObject::tr(" FPS — Kalite: %1 — %2 (Hedef: %3)")
                 .arg(m_jpegQuality).arg(source.label)
                 .arg(targetClientIds.isEmpty() ? QObject::tr("Tümü") : QString::number(targetClientIds.size())));
    emit started();
}

void ScreenBroadcaster::stop() {
    if (!m_timer->isActive()) return;
    m_timer->stop();
    sendEnd();
    Logger::info(QObject::tr("ScreenBroadcaster: Yayın durduruldu"));
    emit stopped();
}

void ScreenBroadcaster::setQuality(int quality) {
    int clamped = qBound(20, quality, 95);
    if (m_jpegQuality != clamped) {
        m_jpegQuality = clamped;
        Logger::info(QObject::tr("ScreenBroadcaster: Yayın kalitesi güncellendi -> %1").arg(m_jpegQuality));
    }
}

QPixmap ScreenBroadcaster::grabSource() const {
    switch (m_source.type) {

    case BroadcastSource::Monitor: {
        const auto screens = QGuiApplication::screens();
        int idx = m_source.monitorIndex;
        QScreen* s = (idx < screens.size())
            ? screens[idx]
            : QGuiApplication::primaryScreen();
        if (!s) break;
        return s->grabWindow(0);
    }

    case BroadcastSource::AllMonitors: {
        const auto screens = QGuiApplication::screens();
        if (screens.isEmpty()) break;

        QRect total;
        for (QScreen* s : screens)
            total = total.united(s->geometry());

        QPixmap canvas(total.size());
        canvas.fill(Qt::black);
        QPainter p(&canvas);
        for (QScreen* s : screens) {
            QPixmap px = s->grabWindow(0);
            if (!px.isNull())
                p.drawPixmap(
                    s->geometry().translated(-total.topLeft()), px);
        }
        p.end();
        return canvas;
    }

    default:
        break;
    }

    // Fallback: birincil ekran
    QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->grabWindow(0) : QPixmap();
}

void ScreenBroadcaster::drawCursor(QPixmap& px, int origW, int origH) const {
    if (origW <= 0 || origH <= 0 || px.isNull()) return;

    QPoint globalPos = QCursor::pos();
    QPoint localPos;

    if (m_source.type == BroadcastSource::AllMonitors) {
        QRect total;
        const auto screens = QGuiApplication::screens();
        for (QScreen* s : screens) {
            total = total.united(s->geometry());
        }
        localPos = globalPos - total.topLeft();
    } else if (m_source.type == BroadcastSource::Monitor) {
        const auto screens = QGuiApplication::screens();
        int idx = m_source.monitorIndex;
        QScreen* screen = (idx >= 0 && idx < screens.size()) ? screens[idx] : QGuiApplication::primaryScreen();
        if (screen) {
            localPos = globalPos - screen->geometry().topLeft();
        } else {
            localPos = globalPos;
        }
    } else {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (screen) {
            localPos = globalPos - screen->geometry().topLeft();
        } else {
            localPos = globalPos;
        }
    }

    // Ekran çözünürlüğü ölçeklenmişse fare imleç koordinatını da ölçekle
    float sx = static_cast<float>(px.width()) / static_cast<float>(origW);
    float sy = static_cast<float>(px.height()) / static_cast<float>(origH);
    QPoint drawPos(qRound(localPos.x() * sx), qRound(localPos.y() * sy));

    if (px.rect().contains(drawPos)) {
        QPainter painter(&px);
        painter.setRenderHint(QPainter::Antialiasing);

        // Standart ok imleç (beyaz dolgu, 1.5px siyah kontur)
        QPolygon cursorPoly;
        cursorPoly << QPoint(0, 0)
                   << QPoint(0, 17)
                   << QPoint(4, 13)
                   << QPoint(8, 21)
                   << QPoint(10, 20)
                   << QPoint(6, 12)
                   << QPoint(11, 12);

        painter.translate(drawPos);
        painter.setBrush(Qt::white);
        painter.setPen(QPen(Qt::black, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolygon(cursorPoly);
    }
}

void ScreenBroadcaster::captureAndSend() {
    if (!m_conn || !m_conn->isConnected()) { stop(); return; }

    // Strict Backpressure: Ağ tıkanıklığı / buffer bloat koruması.
    // Eğer TLS soketinde 200 KB'tan fazla veri birikmişse ekranı hiç yakalamadan
    // ve CPU ile JPEG encode etmeden doğrudan atla (Zero CPU/Memory wasted).
    if (m_conn->bytesToWrite() > 200 * 1024) {
        return;
    }

    QPixmap px = grabSource();
    if (px.isNull()) return;

    int origW = px.width();
    int origH = px.height();

    // Dinamik Çözünürlük ve Bant Genişliği Yönetimi:
    // Körleme %50 küçültme yerine, seçilen kalite seviyesine göre akıllı ölçekleme:
    if (m_jpegQuality >= 70) {
        // Yüksek Netlik Modu (Metin / Slayt / Kod):
        // 1080p ve altındaki ekranlar 1:1 tam netlikte korunur.
        // 4K veya çok geniş ekranlarda ağ krizini önlemek için en fazla 1920x1080 tavanı uygulanır.
        if (px.width() > 1920 || px.height() > 1080) {
            px = px.scaled(1920, 1080, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    } else if (m_jpegQuality >= 50) {
        // Dengeli Mod: %75 çözünürlük (Örn: 1080p -> 1440x810)
        px = px.scaled(origW * 3 / 4, origH * 3 / 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        // Tasarruf Modu (Zayıf Ağ): %50 çözünürlük (Örn: 1080p -> 960x540)
        px = px.scaled(origW / 2, origH / 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Fare imlecini ölçeklenmiş koordinatlara göre kare üzerine çiz
    drawCursor(px, origW, origH);

    m_jpegBuffer.clear();
    QBuffer buf(&m_jpegBuffer);
    buf.open(QIODevice::WriteOnly);
    px.save(&buf, "JPEG", m_jpegQuality);
    buf.close();
    if (m_jpegBuffer.isEmpty()) return;

    okidia::BroadcastFrame frame;
    frame.set_width(static_cast<uint32_t>(px.width()));
    frame.set_height(static_cast<uint32_t>(px.height()));
    frame.set_jpeg_data(m_jpegBuffer.constData(), m_jpegBuffer.size());
    frame.set_timestamp(
        OkidiaUtils::nowSecsU64());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROADCAST_FRAME);
    env.set_sequence_id(0);
    env.set_payload(frame.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void ScreenBroadcaster::sendBegin() {
    okidia::BroadcastBegin beginMsg;
    for (const QString& id : m_targetClientIds) {
        beginMsg.add_target_ids(id.toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROADCAST_BEGIN);
    env.set_sequence_id(0);
    env.set_payload(beginMsg.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void ScreenBroadcaster::sendEnd() {
    if (!m_conn || !m_conn->isConnected()) return;
    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROADCAST_END);
    env.set_sequence_id(0);
    m_conn->sendEnvelope(env);
}
