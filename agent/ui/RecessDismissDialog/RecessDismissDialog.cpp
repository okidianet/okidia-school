// agent/ui/RecessDismissDialog/RecessDismissDialog.cpp
#include "RecessDismissDialog.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/OkidiaConfig.h"
#include "../../system/SystemInfo.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QScreen>
#include <QProcess>
#include <QSettings>

RecessDismissDialog& RecessDismissDialog::instance() {
    static RecessDismissDialog s_instance;
    return s_instance;
}

RecessDismissDialog::RecessDismissDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &RecessDismissDialog::onTick);
}

void RecessDismissDialog::setupUi() {
    setObjectName("RecessDismissDialog");
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFixedSize(440, 200);

    setStyleSheet(
        "QDialog#RecessDismissDialog {"
        "  background-color: #161b22;"
        "  border: 2px solid #30363d;"
        "  border-radius: 14px;"
        "}"
        "QLabel { color: #ffffff; font-family: sans-serif; }"
        "QProgressBar {"
        "  border: 1px solid #30363d;"
        "  border-radius: 6px;"
        "  background-color: #0d1117;"
        "  height: 10px;"
        "  text-align: center;"
        "}"
        "QProgressBar::chunk {"
        "  background-color: #2ea043;"
        "  border-radius: 5px;"
        "}"
    );

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 16, 20, 16);
    rootLayout->setSpacing(12);

    // Başlık
    m_lblTitle = new QLabel(tr("Ders Bitti, İyi Teneffüsler!"), this);
    m_lblTitle->setStyleSheet("font-size: 17px; font-weight: bold; color: #58a6ff;");
    rootLayout->addWidget(m_lblTitle);

    // Açıklama & Geri sayım metni
    m_lblMessage = new QLabel(this);
    m_lblMessage->setStyleSheet("font-size: 13px; color: #8b949e;");
    m_lblMessage->setWordWrap(true);
    rootLayout->addWidget(m_lblMessage);

    // Görsel Geri Sayım Çubuğu
    m_progressBar = new QProgressBar(this);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(8);
    rootLayout->addWidget(m_progressBar);

    // Butonlar (Dokunmatik ekran için geniş ve ferah)
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(12);

    m_btnLockNow = new QPushButton(tr("Şimdi Kilitle"), this);
    m_btnLockNow->setIcon(ThemeManager::icon(":/ui/lock.svg"));
    m_btnLockNow->setMinimumHeight(44);
    m_btnLockNow->setStyleSheet(
        "QPushButton {"
        "  background-color: #21262d;"
        "  border: 1px solid #30363d;"
        "  border-radius: 8px;"
        "  color: #f85149;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "  padding: 8px 14px;"
        "}"
        "QPushButton:hover { background-color: #b62324; color: #ffffff; border-color: #f85149; }"
        "QPushButton:pressed { background-color: #da3633; }"
    );
    connect(m_btnLockNow, &QPushButton::clicked, this, &RecessDismissDialog::onLockNow);
    btnLayout->addWidget(m_btnLockNow);

    m_btnExtend = new QPushButton(tr("+1 Dk Uzat"), this);
    m_btnExtend->setMinimumHeight(44);
    m_btnExtend->setStyleSheet(
        "QPushButton {"
        "  background-color: #238636;"
        "  border: 1px solid #2ea043;"
        "  border-radius: 8px;"
        "  color: #ffffff;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "  padding: 8px 14px;"
        "}"
        "QPushButton:hover { background-color: #2ea043; border-color: #3fb950; }"
        "QPushButton:pressed { background-color: #238636; }"
        "QPushButton:disabled { background-color: #21262d; color: #484f58; border-color: #30363d; }"
    );
    connect(m_btnExtend, &QPushButton::clicked, this, &RecessDismissDialog::onExtendOneMinute);
    btnLayout->addWidget(m_btnExtend);

    m_btnEjectUsb = new QPushButton(tr("⏏️ USB'yi Çıkar"), this);
    m_btnEjectUsb->setIcon(ThemeManager::icon(":/ui/root_drive.svg"));
    m_btnEjectUsb->setMinimumHeight(44);
    m_btnEjectUsb->setStyleSheet(
        "QPushButton {"
        "  background-color: #21262d;"
        "  border: 1px solid #30363d;"
        "  border-radius: 8px;"
        "  color: #58a6ff;"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "  padding: 8px 12px;"
        "}"
        "QPushButton:hover { background-color: #388bfd; color: #ffffff; border-color: #58a6ff; }"
        "QPushButton:pressed { background-color: #1f6feb; }"
        "QPushButton:disabled { background-color: #21262d; color: #2ea043; border-color: #2ea043; }"
    );
    m_btnEjectUsb->setVisible(false);
    connect(m_btnEjectUsb, &QPushButton::clicked, this, &RecessDismissDialog::onEjectUsb);
    btnLayout->addWidget(m_btnEjectUsb);

    rootLayout->addLayout(btnLayout);
}

void RecessDismissDialog::positionToBottomRight() {
    QScreen* screen = QApplication::primaryScreen();
    if (!screen) return;

    QRect availGeom = screen->availableGeometry();
    int x = availGeom.right() - width() - 24;
    int y = availGeom.bottom() - height() - 24;
    move(x, y);
}

bool RecessDismissDialog::isRunning() const {
    return isVisible() && m_timer && m_timer->isActive();
}

void RecessDismissDialog::showGracePeriod(int seconds, const QString& message, bool isLogout) {
    if (isRunning()) {
        Logger::info(QObject::tr("RecessDismissDialog: Nezaket penceresi zaten aktif (%1 sn kaldı), mükerrer çağrı yoksayıldı.")
                     .arg(m_remainingSec));
        return;
    }

    m_totalSec     = seconds > 0 ? seconds : 30;
    m_remainingSec = m_totalSec;
    m_extended     = false;
    m_isLogout     = isLogout;

    if (m_isLogout) {
        m_btnExtend->setVisible(false);
        m_btnLockNow->setText(tr("Şimdi Kapat"));
        m_btnLockNow->setIcon(ThemeManager::icon(":/ui/power.svg"));
        if (!message.isEmpty()) {
            m_lblTitle->setText(message);
        } else {
            m_lblTitle->setText(tr("Ders Bitti, Oturum Kapatılıyor"));
        }
    } else {
        m_btnExtend->setVisible(true);
        m_btnExtend->setEnabled(true);
        m_btnExtend->setText(tr("+1 Dk Uzat"));
        m_btnLockNow->setText(tr("Şimdi Kilitle"));
        m_btnLockNow->setIcon(ThemeManager::icon(":/ui/lock.svg"));
        if (!message.isEmpty()) {
            m_lblTitle->setText(message);
        } else {
            m_lblTitle->setText(tr("Ders Bitti, İyi Teneffüsler!"));
        }
    }

    bool usbProtection = QSettings("okidia", "agent").value("policy/usb_protection_enabled", false).toBool()
                      || OkidiaConfig::isUsbProtectionEnabled();
    if (usbProtection) {
        QProcess::startDetached("sync", {});
        auto drives = SystemInfo::mountedUsbDrives();
        if (!drives.isEmpty()) {
            m_btnEjectUsb->setText(tr("⏏️ USB'yi Çıkar"));
            m_btnEjectUsb->setEnabled(true);
            m_btnEjectUsb->setVisible(true);
        } else {
            m_btnEjectUsb->setVisible(false);
        }
    } else {
        m_btnEjectUsb->setVisible(false);
    }

    updateUiTime();
    positionToBottomRight();
    show();
    raise();

    m_timer->start();
    Logger::info(QObject::tr("RecessDismissDialog: Teneffüs nezaket penceresi açıldı (%1 sn, Oturum Kapatma: %2)")
                 .arg(m_totalSec).arg(m_isLogout ? tr("Evet") : tr("Hayır")));
}

void RecessDismissDialog::dismiss() {
    m_timer->stop();
    hide();
}

void RecessDismissDialog::updateUiTime() {
    if (m_isLogout) {
        m_lblMessage->setText(tr("Oturumunuz %1 saniye içinde kapatılacaktır. Lütfen çalışmalarınızı kaydediniz.").arg(m_remainingSec));
    } else {
        m_lblMessage->setText(tr("Ekran %1 saniye sonra kilitlenecektir.").arg(m_remainingSec));
    }
    m_progressBar->setMaximum(m_totalSec);
    m_progressBar->setValue(m_remainingSec);

    // Kalan süreye göre çubuk rengi (Son 10 saniye uyarıcı amber/kırmızı)
    if (m_remainingSec <= 10) {
        m_progressBar->setStyleSheet("QProgressBar::chunk { background-color: #da3633; border-radius: 5px; }");
    } else {
        m_progressBar->setStyleSheet("QProgressBar::chunk { background-color: #2ea043; border-radius: 5px; }");
    }
}

void RecessDismissDialog::onTick() {
    m_remainingSec--;
    if (m_remainingSec <= 0) {
        dismiss();
        if (m_isLogout) {
            Logger::info(QObject::tr("RecessDismissDialog: Nezaket süresi doldu, oturum kapatılıyor."));
            emit logoutNowRequested();
        } else {
            Logger::info(QObject::tr("RecessDismissDialog: Nezaket süresi doldu, ekran kilitleniyor."));
            emit lockNowRequested();
        }
        return;
    }
    updateUiTime();
}

void RecessDismissDialog::onLockNow() {
    dismiss();
    if (m_isLogout) {
        Logger::info(QObject::tr("RecessDismissDialog: Kullanıcı 'Şimdi Kapat' butonuna bastı."));
        emit logoutNowRequested();
    } else {
        Logger::info(QObject::tr("RecessDismissDialog: Öğretmen 'Şimdi Kilitle' butonuna bastı."));
        emit lockNowRequested();
    }
}

void RecessDismissDialog::onExtendOneMinute() {
    m_remainingSec = 60;
    m_totalSec     = 60;
    m_extended     = true;
    m_btnExtend->setEnabled(false);
    m_btnExtend->setText(tr("Uzatıldı ✓"));
    updateUiTime();
    Logger::info(QObject::tr("RecessDismissDialog: Öğretmen dersi 1 dakika uzattı."));
}

void RecessDismissDialog::onEjectUsb() {
    bool ok = SystemInfo::safeEjectUsbDrives();
    if (ok) {
        m_btnEjectUsb->setText(tr("Çıkarıldı ✓"));
        m_btnEjectUsb->setEnabled(false);
        Logger::info(QObject::tr("RecessDismissDialog: Flaş bellek güvenle ayrıldı."));
    } else {
        m_btnEjectUsb->setText(tr("Meşgul"));
    }
}
