// agent/ui/AgentTray/AgentTray.h
#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QIcon>
#include <QVector>
#include "../../common/network/LocalConnection.h"

class QMenu;
class QAction;

/**
 * Sistem tepsisi ikonu.
 *
 * Durum makinesi:
 *   Bağlı değil  → 3 arama ikonu arasında 1 saniyelik animasyon döngüsü
 *   Bağlı (normal) → tek statik yeşil ikon
 *   Bağlı (kilitli) → tek statik turuncu/sarı ikon
 *   El kaldırıldı → tek statik mor ikon
 *
 * İkon kaynakları Qt resource sistemi üzerinden yüklenir:
 *   :/icons/agent/okidia-agent-connected-22.png
 *   :/icons/agent/okidia-agent-search1-22.png  (animasyon kare 1)
 *   :/icons/agent/okidia-agent-search2-22.png  (animasyon kare 2)
 *   :/icons/agent/okidia-agent-search3-22.png  (animasyon kare 3)
 * İkon dosyaları eksikse çizimli fallback ikon kullanılır.
 */
class AgentTray : public QObject {
    Q_OBJECT

public:
    explicit AgentTray(QObject* parent = nullptr);

    // Servis bağlantısı — el kaldır/indir mesajları buradan gönderilir
    void setLocalConnection(LocalConnection* conn) { m_conn = conn; }
    void clearConnection() { m_conn = nullptr; }

    void setConnected(bool connected);
    void setChatEnabled(bool enabled);  // kilit durumunda sohbet tuşu inaktif
    void setLocked(bool locked);
    void setHandRaised(bool raised);

    void show();
    QSystemTrayIcon* trayIcon() const { return m_tray; }

signals:
    void handRaiseRequested(const QString& message);
    void handLowerRequested();
    void chatStartRequested();

private slots:
    void onHandRaise();
    void onHandLower();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onAnimationTick();   // arama animasyonu adımı

private:
    // İkon yükleme — Resource -> Theme -> Fallback daire
    QIcon loadIcon(const QString& baseName, const QColor& fallbackColor);
    QIcon makeFallbackIcon(const QColor& color);
    void  updateIcon();
    void  startSearchAnimation();
    void  stopSearchAnimation();
    void  updateMenuState();

    QSystemTrayIcon* m_tray     = nullptr;
    QMenu*           m_menu     = nullptr;
    QAction*         m_actRaise = nullptr;
    QAction*         m_actLower = nullptr;
    QAction*         m_actStatus= nullptr;
    QAction*         m_actChat  = nullptr;

    LocalConnection* m_conn      = nullptr;  // servis soketi
    bool        m_connected  = false;
    bool        m_locked     = false;
    bool        m_handRaised = false;

    // İkonlar — constructor'da yüklenir
    QIcon m_iconConnected;   // bağlı, normal
    QIcon m_iconLocked;      // bağlı, kilitli
    QIcon m_iconHandRaised;  // el kaldırıldı
    QVector<QIcon> m_searchIcons;  // [0,1,2] arama animasyonu

    // Animasyon
    QTimer* m_animTimer    = nullptr;
    int     m_animFrame    = 0;
};
