// master/broadcast/ScreenBroadcaster.h
#pragma once

#include <QObject>
#include <QTimer>
#include <QPixmap>
#include <QScreen>
#include <QStringList>
#include "../../common/network/Connection.h"
#include "../ui/BroadcastSourceDialog/BroadcastSourceDialog.h"

/**
 * Master ekranını JPEG olarak yakalar ve seçilen veya tüm agent'lara
 * BROADCAST_BEGIN / BROADCAST_FRAME / BROADCAST_END ile gönderir.
 *
 * Buffer-bloat önleme (backpressure): Eğer soket tamponunda henüz iletilmemiş
 * veri (bytesToWrite) 150 KB'tan fazlaysa o anki kareyi atlar (drop frame).
 */
class ScreenBroadcaster : public QObject {
    Q_OBJECT

public:
    explicit ScreenBroadcaster(Connection* conn,
                                QObject* parent = nullptr);

    void setConnection(Connection* conn) { m_conn = conn; }

    void start(const BroadcastSource& source, const QStringList& targetClientIds = {});
    void stop();

    bool isRunning() const { return m_timer && m_timer->isActive(); }
    BroadcastSource currentSource() const { return m_source; }
    QStringList targetClientIds() const { return m_targetClientIds; }
    int quality() const { return m_jpegQuality; }

public slots:
    void setQuality(int quality);

signals:
    void started();
    void stopped();

private slots:
    void captureAndSend();

private:
    Connection*     m_conn   = nullptr;
    QTimer*         m_timer  = nullptr;
    int             m_jpegQuality = 65;
    BroadcastSource m_source;
    QStringList     m_targetClientIds;
    QByteArray      m_jpegBuffer;

    QPixmap grabSource() const;
    void drawCursor(QPixmap& px, int origW, int origH) const;
    void sendBegin();
    void sendEnd();
};
