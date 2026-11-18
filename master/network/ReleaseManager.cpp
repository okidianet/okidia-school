#include "ReleaseManager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include "../../common/utils/Logger.h"

QString ReleaseItem::downloadUrl(const QString& baseUrl) const {
    if (url.startsWith("http://") || url.startsWith("https://")) {
        return url;
    }
    QString base = baseUrl.trimmed();
    if (!base.endsWith('/')) {
        base += '/';
    }
    QString rel = url;
    if (rel.startsWith('/')) {
        rel = rel.mid(1);
    }
    return base + rel;
}

ReleaseManager::ReleaseManager(QObject* parent)
    : QObject(parent),
      m_baseUrl("https://okidia.net/"),
      m_netManager(new QNetworkAccessManager(this))
{
}

void ReleaseManager::setBaseUrl(const QString& url) {
    m_baseUrl = url.trimmed();
    if (!m_baseUrl.endsWith('/')) {
        m_baseUrl += '/';
    }
}

void ReleaseManager::fetchReleases() {
    QUrl endpoint(m_baseUrl + "api/releases.php");
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::UserAgentHeader, "OkidiaMaster/2.13 (Linux; ReleaseManager)");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onReplyFinished(reply);
    });
}

void ReleaseManager::onReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = tr("Sürüm listesi alınamadı: %1").arg(reply->errorString());
        Logger::warning("ReleaseManager: " + err);
        emit fetchFailed(err);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        QString err = tr("Geçersiz sunucu yanıtı (JSON ayrıştırma hatası): %1").arg(parseErr.errorString());
        Logger::warning("ReleaseManager: " + err);
        emit fetchFailed(err);
        return;
    }

    QJsonObject root = doc.object();
    m_categories.clear();
    m_releases.clear();

    // Kategorileri ayrıştır
    QJsonArray catArray = root.value("categories").toArray();
    for (const QJsonValue& val : catArray) {
        if (!val.isObject()) continue;
        QJsonObject cObj = val.toObject();
        ReleaseCategory cat;
        cat.id = cObj.value("id").toString();
        cat.name = cObj.value("name").toString();
        cat.os = cObj.value("os").toString();
        cat.status = cObj.value("status").toString();
        m_categories.append(cat);
    }

    // Kayıtlı sürümleri ayrıştır
    QJsonArray relArray = root.value("releases").toArray();
    for (const QJsonValue& val : relArray) {
        if (!val.isObject()) continue;
        QJsonObject rObj = val.toObject();
        ReleaseItem item;
        item.id = rObj.value("id").toInt();
        item.version = rObj.value("version").toString();
        item.category = rObj.value("category").toString();
        item.component = rObj.value("component").toString();
        item.arch = rObj.value("arch").toString();
        item.filename = rObj.value("name").toString();
        item.url = rObj.value("url").toString();
        item.sha256 = rObj.value("sha256").toString();
        item.sizeBytes = rObj.value("size_bytes").toVariant().toLongLong();
        item.sizeHuman = rObj.value("size_human").toString();
        item.releaseDate = rObj.value("date").toString();
        item.isRegistered = true;
        m_releases.append(item);
    }

    // Unregistered (otomatik taranan) dosyaları ayrıştır
    QJsonArray unregArray = root.value("unregistered_files").toArray();
    for (const QJsonValue& val : unregArray) {
        if (!val.isObject()) continue;
        QJsonObject uObj = val.toObject();
        ReleaseItem item;
        item.id = 0;
        item.category = uObj.value("category").toString();
        item.component = uObj.value("component").toString();
        item.arch = uObj.value("arch").toString();
        item.filename = uObj.value("filename").toString();
        item.url = uObj.value("url").toString();
        item.sha256 = uObj.value("sha256").toString();
        item.sizeHuman = uObj.value("size_human").toString();
        item.isRegistered = false;

        // Versiyonu dosya adından çıkarmaya çalış (Örn: okidia-agent_ubuntu24_0.1.669_amd64.deb veya setup-v0.1.655.exe)
        QRegularExpression vRegex(R"((?:v|_)([0-9]+\.[0-9]+\.[0-9]+))");
        auto match = vRegex.match(item.filename);
        if (match.hasMatch()) {
            item.version = match.captured(1);
        }

        m_releases.append(item);
    }

    Logger::info(QString("ReleaseManager: %1 kategori ve %2 sürüm başarıyla alındı.")
                 .arg(m_categories.size())
                 .arg(m_releases.size()));

    emit releasesFetched(m_releases, m_categories);
}

ReleaseItem ReleaseManager::findBestMatch(const QString& component, const QString& osTag, const QString& arch, const QString& preferredVersion) const {
    QString comp = component.toLower().trimmed();
    if (!comp.startsWith("okidia-")) {
        comp = "okidia-" + comp;
    }

    QString tag = osTag.toLower().trimmed();
    QString targetArch = arch.toLower().trimmed();
    if (targetArch == "x86_64" || targetArch == "x64") targetArch = "amd64";
    if (targetArch == "aarch64") targetArch = "arm64";

    // Windows eşlemesi
    bool isWindows = (tag.contains("win") || tag.contains("windows"));

    ReleaseItem bestItem;
    for (const ReleaseItem& item : m_releases) {
        if (item.component.toLower() != comp) {
            continue;
        }

        if (isWindows) {
            if (item.category.toLower() != "windows" && !item.filename.toLower().endsWith(".exe")) {
                continue;
            }
        } else {
            // Linux dağıtım eşleşmesi (kategori ubuntu24, debian12 vs.)
            if (!tag.isEmpty() && !item.category.isEmpty() && !item.category.toLower().contains(tag) && !tag.contains(item.category.toLower())) {
                continue;
            }
            // Mimari eşleşmesi
            if (!targetArch.isEmpty() && !item.arch.isEmpty()) {
                QString itemArch = item.arch.toLower();
                if (itemArch == "x86_64" || itemArch == "x64") itemArch = "amd64";
                if (itemArch == "aarch64") itemArch = "arm64";
                if (itemArch != targetArch) {
                    continue;
                }
            }
        }

        // Eğer tercih edilen bir versiyon varsa ve birebir tutuyorsa doğrudan seç
        if (!preferredVersion.isEmpty() && item.version == preferredVersion) {
            return item;
        }

        // Değilse bulunan ilk veya en yüksek sürümlü paketi bestItem yap
        if (bestItem.filename.isEmpty()) {
            bestItem = item;
        } else if (item.version > bestItem.version) {
            bestItem = item;
        }
    }

    return bestItem;
}
