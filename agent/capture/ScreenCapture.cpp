// agent/capture/ScreenCapture.cpp
#include "ScreenCapture.h"
#include "../../common/utils/Logger.h"

#include <QScreen>
#include <QGuiApplication>
#include <QBuffer>
#include <QPixmap>
#include <QPainter>
#include <QCursor>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifdef MOUSE_EVENT
#    undef MOUSE_EVENT
#  endif
#  ifdef KEY_EVENT
#    undef KEY_EVENT
#  endif
#elif defined(HAVE_XTST)
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#endif

#if defined(Q_OS_WIN)
void ScreenCapture::releaseGdi() {
    if (m_hMemoryDC && m_hOldBitmap) {
        SelectObject(static_cast<HDC>(m_hMemoryDC), static_cast<HBITMAP>(m_hOldBitmap));
        m_hOldBitmap = nullptr;
    }
    if (m_hBitmap) {
        DeleteObject(static_cast<HBITMAP>(m_hBitmap));
        m_hBitmap = nullptr;
    }
    if (m_hMemoryDC) {
        DeleteDC(static_cast<HDC>(m_hMemoryDC));
        m_hMemoryDC = nullptr;
    }
    if (m_hScreenDC) {
        ReleaseDC(NULL, static_cast<HDC>(m_hScreenDC));
        m_hScreenDC = nullptr;
    }
    m_cachedGdiW = 0;
    m_cachedGdiH = 0;
}

QImage ScreenCapture::captureGdi(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return {};

    // Boyut değiştiyse eski bitmap'i serbest bırak
    if (m_hBitmap && (m_cachedGdiW != width || m_cachedGdiH != height)) {
        if (m_hMemoryDC && m_hOldBitmap) {
            SelectObject(static_cast<HDC>(m_hMemoryDC), static_cast<HBITMAP>(m_hOldBitmap));
            m_hOldBitmap = nullptr;
        }
        DeleteObject(static_cast<HBITMAP>(m_hBitmap));
        m_hBitmap = nullptr;
        m_cachedGdiW = 0;
        m_cachedGdiH = 0;
    }

    if (!m_hScreenDC) {
        m_hScreenDC = GetDC(NULL);
    }
    if (!m_hScreenDC) return {};

    if (!m_hMemoryDC) {
        m_hMemoryDC = CreateCompatibleDC(static_cast<HDC>(m_hScreenDC));
    }
    if (!m_hMemoryDC) return {};

    if (!m_hBitmap) {
        m_hBitmap = CreateCompatibleBitmap(static_cast<HDC>(m_hScreenDC), width, height);
        if (m_hBitmap) {
            m_hOldBitmap = SelectObject(static_cast<HDC>(m_hMemoryDC), static_cast<HBITMAP>(m_hBitmap));
            m_cachedGdiW = width;
            m_cachedGdiH = height;
        }
    }
    if (!m_hBitmap) return {};

    QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
    bool success = false;

    if (BitBlt(static_cast<HDC>(m_hMemoryDC), 0, 0, width, height, static_cast<HDC>(m_hScreenDC), x, y, SRCCOPY | CAPTUREBLT)) {
        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = width;
        bi.biHeight = -height;  // Top-down DIB
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        if (GetDIBits(static_cast<HDC>(m_hMemoryDC), static_cast<HBITMAP>(m_hBitmap), 0, height, img.bits(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS)) {
            success = true;
        }
    }

    if (success && !img.isNull()) {
        return img;
    }
    return {};
}
#endif

ScreenCapture::ScreenCapture(const CaptureSettings& settings)
    : m_settings(settings) {
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary) {
        m_nativeW = primary->geometry().width();
        m_nativeH = primary->geometry().height();
    }
}

int ScreenCapture::screenWidth() const {
    if (m_nativeW > 0) return m_nativeW;
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary && primary->geometry().width() > 0) return primary->geometry().width();
    return m_previousFrame.width() > 0 ? m_previousFrame.width() : 1920;
}

int ScreenCapture::screenHeight() const {
    if (m_nativeH > 0) return m_nativeH;
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary && primary->geometry().height() > 0) return primary->geometry().height();
    return m_previousFrame.height() > 0 ? m_previousFrame.height() : 1080;
}

int ScreenCapture::scaledWidth() const {
    if (m_previousFrame.width() > 0) return m_previousFrame.width();
    int w = screenWidth();
    if (m_settings.maxWidth > 0 && m_settings.maxWidth < w) {
        return m_settings.maxWidth;
    }
    if (m_settings.scalePercent >= 100 || m_settings.scalePercent <= 0) return w;
    return qMax(w * m_settings.scalePercent / 100, 320);
}

int ScreenCapture::scaledHeight() const {
    if (m_previousFrame.height() > 0) return m_previousFrame.height();
    int h = screenHeight();
    if (m_settings.maxWidth > 0 && screenWidth() > 0 && m_settings.maxWidth < screenWidth()) {
        return h * m_settings.maxWidth / screenWidth();
    }
    if (m_settings.scalePercent >= 100 || m_settings.scalePercent <= 0) return h;
    return qMax(h * m_settings.scalePercent / 100, 240);
}

ScreenCapture::~ScreenCapture() {
#if defined(Q_OS_WIN)
    releaseGdi();
#endif
}

void ScreenCapture::updateSettings(const CaptureSettings& settings) {
    m_settings      = settings;
    m_previousFrame = QImage();
}

void ScreenCapture::setSource(const CaptureSource& source) {
    m_source        = source;
    m_previousFrame = QImage();  // Kaynak değişince full frame gönder
    Logger::info(QObject::tr("ScreenCapture: Kaynak değişti → %1").arg(static_cast<int>(source.type)));
}

// ---------------------------------------------------------------------------
// Monitör listesi
// ---------------------------------------------------------------------------
QList<MonitorDesc> ScreenCapture::availableMonitors() {
    QList<MonitorDesc> list;
    const auto screens = QGuiApplication::screens();
    QScreen*   primary = QGuiApplication::primaryScreen();

    for (int i = 0; i < screens.size(); ++i) {
        QScreen* s = screens[i];
        MonitorDesc d;
        d.index     = i;
        d.name      = s->name();
        d.x         = s->geometry().x();
        d.y         = s->geometry().y();
        d.width     = s->geometry().width();
        d.height    = s->geometry().height();
        d.isPrimary = (s == primary);
        list << d;
    }
    return list;
}

// ---------------------------------------------------------------------------
// Tam frame (ilk bağlantı)
// ---------------------------------------------------------------------------
QByteArray ScreenCapture::captureFullFrame() {
    QImage frame = captureScreen();
    if (frame.isNull()) {
        Logger::error(QObject::tr("ScreenCapture: Ekran yakalanamadı"));
        return {};
    }
    // Native boyutu kaydet (scale öncesi)
    m_nativeW = frame.width(); 
    m_nativeH = frame.height();

    frame           = scaleFrame(frame);
    m_previousFrame = frame;
    return encodeJpeg(frame);
}

QByteArray ScreenCapture::captureFullFrameHQ() {
    // Yüksek kalite uzak kontrol için optimize edilmiş yakalama:
    QImage frame = captureScreen(); // Çoklu ekran desteği (m_source)
    if (frame.isNull()) return {};

    m_nativeW = frame.width();
    m_nativeH = frame.height();

    int maxW = m_settings.maxWidth > 0 ? m_settings.maxWidth : 1920;

    // Panoramik ekranlarda (Tüm Ekranlar) Master'ın gönderdiği hedef genişliği
    // sadece ana ekranın hedef genişliği olarak kabul et ve orantılı olarak büyüt.
    if (m_source.type == CaptureSource::AllMonitors) {
        QScreen* primary = QGuiApplication::primaryScreen();
        if (primary && primary->geometry().width() > 0) {
            double ratio = static_cast<double>(frame.width()) / primary->geometry().width();
            maxW = static_cast<int>(maxW * ratio);
        } else {
            // Fallback
            maxW = maxW * QGuiApplication::screens().size();
        }
    }

    if (frame.width() > maxW) {
        int targetH = frame.height() * maxW / frame.width();
        // FastTransformation (nearest-neighbor): Uzak kontrol akışında hız, kaliteden önceliklidir.
        // SmoothTransformation bilinear ölçekleme 1080p'de ~15-25ms sürer ve 20 FPS hedefinde
        // ciddi darboğaz oluşturur. FastTransformation ~1-2ms sürer.
        frame = frame.scaled(maxW, targetH, Qt::KeepAspectRatio, Qt::FastTransformation);
    }

    QByteArray buf;
    QBuffer buffer(&buf);
    buffer.open(QIODevice::WriteOnly);
    int quality = m_settings.jpegQuality > 0 ? m_settings.jpegQuality : 80;
    if (!frame.save(&buffer, "JPEG", quality)) {
        Logger::error(QObject::tr("ScreenCapture: HQ JPEG encode başarısız"));
        return {};
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Dirty tile diff
// ---------------------------------------------------------------------------
std::vector<TileUpdate> ScreenCapture::captureDirtyTiles() {
    QImage current = captureScreen();
    if (current.isNull()) return {};

    m_nativeW = current.width();
    m_nativeH = current.height();

    current = scaleFrame(current);
    std::vector<TileUpdate> dirty;

    if (m_previousFrame.isNull() ||
        m_previousFrame.size() != current.size()) {
        m_previousFrame = current;
        TileUpdate tile;
        tile.x = 0; tile.y = 0;
        tile.width  = current.width();
        tile.height = current.height();
        tile.jpegData = encodeJpeg(current);
        dirty.push_back(std::move(tile));
        return dirty;
    }

    int tileSize = m_settings.tileSize;
    int cols = (current.width()  + tileSize - 1) / tileSize;
    int rows = (current.height() + tileSize - 1) / tileSize;

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            int x = col * tileSize;
            int y = row * tileSize;
            int w = qMin(tileSize, current.width()  - x);
            int h = qMin(tileSize, current.height() - y);

            if (hashTile(current, x, y, w, h) !=
                hashTile(m_previousFrame, x, y, w, h)) {
                TileUpdate tile;
                tile.x = x; tile.y = y;
                tile.width = w; tile.height = h;
                tile.jpegData = encodeJpeg(current.copy(x, y, w, h));
                dirty.push_back(std::move(tile));
            }
        }
    }

    m_previousFrame = current;
    return dirty;
}

// ---------------------------------------------------------------------------
// Kaynak dispatcher
// ---------------------------------------------------------------------------
QImage ScreenCapture::captureScreen() {
    QImage img;
    switch (m_source.type) {
    case CaptureSource::Monitor:
        img = captureMonitor(m_source.monitorIndex);
        break;
    case CaptureSource::Window:
        img = captureWindow(m_source.windowId);
        break;
    case CaptureSource::AllMonitors:
        img = captureAllMonitors();
        break;
    case CaptureSource::Primary:
    default:
        img = captureMonitor(0);  // 0 = birincil ekran
        break;
    }

    if (!img.isNull()) {
        if (img.format() != QImage::Format_RGB32 &&
            img.format() != QImage::Format_ARGB32_Premultiplied &&
            img.format() != QImage::Format_ARGB32) {
            img = img.convertToFormat(QImage::Format_RGB32);
        }
        if (m_drawCursor) {
            drawCursor(img);
        }
    }
    return img;
}

void ScreenCapture::drawCursor(QImage& img) {
    QPoint globalPos = QCursor::pos();
    QPoint localPos;

    if (m_source.type == CaptureSource::AllMonitors) {
        QRect total;
        const auto screens = QGuiApplication::screens();
        for (QScreen* s : screens)
            total = total.united(s->geometry());
        localPos = globalPos - total.topLeft();
    } else if (m_source.type == CaptureSource::Monitor) {
        const auto screens = QGuiApplication::screens();
        int idx = m_source.monitorIndex;
        QScreen* screen = nullptr;
        if (idx == 0) {
            screen = QGuiApplication::primaryScreen();
        } else if (idx < screens.size()) {
            screen = screens[idx];
        } else {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen) {
            localPos = globalPos - screen->geometry().topLeft();
        } else {
            localPos = globalPos;
        }
    } else if (m_source.type == CaptureSource::Window) {
        return;
    } else {
        // Primary
        QScreen* screen = QGuiApplication::primaryScreen();
        if (screen) {
            localPos = globalPos - screen->geometry().topLeft();
        } else {
            localPos = globalPos;
        }
    }

    if (img.rect().contains(localPos)) {
        QPainter painter(&img);
        painter.setRenderHint(QPainter::Antialiasing);

        // Draw standard cursor: White fill, black border
        QPolygon cursorPoly;
        cursorPoly << QPoint(0, 0)
                   << QPoint(0, 17)
                   << QPoint(4, 13)
                   << QPoint(8, 21)
                   << QPoint(10, 20)
                   << QPoint(6, 12)
                   << QPoint(11, 12);

        painter.translate(localPos);
        painter.setBrush(Qt::white);
        painter.setPen(QPen(Qt::black, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolygon(cursorPoly);
    }
}

// ---------------------------------------------------------------------------
// Belirli monitör
// ---------------------------------------------------------------------------
QImage ScreenCapture::captureMonitor(int idx) {
    const auto screens = QGuiApplication::screens();

    QScreen* screen = nullptr;
    if (idx == 0) {
        screen = QGuiApplication::primaryScreen();
    } else if (idx < screens.size()) {
        screen = screens[idx];
    } else {
        screen = QGuiApplication::primaryScreen();
        Logger::warning(QObject::tr("ScreenCapture: Monitör %1 bulunamadı, birincil ekran kullanılıyor").arg(idx));
    }

    if (!screen) return {};

#if defined(Q_OS_WIN)
    QRect geo = screen->geometry();
    QImage gdiImg = captureGdi(geo.x(), geo.y(), geo.width(), geo.height());
    if (!gdiImg.isNull()) return gdiImg;
#endif

    QPixmap px = screen->grabWindow(0);
    if (px.isNull() || px.width() <= 0 || px.height() <= 0) return {};
    return px.toImage();
}

// ---------------------------------------------------------------------------
// Tüm monitörler birleşik (panoramik)
// ---------------------------------------------------------------------------
QImage ScreenCapture::captureAllMonitors() {
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) return {};

    // Tek monitör varsa panoramik birleştirme gereksiz — doğrudan yakala
    if (screens.size() == 1) return captureMonitor(0);

    // Tüm ekranların birleşik bounding rect'ini hesapla
    QRect total;
    for (QScreen* s : screens)
        total = total.united(s->geometry());

    if (total.isEmpty()) return {};

#if defined(Q_OS_WIN)
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw > 0 && vh > 0) {
        QImage gdiImg = captureGdi(vx, vy, vw, vh);
        if (!gdiImg.isNull()) return gdiImg;
    }
#endif

    QImage canvas(total.size(), QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);

    for (QScreen* s : screens) {
        QRect geo = s->geometry();
        QPixmap px = s->grabWindow(0, geo.x(), geo.y(), geo.width(), geo.height());
        if (px.isNull() || px.width() <= 0 || px.height() <= 0) continue;
        QRect rel = geo.translated(-total.topLeft());
        painter.drawPixmap(rel, px);
    }
    painter.end();
    return canvas;
}

// ---------------------------------------------------------------------------
// Belirli pencere
// ---------------------------------------------------------------------------
QImage ScreenCapture::captureWindow(quint64 wid) {
#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(wid);
    if (!hwnd || !IsWindow(hwnd)) {
        Logger::warning(QObject::tr("ScreenCapture: Pencere HWND bulunamadı → %1").arg(wid));
        return captureMonitor(0);
    }

    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return captureMonitor(0);

    HDC hdcWindow = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWindow, w, h);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

    // PrintWindow ile donanım hızlandırmalı pencereleri de yakala
    if (!PrintWindow(hwnd, hdcMem, 2 /*PW_RENDERFULLCONTENT*/)) {
        BitBlt(hdcMem, 0, 0, w, h, hdcWindow, 0, 0, SRCCOPY);
    }

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    GetDIBits(hdcMem, hbm, 0, h, img.bits(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(hdcMem, hOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);

    if (!img.isNull()) return img;
    return captureMonitor(0);
#elif defined(HAVE_XTST)
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return captureMonitor(0);

    Window win = static_cast<Window>(wid);

    // Pencere boyutunu al
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, win, &attrs)) {
        XCloseDisplay(dpy);
        Logger::warning(QObject::tr("ScreenCapture: Pencere bulunamadı → %1").arg(wid));
        return captureMonitor(0);
    }

    // Qt ile pencere yakala — WId üzerinden
    QPixmap px = QGuiApplication::primaryScreen()->grabWindow(static_cast<WId>(wid));
    XCloseDisplay(dpy);

    if (px.isNull()) return captureMonitor(0);
    return px.toImage();
#else
    Q_UNUSED(wid)
    Logger::warning(QObject::tr("ScreenCapture: Pencere yakalama desteklenmiyor"));
    return captureMonitor(0);
#endif
}

// ---------------------------------------------------------------------------
QImage ScreenCapture::scaleFrame(const QImage& img) {
    if (m_settings.scalePercent >= 100) {
        return img; // %100 ölçekte hiçbir islem yapma, direkt döndür.
    }

    int w = qMax(img.width()  * m_settings.scalePercent / 100, 320);
    int h = qMax(img.height() * m_settings.scalePercent / 100, 240);

    // Remote Control gibi daha yüksek kaliteli yayınlar için SmoothTransformation kullan
    // Thumbnail için (%25) FastTransformation kullan
    Qt::TransformationMode mode = (m_settings.scalePercent > 40) ? Qt::SmoothTransformation : Qt::FastTransformation;

    return img.scaled(w, h, Qt::KeepAspectRatio, mode);
}

QByteArray ScreenCapture::encodeJpeg(const QImage& img) {
    if (img.isNull()) return {};
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "JPEG", m_settings.jpegQuality))
        Logger::error(QObject::tr("ScreenCapture: JPEG encode başarısız"));
    return bytes;
}

uint64_t ScreenCapture::hashTile(const QImage& img,
                                 int x, int y, int w, int h) {
    if (img.isNull()) return 0;
    uint64_t hash  = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    for (int row = y; row < y + h && row < img.height(); ++row) {
        const uchar* line = img.constScanLine(row);
        for (int b = x * 4; b < (x + w) * 4; ++b) {
            hash ^= static_cast<uint64_t>(line[b] & 0xFC);
            hash *= prime;
        }
    }
    return hash;
}
