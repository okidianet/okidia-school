// master/session/NetworkWatcher.h
//
// Ağ bağlantısı değişikliklerini (IP, arayüz, erişilebilirlik) izler.
// Değişim tespit edildiğinde TlsClient::forceReconnect() ve mDNS yeniden
// taraması tetiklenir; böylece Master yeni IP/broker adresini otomatik bulur.
//
// Kullanım:
//   SessionManager constructor'ında oluştur, setTargets() ile bağla.
//
#pragma once

#include <QObject>
#include <QTimer>
#include <QNetworkInformation>

class TlsClient;
class MdnsManager;

class NetworkWatcher : public QObject {
    Q_OBJECT

public:
    explicit NetworkWatcher(QObject* parent = nullptr);
    ~NetworkWatcher() override;

    // İzlenecek nesneleri ayarla — SessionManager constructor'ında çağrılır
    void setTargets(TlsClient* tls, MdnsManager* mdns);

    // Ağ izlemeyi başlat
    void start();
    void stop();

    // Geçerli erişilebilirlik durumu
    bool isReachable() const;

signals:
    // Ağ tekrar erişilebilir oldu — bağlantı yenilenmeli
    void networkRestored();

    // Ağ kesildi — bağlantı kopacak (bilgi amaçlı)
    void networkLost();

private slots:
    void onReachabilityChanged(QNetworkInformation::Reachability reachability);
    void onDebounceTimeout();

private:
    void scheduleReconnect();

    TlsClient*   m_tls          = nullptr;
    MdnsManager* m_mdns         = nullptr;

    // Kısa süreli kesilmeleri filtrele (geçici kıpırdama vs. gerçek değişim)
    QTimer*      m_debounceTimer = nullptr;
    static constexpr int kDebounceMsec = 1500;   // 1.5 sn bekle, sonra aksiyon al

    // mDNS servis adı — yeniden taramada kullanılır
    static constexpr const char* kMdnsService = "_okidia._tcp";

    QNetworkInformation::Reachability m_lastReachability =
        QNetworkInformation::Reachability::Unknown;
};
