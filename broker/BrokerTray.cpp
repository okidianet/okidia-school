// broker/BrokerTray.cpp
#include "BrokerTray.h"

#include <QMenu>
#include <QAction>
#include <QPixmap>
#include <QPainter>
#include <QFile>
#include <QCursor>
#include <QPolygon>

QIcon BrokerTray::loadIcon(const QString& path, const QColor& fallback)
{
    // Hicolor dosya sisteminden dene
    const QString hicolor = "/usr/share/icons/hicolor/22x22/apps/okidia-broker.png";
    if (QFile::exists(hicolor)) {
        QIcon ico(hicolor);
        if (!ico.isNull()) return ico;
    }
    Q_UNUSED(path)
    return makeFallbackIcon(fallback);
}

QIcon BrokerTray::makeFallbackIcon(const QColor& color)
{
    QPixmap px(22, 22);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    // Elmas şekil — daireden görsel ayırt için
    QPolygon diamond;
    diamond << QPoint(11, 2) << QPoint(20, 11)
            << QPoint(11, 20) << QPoint(2, 11);
    p.drawPolygon(diamond);
    p.end();
    return QIcon(px);
}

BrokerTray::BrokerTray(QObject* parent) : QObject(parent)
{
    m_icon = loadIcon("", QColor("#2ecc71"));

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(m_icon);  // constructor yerine explicit setIcon
    m_menu = new QMenu();

    m_actStatus = m_menu->addAction(tr("OKIDIA Broker — Başlatılıyor..."));
    m_actStatus->setEnabled(false);
    m_menu->addSeparator();

    m_actClients = m_menu->addAction(tr("İstemci: 0"));
    m_actClients->setEnabled(false);

    m_menu->addSeparator();
    // TODO: port değişikliği menü öğesi buraya (Veyon çakışması)

    auto* actQuit = m_menu->addAction(tr("Sunucuyu Durdur"));
    connect(actQuit, &QAction::triggered, this, &BrokerTray::quitRequested);

    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated,
            this, &BrokerTray::onTrayActivated);

    updateTooltip();
}

BrokerTray::~BrokerTray() { delete m_menu; }

void BrokerTray::setReady(bool ready)
{
    m_ready = ready;
    m_actStatus->setText(ready
        ? tr("OKIDIA Broker — Hazır")
        : tr("OKIDIA Broker — Başlatılıyor..."));
    updateTooltip();
}

void BrokerTray::setMasterConnected(bool connected)
{
    m_masterConn = connected;
    updateTooltip();
}

void BrokerTray::setClientCount(int count)
{
    m_clientCount = count;
    m_actClients->setText(QString(tr("İstemci: %1")).arg(count));
    updateTooltip();
}

void BrokerTray::setError(const QString& message)
{
    m_errorMsg = message;
    m_actStatus->setText(tr("OKIDIA Broker — Hata!"));
    m_tray->showMessage(tr("OKIDIA Broker"), message,
                        QSystemTrayIcon::Critical, 5000);
    updateTooltip();
}

void BrokerTray::show() { m_tray->show(); }

void BrokerTray::updateTooltip()
{
    QString tip = tr("OKIDIA Broker\n");
    tip += m_ready ? tr("Durum: Hazır\n") : tr("Durum: Başlatılıyor\n");
    tip += QString(tr("Master: %1\n")).arg(m_masterConn ? tr("Bağlı") : tr("Bekleniyor"));
    tip += QString(tr("İstemci: %1")).arg(m_clientCount);
    if (!m_errorMsg.isEmpty())
        tip += tr("\nHata: ") + m_errorMsg;
    m_tray->setToolTip(tip);
}

void BrokerTray::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger)
        m_tray->contextMenu()->popup(QCursor::pos());
}

void BrokerTray::setSetupCode(const QString& code) {
    if (!m_tray) return;
    if (code.isEmpty()) {
        m_tray->setToolTip(tr("OKIDIA Broker — Kurulum kodu kullanıldı"));
    } else {
        m_tray->setToolTip(QString(tr("OKIDIA Broker — Kurulum kodu: %1")).arg(code));
        m_tray->showMessage(tr("OKIDIA Kurulum Kodu"),
                    QString(tr("Kod: %1\nBu kodu Master kurulumunda girin.")).arg(code),
                    QSystemTrayIcon::Information, 30000);
    }
}
