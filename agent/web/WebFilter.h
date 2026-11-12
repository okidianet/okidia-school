// agent/web/WebFilter.h
#pragma once

#include <QObject>
#include <QStringList>
#include "../../common/network/Connection.h"

/**
 * Web filtreleme — sistem genelinde HTTP/HTTPS erişim kontrolü.
 *
 * Yöntem:
 *   - Linux: iptables (TCP 80, 443 REJECT) + GNOME loopback proxy
 *   - Windows: Windows Defender Firewall (netsh advfirewall outbound TCP 80, 443 block) + loopback proxy
 *   - WEB_BLOCK_ALL  → Firewall kuralı ve loopback proxy (tüm HTTP/HTTPS trafiğini engeller)
 *   - WEB_UNBLOCK    → Firewall kuralını kaldır ve proxy'yi temizle
 */
class QTimer;

class WebFilter : public QObject {
    Q_OBJECT

public:
    explicit WebFilter(QObject* parent = nullptr);
    ~WebFilter() override = default;

    void handleEnvelope(const okidia::Envelope& envelope);

    bool isActive() const { return m_active; }
    QStringList blockedDomains() const { return m_blockedDomains; }

signals:
    void filterChanged(bool active);

private slots:
    void verifyRules();

private:
    bool m_active = false;
    QString m_lastReason;
    QStringList m_blockedDomains;
    QTimer* m_verifyTimer = nullptr;

    void blockAll(const QString& reason = QString());
    void blockDomains(const QStringList& domains, const QString& reason = QString());
    void unblock();
    void applyFirewallRules();
    void removeFirewallRules();
    void applyDomainBlockRules();
    void removeDomainBlockRules();
    void showBlockedPage();

    bool isFirewallRuleActive();
    bool isProxyActive();

#if defined(Q_OS_WIN)
    void ensureFirewallServiceRunning();
#else
    // GNOME proxy ayarları
    void setGnomeProxy(const QString& host, int port);
    void clearGnomeProxy();

    void runGsettings(const QStringList& args);
    void runGsettingsBatch(const QList<QStringList>& commands);
#endif
};
