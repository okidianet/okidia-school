// agent/capture/WaylandPortalCapture.h
#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QVariantMap>

#if defined(HAVE_PIPEWIRE)
#include <QDBusUnixFileDescriptor>
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
#endif

/**
 * @brief Wayland oturumlarında GNOME Mutter.ScreenCast (sıfır onay) veya
 *        xdg-desktop-portal (ScreenCast) + PipeWire üzerinden ekran görüntüsü
 *        yakalayan tekil (singleton) yardımcı sınıf.
 */
class WaylandPortalCapture : public QObject {
    Q_OBJECT
public:
    static WaylandPortalCapture& instance();
    static bool isWaylandSession();
    static bool isGnomeWaylandSession();

    /**
     * @brief GNOME Kiosk eklentisi üzerinden masaüstü kilit modunu (Overview/Alt+Tab engeli) açar/kapatır.
     */
    static void setGnomeLockdown(bool locked);

    /**
     * @brief GNOME Kiosk eklentisi üzerinden doğrudan compositor seviyesi tam ekran kilit perdesini
     *        (GDM giriş ekranı, kilit ekranı ve masaüstü dahil tüm katmanların üstünde) açar/kapatır.
     */
    static void setGnomeLockOverlay(bool locked, const QString& title = QString(), const QString& message = QString());

    /**
     * @brief PipeWire akışını başlatır (zaten aktifse veya başlatılıyorsa tekrar etmez).
     */
    void ensureStarted();

    /**
     * @brief Akışı durdurur ve PipeWire kaynaklarını serbest bırakır.
     */
    void stop();

    /**
     * @brief PipeWire akışından alınan en son ekran karesini döndürür.
     */
    QImage latestFrame() const;

    bool isRunning() const { return m_running; }

    /**
     * @brief Akış kesildiğinde bekleme süresini sıfırlayarak anında yeniden başlatır.
     */
    void forceImmediateRestart();

private slots:
    void onMutterPipeWireStreamAdded(uint nodeId);
    void onMutterSessionClosed();
    void onCreateSessionResponse(uint response, const QVariantMap& results);
    void onSelectSourcesResponse(uint response, const QVariantMap& results);
    void onStartResponse(uint response, const QVariantMap& results);

private:
    explicit WaylandPortalCapture(QObject* parent = nullptr);
    ~WaylandPortalCapture() override;

    WaylandPortalCapture(const WaylandPortalCapture&) = delete;
    WaylandPortalCapture& operator=(const WaylandPortalCapture&) = delete;

    void ensureKioskExtensionActive();
    bool tryStartMutterScreenCast();
    void startPortalFlow();
    void startPipeWireStream(int fd, uint32_t nodeId);
    void cleanupPipeWire();
    QImage captureGnomeScreenshotFallback() const;

#if defined(HAVE_PIPEWIRE)
    static void onStreamParamChanged(void* userdata, uint32_t id, const struct spa_pod* param);
    static void onStreamProcess(void* userdata);

    pw_thread_loop* m_pwLoop   = nullptr;
    pw_context*     m_pwCtx    = nullptr;
    pw_core*        m_pwCore   = nullptr;
    pw_stream*      m_pwStream = nullptr;
    spa_hook*       m_streamListener = nullptr;
#endif

    mutable QMutex m_frameMutex;
    mutable QImage m_latestFrame;
    mutable bool   m_hasPipeWireFrame = false;
    mutable qint64 m_lastFallbackCaptureMs = 0;
    int            m_videoW   = 0;
    int            m_videoH   = 0;
    uint32_t       m_videoFmt = 0;

    QString m_sessionHandle;
    QString m_mutterSessionPath;
    bool    m_starting = false;
    bool    m_running  = false;
    qint64  m_lastRetryMs = 0;
};
