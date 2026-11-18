// master/ssh/SshManagerPanelSessions.cpp
#include "SshManagerPanel.h"
#include "SshCredentialDialog.h"
#include "SshDeviceSessionWidget.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../session/SessionManager.h"

#include <QMessageBox>
#include <QTableWidget>

void SshManagerPanel::onOpenTerminal() {
    QList<QTableWidgetItem*> selected = m_hostList->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("Aktif Bağlantı"), tr("Lütfen sol taraftaki listeden bağlanılacak cihaz(lar)ı seçin."));
        return;
    }

    saveSettings();

    QSet<int> rows;
    for(auto* item : selected) {
        if(item) rows.insert(item->row());
    }

    // Eksik SSH bilgisi olan var mı kontrol et (Password seçilip parola boş olanlar)
    bool hasMissingCreds = false;
    for (int r : rows) {
        QString ip = m_hostList->item(r, 1)->data(Qt::UserRole).toString();
        auto cfg = buildConfig(ip);
        if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
            hasMissingCreds = true;
            break;
        }
    }

    if (hasMissingCreds) {
        auto res = QMessageBox::question(this, tr("SSH Kimlik Bilgisi Eksik"),
            tr("Seçilen cihaz(lar)dan bazılarında kayıtlı SSH kullanıcı adı veya parolası bulunamadı.\n\n"
               "Bağlantı kurmadan önce SSH kimlik bilgilerini (kullanıcı adı / parola) girmek ister misiniz?"),
            QMessageBox::Yes | QMessageBox::No);
        if (res == QMessageBox::Yes) {
            onAssignCredentials();
        }
    }

    int staggerIdx = 0;
    for (int r : rows) {
        QString ip = m_hostList->item(r, 1)->data(Qt::UserRole).toString();
        QString hostName = m_hostList->item(r, 0)->text();
        QString tabTitle = hostName;

        // Aynı IP için açık sekme varsa ona odaklan
        bool exists = false;
        for (int i = 0; i < m_activeTabs->count(); ++i) {
            auto* existingSession = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(i));
            if (existingSession && existingSession->ip() == ip) {
                m_activeTabs->setCurrentIndex(i);
                existingSession->focusTerminal();
                exists = true;
                break;
            }
        }
        if (exists) continue;

        // Sekmeyi ve widget'ı bloklamadan anında oluştur (autoConnectTerminal = false)
        auto* sessionWidget = new SshDeviceSessionWidget(m_session, ip, hostName, buildConfig(ip), m_activeTabs, m_lastLocalPath, false);
        connect(sessionWidget, &SshDeviceSessionWidget::logMessage, this, &SshManagerPanel::appendLog);
        connect(sessionWidget, &SshDeviceSessionWidget::localPathChanged, this, &SshManagerPanel::onLocalPathSync);

        int tabIdx = m_activeTabs->addTab(sessionWidget, ThemeManager::icon(":/ui/monitor.svg"), tabTitle);
        m_activeTabs->setCurrentIndex(tabIdx);
        sessionWidget->focusTerminal();

        // Her sekmenin SSH bağlantısını kademeli (staggered) başlat.
        // Böylece GUI hiç donmaz, sekmelerin tümü anında görünür ve Wi-Fi AP'de paket patlaması önlenir.
        QTimer::singleShot(staggerIdx * 100, sessionWidget, [sessionWidget]() {
            if (sessionWidget) {
                sessionWidget->connectTerminal();
            }
        });
        staggerIdx++;
    }

    // Aktif Bağlantılar sekmesini öne getir
    m_tabs->setCurrentIndex(0);
}

void SshManagerPanel::onOpenFileManager() {
    QList<QTableWidgetItem*> selected = m_hostList->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("Dosya Yöneticisi"), tr("Lütfen listeden bağlanılacak cihazı seçin."));
        return;
    }

    saveSettings();

    QSet<int> rows;
    for (auto* item : selected) {
        if (item) rows.insert(item->row());
    }

    for (int r : rows) {
        QString ip = m_hostList->item(r, 1)->data(Qt::UserRole).toString();
        QString hostName = m_hostList->item(r, 0)->text();
        QString tabTitle = hostName;

        // Sekme zaten açık mı?
        SshDeviceSessionWidget* targetSession = nullptr;
        for (int i = 0; i < m_activeTabs->count(); ++i) {
            auto* existing = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(i));
            if (existing && existing->ip() == ip) {
                m_activeTabs->setCurrentIndex(i);
                targetSession = existing;
                break;
            }
        }

        if (!targetSession) {
            targetSession = new SshDeviceSessionWidget(m_session, ip, hostName, buildConfig(ip), m_activeTabs, m_lastLocalPath);
            connect(targetSession, &SshDeviceSessionWidget::logMessage, this, &SshManagerPanel::appendLog);
            connect(targetSession, &SshDeviceSessionWidget::localPathChanged, this, &SshManagerPanel::onLocalPathSync);
            int tabIdx = m_activeTabs->addTab(targetSession, ThemeManager::icon(":/ui/monitor.svg"), tabTitle);
            m_activeTabs->setCurrentIndex(tabIdx);
        }

        if (targetSession) {
            targetSession->focusFileManager();
        }
    }

    m_tabs->setCurrentIndex(0);
}

void SshManagerPanel::openTerminalForClient(const QString& clientId) {
    if (!m_session) return;
    auto state = m_session->clientState(clientId);
    if (state.ip.isEmpty()) return;

    QString hostName = state.hostname.isEmpty() ? clientId : state.hostname;
    
    // Açık olan cihaz oturumu var mı kontrol et
    for (int i = 0; i < m_activeTabs->count(); ++i) {
        auto* existingSession = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(i));
        if (existingSession && existingSession->ip() == state.ip) {
            m_activeTabs->setCurrentIndex(i);
            m_tabs->setCurrentIndex(0); // Aktif Bağlantılar
            existingSession->focusTerminal();
            return;
        }
    }

    SshClient::Config cfg = buildConfig(state.ip);
    if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
        SshCredentialDialog dlg(m_session, this);
        dlg.exec();
        cfg = buildConfig(state.ip);
        if (cfg.password.isEmpty() && cfg.authMethod == SshClient::AuthMethod::Password) {
            QMessageBox::warning(this, tr("Bağlantı Başarısız"), tr("Geçerli bir SSH bilgisi girilmediği için bağlanılamıyor."));
            return;
        }
    }
    
    auto* sessionWidget = new SshDeviceSessionWidget(m_session, state.ip, hostName, cfg, m_activeTabs, m_lastLocalPath);
    connect(sessionWidget, &SshDeviceSessionWidget::logMessage, this, &SshManagerPanel::appendLog);
    connect(sessionWidget, &SshDeviceSessionWidget::localPathChanged, this, &SshManagerPanel::onLocalPathSync);

    int tabIdx = m_activeTabs->addTab(sessionWidget, ThemeManager::icon(":/ui/monitor.svg"), hostName);
    m_activeTabs->setCurrentIndex(tabIdx);
    m_tabs->setCurrentIndex(0); // Aktif Bağlantılar
    sessionWidget->focusTerminal();
}

void SshManagerPanel::onLocalPathSync(const QString& path) {
    if (path.isEmpty()) return;
    m_lastLocalPath = path;
    OkidiaUtils::setLastDirectory("ssh_sftp_local", path);

    for (int i = 0; i < m_activeTabs->count(); ++i) {
        auto* session = qobject_cast<SshDeviceSessionWidget*>(m_activeTabs->widget(i));
        if (session) {
            session->setLocalPath(path);
        }
    }
}
