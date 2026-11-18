// master/ui/MainWindow/MainWindowExamAndFilter.cpp
#include "MainWindow.h"
#include "../../common/utils/Logger.h"
#include "../../audit/AuditLog.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QSettings>
#include "ui_MainWindow.h"
// ============================================================================
// Faz 6 — Sınav, Web filtresi, Ses, Zamanlı komut
// ============================================================================

void MainWindow::onStartExam() {
    if (!m_session) return;

    ExamDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    ExamConfig cfg = dlg.config();
    m_activeExamId = cfg.examId;
    int durSecs = cfg.durationMinutes * 60;
    QStringList targets = commandTargets();
    bool isAll = selectedClientIds().isEmpty() && (targets.size() == m_thumbnails.size());

    if (isAll) {
        m_session->broadcastExamStart(cfg.examId, cfg.title, durSecs);
        if (cfg.lockScreen) {
            m_session->broadcastScreenLock(tr("Sınav başladı: ") + cfg.title);
            m_session->broadcastInputLock();
        }
        if (cfg.blockWeb) m_session->broadcastWebBlockAll(tr("Sınav süresi"));
    } else {
        for (const QString& id : targets) {
            m_session->sendExamStart(id, cfg.examId, cfg.title, durSecs);
            if (cfg.lockScreen) {
                m_session->sendScreenLock(id, tr("Sınav başladı: ") + cfg.title);
                m_session->sendInputLock(id);
            }
            if (cfg.blockWeb) m_session->sendWebBlockAll(id, tr("Sınav süresi"));
        }
    }

    bool ok = !targets.isEmpty();
    AuditLog::instance().recordExam({}, targets.join(", "),
        tr("Sınav başlatıldı: %1 (Süre: %2 dk)").arg(cfg.title).arg(cfg.durationMinutes));

    showCommandStatus(ok,
        tr("Sınav başlatıldı: ") + cfg.title +
        " (" + QString::number(targets.size()) + " istemci)");

    // Toolbar buton durumları
    emit examStateChanged(true);   // sınav başladı

    // Süre belirlendiyse otomatik bitir
    if (cfg.durationMinutes > 0 && m_scheduler) {
        ScheduledTask task;
        task.label    = tr("Sınav bitti: ") + cfg.title;
        task.runAt    = QDateTime::currentDateTime()
                            .addSecs(durSecs);

        okidia::Envelope env;
        env.set_type(okidia::MessageType::EXAM_END);
        env.set_sequence_id(0);
        task.envelope = env;

        m_scheduler->schedule(task);
    }
}

void MainWindow::onCollectExam() {
    if (!m_session || m_activeExamId.isEmpty()) return;
    bool ok = m_session->broadcastExamCollect(m_activeExamId);
    AuditLog::instance().recordExam({}, m_activeExamId, tr("Sınav dosyalarını toplama komutu verildi"));
    showCommandStatus(ok, tr("Sınav toplama isteği gönderildi"));
}

void MainWindow::onEndExam() {
    if (!m_session) return;
    bool ok = m_session->broadcastExamEnd();
    // Kısıtlamaları kaldır
    m_session->broadcastScreenUnlock();
    m_session->broadcastInputUnlock();
    m_session->broadcastWebUnblock();
    AuditLog::instance().recordExam({}, m_activeExamId, tr("Sınav sonlandırıldı"));
    m_activeExamId.clear();
    showCommandStatus(ok, tr("Sınav bitirildi"));

    emit examStateChanged(false);  // sınav bitti
}

// ---------------------------------------------------------------------------
// Web filtresi — tek istemci
// ---------------------------------------------------------------------------
void MainWindow::onClientWebBlockAll(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendWebBlockAll(clientId, tr("Öğretmen komutu"));
    if (ok) {
        m_session->setClientWebFiltered(clientId, true);
        AuditLog::instance().recordSecurity({}, clientId, tr("Tüm internet erişimi engellendi"));
    }
    showCommandStatus(ok, tr("%1 internet engellendi").arg(clientId));
}

void MainWindow::onClientWebUnblock(const QString& clientId) {
    if (!m_session) return;
    bool ok = m_session->sendWebUnblock(clientId);
    if (ok) m_session->setClientWebFiltered(clientId, false);
    showCommandStatus(ok, tr("%1 internet engeli kaldırıldı").arg(clientId));
}

// ---------------------------------------------------------------------------
// Ses yayını
// ---------------------------------------------------------------------------
void MainWindow::onSendAudio() {
    // Dosyadan oynat artık dropdown menüsü içinde işleniyor
    // Bu slot geriye dönük uyumluluk için bırakıldı
}

void MainWindow::onAudioLiveToggle() {
    // Artık kayıt dialog'u üzerinden çalışıyor — bu slot kullanılmıyor
    if (!m_session) return;
    if (m_session->isRecording())
        m_session->stopRecording();
}

// ---------------------------------------------------------------------------
// Zamanlı komut
// ---------------------------------------------------------------------------
void MainWindow::onScheduleCommand() {
    if (!m_session || !m_scheduler) return;

    // Basit diyalog: hangi komut, kaç dakika sonra
    QStringList commandOptions = {
        tr("Tüm Ekranları Kilitle"),
        tr("Tüm Ekranları Aç"),
        tr("Tüm Bilgisayarları Kapat"),
        tr("Tüm Bilgisayarları Yeniden Başlat"),
        tr("Oturumları Kapat"),
        tr("Askıya Al (Uyku)"),
        tr("Sesi Kapat"),
        tr("Sesi Aç"),
        tr("İnterneti Engelle"),
        tr("İnternet Engelini Kaldır")
    };

    bool ok1;
    QString cmd = QInputDialog::getItem(
        this, tr("Zamanlı Komut"),
        tr("Çalıştırılacak komut:"),
        commandOptions, 0, false, &ok1);
    if (!ok1) return;

    bool ok2;
    int minutes = QInputDialog::getInt(
        this, tr("Zamanlı Komut"),
        tr("Kaç dakika sonra çalıştırılsın?"),
        5, 1, 120, 1, &ok2);
    if (!ok2) return;

    // Komut → Envelope
    okidia::Envelope env;
    env.set_sequence_id(0);

    int selIdx = commandOptions.indexOf(cmd);
    if (selIdx == 0) { // Tüm Ekranları Kilitle
        env.set_type(okidia::MessageType::SCREEN_LOCK);
        okidia::ScreenLockCommand lc;
        lc.set_message(tr("Zamanlı kilit uygulandı.").toStdString());
        QSettings s("okidia", "master");
        int defaultPolicy = s.value("smartboard/lockPolicy", 1).toInt();
        lc.set_lock_policy(defaultPolicy == 0 ? 1 : 2);
        env.set_payload(lc.SerializeAsString());
    } else if (selIdx == 1) { // Tüm Ekranları Aç
        env.set_type(okidia::MessageType::SCREEN_UNLOCK);
    } else if (selIdx == 2) { // Tüm Bilgisayarları Kapat
        env.set_type(okidia::MessageType::POWER_SHUTDOWN);
        okidia::PowerCommand pc;
        pc.set_delay_s(0);
        env.set_payload(pc.SerializeAsString());
    } else if (selIdx == 3) { // Tüm Bilgisayarları Yeniden Başlat
        env.set_type(okidia::MessageType::POWER_RESTART);
        okidia::PowerCommand pc;
        pc.set_delay_s(0);
        env.set_payload(pc.SerializeAsString());
    } else if (selIdx == 4) { // Oturumları Kapat
        env.set_type(okidia::MessageType::POWER_LOGOUT);
    } else if (selIdx == 5) { // Askıya Al (Uyku)
        env.set_type(okidia::MessageType::POWER_HIBERNATE);
    } else if (selIdx == 6) { // Sesi Kapat
        env.set_type(okidia::MessageType::AUDIO_MUTE);
    } else if (selIdx == 7) { // Sesi Aç
        env.set_type(okidia::MessageType::AUDIO_UNMUTE);
    } else if (selIdx == 8) { // İnterneti Engelle
        env.set_type(okidia::MessageType::WEB_BLOCK_ALL);
        okidia::WebFilterCommand wfc;
        wfc.set_reason(tr("Zamanlı internet engeli.").toStdString());
        env.set_payload(wfc.SerializeAsString());
    } else if (selIdx == 9) { // İnternet Engelini Kaldır
        env.set_type(okidia::MessageType::WEB_UNBLOCK);
    } else {
        return;
    }

    ScheduledTask task;
    task.label    = QString("%1 (%2 %3)").arg(cmd).arg(minutes).arg(tr("dk sonra"));
    task.runAt    = QDateTime::currentDateTime().addSecs(minutes * 60);
    task.envelope = env;

    QString id = m_scheduler->schedule(task);
    showCommandStatus(true,
        QString("⏱  %1 → %2 %3 [%4]").arg(cmd).arg(minutes).arg(tr("dk sonra")).arg(id));
}

// ============================================================================
// Çoklu monitör — istemci ekran kaynağı seçimi
// ============================================================================

void MainWindow::onClientSelectScreen(const QString& clientId) {
    if (!m_session) return;
    m_session->sendScreenInfoRequest(clientId);
}
void MainWindow::onScreenInfoReceived(
    const QString& clientId,
    const okidia::ScreenInfoResponse& info) {

    // Eğer bu bir otomatik kontrol isteği ise (çift tıklama)
    if (m_autoControlRequested && m_autoControlTarget == clientId) {
        m_autoControlRequested = false;

        // Ekran kilitliyse otomatik aç
        if (m_session && m_session->clientState(clientId).screenLocked)
            m_session->sendScreenUnlock(clientId);

        // Mevcut pencereleri temizle (varsa)
        onRemoteViewStopped(clientId);

        if (m_session) {
            m_session->setClientStreaming(clientId, true);
        }

        // Uzak kontrol için yüksek FPS iste (50ms = ~20fps thumbnail aralığından bağımsız)
        // Önce FPS/kalite ayarını gönderiyoruz ki takeControl geldiğinde ilk kare doğrudan yüksek FPS ile başlasın
        {
            okidia::ScreenConfig cfg;
            cfg.set_interval_ms(50);
            cfg.set_max_width(1920);
            cfg.set_jpeg_quality(85);
            okidia::Envelope fpsEnv;
            fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
            fpsEnv.set_payload(cfg.SerializeAsString());
            m_session->sendCommand(clientId, fpsEnv);
        }

        // Kontrol komutunu gönder — agent frame göndermeye başlasın
        m_session->sendTakeControl(clientId);

        QString hostname = clientId;
        if (m_session) {
            auto st = m_session->clientState(clientId);
            if (!st.hostname.isEmpty()) hostname = st.hostname;
        }

        if (info.monitors_size() <= 1) {
            auto* view = new RemoteControlView(m_session, clientId, this, m_autoControlMode);
            view->setTitle(hostname);
            m_remoteViews.append(view);
            if (m_thumbnails.contains(clientId)) {
                m_thumbnails[clientId]->setStreamingMode(true);
            }
            connect(view, &RemoteControlView::stopped, this, &MainWindow::onRemoteViewStopped);
            connect(view, &RemoteControlView::requestSshTerminal, this, &MainWindow::onOpenSshTerminalForClient);
            view->show();
        } else {
            // Çoklu monitör -> Hepsini aç
            // Acent'a "Tüm monitörleri birleşik gönder" de
            m_session->sendScreenSelectAll(clientId);

            int nativeW = 0;
            int nativeH = 0;
            for (int i = 0; i < info.monitors_size(); ++i) {
                const auto& m = info.monitors(i);
                if (m.x() + static_cast<int>(m.width()) > nativeW) nativeW = m.x() + m.width();
                if (m.y() + static_cast<int>(m.height()) > nativeH) nativeH = m.y() + m.height();
            }
            QSize nativeTotalSize(nativeW, nativeH);

            for (int i = 0; i < info.monitors_size(); ++i) {
                const auto& m = info.monitors(i);
                auto* view = new RemoteControlView(m_session, clientId, this, m_autoControlMode);
                
                QString mName = QString::fromStdString(m.name());
                if (mName.isEmpty()) mName = QString("Ekran %1").arg(i + 1);
                view->setTitle(hostname + " — " + mName);
                
                // Kırpma alanını ayarla
                view->setCropRect(QRect(m.x(), m.y(), m.width(), m.height()), nativeTotalSize);
                
                m_remoteViews.append(view);
                if (m_thumbnails.contains(clientId)) {
                    m_thumbnails[clientId]->setStreamingMode(true);
                }
                connect(view, &RemoteControlView::stopped, this, &MainWindow::onRemoteViewStopped);
            connect(view, &RemoteControlView::requestSshTerminal, this, &MainWindow::onOpenSshTerminalForClient);
                view->show();
            }
        }

        // İlk frame'in hızlı gelmesi için hemen refresh iste
        QTimer::singleShot(100, this, [this, clientId]() {
            if (m_session) m_session->sendRefreshRequest(clientId);
        });

        showCommandStatus(true, clientId + tr(" uzak kontrol başlatıldı"));
        return;
    }

    // Eğer bu istemci için pencereler açıksa monitor listelerini güncelle
    for (auto* view : m_remoteViews) {
        if (view->clientId() == clientId)
            view->updateMonitorList(info);
    }
}

// ============================================================================
// Tree yönetimi — delegasyon TreeManager'a yapılır
// ============================================================================

// filterThumbnailsByNode → rebuildGroupLayout'a yönlendir
