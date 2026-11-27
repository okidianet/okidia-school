// master/session/SessionManager.cpp
// Core: constructor, broker bağlantısı, yetki yönetimi, durum bayrakları.
// Diğer metodlar ilgili modüllere bölünmüştür:
//   SessionManagerHandlers.cpp  — gelen mesaj handler'ları
//   SessionManagerCommands.cpp  — agent'a gönderilen komutlar
//   SessionManagerDiscovery.cpp — discovery/cert/revoke akışı
#include "SessionManager.h"
#include "../transfer/MasterUpdater.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/OkidiaConfig.h"
#include "../../common/crypto/KeyManager.h"
#include "../ui/SettingsDialog/SettingsDialog.h"
#include <QHostInfo>
#include <QSysInfo>
#include <QNetworkInterface>
#include <QHostAddress>

SessionManager::SessionManager(QObject* parent)
    : QObject(parent) {
    m_isSyncingDb = false;
    m_masterUpdater = new MasterUpdater(this, this);

    m_tls  = new TlsClient(this);
    m_mdns = new MdnsManager(this);

    m_tls->setAutoReconnect(true, 5000);
    loadRevokedList();  // Önceki oturumda iptal edilmiş istemcileri yükle

    connect(m_tls, &TlsClient::connected,
            this, &SessionManager::onBrokerConnected);
    connect(m_tls, &TlsClient::disconnected,
            this, &SessionManager::onBrokerDisconnected);

    connect(m_mdns, &MdnsManager::serviceFound,
            this, &SessionManager::onServiceFound);

    // ── Ağ değişim izleyicisi ─────────────────────────────────────────────
    m_netWatcher = new NetworkWatcher(this);
    m_netWatcher->setTargets(m_tls, m_mdns);
    m_netWatcher->start();

    // Ağ geri gelince mDNS yeniden çözümlenebilir; bağlantı iptalini logla
    connect(m_netWatcher, &NetworkWatcher::networkLost, this, []() {
        Logger::info(QObject::tr("SessionManager: Ağ bağlantısı kesildi — yeniden bağlanma bekleniyor"));
    });
    connect(m_netWatcher, &NetworkWatcher::networkRestored, this, [this]() {
        Logger::info(QObject::tr("SessionManager: Ağ geri geldi — broker yeniden aranıyor"));
        AppSettings cfg = AppSettings::load();
        QString fallbackHost = cfg.remoteBrokerHost.trimmed();
        if (fallbackHost.isEmpty()) {
            fallbackHost = OkidiaConfig::instance().brokerIp().trimmed();
        }
        if (!fallbackHost.isEmpty() && !m_isKickedToOffline && isReachableOrValidRemoteHost(fallbackHost)) {
            if (!m_remoteProbeTimer) {
                m_remoteProbeTimer = new QTimer(this);
                m_remoteProbeTimer->setSingleShot(true);
                connect(m_remoteProbeTimer, &QTimer::timeout, this, &SessionManager::tryRemoteBrokerFallback);
            }
            m_remoteProbeTimer->start(2500);
        }
    });

    m_blankThumbWatchdog = new QTimer(this);
    m_blankThumbWatchdog->setInterval(3000);
    connect(m_blankThumbWatchdog, &QTimer::timeout,
            this, &SessionManager::onBlankThumbnailWatchdog);

    m_staggerRefreshTimer = new QTimer(this);
    m_staggerRefreshTimer->setInterval(120);
    connect(m_staggerRefreshTimer, &QTimer::timeout,
            this, &SessionManager::onStaggerRefreshTick);
}

SessionManager::~SessionManager() {
    if (isRecording()) {
        stopRecording();
    }
    if (isLiveAudioBroadcasting()) {
        stopLiveAudioBroadcast();
    }
}

// ── DB'den kara liste ve onaylı agent'ları yükle ─────────────────────
void SessionManager::loadFromDatabase() {
    if (!OkidiaDatabase::instance().isOpen()) return;

    // Kara listeyi yükle
    for (const auto& rec : OkidiaDatabase::instance().blacklist())
        m_ignoredMachineIds.insert(rec.machineId);
    Logger::info(QObject::tr("SessionManager: DB'den ")
        + QString::number(m_ignoredMachineIds.size())
        + tr(" kara liste kaydı yüklendi"));

    // Kayıtlı agent'ları offline olarak yükle
    const auto agents = OkidiaDatabase::instance().allAgents();
    for (const AgentRecord& rec : agents) {
        if (m_ignoredMachineIds.contains(rec.machineId)) continue;
        m_clientGroups[rec.machineId] = rec.classId;

        if (m_clientStates.contains(rec.machineId) && m_clientStates[rec.machineId].connected) {
            if (m_clientStates[rec.machineId].alias.isEmpty() && !rec.alias.isEmpty()) {
                m_clientStates[rec.machineId].alias = rec.alias;
            }
            // Agent zaten bağlı (ClientInfo MainWindow oluşturulmadan önce gelmiş olabilir)
            // MainWindow'a mevcut 'online' durumunu bildir ki UI'da çevrimiçi görünsün
            emit clientConnected(rec.machineId, m_clientStates[rec.machineId]);
            continue;
        }
        
        QStringList toRemove;
        for (auto it = m_pendingDiscoveryData.begin(); it != m_pendingDiscoveryData.end(); ++it) {
            if (QString::fromStdString(it.value().machine_id()) == rec.machineId) {
                toRemove.append(it.key());
            }
        }
        for (const QString& tId : toRemove) {
            m_pendingDiscoveryData.remove(tId);
            emit discoveryRevoked(tId);
        }

        bool currentlyConnected = false;
        if (m_clientStates.contains(rec.machineId) && m_clientStates[rec.machineId].connected) {
            currentlyConnected = true;
        }

        ClientState state;
        state.clientId  = rec.machineId;
        state.hostname  = rec.hostname;
        state.machineId = rec.machineId;
        state.alias     = rec.alias;
        state.ip        = rec.ip;
        state.agentType = rec.agentType;
        state.agentVersion = rec.agentVer;
        state.osPrettyName = rec.osPrettyName.isEmpty() ? rec.osInfo : rec.osPrettyName;
        QString derivedTag = OkidiaUtils::normalizeOsTag(rec.osPrettyName);
        if (derivedTag == "debian12" || derivedTag == "debian13" ||
            derivedTag == "debian11" || derivedTag == "debian10" ||
            derivedTag == "ubuntu24" || derivedTag == "ubuntu22" ||
            derivedTag == "ubuntu20" || derivedTag == "windows") {
            state.osTag = derivedTag;
        } else {
            state.osTag = OkidiaUtils::normalizeOsTag(rec.osInfo);
        }
        state.osArch    = OkidiaUtils::normalizeArch(rec.osArch);
        state.mac       = rec.mac;
        state.connected = currentlyConnected;

        if (!rec.ip.isEmpty() && !state.osTag.isEmpty()) {
            m_hostOsMap[rec.ip] = state.osTag;
        }

        m_clientStates[rec.machineId] = state;
        if (!currentlyConnected) {
            emit clientLoadedOffline(rec.machineId, state);
        }
    }
    Logger::info(QObject::tr("SessionManager: ") + QString::number(agents.size())
                 + tr(" agent offline yüklendi"));

    // Yerel SSH kimliklerini SQLite'tan yükle (Disaster Recovery hazır)
    loadLocalSshCredentials();
}

// ── Kimlik bilgilerini temizle (Logout sonrası otomatik giriş engellemek için) ──────
void SessionManager::clearCredentials() {
    m_username.clear();
    m_passwordHash.clear();
    m_isAuthenticated = false;
    m_reauthRetryAttempted = false;
    if (m_reauthWatchdogTimer && m_reauthWatchdogTimer->isActive()) {
        m_reauthWatchdogTimer->stop();
    }
    m_activeUser = {};
    m_isKickedToOffline = false;
    m_isOfflineMode = false;
    if (m_tls) {
        m_tls->setAutoReconnect(true);
    }
    Logger::info(QObject::tr("SessionManager: Kimlik bilgileri temizlendi ve kısıtlı mod sıfırlandı (otomatik giriş devre dışı)"));
}

// ── Aktif kullanıcı ve yetki ──────────────────────────────────────────────────

void SessionManager::setActiveUser(const UserRecord& user) {
    m_activeUser = user;
    if (!user.username.isEmpty()) {
        m_username = user.username;
    }
    if (m_activeUser.passwordHash.isEmpty() && !m_passwordHash.isEmpty()) {
        m_activeUser.passwordHash = m_passwordHash;
    } else if (!m_activeUser.passwordHash.isEmpty()) {
        m_passwordHash = m_activeUser.passwordHash;
    }
    refreshPermissions();

    // Delege bitiş zamanı takip timer'ı
    if (!m_delegationTimer) {
        m_delegationTimer = new QTimer(this);
        connect(m_delegationTimer, &QTimer::timeout,
                this, &SessionManager::checkDelegationExpiry);
    }
    if (user.role == "OM" && !m_delegationExpiry.isEmpty()) {
        m_delegationTimer->start(60000);  // her dakika kontrol
    } else {
        m_delegationTimer->stop();
    }

    Logger::info(QObject::tr("SessionManager: Aktif kullanıcı → ")
        + user.username + " (" + user.role + ")");
        
    // Fetch credentials immediately upon login so dropdowns are populated
    fetchSshCredentials();
}

void SessionManager::refreshPermissions() {
    m_managedClassIds.clear();
    m_delegationExpiry.clear();

    if (m_activeUser.id.isEmpty()) return;
    if (m_activeUser.role == "YM") return;  // YM için filtre yok

    const qint64 now = OkidiaUtils::nowSecs();

    // 1. Kalıcı atamalar (user_classes)
    for (const auto& uc : OkidiaDatabase::instance().permissionsForUser(m_activeUser.id)) {
        if (uc.permission == "full")
            m_managedClassIds.insert(uc.classId);
    }

    // 2. Aktif delege atamalar (delegations)
    for (const auto& d : OkidiaDatabase::instance().activeDelegationsForUser(m_activeUser.id)) {
        if (d.endsAt > now) {
            m_managedClassIds.insert(d.classId);
            // En geç bitiş zamanını sakla
            if (!m_delegationExpiry.contains(d.classId)
                || m_delegationExpiry[d.classId] < d.endsAt)
                m_delegationExpiry[d.classId] = d.endsAt;
        }
    }

    // 3. Alt Grup ve Hiyerarşi Mirası (Parent Class -> Child Groups)
    // Öğretmene ana sınıf atandığında (örn: AKILLI TAHTA), o sınıfa ait tüm alt gruplar
    // (örn: 1. Kat, Zemin Kat) ve bunların altındaki tüm istemciler yetki kapsamına alınır.
    const QList<ClassRecord> allClasses = OkidiaDatabase::instance().allClasses();
    bool addedAny = true;
    while (addedAny) {
        addedAny = false;
        for (const auto& c : allClasses) {
            if (!c.parentId.isEmpty() && m_managedClassIds.contains(c.parentId) && !m_managedClassIds.contains(c.id)) {
                m_managedClassIds.insert(c.id);
                if (m_delegationExpiry.contains(c.parentId)) {
                    m_delegationExpiry[c.id] = m_delegationExpiry[c.parentId];
                }
                addedAny = true;
            }
        }
    }

    Logger::info(QObject::tr("SessionManager: OM yetki yüklendi → ")
        + QString::number(m_managedClassIds.size()) + tr(" sınıf/grup"));
}

bool SessionManager::canManageClass(const QString& classId) const {
    if (m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM") return false; // Gözlemci hiçbir sınıfı yönetemez
    if (m_activeUser.role == "YM") return true;
    if (classId.isEmpty() || classId == "__root__") return false;  // kök node — sadece YM

    // Delege süresi dolmuş mu kontrol et
    if (m_delegationExpiry.contains(classId)) {
        if (m_delegationExpiry[classId] <= OkidiaUtils::nowSecs()) {
            // Süre dolmuş — const metod, ancak sonraki refreshPermissions'da temizlenecek
            return false;
        }
    }
    if (m_managedClassIds.contains(classId)) return true;

    // Veritabanı sorgusu ile yetki ve aktif delegasyon kontrolü
    if (OkidiaDatabase::instance().isOpen() && !m_activeUser.id.isEmpty()) {
        if (OkidiaDatabase::instance().isUserAuthorizedForClass(m_activeUser.id, classId)) {
            return true;
        }
    }

    // Üst sınıf hiyerarşisi kontrolü (Parent Class yetkisi alt gruplara miras kalır)
    QString current = classId;
    QSet<QString> visited;
    while (!current.isEmpty() && !visited.contains(current)) {
        visited.insert(current);
        const ClassRecord rec = OkidiaDatabase::instance().classById(current);
        if (rec.parentId.isEmpty()) break;
        if (m_managedClassIds.contains(rec.parentId)) return true;
        current = rec.parentId;
    }

    return false;
}

bool SessionManager::canManageClient(const QString& clientId) const {
    if (m_activeUser.role == "OBSERVER" || m_activeUser.role == "GM") return false; // Gözlemci hiçbir istemciyi yönetemez
    if (m_activeUser.role == "YM") return true;
    if (clientId.isEmpty()) return false;

    QString groupId = m_clientGroups.value(clientId);
    if (groupId.isEmpty() || groupId == "__root__") {
        if (OkidiaDatabase::instance().isOpen()) {
            groupId = OkidiaDatabase::instance().agent(clientId).classId;
            if (!groupId.isEmpty()) {
                const_cast<SessionManager*>(this)->m_clientGroups[clientId] = groupId;
            }
        }
    }
    if (groupId.isEmpty() || groupId == "__root__") {
        return false; // Kural: Sınıf ataması yapılmamış agent'lar SADECE YM tarafından yönetilebilir, OM yönetemez
    }
    return canManageClass(groupId);
}

void SessionManager::checkDelegationExpiry() {
    if (m_delegationExpiry.isEmpty()) {
        m_delegationTimer->stop();
        return;
    }
    const qint64 now = OkidiaUtils::nowSecs();
    bool anyExpired = false;
    for (auto it = m_delegationExpiry.constBegin();
         it != m_delegationExpiry.constEnd(); ++it) {
        if (it.value() <= now) {
            anyExpired = true;
            break;
        }
    }
    if (anyExpired) {
        Logger::info(QObject::tr("SessionManager: Süresi dolan delege yetkisi tespit edildi, yenileniyor"));
        refreshPermissions();
        emit delegationExpired();  // MainWindow tree + thumbnails'i günceller
    }
    // Tüm delege bitmişse timer'ı durdur
    if (m_delegationExpiry.isEmpty())
        m_delegationTimer->stop();
}

void SessionManager::connectToBroker(const QString& certPath,
                                      const QString& keyPath,
                                      const QString& caCertPath) {
    m_certPath  = certPath;
    m_keyPath   = keyPath;
    m_caCertPath= caCertPath;
}

void SessionManager::connectToBrokerAnonymous(const QString& host, quint16 port) {
    m_certPath   = "";
    m_keyPath    = "";
    m_caCertPath = ""; // Veya sistem CA'sı gerekebilir ama mTLS discovery'de boş kalabilir
    m_connecting = true;
    m_tls->connectToServer(host, port, "", "", "");
}

void SessionManager::requestLogin(const QString& username, const QString& password) {
    m_username = username;
    m_passwordHash = password;
    if (!m_conn || !m_conn->isConnected()) return;
    okidia::LoginRequest req;
    req.set_username(username.toStdString());
    req.set_password_hash(password.toStdString());
    req.set_app_version(Okidia::Version::c_str());

    QString osTag = OkidiaUtils::detectLocalOsTag();
    QString osPrettyName = OkidiaUtils::detectLocalOsPrettyName();
    QString osArch = OkidiaUtils::detectLocalOsArch();
    req.set_os_tag(osTag.toStdString());
    req.set_os_arch(osArch.toStdString());
    req.set_os_pretty_name(osPrettyName.toStdString());
    req.set_hostname(QSysInfo::machineHostName().toStdString());
    req.set_machine_id(OkidiaUtils::getMachineId().toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::LOGIN_REQUEST);
    env.set_payload(req.SerializeAsString());
    m_conn->sendEnvelope(env);
}

bool SessionManager::requestMasterUpdateDownload(const QString& version, const QString& osTag, const QString& osArch) {
    if (!m_conn || !m_conn->isConnected()) return false;
    QString effectiveTag = OkidiaUtils::normalizeOsTag(osTag);
    if (effectiveTag.isEmpty() || effectiveTag == "unknown") {
        effectiveTag = OkidiaUtils::detectLocalOsTag();
    }
    QString effectiveArch = OkidiaUtils::normalizeArch(osArch);
    if (effectiveArch.isEmpty() || effectiveArch == "unknown") {
        effectiveArch = OkidiaUtils::detectLocalOsArch();
    }
    okidia::MasterUpdateDownloadRequest req;
    req.set_version(version.toStdString());
    req.set_os_tag(effectiveTag.toStdString());
    req.set_os_arch(effectiveArch.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::MASTER_UPDATE_DOWNLOAD_REQ);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

void SessionManager::requestEnroll(const QString& csrPem, const QString& hostname) {
    if (!m_conn || !m_conn->isConnected()) return;
    okidia::EnrollRequest req;
    req.set_csr_pem(csrPem.toStdString());
    req.set_hostname(hostname.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::ENROLL_REQUEST);
    env.set_payload(req.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void SessionManager::requestSetRecoveryKey(const QString& keyHash) {
    if (!m_conn || !m_conn->isConnected()) return;
    okidia::SetRecoveryKeyRequest req;
    req.set_key_hash(keyHash.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SET_RECOVERY_KEY_REQ);
    env.set_payload(req.SerializeAsString());
    m_conn->sendEnvelope(env);
}

void SessionManager::requestPasswordReset(const QString& recoveryKey, const QString& newPasswordHash) {
    if (!m_conn || !m_conn->isConnected()) return;
    okidia::PasswordResetRequest req;
    req.set_recovery_key(recoveryKey.toStdString());
    req.set_new_password_hash(newPasswordHash.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::PASSWORD_RESET_REQ);
    env.set_payload(req.SerializeAsString());
    m_conn->sendEnvelope(env);
}

bool SessionManager::isReachableOrValidRemoteHost(const QString& host) {
    QString h = host.trimmed();
    if (h.isEmpty()) return false;

    if (h.contains(':')) {
        h = h.split(':').first().trimmed();
    }
    if (h.isEmpty()) return false;

    QHostAddress addr(h);
    if (addr.isNull()) {
        // Hostname veya Domain (örn: broker.okul.edu.tr veya Tailscale MagicDNS adı)
        return true;
    }

    if (addr.isLoopback() || h == "localhost") {
        return true;
    }

    // Tailscale CGNAT IP bloğu: 100.64.0.0/10 (100.64.0.0 - 100.127.255.255)
    if (addr.isInSubnet(QHostAddress("100.64.0.0"), 10)) {
        return true;
    }

    // RFC 1918 Özel Yerel IP'ler: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
    bool isPrivateLan = addr.isInSubnet(QHostAddress("192.168.0.0"), 16) ||
                         addr.isInSubnet(QHostAddress("10.0.0.0"), 8) ||
                         addr.isInSubnet(QHostAddress("172.16.0.0"), 12);

    if (!isPrivateLan) {
        // Genel internet WAN IP'si
        return true;
    }

    // Özel LAN IP'si (L3 / VLAN yönlendirmeli ağlar desteklenir)
    // Kurumsal router veya L3 switch varsa, farklı alt ağdaki broker IP'sine rota üzerinden erişilebilir.
    return true;
}

void SessionManager::triggerReconnect() {
    m_isKickedToOffline = false;
    Logger::info(QObject::tr("SessionManager: Manuel veya otomatik yeniden bağlanma tetiklendi."));
    if (m_tls) {
        m_tls->setAutoReconnect(true, 5000);
        m_tls->forceReconnect();
    }
    if (m_mdns) {
        m_mdns->restartBrowsing("_okidia._tcp");
    }
}

void SessionManager::startAutoDiscovery(const QString& certPath,
                                         const QString& keyPath,
                                         const QString& caCertPath) {
    m_certPath   = certPath;
    m_keyPath    = keyPath;
    m_caCertPath = caCertPath;

    m_mdns->startBrowsing("_okidia._tcp");
    Logger::info(QObject::tr("SessionManager: Broker aranıyor (mDNS)..."));

    // Uzak / Tailscale Fallback Kontrolü
    AppSettings cfg = AppSettings::load();
    QString fallbackHost = cfg.remoteBrokerHost.trimmed();
    if (fallbackHost.isEmpty()) {
        fallbackHost = OkidiaConfig::instance().brokerIp().trimmed();
    }

    if (!fallbackHost.isEmpty() && isReachableOrValidRemoteHost(fallbackHost)) {
        if (!m_remoteProbeTimer) {
            m_remoteProbeTimer = new QTimer(this);
            m_remoteProbeTimer->setSingleShot(true);
            connect(m_remoteProbeTimer, &QTimer::timeout, this, &SessionManager::tryRemoteBrokerFallback);
        }
        // Yerel mDNS için 3 saniye bekle; yerel yanıt gelmezse uzak hostu dene
        m_remoteProbeTimer->start(3000);
    }
}

QString SessionManager::activeNetworkKey() const {
    QSettings s("okidia", "master");
    return s.value("net/activeNetworkKey", OkidiaConfig::currentNetworkKey()).toString();
}

void SessionManager::switchNetwork(const QString& networkKey) {
    if (networkKey.isEmpty()) return;
    QString userProfile = m_activeUser.id.isEmpty() ? "DefaultAdmin" : m_activeUser.id;
    m_caCertPath = OkidiaConfig::userCaPath(userProfile, networkKey);
    m_certPath   = OkidiaConfig::userCertPath(userProfile, networkKey);
    m_keyPath    = OkidiaConfig::userKeyPath(userProfile, networkKey);

    QSettings s("okidia", "master");
    s.setValue("net/activeNetworkKey", networkKey);

    // Veritabanı profiline geçiş yap
    QString targetDb = OkidiaConfig::masterDbPath(userProfile, networkKey);
    if (OkidiaDatabase::instance().isOpen()) {
        OkidiaDatabase::instance().switchDatabase(targetDb);
        QString dbSchool = OkidiaDatabase::instance().metaValue("school_name").trimmed();
        if (!dbSchool.isEmpty()) {
            m_schoolName = dbSchool;
            s.setValue("school/name", dbSchool);
            emit schoolNameChanged(m_schoolName);
        }
    }

    Logger::info(QObject::tr("SessionManager: Ağ profili değiştirildi → ") + networkKey);

    if (m_tls) {
        m_tls->disconnectFromServer();
        m_tls->setAutoReconnect(true, 5000);
    }
    if (m_mdns) {
        m_mdns->restartBrowsing("_okidia._tcp");
    }
}

void SessionManager::tryRemoteBrokerFallback() {
    if (isConnected() || m_connecting) {
        return; // Zaten bağlı veya bağlanıyor
    }

    AppSettings cfg = AppSettings::load();
    QString fallbackHost = cfg.remoteBrokerHost.trimmed();
    if (fallbackHost.isEmpty()) {
        fallbackHost = OkidiaConfig::instance().brokerIp().trimmed();
    }
    int port = cfg.brokerPort > 0 ? cfg.brokerPort : OkidiaConfig::instance().brokerPort();

    if (fallbackHost.isEmpty()) {
        // L3 / VLAN İzolasyonu: Yerel DNS'ten okidia-broker ve broker.okidia.local ara
        QHostInfo::lookupHost(QString::fromLatin1(OkidiaConfig::DEFAULT_BROKER_HOSTNAME), this, [this, port](const QHostInfo& info) {
            if (isConnected() || m_connecting) return;
            if (info.error() == QHostInfo::NoError && !info.addresses().isEmpty()) {
                QString dnsIp = info.addresses().first().toString();
                Logger::info(QObject::tr("SessionManager: Yerel DNS ile Broker çözümlendi (okidia-broker) → ") + dnsIp);
                m_isRemoteConnection = true;
                m_isProbingRemote = true;
                m_connecting = true;
                if (m_certPath.isEmpty() || m_keyPath.isEmpty()) {
                    m_tls->connectToServerAnonymous(dnsIp, port);
                } else {
                    m_tls->connectToServer(dnsIp, port, m_certPath, m_keyPath, m_caCertPath);
                }
                return;
            }

            QHostInfo::lookupHost(QString::fromLatin1(OkidiaConfig::DEFAULT_BROKER_FQDN), this, [this, port](const QHostInfo& info2) {
                if (isConnected() || m_connecting) return;
                if (info2.error() == QHostInfo::NoError && !info2.addresses().isEmpty()) {
                    QString dnsIp = info2.addresses().first().toString();
                    Logger::info(QObject::tr("SessionManager: Yerel DNS ile Broker çözümlendi (broker.okidia.local) → ") + dnsIp);
                    m_isRemoteConnection = true;
                    m_isProbingRemote = true;
                    m_connecting = true;
                    if (m_certPath.isEmpty() || m_keyPath.isEmpty()) {
                        m_tls->connectToServerAnonymous(dnsIp, port);
                    } else {
                        m_tls->connectToServer(dnsIp, port, m_certPath, m_keyPath, m_caCertPath);
                    }
                }
            });
        });
        return;
    }

    if (!isReachableOrValidRemoteHost(fallbackHost)) {
        return;
    }

    // Eğer kullanıcı IP veya Hostname sonuna ":PORT" eklediyse (ör. 100.78.36.38:11200), bunu ayıkla
    if (fallbackHost.contains(':')) {
        QStringList parts = fallbackHost.split(':');
        fallbackHost = parts[0].trimmed();
        if (parts.size() > 1 && !parts[1].trimmed().isEmpty()) {
            bool ok = false;
            int parsedPort = parts[1].trimmed().toInt(&ok);
            if (ok && parsedPort > 0 && parsedPort <= 65535) {
                port = parsedPort;
            }
        }
    }

    if (!m_localBrokerHost.isEmpty() && fallbackHost == m_localBrokerHost && port == m_localBrokerPort) {
        return;
    }

    Logger::info(QObject::tr("SessionManager: Yerel mDNS yanıt vermedi, Uzak/Tailscale Broker deneniyor → %1:%2")
                 .arg(fallbackHost).arg(port));

    m_isRemoteConnection = true;
    m_isProbingRemote = true;
    m_connecting = true;
    if (m_certPath.isEmpty() || m_keyPath.isEmpty()) {
        m_tls->connectToServerAnonymous(fallbackHost, port);
    } else {
        m_tls->connectToServer(fallbackHost, port,
                               m_certPath,
                               m_keyPath,
                               m_caCertPath);
    }
}

void SessionManager::registerThumbnail(const QString& clientId,
                                        ClientThumbnail* thumbnail) {
    m_thumbnails[clientId] = thumbnail;
}

void SessionManager::unregisterThumbnail(const QString& clientId) {
    m_thumbnails.remove(clientId);
}

void SessionManager::clearThumbnails() {
    m_thumbnails.clear();
}

bool SessionManager::sendCommand(const QString& clientId,
                                  const okidia::Envelope& envelope) {
    if (!m_conn || !m_conn->isConnected() || !m_isAuthenticated) return false;

    // OM yetki kontrolü
    if (!canManageClient(clientId)) {
        Logger::warning(QObject::tr("SessionManager: OM yetki engeli → ")
            + m_activeUser.username + " → " + clientId);
        return false;
    }

    const bool online = m_clientStates.contains(clientId)
                        && m_clientStates.value(clientId).connected;

    if (!online) {
        // Bazı komutlar agent çevrimdışı gibi görünse de broker üzerinden
        // hâlâ ulaşılabilir (master yeniden bağlandı, agent hâlâ broker'da).
        // Bu komutlar için online kontrolünü atla — direkt broker'a gönder.
        static const QSet<int> brokerDirect = {
            okidia::MessageType::REFRESH_REQUEST,
            okidia::MessageType::SCREEN_INFO_REQUEST,
            okidia::MessageType::SCREEN_FPS_SET,
            okidia::MessageType::SCREEN_STREAM_PAUSE,
            okidia::MessageType::METRICS_STREAM_START,
            okidia::MessageType::METRICS_STREAM_STOP,
            okidia::MessageType::APP_LIST_REQUEST,
        };
        if (brokerDirect.contains(static_cast<int>(envelope.type()))) {
            okidia::Envelope routed = envelope;
            routed.set_target_id(clientId.toStdString());
            return m_conn->sendEnvelope(routed);
        }

        // Bekletilebilir komut mu? (anlık gerektirmeyen)
        static const QSet<int> deferrable = {
            okidia::MessageType::SCREEN_LOCK,
            okidia::MessageType::SCREEN_UNLOCK,
            okidia::MessageType::INPUT_LOCK,
            okidia::MessageType::INPUT_UNLOCK,
            okidia::MessageType::POWER_SHUTDOWN,
            okidia::MessageType::POWER_RESTART,
            okidia::MessageType::POWER_LOGOUT,
            okidia::MessageType::MSG_POPUP,
            okidia::MessageType::WEB_BLOCK_ALL,
            okidia::MessageType::WEB_UNBLOCK,
            okidia::MessageType::APP_BLOCK_BYNAME,
            okidia::MessageType::APP_BLOCK_BYHASH,
            okidia::MessageType::APP_UNBLOCK,
            okidia::MessageType::APP_WHITELIST_SET,
        };
        if (deferrable.contains(static_cast<int>(envelope.type()))) {
            // machineId bul
            const QString machineId = m_clientStates.contains(clientId)
                ? m_clientStates[clientId].machineId
                : clientId;
            if (!machineId.isEmpty()) {
                okidia::Envelope routed = envelope;
                routed.set_target_id(clientId.toStdString());
                OkidiaDatabase::instance().addPendingCommand(
                    machineId,
                    static_cast<int>(envelope.type()),
                    QByteArray::fromStdString(routed.SerializeAsString()),
                    86400  // 24 saat TTL
                );
                Logger::info(QObject::tr("SessionManager: Bekleyen komut kaydedildi → ")
                             + machineId + QObject::tr(" type=") + QString::number(envelope.type()));
                return true;
            }
        }
        return true;  // bekletilemez komut, sessizce atla
    }

    // Online: direkt gönder
    okidia::Envelope routed = envelope;
    routed.set_target_id(clientId.toStdString());
    return m_conn->sendEnvelope(routed);
}

// ── Broker'a broadcast REFRESH_REQUEST: tüm agent'lardan CLIENT_INFO + tam frame ──
// Master yeniden bağlandığında veya ağ değişiminde çağrılır.
// Broker bu mesajı tüm bağlı agent'lara iletir; agent'lar CLIENT_INFO + full
// frame göndererek master'ın durum tablosunu tazeler.
void SessionManager::sendRefreshBroadcast() {
    if (!m_conn || !m_conn->isConnected()) return;

    // Broadcast (target_id boş → broker herkese iletir)
    okidia::Envelope env;
    env.set_type(okidia::MessageType::REFRESH_REQUEST);
    m_conn->sendEnvelope(env);
    Logger::info(QObject::tr("SessionManager: REFRESH_REQUEST broadcast edildi — agent durum yenilemesi bekleniyor"));
}

int SessionManager::calculateAdaptiveFps() const {
    int count = m_visibleClients.isEmpty() ? m_clientStates.size() : m_visibleClients.size();
    return calculateAdaptiveFps(count);
}

int SessionManager::calculateAdaptiveFps(int visibleCount) const {
    if (m_isStreamPaused) {
        return 0;
    }
    if (m_isUserFpsFixed && m_userConfiguredIntervalMs > 0) {
        return m_userConfiguredIntervalMs; // Kullanıcının tercihi asla ezilmez
    }
    if (visibleCount <= 5) {
        return 200;  // 5 FPS
    } else if (visibleCount <= 12) {
        return 350;  // ~3 FPS
    } else if (visibleCount <= 25) {
        return 500;  // 2 FPS
    } else {
        return 1000; // 1 FPS
    }
}

void SessionManager::setStreamPaused(bool paused) {
    if (m_isStreamPaused == paused) return;
    m_isStreamPaused = paused;
    if (m_isStreamPaused) {
        Logger::info(QObject::tr("SessionManager: Ekran akışı duraklatıldı (0 FPS)"));
        if (m_conn && m_conn->isConnected()) {
            for (const QString& cid : m_visibleClients) {
                if (!m_streamingClients.contains(cid)) {
                    okidia::Envelope pauseEnv;
                    pauseEnv.set_type(okidia::MessageType::SCREEN_STREAM_PAUSE);
                    pauseEnv.set_target_id(cid.toStdString());
                    m_conn->sendEnvelope(pauseEnv);
                }
            }
        }
    } else {
        Logger::info(QObject::tr("SessionManager: Ekran akışı devam ettiriliyor"));
        m_lastSentIntervalMs = -1;
        onVisibleDebounceTimeout();
    }
}

void SessionManager::setUserFpsInterval(int intervalMs) {
    if (intervalMs == 0) {
        setStreamPaused(true);
        return;
    }

    if (m_isStreamPaused) {
        m_isStreamPaused = false;
    }

    if (intervalMs < 0) {
        m_isUserFpsFixed = false;
        m_userConfiguredIntervalMs = -1;
        Logger::info(QObject::tr("SessionManager: FPS kipi Adaptif (Otomatik) olarak ayarlandı"));
    } else {
        m_isUserFpsFixed = true;
        m_userConfiguredIntervalMs = intervalMs;
        Logger::info(QObject::tr("SessionManager: FPS kipi sabitlendi → %1 ms").arg(intervalMs));
    }
    m_lastSentIntervalMs = -1;
    if (m_conn && m_conn->isConnected() && !m_visibleClients.isEmpty()) {
        onVisibleDebounceTimeout();
    }
}

void SessionManager::setVisibleClients(const QSet<QString>& visibleClients) {
    m_pendingVisibleClients = visibleClients;
    if (!m_visibleDebounceTimer) {
        m_visibleDebounceTimer = new QTimer(this);
        m_visibleDebounceTimer->setSingleShot(true);
        connect(m_visibleDebounceTimer, &QTimer::timeout, this, &SessionManager::onVisibleDebounceTimeout);
    }
    // 150ms debounce ile ardışık tıklamalarda paket fırtınasını önle
    m_visibleDebounceTimer->start(150);
}

void SessionManager::onVisibleDebounceTimeout() {
    if (!m_conn || !m_conn->isConnected() || !m_isAuthenticated || m_isOfflineMode) {
        m_visibleClients = m_pendingVisibleClients;
        return;
    }

    const QSet<QString> oldVisible = m_visibleClients;
    const QSet<QString> newVisible = m_pendingVisibleClients;
    m_visibleClients = newVisible;

    if (m_isStreamPaused) {
        // Akış duraklatıldı: Tüm görünür istemcilere SCREEN_STREAM_PAUSE gönder
        for (const QString& cid : oldVisible) {
            if (!m_streamingClients.contains(cid)) {
                okidia::Envelope pauseEnv;
                pauseEnv.set_type(okidia::MessageType::SCREEN_STREAM_PAUSE);
                pauseEnv.set_target_id(cid.toStdString());
                m_conn->sendEnvelope(pauseEnv);
            }
        }
        for (const QString& cid : newVisible) {
            if (!m_streamingClients.contains(cid)) {
                okidia::Envelope pauseEnv;
                pauseEnv.set_type(okidia::MessageType::SCREEN_STREAM_PAUSE);
                pauseEnv.set_target_id(cid.toStdString());
                m_conn->sendEnvelope(pauseEnv);
            }
        }
        return;
    }

    // Ayrılanlar: oldVisible - newVisible → SCREEN_STREAM_PAUSE
    // Ancak istemci uzaktan kontrolde / tam ekran akışında ise duraklatma!
    for (const QString& cid : oldVisible) {
        if (!newVisible.contains(cid) && !m_streamingClients.contains(cid)) {
            okidia::Envelope pauseEnv;
            pauseEnv.set_type(okidia::MessageType::SCREEN_STREAM_PAUSE);
            pauseEnv.set_target_id(cid.toStdString());
            m_conn->sendEnvelope(pauseEnv);
        }
    }

    if (newVisible.isEmpty()) return;

    // Adaptif / sabit interval hesabı
    int intervalMs = calculateAdaptiveFps(newVisible.size());
    const bool fpsChanged = (intervalMs != m_lastSentIntervalMs);
    m_lastSentIntervalMs = intervalMs;

    // Eğer görünür küme değişmediyse VE FPS aynıysa hiçbir paket gönderme
    if (oldVisible == newVisible && !fpsChanged) {
        return;
    }

    // Yeni gelen veya görünür olan istemciler için SCREEN_FPS_SET gönder (Broker Pub/Sub subscribe eder)
    // Uzak kontrol veya tam ekran akışında olan istemcileri atla — yüksek kalite ayarlarını ezme!
    for (const QString& cid : newVisible) {
        if (m_streamingClients.contains(cid)) {
            continue;
        }
        // Eğer FPS değişmediyse, zaten abone ve akmakta olan eski görünür istemcileri atla
        if (!fpsChanged && oldVisible.contains(cid)) {
            continue;
        }

        okidia::ScreenConfig cfg;
        cfg.set_interval_ms(static_cast<uint32_t>(intervalMs));
        cfg.set_jpeg_quality(60);
        cfg.set_max_width(360);
        okidia::Envelope fpsEnv;
        fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
        fpsEnv.set_payload(cfg.SerializeAsString());
        fpsEnv.set_target_id(cid.toStdString());
        m_conn->sendEnvelope(fpsEnv);
    }

    Logger::info(QObject::tr("SessionManager: Görünür istemciler güncellendi (Görünür: %1 adet, Hız: %2 ms)")
                 .arg(newVisible.size()).arg(intervalMs));
}

void SessionManager::setClientStreaming(const QString& clientId, bool streaming) {
    if (streaming) {
        m_streamingClients.insert(clientId);
        Logger::info(QObject::tr("SessionManager: İstemci akış moduna alındı (Thumbnail FPS ezmesi pasif) → ") + clientId);
    } else {
        m_streamingClients.remove(clientId);
        Logger::info(QObject::tr("SessionManager: İstemci akış modundan çıkarıldı → ") + clientId);
    }
}

bool SessionManager::isClientStreaming(const QString& clientId) const {
    return m_streamingClients.contains(clientId);
}

void SessionManager::updateAdaptiveFps() {
    if (!m_conn || !m_conn->isConnected()) return;

    if (!m_visibleClients.isEmpty()) {
        setVisibleClients(m_visibleClients);
    } else {
        int intervalMs = calculateAdaptiveFps();
        Logger::info(QObject::tr("SessionManager: Adaptif FPS aralığı: %1 ms").arg(intervalMs));
        broadcastScreenConfig(intervalMs, 0);
    }
}

bool SessionManager::broadcastCommand(const okidia::Envelope& envelope) {
    if (!m_conn || !m_conn->isConnected()) return false;
    return m_conn->sendEnvelope(envelope);
}

bool SessionManager::isConnected() const {
    return m_tls && m_tls->isConnected();
}

// ---------------------------------------------------------------------------
// ClientState erişimi
// ---------------------------------------------------------------------------
ClientState SessionManager::clientState(const QString& clientId) const {
    return m_clientStates.value(clientId, ClientState{});
}

QList<ClientState> SessionManager::allClientStates() const {
    return m_clientStates.values();
}

QStringList SessionManager::connectedClientIds() const {
    QStringList ids;
    for (auto it = m_clientStates.constBegin();
         it != m_clientStates.constEnd(); ++it) {
        if (it->connected) ids << it.key();
    }
    return ids;
}

int SessionManager::connectedClientCount() const {
    int n = 0;
    for (const auto& s : m_clientStates)
        if (s.connected) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Durum bayrak güncellemeleri — komut gönderildikten sonra UI'ı sync et
// ---------------------------------------------------------------------------
void SessionManager::setClientScreenLocked(const QString& clientId,
                                            bool locked) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].screenLocked = locked;
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

void SessionManager::setClientInputLocked(const QString& clientId,
                                           bool locked) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].inputLocked = locked;
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

void SessionManager::setClientAudioMuted(const QString& clientId,
                                           bool muted) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].audioMuted = muted;
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

void SessionManager::setClientWebFiltered(const QString& clientId,
                                           bool filtered) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].webFiltered = filtered;
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

void SessionManager::setClientAppBlocked(const QString& clientId,
                                          bool blocked) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].appBlocked = blocked;
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

bool SessionManager::isClientAppBlocked(const QString& clientId) const {
    if (!m_clientStates.contains(clientId)) return false;
    return m_clientStates[clientId].appBlocked;
}

void SessionManager::setClientHandRaised(const QString& clientId,
                                          bool raised,
                                          const QString& message) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].handRaised = raised;
    m_clientStates[clientId].handRaiseMessage = raised ? message : QString();
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

void SessionManager::setClientExamActive(const QString& clientId,
                                          bool active) {
    if (!m_clientStates.contains(clientId)) return;
    m_clientStates[clientId].examActive = active;
    if (m_thumbnails.contains(clientId))
        m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
    emit clientStateChanged(clientId, m_clientStates[clientId]);
}

void SessionManager::onServiceFound(const QString& name,
                                     const QString& host,
                                     quint16 port,
                                     const QString& role,
                                     const QString& caFingerprint) {
    Q_UNUSED(role)  // Faz 13'te MB/SB öncelik sıralaması için kullanılacak

    // Çoklu Broker ortamı ve CA eşleştirme denetimi:
    if (!caFingerprint.isEmpty()) {
        auto normalizeFp = [](const QString& fp) -> QString {
            QString clean = fp;
            clean.remove(':').remove(' ').remove('-');
            return clean.trimmed().toUpper();
        };

        const QString normCafp = normalizeFp(caFingerprint);
        bool fpMatches = false;
        if (!m_caCertPath.isEmpty() && QFile::exists(m_caCertPath)) {
            QString localFp = KeyManager::getFingerprint(m_caCertPath);
            if (!localFp.isEmpty() && (normalizeFp(localFp) == normCafp || localFp.compare(caFingerprint, Qt::CaseInsensitive) == 0)) {
                fpMatches = true;
            }
        }

        // Eğer mevcut m_caCertPath eşleşmediyse, kullanıcının kayıtlı diğer ağ klasörlerini tara (Multi-Tenant Auto-Switch)
        if (!fpMatches) {
            QString userProfile = m_activeUser.id.isEmpty() ? "DefaultAdmin" : m_activeUser.id;
            QString networksBase = OkidiaConfig::masterCertBaseDir() + "/" + userProfile + "/Networks";
            QDir netDir(networksBase);
            if (netDir.exists()) {
                for (const QString& d : netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    QString candidateCa = networksBase + "/" + d + "/ca.crt";
                    if (QFile::exists(candidateCa)) {
                        QString candFp = KeyManager::getFingerprint(candidateCa);
                        if (!candFp.isEmpty() && (normalizeFp(candFp) == normCafp || candFp.compare(caFingerprint, Qt::CaseInsensitive) == 0)) {
                            Logger::info(QObject::tr("SessionManager: mDNS anonsu ile kayıtlı ağ tespit edildi → ") + d);
                            m_caCertPath = candidateCa;
                            m_certPath   = networksBase + "/" + d + "/master.crt";
                            m_keyPath    = networksBase + "/" + d + "/master.key";
                            fpMatches = true;

                            QSettings s("okidia", "master");
                            s.setValue("net/activeNetworkKey", d);
                            s.setValue("net/activeCaFingerprint", caFingerprint);

                            QString targetDb = OkidiaConfig::masterDbPath(userProfile, d);
                            if (OkidiaDatabase::instance().isOpen()) {
                                OkidiaDatabase::instance().switchDatabase(targetDb);
                            }
                            break;
                        }
                    }
                }
            }
        }

        if (!fpMatches && !m_caCertPath.isEmpty() && QFile::exists(m_caCertPath)) {
            Logger::info(QObject::tr("SessionManager: Farklı CA imzasına sahip yabancı Broker anonsu yok sayıldı → ") + name + " (" + host + ")");
            return;
        }
    }

    if (m_remoteProbeTimer && m_remoteProbeTimer->isActive()) {
        m_remoteProbeTimer->stop();
    }

    m_localBrokerHost = host;
    m_localBrokerPort = port;

    if (isConnected()) {
        if (m_tls->host() == host && m_tls->port() == port) return;
        // Eğer bağlı olduğumuz adres 127.0.0.1 ise ve yeni gelen host gerçek bir LAN IP'si ise geç,
        // veya uzaktan bağlıyken yerel mDNS broker bulunduysa yerel broker'a geç
        if (m_isRemoteConnection || (m_tls->host() == "127.0.0.1" && host != "127.0.0.1")) {
            Logger::info(QObject::tr("SessionManager: Yerel ağda mDNS Broker keşfedildi, mevcut bağlantı yerel IP'ye aktarılıyor → %1:%2").arg(host).arg(port));
        } else {
            return;
        }
    } else if (m_connecting) {
        if (m_tls->host() == host && m_tls->port() == port) return;
        Logger::info(QObject::tr("SessionManager: Bekleyen bağlantı iptal edilip yeni keşfedilen yerel mDNS Broker'a bağlanılıyor → %1:%2").arg(host).arg(port));
    } else {
        Logger::info(QObject::tr("SessionManager: Broker bulundu (mDNS) → %1:%2").arg(host).arg(port));
    }

    m_isRemoteConnection = false;
    m_isProbingRemote = false;
    m_connecting = true;
    if (m_certPath.isEmpty() || m_keyPath.isEmpty()) {
        m_tls->connectToServerAnonymous(host, port);
    } else {
        m_tls->connectToServer(host, port,
                                m_certPath,
                                m_keyPath,
                                m_caCertPath);
    }
}

void SessionManager::onBrokerConnected(Connection* conn) {
    if (m_remoteProbeTimer && m_remoteProbeTimer->isActive()) {
        m_remoteProbeTimer->stop();
    }

    m_connecting = false;
    m_isProbingRemote = false;
    m_isAuthenticated = false;
    m_reauthRetryAttempted = false;
    if (m_reauthWatchdogTimer && m_reauthWatchdogTimer->isActive()) {
        m_reauthWatchdogTimer->stop();
    }
    if (m_blankThumbWatchdog && m_blankThumbWatchdog->isActive()) {
        m_blankThumbWatchdog->stop();
    }
    m_conn = conn;
    if (!m_isRemoteConnection) {
        m_localBrokerHost = m_tls->host();
        m_localBrokerPort = m_tls->port();
    }
    Logger::info(QObject::tr("SessionManager: Broker bağlantısı kuruldu (%1:%2)")
                 .arg(m_tls->host()).arg(m_tls->port()));

    connect(m_conn, &Connection::envelopeReceived,
            this, &SessionManager::onMessageReceived);

    connect(m_conn, &Connection::disconnected,
            m_conn, [this]() {
        m_conn = nullptr;
    });

    // Master kendini broker'a tanıtsın
    okidia::ClientInfo masterInfo;
    masterInfo.set_hostname("__master__");
    masterInfo.set_machine_id("master");

    okidia::Envelope env;
    env.set_type(okidia::MessageType::CLIENT_INFO);
    env.set_payload(masterInfo.SerializeAsString());
    conn->sendEnvelope(env);
    Logger::info(QObject::tr("SessionManager: Master kimliği broker'a gönderildi"));

    // Yeniden bağlantıda otomatik olarak giriş yap
    QString loginUser = !m_username.isEmpty() ? m_username : m_activeUser.username;
    QString loginPass = !m_passwordHash.isEmpty() ? m_passwordHash : m_activeUser.passwordHash;

    // Bellekteki parola hash'i boş kaldıysa yerel SQLite DB'den aktif kullanıcı hash'ini kurtar
    if (loginPass.isEmpty() && OkidiaDatabase::instance().isOpen() &&
        !m_activeUser.id.isEmpty() && m_activeUser.id != "offline_admin") {
        UserRecord dbUser = OkidiaDatabase::instance().userById(m_activeUser.id);
        if (dbUser.id.isEmpty() && !loginUser.isEmpty()) {
            dbUser = OkidiaDatabase::instance().userByUsername(loginUser);
        }
        if (!dbUser.id.isEmpty() && !dbUser.passwordHash.isEmpty()) {
            if (loginUser.isEmpty()) loginUser = dbUser.username;
            loginPass = dbUser.passwordHash;
            m_username = loginUser;
            m_passwordHash = loginPass;
            m_activeUser.passwordHash = loginPass;
            Logger::info(QObject::tr("SessionManager: Otomatik yeniden giriş için kimlik bilgisi yerel DB'den kurtarıldı → ") + loginUser);
        }
    }

    bool hasCredentials = !loginUser.isEmpty() && !loginPass.isEmpty();

    if (m_isKickedToOffline) {
        Logger::info(QObject::tr("SessionManager: Kısıtlı modda (kicked) olduğumuz için otomatik login yapılmıyor."));
    } else if (hasCredentials) {
        Logger::info(QObject::tr("SessionManager: Bağlantı yeniden kuruldu, otomatik olarak tekrar giriş yapılıyor..."));
        requestLogin(loginUser, loginPass);

        // Broker yeniden başlatma / IP değişimi yarışında LOGIN_RESPONSE düşerse
        // 5 saniye sonra tek seferlik otomatik tekrar deneme yapan watchdog
        if (!m_reauthWatchdogTimer) {
            m_reauthWatchdogTimer = new QTimer(this);
            m_reauthWatchdogTimer->setSingleShot(true);
            connect(m_reauthWatchdogTimer, &QTimer::timeout, this, [this]() {
                if (!m_conn || !m_conn->isConnected() || m_isAuthenticated || m_isKickedToOffline) {
                    return;
                }
                const QString u = !m_username.isEmpty() ? m_username : m_activeUser.username;
                const QString p = !m_passwordHash.isEmpty() ? m_passwordHash : m_activeUser.passwordHash;
                if (!m_reauthRetryAttempted && !u.isEmpty() && !p.isEmpty()) {
                    m_reauthRetryAttempted = true;
                    Logger::warning(QObject::tr("SessionManager: Otomatik yeniden giriş yanıtı 5 sn içinde alınamadı, tekrar deneniyor..."));
                    requestLogin(u, p);
                    m_reauthWatchdogTimer->start(5000);
                } else if (m_isOfflineMode) {
                    Logger::error(QObject::tr("SessionManager: Otomatik yeniden giriş zaman aşımına uğradı, kullanıcıya bildirim gönderiliyor."));
                    emit offlineLoginFailed(tr("Sunucu kimlik doğrulama yanıtı zaman aşımına uğradı."));
                }
            });
        }
        m_reauthWatchdogTimer->start(5000);
    } else if (!m_activeUser.id.isEmpty() && m_activeUser.id != "offline_admin") {
        Logger::warning(QObject::tr("SessionManager: Aktif oturum (%1) için önbellekte parola hash'i bulunamadı, otomatik yeniden giriş yapılamıyor!")
                        .arg(loginUser.isEmpty() ? m_activeUser.id : loginUser));
        if (m_isOfflineMode) {
            emit offlineLoginFailed(tr("Oturum kimlik bilgileri bellekte bulunamadı."));
        }
    }

    emit brokerConnected();
    emit connectionModeChanged(connectionMode());

    // Yeniden bağlantıda veya ilk bağlantıda:
    // Görünür istemci abonelikleri ve boş-kare watchdog'u YALNIZCA LOGIN_RESPONSE
    // başarılı döndükten sonra başlatılır; böylece oturum açılmadan Broker'a yetkisiz
    // SCREEN_FPS_SET / REFRESH_REQUEST paketleri gönderilmez.
}

void SessionManager::onBlankThumbnailWatchdog() {
    if (!m_conn || !m_conn->isConnected() || !m_isAuthenticated || m_isOfflineMode || m_isStreamPaused || m_visibleClients.isEmpty()) {
        return;
    }

    const int intervalMs = calculateAdaptiveFps(m_visibleClients.size());
    if (intervalMs <= 0) return;

    QStringList blankClients;
    for (const QString& cid : m_visibleClients) {
        if (m_streamingClients.contains(cid)) continue;
        const auto stIt = m_clientStates.constFind(cid);
        if (stIt == m_clientStates.constEnd() || !stIt->connected || stIt->agentType == 2) {
            continue;
        }
        if (stIt->lockType == "RemoteControl" || stIt->lockType == "ScreenView" || stIt->lockType == "Broadcast") {
            continue;
        }
        ClientThumbnail* thumb = m_thumbnails.value(cid, nullptr);
        if (!thumb || thumb->hasValidFrame()) {
            continue;
        }
        blankClients << cid;
    }

    if (blankClients.isEmpty()) {
        m_blankWatchdogCursor = 0;
        return;
    }

    // Senkron ağ patlamasını (thundering herd) önlemek için her döngüde en fazla 3 boş istemciye
    // round-robin sırayla kompakt thumbnail konfigürasyonu (max_width=360) + tam kare (REFRESH_REQUEST) iste.
    const int batchCount = qMin(3, blankClients.size());
    if (m_blankWatchdogCursor >= blankClients.size()) {
        m_blankWatchdogCursor = 0;
    }
    for (int i = 0; i < batchCount; ++i) {
        const QString& cid = blankClients[(m_blankWatchdogCursor + i) % blankClients.size()];

        okidia::ScreenConfig cfg;
        cfg.set_interval_ms(static_cast<uint32_t>(intervalMs));
        cfg.set_jpeg_quality(60);
        cfg.set_max_width(360);
        okidia::Envelope fpsEnv;
        fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
        fpsEnv.set_payload(cfg.SerializeAsString());
        fpsEnv.set_target_id(cid.toStdString());
        m_conn->sendEnvelope(fpsEnv);

        okidia::Envelope refEnv;
        refEnv.set_type(okidia::MessageType::REFRESH_REQUEST);
        refEnv.set_target_id(cid.toStdString());
        m_conn->sendEnvelope(refEnv);
    }
    m_blankWatchdogCursor = (m_blankWatchdogCursor + batchCount) % blankClients.size();
}

int SessionManager::refreshVisibleThumbnails() {
    if (!m_conn || !m_conn->isConnected() || !m_isAuthenticated || m_isOfflineMode || m_visibleClients.isEmpty()) {
        return 0;
    }
    if (m_isStreamPaused) {
        setStreamPaused(false);
    }

    m_staggerRefreshQueue.clear();
    for (const QString& cid : m_visibleClients) {
        if (m_streamingClients.contains(cid)) continue;
        const auto stIt = m_clientStates.constFind(cid);
        if (stIt == m_clientStates.constEnd() || !stIt->connected || stIt->agentType == 2) {
            continue;
        }
        if (stIt->lockType == "RemoteControl" || stIt->lockType == "ScreenView" || stIt->lockType == "Broadcast") {
            continue;
        }
        m_staggerRefreshQueue << cid;
    }

    const int count = m_staggerRefreshQueue.size();
    if (count > 0 && m_staggerRefreshTimer) {
        onStaggerRefreshTick(); // İlk 3 istemciyi anında gönder
        if (!m_staggerRefreshQueue.isEmpty() && !m_staggerRefreshTimer->isActive()) {
            m_staggerRefreshTimer->start();
        }
    }
    return count;
}

void SessionManager::onStaggerRefreshTick() {
    if (!m_conn || !m_conn->isConnected() || !m_isAuthenticated || m_isOfflineMode || m_staggerRefreshQueue.isEmpty()) {
        m_staggerRefreshQueue.clear();
        if (m_staggerRefreshTimer) m_staggerRefreshTimer->stop();
        return;
    }

    const int intervalMs = qMax(200, calculateAdaptiveFps(m_visibleClients.size()));
    const int batch = qMin(3, m_staggerRefreshQueue.size());
    for (int i = 0; i < batch; ++i) {
        const QString cid = m_staggerRefreshQueue.takeFirst();
        if (m_streamingClients.contains(cid)) continue;

        okidia::ScreenConfig cfg;
        cfg.set_interval_ms(static_cast<uint32_t>(intervalMs));
        cfg.set_jpeg_quality(60);
        cfg.set_max_width(360);
        okidia::Envelope fpsEnv;
        fpsEnv.set_type(okidia::MessageType::SCREEN_FPS_SET);
        fpsEnv.set_payload(cfg.SerializeAsString());
        fpsEnv.set_target_id(cid.toStdString());
        m_conn->sendEnvelope(fpsEnv);

        okidia::Envelope refEnv;
        refEnv.set_type(okidia::MessageType::REFRESH_REQUEST);
        refEnv.set_target_id(cid.toStdString());
        m_conn->sendEnvelope(refEnv);
    }

    if (m_staggerRefreshQueue.isEmpty() && m_staggerRefreshTimer) {
        m_staggerRefreshTimer->stop();
    }
}

void SessionManager::onBrokerDisconnected() {
    if (m_reauthWatchdogTimer && m_reauthWatchdogTimer->isActive()) {
        m_reauthWatchdogTimer->stop();
    }
    m_reauthRetryAttempted = false;
    if (m_blankThumbWatchdog) {
        m_blankThumbWatchdog->stop();
    }
    if (m_staggerRefreshTimer) {
        m_staggerRefreshTimer->stop();
    }
    m_staggerRefreshQueue.clear();
    m_blankWatchdogCursor = 0;
    m_connecting = false;
    m_isAuthenticated = false;
    m_conn = nullptr;
    m_lastSentIntervalMs = -1; // Yeniden bağlantıda SCREEN_FPS_SET aboneliğinin zorla yenilenmesini garanti et
    Logger::warning(QObject::tr("SessionManager: Broker bağlantısı kesildi"));

    // Eğer uzak host probu denenirken bağlantı koptuysa veya zaman aşımına uğradıysa,
    // yerel broker adresine geri dön ki auto-reconnect döngüsü ölü uzak IP'ye kilitlenmesin
    if (m_isProbingRemote) {
        m_isProbingRemote = false;
        m_isRemoteConnection = false;
        if (!m_localBrokerHost.isEmpty()) {
            Logger::info(QObject::tr("SessionManager: Uzak broker denemesi sonuçsuz kaldı, yerel broker adresine (%1:%2) geri dönülüyor...")
                         .arg(m_localBrokerHost).arg(m_localBrokerPort));
            m_tls->connectToServer(m_localBrokerHost, m_localBrokerPort,
                                   m_certPath, m_keyPath, m_caCertPath);
        }
    }

    // Tüm istemcileri çevrimdışı göster ve sinyal yay
    for (auto it = m_clientStates.begin();
         it != m_clientStates.end(); ++it) {
        it->connected = false;
        if (m_thumbnails.contains(it.key())) {
            m_thumbnails[it.key()]->updateState(*it);
        }
        emit clientDisconnected(it.key());
    }

    // Eğer yetki kısıtlamasından (kicked) düşmediysek, normal kopmalarda offline moda al
    if (!m_isKickedToOffline) {
        m_isOfflineMode = true;
    }

    emit brokerDisconnected();
    emit connectionModeChanged(connectionMode());

    // mDNS taramasını derhal sıfırlayıp yeniden başlat — yeniden başlayan veya IP'si değişen broker'ı anında yakalasın
    if (m_mdns) {
        m_mdns->restartBrowsing("_okidia._tcp");
    }

    // Kopma durumunda eğer Uzak/Tailscale host yapılandırılmışsa, 5 saniye sonra fallback timer'ı tetikle
    AppSettings cfg = AppSettings::load();
    QString fallbackHost = cfg.remoteBrokerHost.trimmed();
    if (fallbackHost.isEmpty()) {
        fallbackHost = OkidiaConfig::instance().brokerIp().trimmed();
    }
    if (!fallbackHost.isEmpty() && !m_isKickedToOffline && isReachableOrValidRemoteHost(fallbackHost)) {
        if (!m_remoteProbeTimer) {
            m_remoteProbeTimer = new QTimer(this);
            m_remoteProbeTimer->setSingleShot(true);
            connect(m_remoteProbeTimer, &QTimer::timeout, this, &SessionManager::tryRemoteBrokerFallback);
        }
        // Yerel broker yeniden başlatılıyorsa (OTA veya servis restart) yerel mDNS ve auto-reconnect'e en az 15 sn tanı.
        int waitTime = m_isRemoteConnection ? 5000 : 15000;
        m_remoteProbeTimer->start(waitTime);
    }
}


bool SessionManager::sendAgentTypeSet(const QString& clientId, uint32_t type) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::Envelope env;
    env.set_type(okidia::MessageType::AGENT_TYPE_SET);
    env.set_payload(std::to_string(type));
    env.set_target_id(clientId.toStdString());

    // Yerel state ve arayüzü anında güncelle
    if (m_clientStates.contains(clientId)) {
        m_clientStates[clientId].agentType = type;
        if (m_thumbnails.contains(clientId)) {
            m_thumbnails[clientId]->updateState(m_clientStates[clientId]);
        }
        emit clientStateChanged(clientId, m_clientStates[clientId]);
    } else {
        for (auto it = m_clientStates.begin(); it != m_clientStates.end(); ++it) {
            if (it.value().machineId == clientId) {
                it.value().agentType = type;
                if (m_thumbnails.contains(it.key())) {
                    m_thumbnails[it.key()]->updateState(it.value());
                }
                emit clientStateChanged(it.key(), it.value());
                break;
            }
        }
    }

    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendAgentAliasSet(const QString& machineId, const QString& alias) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::AgentAliasSet msg;
    msg.set_machine_id(machineId.toStdString());
    msg.set_alias(alias.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AGENT_ALIAS_SET);
    env.set_payload(msg.SerializeAsString());
    env.set_target_id(machineId.toStdString());
    bool ok = m_conn->sendEnvelope(env);

    // Yerel durumu anında güncelle
    if (m_clientStates.contains(machineId)) {
        m_clientStates[machineId].alias = alias;
        if (m_thumbnails.contains(machineId)) {
            m_thumbnails[machineId]->updateState(m_clientStates[machineId]);
        }
        emit clientStateChanged(machineId, m_clientStates[machineId]);
    } else {
        for (auto it = m_clientStates.begin(); it != m_clientStates.end(); ++it) {
            if (it.value().machineId == machineId) {
                it.value().alias = alias;
                if (m_thumbnails.contains(it.key())) {
                    m_thumbnails[it.key()]->updateState(it.value());
                }
                emit clientStateChanged(it.key(), it.value());
                break;
            }
        }
    }
    return ok;
}

bool SessionManager::sendBrokerSettingsUpdate(int retentionDays, int backupRetentionCount, bool includeChat, 
                                              const QString& backupPassword,
                                              const QString& schoolScheduleJson,
                                              const QString& schoolTimezone,
                                              uint32_t smartboardLockPolicy) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::BrokerSettingsUpdate bsu;
    bsu.set_audit_retention_days(retentionDays);
    bsu.set_auto_backup_retention_count(backupRetentionCount);
    bsu.set_auto_backup_include_chat(includeChat);
    if (!backupPassword.isNull()) {
        bsu.set_backup_password(backupPassword.toStdString());
        bsu.set_has_backup_password(true);
    }
    if (!schoolScheduleJson.isEmpty()) {
        bsu.set_school_schedule_json(schoolScheduleJson.toStdString());
    }
    if (!schoolTimezone.isEmpty()) {
        bsu.set_school_timezone(schoolTimezone.toStdString());
    }
    if (smartboardLockPolicy > 0) {
        bsu.set_smartboard_lock_policy(smartboardLockPolicy);
    }
    
    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROKER_SETTINGS_UPDATE);
    env.set_payload(bsu.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

bool SessionManager::requestDbBackup() {
    if (!m_conn || !m_conn->isConnected()) return false;
    if (m_isSyncingDb) {
        Logger::info(QObject::tr("SessionManager: Veritabanı senkronizasyonu zaten devam ediyor, mükerrer istek atlanıyor."));
        return false;
    }

    m_isSyncingDb = true;
    QTimer::singleShot(10000, this, [this]() {
        if (m_isSyncingDb) {
            m_isSyncingDb = false;
        }
    });

    okidia::DbBackupRequest req;
    req.set_requester_user_id(m_activeUser.id.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_BACKUP_REQUEST);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

bool SessionManager::requestSystemBackup(const QString& targetDir, const QString& password, bool includeAudit, bool includeChat) {
    if (!m_conn || !m_conn->isConnected()) return false;
    
    m_isManualBackup = true;
    m_manualBackupDir = targetDir;
    m_manualBackupPassword = password;
    m_manualBackupIncludeAudit = includeAudit;
    m_manualBackupIncludeChat = includeChat;
    
    okidia::DbBackupRequest req;
    req.set_requester_user_id(m_activeUser.id.toStdString());
    req.set_include_audit(includeAudit);
    req.set_include_chat(includeChat);
    
    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_BACKUP_REQUEST);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

bool SessionManager::sendDbRestoreRequest(const QByteArray& encryptedData, const QByteArray& brokerKey, 
                                          const QMap<QString, QByteArray>& identityBundle, const QString& setupCode,
                                          const QByteArray& encryptedAuditData, const QByteArray& encryptedChatData,
                                          bool restoreAudit, bool restoreChat) {
    if (!m_conn || !m_conn->isConnected()) return false;
    okidia::DbRestoreRequest req;
    req.set_encrypted_db(encryptedData.toStdString());
    if (!brokerKey.isEmpty()) {
        req.set_broker_key(brokerKey.toStdString());
    }
    if (!setupCode.isEmpty()) {
        req.set_setup_code(setupCode.toStdString());
    }
    if (!encryptedAuditData.isEmpty()) {
        req.set_encrypted_audit_db(encryptedAuditData.toStdString());
        req.set_restore_audit(restoreAudit);
    }
    if (!encryptedChatData.isEmpty()) {
        req.set_encrypted_chat_db(encryptedChatData.toStdString());
        req.set_restore_chat(restoreChat);
    }

    // Identity Bundle ekle
    for (auto it = identityBundle.begin(); it != identityBundle.end(); ++it) {
        auto* idFile = req.add_identity_bundle();
        idFile->set_name(it.key().toStdString());
        idFile->set_content(it.value().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_RESTORE_REQUEST);
    env.set_payload(req.SerializeAsString());
    return m_conn->sendEnvelope(env);
}

void SessionManager::sendDbSyncSignal() {
    if (!m_conn || !m_conn->isConnected()) return;
    okidia::Envelope env;
    env.set_type(okidia::MessageType::REFRESH_REQUEST);
    env.set_target_id("__sync_masters__");
    m_conn->sendEnvelope(env);
    Logger::info(QObject::tr("SessionManager: Diğer master'lara DB eşitleme sinyali gönderildi"));
}

void SessionManager::updateCachedPassword(const QString& hash) {
    m_passwordHash = hash;
    m_activeUser.passwordHash = hash;
}

void SessionManager::setPendingCredentials(const QString& username, const QString& hash) {
    m_username = username;
    m_passwordHash = hash;
    m_isOfflineMode = true;
    Logger::info(QObject::tr("SessionManager: Çevrimdışı (Offline) Mod için kimlik bilgileri askıya alındı → ") + username);
    emit connectionModeChanged(connectionMode());
}

bool SessionManager::isOfflineMode() const {
    return m_isOfflineMode;
}

SessionManager::ConnectionMode SessionManager::connectionMode() const {
    if (m_isOfflineMode || m_isKickedToOffline) {
        return ConnectionMode::SshOnly;
    }
    if (isConnected()) {
        return m_isRemoteConnection ? ConnectionMode::Remote : ConnectionMode::Local;
    }
    return ConnectionMode::Disconnected;
}

QString SessionManager::connectionModeString() const {
    switch (connectionMode()) {
        case ConnectionMode::Local:
            return tr("Yerel Ağ");
        case ConnectionMode::Remote:
            return tr("Uzak Ağ");
        case ConnectionMode::SshOnly:
            return tr("SSH Yönetimi");
        case ConnectionMode::Disconnected:
        default:
            return tr("Bağlantı Yok");
    }
}

QString SessionManager::connectionDetails() const {
    switch (connectionMode()) {
        case ConnectionMode::Local:
            return tr("Broker'a yerel ağ (mDNS) üzerinden bağlı:\n%1:%2")
                   .arg(m_tls ? m_tls->host() : "").arg(m_tls ? m_tls->port() : 0);
        case ConnectionMode::Remote:
            return tr("Broker'a uzak ağ (Tailscale/WAN) üzerinden bağlı:\n%1:%2")
                   .arg(m_tls ? m_tls->host() : "").arg(m_tls ? m_tls->port() : 0);
        case ConnectionMode::SshOnly:
            return tr("Broker çevrimdışı — Yalnızca doğrudan SSH ve yerel yönetim aktif.");
        case ConnectionMode::Disconnected:
        default:
            return tr("Broker aranıyor veya bağlantı kapalı.");
    }
}

void SessionManager::setHostOs(const QString& ip, const QString& osTag) {
    if (ip.isEmpty() || osTag.isEmpty() || osTag == "unknown") return;
    QString cleanOs = osTag.toLower();
    if (m_hostOsMap.value(ip) != cleanOs) {
        m_hostOsMap[ip] = cleanOs;
        emit hostOsUpdated(ip, cleanOs);
    }
}

QString SessionManager::hostOs(const QString& ip) const {
    if (ip.isEmpty()) return {};
    return m_hostOsMap.value(ip);
}

