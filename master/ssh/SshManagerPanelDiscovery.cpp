// master/ssh/SshManagerPanelDiscovery.cpp
#include "SshManagerPanel.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../session/SessionManager.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QHostAddress>
#include <QProcess>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>

static ClientState findClientByIpHelper(SessionManager* session, const QString& ip) {
    if (!session || ip.isEmpty()) return {};
    for (const auto& cs : session->clientStates()) {
        if (cs.ip == ip) return cs;
    }
    return {};
}

void SshManagerPanel::onFetchActiveClients() {
    if (!m_session) return;
    
    QStringList connected = m_session->connectedClientIds();
    int added = 0;
    
    auto hasIp = [this](const QString& targetIp) {
        for (int i = 0; i < m_hostList->rowCount(); ++i) {
            if (m_hostList->item(i, 1) && m_hostList->item(i, 1)->data(Qt::UserRole).toString() == targetIp) return true;
        }
        return false;
    };

    m_hostList->setSortingEnabled(false);

    // Sadece aktif çevrimiçi istemcileri ekle
    for (const QString& cid : connected) {
        ClientState state = m_session->clientState(cid);
        QString ip = state.ip;
        QString hostname = state.hostname;
        if (hostname.isEmpty()) hostname = ip;

        if (!ip.isEmpty() && ip != "127.0.0.1" && !hasIp(ip)) {
            int r = m_hostList->rowCount();
            m_hostList->insertRow(r);
            
            auto* nameItem = new OkidiaTableItem(hostname);
            nameItem->setData(Qt::UserRole, ip);
            auto* ipItem = new OkidiaTableItem(ip);
            ipItem->setData(Qt::UserRole, ip);
            bool ok = false;
            quint32 ipv4 = QHostAddress(ip).toIPv4Address(&ok);
            if (ok) {
                ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(ipv4));
            }
            
            QString osTag = resolveHostOs(ip, hostname);
            QString osDisplay = tr("Bilinmeyen");
            QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
            if (osTag.contains("win")) {
                osTag = "windows";
                osDisplay = tr("Windows");
                osIcon = ThemeManager::icon(":/ui/windows.svg");
            } else if (osTag.contains("linux") || osTag.contains("ubuntu") || osTag.contains("pardus") || osTag.contains("debian")) {
                osTag = "linux";
                osDisplay = tr("Linux");
                osIcon = ThemeManager::icon(":/ui/linux.svg");
            }
            auto* osItem = new OkidiaTableItem(osIcon, "");
            osItem->setTextAlignment(Qt::AlignCenter);
            osItem->setToolTip(osDisplay);
            osItem->setData(Qt::UserRole, osTag);
            
            m_hostList->setItem(r, 0, nameItem);
            m_hostList->setItem(r, 1, ipItem);
            m_hostList->setItem(r, 2, osItem);

            if (osTag == "unknown") {
                asyncResolveHostOs(ip);
            }
            
            added++;
        }
    }
    
    m_hostList->setSortingEnabled(true);
    m_hostList->sortByColumn(m_hostList->horizontalHeader()->sortIndicatorSection(),
                             m_hostList->horizontalHeader()->sortIndicatorOrder());
    
    if (added == 0) {
        appendLog(tr("Listede olmayan yeni çevrimiçi istemci bulunamadı."));
    } else {
        appendLog(QString(tr("%1 adet aktif istemci eklendi.")).arg(added));
    }
}

void SshManagerPanel::onAddSavedHosts() {
    if (!m_session) return;
    QList<SshCredentialRecord> allCreds = m_session->sshCredentials();
    QList<SshCredentialRecord> agentCreds;
    for (const auto& cred : allCreds) {
        if (cred.targetType == "agent" && !cred.targetId.isEmpty()) {
            agentCreds.append(cred);
        }
    }

    if (agentCreds.isEmpty()) {
        QMessageBox::information(this, tr("Kayıtlı Cihaz Yok"), tr("Veritabanında 'agent' tipinde kayıtlı hiçbir SSH bilgisi bulunamadı."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Tüm Cihazları Ekle"));
    dlg.resize(400, 300);
    auto* lay = new QVBoxLayout(&dlg);

    lay->addWidget(new QLabel(tr("Lütfen listeye eklemek istediğiniz cihazları seçin:")));

    QTableWidget* table = new QTableWidget(agentCreds.size(), 3, &dlg);
    table->setHorizontalHeaderLabels({tr("Cihaz Adı"), tr("IP Adresi"), tr("İS")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table->setColumnWidth(2, 42);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    for (int i = 0; i < agentCreds.size(); ++i) {
        QString displayName = agentCreds[i].name;
        int lastColon = displayName.lastIndexOf(": ");
        if (lastColon != -1) {
            displayName = displayName.mid(lastColon + 2).trimmed();
        }

        if (m_session) {
            for (const auto& cs : m_session->clientStates()) {
                if (cs.ip == agentCreds[i].targetId && !cs.hostname.isEmpty() &&
                    cs.hostname != "Linux PC" && cs.hostname != "Windows PC" && cs.hostname != "Bilinmeyen Cihaz" && cs.hostname != "Gizli Cihaz" &&
                    cs.hostname != tr("Bilinmeyen Cihaz") && cs.hostname != tr("Gizli Cihaz")) {
                    if (displayName != cs.hostname) {
                        displayName = cs.hostname;
                        int colonIdx = agentCreds[i].name.lastIndexOf(": ");
                        QString prefix = (colonIdx != -1) ? agentCreds[i].name.left(colonIdx + 2) : "Manuel: ";
                        agentCreds[i].name = prefix + displayName;
                        m_session->addSshCredential(agentCreds[i]);
                    }
                    break;
                }
            }
        }

        auto* nameItem = new OkidiaTableItem(displayName);
        auto* ipItem = new OkidiaTableItem(agentCreds[i].targetId);
        ipItem->setData(Qt::UserRole, agentCreds[i].targetId);
        bool ok = false;
        quint32 ipv4 = QHostAddress(agentCreds[i].targetId).toIPv4Address(&ok);
        if (ok) {
            ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(ipv4));
        }

        QString osTag = resolveHostOs(agentCreds[i].targetId, displayName);
        QString osDisplay = tr("Bilinmeyen");
        QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
        if (osTag.contains("win")) {
            osTag = "windows";
            osDisplay = tr("Windows");
            osIcon = ThemeManager::icon(":/ui/windows.svg");
        } else if (osTag.contains("linux") || osTag.contains("ubuntu") || osTag.contains("pardus") || osTag.contains("debian")) {
            osTag = "linux";
            osDisplay = tr("Linux");
            osIcon = ThemeManager::icon(":/ui/linux.svg");
        }
        auto* osItem = new OkidiaTableItem(osIcon, "");
        osItem->setTextAlignment(Qt::AlignCenter);
        osItem->setToolTip(osDisplay);
        osItem->setData(Qt::UserRole, osTag);

        table->setItem(i, 0, nameItem);
        table->setItem(i, 1, ipItem);
        table->setItem(i, 2, osItem);
    }
    table->setSortingEnabled(true);
    table->horizontalHeader()->setSortIndicatorShown(true);
    table->horizontalHeader()->setSectionsClickable(true);
    table->sortByColumn(0, Qt::AscendingOrder);
    lay->addWidget(table);

    auto* btnLay = new QHBoxLayout();
    auto* okBtn = new QPushButton(tr("Seçilenleri Ekle"), &dlg);
    auto* cancelBtn = new QPushButton(tr("İptal"), &dlg);
    btnLay->addStretch();
    btnLay->addWidget(cancelBtn);
    btnLay->addWidget(okBtn);
    lay->addLayout(btnLay);

    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        QList<QTableWidgetItem*> selectedItems = table->selectedItems();
        QSet<int> rows;
        for (auto* item : selectedItems) {
            rows.insert(item->row());
        }

        for (int r : rows) {
            QString ip = table->item(r, 1)->data(Qt::UserRole).toString();
            QString name = table->item(r, 0)->text();

            bool exists = false;
            for (int i = 0; i < m_hostList->rowCount(); ++i) {
                if (m_hostList->item(i, 1) && m_hostList->item(i, 1)->data(Qt::UserRole).toString() == ip) { 
                    exists = true; 
                    break; 
                }
            }

            if (!exists) {
                int row = m_hostList->rowCount();
                m_hostList->setSortingEnabled(false);
                m_hostList->insertRow(row);
                auto* nameItem = new OkidiaTableItem(name.isEmpty() ? ip : name);
                nameItem->setData(Qt::UserRole, ip);
                auto* ipItem = new OkidiaTableItem(ip);
                ipItem->setData(Qt::UserRole, ip);
                bool ok = false;
                quint32 ipv4 = QHostAddress(ip).toIPv4Address(&ok);
                if (ok) {
                    ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(ipv4));
                }

                auto* dlgOsItem = table->item(r, 2);
                auto* osItem = new OkidiaTableItem();
                if (dlgOsItem) {
                    osItem->setIcon(dlgOsItem->icon());
                    osItem->setText("");
                    osItem->setTextAlignment(Qt::AlignCenter);
                    osItem->setToolTip(dlgOsItem->toolTip());
                    osItem->setData(Qt::UserRole, dlgOsItem->data(Qt::UserRole));
                } else {
                    osItem->setIcon(ThemeManager::icon(":/ui/settings.svg"));
                    osItem->setText("");
                    osItem->setTextAlignment(Qt::AlignCenter);
                    osItem->setToolTip(tr("Bilinmeyen"));
                    osItem->setData(Qt::UserRole, "unknown");
                }

                m_hostList->setItem(row, 0, nameItem);
                m_hostList->setItem(row, 1, ipItem);
                m_hostList->setItem(row, 2, osItem);
                m_hostList->setSortingEnabled(true);
                m_hostList->sortByColumn(m_hostList->horizontalHeader()->sortIndicatorSection(),
                                         m_hostList->horizontalHeader()->sortIndicatorOrder());

                if (osItem->data(Qt::UserRole).toString() == "unknown") {
                    asyncResolveHostOs(ip);
                }
            }
        }
        m_hostList->clearSelection();
    }
}

QString SshManagerPanel::resolveHostOs(const QString& ip, const QString& fallbackHostname) {
    if (ip.isEmpty()) return "unknown";

    // 1. SessionManager genel önbelleği
    if (m_session) {
        QString os = m_session->hostOs(ip);
        if (!os.isEmpty() && os != "unknown") {
            return os;
        }
    }

    // 2. Çevrimiçi istemci durumu (Broker üzerinden gelen paketler)
    if (m_session) {
        ClientState cs = findClientByIpHelper(m_session, ip);
        if (!cs.osTag.isEmpty() && cs.osTag != "unknown") {
            QString lower = cs.osTag.toLower();
            if (lower.contains("win")) return "windows";
            if (lower.contains("linux") || lower.contains("ubuntu") || lower.contains("pardus") || lower.contains("debian")) return "linux";
            return lower;
        }
    }

    // 3. OkidiaDatabase yerel istemci veritabanı (çevrimdışı kaydedilmiş istemciler)
    QList<AgentRecord> agents = OkidiaDatabase::instance().allAgents();
    for (const auto& a : agents) {
        if (a.ip == ip) {
            QString lower = (a.osInfo + " " + a.osPrettyName).toLower();
            if (lower.contains("win")) return "windows";
            if (lower.contains("linux") || lower.contains("ubuntu") || lower.contains("pardus") || lower.contains("debian")) return "linux";
        }
    }

    // 4. Kayıtlı SSH kimlik bilgileri ipuçları (isim / kullanıcı adı / hedef)
    if (m_session) {
        for (const auto& cred : m_session->sshCredentials()) {
            if (cred.targetId == ip) {
                QString nameLower = cred.name.toLower();
                if (nameLower.contains("windows") || nameLower.contains("win")) return "windows";
                if (nameLower.contains("linux") || nameLower.contains("ubuntu") || nameLower.contains("pardus") || nameLower.contains("debian")) return "linux";
                QString userLower = cred.username.toLower();
                if (userLower == "administrator") return "windows";
            }
        }
    }

    // 5. Hostname ipucu
    QString nameLower = fallbackHostname.toLower();
    if (nameLower.contains("windows") || nameLower.contains("win")) return "windows";
    if (nameLower.contains("linux") || nameLower.contains("ubuntu") || nameLower.contains("pardus") || nameLower.contains("debian")) return "linux";

    return "unknown";
}

void SshManagerPanel::asyncResolveHostOs(const QString& ip) {
    if (ip.isEmpty()) return;

    (void)QtConcurrent::run([this, ip]() {
        QString detectedOs = "unknown";

        // 1. TTL kontrolü (Ping ile: TTL <= 64 Linux, 64 < TTL <= 128 Windows)
        QProcess pingProc;
        pingProc.start("ping", {"-c", "1", "-W", "1", ip});
        if (pingProc.waitForFinished(1200)) {
            QString out = QString::fromUtf8(pingProc.readAllStandardOutput());
            QRegularExpression ttlRegex("ttl=(\\d+)", QRegularExpression::CaseInsensitiveOption);
            auto match = ttlRegex.match(out);
            if (match.hasMatch()) {
                int ttl = match.captured(1).toInt();
                if (ttl > 0 && ttl <= 64) {
                    detectedOs = "linux";
                } else if (ttl > 64 && ttl <= 128) {
                    detectedOs = "windows";
                }
            }
        }

        // 2. SSH banner kontrolü (OpenSSH Windows veya Linux tespiti)
        if (detectedOs == "unknown") {
            QTcpSocket socket;
            socket.connectToHost(ip, 22);
            if (socket.waitForConnected(1000)) {
                if (socket.waitForReadyRead(1000)) {
                    QByteArray banner = socket.readAll();
                    QString bannerStr = QString::fromUtf8(banner).toLower();
                    if (bannerStr.contains("windows")) {
                        detectedOs = "windows";
                    } else if (bannerStr.contains("ubuntu") || bannerStr.contains("debian") || bannerStr.contains("linux")) {
                        detectedOs = "linux";
                    }
                }
                socket.disconnectFromHost();
            }
        }

        if (detectedOs != "unknown") {
            QMetaObject::invokeMethod(this, [this, ip, detectedOs]() {
                if (m_session) {
                    m_session->setHostOs(ip, detectedOs);
                } else {
                    updateHostRowOs(ip, detectedOs);
                }
            }, Qt::QueuedConnection);
        }
    });
}

void SshManagerPanel::updateHostRowOs(const QString& ip, const QString& osTag) {
    if (ip.isEmpty() || !m_hostList) return;

    QString normalized = osTag.toLower();
    QString osDisplay = tr("Bilinmeyen");
    QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
    if (normalized.contains("win")) {
        normalized = "windows";
        osDisplay = tr("Windows");
        osIcon = ThemeManager::icon(":/ui/windows.svg");
    } else if (normalized.contains("linux") || normalized.contains("ubuntu") || normalized.contains("pardus") || normalized.contains("debian")) {
        normalized = "linux";
        osDisplay = tr("Linux");
        osIcon = ThemeManager::icon(":/ui/linux.svg");
    }

    for (int r = 0; r < m_hostList->rowCount(); ++r) {
        auto* ipItem = m_hostList->item(r, 1);
        if (ipItem && ipItem->data(Qt::UserRole).toString() == ip) {
            auto* osItem = m_hostList->item(r, 2);
            if (!osItem) {
                osItem = new OkidiaTableItem();
                m_hostList->setItem(r, 2, osItem);
            }
            osItem->setIcon(osIcon);
            osItem->setText("");
            osItem->setTextAlignment(Qt::AlignCenter);
            osItem->setToolTip(osDisplay);
            osItem->setData(Qt::UserRole, normalized);
            break;
        }
    }
}
