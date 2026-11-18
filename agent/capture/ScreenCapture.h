// agent/capture/ScreenCapture.h
#pragma once

#include <QImage>
#include <QByteArray>
#include <QList>
#include <vector>
#include <cstdint>

struct TileUpdate {
    int x, y, width, height;
    QByteArray jpegData;
};

struct CaptureSettings {
    int scalePercent  = 25;
    int jpegQuality   = 60;   // 50→60: metin okunabilirliğini iyileştirir (~%30 bant artışı)
    int tileSize      = 64;   // 128→64: daha hassas dirty detection, daha az gereksiz veri
    int fps           = 5;
    int fullscreenFps = 15;
    int maxWidth      = 0;
};

struct CaptureSource {
    enum Type { Primary, Monitor, Window, AllMonitors };
    Type    type         = AllMonitors;
    int     monitorIndex = 0;
    quint64 windowId     = 0;
};

struct MonitorDesc {
    int     index;
    QString name;
    int     x, y, width, height;
    bool    isPrimary;
};

class ScreenCapture {
public:
    explicit ScreenCapture(const CaptureSettings& settings = CaptureSettings());
    ~ScreenCapture();

    void updateSettings(const CaptureSettings& settings);
    CaptureSettings settings() const { return m_settings; }

    void setSource(const CaptureSource& source);
    CaptureSource source() const { return m_source; }

    static QList<MonitorDesc> availableMonitors();

    QByteArray captureFullFrame();          // Thumbnail kalitesi (%25 scale)
    QByteArray captureFullFrameHQ();        // Yüksek kalite (%100 scale, %90 JPEG)
    std::vector<TileUpdate> captureDirtyTiles();

    int screenWidth() const;
    int screenHeight() const;
    int scaledWidth()  const;
    int scaledHeight() const;

    void setDrawCursor(bool draw) { m_drawCursor = draw; }
    bool isDrawCursorEnabled() const { return m_drawCursor; }

    void resetPreviousFrame() { m_previousFrame = QImage(); }

private:
    CaptureSettings m_settings;
    int             m_nativeW = 0;   // Gerçek ekran genişliği
    int             m_nativeH = 0;   // Gerçek ekran yüksekliği
    CaptureSource   m_source;
    QImage          m_previousFrame;
    bool            m_drawCursor = true;

#if defined(Q_OS_WIN)
    void* m_hScreenDC  = nullptr;
    void* m_hMemoryDC  = nullptr;
    void* m_hBitmap    = nullptr;
    void* m_hOldBitmap = nullptr;
    int   m_cachedGdiW = 0;
    int   m_cachedGdiH = 0;
    QImage captureGdi(int x, int y, int width, int height);
    void   releaseGdi();
#endif

    QImage     captureScreen();
    void       drawCursor(QImage& img);
    QImage     captureMonitor(int idx);
    QImage     captureWindow(quint64 wid);
    QImage     captureAllMonitors();
    QImage     scaleFrame(const QImage& img);
    QByteArray encodeJpeg(const QImage& img);
    uint64_t   hashTile(const QImage& img, int x, int y, int w, int h);
};
