// master/session/NetworkWatcher.cpp
//
// Ağ erişilebilirliği değişince TlsClient ve MdnsManager'ı yeniden başlatır.
// Qt 6.1+ QNetworkInformation API'si kullanılır.
//
// Akış:
//   Ağ geldi → 1.5 sn debounce → forceReconnect() + mDNS yeniden tara
//   Ağ gitti → networkLost() sinyali (bilgi amaçlı, TlsClient kendi halleder)
//
#include "NetworkWatcher.h"
#include "../../common/network/TlsClient.h"
#include "../../common/network/MdnsManager.h"
#include "../../common/utils/Logger.h"

NetworkWatcher::NetworkWatcher(QObject* parent)
    : QObject(parent)
{
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(kDebounceMsec);
    connect(m_debounceTimer, &QTimer::timeout,
            this,            &NetworkWatcher::onDebounceTimeout);
}

NetworkWatcher::~NetworkWatcher()
{
    stop();
}

void NetworkWatcher::setTargets(TlsClient* tls, MdnsManager* mdns)
{
    m_tls  = tls;
    m_mdns = mdns;
}

void NetworkWatcher::start()
{
    // Qt 6.1+ QNetworkInformation yükle
    if (!QNetworkInformation::loadBackendByFeatures(
            QNetworkInformation::Feature::Reachability)) {
        Logger::warning(QObject::tr("NetworkWatcher: QNetworkInformation backend yüklenemedi — ağ değişim tespiti devre dışı"));
        return;
    }

    auto* ni = QNetworkInformation::instance();
    if (!ni) return;

    m_lastReachability = ni->reachability();

    connect(ni, &QNetworkInformation::reachabilityChanged,
            this, &NetworkWatcher::onReachabilityChanged,
            Qt::UniqueConnection);

    Logger::info(QObject::tr("NetworkWatcher: Ağ izleme başlatıldı (backend: ")
                 + ni->backendName() + ")");
}

void NetworkWatcher::stop()
{
    m_debounceTimer->stop();
    if (auto* ni = QNetworkInformation::instance())
        ni->disconnect(this);
}

bool NetworkWatcher::isReachable() const
{
    auto* ni = QNetworkInformation::instance();
    if (!ni) return true;  // bilinmiyorsa varsayılan olarak erişilebilir say
    return ni->reachability() == QNetworkInformation::Reachability::Online
        || ni->reachability() == QNetworkInformation::Reachability::Site;
}

// ── Slot: QNetworkInformation'dan ağ erişilebilirlik değişimi ──────────────

void NetworkWatcher::onReachabilityChanged(
    QNetworkInformation::Reachability reachability)
{
    using R = QNetworkInformation::Reachability;

    const bool wasOnline = (m_lastReachability == R::Online ||
                             m_lastReachability == R::Site);
    const bool isOnline  = (reachability == R::Online ||
                             reachability == R::Site);

    Logger::info(QObject::tr("NetworkWatcher: Ağ durumu değişti → %1 → %2")
                 .arg(static_cast<int>(m_lastReachability))
                 .arg(static_cast<int>(reachability)));

    m_lastReachability = reachability;

    if (!wasOnline && isOnline) {
        // Ağ geldi → debounce başlat, sonra yeniden bağlan
        Logger::info(QObject::tr("NetworkWatcher: Ağ geri geldi, yeniden bağlanma bekleniyor..."));
        m_debounceTimer->start();
    } else if (wasOnline && !isOnline) {
        // Ağ gitti — TlsClient zaten keepalive ile bunu fark edecek
        m_debounceTimer->stop();
        Logger::info(QObject::tr("NetworkWatcher: Ağ bağlantısı kesildi"));
        emit networkLost();
    }
}

// ── Slot: Debounce süresi doldu → gerçek yeniden bağlanma ─────────────────

void NetworkWatcher::onDebounceTimeout()
{
    Logger::info(QObject::tr("NetworkWatcher: Ağ değişimi onaylandı → yeniden bağlanılıyor"));

    // 1. Broker bağlantısını yenile (mevcut soketi kapat, yenisini aç)
    if (m_tls) {
        m_tls->forceReconnect();
    }

    // 2. mDNS taramasını yenile — IP/ağ değişince broker yeni adreste olabilir
    //    restartBrowsing: Avahi client'i sıfırlayarak sıfırdan keşif başlatır
    if (m_mdns) {
        m_mdns->restartBrowsing(kMdnsService);
    }

    emit networkRestored();
}
