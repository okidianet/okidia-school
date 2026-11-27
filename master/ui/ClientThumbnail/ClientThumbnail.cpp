// master/ui/ClientThumbnail/ClientThumbnail.cpp
#include "ClientThumbnail.h"
#include "../LiveTooltip/LiveTooltip.h"
#include <QTimer>
#include <QResizeEvent>
#include <QApplication>
#include "ui_ClientThumbnail.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QIcon>
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/db/OkidiaDatabase.h"

LiveTooltip* ClientThumbnail::s_liveTooltip = nullptr;
ClientThumbnail* ClientThumbnail::s_currentTooltipOwner = nullptr;

void ClientThumbnail::hideLiveTooltip() {
    if (s_currentTooltipOwner) {
        s_currentTooltipOwner->m_hoverTimer->stop();
        s_currentTooltipOwner->m_hovered = false;
        s_currentTooltipOwner->updateSelectionStyle();
        emit s_currentTooltipOwner->metricsStreamRequested(s_currentTooltipOwner->m_state.clientId, false);
        s_currentTooltipOwner = nullptr;
    }
    if (s_liveTooltip && s_liveTooltip->isVisible()) {
        s_liveTooltip->stopDismissTimer();
        s_liveTooltip->hide();
    }
}

ClientThumbnail::ClientThumbnail(const QString& clientId,
                                   QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::ClientThumbnail) {

    ui->setupUi(this);
    m_state.clientId = clientId;
    m_state.hostname = clientId;

    ui->hostnameLabel->setText(clientId);
    ui->usernameLabel->setText("");
    ui->aliasLabel->setText("");
    ui->aliasLabel->setVisible(false);
    ui->statusIcon->setProperty("class", "statusIconOffline");

    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(500); // 500ms delay for tooltip
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() {
        if (!m_hovered || !isActiveWindow()) return;
        if (s_currentTooltipOwner && s_currentTooltipOwner != this) {
            s_currentTooltipOwner->m_hoverTimer->stop();
            s_currentTooltipOwner->m_hovered = false;
            s_currentTooltipOwner->updateSelectionStyle();
            emit s_currentTooltipOwner->metricsStreamRequested(s_currentTooltipOwner->m_state.clientId, false);
        }
        s_currentTooltipOwner = this;
        if (!s_liveTooltip) {
            s_liveTooltip = new LiveTooltip();
            connect(s_liveTooltip, &LiveTooltip::tooltipDismissed, []() {
                if (s_currentTooltipOwner) {
                    s_currentTooltipOwner->m_hovered = false;
                    s_currentTooltipOwner->updateSelectionStyle();
                    emit s_currentTooltipOwner->metricsStreamRequested(s_currentTooltipOwner->m_state.clientId, false);
                    s_currentTooltipOwner = nullptr;
                }
            });
        }
        s_liveTooltip->updateData(m_state);
        s_liveTooltip->showAt(this);
        emit metricsStreamRequested(m_state.clientId, true);
    });

    ui->iconHandRaise->installEventFilter(this);
    ui->screenLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    ui->infoWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // Tema değiştiğinde ikonları ve arayüzü güncelle
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        updateStatusIcons();
        if (m_state.agentType == 2 || !m_state.connected) {
            showPixmap(); // İdari PC ve çevrim dışı yazısının rengini güncellemek için
        }
    });
}

ClientThumbnail::~ClientThumbnail() {
    if (s_currentTooltipOwner == this) {
        hideLiveTooltip();
    }
    clearFrameBuffer();
    delete ui;
}

void ClientThumbnail::clearFrameBuffer() {
    m_frameBuffer = QPixmap();
    m_hasValidFrame = false;
    m_remoteWidth = 0;
    m_remoteHeight = 0;
    m_state.initialFrame.clear();
    if (ui && ui->screenLabel) {
        ui->screenLabel->clear();
    }
}

void ClientThumbnail::setStreamingMode(bool streaming) {
    m_isStreaming = streaming;
    m_hasValidFrame = false;
    if (streaming) {
        m_frameBuffer = QPixmap();
    } else {
        m_frameBuffer = QPixmap();
        m_remoteWidth = 0;
        m_remoteHeight = 0;
    }
    showPixmap();
}

void ClientThumbnail::setBrokerVersion(const QString& ver) {
    m_brokerVersion = ver;
    updateStatusIcons();
}


void ClientThumbnail::updateFrame(const QByteArray& jpegData) {
    if (m_state.agentType == 2) {
        m_frameBuffer = QPixmap();
        showPixmap();
        return;
    }

    if (!m_state.connected) {
        ui->usernameLabel->setText("");
        ui->statusIcon->setProperty("class", "statusIconOffline");
        ui->statusIcon->style()->unpolish(ui->statusIcon);
        ui->statusIcon->style()->polish(ui->statusIcon);
        m_frameBuffer = QPixmap();
        m_hasValidFrame = false;
        
        if (m_state.agentType == 2) {
            ui->screenLabel->clear();
            ui->screenLabel->setProperty("class", "");
            ui->screenLabel->style()->unpolish(ui->screenLabel);
            ui->screenLabel->style()->polish(ui->screenLabel);
            showPixmap();
        } else {
            ui->screenLabel->setProperty("class", "screenLabelOffline");
            ui->screenLabel->style()->unpolish(ui->screenLabel);
            ui->screenLabel->style()->polish(ui->screenLabel);
            ui->screenLabel->setText(tr("Çevrim dışı"));
            ui->screenLabel->setAlignment(Qt::AlignCenter);
        }
        return;
    }

    if (m_state.agentType == 2) {
        m_frameBuffer = QPixmap();
        ui->screenLabel->clear();
        ui->screenLabel->setProperty("class", "");
        ui->screenLabel->style()->unpolish(ui->screenLabel);
        ui->screenLabel->style()->polish(ui->screenLabel);
        showPixmap();
        return;
    }

    if (jpegData.isEmpty()) {
        // Online ama görüntü yok (Siyah ekran + hostname)
        m_frameBuffer = QPixmap();
        m_hasValidFrame = false;
        ui->screenLabel->setProperty("class", "screenLabelOnline");
        ui->screenLabel->style()->unpolish(ui->screenLabel);
        ui->screenLabel->style()->polish(ui->screenLabel);
        QString name = m_state.hostname.isEmpty()
            ? m_state.clientId.left(12) : m_state.hostname;
        ui->screenLabel->setText(name);
        ui->screenLabel->setAlignment(Qt::AlignCenter);
        return;
    }

    if (m_isStreaming || m_state.lockType == "RemoteControl" || m_state.lockType == "ScreenView" || m_state.lockType == "Broadcast") {
        // Buffer'ı temizlemiyoruz, sadece overlay modunu gösteriyoruz
        showPixmap();
        return;
    }

    QPixmap pixmap;
    if (!pixmap.loadFromData(jpegData, "JPEG")) return;

    // Eğer 1x1 gibi mikro bir placeholder geldiyse (örn. sendBlackFrame),
    // buffer'ı 1x1 yapıp minyatür arayüzü bozmak yerine temiz siyah bir buffer oluştur:
    if (pixmap.width() <= 10 || pixmap.height() <= 10) {
        int bufW = m_remoteWidth > 0 ? m_remoteWidth : 320;
        int bufH = m_remoteHeight > 0 ? m_remoteHeight : 180;
        m_frameBuffer = QPixmap(bufW, bufH);
        m_frameBuffer.fill(Qt::black);
        m_hasValidFrame = false;
        showPixmap();
        return;
    }

    ui->screenLabel->clear();
    ui->screenLabel->setProperty("class", "");
    ui->screenLabel->style()->unpolish(ui->screenLabel);
    ui->screenLabel->style()->polish(ui->screenLabel);
    m_frameBuffer = pixmap;
    m_hasValidFrame = true;
    m_remoteWidth  = pixmap.width();
    m_remoteHeight = pixmap.height();
    showPixmap();
}

void ClientThumbnail::showPixmap() {
    if (m_isStreaming || m_state.lockType == "RemoteControl" || m_state.lockType == "ScreenView" || m_state.lockType == "Broadcast") {
        QSize labelSize = ui->screenLabel->size();
        if (labelSize.width() < 10 || labelSize.height() < 10) return;
        
        QPixmap pm(labelSize);
        pm.fill(QColor(130, 0, 0)); // Dikkat çekici koyu kırmızı
        QPainter p(&pm);
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSize(10);
        p.setFont(f);
        
        QString masterName = m_state.lockedByDisplayName.isEmpty() ? m_state.lockedBy : m_state.lockedByDisplayName;
        if (masterName.isEmpty()) masterName = tr("Yönetici");
        QString text;
        if (m_state.lockType == "Broadcast") {
            text = QString(tr("%1\ntarafından\nekran yayını yapılıyor")).arg(masterName);
        } else {
            text = QString(tr("%1\ntarafından\nyönetiliyor / izleniyor")).arg(masterName);
        }
        p.drawText(pm.rect(), Qt::AlignCenter, text);
        p.end();
        
        ui->screenLabel->setPixmap(pm);
        return;
    }

    if (m_state.agentType == 2) {
        QSize labelSize = ui->screenLabel->size();
        if (labelSize.width() < 10 || labelSize.height() < 10) return;
        
        QPixmap pm(labelSize);
        pm.fill(QColor(26, 36, 54)); // Lacivert (Koyu mavi)
        QPainter p(&pm);
        p.setPen(QColor(236, 240, 241));
        QFont f = p.font();
        f.setPointSize(11);
        f.setBold(true);
        p.setFont(f);
        
        QString text;
        if (m_state.connected) {
            text = tr("İDARİ BİLGİSAYAR\n(İzleme mümkün değil)");
        } else {
            text = tr("Çevrim dışı");
        }
        
        p.drawText(pm.rect(), Qt::AlignCenter, text);
        p.end();
        
        ui->screenLabel->setPixmap(pm);
        return;
    }



    if (m_frameBuffer.isNull()) {
        ui->screenLabel->clear();
        ui->screenLabel->setProperty("class", "screenLabelPlaceholder");
        ui->screenLabel->style()->unpolish(ui->screenLabel);
        ui->screenLabel->style()->polish(ui->screenLabel);
        return;
    }

    QSize labelSize = ui->screenLabel->size();
    if (labelSize.width() < 10 || labelSize.height() < 10) return;

    ui->screenLabel->setPixmap(
        m_frameBuffer.scaled(
            labelSize,
            Qt::KeepAspectRatio,
            Qt::FastTransformation));
}

void ClientThumbnail::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    showPixmap();  // Boyut değişince yeniden ölçekle
}

void ClientThumbnail::updateState(const ClientState& state) {
    const bool wasSelected = m_state.selected;  // Seçim durumunu koru
    const QString prevLockType = m_state.lockType;
    m_state = state;
    m_state.selected = wasSelected;              // SessionManager seçimi sıfırlamasın

    ui->hostnameLabel->setText(state.hostname);
    if (!state.alias.isEmpty()) {
        ui->aliasLabel->setText(state.alias);
        ui->aliasLabel->setVisible(true);
    } else {
        ui->aliasLabel->clear();
        ui->aliasLabel->setVisible(false);
    }

    if (!state.connected) {
        ui->usernameLabel->setText("");
        ui->usernameLabel->setToolTip("");
        ui->usernameLabel->setProperty("status", "");
        setToolTip("");
        ui->statusIcon->setProperty("class", "statusIconOffline");
        ui->statusIcon->style()->unpolish(ui->statusIcon);
        ui->statusIcon->style()->polish(ui->statusIcon);
        
        m_frameBuffer = QPixmap();
        m_hasValidFrame = false;
        if (state.agentType == 2) {
            ui->screenLabel->clear();
            ui->screenLabel->setProperty("class", "");
            ui->screenLabel->style()->unpolish(ui->screenLabel);
            ui->screenLabel->style()->polish(ui->screenLabel);
            showPixmap();
        } else {
            // Kırmızı ekran + Çevrim dışı
            ui->screenLabel->setProperty("class", "screenLabelOffline");
            ui->screenLabel->style()->unpolish(ui->screenLabel);
            ui->screenLabel->style()->polish(ui->screenLabel);
            ui->screenLabel->setText(tr("Çevrim dışı"));
            ui->screenLabel->setAlignment(Qt::AlignCenter);
        }
        return;
    }

    QString userText = state.username;
    if (!state.userDisplayName.isEmpty() && state.userDisplayName != state.username) {
        userText = QString("%1 (%2)").arg(state.userDisplayName, state.username);
    }
    ui->usernameLabel->setText(userText);
    ui->usernameLabel->setToolTip(userText);
    ui->usernameLabel->setProperty("status", "");
    ui->statusIcon->setProperty("class", "statusIconOnline");
    ui->statusIcon->style()->unpolish(ui->statusIcon);
    ui->statusIcon->style()->polish(ui->statusIcon);

    if (state.agentType == 2) {
        m_frameBuffer = QPixmap();
        ui->screenLabel->clear();
        ui->screenLabel->setProperty("class", "");
        ui->screenLabel->style()->unpolish(ui->screenLabel);
        ui->screenLabel->style()->polish(ui->screenLabel);
        showPixmap();
    }

    updateStatusIcons();
    if (!state.lockType.isEmpty()) {
        setToolTip(QString(tr("İstemci Kilitli\nKilitleyen: %1\nKilit Tipi: %2"))
                   .arg(state.lockedByDisplayName.isEmpty() ? state.lockedBy : state.lockedByDisplayName)
                   .arg(state.lockType));
    } else {
        setToolTip("");
    }
    updateSelectionStyle();
    if (s_liveTooltip && s_liveTooltip->isVisible() && s_currentTooltipOwner == this) {
        s_liveTooltip->updateData(m_state);
    }
    if (prevLockType != state.lockType) {
        showPixmap();
    }
}

void ClientThumbnail::setSelected(bool selected) {
    if (m_state.selected == selected) return;
    m_state.selected = selected;
    updateSelectionStyle();
    if (s_liveTooltip && s_liveTooltip->isVisible() && s_currentTooltipOwner == this) {
        s_liveTooltip->updateData(m_state);
    }
}

void ClientThumbnail::updateStatusIcons() {
    const int iconSize = 12;

    // İstemci Tipi (Client Type) İkonu
    if (m_state.agentType == 1) {
        ui->iconAgentType->setPixmap(ThemeManager::icon(":/ui/student.svg").pixmap(iconSize, iconSize));
        ui->iconAgentType->setToolTip(tr("Öğrenci Bilgisayarı"));
        ui->iconAgentType->setVisible(true);
    } else if (m_state.agentType == 2) {
        ui->iconAgentType->setPixmap(ThemeManager::icon(":/ui/admin.svg").pixmap(iconSize, iconSize));
        ui->iconAgentType->setToolTip(tr("İdari Bilgisayar"));
        ui->iconAgentType->setVisible(true);
    } else if (m_state.agentType == 3) {
        ui->iconAgentType->setPixmap(ThemeManager::icon(":/ui/smartboard.svg").pixmap(iconSize, iconSize));
        ui->iconAgentType->setToolTip(tr("Akıllı Tahta"));
        ui->iconAgentType->setVisible(true);
    } else if (m_state.agentType >= 100) {
        ui->iconAgentType->setPixmap(ThemeManager::icon(":/ui/custom_profile.svg").pixmap(iconSize, iconSize));
        QString pName = OkidiaDatabase::instance().customProfile(m_state.agentType).name;
        if (pName.isEmpty()) pName = tr("Özel Profil (%1)").arg(m_state.agentType);
        ui->iconAgentType->setToolTip(pName);
        ui->iconAgentType->setVisible(true);
    } else {
        ui->iconAgentType->setVisible(false);
    }

    // OS İkonu
    if (m_state.osTag.contains("win", Qt::CaseInsensitive) || 
        m_state.osPrettyName.contains("Windows", Qt::CaseInsensitive)) {
        ui->iconOs->setPixmap(ThemeManager::icon(":/ui/windows.svg").pixmap(iconSize, iconSize));
        ui->iconOs->setToolTip(tr("İşletim Sistemi: %1").arg(m_state.osPrettyName.isEmpty() ? QStringLiteral("Windows") : m_state.osPrettyName));
        ui->iconOs->setVisible(true);
    } else if (!m_state.osTag.isEmpty() || !m_state.osPrettyName.isEmpty()) {
        ui->iconOs->setPixmap(ThemeManager::icon(":/ui/linux.svg").pixmap(iconSize, iconSize));
        ui->iconOs->setToolTip(tr("İşletim Sistemi: %1").arg(m_state.osPrettyName.isEmpty() ? QStringLiteral("Linux") : m_state.osPrettyName));
        ui->iconOs->setVisible(true);
    } else {
        ui->iconOs->setVisible(false);
    }

    // SSH Durumu
    ui->iconSshStatus->setPixmap(ThemeManager::icon(":/ui/key.svg").pixmap(iconSize, iconSize));
    ui->iconSshStatus->setVisible(m_state.sshConfigured); // ClientState'te sshConfigured özelliği varsa (yoksa build hatası verir, kontrol edelim)

    // Dinamik Durum İkonları
    ui->iconScreenLock->setPixmap(ThemeManager::icon(":/ui/lock.svg").pixmap(iconSize, iconSize));
    ui->iconScreenLock->setVisible(m_state.screenLocked);
    
    ui->iconInputLock->setPixmap(ThemeManager::icon(":/ui/lock.svg").pixmap(iconSize, iconSize));
    ui->iconInputLock->setVisible(m_state.inputLocked);
    
    ui->iconMute->setPixmap(ThemeManager::icon(":/ui/mute.svg").pixmap(iconSize, iconSize));
    ui->iconMute->setVisible(m_state.audioMuted);
    
    ui->iconWebFilter->setPixmap(ThemeManager::icon(":/ui/network.svg").pixmap(iconSize, iconSize));
    ui->iconWebFilter->setVisible(m_state.webFiltered);
    
    ui->iconAppBlock->setPixmap(ThemeManager::icon(":/ui/forbidden.svg").pixmap(iconSize, iconSize));
    ui->iconAppBlock->setVisible(m_state.appBlocked);
    
    ui->iconChat->setPixmap(ThemeManager::icon(":/ui/chat.svg").pixmap(iconSize, iconSize));
    ui->iconChat->setVisible(m_state.chatActive);
    
    // iconExam için SVG yoksa eski hali kalır (ama svg bulalım)
    // ui->iconExam->setPixmap(ThemeManager::icon(":/ui/exam.svg").pixmap(iconSize, iconSize));
    ui->iconExam->setVisible(m_state.examActive);
    
    ui->iconHandRaise->setPixmap(ThemeManager::icon(":/ui/hand.svg").pixmap(iconSize, iconSize));
    ui->iconHandRaise->setVisible(m_state.handRaised);
    if (m_state.handRaised) {
        ui->iconHandRaise->setCursor(Qt::PointingHandCursor);
        QString tooltipText = tr("<b>Yardım Talebi</b>");
        if (!m_state.handRaiseMessage.isEmpty()) {
            tooltipText += "<br><i>\"" + m_state.handRaiseMessage.toHtmlEscaped() + "\"</i>";
        }
        tooltipText += tr("<br><font color='#3b82f6'>Özel sohbet açmak için tıklayın</font>");
        ui->iconHandRaise->setToolTip(tooltipText);
    } else {
        ui->iconHandRaise->setToolTip("");
    }

    // Uyumsuz Sürüm (Agent Sürümü > Broker Sürümü)
    bool versionMismatch = false;
    if (m_state.connected && !m_brokerVersion.isEmpty() && !m_state.agentVersion.isEmpty() && m_state.agentVersion != "0.0.0") {
        if (OkidiaUtils::compareVersions(m_state.agentVersion, m_brokerVersion) > 0) {
            versionMismatch = true;
        }
    }

    if (ui->iconVersionWarning) {
        if (m_state.rtcBatteryLow) {
            ui->iconVersionWarning->setPixmap(ThemeManager::icon(":/ui/warning.svg").pixmap(iconSize, iconSize));
            ui->iconVersionWarning->setVisible(true);
            ui->iconVersionWarning->setToolTip(
                tr("<b>Donanım Saati / BIOS Pili Arızası</b><br>Bu cihazın BIOS pili bitik veya sistem saati sıfırlanmış.<br>Lütfen BIOS pilini kontrol edin / değiştirin."));
        } else {
            ui->iconVersionWarning->setPixmap(ThemeManager::icon(":/ui/warning.svg").pixmap(iconSize, iconSize));
            ui->iconVersionWarning->setVisible(versionMismatch);
            if (versionMismatch) {
                ui->iconVersionWarning->setToolTip(
                    QString(tr("<b>Uyumsuz Sürüm Uyarısı</b><br>İstemci Sürümü (v%1) sunucudan (v%2) daha yeni!<br>Kararlı sürüme geri dönmek için sağ tıklayın."))
                        .arg(m_state.agentVersion, m_brokerVersion));
            } else {
                ui->iconVersionWarning->setToolTip("");
            }
        }
    }
}


bool ClientThumbnail::eventFilter(QObject* watched, QEvent* event) {
    if (watched == ui->iconHandRaise && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && m_state.handRaised) {
            emit handRaiseClicked(m_state.clientId);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ClientThumbnail::updateSelectionStyle() {
    update();  // paintEvent tetikle
}

void ClientThumbnail::mousePressEvent(QMouseEvent* event) {
    if (m_hoverTimer) m_hoverTimer->stop();
    if (s_currentTooltipOwner == this) {
        hideLiveTooltip();
    }

    if (event->button() == Qt::LeftButton)
        m_dragStartPos = event->pos();   // drag eşiği için kaydet

    if (event->button() == Qt::RightButton) {
        emit contextMenuRequested(m_state.clientId,
                                   event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_state.selected && event->modifiers() == Qt::NoModifier) {
        // Zaten seçiliyse ve modifier yoksa seçimi hemen bozma, drag işlemi olabilir.
        // Seçimi mouseReleaseEvent'e bırakıyoruz.
        return;
    }

    // Sol tık: modifier sinyalini gönder, MainWindow seçimi yönetir
    emit clickedWithModifiers(m_state.clientId,
                               event->modifiers());
    emit clicked(m_state.clientId);
}

void ClientThumbnail::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_state.selected && event->modifiers() == Qt::NoModifier) {
        // Drag yapılmadı ve sadece tıklandıysa, şimdi tekli seçimi uygula
        emit clickedWithModifiers(m_state.clientId, event->modifiers());
        emit clicked(m_state.clientId);
    }
    QWidget::mouseReleaseEvent(event);
}

void ClientThumbnail::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) return;
    if ((event->pos() - m_dragStartPos).manhattanLength()
        < QApplication::startDragDistance()) return;
    // Sadece zaten seçili olan bir kart sürüklenmek istendiğinde QDrag başlat
    if (!m_state.selected) return;
    // dragInitiated → MainWindow MIME data ile QDrag başlatır
    emit dragInitiated(m_state.clientId);
}

void ClientThumbnail::enterEvent(QEnterEvent* event) {
    Q_UNUSED(event)
    if (!isActiveWindow()) return;
    m_hovered = true;
    updateSelectionStyle();
    m_hoverTimer->start();
}

void ClientThumbnail::leaveEvent(QEvent* event) {
    Q_UNUSED(event)
    if (!isActiveWindow()) {
        m_hovered = false;
        updateSelectionStyle();
        m_hoverTimer->stop();
        if (s_currentTooltipOwner == this) {
            hideLiveTooltip();
        }
        return;
    }
    if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
        m_hovered = false;
        updateSelectionStyle();
        m_hoverTimer->stop();
        if (s_currentTooltipOwner == this) {
            hideLiveTooltip();
        }
    }
}

void ClientThumbnail::mouseDoubleClickEvent(QMouseEvent* event) {
    Q_UNUSED(event)
    emit doubleClicked(m_state.clientId);
}

void ClientThumbnail::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect r = rect().adjusted(1, 1, -1, -1);

    if (m_state.selected) {
        // Yarı saydam mavi arka plan katmanı
        p.fillRect(r, QColor(137, 180, 250, 45));

        // Kalın mavi kenarlık
        p.setPen(QPen(QColor(137, 180, 250), 2));
        p.drawRoundedRect(r, 5, 5);

        // Sol üst köşe: mavi rozetli checkmark
        const int bSize = 20;
        const QRect badge(r.left() + 6, r.top() + 6, bSize, bSize);
        p.setBrush(QColor(137, 180, 250));
        p.setPen(Qt::NoPen);
        p.drawEllipse(badge);

        // Checkmark
        p.setPen(QPen(QColor(30, 30, 46), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF check;
        check << QPointF(badge.left() + 5,  badge.center().y())
              << QPointF(badge.center().x() - 1, badge.bottom() - 5)
              << QPointF(badge.right() - 4, badge.top() + 5);
        p.drawPolyline(check);
    } else if (m_hovered) {
        p.setPen(QPen(palette().mid(), 1));
        p.setBrush(palette().alternateBase());
        p.drawRoundedRect(r, 5, 5);
    } else {
        p.setPen(QPen(palette().shadow(), 1));
        p.setBrush(palette().base());
        p.drawRoundedRect(r, 5, 5);
    }

    // Draw lock overlay if locked
    if (!m_state.lockType.isEmpty()) {
        // Draw a small lock overlay
        p.setBrush(QColor(230, 75, 75, 200)); // reddish semi-transparent badge
        p.setPen(Qt::NoPen);
        QRect lockBadge(r.right() - 25, r.top() + 6, 20, 20);
        p.drawRoundedRect(lockBadge, 4, 4);
        
        // Draw lock symbol in badge
        p.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        // Draw lock body
        p.drawRoundedRect(lockBadge.center().x() - 5, lockBadge.center().y(), 10, 6, 1, 1);
        // Draw shackle
        p.drawArc(lockBadge.center().x() - 3, lockBadge.center().y() - 4, 6, 8, 0, 180 * 16);
    }
}

void ClientThumbnail::updateTile(int x, int y, int w, int h,
                                  const QByteArray& jpegData,
                                  bool showNow) {
    if (m_state.agentType == 2) {
        m_frameBuffer = QPixmap();
        if (showNow) showPixmap();
        return;
    }

    if (m_isStreaming || m_state.lockType == "RemoteControl" || m_state.lockType == "ScreenView" || m_state.lockType == "Broadcast") {
        if (showNow) showPixmap();
        return;
    }
    
    if (jpegData.isEmpty()) return;

    // B4: QImage decode (CPU only) — QPixmap::loadFromData'daki per-tile
    // X11 GPU round-trip upload overhead'ini ortadan kaldırır
    QImage tileImg;
    if (!tileImg.loadFromData(jpegData, "JPEG")) return;

    // Ekran çözünürlüğü değiştiğinde veya buffer null olduğunda yeniden oluştur
    if (m_frameBuffer.isNull() ||
        (m_remoteWidth > 0 && m_remoteHeight > 0 &&
         (m_frameBuffer.width() != m_remoteWidth ||
          m_frameBuffer.height() != m_remoteHeight))) {
        
        int bufW = m_remoteWidth > 0 ? m_remoteWidth : qMax(x + w, 640);
        int bufH = m_remoteHeight > 0 ? m_remoteHeight : qMax(y + h, 480);
        m_frameBuffer = QPixmap(bufW, bufH);
        m_frameBuffer.fill(QColor(30, 30, 46));  // koyu arka plan
    }

    // Tile boyutu buffer boyutunu aşıyorsa buffer'ı genişlet
    // (ekran çözünürlüğü değiştiğinde olabilir)
    int neededW = x + w;
    int neededH = y + h;
    if (neededW > m_frameBuffer.width() ||
        neededH > m_frameBuffer.height()) {
        // Gelen karo koordinatları beklenen çözünürlükten aşırı büyükse (uzak kontrolden kalan gecikmiş karo),
        // thumbnail buffer geometrisini bozmamak için bu karoyu atla:
        if (m_remoteWidth > 0 && (neededW > m_remoteWidth * 2 || neededH > m_remoteHeight * 2)) {
            return;
        }

        QPixmap grown(qMax(neededW, m_frameBuffer.width()),
                      qMax(neededH, m_frameBuffer.height()));
        grown.fill(QColor(30, 30, 46));
        QPainter p(&grown);
        p.drawPixmap(0, 0, m_frameBuffer);
        p.end();
        m_frameBuffer = grown;
    }

    // Tile'ı frame buffer'a uygula — drawImage: QImage → QPixmap tek seferde
    QPainter painter(&m_frameBuffer);
    painter.drawImage(x, y, tileImg);
    painter.end();
    m_hasValidFrame = true;

    // Batch modda render atlanır — batch sonunda refreshDisplay() çağrılır
    if (showNow)
        showPixmap();
}

void ClientThumbnail::refreshDisplay() {
    showPixmap();
}

void ClientThumbnail::setFrameSize(int width, int height) {
    // Ham ekran boyutunu sakla (ileride oran hesabında kullanılabilir).
    // Buffer'ı BURADA OLUŞTURMA — ölçeklenmiş boyut farklı olduğundan
    // siyah görüntüye neden olur. Buffer updateFrame() tarafından ayarlanır.
    m_remoteWidth  = width;
    m_remoteHeight = height;
}