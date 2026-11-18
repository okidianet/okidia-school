// master/ssh/SshDeviceSessionWidget.cpp
#include "SshDeviceSessionWidget.h"
#include "SshTerminalWidget.h"
#include "SshFileManagerWidget.h"
#include "../session/SessionManager.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/Logger.h"
#include <QVBoxLayout>
#include <QTabBar>
#include <QCloseEvent>
#include <QMessageBox>

SshDeviceSessionWidget::SshDeviceSessionWidget(SessionManager* session,
                                               const QString& ip,
                                               const QString& hostname,
                                               const SshClient::Config& cfg,
                                               QWidget* parent,
                                               const QString& initialLocalPath,
                                               bool autoConnectTerminal)
    : QWidget(parent)
    , m_session(session)
    , m_ip(ip)
    , m_hostname(hostname)
    , m_cfg(cfg)
{
    setupUi(initialLocalPath);
    if (autoConnectTerminal) {
        connectTerminal();
    }
}

SshDeviceSessionWidget::~SshDeviceSessionWidget() {
    stopAllSessions();
    m_cfg.wipe();
}

void SshDeviceSessionWidget::setupUi(const QString& initialLocalPath) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_subTabs = new QTabWidget(this);
    m_subTabs->setTabPosition(QTabWidget::North);
    m_subTabs->setDocumentMode(true);

    // 1. Terminal Sekmesi
    m_terminal = new SshTerminalWidget(m_subTabs);
    m_terminal->setProperty("ip", m_ip);
    m_subTabs->addTab(m_terminal, ThemeManager::icon(":/ui/terminal.svg"), tr("Terminal"));

    // 2. Dosya Yöneticisi Sekmesi (SFTP)
    m_fileManager = new SshFileManagerWidget(m_session, m_subTabs, initialLocalPath);
    m_fileManager->setProperty("ip", m_ip);
    m_fileManager->setProperty("hostname", m_hostname);
    m_subTabs->addTab(m_fileManager, ThemeManager::icon(":/ui/file.svg"), tr("Dosya Yöneticisi"));

    lay->addWidget(m_subTabs);

    connect(m_subTabs, &QTabWidget::currentChanged, this, &SshDeviceSessionWidget::onSubTabChanged);

    connect(m_fileManager, &SshFileManagerWidget::logMessage, this, &SshDeviceSessionWidget::logMessage);
    connect(m_fileManager, &SshFileManagerWidget::localPathChanged, this, &SshDeviceSessionWidget::localPathChanged);
    connect(m_fileManager, &SshFileManagerWidget::connectionStatusChanged, this, [this](bool connected) {
        if (!connected) {
            m_fileManagerConnected = false;
        }
    });
}

void SshDeviceSessionWidget::connectTerminal() {
    if (m_termClient) return;

    m_terminal->appendOutput(QByteArray("\033[33mBaglaniyor: " + m_ip.toUtf8() + "...\033[0m\r\n"));

    m_termClient = new SshClient(this);

    connect(m_termClient, &SshClient::connected, this, [this]() {
        emit logMessage(tr("Terminal bağlandı: ") + m_ip);
        m_terminal->appendOutput(QByteArray("\033[32mBaglanti basarili.\033[0m\r\n"));
        m_terminal->startSession(m_termClient);
    });

    connect(m_termClient, &SshClient::errorOccurred, this, [this](const QString& err) {
        emit logMessage(QString(tr("Terminal hatası (%1): %2")).arg(m_ip, err));
        m_terminal->appendOutput(QByteArray("\033[31mBaglanti hatasi: " + err.toUtf8() + "\033[0m\r\n"));
        if (m_termClient) {
            m_termClient->deleteLater();
            m_termClient = nullptr;
        }
    });

    connect(m_terminal, &SshTerminalWidget::sessionFinished, this, [this](int exitCode) {
        emit logMessage(QString(tr("Terminal oturumu kapandı (%1) [exit: %2]")).arg(m_ip).arg(exitCode));
        if (m_termClient) {
            m_termClient->deleteLater();
            m_termClient = nullptr;
        }
        emit sessionFinished(exitCode);
    });

    m_termClient->connectToHost(m_cfg);
}

void SshDeviceSessionWidget::connectFileManager() {
    if (m_fileManagerConnected) return;
    m_fileManagerConnected = true;

    emit logMessage(tr("Dosya yöneticisi bağlanıyor: ") + m_ip);

    // Eğer m_cfg içinde kimlik bilgisi eksikse session üzerinden tazele
    if (m_session && m_cfg.password.isEmpty() && m_cfg.privateKeyPath.isEmpty() &&
        m_cfg.authMethod == SshClient::AuthMethod::Password) {
        const auto brokerCreds = m_session->sshCredentials();
        for (const auto& c : brokerCreds) {
            if ((c.targetId == m_ip || c.targetId == m_hostname) && c.targetType == "agent") {
                m_cfg.username = c.username;
                if (c.isKey) {
                    m_cfg.authMethod = SshClient::AuthMethod::PrivateKey;
                    m_cfg.privateKeyPath = QString::fromUtf8(c.secret);
                } else {
                    m_cfg.authMethod = SshClient::AuthMethod::Password;
                    m_cfg.password = QString::fromUtf8(c.secret);
                }
                break;
            }
        }
    }

    m_fileManager->connectToHost(m_cfg);
}

void SshDeviceSessionWidget::onSubTabChanged(int index) {
    if (index == 0) {
        // Terminale odaklan
        if (m_terminal) {
            m_terminal->setFocus();
        }
    } else if (index == 1) {
        // Dosya yöneticisine ilk geçildiğinde lazy connect
        if (!m_fileManagerConnected) {
            connectFileManager();
        }
    }
}

void SshDeviceSessionWidget::stopAllSessions() {
    if (m_terminal) {
        m_terminal->stopSession();
    }
    if (m_termClient) {
        m_termClient->disconnectFromHost();
        m_termClient->deleteLater();
        m_termClient = nullptr;
    }
    if (m_fileManager) {
        m_fileManager->disconnectFromHost();
    }
    m_fileManagerConnected = false;
}

void SshDeviceSessionWidget::safeDetachAllSessions() {
    if (m_terminal) {
        m_terminal->safeDetachSession();
    }
    if (m_termClient) {
        m_termClient->disconnectFromHost();
        m_termClient->deleteLater();
        m_termClient = nullptr;
    }
    if (m_fileManager) {
        m_fileManager->disconnectFromHost();
    }
    m_fileManagerConnected = false;
}

void SshDeviceSessionWidget::setTerminalFontSize(int pt) {
    if (m_terminal) {
        m_terminal->setFontSize(pt);
    }
}

void SshDeviceSessionWidget::clearTerminalScreen() {
    if (m_terminal) {
        m_terminal->clearScreen();
    }
}

void SshDeviceSessionWidget::focusTerminal() {
    if (m_subTabs) {
        m_subTabs->setCurrentIndex(0);
    }
    if (m_terminal) {
        m_terminal->setFocus();
    }
}

void SshDeviceSessionWidget::focusFileManager() {
    if (m_subTabs) {
        m_subTabs->setCurrentIndex(1);
    }
    if (!m_fileManagerConnected) {
        connectFileManager();
    }
    if (m_fileManager) {
        m_fileManager->setFocus();
    }
}

bool SshDeviceSessionWidget::isTerminalBusy() const {
    if (!m_termClient || !m_termClient->isConnected()) return false;
    if (m_terminal) {
        return m_terminal->isBusy() || m_terminal->isRunning();
    }
    return true;
}

void SshDeviceSessionWidget::setLocalPath(const QString& path) {
    if (m_fileManager) {
        m_fileManager->setLocalPath(path);
    }
}

void SshDeviceSessionWidget::closeEvent(QCloseEvent* event) {
    if (isTerminalBusy()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Aktif İşlem Uyarısı"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setText(tr("<b>%1</b> (%2) cihazında aktif bir terminal oturumu veya çalışan bir komut tespit edildi.<br><br>"
                          "Pencereyi kapatırsanız çalışan işlem yarıda kalabilir. Nasıl devam etmek istersiniz?")
                       .arg(m_hostname, m_ip));
        auto* cancelBtn = msgBox.addButton(tr("İptal"), QMessageBox::RejectRole);
        auto* detachBtn = msgBox.addButton(tr("Arka Planda Devam Ettir (Güvenli Kapat)"), QMessageBox::ActionRole);
        auto* forceBtn  = msgBox.addButton(tr("Zorla Kapat"), QMessageBox::DestructiveRole);
        msgBox.setDefaultButton(detachBtn);
        msgBox.exec();

        if (msgBox.clickedButton() == cancelBtn) {
            event->ignore();
            return;
        } else if (msgBox.clickedButton() == detachBtn) {
            safeDetachAllSessions();
        } else {
            stopAllSessions();
        }
    } else {
        stopAllSessions();
    }
    event->accept();
}
