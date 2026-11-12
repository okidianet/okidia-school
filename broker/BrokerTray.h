// broker/BrokerTray.h
#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QIcon>

class QMenu;
class QAction;

/**
 * Broker sistem tepsisi ikonu — tek statik ikon.
 * Tooltip üzerinden durum gösterilir.
 * TODO: port değişikliği menü öğesi (Veyon çakışması)
 */
class BrokerTray : public QObject {
    Q_OBJECT

public:
    explicit BrokerTray(QObject* parent = nullptr);
    ~BrokerTray();

    void setReady(bool ready);
    void setMasterConnected(bool connected);
    void setClientCount(int count);
    void setError(const QString& message);
    // Kurulum kodu — boşsa "kullanıldı" gösterir
    void setSetupCode(const QString& code);
    void show();

signals:
    void quitRequested();

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    QIcon loadIcon(const QString& path, const QColor& fallback);
    QIcon makeFallbackIcon(const QColor& color);
    void  updateTooltip();

    QSystemTrayIcon* m_tray       = nullptr;
    QMenu*           m_menu       = nullptr;
    QAction*         m_actStatus  = nullptr;
    QAction*         m_actClients = nullptr;

    QIcon m_icon;

    bool    m_ready       = false;
    bool    m_masterConn  = false;
    int     m_clientCount = 0;
    QString m_errorMsg;
};
