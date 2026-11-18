#include "SshManagerPanel.h"
#include "SshTemplateDialog.h"
#include "SshCredentialDialog.h"
#include "AccordionLogWidget.h"
#include "../../common/ui/ThemeManager.h"
#include "../session/SessionManager.h"

#include <QMessageBox>
#include <QSettings>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QtConcurrent>
#include <QDateTime>

static ClientState findClientByIpForCmd(SessionManager* session, const QString& ip) {
    if (!session || ip.isEmpty()) return {};
    for (const auto& cs : session->clientStates()) {
        if (cs.ip == ip) return cs;
    }
    return {};
}

void SshManagerPanel::onRunCommand() {
    QString cmd = m_cmdEdit->toPlainText().trimmed();
    if (cmd.isEmpty()) return;

    struct HostInfo {
        QString ip;
        QString hostName;
        QString osTag;
        SshClient::Config cfg;
    };
    QList<HostInfo> hosts;
    QSet<int> rows;
    for(auto* item : m_hostList->selectedItems()) {
        if(item) rows.insert(item->row());
    }

    for (int r : rows) {
        QString ip = m_hostList->item(r, 1)->data(Qt::UserRole).toString();
        QString hostName = m_hostList->item(r, 0)->text();
        QString osTag = (m_hostList->item(r, 2) ? m_hostList->item(r, 2)->data(Qt::UserRole).toString() : "unknown");
        if (osTag == "unknown" && m_session) {
            auto cs = findClientByIpForCmd(m_session, ip);
            if (!cs.osTag.isEmpty()) osTag = cs.osTag.toLower();
        }
        if (!ip.isEmpty()) {
            hosts.append({ip, hostName, osTag, buildConfig(ip)});
        }
    }

    if (hosts.isEmpty()) {
        QMessageBox::information(this, tr("Komut"), tr("Lütfen listeden komut çalıştırılacak hedef istemcileri seçin."));
        return;
    }

    // Eksik SSH bilgisi olan var mı kontrol et
    bool missingCreds = false;
    for (const auto& h : hosts) {
        if (h.cfg.password.isEmpty() && h.cfg.authMethod == SshClient::AuthMethod::Password) {
            missingCreds = true;
            break;
        }
    }

    if (missingCreds) {
        QMessageBox::information(this, tr("Eksik SSH Bilgisi"),
            tr("Seçilen istemcilerden bazılarında kayıtlı SSH parolası bulunamadı.\n"
               "Lütfen önce SSH kimlik bilgilerini tanımlayın."));
        SshCredentialDialog dlg(m_session, this);
        dlg.exec();
        
        // Şifreler girilmiş olabilir, listeyi tekrar oluştur
        hosts.clear();
        for (int r : rows) {
            QString ip = m_hostList->item(r, 1)->data(Qt::UserRole).toString();
            QString hostName = m_hostList->item(r, 0)->text();
            QString osTag = (m_hostList->item(r, 2) ? m_hostList->item(r, 2)->data(Qt::UserRole).toString() : "unknown");
            if (osTag == "unknown" && m_session) {
                auto cs = findClientByIpForCmd(m_session, ip);
                if (!cs.osTag.isEmpty()) osTag = cs.osTag.toLower();
            }
            if (!ip.isEmpty()) {
                auto cfg = buildConfig(ip);
                if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
                    QMessageBox::warning(this, tr("Eksik SSH Bilgisi"), ip + tr(" için geçerli bir SSH şifresi girilmedi. Bu istemci atlanacak."));
                    continue;
                }
                hosts.append({ip, hostName, osTag, cfg});
            }
        }
        
        if (hosts.isEmpty()) return;
    }

    // Temizle
    QLayoutItem* item;
    while ((item = m_cmdLogLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    // Hedef OS filtre kontrolü
    QString targetFilter = m_targetOsFilterCombo ? m_targetOsFilterCombo->currentData().toString() : "all";
    QList<HostInfo> execHosts;

    for (const auto& h : hosts) {
        bool isWin = h.osTag.contains("win");
        bool isLinux = h.osTag.contains("linux") || h.osTag.contains("pardus") || h.osTag.contains("ubuntu") || h.osTag.contains("debian");

        if (targetFilter == "linux" && isWin) {
            auto* acc = new AccordionLogWidget(QString("%1 - %2 [Windows]").arg(h.hostName, h.ip), this);
            m_cmdLogLayout->addWidget(acc);
            acc->appendLog(tr("⏭ Atlandı: Komut hedef filtresi 'Sadece Linux' olarak belirlendi."));
            acc->setStatus(1);
            continue;
        }
        if (targetFilter == "windows" && isLinux) {
            auto* acc = new AccordionLogWidget(QString("%1 - %2 [Linux]").arg(h.hostName, h.ip), this);
            m_cmdLogLayout->addWidget(acc);
            acc->appendLog(tr("⏭ Atlandı: Komut hedef filtresi 'Sadece Windows' olarak belirlendi."));
            acc->setStatus(1);
            continue;
        }
        execHosts.append(h);
    }

    if (execHosts.isEmpty()) {
        auto* acc = new AccordionLogWidget(tr("Sonuç"), this);
        m_cmdLogLayout->addWidget(acc);
        acc->appendLog(tr("[UYARI] Seçili filtreye uyan hiçbir istemci bulunamadı."));
        acc->setStatus(2);
        return;
    }

    // 1. Önceki çalışan/askıda kalan tüm oturumları tamamen iptal et ve temizle (Clean Abort)
    const uint64_t currentEpoch = ++m_cmdExecutionEpoch;
    {
        QMutexLocker lock(&m_runningCmdMutex);
        for (auto clientPtr : m_runningCmdClients) {
            if (clientPtr) {
                clientPtr->disconnectFromHost();
            }
        }
        m_runningCmdClients.clear();
    }

    m_runBtn->setEnabled(false);

    bool isNohup = m_cmdNohupCheck ? m_cmdNohupCheck->isChecked() : false;
    int concurrencyLimit = (m_cmdSpeedCombo && m_cmdSpeedCombo->currentData().toInt() > 0)
        ? m_cmdSpeedCombo->currentData().toInt() : 4;
    auto poolSemaphore = std::make_shared<QSemaphore>(concurrencyLimit);
    std::shared_ptr<QAtomicInt> pendingTasks = std::make_shared<QAtomicInt>(execHosts.size());

    for (const auto& h : execHosts) {
        auto* acc = new AccordionLogWidget(QString("%1 - %2").arg(h.hostName, h.ip), this);
        m_cmdLogLayout->addWidget(acc);

        std::ignore = QtConcurrent::run([this, h, cmd, isNohup, pendingTasks, acc, poolSemaphore, currentEpoch]() {
            // İptal kontrolü: Eğer bu işlem çalışana kadar yeni bir komut tetiklendiyse dur
            if (m_cmdExecutionEpoch.load() != currentEpoch) {
                QMetaObject::invokeMethod(this, [this, acc, pendingTasks]() {
                    acc->appendLog(tr("⏹ İptal Edildi: Yeni bir komut başlatıldığı için önceki istek iptal edildi.\n"));
                    acc->setStatus(2);
                    if (pendingTasks->deref() == 0) {
                        m_runBtn->setEnabled(true);
                    }
                }, Qt::QueuedConnection);
                return;
            }

            // Hızlı Pre-Flight TCP Probe (750 ms): Kapalı veya erişilemeyen cihazlar SSH yükleme/timeout beklemesin
            {
                QTcpSocket probeSock;
                probeSock.connectToHost(h.ip, h.cfg.port > 0 ? h.cfg.port : 22);
                bool portOpen = probeSock.waitForConnected(750);
                probeSock.disconnectFromHost();

                if (!portOpen) {
                    QMetaObject::invokeMethod(this, [this, acc, pendingTasks]() {
                        acc->appendLog(tr("⏭ Cihaz kapalı veya SSH portuna erişilemiyor (750 ms pre-flight zaman aşımı). Atlandı.\n"));
                        acc->setStatus(2);
                        if (pendingTasks->deref() == 0) {
                            m_runBtn->setEnabled(true);
                        }
                    }, Qt::QueuedConnection);
                    return;
                }
            }

            // Wi-Fi Airtime darboğazını önlemek için concurrency limiti bekle
            poolSemaphore->acquire();

            if (m_cmdExecutionEpoch.load() != currentEpoch) {
                poolSemaphore->release();
                QMetaObject::invokeMethod(this, [this, acc, pendingTasks]() {
                    acc->appendLog(tr("⏹ İptal Edildi: Yeni bir komut başlatıldığı için önceki istek iptal edildi.\n"));
                    acc->setStatus(2);
                    if (pendingTasks->deref() == 0) {
                        m_runBtn->setEnabled(true);
                    }
                }, Qt::QueuedConnection);
                return;
            }

            auto client = std::make_shared<SshClient>();
            {
                QMutexLocker lock(&m_runningCmdMutex);
                if (m_cmdExecutionEpoch.load() == currentEpoch) {
                    m_runningCmdClients.append(client.get());
                } else {
                    poolSemaphore->release();
                    return;
                }
            }

            client->connectToHost(h.cfg);

            QString result;
            bool success = false;
            
            if (client->isConnected()) {
                QString finalCmd = cmd;
                if (!h.osTag.contains("win")) {
                    if (isNohup) {
                        QString escapedCmd = cmd;
                        escapedCmd.replace("'", "'\\''");
                        QString nohupLog = QString("/tmp/okidia_cmd_%1.log").arg(QDateTime::currentSecsSinceEpoch());
                        if (cmd.contains("sudo") && h.cfg.username != "root" && !h.cfg.password.isEmpty()) {
                            finalCmd = QString("echo '%1' | sudo -S -p '' bash -c 'export DEBIAN_FRONTEND=noninteractive; nohup %2 > %3 2>&1 & disown'")
                                        .arg(h.cfg.password)
                                        .arg(escapedCmd)
                                        .arg(nohupLog);
                        } else {
                            finalCmd = QString("export DEBIAN_FRONTEND=noninteractive; nohup bash -c '%1' > %2 2>&1 & disown")
                                        .arg(escapedCmd)
                                        .arg(nohupLog);
                        }
                    } else if (finalCmd.contains("sudo") && h.cfg.username != "root" && !h.cfg.password.isEmpty()) {
                        QString escapedCmd = cmd;
                        escapedCmd.replace("'", "'\\''");
                        finalCmd = QString("echo '%1' | sudo -S -p '' bash -c 'export DEBIAN_FRONTEND=noninteractive; %2 </dev/null'")
                                    .arg(h.cfg.password)
                                    .arg(escapedCmd);
                    } else {
                        finalCmd = QString("export DEBIAN_FRONTEND=noninteractive; %1 </dev/null").arg(cmd);
                    }
                }

                // Canlı yayın için callback tanımla
                int exitCode = 0;
                QString out = client->executeSync(finalCmd, isNohup ? 10000 : 600000, &exitCode, [acc](const QByteArray& data) {
                    QString text = QString::fromUtf8(data);
                    QMetaObject::invokeMethod(acc, [acc, text]() {
                        acc->appendLog(text);
                    }, Qt::QueuedConnection);
                }, [this, currentEpoch]() -> bool {
                    return m_cmdExecutionEpoch.load() != currentEpoch;
                });

                if (isNohup) {
                    if (out.trimmed().isEmpty()) {
                        result = tr("✓ Komut uzak makinede arka planda (nohup) kesintisiz olarak başlatıldı.");
                    } else {
                        result = out.trimmed() + "\n" + tr("✓ Arka plan işlemi aktif olarak yürütülüyor.");
                    }
                }
                
                success = (exitCode == 0);
                client->disconnectFromHost();
            } else {
                result = tr("[HATA] ") + client->lastError();
                success = false;
            }

            {
                QMutexLocker lock(&m_runningCmdMutex);
                m_runningCmdClients.removeAll(client.get());
            }

            poolSemaphore->release();

            QMetaObject::invokeMethod(this, [this, acc, result, success, pendingTasks]() {
                if (!result.isEmpty()) {
                    acc->appendLog(result + "\n");
                }
                acc->setStatus(success ? 1 : 2);
                
                if (pendingTasks->deref() == 0) {
                    m_runBtn->setEnabled(true);
                }
            }, Qt::QueuedConnection);
        });
    }
}

void SshManagerPanel::onSaveTemplate() {
    QString cmd = m_cmdEdit->toPlainText().trimmed();
    if (cmd.isEmpty()) return;

    QDialog d(this);
    d.setWindowTitle(tr("Şablon Olarak Kaydet"));
    auto* form = new QFormLayout(&d);

    auto* nameEdit = new QLineEdit(&d);
    nameEdit->setPlaceholderText(tr("Örn: Disk Alanını Kontrol Et"));

    auto* osCombo = new QComboBox(&d);
    osCombo->addItem(ThemeManager::icon(":/ui/group.svg"), tr("Tümü (Genel)"), "all");
    osCombo->addItem(ThemeManager::icon(":/ui/linux.svg"), tr("Linux"), "linux");
    osCombo->addItem(ThemeManager::icon(":/ui/windows.svg"), tr("Windows"), "windows");

    if (m_targetOsFilterCombo) {
        QString currentFilter = m_targetOsFilterCombo->currentData().toString();
        int idx = osCombo->findData(currentFilter);
        if (idx >= 0) osCombo->setCurrentIndex(idx);
    }

    auto* nohupCheck = new QCheckBox(tr("Arka Planda Sürdür (nohup)"), &d);
    if (m_cmdNohupCheck) {
        nohupCheck->setChecked(m_cmdNohupCheck->isChecked());
    }

    form->addRow(tr("Şablon Adı:"), nameEdit);
    form->addRow(tr("Hedef OS:"), osCombo);
    form->addRow(tr("Çalışma Modu:"), nohupCheck);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(btns);
    connect(btns, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &d, &QDialog::reject);

    if (d.exec() == QDialog::Accepted) {
        QString name = nameEdit->text().trimmed();
        QString targetOs = osCombo->currentData().toString();
        bool isNohup = nohupCheck->isChecked();
        if (!name.isEmpty()) {
            QSettings s("okidia", "master");
            int size = s.beginReadArray("ssh_cmd_templates");
            s.endArray();
            
            s.beginWriteArray("ssh_cmd_templates");
            s.setArrayIndex(size);
            s.setValue("name", name);
            s.setValue("target_os", targetOs);
            s.setValue("cmd", cmd);
            s.setValue("nohup", isNohup);
            s.endArray();
            
            loadTemplates();
            m_cmdPresetCombo->setCurrentIndex(m_cmdPresetCombo->count() - 1);
        }
    }
}

void SshManagerPanel::onManageTemplates() {
    SshTemplateDialog dlg(this);
    connect(&dlg, &SshTemplateDialog::templatesChanged, this, &SshManagerPanel::loadTemplates);
    dlg.exec();
}

void SshManagerPanel::loadTemplates() {
    m_cmdPresetCombo->blockSignals(true);
    m_cmdPresetCombo->clear();
    m_cmdPresetCombo->addItem(tr("Özel Komut (El ile Yaz)"), "");
    m_cmdPresetCombo->setItemData(0, "all", Qt::UserRole + 1);
    m_cmdPresetCombo->setItemData(0, false, Qt::UserRole + 2);
    
    QString filterOs = m_tplOsFilterCombo ? m_tplOsFilterCombo->currentData().toString() : "all";

    QSettings s("okidia", "master");
    int version = s.value("ssh_cmd_templates_version", 0).toInt();
    int size = s.beginReadArray("ssh_cmd_templates");
    
    if (version < 3 || size == 0) {
        s.endArray();
        s.beginWriteArray("ssh_cmd_templates");
        struct Tpl { QString name; QString target_os; QString cmd; bool nohup; };
        QList<Tpl> defaults = {
            // Linux Şablonları
            {tr("Okidia İstemci Servisini Yeniden Başlat"), "linux", "sudo systemctl restart okidia-agent", true},
            {tr("Sistem Yeniden Başlat (Reboot)"), "linux", "sudo reboot", true},
            {tr("Cihazı Kapat (Shutdown)"), "linux", "sudo shutdown -h now", true},
            {tr("Sistem Durumu ve İstemci Sürümünü Sorgula"), "linux", "systemctl is-active okidia-agent && dpkg-query -W -f='${Version}\\n' okidia-agent", false},
            {tr("Disk ve Bellek Kullanımı"), "linux", "df -h / && free -m", false},
            // Windows Şablonları
            {tr("Okidia İstemci Servisini Yeniden Başlat"), "windows", "powershell -Command \"Restart-Service OkidiaAgent\"", true},
            {tr("Sistem Yeniden Başlat (Reboot)"), "windows", "shutdown /r /t 0", true},
            {tr("Cihazı Kapat (Shutdown)"), "windows", "shutdown /s /t 0", true},
            {tr("Okidia Servis Durumunu Sorgula"), "windows", "sc query OkidiaAgent", false},
            {tr("Sistem Bilgisi ve Uptime"), "windows", "systeminfo | findstr /B /C:\"OS Name\" /C:\"System Boot Time\"", false},
            {tr("Disk ve Bellek Durumu"), "windows", "powershell -Command \"Get-PSDrive C; Get-CimInstance Win32_OperatingSystem | Select-Object TotalVisibleMemorySize, FreePhysicalMemory\"", false},
            // Genel Şablonlar
            {tr("Hostname ve Aktif Kullanıcı"), "all", "hostname && whoami", false},
            {tr("İnternet Bağlantı Testi (Ping)"), "all", "ping -c 2 8.8.8.8 || ping -n 2 8.8.8.8", false}
        };
        for (int i = 0; i < defaults.size(); ++i) {
            s.setArrayIndex(i);
            s.setValue("name", defaults[i].name);
            s.setValue("target_os", defaults[i].target_os);
            s.setValue("cmd", defaults[i].cmd);
            s.setValue("nohup", defaults[i].nohup);
        }
        s.endArray();
        s.setValue("ssh_cmd_templates_version", 3);
        
        size = s.beginReadArray("ssh_cmd_templates");
    }
    
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        QString name = s.value("name").toString();
        QString targetOs = s.value("target_os", "all").toString().toLower();
        QString cmd = s.value("cmd").toString();
        bool isNohup = s.value("nohup", false).toBool();

        if (name.contains(tr("Sıfırla")) || cmd.contains("machine_id") || cmd.contains("machine-id")) {
            continue;
        }

        if (filterOs == "linux" && targetOs == "windows") continue;
        if (filterOs == "windows" && targetOs == "linux") continue;

        QIcon itemIcon = ThemeManager::icon(":/ui/settings.svg");
        if (targetOs == "linux") itemIcon = ThemeManager::icon(":/ui/linux.svg");
        else if (targetOs == "windows") itemIcon = ThemeManager::icon(":/ui/windows.svg");

        m_cmdPresetCombo->addItem(itemIcon, name, cmd);
        m_cmdPresetCombo->setItemData(m_cmdPresetCombo->count() - 1, targetOs, Qt::UserRole + 1);
        m_cmdPresetCombo->setItemData(m_cmdPresetCombo->count() - 1, isNohup, Qt::UserRole + 2);
    }
    s.endArray();
    m_cmdPresetCombo->blockSignals(false);
}
