// agent/commands/CommandHandlerScreen.cpp
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/OkidiaConfig.h"
#include "../capture/ScreenCapture.h"
#include "../ui/ScreenLockOverlay/ScreenLockOverlay.h"
#include "CommandHandler.h"
#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QDateTime>
#include <QPixmap>
#include <QScreen>
#ifdef HAVE_XTST
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#include <QSettings>
#include "../ui/SmartboardLockOverlay/SmartboardLockOverlay.h"
#include "../ui/RecessDismissDialog/RecessDismissDialog.h"
#include "../auth/OfflineAuthManager.h"
#include "../system/SessionSwitchManager.h"

void CommandHandler::screenLock(const okidia::ScreenLockCommand &cmd) {
  // Tip 2: İdari PC (KVKK gereği ekran kilitlenemez)
  if (m_agentType == 2) {
    Logger::info(QObject::tr("CommandHandler: İdari PC (Tip 2) KVKK gereği SCREEN_LOCK komutu yoksayıldı."));
    sendResult(false, QObject::tr("İdari PC ekran kilidi KVKK gereği kısıtlanamaz."));
    return;
  }

  // Teneffüste Öğrenci Oturumu Kapatma (lock_policy == 3)
  if (cmd.lock_policy() == 3) {
    if (cmd.grace_period_sec() > 0) {
      if (RecessDismissDialog::instance().isRunning()) {
        Logger::info(QObject::tr("CommandHandler: Teneffüs oturum kapatma nezaket penceresi zaten aktif, mükerrer istek atlandı."));
        sendResult(true, QObject::tr("Teneffüs oturum kapatma nezaket penceresi devrede"));
        return;
      }

      Logger::info(QObject::tr("CommandHandler: Teneffüs öğrenci oturum kapatma nezaket penceresi başlatılıyor (%1 sn)").arg(cmd.grace_period_sec()));

      QObject::disconnect(&RecessDismissDialog::instance(), &RecessDismissDialog::logoutNowRequested, nullptr, nullptr);
      QObject::connect(&RecessDismissDialog::instance(), &RecessDismissDialog::logoutNowRequested, [this, cmd]() {
        okidia::ScreenLockCommand immediateCmd = cmd;
        immediateCmd.set_grace_period_sec(0);
        screenLock(immediateCmd);
      });

      QString msg = QString::fromStdString(cmd.message());
      RecessDismissDialog::instance().showGracePeriod(cmd.grace_period_sec(), msg, true);
      sendResult(true, QObject::tr("Teneffüs oturum kapatma nezaket penceresi devrede (%1 sn)").arg(cmd.grace_period_sec()));
      return;
    }

    RecessDismissDialog::instance().dismiss();
    Logger::info(QObject::tr("CommandHandler: Teneffüs süresi doldu, öğrenci oturumu kapatılıyor..."));
    powerLogout();
    return;
  }

  // Teneffüs nezaket süresi (Grace Period) kontrolü:
  // Eğer grace_period_sec > 0 ise ve ekran zaten kilitli değilse,
  // doğrudan kilitlemek yerine nezaket penceresini gösterip bekle.
  bool isAlreadyLocked = SmartboardLockOverlay::instance().isLocked() ||
                         ScreenLockOverlay::instance().isLocked();

  if (cmd.grace_period_sec() > 0) {
    if (isAlreadyLocked) {
      sendResult(true, QObject::tr("Ekran zaten kilitli"));
      return;
    }

    if (RecessDismissDialog::instance().isRunning()) {
      Logger::info(QObject::tr("CommandHandler: Teneffüs nezaket penceresi zaten aktif, mükerrer istek atlandı."));
      sendResult(true, QObject::tr("Teneffüs nezaket penceresi devrede"));
      return;
    }

    Logger::info(QObject::tr("CommandHandler: Teneffüs nezaket penceresi başlatılıyor (%1 sn)").arg(cmd.grace_period_sec()));

    // Tek seferlik bağlantı: süre dolduğunda veya 'Şimdi Kilitle'ye basıldığında kilit uygula
    QObject::disconnect(&RecessDismissDialog::instance(), &RecessDismissDialog::lockNowRequested, nullptr, nullptr);
    QObject::connect(&RecessDismissDialog::instance(), &RecessDismissDialog::lockNowRequested, [this, cmd]() {
      okidia::ScreenLockCommand immediateCmd = cmd;
      immediateCmd.set_grace_period_sec(0);
      screenLock(immediateCmd);
    });

    QString msg = QString::fromStdString(cmd.message());
    RecessDismissDialog::instance().showGracePeriod(cmd.grace_period_sec(), msg);
    sendResult(true, QObject::tr("Teneffüs nezaket penceresi devrede (%1 sn)").arg(cmd.grace_period_sec()));
    return;
  }

  // Doğrudan kilitleniyorsa açık kalan nezaket penceresini kapat
  RecessDismissDialog::instance().dismiss();

  bool usbProtection = QSettings("okidia", "agent").value("policy/usb_protection_enabled", false).toBool()
                    || OkidiaConfig::isUsbProtectionEnabled();
  if (usbProtection) {
    QProcess::startDetached("sync", {});
  }

  QString msg = QString::fromStdString(cmd.message());

  // Kilit politikası kontrolü:
  // cmd.lock_policy():
  // 0 = Varsayılan / Belirtilmemiş (Ajanın yerel ayarına bakılır; yoksa Tip 3 için QR Kilit, Tip 1 için Standart Perde)
  // 1 = Zorunlu Normal Perde (ScreenLockOverlay)
  // 2 = Dinamik QR & PIN Kilit (Sadece Tip 3 Akıllı Tahtalar için)
  uint32_t effectivePolicy = cmd.lock_policy();
  if (effectivePolicy == 0) {
    QSettings sApp("okidia", "agent");
    effectivePolicy = sApp.value("smartboard/lockPolicy", 0).toUInt();
  } else {
    QSettings sApp("okidia", "agent");
    sApp.setValue("smartboard/lockPolicy", effectivePolicy);
  }

  bool useQrLock = (m_agentType == 3 && effectivePolicy != 1);

  Logger::info(QObject::tr("CommandHandler: Ekran kilitleniyor (Tip %1, gelen_policy: %2, efektif_policy: %3, useQrLock: %4) → mesaj: %5")
               .arg(m_agentType).arg(cmd.lock_policy()).arg(effectivePolicy).arg(useQrLock ? "EVET" : "HAYIR").arg(msg));

  if (useQrLock) {
    // Normal perde açıksa karşılıklı dışlama (mutual exclusion) gereği kapat
    if (ScreenLockOverlay::instance().isLocked()) {
      ScreenLockOverlay::instance().hideLock();
    }

    if (SmartboardLockOverlay::instance().isLocked()) {
      sendResult(true, QObject::tr("Akıllı Tahta QR kilit ekranı devrede"));
      return;
    }

    // Açık olan öğretmen oturumunu askıya al/kilitle (Oturum kapatılmaz, askıda tutulur)
    SessionSwitchManager::instance().terminateCurrentSession();
    SmartboardLockOverlay::instance().showSmartboardLock(msg);

    // Dev/Test telemetrisi: aktif geçerli PIN'i Master'a raporla
    QString activePin = OfflineAuthManager::instance().currentTestPin();
    int remSec = OfflineAuthManager::instance().remainingSeconds();

    okidia::SmartboardLockPinReport report;
    report.set_client_id("current");
    report.set_current_pin(activePin.toStdString());
    report.set_remaining_seconds(static_cast<uint32_t>(remSec));
    report.set_timestamp(static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch()));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SMARTBOARD_LOCK_PIN_REPORT);
    env.set_payload(report.SerializeAsString());
    if (m_sender) m_sender(env);

    sendResult(true, QObject::tr("Akıllı Tahta QR kilit ekranı devrede"));
    return;
  }

  // Normal Perde Modu: QR kilit ekranı açıksa kesinlikle kapat!
  if (SmartboardLockOverlay::instance().isLocked()) {
    SmartboardLockOverlay::instance().hideSmartboardLock();
  }

  // Tip 1 veya Tip 3 Normal Perde Kilidi
  if (ScreenLockOverlay::instance().isLocked()) {
    ScreenLockOverlay::instance().setMessage(msg);
    sendResult(true, QObject::tr("Ekran zaten kilitli"));
    return;
  }

  ScreenLockOverlay::instance().showLock(msg);
  sendResult(true, QObject::tr("Ekran kilitlendi"));
}

void CommandHandler::screenUnlock() {
  if (m_agentType == 2) {
    return; // Tip 2 İdari PC zaten kilitlenmez
  }

  Logger::info(QObject::tr("CommandHandler: Ekran kilidi açılıyor (Tip %1)").arg(m_agentType));

  RecessDismissDialog::instance().dismiss();
  SmartboardLockOverlay::instance().hideSmartboardLock();
  ScreenLockOverlay::instance().hideLock();
  sendResult(true, QObject::tr("Ekran kilidi açıldı"));
}

void CommandHandler::screenSelect(const okidia::ScreenSelectCommand &cmd) {
  if (!m_capture) {
    sendResult(false, QObject::tr("ScreenCapture bağlı değil"));
    return;
  }

  CaptureSource src;
  switch (cmd.source()) {
  case okidia::ScreenSelectCommand::MONITOR:
    src.type = CaptureSource::Monitor;
    src.monitorIndex = static_cast<int>(cmd.monitor_index());
    Logger::info(QObject::tr("CommandHandler: Ekran seçildi → monitör ") +
                 QString::number(src.monitorIndex));
    break;
  case okidia::ScreenSelectCommand::WINDOW:
    src.type = CaptureSource::Window;
    src.windowId = cmd.window_id();
    Logger::info(QObject::tr("CommandHandler: Ekran seçildi → pencere WID:") +
                 QString::number(src.windowId));
    break;
  case okidia::ScreenSelectCommand::ALL:
  default:
    src.type = CaptureSource::AllMonitors;
    Logger::info(QObject::tr("CommandHandler: Ekran seçildi → tüm monitörler"));
    break;
  }

  m_capture->setSource(src);

  // Hemen tam frame gönder — yeni kaynak için buffer sıfırlandı
  if (m_sender) {
    QByteArray fullFrame = m_capture->captureFullFrame();
    if (!fullFrame.isEmpty()) {
      okidia::ScreenFrame frame;
      frame.set_width(static_cast<uint32_t>(m_capture->screenWidth()));
      frame.set_height(static_cast<uint32_t>(m_capture->screenHeight()));
      frame.set_jpeg_data(fullFrame.constData(), fullFrame.size());
      frame.set_is_full_frame(true);

      okidia::Envelope env;
      env.set_type(okidia::MessageType::SCREEN_FRAME);
      env.set_payload(frame.SerializeAsString());
      if (m_sender)
        m_sender(env);
    }
  }
  sendResult(true);
}

void CommandHandler::handleScreenConfig(const okidia::ScreenConfig &cmd) {
  if (!m_capture)
    return;

  // Uzak kontrol veya tam ekran akışı aktifken:
  // Eğer gelen komutta max_width belirtilmemişse (cmd.max_width() <= 0),
  // bu Master'ın thumbnail adaptif güncellemesidir!
  // Aktif yüksek kaliteli akışı thumbnail ölçeğine (%25) DÜŞÜRME!
  if ((m_remoteControlActive || m_screenStreamActive) && cmd.max_width() <= 0) {
    Logger::info(QObject::tr("CommandHandler: Uzak kontrol aktif, thumbnail FPS/ölçek ezmesi engellendi"));
    return;
  }

  CaptureSettings settings = m_capture->settings();
  if (cmd.interval_ms() > 0) {
    settings.fps = 1000 / cmd.interval_ms();
    if (settings.fps > 30)
      settings.fps = 30; // Max 30 fps
  }
  if (cmd.jpeg_quality() > 0) {
    settings.jpegQuality = cmd.jpeg_quality();
  }
  if (cmd.max_width() > 0) {
    settings.maxWidth = cmd.max_width();
    int screenW = m_capture->screenWidth();
    if (screenW > 0) {
      settings.scalePercent = (cmd.max_width() * 100) / screenW;
      if (settings.scalePercent > 100)
        settings.scalePercent = 100;
      if (settings.scalePercent < 10)
        settings.scalePercent = 10;
    }
  } else {
    // Genişlik belirtilmemişse varsayılan %25 thumbnail ölçeği
    settings.scalePercent = 25;
  }

  m_capture->updateSettings(settings);

  Logger::info(QObject::tr("CommandHandler: Ekran ayarları güncellendi -> FPS: %1, "
                       "Kalite: %2, MaxWidth: %3, Ölçek: %4%")
                   .arg(settings.fps)
                   .arg(settings.jpegQuality)
                   .arg(settings.maxWidth)
                   .arg(settings.scalePercent));
}

void CommandHandler::screenInfoRequest() {
  if (!m_sender)
    return;

  Logger::info(QObject::tr("CommandHandler: Monitör bilgisi istendi"));

  okidia::ScreenInfoResponse resp;

  // Monitörler
  const auto monitors = ScreenCapture::availableMonitors();
  for (const auto &m : monitors) {
    auto *mi = resp.add_monitors();
    mi->set_index(static_cast<uint32_t>(m.index));
    mi->set_name(m.name.toStdString());
    mi->set_x(m.x);
    mi->set_y(m.y);
    mi->set_width(static_cast<uint32_t>(m.width));
    mi->set_height(static_cast<uint32_t>(m.height));
    mi->set_is_primary(m.isPrimary);
  }

  // Aktif pencereler (X11)
#ifdef HAVE_XTST
  Display *dpy = XOpenDisplay(nullptr);
  if (dpy) {
    Window root = DefaultRootWindow(dpy);
    Atom prop = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
    Atom type;
    int format;
    ulong count, remaining;
    uchar *data = nullptr;

    if (prop != None &&
        XGetWindowProperty(dpy, root, prop, 0, 1024, False, XA_WINDOW, &type,
                           &format, &count, &remaining, &data) == Success &&
        data) {
      auto *wins = reinterpret_cast<Window *>(data);
      for (ulong i = 0; i < count; ++i) {
        // Pencere başlığını al — önce _NET_WM_NAME (UTF-8), sonra XFetchName
        QString title;

#ifdef HAVE_XTST
        // _NET_WM_NAME atom — UTF-8 başlık
        Atom netWmName = XInternAtom(dpy, "_NET_WM_NAME", False);
        Atom utf8Str = XInternAtom(dpy, "UTF8_STRING", False);
        Atom actualType;
        int actualFmt;
        ulong nItems, bytesAfter;
        unsigned char *prop = nullptr;

        if (XGetWindowProperty(dpy, wins[i], netWmName, 0, 1024, False, utf8Str,
                               &actualType, &actualFmt, &nItems, &bytesAfter,
                               &prop) == Success &&
            prop) {
          title =
              QString::fromUtf8(reinterpret_cast<const char *>(prop), nItems);
          XFree(prop);
        }
#endif
        // Fallback: XFetchName (Latin-1 → UTF-8 dönüştür)
        if (title.isEmpty()) {
          char *name = nullptr;
          XFetchName(dpy, wins[i], &name);
          if (name) {
            title = QString::fromLocal8Bit(name);
            XFree(name);
          }
        }

        if (title.isEmpty())
          continue;

        // UTF-8 geçerliliğini garantile
        if (!QString::fromUtf8(title.toUtf8()).isEmpty())
          title = QString::fromUtf8(title.toUtf8());

        XWindowAttributes attrs;
        if (!XGetWindowAttributes(dpy, wins[i], &attrs) ||
            attrs.map_state != IsViewable) {
          continue;
        }

        auto *w = resp.add_windows();
        w->set_title(title.toUtf8().toStdString());
        w->set_wid(static_cast<uint64_t>(wins[i]));
        w->set_x(static_cast<uint32_t>(attrs.x));
        w->set_y(static_cast<uint32_t>(attrs.y));
        w->set_width(static_cast<uint32_t>(attrs.width));
        w->set_height(static_cast<uint32_t>(attrs.height));
      }
      XFree(data);
    }
    XCloseDisplay(dpy);
  }
#endif

  okidia::Envelope env;
  env.set_type(okidia::MessageType::SCREEN_INFO_RESPONSE);
  env.set_payload(resp.SerializeAsString());
  if (m_sender)
    m_sender(env);
}

void CommandHandler::screenCapture() {
  Logger::info(QObject::tr("CommandHandler: Ekran görüntüsü alınıyor"));

  QScreen *screen = QApplication::primaryScreen();
  if (!screen) {
    sendResult(false, QObject::tr("Ekran bulunamadı"));
    return;
  }

  QPixmap screenshot = screen->grabWindow(0);
  QByteArray pngData;
  QBuffer buffer(&pngData);
  buffer.open(QIODevice::WriteOnly);
  screenshot.save(&buffer, "PNG");
  buffer.close();

  if (pngData.isEmpty()) {
    sendResult(false, QObject::tr("Ekran görüntüsü alınamadı"));
    return;
  }

  if (!m_sender)
    return;

  okidia::ScreenCaptureResult result;
  result.set_png_data(pngData.toStdString());
  result.set_timestamp(OkidiaUtils::nowSecsU64());
  result.set_width(static_cast<uint32_t>(screenshot.width()));
  result.set_height(static_cast<uint32_t>(screenshot.height()));

  okidia::Envelope env;
  env.set_type(okidia::MessageType::SCREEN_CAPTURE_RESULT);
  env.set_payload(result.SerializeAsString());
  if (m_sender)
    m_sender(env);

  Logger::info(QObject::tr("CommandHandler: Ekran görüntüsü gönderildi → ") +
               QString::number(pngData.size() / 1024) + QObject::tr(" KB"));
}
