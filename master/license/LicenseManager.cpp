// master/license/LicenseManager.cpp
#include "LicenseManager.h"
#include "../../common/utils/Logger.h"

#include <QFile>
#include <QTimer>
#include <QTimeZone>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

LicenseManager& LicenseManager::instance() {
    static LicenseManager s;
    return s;
}

LicenseManager::LicenseManager(QObject* parent)
    : QObject(parent) {}

// ---------------------------------------------------------------------------
bool LicenseManager::load(const QString& certPath) {
    m_certPath = certPath;
    parseFromCert(certPath);
    checkExpiry();

    if (m_info.valid) {
        Logger::info(QObject::tr("LicenseManager: Lisans yüklendi → ") +
                     m_info.schoolName + " [" +
                     (m_info.type == LicenseType::Campus
                          ? tr("Kampüs") : tr("Sınıf")) + "]");
    } else {
        Logger::warning(QObject::tr("LicenseManager: Geçersiz lisans → ") + certPath);
    }
    return m_info.valid;
}

void LicenseManager::parseFromCert(const QString& certPath) {
    // X.509 sertifikasını oku
    QFile f(certPath);
    if (!f.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("LicenseManager: Sertifika açılamadı → ") + certPath);
        return;
    }
    QByteArray pemData = f.readAll();
    f.close();

    BIO* bio = BIO_new_mem_buf(pemData.constData(),
                                pemData.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!cert) {
        Logger::error(QObject::tr("LicenseManager: Sertifika parse hatası"));
        return;
    }

    // Subject CN → okul adı
    const auto* subject = X509_get_subject_name(cert);
    char cn[256] = {};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    X509_NAME_get_text_by_NID(const_cast<X509_NAME*>(subject), NID_commonName, cn, sizeof(cn));
    m_info.schoolName = QString::fromUtf8(cn);

    // Geçerlilik tarihi
    const ASN1_TIME* notAfter = X509_get0_notAfter(cert);
    struct tm t = {};
    ASN1_TIME_to_tm(notAfter, &t);
    m_info.expiresAt = QDateTime(
        QDate(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday),
        QTime(t.tm_hour, t.tm_min, t.tm_sec),
        QTimeZone(0));

    // Özel extension'lar: okidia-license-type, okidia-max-clients
    // OID: 1.3.6.1.4.1.99999.1 (örnek — gerçek OID kayıt edilecek)
    // Şimdilik Subject OU alanından okuyoruz
    char ou[256] = {};
    X509_NAME_get_text_by_NID(const_cast<X509_NAME*>(subject), NID_organizationalUnitName,
                               ou, sizeof(ou));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    QString ouStr = QString::fromUtf8(ou).toLower();

    if (ouStr.contains("campus")) {
        m_info.type       = LicenseType::Campus;
        m_info.maxClients = INT_MAX;
        m_info.maxTeachers= INT_MAX;
    } else {
        m_info.type        = LicenseType::Free;
        m_info.maxClients  = 50;
        m_info.maxTeachers = 1;
    }

    X509_free(cert);
    m_info.valid = true;
}

void LicenseManager::checkExpiry() {
    if (!m_info.valid) return;

    // Free lisansta bitiş tarihi yok
    if (m_info.type == LicenseType::Free) return;

    if (!m_info.expiresAt.isValid()) return;

    QDateTime now = QDateTime::currentDateTimeUtc();
    int daysLeft  = static_cast<int>(now.daysTo(m_info.expiresAt));

    if (daysLeft < 0) {
        Logger::warning(QObject::tr("LicenseManager: Lisans süresi dolmuş!"));
        emit licenseExpired();
    } else if (daysLeft <= 14) {
        Logger::warning(QObject::tr("LicenseManager: Lisans ") +
                        QString::number(daysLeft) + tr(" gün sonra doluyor!"));
        emit licenseExpiringSoon(daysLeft);
    }
}

bool LicenseManager::isValid() const {
    if (!m_info.valid) return false;
    if (m_info.type == LicenseType::Free) return true;

    // Grace period kontrolü
    if (m_info.expiresAt.isValid()) {
        QDateTime now = QDateTime::currentDateTimeUtc();
        if (now > m_info.expiresAt.addDays(GRACE_DAYS))
            return false;
    }
    return true;
}

bool LicenseManager::isExpired() const {
    if (m_info.type == LicenseType::Free) return false;
    if (!m_info.expiresAt.isValid()) return false;
    return QDateTime::currentDateTimeUtc() > m_info.expiresAt;
}

bool LicenseManager::canAddClient() const {
    // Gerçek istemci sayısı SessionManager'dan alınacak — şimdi sadece limit kontrolü
    return m_info.maxClients == INT_MAX; // Free: SessionManager kontrol eder
}

bool LicenseManager::canAddTeacher() const {
    return m_info.maxTeachers == INT_MAX;
}

void LicenseManager::startPeriodicSync(const QString& serverUrl) {
    m_serverUrl = serverUrl;

    // Hemen bir kez dene, sonra 24 saatte bir
    QTimer* timer = new QTimer(this);
    timer->setInterval(24 * 60 * 60 * 1000); // 24 saat
    connect(timer, &QTimer::timeout,
            this, &LicenseManager::syncWithServer);
    timer->start();

    // İlk sync 30 saniye sonra (startup tamamlandıktan sonra)
    QTimer::singleShot(30000, this, &LicenseManager::syncWithServer);
}

void LicenseManager::syncWithServer() {
    if (m_serverUrl.isEmpty() || m_certPath.isEmpty()) return;

    auto* manager = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(m_serverUrl + "/api/validate"));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/json");

    QJsonObject body;
    body["serial"] = m_info.serialNumber;
    body["school"] = m_info.schoolName;

    auto* reply = manager->post(
        req, QJsonDocument(body).toJson());

    connect(reply, &QNetworkReply::finished, [this, reply, manager]() {
        if (reply->error() == QNetworkReply::NoError) {
            m_lastSync = QDateTime::currentDateTime();
            QSettings s("okidia", "license");
            s.setValue("last_sync", m_lastSync.toString(Qt::ISODate));
            Logger::info(QObject::tr("LicenseManager: Sunucu sync başarılı"));
            emit licenseUpdated();
        } else {
            Logger::warning(QObject::tr("LicenseManager: Sunucu erişilemedi → ") +
                            reply->errorString());
            // Grace period kontrolü
            checkExpiry();
        }
        reply->deleteLater();
        manager->deleteLater();
    });
}
