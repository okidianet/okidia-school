// agent/capture/WaylandPortalCapture.cpp
#include "WaylandPortalCapture.h"
#include "../../common/utils/Logger.h"

#include <QGuiApplication>
#include <QScreen>
#include <QMutexLocker>
#include <QDateTime>
#include <QSettings>
#include <QUuid>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QThread>
#include <QTimer>

#if defined(HAVE_PIPEWIRE)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusArgument>
#include <QDBusReply>

#include <unistd.h>
#include <cstring>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/debug/types.h>
#endif

namespace {
constexpr const char* PORTAL_SERVICE   = "org.freedesktop.portal.Desktop";
constexpr const char* PORTAL_PATH      = "/org/freedesktop/portal/desktop";
constexpr const char* SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
constexpr const char* REQUEST_IFACE    = "org.freedesktop.portal.Request";

QString makeToken(const QString& prefix) {
    QString raw = QUuid::createUuid().toString(QUuid::WithoutBraces);
    raw.remove('-');
    return prefix + "_" + raw.left(12);
}
} // namespace

WaylandPortalCapture& WaylandPortalCapture::instance() {
    static WaylandPortalCapture s_inst;
    return s_inst;
}

bool WaylandPortalCapture::isWaylandSession() {
    if (QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
        return true;
    }
    if (qEnvironmentVariable("XDG_SESSION_TYPE").compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        return true;
    }
    return false;
}

bool WaylandPortalCapture::isGnomeWaylandSession() {
    if (!isWaylandSession()) return false;

    const QString xdgDesktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    const QString desktopSess = qEnvironmentVariable("DESKTOP_SESSION");
    const QString gdmsess = qEnvironmentVariable("GDMSESSION");
    const QString user = qEnvironmentVariable("USER");

    if (xdgDesktop.contains(QStringLiteral("GNOME"), Qt::CaseInsensitive) ||
        xdgDesktop.contains(QStringLiteral("ubuntu"), Qt::CaseInsensitive) ||
        desktopSess.contains(QStringLiteral("gnome"), Qt::CaseInsensitive) ||
        desktopSess.contains(QStringLiteral("ubuntu"), Qt::CaseInsensitive) ||
        gdmsess.contains(QStringLiteral("gnome"), Qt::CaseInsensitive) ||
        gdmsess.contains(QStringLiteral("ubuntu"), Qt::CaseInsensitive) ||
        user.startsWith(QStringLiteral("gdm"), Qt::CaseInsensitive) ||
        user.contains(QStringLiteral("greeter"), Qt::CaseInsensitive) ||
        user == QStringLiteral("Debian-gdm")) {
        return true;
    }

#if defined(HAVE_PIPEWIRE)
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface()) {
        if (bus.interface()->isServiceRegistered(QStringLiteral("org.gnome.Mutter.ScreenCast")) ||
            bus.interface()->isServiceRegistered(QStringLiteral("org.gnome.Shell"))) {
            return true;
        }
    }
#endif
    return false;
}

void WaylandPortalCapture::setGnomeLockdown(bool locked) {
#if defined(HAVE_PIPEWIRE)
    if (!isWaylandSession()) return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    if (locked) {
        instance().ensureKioskExtensionActive();
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("net.okidia.Kiosk"),
        QStringLiteral("/net/okidia/Kiosk"),
        QStringLiteral("net.okidia.Kiosk"),
        QStringLiteral("SetLockdown"));
    msg << locked;
    bus.asyncCall(msg);
#else
    Q_UNUSED(locked);
#endif
}

void WaylandPortalCapture::setGnomeLockOverlay(bool locked, const QString& title, const QString& message) {
#if defined(HAVE_PIPEWIRE)
    if (!isWaylandSession()) return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    if (locked) {
        instance().ensureKioskExtensionActive();
    }

    QDBusMessage overlayMsg = QDBusMessage::createMethodCall(
        QStringLiteral("net.okidia.Kiosk"),
        QStringLiteral("/net/okidia/Kiosk"),
        QStringLiteral("net.okidia.Kiosk"),
        QStringLiteral("SetLockOverlay"));
    overlayMsg << locked << title << message;
    bus.asyncCall(overlayMsg);

    QDBusMessage lockMsg = QDBusMessage::createMethodCall(
        QStringLiteral("net.okidia.Kiosk"),
        QStringLiteral("/net/okidia/Kiosk"),
        QStringLiteral("net.okidia.Kiosk"),
        QStringLiteral("SetLockdown"));
    lockMsg << locked;
    bus.asyncCall(lockMsg);
#else
    Q_UNUSED(locked);
    Q_UNUSED(title);
    Q_UNUSED(message);
#endif
}

WaylandPortalCapture::WaylandPortalCapture(QObject* parent)
    : QObject(parent) {
}

WaylandPortalCapture::~WaylandPortalCapture() {
    stop();
}

QImage WaylandPortalCapture::latestFrame() const {
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_hasPipeWireFrame && !m_latestFrame.isNull()) {
            return m_latestFrame;
        }
    }

    // PipeWire henüz ilk kareyi üretmediyse veya gdm-greeter oturumunda PipeWire
    // servisi çalışmıyorsa, GNOME Shell compositor D-Bus (org.gnome.Shell.Screenshot)
    // üzerinden sıfır flaşlı doğrudan sahne yakalama yedeğini çalıştır.
    // NOT: GNOME Shell compositor'de ve ses akışında (PipeWire) takılma/kesiklik (stutter)
    // yaratmaması için bu senkron D-Bus PNG fallback'i en fazla 2.5 saniyede bir çağrılır!
    if (isGnomeWaylandSession()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        bool needCapture = false;
        {
            QMutexLocker lock(&m_frameMutex);
            needCapture = m_latestFrame.isNull() || (now - m_lastFallbackCaptureMs >= 2500);
        }
        if (needCapture) {
            m_lastFallbackCaptureMs = now;
            QImage fb = captureGnomeScreenshotFallback();
            if (!fb.isNull()) {
                QMutexLocker lock(&m_frameMutex);
                if (!m_hasPipeWireFrame) {
                    m_latestFrame = fb;
                }
                return m_latestFrame;
            }
        }
    }

    QMutexLocker lock(&m_frameMutex);
    return m_latestFrame;
}

QImage WaylandPortalCapture::captureGnomeScreenshotFallback() const {
#if !defined(HAVE_PIPEWIRE)
    return {};
#else
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return {};

    QDBusInterface kiosk(QStringLiteral("net.okidia.Kiosk"),
                         QStringLiteral("/net/okidia/Kiosk"),
                         QStringLiteral("net.okidia.Kiosk"), bus);
    bool usedUnsafe = false;
    if (kiosk.isValid()) {
        kiosk.call(QStringLiteral("SetScreenCastUnsafe"), true);
        usedUnsafe = true;
    }

    QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtimeDir.isEmpty() || !QDir(runtimeDir).exists()) {
        runtimeDir = QDir::tempPath();
    }
    const QString tmpPath = runtimeDir + QStringLiteral("/okidia_cap_%1.png").arg(::getuid());

    QDBusInterface shotIface(QStringLiteral("org.gnome.Shell.Screenshot"),
                             QStringLiteral("/org/gnome/Shell/Screenshot"),
                             QStringLiteral("org.gnome.Shell.Screenshot"), bus);
    QImage img;
    if (shotIface.isValid()) {
        QDBusMessage reply = shotIface.call(QStringLiteral("Screenshot"), true, false, tmpPath);
        if (reply.type() == QDBusMessage::ReplyMessage && QFile::exists(tmpPath)) {
            img.load(tmpPath);
            QFile::remove(tmpPath);
        }
    }

    if (usedUnsafe && kiosk.isValid()) {
        kiosk.call(QStringLiteral("SetScreenCastUnsafe"), false);
    }
    return img;
#endif
}

void WaylandPortalCapture::forceImmediateRestart() {
    m_lastRetryMs = 0;
    stop();
    ensureStarted();
}

void WaylandPortalCapture::ensureStarted() {
#if !defined(HAVE_PIPEWIRE)
    return;
#else
    if (m_running || m_starting) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 cooldownMs = isGnomeWaylandSession() ? 6000 : 10000;
    if (m_lastRetryMs > 0 && (now - m_lastRetryMs < cooldownMs)) {
        return;
    }
    m_lastRetryMs = now;
    m_starting = true;

    if (tryStartMutterScreenCast()) {
        return;
    }

    // KESİN KURAL: GNOME / Mutter Wayland oturumlarında kullanıcıya ekran paylaşımı onay penceresi
    // çıkaran xdg-desktop-portal (startPortalFlow) kesinlikle ÇAĞRILMAZ!
    if (isGnomeWaylandSession()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: GNOME oturumunda Mutter.ScreenCast henüz hazır değil, onay penceresini engellemek için Portal atlandı. 2 sn sonra yeniden denenecek."));
        m_starting = false;
        return;
    }

    startPortalFlow();
#endif
}

void WaylandPortalCapture::stop() {
#if defined(HAVE_PIPEWIRE)
    cleanupPipeWire();
    if (!m_mutterSessionPath.isEmpty()) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.gnome.Mutter.ScreenCast"), m_mutterSessionPath,
            QStringLiteral("org.gnome.Mutter.ScreenCast.Session"), QStringLiteral("Stop"));
        QDBusConnection::sessionBus().asyncCall(msg);
        m_mutterSessionPath.clear();
    }
    if (!m_sessionHandle.isEmpty()) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            PORTAL_SERVICE, m_sessionHandle,
            QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(msg);
        m_sessionHandle.clear();
    }
#endif
    m_starting = false;
    m_running = false;
}

void WaylandPortalCapture::ensureKioskExtensionActive() {
#if defined(HAVE_PIPEWIRE)
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    if (bus.interface() && bus.interface()->isServiceRegistered(QStringLiteral("net.okidia.Kiosk"))) {
        return;
    }

    // Sistem dizininde (/usr/share/gnome-shell/extensions/okidia-kiosk@okidia.net) eklenti kuruluysa,
    // kullanıcının ev dizinindeki (~/.local/share/...) kopyayı kaldır ki GNOME Shell eklentiyi PER_USER
    // yerine SYSTEM eklentisi olarak sınıflandırsın (gdm ve unlock-dialog modları için zorunludur).
    const QString sysExtDir = QStringLiteral("/usr/share/gnome-shell/extensions/okidia-kiosk@okidia.net");
    const QString homeDir = QDir::homePath();
    if (!homeDir.isEmpty() && QFile::exists(sysExtDir + QStringLiteral("/extension.js"))) {
        const QString userExtDir = homeDir + QStringLiteral("/.local/share/gnome-shell/extensions/okidia-kiosk@okidia.net");
        if (QDir(userExtDir).exists()) {
            QDir(userExtDir).removeRecursively();
        }
    }

    // D-Bus üzerinden GNOME Shell.Extensions servisine asenkron etkinleştirme komutu gönder
    // (GUI thread'i QThread::msleep ile bloklama!)
    QDBusMessage enableMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.gnome.Shell.Extensions"),
        QStringLiteral("/org/gnome/Shell/Extensions"),
        QStringLiteral("org.gnome.Shell.Extensions"),
        QStringLiteral("EnableExtension"));
    enableMsg << QStringLiteral("okidia-kiosk@okidia.net");
    bus.asyncCall(enableMsg);
#endif
}

bool WaylandPortalCapture::tryStartMutterScreenCast() {
#if !defined(HAVE_PIPEWIRE)
    return false;
#else
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;

    // 1. Üst çubuk turuncu paylaşım göstergesini gizleyen Kiosk eklentisinin aktif olduğundan emin ol
    ensureKioskExtensionActive();

    QDBusInterface kiosk(QStringLiteral("net.okidia.Kiosk"), QStringLiteral("/net/okidia/Kiosk"),
                         QStringLiteral("net.okidia.Kiosk"), bus);

    QDBusInterface mutter(QStringLiteral("org.gnome.Mutter.ScreenCast"),
                          QStringLiteral("/org/gnome/Mutter/ScreenCast"),
                          QStringLiteral("org.gnome.Mutter.ScreenCast"), bus);
    if (!mutter.isValid()) {
        return false;
    }

    // Önce unsafe_mode açmadan doğrudan CreateSession dene; yalnızca Mutter erişimi reddederse
    // Kiosk eklentisinin sessiz (bildirimsiz) SetScreenCastUnsafe ve ForceUninhibit köprüsünü kullan.
    bool usedUnsafe = false;
    QDBusReply<QDBusObjectPath> sessReply = mutter.call(QStringLiteral("CreateSession"), QVariantMap());
    if (!sessReply.isValid() && kiosk.isValid()) {
        kiosk.call(QStringLiteral("SetScreenCastUnsafe"), true);
        usedUnsafe = true;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (sessReply.error().message().contains(QStringLiteral("inhibited"), Qt::CaseInsensitive)) {
                kiosk.call(QStringLiteral("ForceUninhibit"));
            }
            sessReply = mutter.call(QStringLiteral("CreateSession"), QVariantMap());
            if (sessReply.isValid() || !sessReply.error().message().contains(QStringLiteral("inhibited"), Qt::CaseInsensitive)) {
                break;
            }
        }
    }
    if (!sessReply.isValid()) {
        if (usedUnsafe && kiosk.isValid()) kiosk.call(QStringLiteral("SetScreenCastUnsafe"), false);
        Logger::warning(QObject::tr("WaylandPortalCapture: Mutter.ScreenCast CreateSession hatası: ") + sessReply.error().message());
        return false;
    }

    m_mutterSessionPath = sessReply.value().path();
    QDBusInterface sessIface(QStringLiteral("org.gnome.Mutter.ScreenCast"), m_mutterSessionPath,
                             QStringLiteral("org.gnome.Mutter.ScreenCast.Session"), bus);

    bus.connect(QStringLiteral("org.gnome.Mutter.ScreenCast"), m_mutterSessionPath,
                QStringLiteral("org.gnome.Mutter.ScreenCast.Session"), QStringLiteral("Closed"),
                this, SLOT(onMutterSessionClosed()));

    QVariantMap recProps;
    recProps.insert(QStringLiteral("cursor-mode"), static_cast<uint>(1)); // 1 = Embedded

    // Farklı Mutter sürümleri için konnektör adı adaylarını sırayla dene:
    // 1) Birincil ekran konnektör adı (örn. "eDP-1", "Virtual-1", "HDMI-1")
    // 2) Boş string ""
    // 3) "Default"
    QStringList connectorCandidates;
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
        const QString pName = primary->name().trimmed();
        if (!pName.isEmpty()) {
            connectorCandidates << pName;
        }
    }
    connectorCandidates << QString() << QStringLiteral("Default");

    QString streamPath;
    QString lastErr;
    for (const QString& connName : connectorCandidates) {
        QDBusReply<QDBusObjectPath> streamReply = sessIface.call(QStringLiteral("RecordMonitor"), connName, recProps);
        if (streamReply.isValid() && !streamReply.value().path().isEmpty()) {
            streamPath = streamReply.value().path();
            break;
        }
        lastErr = streamReply.error().message();
    }

    if (streamPath.isEmpty() && !usedUnsafe && kiosk.isValid()) {
        kiosk.call(QStringLiteral("SetScreenCastUnsafe"), true);
        usedUnsafe = true;
        for (const QString& connName : connectorCandidates) {
            QDBusReply<QDBusObjectPath> streamReply = sessIface.call(QStringLiteral("RecordMonitor"), connName, recProps);
            if (streamReply.isValid() && !streamReply.value().path().isEmpty()) {
                streamPath = streamReply.value().path();
                break;
            }
            lastErr = streamReply.error().message();
        }
    }

    if (streamPath.isEmpty()) {
        if (usedUnsafe && kiosk.isValid()) kiosk.call(QStringLiteral("SetScreenCastUnsafe"), false);
        Logger::warning(QObject::tr("WaylandPortalCapture: Mutter.ScreenCast RecordMonitor hatası: ") + lastErr);
        sessIface.call(QStringLiteral("Stop"));
        m_mutterSessionPath.clear();
        return false;
    }

    bus.connect(QStringLiteral("org.gnome.Mutter.ScreenCast"), streamPath,
                QStringLiteral("org.gnome.Mutter.ScreenCast.Stream"), QStringLiteral("PipeWireStreamAdded"),
                this, SLOT(onMutterPipeWireStreamAdded(uint)));

    QDBusReply<void> startReply = sessIface.call(QStringLiteral("Start"));
    if (usedUnsafe && kiosk.isValid()) {
        kiosk.call(QStringLiteral("SetScreenCastUnsafe"), false);
    }

    if (!startReply.isValid()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: Mutter.ScreenCast Start hatası: ") + startReply.error().message());
        m_mutterSessionPath.clear();
        return false;
    }

    Logger::info(QObject::tr("WaylandPortalCapture: GNOME Mutter.ScreenCast (sıfır onay) başlatıldı."));
    QTimer::singleShot(3000, this, [this]() {
        if (m_starting && !m_running) {
            m_starting = false;
        }
    });
    return true;
#endif
}

void WaylandPortalCapture::onMutterPipeWireStreamAdded(uint nodeId) {
#if defined(HAVE_PIPEWIRE)
    if (nodeId == 0) return;
    Logger::info(QObject::tr("WaylandPortalCapture: Mutter PipeWire akışı hazır (node_id=%1).").arg(nodeId));
    startPipeWireStream(-1, nodeId);
#else
    Q_UNUSED(nodeId);
#endif
}

void WaylandPortalCapture::onMutterSessionClosed() {
#if defined(HAVE_PIPEWIRE)
    Logger::warning(QObject::tr("WaylandPortalCapture: Mutter.ScreenCast oturumu dışarıdan kapatıldı, anında yeniden başlatılıyor..."));
    m_mutterSessionPath.clear();
    QTimer::singleShot(200, this, &WaylandPortalCapture::forceImmediateRestart);
#endif
}

void WaylandPortalCapture::startPortalFlow() {
#if defined(HAVE_PIPEWIRE)
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: D-Bus oturum veri yoluna bağlanılamadı."));
        m_starting = false;
        return;
    }

    const QString handleToken  = makeToken(QStringLiteral("okidia_req"));
    const QString sessionToken = makeToken(QStringLiteral("okidia_sess"));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), handleToken);
    opts.insert(QStringLiteral("session_handle_token"), sessionToken);

    QDBusInterface portal(PORTAL_SERVICE, PORTAL_PATH, SCREENCAST_IFACE, bus);
    QDBusReply<QDBusObjectPath> reply = portal.call(QStringLiteral("CreateSession"), opts);
    if (!reply.isValid()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: CreateSession hatası: ") + reply.error().message());
        m_starting = false;
        return;
    }

    const QString reqPath = reply.value().path();
    bus.connect(PORTAL_SERVICE, reqPath, REQUEST_IFACE, QStringLiteral("Response"),
                this, SLOT(onCreateSessionResponse(uint,QVariantMap)));
#endif
}

void WaylandPortalCapture::onCreateSessionResponse(uint response, const QVariantMap& results) {
#if defined(HAVE_PIPEWIRE)
    if (response != 0) {
        Logger::warning(QObject::tr("WaylandPortalCapture: CreateSession reddedildi (kod=%1)").arg(response));
        m_starting = false;
        return;
    }

    m_sessionHandle = results.value(QStringLiteral("session_handle")).toString();
    if (m_sessionHandle.isEmpty()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: Geçersiz session_handle alındı."));
        m_starting = false;
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString handleToken = makeToken(QStringLiteral("okidia_sel"));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), handleToken);
    opts.insert(QStringLiteral("types"), static_cast<uint>(1));       // 1 = Monitor
    opts.insert(QStringLiteral("multiple"), false);
    opts.insert(QStringLiteral("cursor_mode"), static_cast<uint>(2)); // 2 = Embedded
    opts.insert(QStringLiteral("persist_mode"), static_cast<uint>(2)); // 2 = Kalıcı

    QSettings s(QStringLiteral("okidia"), QStringLiteral("tray"));
    const QString savedToken = s.value(QStringLiteral("wayland/screencast_restore_token")).toString();
    if (!savedToken.isEmpty()) {
        opts.insert(QStringLiteral("restore_token"), savedToken);
    }

    QDBusInterface portal(PORTAL_SERVICE, PORTAL_PATH, SCREENCAST_IFACE, bus);
    QDBusReply<QDBusObjectPath> reply = portal.call(
        QStringLiteral("SelectSources"), QDBusObjectPath(m_sessionHandle), opts);
    if (!reply.isValid()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: SelectSources hatası: ") + reply.error().message());
        m_starting = false;
        return;
    }

    bus.connect(PORTAL_SERVICE, reply.value().path(), REQUEST_IFACE, QStringLiteral("Response"),
                this, SLOT(onSelectSourcesResponse(uint,QVariantMap)));
#else
    Q_UNUSED(response);
    Q_UNUSED(results);
#endif
}

void WaylandPortalCapture::onSelectSourcesResponse(uint response, const QVariantMap& results) {
    Q_UNUSED(results);
#if defined(HAVE_PIPEWIRE)
    if (response != 0) {
        Logger::warning(QObject::tr("WaylandPortalCapture: SelectSources reddedildi (kod=%1)").arg(response));
        m_starting = false;
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString handleToken = makeToken(QStringLiteral("okidia_start"));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), handleToken);

    QDBusInterface portal(PORTAL_SERVICE, PORTAL_PATH, SCREENCAST_IFACE, bus);
    QDBusReply<QDBusObjectPath> reply = portal.call(
        QStringLiteral("Start"), QDBusObjectPath(m_sessionHandle), QString(), opts);
    if (!reply.isValid()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: Start hatası: ") + reply.error().message());
        m_starting = false;
        return;
    }

    bus.connect(PORTAL_SERVICE, reply.value().path(), REQUEST_IFACE, QStringLiteral("Response"),
                this, SLOT(onStartResponse(uint,QVariantMap)));
#else
    Q_UNUSED(response);
#endif
}

void WaylandPortalCapture::onStartResponse(uint response, const QVariantMap& results) {
#if defined(HAVE_PIPEWIRE)
    if (response != 0) {
        Logger::warning(QObject::tr("WaylandPortalCapture: Ekran paylaşımı kullanıcı tarafından reddedildi veya iptal edildi (kod=%1)").arg(response));
        m_starting = false;
        return;
    }

    const QString restoreToken = results.value(QStringLiteral("restore_token")).toString();
    if (!restoreToken.isEmpty()) {
        QSettings s(QStringLiteral("okidia"), QStringLiteral("tray"));
        s.setValue(QStringLiteral("wayland/screencast_restore_token"), restoreToken);
    }

    uint32_t nodeId = 0;
    const QVariant streamsVar = results.value(QStringLiteral("streams"));
    if (streamsVar.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = streamsVar.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            uint32_t id = 0;
            QVariantMap props;
            arg.beginStructure();
            arg >> id >> props;
            arg.endStructure();
            if (nodeId == 0 && id != 0) {
                nodeId = id;
            }
        }
        arg.endArray();
    }

    if (nodeId == 0) {
        Logger::warning(QObject::tr("WaylandPortalCapture: Portal akışından geçerli PipeWire node_id alınamadı."));
        m_starting = false;
        return;
    }

    QDBusInterface portal(PORTAL_SERVICE, PORTAL_PATH, SCREENCAST_IFACE, QDBusConnection::sessionBus());
    QDBusReply<QDBusUnixFileDescriptor> fdReply = portal.call(
        QStringLiteral("OpenPipeWireRemote"), QDBusObjectPath(m_sessionHandle), QVariantMap());
    if (!fdReply.isValid()) {
        Logger::warning(QObject::tr("WaylandPortalCapture: OpenPipeWireRemote hatası: ") + fdReply.error().message());
        m_starting = false;
        return;
    }

    int dupFd = ::dup(fdReply.value().fileDescriptor());
    if (dupFd < 0) {
        Logger::error(QObject::tr("WaylandPortalCapture: PipeWire FD çoğaltılamadı."));
        m_starting = false;
        return;
    }

    Logger::info(QObject::tr("WaylandPortalCapture: Portal ekran akışı başlatılıyor (node_id=%1)...").arg(nodeId));
    startPipeWireStream(dupFd, nodeId);
#else
    Q_UNUSED(response);
    Q_UNUSED(results);
#endif
}

#if defined(HAVE_PIPEWIRE)
static void onStreamStateChangedCb(void* userdata, pw_stream_state oldState, pw_stream_state state, const char* error) {
    Q_UNUSED(oldState);
    auto* self = static_cast<WaylandPortalCapture*>(userdata);
    if (state == PW_STREAM_STATE_ERROR && error) {
        Logger::warning(QObject::tr("WaylandPortalCapture: PipeWire akış hatası: ") + QString::fromUtf8(error));
    }
    if ((state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) && self && self->isRunning()) {
        QMetaObject::invokeMethod(self, [self]() {
            Logger::warning(QObject::tr("WaylandPortalCapture: Akış kesildi, anında yeniden başlatılıyor..."));
            self->forceImmediateRestart();
        }, Qt::QueuedConnection);
    }
}

void WaylandPortalCapture::onStreamParamChanged(void* userdata, uint32_t id, const struct spa_pod* param) {
    auto* self = static_cast<WaylandPortalCapture*>(userdata);
    if (!self || !param || id != SPA_PARAM_Format) return;

    struct spa_video_info_raw info;
    std::memset(&info, 0, sizeof(info));
    if (spa_format_video_raw_parse(param, &info) < 0) return;

    self->m_videoW = info.size.width;
    self->m_videoH = info.size.height;
    self->m_videoFmt = info.format;

    Logger::info(QObject::tr("WaylandPortalCapture: Video formatı müzakere edildi → %1x%2 (fmt=%3)")
                 .arg(self->m_videoW).arg(self->m_videoH).arg(self->m_videoFmt));
}

void WaylandPortalCapture::onStreamProcess(void* userdata) {
    auto* self = static_cast<WaylandPortalCapture*>(userdata);
    if (!self || !self->m_pwStream) return;

    struct pw_buffer* b = pw_stream_dequeue_buffer(self->m_pwStream);
    if (!b) return;

    struct spa_buffer* buf = b->buffer;
    if (buf && buf->n_datas > 0 && buf->datas[0].data && self->m_videoW > 0 && self->m_videoH > 0) {
        const uint8_t* src = static_cast<const uint8_t*>(buf->datas[0].data);
        const int stride = buf->datas[0].chunk ? static_cast<int>(buf->datas[0].chunk->stride) : (self->m_videoW * 4);

        QImage::Format qfmt = QImage::Format_RGB32;
        if (self->m_videoFmt == SPA_VIDEO_FORMAT_BGRA) {
            qfmt = QImage::Format_ARGB32;
        } else if (self->m_videoFmt == SPA_VIDEO_FORMAT_RGBA || self->m_videoFmt == SPA_VIDEO_FORMAT_RGBx) {
            qfmt = QImage::Format_RGBA8888;
        }

        if (stride >= self->m_videoW * 4) {
            QImage frame(src, self->m_videoW, self->m_videoH, stride, qfmt);
            QImage copy = (qfmt == QImage::Format_RGBA8888)
                ? frame.convertToFormat(QImage::Format_RGB32)
                : frame.copy();

            QMutexLocker lock(&self->m_frameMutex);
            self->m_latestFrame = std::move(copy);
            self->m_hasPipeWireFrame = true;
        }
    }

    pw_stream_queue_buffer(self->m_pwStream, b);
}
#endif

void WaylandPortalCapture::startPipeWireStream(int fd, uint32_t nodeId) {
#if defined(HAVE_PIPEWIRE)
    cleanupPipeWire();
    pw_init(nullptr, nullptr);

    m_pwLoop = pw_thread_loop_new("okidia-pw", nullptr);
    if (!m_pwLoop) {
        if (fd >= 0) ::close(fd);
        m_starting = false;
        return;
    }

    pw_thread_loop_lock(m_pwLoop);

    m_pwCtx = pw_context_new(pw_thread_loop_get_loop(m_pwLoop), nullptr, 0);
    if (!m_pwCtx) {
        pw_thread_loop_unlock(m_pwLoop);
        if (fd >= 0) ::close(fd);
        cleanupPipeWire();
        m_starting = false;
        return;
    }

    m_pwCore = (fd >= 0)
        ? pw_context_connect_fd(m_pwCtx, fd, nullptr, 0)
        : pw_context_connect(m_pwCtx, nullptr, 0);
    if (!m_pwCore) {
        pw_thread_loop_unlock(m_pwLoop);
        if (fd >= 0) ::close(fd);
        cleanupPipeWire();
        m_starting = false;
        return;
    }

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr);

    m_pwStream = pw_stream_new(m_pwCore, "okidia-screencast", props);
    if (!m_pwStream) {
        pw_thread_loop_unlock(m_pwLoop);
        cleanupPipeWire();
        m_starting = false;
        return;
    }

    static const struct pw_stream_events streamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = onStreamStateChangedCb,
        .param_changed = &WaylandPortalCapture::onStreamParamChanged,
        .process = &WaylandPortalCapture::onStreamProcess,
    };

    m_streamListener = new spa_hook();
    std::memset(m_streamListener, 0, sizeof(spa_hook));
    pw_stream_add_listener(m_pwStream, m_streamListener, &streamEvents, this);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_rectangle defSize = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle minSize = SPA_RECTANGLE(1, 1);
    struct spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);
    struct spa_fraction defFps = SPA_FRACTION(15, 1);
    struct spa_fraction minFps = SPA_FRACTION(0, 1);
    struct spa_fraction maxFps = SPA_FRACTION(60, 1);

    const struct spa_pod* params[1];
    params[0] = static_cast<const struct spa_pod*>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defFps, &minFps, &maxFps)));

    int res = pw_stream_connect(
        m_pwStream,
        PW_DIRECTION_INPUT,
        nodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);

    if (res < 0) {
        Logger::warning(QObject::tr("WaylandPortalCapture: pw_stream_connect başarısız (%1)").arg(res));
        pw_thread_loop_unlock(m_pwLoop);
        cleanupPipeWire();
        m_starting = false;
        return;
    }

    pw_thread_loop_unlock(m_pwLoop);
    pw_thread_loop_start(m_pwLoop);

    m_starting = false;
    m_running = true;
    Logger::info(QObject::tr("WaylandPortalCapture: PipeWire ekran yakalama akışı aktif."));
#else
    Q_UNUSED(fd);
    Q_UNUSED(nodeId);
#endif
}

void WaylandPortalCapture::cleanupPipeWire() {
#if defined(HAVE_PIPEWIRE)
    if (m_pwLoop) {
        pw_thread_loop_stop(m_pwLoop);
        pw_thread_loop_lock(m_pwLoop);
    }
    if (m_streamListener) {
        spa_hook_remove(m_streamListener);
        delete m_streamListener;
        m_streamListener = nullptr;
    }
    if (m_pwStream) {
        pw_stream_destroy(m_pwStream);
        m_pwStream = nullptr;
    }
    if (m_pwCore) {
        pw_core_disconnect(m_pwCore);
        m_pwCore = nullptr;
    }
    if (m_pwCtx) {
        pw_context_destroy(m_pwCtx);
        m_pwCtx = nullptr;
    }
    if (m_pwLoop) {
        pw_thread_loop_unlock(m_pwLoop);
        pw_thread_loop_destroy(m_pwLoop);
        m_pwLoop = nullptr;
    }
#endif
    m_hasPipeWireFrame = false;
    m_running = false;
}
