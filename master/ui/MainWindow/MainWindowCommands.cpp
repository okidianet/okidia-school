// master/ui/MainWindow/MainWindowCommands.cpp
#include "MainWindow.h"
#include "../../../common/ui/ThemeManager.h"
#include "ui_MainWindow.h"
#include <QMessageBox>
#include "../../common/utils/Logger.h"
#include "../../audit/AuditLog.h"
#include <QFileDialog>
#include <QInputDialog>
#include "../../common/utils/OkidiaUtils.h"
#include "../BroadcastSourceDialog/BroadcastSourceDialog.h"
#include "../SmartboardConfirmDialog/SmartboardConfirmDialog.h"
void MainWindow::onLockAll() {
    if (!m_session) return;
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) return;

    // Broadcast değil — sadece görünür/seçili istemcilere
    for (const QString& id : targets) {
        m_session->sendInputLock(id);
        m_session->sendScreenLock(id, tr("Öğretmen ekranınızı kilitledi."));
        m_session->setClientScreenLocked(id, true);
        m_session->setClientInputLocked(id, true);
    }
    AuditLog::instance().recordSecurity({}, targets.join(", "), tr("Ekran ve girdi kilitlendi"));
    showCommandStatus(true, QString(tr("%1 ekran kilitlendi")).arg(targets.size()));

    // Node kilit durumunu kaydet
    {
        const QList<QTreeWidgetItem*> tsel = ui->classTree->selectedItems();
        QTreeWidgetItem* node = tsel.isEmpty() ? nullptr : tsel.first();
        if (node && m_treeManager && m_treeManager->nodeType(node) == TreeManager::kTypeClient) {
            node = node->parent();
        }
        const QString nodeId = (node && m_treeManager) ? m_treeManager->publicStableId(node) : "__root__";
        m_nodeLockState[nodeId] = true;
    }

    updateLockToggleState();
}

void MainWindow::onUnlockAll() {
    if (!m_session) return;
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) return;

    Logger::info(QObject::tr("MainWindow: onUnlockAll called for %1 targets").arg(targets.size()));

    for (const QString& id : targets) {
        m_session->sendInputUnlock(id);
        m_session->sendScreenUnlock(id);
        m_session->setClientScreenLocked(id, false);
        m_session->setClientInputLocked(id, false);
        if (m_treeManager) m_treeManager->setClientDevPin(id, QString(), 0);
    }
    AuditLog::instance().recordSecurity({}, targets.join(", "), tr("Ekran ve girdi kilidi açıldı"));
    showCommandStatus(true, QString(tr("%1 ekran kilidi açıldı")).arg(targets.size()));

    // Node kilit durumunu kaydet
    {
        const QList<QTreeWidgetItem*> tsel = ui->classTree->selectedItems();
        QTreeWidgetItem* node = tsel.isEmpty() ? nullptr : tsel.first();
        if (node && m_treeManager && m_treeManager->nodeType(node) == TreeManager::kTypeClient) {
            node = node->parent();
        }
        const QString nodeId = (node && m_treeManager) ? m_treeManager->publicStableId(node) : "__root__";
        m_nodeLockState[nodeId] = false;
    }

    updateLockToggleState();
}

void MainWindow::updateLockToggleState() {
    if (!ui->btnLockToggle) return;

    if (m_activeUser.role == "OBSERVER") {
        ui->btnLockToggle->setEnabled(false);
        ui->btnLockToggle->setToolTip(tr("Gözlemci Modu: Yalnızca canlı izleme yetkiniz vardır."));
        if (ui->btnBroadcast) {
            ui->btnBroadcast->setEnabled(false);
            ui->btnBroadcast->setToolTip(tr("Gözlemci rolündeki kullanıcılar ekran yayını yapamaz."));
        }
        if (ui->btnExam) {
            ui->btnExam->setEnabled(false);
            ui->btnExam->setToolTip(tr("Gözlemci rolündeki kullanıcılar sınav başlatamaz."));
        }
        if (ui->btnPower) {
            ui->btnPower->setEnabled(false);
            ui->btnPower->setToolTip(tr("Gözlemci rolündeki kullanıcılar güç komutu gönderemez."));
        }
        return;
    }

    const QStringList targets = commandTargets();
    bool hasNonIdari = false;
    bool hasStudents = false;
    bool hasBroadcastable = false;
    bool anyLocked = false;

    if (targets.isEmpty()) {
        hasNonIdari = true;
        hasStudents = true;
        hasBroadcastable = true;
    } else {
        for (const QString& id : targets) {
            const auto s = m_session ? m_session->clientState(id) : ClientState{};
            if (s.agentType != 2) {
                hasNonIdari = true;
            }
            if (s.agentType == 1 || s.agentType == 0) {
                hasStudents = true;
                hasBroadcastable = true;
            } else if (s.agentType == 3) {
                hasBroadcastable = true;
            }
            if (s.screenLocked) {
                anyLocked = true;
            }
        }
    }

    // 1. Ekran Kilidi
    ui->btnLockToggle->setEnabled(hasNonIdari);
    if (anyLocked) {
        ui->btnLockToggle->setChecked(true);
        ui->btnLockToggle->setIcon(ThemeManager::icon(":/ui/unlock.svg"));
        ui->btnLockToggle->setText(tr("Kilidi Aç"));
        ui->btnLockToggle->setToolTip(tr("Ekran kilidini aç"));
    } else {
        ui->btnLockToggle->setChecked(false);
        ui->btnLockToggle->setIcon(ThemeManager::icon(":/ui/lock.svg"));
        ui->btnLockToggle->setText(tr("Kilitle"));
        if (!hasNonIdari) {
            ui->btnLockToggle->setToolTip(tr("İdari PC'ler kilitlenemez."));
        } else {
            ui->btnLockToggle->setToolTip(tr("Ekran + klavye/fare kilitle"));
        }
    }

    // 2. Ekran Yayını
    if (ui->btnBroadcast && !(m_broadcaster && m_broadcaster->isRunning())) {
        ui->btnBroadcast->setEnabled(hasBroadcastable);
        if (!hasBroadcastable) {
            ui->btnBroadcast->setToolTip(tr("İdari PC'lere ekran yayını yapılamaz."));
        } else {
            ui->btnBroadcast->setToolTip(tr("Ekran yayını başlat"));
        }
    }

    // 3. Sınav
    if (ui->btnExam) {
        ui->btnExam->setEnabled(hasStudents);
        if (!hasStudents) {
            ui->btnExam->setToolTip(tr("Yalnızca Öğrenci bilgisayarları sınav moduna alınabilir."));
        } else {
            ui->btnExam->setToolTip(tr("Sınav işlemleri"));
        }
    }

    // 4. Güç
    if (ui->btnPower) {
        ui->btnPower->setEnabled(hasNonIdari);
        if (!hasNonIdari) {
            ui->btnPower->setToolTip(tr("İdari PC'ler uzaktan kapatılamaz veya yeniden başlatılamaz."));
        } else {
            ui->btnPower->setToolTip(tr("Güç seçenekleri"));
        }
    }
}

void MainWindow::onLockToggle() {
    // Toggle buton: kilitli değilse kilitle, kilitliyse aç
    // Seçili istemcilerin durumuna bakarak karar ver
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) return;

    bool anyLocked = false;
    for (const QString& id : targets) {
        const auto s = m_session ? m_session->clientState(id) : ClientState{};
        if (s.screenLocked) { anyLocked = true; break; }
    }

    Logger::info(QObject::tr("MainWindow: onLockToggle called, anyLocked = %1, btnChecked = %2")
                 .arg(anyLocked)
                 .arg(ui->btnLockToggle->isChecked()));

    if (anyLocked) onUnlockAll();
    else           onLockAll();
    
    updateLockToggleState();
}
void MainWindow::stopBroadcast() {
    if (m_broadcaster && m_broadcaster->isRunning()) {
        m_broadcaster->stop();
    }
}

void MainWindow::onBroadcast() {
    onBroadcastToggle();
}

void MainWindow::onBroadcastToggle() {
    if (!m_session) return;

    if (m_broadcaster && m_broadcaster->isRunning()) {
        stopBroadcast();
        return;
    }

    if (!m_session->isConnected()) {
        showCommandStatus(false, tr("Sunucu bağlantısı yok"));
        ui->btnBroadcast->setChecked(false);
        return;
    }

    // Gözlemci yayın yapamaz
    if (isObserver()) {
        showCommandStatus(false, tr("Gözlemci rolündeki kullanıcılar ekran yayını yapamaz."));
        ui->btnBroadcast->setChecked(false);
        return;
    }

    // Hedef istemcileri belirle
    QStringList potentialTargets = commandTargets();
    if (potentialTargets.isEmpty()) {
        // Eğer seçili istemci yoksa, yönetebildiği tüm bağlı istemcileri al
        for (auto it = m_thumbnails.constBegin(); it != m_thumbnails.constEnd(); ++it) {
            const QString& cid = it.key();
            if (m_session->canManageClient(cid)) {
                potentialTargets << cid;
            }
        }
    }

    // Kaynak seçim diyaloğunu hedef istemci sayısıyla aç (FPS önerisi için)
    BroadcastSourceDialog dlg(potentialTargets.size(), this);
    if (dlg.exec() != QDialog::Accepted) {
        ui->btnBroadcast->setChecked(false);
        return;
    }

    BroadcastSource src = dlg.selectedSource();

    // Seçilen filtrelere göre hedef istemcileri ele:
    // Öğrenci PC (agentType == 1 veya 0) ve Akıllı Tahta (agentType == 3)
    QStringList finalTargets;
    QMap<QString, QString> targetedSmartboards; // clientId -> display name

    for (const QString& cid : potentialTargets) {
        const ClientState cs = m_session->clientState(cid);
        int aType = cs.agentType;
        if (aType == 3) {
            if (src.targetSmartboards) {
                QString name = cs.hostname.isEmpty() ? cid : cs.hostname;
                if (!cs.username.isEmpty()) name += " (" + cs.username + ")";
                targetedSmartboards.insert(cid, name);
                finalTargets << cid;
            }
        } else if (aType == 2) {
            // İdari PC (Tip 2) KVKK / gizlilik gereği ekran yayınını kabul etmez, atla
            continue;
        } else {
            // Öğrenci veya standart PC
            if (src.targetStudents) {
                finalTargets << cid;
            }
        }
    }

    // Eğer Akıllı Tahta hedeflenmişse ve tahta varsa, öğretmeni uyar ve teyit al
    if (src.targetSmartboards && !targetedSmartboards.isEmpty()) {
        SmartboardConfirmDialog confirmDlg(targetedSmartboards, this);
        if (confirmDlg.exec() != QDialog::Accepted) {
            ui->btnBroadcast->setChecked(false);
            return;
        }
        QStringList confirmedBoards = confirmDlg.selectedClientIds();
        // İşareti kaldırılan tahtaları hedeften çıkar
        for (const QString& boardId : targetedSmartboards.keys()) {
            if (!confirmedBoards.contains(boardId)) {
                finalTargets.removeAll(boardId);
            }
        }
    }

    if (finalTargets.isEmpty()) {
        QMessageBox::information(this, tr("Ekran Yayını"),
            tr("Seçilen hedef kriterlerine (Öğrenci PC / Akıllı Tahta) uygun veya yetkiniz dahilinde olan çevrimiçi istemci bulunamadı."));
        ui->btnBroadcast->setChecked(false);
        return;
    }

    // Bağlantıyı güncelle ve yayını başlat
    if (m_session->tlsClient()) {
        m_broadcaster->setConnection(m_session->tlsClient()->connection());
    }
    m_broadcaster->start(src, finalTargets);
    showCommandStatus(true, QString(tr("Ekran yayını başlatıldı: %1 (%2 FPS, %3 hedef)"))
                      .arg(src.label).arg(src.fps).arg(finalTargets.size()));
}

void MainWindow::onSendFile(bool readOnly) {
    if (!m_session) return;

    if (m_fileSender) {
        QMessageBox::warning(this, tr("Dosya Gönderimi"),
            tr("Hali hazırda devam eden bir dosya transferi var. Lütfen bitmesini bekleyin."));
        return;
    }

    const QStringList targets = commandTargets();
    if (targets.isEmpty()) {
        showCommandStatus(false, tr("Hedef istemci yok"));
        return;
    }

    QString path = OkidiaUtils::getOpenFileName(
        this, tr("Dosya Gönder"), "file_transfer",
        tr("Tüm Dosyalar (*)"));
    if (path.isEmpty()) return;

    QFileInfo info(path);

    // Büyük dosya uyarısı (>50 MB)
    constexpr qint64 WARN_BYTES = 50 * 1024 * 1024;
    if (info.size() > WARN_BYTES) {
        const QString sizeMB = QString::number(info.size() / (1024*1024));
        auto btn = QMessageBox::question(this,
            tr("Büyük Dosya"),
            QString(tr("%1 boyutu %2 MB. Tüm hedef istemcilere göndermek "
                       "ağ trafiğini artırabilir. Devam edilsin mi?"))
                .arg(info.fileName(), sizeMB),
            QMessageBox::Yes | QMessageBox::No);
        if (btn != QMessageBox::Yes) return;
    }

    Connection* conn = m_session->tlsClient() ? m_session->tlsClient()->connection() : nullptr;
    if (!conn || !conn->isConnected()) {
        showCommandStatus(false, tr("Sunucu bağlantısı yok!"));
        return;
    }

    // Multicast hedef formatı: __multicast__:id1,id2,id3...
    QString multicastTarget = "__multicast__:" + targets.join(",");

    m_fileSender = new FileSender(conn, this);
    connect(m_fileSender, &FileSender::progress, this, [this](int pct) {
        showCommandStatus(true, QString(tr("Dosya yükleniyor: %1%")).arg(pct));
    });
    connect(m_fileSender, &FileSender::finished, this, [this, targets, info](bool ok, const QString& msg) {
        if (ok) {
            showCommandStatus(true, QString(tr("Dosya yükleme tamamlandı, dağıtılıyor → %1")).arg(info.fileName()));
            AuditLog::instance().recordFile({}, targets.join(", "), info.fileName());
        } else {
            showCommandStatus(false, tr("Dosya gönderme hatası: ") + msg);
        }
        m_fileSender->deleteLater();
        m_fileSender = nullptr;
    });

    if (!m_fileSender->sendToClient(multicastTarget, path, readOnly)) {
        showCommandStatus(false, tr("Transfer başlatılamadı."));
        m_fileSender->deleteLater();
        m_fileSender = nullptr;
    }
}

void MainWindow::onPowerWakeOnLan() {
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) {
        showCommandStatus(false, tr("Hedef bilgisayar yok"));
        return;
    }
    onClientsWakeOnLan(targets);
}

void MainWindow::onPowerShutdown() {
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) {
        showCommandStatus(false, tr("Hedef bilgisayar yok"));
        return;
    }
    QTimer::singleShot(0, this, [this, targets]() {
        onClientsShutdown(targets);
    });
}

void MainWindow::onPowerRestart() {
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) {
        showCommandStatus(false, tr("Hedef bilgisayar yok"));
        return;
    }
    QTimer::singleShot(0, this, [this, targets]() {
        onClientsRestart(targets);
    });
}

void MainWindow::onPowerLogout() {
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) {
        showCommandStatus(false, tr("Hedef bilgisayar yok"));
        return;
    }
    QTimer::singleShot(0, this, [this, targets]() {
        onClientsLogout(targets);
    });
}

void MainWindow::onPowerHibernate() {
    const QStringList targets = commandTargets();
    if (targets.isEmpty()) {
        showCommandStatus(false, tr("Hedef bilgisayar yok"));
        return;
    }
    QTimer::singleShot(0, this, [this, targets]() {
        onClientsHibernate(targets);
    });
}

void MainWindow::onPowerWakeOnLanAll() {
    if (!m_session) return;
    m_session->broadcastWakeOnLan();
    AuditLog::instance().recordPower({}, tr("Tüm Sınıf"), tr("Tüm sınıfa Wake-on-LAN sinyali gönderildi"));
    showCommandStatus(true, tr("Tüm sınıfa (İdari PC'ler hariç) açma sinyali (Wake-on-LAN) gönderildi"));
}

void MainWindow::onPowerShutdownAll() {
    QTimer::singleShot(0, this, [this]() {
        if (!m_session) return;
        int ret = QMessageBox::question(
            this, tr("Tüm Sınıfı Kapat"),
            tr("Sınıftaki TÜM bilgisayarları kapatmak istediğinize emin misiniz?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;

        m_session->broadcastShutdown(0);
        AuditLog::instance().recordPower({}, tr("Tüm Sınıf"), tr("Tüm sınıfı kapatma komutu verildi"));
        showCommandStatus(true, tr("Tüm sınıf kapatılıyor"));
    });
}


