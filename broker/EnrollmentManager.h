#ifndef ENROLLMENTMANAGER_H
#define ENROLLMENTMANAGER_H

#include <QString>
#include <QMap>
#include <functional>
#include "messages.pb.h"

class Connection;
class ClientRegistry;

class EnrollmentManager {
public:
    EnrollmentManager(ClientRegistry* registry);
    
    // İşlemci metodu. Eğer kayıt ile ilgili bir zarf (envelope) ise true döner ve işler.
    // Değilse false döner (main.cpp'nin devam edebilmesi için).
    bool process(Connection* conn, const okidia::Envelope& env, const QString& peerId);

    // Durum yayını için callback
    void setBroadcastStatusCallback(std::function<void()> callback);

    // Broker saat geçerliliği ve kalan Master toleransı yönetimi
    void setBrokerTimeStatus(bool isValid, int remainingLogins);
    bool isBrokerTimeValid() const { return m_brokerTimeValid; }
    int remainingGraceLogins() const { return m_remainingGraceLogins; }
    void onMasterLoginSuccess(Connection* conn);

private:
    void handleDiscoveryRequest(Connection* conn, const okidia::Envelope& env);
    void handleDiscoveryApprove(Connection* conn, const okidia::Envelope& env, const QString& peerId);
    void handleDiscoveryReject(Connection* conn, const okidia::Envelope& env, const QString& peerId);
    void handleDiscoveryIgnore(Connection* conn, const okidia::Envelope& env, const QString& peerId);
    void handleEnrollRequest(Connection* conn, const okidia::Envelope& env);
    void handleTimeSyncRequest(Connection* conn, const okidia::Envelope& env);

    ClientRegistry* m_registry;
    std::function<void()> m_broadcastCallback;
    bool m_brokerTimeValid = true;
    int m_remainingGraceLogins = 3;
    QMap<QString, int> m_rtcBatteryFails; // machine_id -> ardışık başarısızlık sayısı
};

#endif // ENROLLMENTMANAGER_H
