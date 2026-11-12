// agent/broadcast/ScreenBroadcastReceiver.h
#pragma once

#include <QWidget>
#include <QPixmap>
#include "../../common/network/Connection.h"

/**
 * Master'ın ekranını tam ekran olarak gösterir.
 * BROADCAST_BEGIN → pencereyi aç
 * BROADCAST_FRAME → kareleri göster
 * BROADCAST_END   → pencereyi kapat
 */
class ScreenBroadcastReceiver : public QWidget {
    Q_OBJECT

public:
    static ScreenBroadcastReceiver& instance();

    void handleEnvelope(const okidia::Envelope& envelope);

    // Yayını zorla kapat (bağlantı koptuğunda çağrılır)
    void handleEnd();

    bool isBroadcasting() const { return m_isBroadcasting; }

signals:
    void broadcastStarted();
    void broadcastEnded();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event)  override;
    void closeEvent(QCloseEvent* event)  override;

private:
    explicit ScreenBroadcastReceiver(QWidget* parent = nullptr);

    ScreenBroadcastReceiver(const ScreenBroadcastReceiver&)            = delete;
    ScreenBroadcastReceiver& operator=(const ScreenBroadcastReceiver&) = delete;

    QPixmap    m_frame;
    QByteArray m_pendingJpegData;
    bool       m_renderPending = false;
    bool       m_isBroadcasting = false;

    void handleBegin();
    void handleFrame(const okidia::BroadcastFrame& msg);
};
