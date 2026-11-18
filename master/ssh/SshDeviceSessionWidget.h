// master/ssh/SshDeviceSessionWidget.h
#pragma once

#include <QWidget>
#include <QTabWidget>
#include "SshClient.h"

class SessionManager;
class SshTerminalWidget;
class SshFileManagerWidget;

/**
 * @brief Tek bir cihaza (IP / Hostname) ait Terminal ve Dosya Yöneticisi
 * oturumlarını tek bir arayüz altında toplayan cihaz odaklı sekme bileşeni.
 */
class SshDeviceSessionWidget : public QWidget {
    Q_OBJECT

public:
    explicit SshDeviceSessionWidget(SessionManager* session,
                                    const QString& ip,
                                    const QString& hostname,
                                    const SshClient::Config& cfg,
                                    QWidget* parent = nullptr,
                                    const QString& initialLocalPath = QString(),
                                    bool autoConnectTerminal = true);
    ~SshDeviceSessionWidget() override;

    QString ip() const { return m_ip; }
    QString hostname() const { return m_hostname; }
    SshClient::Config config() const { return m_cfg; }

    SshTerminalWidget* terminalWidget() const { return m_terminal; }
    SshFileManagerWidget* fileManagerWidget() const { return m_fileManager; }

    void connectTerminal();
    void connectFileManager();
    void stopAllSessions();
    void safeDetachAllSessions();

    void setTerminalFontSize(int pt);
    void clearTerminalScreen();
    void focusTerminal();
    void focusFileManager();

    bool isTerminalBusy() const;
    void setLocalPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

signals:
    void logMessage(const QString& msg);
    void sessionFinished(int exitCode);
    void localPathChanged(const QString& path);

private slots:
    void onSubTabChanged(int index);

private:
    void setupUi(const QString& initialLocalPath = QString());

    SessionManager*       m_session     = nullptr;
    QString               m_ip;
    QString               m_hostname;
    SshClient::Config     m_cfg;

    QTabWidget*           m_subTabs     = nullptr;
    SshTerminalWidget*    m_terminal    = nullptr;
    SshFileManagerWidget* m_fileManager = nullptr;
    SshClient*            m_termClient  = nullptr;

    bool                  m_fileManagerConnected = false;
};
