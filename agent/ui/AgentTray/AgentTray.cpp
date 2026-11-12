// agent/ui/AgentTray/AgentTray.cpp
#include "AgentTray.h"
#include "../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"

#include <QMenu>
#include <QAction>
#include <QPixmap>
#include <QPainter>
#include <QDateTime>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include "../../../common/ui/ThemeManager.h"

// ── İkon yardımcıları ────────────────────────────────────────────────────────
QIcon AgentTray::loadIcon(const QString& baseName, const QColor& fallbackColor)
{
    // 1. Gömülü Qt Resource kontrolü (Multi-resolution: 16px, 22px, 32px)
    QIcon resIcon;
    resIcon.addFile(QString(":/icons/agent/%1-16.png").arg(baseName));
    resIcon.addFile(QString(":/icons/agent/%1-22.png").arg(baseName));
    resIcon.addFile(QString(":/icons/agent/%1-32.png").arg(baseName));

    if (!resIcon.isNull() && !resIcon.availableSizes().isEmpty()) {
        return resIcon;
    }

    // 2. Tekil dosya yolları kontrolü
    if (QFile::exists(QString(":/icons/agent/%1-22.png").arg(baseName))) {
        return QIcon(QString(":/icons/agent/%1-22.png").arg(baseName));
    }
    if (QFile::exists(QString(":/icons/agent/%1.png").arg(baseName))) {
        return QIcon(QString(":/icons/agent/%1.png").arg(baseName));
    }

    // 3. Linux XDG sistem teması kontrolü
    if (QIcon::hasThemeIcon(baseName)) {
        return QIcon::fromTheme(baseName);
    }
    if (baseName == "okidia-agent-connected" && QIcon::hasThemeIcon("okidia-agent")) {
        return QIcon::fromTheme("okidia-agent");
    }

    // 4. Fallback: Çizimli renkli daire
    return makeFallbackIcon(fallbackColor);
}

QIcon AgentTray::makeFallbackIcon(const QColor& color)
{
    QPixmap px(22, 22);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawEllipse(2, 2, 18, 18);
    p.end();
    return QIcon(px);
}

// ── Constructor ──────────────────────────────────────────────────────────────

AgentTray::AgentTray(QObject* parent) : QObject(parent)
{
    // Gömülü resource'lardan çoklu çözünürlük yükle (Windows & Linux tam uyumlu)
    m_iconConnected  = loadIcon("okidia-agent-connected", QColor("#2ecc71"));

    QIcon lockIco(":/ui/lock.svg");
    m_iconLocked     = !lockIco.isNull() ? lockIco : makeFallbackIcon(QColor("#f39c12"));

    QIcon handIco(":/ui/hand.svg");
    m_iconHandRaised = !handIco.isNull() ? handIco : makeFallbackIcon(QColor("#9b59b6"));

    // Arama animasyonu — okidia-agent-search1/2/3
    m_searchIcons << loadIcon("okidia-agent-search1", QColor("#c0392b"));
    m_searchIcons << loadIcon("okidia-agent-search2", QColor("#e74c3c"));
    m_searchIcons << loadIcon("okidia-agent-search3", QColor("#ff6b6b"));

    // ── Animasyon timer ───────────────────────────────────────────
    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(1000);
    connect(m_animTimer, &QTimer::timeout, this, &AgentTray::onAnimationTick);

    // ── Menü ─────────────────────────────────────────────────────
    m_tray = new QSystemTrayIcon(this);
    m_menu = new QMenu();

    m_actStatus = m_menu->addAction(tr("OKIDIA — Bağlanıyor..."));
    m_actStatus->setEnabled(false);
    m_menu->addSeparator();

    m_actRaise = m_menu->addAction(QIcon(":/ui/hand.svg"), tr("Yardım İste"));
    m_actLower = m_menu->addAction(QIcon(":/ui/hand.svg"), tr("Eli İndir"));
    m_actLower->setVisible(false);
    m_menu->addSeparator();

    auto* actChat = m_menu->addAction(QIcon(":/ui/chat.svg"), tr("Sohbet"));
    actChat->setEnabled(false);
    actChat->setText(tr("Sohbet (Atanmadı)"));
    connect(actChat, &QAction::triggered, [this]() { emit chatStartRequested(); });
    m_actChat = actChat;  // pointer sakla → setChatEnabled için
    m_menu->addSeparator();

    auto* themeMenu = m_menu->addMenu(tr("Görünüm Teması"));
    const auto themes = ThemeManager::instance().availableThemes();
    for (int i = 0; i < themes.size(); ++i) {
        auto* act = themeMenu->addAction(themes[i].name);
        connect(act, &QAction::triggered, [i]() {
            ThemeManager::instance().applyTheme(i);
        });
    }

    m_menu->addSeparator();
    m_menu->addAction(tr("Hakkında OKIDIA"), []() {
        const QString ver = qApp->applicationVersion().isEmpty() ? Okidia::Version::string() : qApp->applicationVersion();
        QMessageBox msgBox;
        msgBox.setWindowTitle(QObject::tr("Hakkında OKIDIA"));
        msgBox.setIconPixmap(QIcon(":/icons/agent/okidia-agent-connected-32.png").pixmap(48, 48));
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(
            QString("<h3>OKIDIA Agent <span style='font-size: 13px; font-weight: normal;'>v%1</span></h3>"
                    "<p><b>%2</b></p>"
                    "<p>%3</p>"
                    "<hr/>"
                    "<p><b>%4:</b> <a href='https://www.okidia.net'>https://www.okidia.net</a><br/>"
                    "<b>%5:</b> <a href='mailto:info@okidia.net'>info@okidia.net</a></p>"
                    "<hr/>"
                    "<p style='font-size: 11px; color: #7f8c8d;'>"
                    "<b>%6:</b> Qt 6 (LGPLv3), OpenSSL, Protocol Buffers, X11, libjpeg-turbo.<br/>"
                    "%7"
                    "</p>")
            .arg(ver)
            .arg(QObject::tr("Okul İçi Denetim ve İletişim Ağı — Öğrenci ve İstemci Servisi"))
            .arg(QObject::tr("Yüksek performanslı ekran aktarımı, ders etkileşimi, dosya alma, anons dinleme ve güvenli mesajlaşma istemcisi."))
            .arg(QObject::tr("Web Sitesi"))
            .arg(QObject::tr("İletişim"))
            .arg(QObject::tr("Açık Kaynak Bileşenler"))
            .arg(QObject::tr("Telif Hakkı © 2024-2026 OKIDIA. Tüm hakları saklıdır."))
        );
        QAbstractButton* btnLicense = msgBox.addButton(QObject::tr("Lisans Bilgisi"), QMessageBox::ActionRole);
        msgBox.addButton(QMessageBox::Ok);
        msgBox.exec();
        if (msgBox.clickedButton() == btnLicense) {
            OkidiaUtils::showLicenseDialog();
        }
    });

    m_tray->setContextMenu(m_menu);
    connect(m_actRaise, &QAction::triggered, this, &AgentTray::onHandRaise);
    connect(m_actLower, &QAction::triggered, this, &AgentTray::onHandLower);
    connect(m_tray, &QSystemTrayIcon::activated, this, &AgentTray::onTrayActivated);

    startSearchAnimation();
}

// ── Durum güncellemeleri ─────────────────────────────────────────────────────

void AgentTray::setConnected(bool connected)
{
    m_connected = connected;
    if (connected) {
        stopSearchAnimation();
        m_actStatus->setText(tr("OKIDIA — Bağlı"));
    } else {
        m_actStatus->setText(tr("OKIDIA — Bağlantı aranıyor..."));
        startSearchAnimation();
    }
    updateIcon();
    updateMenuState();
}

void AgentTray::setLocked(bool locked)    { m_locked = locked;       updateIcon(); }
void AgentTray::setChatEnabled(bool enabled) {
    if (m_actChat) {
        m_actChat->setEnabled(enabled);
        m_actChat->setIcon(QIcon(enabled ? ":/ui/chat.svg" : ":/ui/mute.svg"));
        m_actChat->setText(enabled ? tr("Sohbet") : tr("Sohbet (Kilitli / Kapalı)"));
    }
}
void AgentTray::setHandRaised(bool raised)
{
    m_handRaised = raised;
    m_actRaise->setVisible(!raised);
    m_actLower->setVisible(raised);
    updateIcon();
}

void AgentTray::show() { m_tray->show(); }

// ── Animasyon ────────────────────────────────────────────────────────────────

void AgentTray::startSearchAnimation()
{
    m_animFrame = 0;
    m_tray->setIcon(m_searchIcons[0]);
    m_animTimer->start();
}

void AgentTray::stopSearchAnimation() { m_animTimer->stop(); }

void AgentTray::onAnimationTick()
{
    m_animFrame = (m_animFrame + 1) % m_searchIcons.size();
    m_tray->setIcon(m_searchIcons[m_animFrame]);
}

// ── İkon güncelleme ──────────────────────────────────────────────────────────

void AgentTray::updateIcon()
{
    m_tray->setToolTip(m_actStatus->text());

    if (!m_connected) return;  // animasyon zaten doğru ikonu gösteriyor

    if      (m_handRaised) m_tray->setIcon(m_iconHandRaised);
    else if (m_locked)     m_tray->setIcon(m_iconLocked);
    else                   m_tray->setIcon(m_iconConnected);
}

// ── El kaldırma ──────────────────────────────────────────────────────────────

void AgentTray::onHandRaise()
{
    bool ok;
    QString msg = QInputDialog::getText(nullptr, tr("Yardım İste"),
        tr("Öğretmeninize kısa bir not ekleyebilirsiniz (opsiyonel):"),
        QLineEdit::Normal, QString(), &ok);

    if (!ok) return;  // iptal edildi

    okidia::HandRaiseEvent evt;
    evt.set_message(msg.toStdString());
    evt.set_timestamp(OkidiaUtils::nowSecsU64());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::HAND_RAISE);
    env.set_payload(evt.SerializeAsString());

    // m_conn: LocalConnection (servis soketi) — null ise mesaj gönderilemez
    if (m_conn && m_conn->isConnected())
        m_conn->sendEnvelope(env);
    else
        Logger::info(QObject::tr("AgentTray: El kaldırma gönderilemedi — servis bağlantısı yok"));

    setHandRaised(true);
    m_tray->showMessage("OKIDIA", tr("Yardım isteğiniz gönderildi."),
                        QSystemTrayIcon::Information, 3000);
    emit handRaiseRequested(msg);
}

void AgentTray::onHandLower()
{
    okidia::Envelope env;
    env.set_type(okidia::MessageType::HAND_LOWER);

    if (m_conn && m_conn->isConnected())
        m_conn->sendEnvelope(env);

    setHandRaised(false);
    emit handLowerRequested();
}

void AgentTray::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    // Sağ tık: Qt zaten contextMenu()'yu gösteriyor (setContextMenu ile bağlı).
    // Sol tık (Trigger) için ayrıca popup() çağırmaya gerek yok —
    // popup() kendi event loop'u başlatıp diğer pencereleri bloke ediyor.
    Q_UNUSED(reason)
}

void AgentTray::updateMenuState()
{
    m_actRaise->setEnabled(m_connected && !m_handRaised);
}
