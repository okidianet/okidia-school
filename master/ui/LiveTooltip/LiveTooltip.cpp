#include "LiveTooltip.h"
#include "../ClientThumbnail/ClientThumbnail.h" // For ClientState
#include "../../../common/db/OkidiaDatabase.h"
#include <QApplication>
#include <QScreen>
#include <QTimer>
#include <QHideEvent>

LiveTooltip::LiveTooltip(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    
    m_contentLabel = new QLabel(this);
    m_contentLabel->setStyleSheet(
        "background-color: palette(window);"
        "color: palette(window-text);"
        "border: 1px solid palette(mid);"
        "border-radius: 6px;"
        "padding: 8px;"
        "font-size: 11px;"
    );
    layout->addWidget(m_contentLabel);

    m_dismissTimer = new QTimer(this);
    m_dismissTimer->setSingleShot(true);
    connect(m_dismissTimer, &QTimer::timeout, this, [this]() {
        hide();
        emit tooltipDismissed();
    });
}

void LiveTooltip::updateData(const ClientState& state) {
    QString userStr = state.username.isEmpty() ? tr("Oturum Yok") : state.username;
    if (!state.userDisplayName.isEmpty() && state.userDisplayName != state.username) {
        userStr = QString("%1 (%2)").arg(state.userDisplayName, state.username);
    }
    QString html = "<b>" + state.hostname + "</b> (" + userStr + ")<br><br>";
    
    if (state.connected) {
        QString agentTypeStr;
        if (state.agentType == 1) agentTypeStr = tr("Öğrenci Bilgisayarı");
        else if (state.agentType == 2) agentTypeStr = tr("İdari Bilgisayar");
        else if (state.agentType == 3) agentTypeStr = tr("Akıllı Tahta");
        else if (state.agentType >= 100) {
            agentTypeStr = OkidiaDatabase::instance().customProfile(state.agentType).name;
            if (agentTypeStr.isEmpty()) agentTypeStr = tr("Özel Profil (%1)").arg(state.agentType);
        }
        else agentTypeStr = tr("Standart İstemci");
        html += QString(tr("<b>İstemci Tipi:</b> %1<br>")).arg(agentTypeStr);

        html += QString(tr("<b>CPU:</b> %1% &nbsp;&nbsp; <b>RAM:</b> %2%<br>"))
                .arg(state.cpuUsage).arg(state.ramUsage);
        html += QString(tr("<b>Disk:</b> %1%<br><br>")).arg(state.diskUsage);
        
        html += QString(tr("<b>OS:</b> %1<br>")).arg(state.osPrettyName.isEmpty() ? tr("Bilinmiyor") : state.osPrettyName);
        html += QString(tr("<b>IP:</b> %1<br>")).arg(state.ip.isEmpty() ? tr("Bilinmiyor") : state.ip);
        if (!state.mac.isEmpty()) {
            html += QString(tr("<b>MAC:</b> %1<br>")).arg(state.mac);
        }
        if (!state.screens.isEmpty()) {
            html += QString(tr("<b>Çözünürlük:</b> %1<br>")).arg(state.screens);
        }
        
        QString verText = state.agentVersion.isEmpty() ? tr("Bilinmiyor") : ("v" + state.agentVersion);
        html += QString(tr("<b>İstemci Sürümü:</b> %1<br>")).arg(verText);

        if (state.rtcBatteryLow) {
            html += QString(tr("<br><font color='#eab308'><b>⚠ UYARI:</b> Donanım Saati / BIOS Pili Arızalı!</font><br>"));
        }

        
        QStringList activeLocks;
        if (state.screenLocked) activeLocks << tr("Ekran");
        if (state.inputLocked) activeLocks << tr("Giriş");
        if (state.examActive) activeLocks << tr("Sınav");
        
        if (!activeLocks.isEmpty()) {
            html += "<br><b>" + tr("Aktif Kilitler:") + "</b> " + activeLocks.join(", ") + "<br>";
            if (!state.lockedByDisplayName.isEmpty()) {
                html += "<b>" + tr("Kilitleyen:") + "</b> " + state.lockedByDisplayName + "<br>";
            }
        }
    } else {
        html += tr("<font color='red'>Çevrim dışı</font>");
    }
    
    m_contentLabel->setText(html);
    adjustSize();
}

void LiveTooltip::showAt(QWidget* target) {
    if (!target) return;
    
    // İmlecin mevcut konumunu referans al
    QPoint globalPos = QCursor::pos() + QPoint(14, 16);
    adjustSize();
    
    // Ekran sınırlarını taşmayı engelle
    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen) {
        screen = target->screen();
    }
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    if (screen) {
        QRect screenRect = screen->geometry();
        if (globalPos.x() + width() > screenRect.right()) {
            globalPos.setX(screenRect.right() - width() - 8);
        }
        if (globalPos.y() + height() > screenRect.bottom()) {
            // İmlecin yukarısına taşı
            globalPos.setY(QCursor::pos().y() - height() - 8);
        }
        if (globalPos.x() < screenRect.left()) {
            globalPos.setX(screenRect.left() + 8);
        }
        if (globalPos.y() < screenRect.top()) {
            globalPos.setY(screenRect.top() + 8);
        }
    }
    
    move(globalPos);
    show();
    raise();
    startDismissTimer(7000);
}

void LiveTooltip::startDismissTimer(int timeoutMs) {
    if (m_dismissTimer) m_dismissTimer->start(timeoutMs);
}

void LiveTooltip::stopDismissTimer() {
    if (m_dismissTimer) m_dismissTimer->stop();
}

void LiveTooltip::hideEvent(QHideEvent* event) {
    stopDismissTimer();
    QWidget::hideEvent(event);
}
