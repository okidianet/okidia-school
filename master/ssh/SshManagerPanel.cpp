// master/ssh/SshManagerPanel.cpp
#include "SshManagerPanel.h"
#include "SshCredentialDialog.h"
#include "SshCredStore.h"
#include "SshFileManagerWidget.h"
#include "SshCredSelectorWidget.h"
#include "SshTerminalWidget.h"
#include "SshDeviceSessionWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QHeaderView>
#include <QHostAddress>
#include <QWheelEvent>
#include <QMenu>
#include <QClipboard>
#include <QGuiApplication>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QThread>

#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../session/SessionManager.h"

SshManagerPanel::SshManagerPanel(SessionManager* session, QWidget* parent)
    : QWidget(parent), m_session(session)
{
    m_lastLocalPath = OkidiaUtils::getLastDirectory("ssh_sftp_local", QDir::homePath());
    setupUi();
    loadSettings();
    
    if (m_session) {
        m_session->fetchSshCredentials();
        connect(m_session, &SessionManager::hostOsUpdated, this, &SshManagerPanel::updateHostRowOs);
    }
}

SshManagerPanel::~SshManagerPanel() {
    saveSettings();
    for (auto* client : std::as_const(m_activeClients)) {
        client->disconnectFromHost();
        client->deleteLater();
    }
    m_activeClients.clear();
}

void SshManagerPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_session) {
        m_session->fetchSshCredentials();
    }
}

bool SshManagerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (m_activeTabs && watched == m_activeTabs->tabBar()) {
        if (event->type() == QEvent::Wheel) {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            int numDegrees = wheelEvent->angleDelta().y() / 8;
            int numSteps = numDegrees / 15;
            if (numSteps != 0) {
                int newIndex = m_activeTabs->currentIndex() - numSteps;
                if (newIndex >= 0 && newIndex < m_activeTabs->count()) {
                    m_activeTabs->setCurrentIndex(newIndex);
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SshManagerPanel::saveSettings() {
    // nothing to save for now
}

void SshManagerPanel::loadSettings() {
    // nothing to load for now
}

void SshManagerPanel::appendLog(const QString& msg) {
    Logger::info(QObject::tr("SshManager: ") + msg);
}

SshClient::Config SshManagerPanel::buildConfig(const QString& ip) const {
    SshClient::Config cfg;
    cfg.host = ip;
    cfg.port = 22;
    cfg.connectTimeoutMs = 4000;
    cfg.hostKeyCheck = false;

    QString user, pass;
    bool isKey = false;
    bool found = false;

    if (m_session) {
        const auto brokerCreds = m_session->sshCredentials();
        const auto connectedAgents = m_session->allClientStates();
        
        // IP bazlı machineId ve classId'leri bul
        QString machineId, classId;
        for (const auto& agent : connectedAgents) {
            if (agent.ip == ip) {
                machineId = agent.machineId;
                classId = m_session->clientGroup(machineId);
                break;
            }
        }

        // Tablodaki hostname bilgisini bul (varsa)
        QString hostNameFromTable;
        if (m_hostList) {
            for (int r = 0; r < m_hostList->rowCount(); ++r) {
                if (m_hostList->item(r, 1) && m_hostList->item(r, 1)->data(Qt::UserRole).toString() == ip) {
                    if (m_hostList->item(r, 0)) {
                        hostNameFromTable = m_hostList->item(r, 0)->text().trimmed();
                    }
                    break;
                }
            }
        }

        // Öncelik Sırası: IP Eşleşmesi -> MachineID Eşleşmesi -> Hostname Eşleşmesi -> Sınıf/Grup Eşleşmesi
        for (const auto& c : brokerCreds) {
            if (c.targetId == ip && c.targetType == "agent") {
                user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
            }
        }
        if (!found && !machineId.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if ((c.targetId == machineId || c.machineId == machineId) && c.targetType == "agent") {
                    user = c.username; pass = QString::fromUtf8(c.secret); isKey = c.isKey; found = true; break;
                }
            }
        }
        if (!found && !hostNameFromTable.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if (c.targetType == "agent" && (c.targetId == hostNameFromTable || c.name.contains(hostNameFromTable, Qt::CaseInsensitive))) {
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
    }

    if (found) {
        cfg.username = user;
        if (isKey) {
            cfg.authMethod = SshClient::AuthMethod::PrivateKey;
            cfg.privateKeyPath = pass;
            cfg.password.clear();
        } else {
            cfg.authMethod = SshClient::AuthMethod::Password;
            cfg.password = pass;
            cfg.privateKeyPath.clear();
        }
    } else {
        cfg.username = "root";
        cfg.authMethod = SshClient::AuthMethod::Password;
        cfg.password = "";
    }

    return cfg;
}

// ----------------------------------------------------------------------------
// SSH Cihaz & Kimlik İşlemleri
// ----------------------------------------------------------------------------
void SshManagerPanel::onAddDevice() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Cihaz Ekle & Doğrula"));
    dlg.setFixedWidth(440);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(12);

    auto* formLay = new QFormLayout();
    formLay->setSpacing(10);

    auto* targetEdit = new QLineEdit(&dlg);
    targetEdit->setPlaceholderText(tr("Örn: 192.168.1.104 veya pardus-server"));

    auto* displayNameEdit = new QLineEdit(&dlg);
    displayNameEdit->setPlaceholderText(tr("İsteğe bağlı görünüm adı (boşsa otomatik)"));

    formLay->addRow(tr("Hedef (IP veya Hostname):"), targetEdit);
    formLay->addRow(tr("Cihaz Adı:"), displayNameEdit);
    lay->addLayout(formLay);

    auto* credGroup = new QGroupBox(tr("SSH Kimlik Bilgileri"), &dlg);
    auto* credLay = new QVBoxLayout(credGroup);
    auto* credSelector = new SshCredSelectorWidget(m_session, credGroup);
    credSelector->loadSettings("SshManagerPanel");
    credLay->addWidget(credSelector);
    lay->addWidget(credGroup);

    auto* statusLbl = new QLabel(&dlg);
    statusLbl->setWordWrap(true);
    statusLbl->setStyleSheet("color: gray;");
    lay->addWidget(statusLbl);

    auto* btnRow = new QHBoxLayout();
    auto* verifyAddBtn = new QPushButton(ThemeManager::icon(":/ui/refresh.svg"), tr("Doğrula ve Ekle"), &dlg);
    verifyAddBtn->setProperty("class", "primary");

    auto* directAddBtn = new QPushButton(tr("Doğrulamadan Ekle"), &dlg);
    directAddBtn->setProperty("class", "secondary");

    auto* cancelBtn = new QPushButton(tr("İptal"), &dlg);

    btnRow->addWidget(verifyAddBtn);
    btnRow->addWidget(directAddBtn);
    btnRow->addWidget(cancelBtn);
    lay->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    // Ortak ekleme ve kimlik kaydetme lambda fonksiyonu
    auto finalizeAdd = [&](const QString& targetAddress, const QString& finalHostname, const QString& osTag) {
        credSelector->saveSettings("SshManagerPanel");
        QString user = credSelector->username().trimmed();
        QString pass = credSelector->secret();
        bool isKey = credSelector->isPrivateKey();

        if (m_session && !user.isEmpty()) {
            SshCredentialRecord rec;
            rec.id = OkidiaUtils::generateUuid();
            if (credSelector->currentCredentialId().isEmpty()) {
                rec.name = "Manuel: " + finalHostname;
            } else {
                QString tplName = tr("Şablon");
                for (const auto& c : m_session->sshCredentials()) {
                    if (c.id == credSelector->currentCredentialId()) {
                        tplName = c.name;
                        break;
                    }
                }
                rec.name = tr("Şablon (") + tplName + "): " + finalHostname;
            }
            rec.username = user;
            rec.secret = pass.toUtf8();
            rec.targetId = targetAddress;
            rec.targetType = "agent";
            rec.isKey = isKey;
            rec.ownerUserId = m_session->activeUser().id;
            rec.createdAt = OkidiaUtils::nowSecs();

            m_session->addSshCredential(rec);
        }

        for (int i = 0; i < m_hostList->rowCount(); ++i) {
            if (m_hostList->item(i, 1) && m_hostList->item(i, 1)->data(Qt::UserRole).toString() == targetAddress) {
                m_hostList->selectRow(i);
                dlg.accept();
                return;
            }
        }

        m_hostList->setSortingEnabled(false);
        int r = m_hostList->rowCount();
        m_hostList->insertRow(r);

        auto* nameItem = new OkidiaTableItem(finalHostname);
        nameItem->setData(Qt::UserRole, targetAddress);
        auto* ipItem = new OkidiaTableItem(targetAddress);
        ipItem->setData(Qt::UserRole, targetAddress);
        bool ok = false;
        quint32 ipv4 = QHostAddress(targetAddress).toIPv4Address(&ok);
        if (ok) {
            ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(ipv4));
        }

        QString normalizedOs = osTag.toLower();
        if (normalizedOs == "auto" || normalizedOs == "unknown") {
            normalizedOs = resolveHostOs(targetAddress, finalHostname);
        }
        QString osDisplay = tr("Bilinmeyen");
        QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
        if (normalizedOs.contains("win")) {
            normalizedOs = "windows";
            osDisplay = tr("Windows");
            osIcon = ThemeManager::icon(":/ui/windows.svg");
        } else if (normalizedOs.contains("linux") || normalizedOs.contains("ubuntu") || normalizedOs.contains("pardus") || normalizedOs.contains("debian")) {
            normalizedOs = "linux";
            osDisplay = tr("Linux");
            osIcon = ThemeManager::icon(":/ui/linux.svg");
        }

        auto* osItem = new OkidiaTableItem(osIcon, "");
        osItem->setTextAlignment(Qt::AlignCenter);
        osItem->setToolTip(osDisplay);
        osItem->setData(Qt::UserRole, normalizedOs);

        m_hostList->setItem(r, 0, nameItem);
        m_hostList->setItem(r, 1, ipItem);
        m_hostList->setItem(r, 2, osItem);

        m_hostList->setSortingEnabled(true);
        m_hostList->sortByColumn(m_hostList->horizontalHeader()->sortIndicatorSection(),
                                 m_hostList->horizontalHeader()->sortIndicatorOrder());

        if (normalizedOs == "unknown") {
            asyncResolveHostOs(targetAddress);
        }

        for (int i = 0; i < m_hostList->rowCount(); ++i) {
            if (m_hostList->item(i, 1) && m_hostList->item(i, 1)->data(Qt::UserRole).toString() == targetAddress) {
                m_hostList->selectRow(i);
                break;
            }
        }

        appendLog(QString(tr("Yeni cihaz eklendi: %1 (%2)")).arg(finalHostname, targetAddress));
        dlg.accept();
    };

    // Doğrudan ekle
    connect(directAddBtn, &QPushButton::clicked, [&]() {
        QString input = targetEdit->text().trimmed();
        if (input.isEmpty()) {
            QMessageBox::warning(&dlg, tr("Eksik Bilgi"), tr("Lütfen hedef IP veya makine adı (hostname) girin."));
            return;
        }
        QString disp = displayNameEdit->text().trimmed();
        if (disp.isEmpty()) disp = input;
        finalizeAdd(input, disp, "unknown");
    });

    // Doğrula ve Ekle
    connect(verifyAddBtn, &QPushButton::clicked, [&]() {
        QString input = targetEdit->text().trimmed();
        if (input.isEmpty()) {
            QMessageBox::warning(&dlg, tr("Eksik Bilgi"), tr("Lütfen hedef IP veya makine adı (hostname) girin."));
            return;
        }

        verifyAddBtn->setEnabled(false);
        directAddBtn->setEnabled(false);
        statusLbl->setText(tr("Bağlantı ve cihaz kimliği doğrulanıyor (SSH probe)..."));
        qApp->processEvents();

        SshClient::Config cfg;
        cfg.host = input;
        cfg.port = 22;
        cfg.connectTimeoutMs = 2500;
        cfg.hostKeyCheck = false;
        cfg.username = credSelector->username().trimmed();
        if (credSelector->isPrivateKey()) {
            cfg.authMethod = SshClient::AuthMethod::PrivateKey;
            cfg.privateKeyPath = credSelector->secret();
        } else {
            cfg.authMethod = SshClient::AuthMethod::Password;
            cfg.password = credSelector->secret();
        }

        auto probeFuture = QtConcurrent::run([cfg]() -> QPair<bool, QStringList> {
            SshClient client;
            client.connectToHost(cfg);
            if (!client.isConnected()) {
                return {false, {client.lastError()}};
            }
            QString outUname = client.executeSync("uname -s -n 2>/dev/null", 2000).trimmed();
            QString outOsRelease = client.executeSync("cat /etc/os-release 2>/dev/null | grep '^ID='", 1500).trimmed();
            client.disconnectFromHost();
            return {true, {outUname, outOsRelease}};
        });

        QElapsedTimer timer;
        timer.start();
        while (!probeFuture.isFinished() && timer.elapsed() < 4500) {
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents, 50);
            QThread::msleep(20);
        }

        verifyAddBtn->setEnabled(true);
        directAddBtn->setEnabled(true);

        if (!probeFuture.isFinished() || !probeFuture.result().first) {
            QString err = probeFuture.isFinished() ? probeFuture.result().second.value(0) : tr("Zaman aşımı");
            statusLbl->setText(tr("Doğrulama başarısız: %1").arg(err));
            auto res = QMessageBox::question(&dlg, tr("Doğrulama Başarısız"),
                tr("Hedef cihaza SSH bağlantısı kurulamadı:\n%1\n\nCihazı yine de hedef listesine eklemek istiyor musunuz?").arg(err),
                QMessageBox::Yes | QMessageBox::No);
            if (res == QMessageBox::Yes) {
                QString disp = displayNameEdit->text().trimmed();
                if (disp.isEmpty()) disp = input;
                finalizeAdd(input, disp, "unknown");
            }
            return;
        }

        auto resList = probeFuture.result().second;
        QString unameOut = resList.value(0);
        QString osRelOut = resList.value(1);

        QString detectedHostname = input;
        QString detectedOs = "linux";

        QStringList parts = unameOut.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            if (parts[0].toLower().contains("linux")) {
                detectedOs = "linux";
            }
            detectedHostname = parts[1].trimmed();
        } else if (!unameOut.isEmpty()) {
            detectedHostname = unameOut;
        }

        if (osRelOut.toLower().contains("windows") || unameOut.toLower().contains("mingw") || unameOut.toLower().contains("msys")) {
            detectedOs = "windows";
        }

        QString userDisp = displayNameEdit->text().trimmed();
        QString finalHost = userDisp.isEmpty() ? detectedHostname : userDisp;

        statusLbl->setText(tr("Doğrulandı! Hostname: %1, İS: %2").arg(detectedHostname, detectedOs));
        finalizeAdd(input, finalHost, detectedOs);
    });

    dlg.exec();
}

void SshManagerPanel::onRefreshDevice(int row) {
    if (row < 0) {
        row = m_hostList->currentRow();
    }
    if (row < 0 || row >= m_hostList->rowCount()) return;

    QString currentHost = m_hostList->item(row, 0) ? m_hostList->item(row, 0)->text() : "";
    QString ip = m_hostList->item(row, 1) ? m_hostList->item(row, 1)->data(Qt::UserRole).toString() : "";
    if (ip.isEmpty()) return;

    appendLog(tr("Cihaz bilgileri güncelleniyor: %1 (%2)...").arg(currentHost, ip));

    auto cfg = buildConfig(ip);
    cfg.connectTimeoutMs = 2500;

    (void)QtConcurrent::run([this, row, ip, currentHost, cfg]() {
        SshClient client;
        client.connectToHost(cfg);
        if (!client.isConnected()) {
            QMetaObject::invokeMethod(this, [this, ip, err = client.lastError()]() {
                appendLog(tr("Cihaz bilgisi güncelleme başarısız (%1): %2").arg(ip, err));
            }, Qt::QueuedConnection);
            return;
        }

        QString outUname = client.executeSync("uname -s -n 2>/dev/null", 2000).trimmed();
        QString outOsRelease = client.executeSync("cat /etc/os-release 2>/dev/null | grep '^ID='", 1500).trimmed();
        client.disconnectFromHost();

        QString detectedHostname = currentHost;
        QString detectedOs = "linux";
        QStringList parts = outUname.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            detectedHostname = parts[1].trimmed();
        } else if (!outUname.isEmpty()) {
            detectedHostname = outUname;
        }
        if (outOsRelease.toLower().contains("windows") || outUname.toLower().contains("mingw") || outUname.toLower().contains("msys")) {
            detectedOs = "windows";
        }

        QMetaObject::invokeMethod(this, [this, row, ip, detectedHostname, detectedOs]() {
            if (row < m_hostList->rowCount() && m_hostList->item(row, 1) && m_hostList->item(row, 1)->data(Qt::UserRole).toString() == ip) {
                if (m_hostList->item(row, 0)) {
                    m_hostList->item(row, 0)->setText(detectedHostname);
                }
                updateHostRowOs(ip, detectedOs);
                if (m_session) {
                    m_session->setHostOs(ip, detectedOs);
                }
                appendLog(tr("Cihaz bilgisi güncellendi: %1 (%2) [%3]").arg(detectedHostname, ip, detectedOs));
            }
        }, Qt::QueuedConnection);
    });
}

void SshManagerPanel::onHostContextMenu(const QPoint& pos) {
    int row = m_hostList->rowAt(pos.y());
    if (row < 0) return;

    if (!m_hostList->item(row, 0)->isSelected()) {
        m_hostList->selectRow(row);
    }

    QString hostname = m_hostList->item(row, 0) ? m_hostList->item(row, 0)->text() : "";
    QString ip = m_hostList->item(row, 1) ? m_hostList->item(row, 1)->data(Qt::UserRole).toString() : "";

    QMenu menu(this);

    auto* connectAct = menu.addAction(ThemeManager::icon(":/ui/monitor.svg"), tr("Bağlan (Terminal)"));
    auto* sftpAct = menu.addAction(ThemeManager::icon(":/ui/file.svg"), tr("Dosya Yöneticisi Olarak Aç"));
    menu.addSeparator();

    auto* editCredAct = menu.addAction(ThemeManager::icon(":/ui/key.svg"), tr("SSH Bilgilerini Düzenle..."));
    auto* refreshAct = menu.addAction(ThemeManager::icon(":/ui/refresh.svg"), tr("Cihaz Bilgilerini Güncelle (Yenile)"));
    menu.addSeparator();

    auto* copyIpAct = menu.addAction(ThemeManager::icon(":/ui/copy.svg"), tr("IP Adresini Kopyala (%1)").arg(ip));
    auto* copyHostAct = menu.addAction(ThemeManager::icon(":/ui/copy.svg"), tr("Cihaz Adını Kopyala (%1)").arg(hostname));
    menu.addSeparator();

    auto* removeAct = menu.addAction(ThemeManager::icon(":/ui/close.svg"), tr("Listeden Kaldır"));
    auto* delCredAct = menu.addAction(ThemeManager::icon(":/ui/delete.svg"), tr("Kayıtlı SSH Kimliğini Sil"));

    connect(connectAct, &QAction::triggered, this, &SshManagerPanel::onOpenTerminal);
    connect(sftpAct, &QAction::triggered, this, &SshManagerPanel::onOpenFileManager);
    connect(editCredAct, &QAction::triggered, this, &SshManagerPanel::onAssignCredentials);
    connect(refreshAct, &QAction::triggered, this, [this, row]() { onRefreshDevice(row); });

    connect(copyIpAct, &QAction::triggered, this, [ip]() {
        QGuiApplication::clipboard()->setText(ip);
    });
    connect(copyHostAct, &QAction::triggered, this, [hostname]() {
        QGuiApplication::clipboard()->setText(hostname);
    });

    connect(removeAct, &QAction::triggered, this, [this, row]() {
        if (row >= 0 && row < m_hostList->rowCount()) {
            m_hostList->removeRow(row);
        }
    });

    connect(delCredAct, &QAction::triggered, this, [this, ip, hostname]() {
        if (!m_session) return;
        QString credId;
        for (const auto& c : m_session->sshCredentials()) {
            if (c.targetId == ip || c.targetId == hostname) {
                credId = c.id;
                break;
            }
        }
        if (credId.isEmpty()) {
            QMessageBox::information(this, tr("Kayıt Bulunamadı"), tr("Bu cihaz için özel atanmış bir SSH kimlik kaydı bulunamadı."));
            return;
        }

        auto res = QMessageBox::question(this, tr("Kimliği Sil"),
            tr("<b>%1</b> (%2) cihazına ait kayıtlı SSH kimlik bilgisini silmek istiyor musunuz?").arg(hostname, ip),
            QMessageBox::Yes | QMessageBox::No);
        if (res == QMessageBox::Yes) {
            m_session->deleteSshCredential(credId);
            appendLog(tr("Cihaz SSH kimlik kaydı silindi: %1 (%2)").arg(hostname, ip));
        }
    });

    menu.exec(m_hostList->viewport()->mapToGlobal(pos));
}

void SshManagerPanel::onAssignCredentials() {
    if (!m_session) return;

    QList<QTableWidgetItem*> selectedItems = m_hostList->selectedItems();
    QSet<int> selectedRows;
    for (auto* item : selectedItems) {
        if (item) selectedRows.insert(item->row());
    }

    if (selectedRows.isEmpty()) {
        QMessageBox::information(this, tr("Cihaz Seçilmedi"),
            tr("Lütfen önce listeden SSH kimlik bilgisi atanacak cihaz(lar)ı seçin.\n\n"
               "Eğer bağlanmak istediğiniz cihaz listede yoksa, 'Cihaz Ekle' butonuyla cihazınızı listeye ekleyebilirsiniz."));
        return;
    }

    struct TargetDevice {
        QString ip;
        QString hostname;
        int row;
    };
    QList<TargetDevice> targets;

    for (int r : selectedRows) {
        if (auto* ipItem = m_hostList->item(r, 1)) {
            QString ip = ipItem->data(Qt::UserRole).toString();
            QString hName = m_hostList->item(r, 0) ? m_hostList->item(r, 0)->text() : ip;
            targets.append({ip, hName, r});
        }
    }

    if (targets.isEmpty()) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("SSH Kimlik Bilgisi Ata"));
    dlg.setFixedWidth(420);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(12);

    auto* info = new QLabel(
        QString(tr("<b>%1</b> adet seçili cihaz için SSH kimlik bilgilerini (kullanıcı adı ve parola/anahtar) belirleyin:"))
            .arg(targets.size()),
        &dlg);
    info->setWordWrap(true);
    lay->addWidget(info);

    auto* selector = new SshCredSelectorWidget(m_session, &dlg);
    selector->loadSettings("SshManagerPanel");
    lay->addWidget(selector);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Kaydet ve Ata"));
    bb->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    selector->saveSettings("SshManagerPanel");

    QString user = selector->username().trimmed();
    QString pass = selector->secret();
    bool isKey = selector->isPrivateKey();

    if (user.isEmpty()) {
        QMessageBox::warning(this, tr("Eksik Bilgi"), tr("Kullanıcı adı boş bırakılamaz."));
        return;
    }

    for (const auto& target : targets) {
        QString ip = target.ip;
        QString hostname = target.hostname;

        QString existingId;
        QString possibleMachineId;
        for (const auto& a : OkidiaDatabase::instance().allAgents()) {
            if (a.ip == ip) {
                possibleMachineId = a.machineId;
                break;
            }
        }

        for (const auto& c : m_session->sshCredentials()) {
            if (c.targetType == "agent") {
                if (!possibleMachineId.isEmpty() && c.machineId == possibleMachineId) {
                    existingId = c.id;
                    break;
                }
                if (c.targetId == ip) {
                    existingId = c.id;
                    break;
                }
            }
        }

        SshCredentialRecord rec;
        rec.id = existingId.isEmpty() ? OkidiaUtils::generateUuid() : existingId;
        if (selector->currentCredentialId().isEmpty()) {
            rec.name = "Manuel: " + hostname;
        } else {
            QString tplName = tr("Şablon");
            for (const auto& c : m_session->sshCredentials()) {
                if (c.id == selector->currentCredentialId()) {
                    tplName = c.name;
                    break;
                }
            }
            rec.name = tr("Şablon (") + tplName + "): " + hostname;
        }
        rec.username = user;
        rec.secret = pass.toUtf8();
        rec.targetId = ip;
        rec.targetType = "agent";
        rec.machineId = possibleMachineId;
        rec.isKey = isKey;
        rec.ownerUserId = m_session->activeUser().id;
        rec.createdAt = OkidiaUtils::nowSecs();

        m_session->addSshCredential(rec);
    }

    appendLog(QString(tr("%1 adet cihaz için SSH kimlik bilgisi kaydedildi.")).arg(targets.size()));
}

void SshManagerPanel::onManageCredentials() {
    if (!m_session) return;
    SshCredentialDialog dlg(m_session, this);
    dlg.exec();
    m_session->fetchSshCredentials();
}
