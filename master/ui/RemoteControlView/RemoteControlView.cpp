#include <QMessageBox>
#include "../../ssh/SshTerminalWidget.h"
#include "../../ssh/SshClient.h"
#include "../../ssh/SshCredentialDialog.h"
#include "../../ssh/SshDeviceSessionWidget.h"
#include <QMenu>
// master/ui/RemoteControlView/RemoteControlView.cpp
#include "RemoteControlView.h"
#include "../../../common/ui/ThemeManager.h"
#include "ui_RemoteControlView.h"
#include "../../common/utils/Logger.h"
#include "../../ssh/SshFileManagerWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QApplication>
#include <QEvent>
#include <QScreen>
#include <QPainter>
#include <QLabel>
#include <QSet>
#include <QFocusEvent>
#include <QCursor>
#include <QSettings>

// ---------------------------------------------------------------------------
// Qt::Key → X11 KeySym dönüştürücü
// nativeVirtualKey() Linux/X11'de XKB keycode döndürür, keysym değil.
// Qt::Key değerleri X11 keysym ile çakışır ama tam eşleşme yoktur.
// Bu tablo sıkça kullanılan tuşlar için doğru X11 keysym eşlemesi yapar.
// ---------------------------------------------------------------------------
static uint32_t qtKeyToX11Keysym(QKeyEvent* event) {
    int qtKey = event->key();
    uint32_t nativeVirtualKey = static_cast<uint32_t>(event->nativeVirtualKey());
    Qt::KeyboardModifiers mods = event->modifiers();

    // 1. Mod tuşları ve özel kontrol/fonksiyon tuşları
    switch (qtKey) {
    // ― Özel tuşlar ―――――――――――――――――――――――――――――――――――
    case Qt::Key_Return:       return 0xFF0D;  // XK_Return
    case Qt::Key_Enter:        return 0xFF8D;  // XK_KP_Enter
    case Qt::Key_Backspace:    return 0xFF08;  // XK_BackSpace
    case Qt::Key_Delete:       return 0xFFFF;  // XK_Delete
    case Qt::Key_Insert:       return 0xFF63;  // XK_Insert
    case Qt::Key_Tab:          return 0xFF09;  // XK_Tab
    case Qt::Key_Backtab:      return 0xFE20;  // XK_ISO_Left_Tab (Shift+Tab)
    case Qt::Key_Escape:       return 0xFF1B;  // XK_Escape
    case Qt::Key_Space:        return 0x0020;  // XK_space
    // ― Yön tuşları ―――――――――――――――――――――――――――――――――
    case Qt::Key_Left:         return 0xFF51;  // XK_Left
    case Qt::Key_Up:           return 0xFF52;  // XK_Up
    case Qt::Key_Right:        return 0xFF53;  // XK_Right
    case Qt::Key_Down:         return 0xFF54;  // XK_Down
    case Qt::Key_Home:         return 0xFF50;  // XK_Home
    case Qt::Key_End:          return 0xFF57;  // XK_End
    case Qt::Key_PageUp:       return 0xFF55;  // XK_Prior
    case Qt::Key_PageDown:     return 0xFF56;  // XK_Next
    // ― Fonksiyon tuşları ――――――――――――――――――――――――――――――
    case Qt::Key_F1:           return 0xFFBE;
    case Qt::Key_F2:           return 0xFFBF;
    case Qt::Key_F3:           return 0xFFC0;
    case Qt::Key_F4:           return 0xFFC1;
    case Qt::Key_F5:           return 0xFFC2;
    case Qt::Key_F6:           return 0xFFC3;
    case Qt::Key_F7:           return 0xFFC4;
    case Qt::Key_F8:           return 0xFFC5;
    case Qt::Key_F9:           return 0xFFC6;
    case Qt::Key_F10:          return 0xFFC7;
    case Qt::Key_F11:          return 0xFFC8;
    case Qt::Key_F12:          return 0xFFC9;
    // ― Mod tuşları ―――――――――――――――――――――――――――――――――――
    case Qt::Key_Shift:        return 0xFFE1;  // XK_Shift_L
    case Qt::Key_Control:      return 0xFFE3;  // XK_Control_L
    case Qt::Key_Alt:          return 0xFFE9;  // XK_Alt_L
    case Qt::Key_AltGr:        return 0xFE03;  // XK_ISO_Level3_Shift (AltGr)
    case Qt::Key_Meta:         return 0xFFEB;  // XK_Super_L
    case Qt::Key_CapsLock:     return 0xFFE5;  // XK_Caps_Lock
    case Qt::Key_NumLock:      return 0xFF7F;  // XK_Num_Lock
    case Qt::Key_ScrollLock:   return 0xFF14;  // XK_Scroll_Lock
    case Qt::Key_Print:        return 0xFF61;  // XK_Print
    case Qt::Key_Pause:        return 0xFF13;  // XK_Pause
    default:
        break;
    }

    // 2. Yazdırılabilir metin kontrolü (Shift, AltGr, CapsLock ve klavye düzeni kombinasyonları)
    // Salt Control veya Meta basılıyken (Alt/AltGr olmadan) metin yerine kısayol keysym'ini tercih et
    bool isPureCtrlOrMeta = (mods & (Qt::ControlModifier | Qt::MetaModifier)) && !(mods & Qt::AltModifier);

    if (!isPureCtrlOrMeta) {
        QString text = event->text();
        if (!text.isEmpty()) {
            QList<uint> ucs4 = text.toUcs4();
            if (!ucs4.isEmpty()) {
                uint32_t codepoint = ucs4.first();
                if (codepoint >= 0x20 && codepoint != 0x7F) {
                    // ASCII (0x20..0x7E) ve Latin-1 Supplement (0xA0..0xFF, e.g. ½, ¾, §, £, ç, ö, ü vb.)
                    // X11 KeySym standardında 0x0020..0x00FF aralığı doğrudan kod noktasıdır.
                    if (codepoint <= 0x00FF) {
                        return codepoint;
                    }
                    // Latin-1 dışındaki Unicode karakterler (0x0100..), örneğin €, ₺, ğ, ı, ş vb.
                    // X11 standardında 0x01000000 | codepoint olarak iletilir.
                    return 0x01000000 | codepoint;
                }
            }
        }
    }

    // 3. Kısayollar ve metin alanı boş olan tuşlar (örn. Ctrl+C, harfler, sayılar)
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        bool shifted = (mods & Qt::ShiftModifier);
        return static_cast<uint32_t>(qtKey + (shifted ? 0 : 0x20));
    }

    if (qtKey >= 0x20 && qtKey <= 0x7E) {
        return static_cast<uint32_t>(qtKey);
    }

    // Linux/X11 üzerinde nativeVirtualKey (108 AltGr/Alt_R keycode'udur, 'l' keysym'ine düşmemeli)
    if (nativeVirtualKey != 0 && nativeVirtualKey != 108) {
        return nativeVirtualKey;
    }

    return 0;
}

RemoteControlView::RemoteControlView(SessionManager* session,
                                      const QString& clientId,
                                      QWidget* parent,
                                      Mode mode)
    : QWidget(parent, Qt::Window)
    , ui(new Ui::RemoteControlView)
    , m_session(session)
    , m_clientId(clientId)
    , m_mode(mode) {

    ui->setupUi(this);
    m_baseTitle = clientId;
    if (session) {
        auto st = session->clientState(clientId);
        if (!st.hostname.isEmpty()) {
            m_baseTitle = st.hostname;
        }
    }
    setMouseTracking(true);
    // screenLabel fare eventlerine sayıdam yap:
    // Bu attribute ile tüm mouse event'leri doğrudan RemoteControlView'a gider.
    // Çift event (child + parent) sorunu olmaz.
    ui->screenLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    ui->screenLabel->setMouseTracking(false);  // Track sadece parent'ta
    setFocusPolicy(Qt::StrongFocus);

    // Sağ tıkta Qt'un kendi context menüsünü açmasını engelle.
    // Qt context menüsü açılırsa mouse capture çalışır ve mouseReleaseEvent
    // tetiklenmez → agent'ta sağ buton basılı kalır → hover = seçim yapılır.
    setContextMenuPolicy(Qt::PreventContextMenu);

    // İstemci adı
    ui->clientLabel->setText("" + clientId);

    // Buton bağlantıları
    connect(ui->btnStop,       &QPushButton::clicked,
            this, &RemoteControlView::onStopClicked);
    connect(ui->btnFullscreen, &QPushButton::clicked,
            this, &RemoteControlView::onFullscreenClicked);
    ui->btnFullscreen->setIcon(ThemeManager::icon(":/ui/fullscreen.svg"));
    connect(ui->btnViewOnly,   &QPushButton::clicked,
            this, &RemoteControlView::onViewOnlyClicked);
    connect(ui->btnControl,    &QPushButton::clicked,
            this, &RemoteControlView::onControlClicked);
    connect(ui->btnLockAgentInput, &QPushButton::clicked,
            this, &RemoteControlView::onLockAgentInputClicked);
            
    ui->btnFileManager->setText(tr("SSH"));
    ui->btnFileManager->setIcon(ThemeManager::icon(":/ui/terminal.svg"));
    connect(ui->btnFileManager, &QPushButton::clicked, this, &RemoteControlView::onSshClicked);

    setupShortcutsMenu();

    // Başlangıçta agent girdisi açık (kilitli değil) varsayalım
    ui->btnLockAgentInput->setChecked(false);
    ui->btnLockAgentInput->setIcon(ThemeManager::icon(":/ui/unlock.svg"));

    // Ekran kaynağı menüsü
    ui->comboSource->addItem(tr("Tüm Ekranlar"), -1);
    ui->comboSource->addItem(tr("Ekran 1"), 0);
    ui->comboSource->addItem(tr("Ekran 2"), 1);
    ui->comboSource->setCurrentIndex(0); // Tüm Ekranlar

    connect(ui->comboSource, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RemoteControlView::onSourceChanged);

    // Kalite combobox'ını ekle
    m_cbQuality = new QComboBox(this);
    m_cbQuality->addItem(tr("Orijinal (1080p, En Net)"), 1); // 1920w, %85 jpeg
    m_cbQuality->addItem(tr("Yüksek (1080p)"), 2);           // 1920w, %75 jpeg
    m_cbQuality->addItem(tr("Orta (720p)"), 3);              // 1280w, %70 jpeg
    m_cbQuality->addItem(tr("Düşük (480p)"), 4);             // 854w, %60 jpeg

    QSettings settings("okidia", "master");
    int savedQualityIndex = settings.value("RemoteControl/QualityIndex", 0).toInt();
    if (savedQualityIndex >= m_cbQuality->count() || savedQualityIndex < 0) {
        savedQualityIndex = 0;
    }
    
    ui->topBarLayout->insertWidget(2, m_cbQuality);
    
    connect(m_cbQuality, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RemoteControlView::onQualityChanged);

    m_cbQuality->setCurrentIndex(savedQualityIndex);
    // İlk açılışta seçili kaliteyi doğrudan Agent'a gönder
    onQualityChanged(savedQualityIndex);

    // Bilgi isteği MainWindow tarafından yönetiliyor, burada tekrar istemeye gerek yok
    // if (m_session) m_session->sendScreenInfoRequest(clientId);

    m_frameBuffer = QPixmap(m_remoteW, m_remoteH);
    m_frameBuffer.fill(Qt::black);

    m_mouseThrottleTimer.start();
    updateModeButtons();

    // Sanal fare imleci (overlay)
    m_remoteCursorLabel = new QLabel(ui->screenLabel);
    m_remoteCursorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_remoteCursorLabel->setAttribute(Qt::WA_TranslucentBackground);
    m_remoteCursorLabel->setStyleSheet("background: transparent; border: none;");
    m_remoteCursorLabel->setFixedSize(24, 24);
    m_remoteCursorLabel->hide();

    // Canlı ekran yayını bilgilendirme rozeti (Hafif durum bildirimi)
    m_broadcastNoticeLabel = new QLabel(this);
    m_broadcastNoticeLabel->setObjectName("broadcastNoticeLabel");
    m_broadcastNoticeLabel->setStyleSheet(
        "background-color: #854d0e; color: #fef08a; font-size: 11px; font-weight: bold; "
        "border-radius: 4px; padding: 2px 8px; border: 1px solid #ca8a04;"
    );
    m_broadcastNoticeLabel->hide();
    ui->topBarLayout->insertWidget(ui->topBarLayout->count() - 1, m_broadcastNoticeLabel);

    // ― Stream kopma overlay’i ――――――――――――――――――――――――――――――――――――――――――
    m_streamOverlay = new QLabel(this);
    m_streamOverlay->setAlignment(Qt::AlignCenter);
    m_streamOverlay->setText(
        "<div style='color:#fff; font-size:22px; font-weight:bold;'>" +
        tr("Yeniden Bağlanıyor...") + "</div>"
        "<div style='color:#ccc; font-size:13px; margin-top:8px;'>" +
        tr("Oturum geçişi algılandı. Ekran akışı yeniden başlatılıyor.") + "</div>"
    );
    m_streamOverlay->setStyleSheet(
        "background: rgba(15, 15, 20, 210);"
        "border-radius: 12px;"
        "padding: 28px 48px;"
    );
    m_streamOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_streamOverlay->hide();

    // Frame watchdog: İlk açılışta akışın (WebRTC/VNC/JPEG) başlaması ve el sıkışma için 8 sn bekle
    m_frameWatchdog = new QTimer(this);
    m_frameWatchdog->setInterval(4000);
    m_frameWatchdog->setSingleShot(true);
    connect(m_frameWatchdog, &QTimer::timeout, this, [this]() {
        showStreamOverlay(true);
    });
    m_frameWatchdog->start(8000);

    // Normal pencere olarak aç, başlık çubuğu ile taşınabilir
    updateTitle();
    setMinimumSize(640, 480);
    resize(1024, 680);
    show();
    setFocus();
}

RemoteControlView::~RemoteControlView() { delete ui; }

void RemoteControlView::setBroadcastNotice(bool active, const QString& teacherName) {
    if (!m_broadcastNoticeLabel) return;
    if (active) {
        QString teacher = teacherName.isEmpty() ? tr("Öğretmen") : teacherName;
        m_broadcastNoticeLabel->setText(QString(tr("📢 %1 Sınıfta Canlı Yayında")).arg(teacher));
        m_broadcastNoticeLabel->show();
    } else {
        m_broadcastNoticeLabel->hide();
    }
}

// (eventFilter artık kullanılmıyor — screenLabel WA_TransparentForMouseEvents ile fare olaylarına
//  sayıdam. Tüm mouse event'leri doğrudan RemoteControlView'a geliyor.)
bool RemoteControlView::eventFilter(QObject* obj, QEvent* event) {
    return QWidget::eventFilter(obj, event);
}

void RemoteControlView::updateRemoteFrame(const QByteArray& jpegData,
                                             int w, int h) {
    if (jpegData.isEmpty()) return;
    QPixmap px;
    if (!px.loadFromData(jpegData, "JPEG")) return;

    // Frame geldi → overlay'i gizle ve watchdog'u yeniden başlat
    showStreamOverlay(false);
    if (m_frameWatchdog) {
        m_frameWatchdog->start(4000);
    }

    m_remoteW = w > 0 ? w : px.width();
    m_remoteH = h > 0 ? h : px.height();

    // Gelen pixmap'i olduğu gibi frameBuffer'a ata.
    // Ölçekleme yalnızca renderFrame() içinde tek seferde yapılır.
    m_frameBuffer = px;
    
    renderFrame();
}

void RemoteControlView::updateRemoteCursor(int /*cursorId*/, int x, int y) {
    // Agent'ın güncel imleç konumunu label üzerinde göster (Ghost Cursor)
    // Şekil aktarımı iptal edildi, sadece konum gösteriyoruz.
    
    int viewW = m_cropRect.isValid() ? m_cropRect.width() : m_remoteW;
    int viewH = m_cropRect.isValid() ? m_cropRect.height() : m_remoteH;
    QSize labelSize = ui->screenLabel->size();
    QSize imgSize = QSize(viewW, viewH).scaled(labelSize, Qt::KeepAspectRatio);

    int offX = (labelSize.width()  - imgSize.width())  / 2;
    int offY = (labelSize.height() - imgSize.height()) / 2;

    int lx = x;
    int ly = y;
    if (m_cropRect.isValid()) {
        lx -= m_cropRect.x();
        ly -= m_cropRect.y();
    }

    int localX = offX + static_cast<int>(lx * static_cast<double>(imgSize.width()) / viewW);
    int localY = offY + static_cast<int>(ly * static_cast<double>(imgSize.height()) / viewH);

    m_remoteCursorLabel->move(localX, localY);

    // Basit bir "ghost" imleç ikonu oluştur (Yarım şeffaf ok veya nokta)
    if (m_remoteCursorLabel->pixmap().isNull()) {
        QPixmap curPix(24, 24);
        curPix.fill(Qt::transparent);
        QPainter p(&curPix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(255, 0, 0, 160)); // Yarı şeffaf kırmızı
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(2, 2, 14, 14); // Biraz daha belirgin bir nokta
        m_remoteCursorLabel->setPixmap(curPix);
        m_remoteCursorLabel->setFixedSize(curPix.size());
    }

    m_remoteCursorLabel->show();
    m_remoteCursorLabel->raise(); // Her zaman üstte kalsın
}

void RemoteControlView::setTitle(const QString& title) {
    // Suffix'leri temizle (Eski formatta çağrılmışsa)
    QString cleanTitle = title;
    if (cleanTitle.endsWith(tr(" — İzleme"))) {
        cleanTitle.chop(10);
    } else if (cleanTitle.endsWith(tr(" — Kontrol"))) {
        cleanTitle.chop(12);
    } else if (cleanTitle.endsWith(tr(" (İzleme)"))) {
        cleanTitle.chop(11);
    }
    m_baseTitle = cleanTitle;
    updateTitle();
}

// ---------------------------------------------------------------------------
// Mod yönetimi
// ---------------------------------------------------------------------------
void RemoteControlView::setMode(Mode mode) {
    m_mode = mode;
    if (m_mode == Mode::ViewOnly) {
        releaseAllButtons();
        releaseAllKeys();
    }
    updateModeButtons();
    updateTitle();
    // Kontrol modunda fare olayları iletilir, izleme modunda iletilmez
    // (mouseMoveEvent vb. m_mode kontrolü yapıyor)
}

void RemoteControlView::updateTitle() {
    QString modeSuffix = (m_mode == Mode::ViewOnly) ? tr(" — İzleme") : tr(" — Kontrol");
    QString fullTitle = m_baseTitle;
    if (fullTitle.isEmpty()) {
        fullTitle = m_clientId;
    }
    fullTitle += modeSuffix;

    if (ui && ui->clientLabel) {
        ui->clientLabel->setText(fullTitle);
    }
    setWindowTitle(fullTitle);
}

void RemoteControlView::updateModeButtons() {
    ui->btnViewOnly->setChecked(m_mode == Mode::ViewOnly);
    ui->btnControl ->setChecked(m_mode == Mode::Control);
    ui->btnShortcuts->setEnabled(m_mode == Mode::Control);
}

void RemoteControlView::onViewOnlyClicked() {
    setMode(Mode::ViewOnly);
}

void RemoteControlView::onControlClicked() {
    setMode(Mode::Control);
}

void RemoteControlView::onLockAgentInputClicked() {
    if (!m_session) return;
    if (ui->btnLockAgentInput->isChecked()) {
        m_session->sendInputLock(m_clientId);
        ui->btnLockAgentInput->setIcon(ThemeManager::icon(":/ui/lock.svg"));
    } else {
        m_session->sendInputUnlock(m_clientId);
        ui->btnLockAgentInput->setIcon(ThemeManager::icon(":/ui/unlock.svg"));
    }
}
void RemoteControlView::setCropRect(const QRect& rect, const QSize& nativeTotalSize) {
    m_nativeCropRect = rect;
    m_nativeTotalSize = nativeTotalSize;
    renderFrame();
}

void RemoteControlView::onFullscreenClicked() {
    if (isFullScreen()) {
        showNormal();
        // Kaydedilmiş geometriyi geri yükle
        if (!m_savedGeometry.isNull())
            setGeometry(m_savedGeometry);
        ui->btnFullscreen->setText(tr("Tam Ekran"));
        m_isFullscreen = false;
    } else {
        // Tam ekrana geçmeden önce geometriyi kaydet
        m_savedGeometry = geometry();
        showFullScreen();
        ui->btnFullscreen->setText(tr("Küçült"));
        m_isFullscreen = true;
    }
}

// ---------------------------------------------------------------------------
// Ekran güncelleme
// ---------------------------------------------------------------------------
void RemoteControlView::updateFrame(const QByteArray& jpegData) {
    if (jpegData.isEmpty()) return;
    QImage img;
    if (!img.loadFromData(jpegData, "JPEG")) return;

    // Frame geldi → overlay'i gizle ve watchdog'u yeniden başlat
    showStreamOverlay(false);
    if (m_frameWatchdog) {
        m_frameWatchdog->start(4000);
    }

    // Thumbnail ölçeğindeki boyutu kullan (tile'larla uyumlu)
    m_remoteW = img.width();
    m_remoteH = img.height();
    m_frameBuffer = QPixmap::fromImage(img);
    renderFrame();
}

void RemoteControlView::updateTile(int x, int y, int w, int h,
                                    const QByteArray& jpegData) {
    if (jpegData.isEmpty()) return;
    QImage tileImg;
    if (!tileImg.loadFromData(jpegData, "JPEG")) return;

    // Tile geldi → overlay'i gizle ve watchdog'u yeniden başlat
    showStreamOverlay(false);
    if (m_frameWatchdog) {
        m_frameWatchdog->start(4000);
    }

    // Çözünürlük değiştiyse (tam ekran genişliğinde tile geldiyse)
    if (x == 0 && y == 0 && w > 128 && (w != m_remoteW || h != m_remoteH)) {
        m_remoteW = w;
        m_remoteH = h;
        m_frameBuffer = QPixmap(w, h);
        m_frameBuffer.fill(Qt::black);
    }

    if (m_frameBuffer.isNull()) {
        m_frameBuffer = QPixmap(qMax(m_remoteW, 320), qMax(m_remoteH, 240));
        m_frameBuffer.fill(Qt::black);
    }

    QPainter painter(&m_frameBuffer);
    painter.drawImage(x, y, tileImg);
    painter.end();

    // Batch işlemi nedeniyle renderFrame'i burada çağırmıyoruz. (clientTileBatchFinished sinyali çağıracak)
}

void RemoteControlView::setStreamSize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (m_remoteW != w || m_remoteH != h) {
        m_remoteW = w;
        m_remoteH = h;
        m_frameBuffer = QPixmap(w, h);
        m_frameBuffer.fill(Qt::black);
        renderFrame();
    }
}

void RemoteControlView::refreshDisplay() {
    // Keep-alive veya normal tile batch tamamlandığında watchdog'u sıfırla
    showStreamOverlay(false);
    if (m_frameWatchdog) {
        m_frameWatchdog->start(4000);
    }
    renderFrame();
}

void RemoteControlView::renderFrame() {
    if (m_frameBuffer.isNull()) return;

    if (m_nativeCropRect.isValid() && !m_nativeTotalSize.isEmpty() && m_remoteW > 0 && m_remoteH > 0) {
        double scaleX = static_cast<double>(m_remoteW) / m_nativeTotalSize.width();
        double scaleY = static_cast<double>(m_remoteH) / m_nativeTotalSize.height();
        m_cropRect = QRect(
            m_nativeCropRect.x() * scaleX,
            m_nativeCropRect.y() * scaleY,
            m_nativeCropRect.width() * scaleX,
            m_nativeCropRect.height() * scaleY
        );
    } else if (m_nativeCropRect.isValid()) {
        m_cropRect = m_nativeCropRect;
    } else {
        m_cropRect = QRect();
    }

    // Kırpılacak görüntü parçası
    QPixmap displayPx;
    if (m_cropRect.isValid() && !m_cropRect.isNull()) {
        displayPx = m_frameBuffer.copy(m_cropRect);
    } else {
        displayPx = m_frameBuffer;
    }

    // screenLabel boyutu 0 ise widget henüz görünür değil — tüm pencereyi kullan
    QSize labelSize = ui->screenLabel->size();
    if (labelSize.width() < 10 || labelSize.height() < 10) {
        // topBar yüksekliğini çıkar
        int topH = ui->topBar ? ui->topBar->height() : 40;
        labelSize = QSize(width(), height() - topH);
    }
    if (labelSize.width() < 10 || labelSize.height() < 10) return;

    // RemoteControl ekranı için yazının okunabilirliği önemlidir. Bu nedenle SmoothTransformation kullanıyoruz.
    ui->screenLabel->setPixmap(
        displayPx.scaled(labelSize,
                         Qt::KeepAspectRatio,
                         Qt::SmoothTransformation));
}

void RemoteControlView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    renderFrame();

    // Overlay'i ortala
    if (m_streamOverlay && m_streamOverlay->isVisible()) {
        m_streamOverlay->adjustSize();
        int ox = (width() - m_streamOverlay->width()) / 2;
        int oy = (height() - m_streamOverlay->height()) / 2;
        m_streamOverlay->move(ox, oy);
    }
}

void RemoteControlView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        bool nowFullScreen = isFullScreen();
        bool nowMaximized  = isMaximized();

        if (nowFullScreen || nowMaximized) {
            // Maximize/fullscreen geçişinde normal geometriyi kaydet
            if (!m_isFullscreen && !m_savedGeometry.isNull() == false)
                m_savedGeometry = geometry();
            m_isFullscreen = nowFullScreen;
            ui->btnFullscreen->setText(
                nowFullScreen ? tr("Küçült") : tr("Tam Ekran"));
        } else {
            // Normal boyuta döndü
            if (!m_savedGeometry.isNull())
                setGeometry(m_savedGeometry);
            m_isFullscreen = false;
            ui->btnFullscreen->setText(tr("Tam Ekran"));
        }
        renderFrame();
    }
}

// ---------------------------------------------------------------------------
// Koordinat dönüşümü: widget → remote
// ---------------------------------------------------------------------------
QPoint RemoteControlView::toRemote(const QPoint& widgetPos) const {
    if (!ui || !ui->screenLabel) return QPoint(0, 0);

    QSize labelSize = ui->screenLabel->size();
    
    // Görüntülenen alanın boyutu (kırpılmış veya tam)
    int viewW = m_cropRect.isValid() ? m_cropRect.width() : m_remoteW;
    int viewH = m_cropRect.isValid() ? m_cropRect.height() : m_remoteH;

    if (viewW <= 0 || viewH <= 0 || labelSize.width() <= 0 || labelSize.height() <= 0) {
        return QPoint(0, 0);
    }

    QSize imgSize = QSize(viewW, viewH).scaled(labelSize, Qt::KeepAspectRatio);
    if (imgSize.width() <= 0 || imgSize.height() <= 0) {
        return QPoint(0, 0);
    }

    int offX = (labelSize.width()  - imgSize.width())  / 2;
    int offY = (labelSize.height() - imgSize.height()) / 2;

    QPoint labelOrigin = ui->screenLabel->mapTo(this, QPoint(0, 0));
    QPoint relToImg    = widgetPos - labelOrigin - QPoint(offX, offY);

    int rx = static_cast<int>(
        relToImg.x() * static_cast<double>(viewW) / imgSize.width());
    int ry = static_cast<int>(
        relToImg.y() * static_cast<double>(viewH) / imgSize.height());

    // Eğer kırpma varsa, ana panoramic görüntüdeki koordinatlara ekle
    if (m_cropRect.isValid()) {
        rx += m_cropRect.x();
        ry += m_cropRect.y();
    }

    return { qBound(0, rx, m_remoteW-1), qBound(0, ry, m_remoteH-1) };
}

int RemoteControlView::qtButtonToX11(Qt::MouseButton btn) {
    switch (btn) {
    case Qt::LeftButton:   return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton:  return 3;
    default:               return 1;
    }
}

// ---------------------------------------------------------------------------
// Klavye olayları
// ---------------------------------------------------------------------------
void RemoteControlView::keyPressEvent(QKeyEvent* event) {
    // Auto-repeat olaylarını engelleyelim:
    // Basılı tutulan tuş çok sayıda tekrarlanan mesaj gönderiyor.
    // Mod tuşları (Shift, Ctrl vb.) için auto-repeat gerekli değildir.
    if (event->isAutoRepeat()) { event->accept(); return; }

    if (event->key() == Qt::Key_Escape) { onStopClicked(); return; }
    if (event->key() == Qt::Key_F11)    { onFullscreenClicked(); return; }

    if (m_mode != Mode::Control || !m_session) return;

    const uint32_t keysym = qtKeyToX11Keysym(event);

    if (keysym == 0) { event->accept(); return; }

    // Basılan tuşun keysym değerini kaydet; keyReleaseEvent sırasında text boş geleceğinden
    // release olayında aynı keysym'in iletilmesi sağlanır.
    m_pressedKeysyms[event->key()] = keysym;

    okidia::KeyEvent evt;
    evt.set_keysym(keysym);
    evt.set_pressed(true);
    m_session->sendKeyEvent(m_clientId, evt);
    event->accept();
}

void RemoteControlView::keyReleaseEvent(QKeyEvent* event) {
    // Auto-repeat release'leri de yoksay
    if (event->isAutoRepeat()) { event->accept(); return; }

    if (m_mode != Mode::Control || !m_session) return;

    // Basılmış olan tuşun keysym'ini haritadan al (eğer haritada yoksa fallback olarak hesapla)
    uint32_t keysym = m_pressedKeysyms.take(event->key());
    if (keysym == 0) {
        keysym = qtKeyToX11Keysym(event);
    }

    if (keysym == 0) { event->accept(); return; }

    okidia::KeyEvent evt;
    evt.set_keysym(keysym);
    evt.set_pressed(false);
    m_session->sendKeyEvent(m_clientId, evt);
    event->accept();
}

bool RemoteControlView::focusNextPrevChild(bool next) {
    // Kontrol modundayken Tab veya Shift+Tab tuşlarının arayüzdeki düğmeler arasında odak (focus)
    // geçişi yapmasını engelliyoruz. Bu tuşlar keyPressEvent/keyReleaseEvent üzerinden istemciye iletilecek.
    if (m_mode == Mode::Control) {
        return false;
    }
    return QWidget::focusNextPrevChild(next);
}

// ---------------------------------------------------------------------------
// Fare olayları — sadece Kontrol modunda iletilir
// ---------------------------------------------------------------------------
void RemoteControlView::mouseMoveEvent(QMouseEvent* event) {
    if (m_mode != Mode::Control || !m_session) return;

    // Throttle: 20ms (max 50 events/sec)
    if (m_mouseThrottleTimer.isValid() && m_mouseThrottleTimer.elapsed() < 20) {
        return;
    }
    m_mouseThrottleTimer.restart();

    // Sağ tuş basılıyken hareketleri engelle (agent tarafındaki sağ tık menüsünün
    // hover ile otomatik seçim yapmasını / ghost click'leri önlemek amacıyla).
    if (m_pressedButtons & Qt::RightButton) {
        return;
    }

    // Sağ tık bırakıldıktan sonra kısa bir süre fare hareketlerini engelle.
    // Agent tarafında açılan menünün kapanması/stabilize olması için bekleme.
    if (m_rightClickCooldown.isValid() && m_rightClickCooldown.elapsed() < 350) {
        return;
    }

    // Sürükleme yapılmıyorsa (sol veya orta buton basılı değilse) ve imleç remote resim alanının
    // dışındaysa event'i yok say. (Sürükleme yapılıyorsa ekran dışına kadar taşınabilsin).
    bool isDragging = (m_pressedButtons & Qt::LeftButton) || (m_pressedButtons & Qt::MiddleButton);
    if (!isDragging && !imageRect().contains(event->pos())) return;

    QPoint remote = toRemote(event->pos());
    okidia::MouseEvent evt;
    evt.set_x(remote.x()); evt.set_y(remote.y());
    evt.set_is_move(true);
    m_session->sendMouseEvent(m_clientId, evt);
}

void RemoteControlView::mousePressEvent(QMouseEvent* event) {
    if (m_mode != Mode::Control || !m_session) return;

    // Resim alanı dışındaki tıklamaları (örneğin toolbar boşlukları) engelle
    if (!imageRect().contains(event->pos())) return;

    m_pressedButtons |= event->button();

    QPoint remote = toRemote(event->pos());
    okidia::MouseEvent evt;
    evt.set_x(remote.x()); evt.set_y(remote.y());
    evt.set_button(qtButtonToX11(event->button()));
    evt.set_pressed(true); evt.set_is_move(false);
    m_session->sendMouseEvent(m_clientId, evt);
}

void RemoteControlView::mouseReleaseEvent(QMouseEvent* event) {
    if (m_mode != Mode::Control || !m_session) return;

    // Sadece gerçekten bastığımız butonu bırak
    bool wasPressed = m_pressedButtons & event->button();
    m_pressedButtons &= ~event->button();

    if (!wasPressed) return;

    // Sağ tuş bırakıldığında cooldown başlat
    // Menü açıldıktan sonra fare hareketlerinin menüye girmesini
    // kısa süre geciktirmek için.
    if (event->button() == Qt::RightButton) {
        m_rightClickCooldown.restart();
    }

    QPoint remote = toRemote(event->pos());
    okidia::MouseEvent evt;
    evt.set_x(remote.x()); evt.set_y(remote.y());
    evt.set_button(qtButtonToX11(event->button()));
    evt.set_pressed(false); evt.set_is_move(false);
    m_session->sendMouseEvent(m_clientId, evt);
}

void RemoteControlView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_mode != Mode::Control || !m_session) return;
    // Çift tıklamayı normal bir basma olayı (press) olarak istemciye ilet
    mousePressEvent(event);
}

void RemoteControlView::wheelEvent(QWheelEvent* event) {
    if (m_mode != Mode::Control || !m_session) return;

    // Sadece resim alanı içindeyse tekerleği ilet
    if (!imageRect().contains(event->position().toPoint())) return;

    int delta = event->angleDelta().y() > 0 ? 1 : -1;
    okidia::MouseEvent evt;
    evt.set_is_scroll(true); evt.set_delta(delta);
    m_session->sendMouseEvent(m_clientId, evt);
}

// ---------------------------------------------------------------------------
// Ekran seçimi
// ---------------------------------------------------------------------------
void RemoteControlView::updateMonitorList(const okidia::ScreenInfoResponse& info) {
    if (!ui) return;
    
    // Geçici olarak sinyali engelle (temizlerken tetiklenmesin)
    ui->comboSource->blockSignals(true);
    ui->comboSource->clear();

    // Tek ekran varsa sadece "Tüm Ekranlar" eklensin.
    if (info.monitors_size() <= 1) {
        ui->comboSource->addItem(tr("Tüm Ekranlar"), -1);
    } else {
        ui->comboSource->addItem(tr("Tüm Ekranlar"), -1);

        // Panoramik boyutları hesapla
        int nativeW = 0;
        int nativeH = 0;
        for (int i = 0; i < info.monitors_size(); ++i) {
            const auto& m = info.monitors(i);
            if (m.x() + static_cast<int>(m.width()) > nativeW) nativeW = m.x() + m.width();
            if (m.y() + static_cast<int>(m.height()) > nativeH) nativeH = m.y() + m.height();
        }
        m_nativeTotalSize = QSize(nativeW, nativeH);

        for (int i = 0; i < info.monitors_size(); ++i) {
            const auto& m = info.monitors(i);
            QString name = QString::fromStdString(m.name());
            if (name.isEmpty()) name = QString("Ekran %1").arg(i + 1);
            
            // Ekranın kırpma koordinatlarını itemData olarak sakla
            QRect rect(m.x(), m.y(), m.width(), m.height());
            ui->comboSource->addItem(ThemeManager::icon(":/ui/monitor.svg"), name, QVariant::fromValue(rect));
        }
    }
    
    if (info.windows_size() > 0) {
        ui->comboSource->insertSeparator(ui->comboSource->count());
        for (int i = 0; i < info.windows_size(); ++i) {
            const auto& w = info.windows(i);
            QString title = QString::fromStdString(w.title());
            if (title.isEmpty()) continue;
            if (title.length() > 40) title = title.left(37) + "...";
            ui->comboSource->addItem(ThemeManager::icon(":/ui/smartboard.svg"), title, QVariant::fromValue(w.wid())); // window id
        }
    }
    
    // Geçerli bir kırpma alanı varsa, ilgili monitörü seçili hale getir
    int currentIndex = 0;
    if (m_nativeCropRect.isValid()) {
        for (int i = 0; i < ui->comboSource->count(); ++i) {
            if (ui->comboSource->itemData(i).typeId() == QMetaType::QRect) {
                if (ui->comboSource->itemData(i).toRect() == m_nativeCropRect) {
                    currentIndex = i;
                    break;
                }
            }
        }
    }
    ui->comboSource->setCurrentIndex(currentIndex);
    ui->comboSource->blockSignals(false);
}

void RemoteControlView::onSourceChanged(int index) {
    if (!m_session || index < 0) return;

    QVariant data = ui->comboSource->itemData(index);
    if (data.typeId() == QMetaType::Int) {
        int val = data.toInt();
        if (val == -1) {
            m_session->sendScreenSelectAll(m_clientId);
            m_nativeCropRect = QRect(); // Kırpmayı kaldır
            renderFrame();
        }
    } else if (data.typeId() == QMetaType::QRect) {
        // Belirli bir monitör seçildi: İstemciden her zaman tüm ekranları iste ve lokal olarak kırp.
        // Bu sayede diğer açık remote control pencerelerinin görüntüsü bozulmaz.
        m_session->sendScreenSelectAll(m_clientId);
        m_nativeCropRect = data.toRect();
        renderFrame();
    } else if (data.typeId() == QMetaType::ULongLong) {
        quint64 wid = data.toULongLong();
        m_session->sendScreenSelectWindow(m_clientId, wid);
        m_nativeCropRect = QRect(); // Kırpmayı kaldır
        renderFrame();
    }
}

void RemoteControlView::onQualityChanged(int index) {
    if (!m_session || !m_cbQuality) return;

    int quality = 80;
    int maxWidth = 1920;

    int data = m_cbQuality->itemData(index).toInt();
    if (data == 1) {        // Orijinal (1080p, %85 jpeg)
        maxWidth = 1920;
        quality = 85;
    } else if (data == 2) { // Yüksek (1080p, %75 jpeg)
        maxWidth = 1920;
        quality = 75;
    } else if (data == 3) { // Orta (720p, %70 jpeg)
        maxWidth = 1280;
        quality = 70;
    } else if (data == 4) { // Düşük (480p, %60 jpeg)
        maxWidth = 854;
        quality = 60;
    }

    QSettings settings("okidia", "master");
    settings.setValue("RemoteControl/QualityIndex", index);

    Logger::info(QObject::tr("RemoteControlView: Kalite değiştirildi -> Kalite: %1, MaxWidth: %2").arg(quality).arg(maxWidth));
    
    // 50ms interval = 20 fps
    m_session->sendScreenConfig(m_clientId, 50, quality, maxWidth);
}

// ---------------------------------------------------------------------------
// Stream overlay
// ---------------------------------------------------------------------------
void RemoteControlView::showStreamOverlay(bool show) {
    if (!m_streamOverlay) return;
    if (show == m_streamOverlay->isVisible()) return;  // Gereksiz yeniden render'ı engelle

    if (show) {
        m_streamConnected = false;
        m_streamOverlay->adjustSize();
        int ox = (width()  - m_streamOverlay->width())  / 2;
        int oy = (height() - m_streamOverlay->height()) / 2;
        m_streamOverlay->move(ox, oy);
        m_streamOverlay->show();
        m_streamOverlay->raise();
    } else {
        m_streamConnected = true;
        m_streamOverlay->hide();
    }
}

// ---------------------------------------------------------------------------
// Kapatma
// ---------------------------------------------------------------------------
void RemoteControlView::onStopClicked() {
    close();
}

void RemoteControlView::closeEvent(QCloseEvent* event) {
    releaseAllButtons();
    releaseAllKeys();
    emit stopped(m_clientId);
    if (m_fileManagerWindow) {
        m_fileManagerWindow->close();
        m_fileManagerWindow->deleteLater();
        m_fileManagerWindow = nullptr;
    }
    event->accept();
}

QRect RemoteControlView::imageRect() const {
    if (!ui || !ui->screenLabel) return QRect();

    QSize labelSize = ui->screenLabel->size();
    
    // Görüntülenen alanın boyutu (kırpılmış veya tam)
    int viewW = m_cropRect.isValid() ? m_cropRect.width() : m_remoteW;
    int viewH = m_cropRect.isValid() ? m_cropRect.height() : m_remoteH;

    QSize imgSize = QSize(viewW, viewH).scaled(labelSize, Qt::KeepAspectRatio);

    int offX = (labelSize.width()  - imgSize.width())  / 2;
    int offY = (labelSize.height() - imgSize.height()) / 2;

    QPoint labelOrigin = ui->screenLabel->mapTo(this, QPoint(0, 0));
    return QRect(labelOrigin.x() + offX, labelOrigin.y() + offY, imgSize.width(), imgSize.height());
}

void RemoteControlView::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    releaseAllButtons();
    releaseAllKeys();
}

void RemoteControlView::releaseAllButtons() {
    if (!m_session || m_mode != Mode::Control) return;

    if (m_pressedButtons & Qt::LeftButton) {
        sendMouseRelease(Qt::LeftButton);
    }
    if (m_pressedButtons & Qt::RightButton) {
        sendMouseRelease(Qt::RightButton);
    }
    if (m_pressedButtons & Qt::MiddleButton) {
        sendMouseRelease(Qt::MiddleButton);
    }
    m_pressedButtons = Qt::NoButton;
}

void RemoteControlView::releaseAllKeys() {
    if (!m_session || m_mode != Mode::Control) {
        m_pressedKeysyms.clear();
        return;
    }

    for (auto it = m_pressedKeysyms.begin(); it != m_pressedKeysyms.end(); ++it) {
        okidia::KeyEvent evt;
        evt.set_keysym(it.value());
        evt.set_pressed(false);
        m_session->sendKeyEvent(m_clientId, evt);
    }
    m_pressedKeysyms.clear();
}

void RemoteControlView::setupShortcutsMenu() {
    ui->btnShortcuts->setIcon(ThemeManager::icon(":/ui/key.svg"));
    QMenu* menu = new QMenu(ui->btnShortcuts);

    auto rebuildMenu = [this, menu]() {
        menu->clear();

        ClientState state;
        if (m_session) {
            state = m_session->clientState(m_clientId);
        }
        const QString osStr = (state.osTag + " " + state.osPrettyName).toLower();
        const bool isWin = osStr.contains("windows") || osStr.contains("win");
        const bool isLin = !isWin && (!state.osTag.isEmpty() || !state.osPrettyName.isEmpty() ||
                                      osStr.contains("linux") || osStr.contains("pardus") ||
                                      osStr.contains("ubuntu") || osStr.contains("debian"));

        if (isWin) {
            // ── Windows İstemci Kısayolları ────────────────────────────
            QAction* actCad = menu->addAction(tr("Ctrl + Alt + Del (Güvenlik / Oturum)"));
            connect(actCad, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFFE9, 0xFFFF });
            });

            QAction* actSuper = menu->addAction(tr("Windows Tuşu (Başlat)"));
            connect(actSuper, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB });
            });

            QAction* actCtrlEsc = menu->addAction(tr("Ctrl + Esc (Başlat Menüsü)"));
            connect(actCtrlEsc, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFF1B });
            });

            menu->addSeparator();

            QAction* actWinL = menu->addAction(tr("Win + L (Ekranı Kilitle)"));
            connect(actWinL, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB, 0x006C });
            });

            QAction* actWinD = menu->addAction(tr("Win + D (Masaüstünü Göster / Gizle)"));
            connect(actWinD, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB, 0x0064 });
            });

            QAction* actWinR = menu->addAction(tr("Win + R (Çalıştır Diyalogu)"));
            connect(actWinR, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB, 0x0072 });
            });

            QAction* actWinE = menu->addAction(tr("Win + E (Dosya Gezgini)"));
            connect(actWinE, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB, 0x0065 });
            });

            QAction* actWinX = menu->addAction(tr("Win + X (Hızlı Bağlantı / Güç Menüsü)"));
            connect(actWinX, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB, 0x0078 });
            });

            QAction* actTaskMgr = menu->addAction(tr("Ctrl + Shift + Esc (Görev Yöneticisi)"));
            connect(actTaskMgr, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFFE1, 0xFF1B });
            });

            menu->addSeparator();

            QAction* actAltTab = menu->addAction(tr("Alt + Tab (Pencereler Arasında Geçiş)"));
            connect(actAltTab, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFF09 });
            });

            QAction* actAltF4 = menu->addAction(tr("Alt + F4 (Aktif Pencereyi Kapat)"));
            connect(actAltF4, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFFC1 });
            });

        } else if (isLin) {
            // ── Linux İstemci Kısayolları ──────────────────────────────
            QAction* actSuper = menu->addAction(tr("Süper Tuşu (Etkinlikler / Menü)"));
            connect(actSuper, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB });
            });

            QAction* actTerminal = menu->addAction(tr("Terminal Aç (Ctrl + Alt + T)"));
            connect(actTerminal, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFFE9, 0x0074 });
            });

            QAction* actCad = menu->addAction(tr("Ctrl + Alt + Del (Oturumu Kapat / Sistem)"));
            connect(actCad, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFFE9, 0xFFFF });
            });

            menu->addSeparator();

            QAction* actSuperD = menu->addAction(tr("Süper + D (Masaüstünü Göster)"));
            connect(actSuperD, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB, 0x0064 });
            });

            QAction* actAltF2 = menu->addAction(tr("Alt + F2 (Komut Çalıştır)"));
            connect(actAltF2, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFFBF });
            });

            QAction* actAltTab = menu->addAction(tr("Alt + Tab (Pencereler Arasında Geçiş)"));
            connect(actAltTab, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFF09 });
            });

            QAction* actAltF4 = menu->addAction(tr("Alt + F4 (Pencereyi Kapat)"));
            connect(actAltF4, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFFC1 });
            });

            menu->addSeparator();

            QMenu* ttyMenu = menu->addMenu(tr("Sanal Konsollar (TTY)"));
            for (int i = 1; i <= 6; ++i) {
                uint32_t fKey = 0xFFBD + i;
                QAction* actTty = ttyMenu->addAction(QString(tr("Sanal Konsol TTY %1 (Ctrl + Alt + F%1)")).arg(i));
                connect(actTty, &QAction::triggered, this, [this, fKey]() {
                    sendKeyCombination({ 0xFFE3, 0xFFE9, fKey });
                });
            }
            QAction* actTty7 = ttyMenu->addAction(tr("Grafik Oturumuna Dön (Ctrl + Alt + F7)"));
            connect(actTty7, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFFE9, 0xFFC4 });
            });

        } else {
            // ── Belirsiz / Karışık İstemci Durumu ──────────────────────
            QAction* actCad = menu->addAction(tr("Ctrl + Alt + Del"));
            connect(actCad, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE3, 0xFFE9, 0xFFFF });
            });

            QAction* actSuper = menu->addAction(tr("Başlat / Süper Tuşu (Super)"));
            connect(actSuper, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFEB });
            });

            QAction* actAltTab = menu->addAction(tr("Alt + Tab (Pencereler Arasında Geçiş)"));
            connect(actAltTab, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFF09 });
            });

            QAction* actAltF4 = menu->addAction(tr("Alt + F4 (Pencereyi Kapat)"));
            connect(actAltF4, &QAction::triggered, this, [this]() {
                sendKeyCombination({ 0xFFE9, 0xFFC1 });
            });

            menu->addSeparator();

            QMenu* winSub = menu->addMenu(tr("Windows Kısayolları"));
            QAction* wCtrlEsc = winSub->addAction(tr("Ctrl + Esc (Başlat Menüsü)"));
            connect(wCtrlEsc, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFE3, 0xFF1B }); });
            QAction* wWinL = winSub->addAction(tr("Win + L (Ekranı Kilitle)"));
            connect(wWinL, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFEB, 0x006C }); });
            QAction* wWinD = winSub->addAction(tr("Win + D (Masaüstünü Göster)"));
            connect(wWinD, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFEB, 0x0064 }); });
            QAction* wWinR = winSub->addAction(tr("Win + R (Çalıştır)"));
            connect(wWinR, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFEB, 0x0072 }); });
            QAction* wWinE = winSub->addAction(tr("Win + E (Dosya Gezgini)"));
            connect(wWinE, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFEB, 0x0065 }); });
            QAction* wTaskMgr = winSub->addAction(tr("Ctrl + Shift + Esc (Görev Yöneticisi)"));
            connect(wTaskMgr, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFE3, 0xFFE1, 0xFF1B }); });

            QMenu* linSub = menu->addMenu(tr("Linux Kısayolları"));
            QAction* lTerm = linSub->addAction(tr("Terminal Aç (Ctrl + Alt + T)"));
            connect(lTerm, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFE3, 0xFFE9, 0x0074 }); });
            QAction* lAltF2 = linSub->addAction(tr("Alt + F2 (Komut Çalıştır)"));
            connect(lAltF2, &QAction::triggered, this, [this]() { sendKeyCombination({ 0xFFE9, 0xFFBF }); });
            QMenu* ttySub = linSub->addMenu(tr("Sanal Konsollar (TTY)"));
            for (int i = 1; i <= 6; ++i) {
                uint32_t fKey = 0xFFBD + i;
                QAction* actTty = ttySub->addAction(QString(tr("TTY %1 (Ctrl + Alt + F%1)")).arg(i));
                connect(actTty, &QAction::triggered, this, [this, fKey]() {
                    sendKeyCombination({ 0xFFE3, 0xFFE9, fKey });
                });
            }
        }
    };

    connect(menu, &QMenu::aboutToShow, this, rebuildMenu);
    rebuildMenu();
    ui->btnShortcuts->setMenu(menu);
}

void RemoteControlView::sendKeyCombination(const QList<uint32_t>& keysyms) {
    if (!m_session || m_mode != Mode::Control || keysyms.isEmpty()) return;

    // 1. Sırayla tüm tuşları bas
    for (uint32_t keysym : keysyms) {
        okidia::KeyEvent evt;
        evt.set_keysym(keysym);
        evt.set_pressed(true);
        m_session->sendKeyEvent(m_clientId, evt);
    }

    // 2. 60 ms bekle (işletim sisteminin çoklu tuş kombinasyonunu algılaması için)
    QTimer::singleShot(60, this, [this, keysyms]() {
        if (!m_session) return;
        // Ters sırada tüm tuşları bırak
        for (int i = keysyms.size() - 1; i >= 0; --i) {
            okidia::KeyEvent evt;
            evt.set_keysym(keysyms[i]);
            evt.set_pressed(false);
            m_session->sendKeyEvent(m_clientId, evt);
        }
    });
}

void RemoteControlView::sendMouseRelease(Qt::MouseButton btn) {
    QPoint remote = toRemote(mapFromGlobal(QCursor::pos()));
    okidia::MouseEvent evt;
    evt.set_x(remote.x()); evt.set_y(remote.y());
    evt.set_button(qtButtonToX11(btn));
    evt.set_pressed(false); evt.set_is_move(false);
    m_session->sendMouseEvent(m_clientId, evt);
}

void RemoteControlView::onSshClicked() {
    auto st = m_session->clientState(m_clientId);
    if (st.ip.isEmpty()) return;

    QString ip = st.ip;
    QString hostName = st.hostname.isEmpty() ? m_clientId : st.hostname;
    
    auto fetchConfig = [this, ip, st]() -> SshClient::Config {
        SshClient::Config cfg;
        cfg.host = ip;
        cfg.port = 22;
        cfg.connectTimeoutMs = 4000;
        cfg.hostKeyCheck = false;
        
        QString user, pass;
        bool isKey = false, found = false;
        
        const auto brokerCreds = m_session->sshCredentials();
        QString machineId = st.machineId;
        QString classId = m_session->clientGroup(machineId);

        for (const auto& c : brokerCreds) {
            if (c.targetId == ip && c.targetType == "agent") {
                user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
            }
        }
        if (!found && !machineId.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if (c.targetId == machineId && c.targetType == "agent") {
                    user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
                }
            }
        }
        if (!found && !classId.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if (c.targetId == classId && c.targetType == "class") {
                    user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
                }
            }
        }

        if (found) {
            cfg.username = user;
            if (isKey) {
                cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                cfg.privateKeyPath = pass;
            } else {
                cfg.authMethod = SshClient::AuthMethod::Password;
                cfg.password = pass;
            }
        } else {
            cfg.username = "root";
            cfg.authMethod = SshClient::AuthMethod::Password;
        }
        return cfg;
    };

    SshClient::Config cfg = fetchConfig();

    if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
        SshCredentialDialog dlg(m_session, this);
        dlg.exec();
        
        cfg = fetchConfig();
        if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
            QMessageBox::warning(this, tr("Bağlantı Başarısız"), tr("Geçerli bir SSH bilgisi girilmediği için bağlanılamıyor."));
            return;
        }
    }

    auto* sessionWidget = new SshDeviceSessionWidget(m_session, ip, hostName, cfg);
    sessionWidget->setAttribute(Qt::WA_DeleteOnClose);
    sessionWidget->setWindowTitle(QString(tr("%1 (%2) — SSH Yönetimi (Terminal & Dosyalar)")).arg(hostName, ip));
    sessionWidget->resize(920, 620);
    sessionWidget->show();
    sessionWidget->raise();
    sessionWidget->activateWindow();
}
