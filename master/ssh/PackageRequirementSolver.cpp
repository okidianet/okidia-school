#include "PackageRequirementSolver.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QStandardPaths>
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

QString PackageRequirement::statusText() const {
    switch (status) {
        case PackageStatus::Ready:
            return QObject::tr("Kullanıma Hazır (Doğrulandı)");
        case PackageStatus::Missing:
            return QObject::tr("İndirilmeli");
        case PackageStatus::HashMismatch:
            return QObject::tr("Bozuk / Hash Uyuşmuyor");
        case PackageStatus::Unavailable:
            return QObject::tr("Uygun Paket Bulunamadı");
    }
    return QString();
}

PackageRequirementSolver::PackageRequirementSolver(QObject* parent)
    : QObject(parent)
{
}

QString PackageRequirementSolver::defaultPackageCacheDir() {
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        cacheRoot = QDir::homePath() + "/.cache/okidia";
    }
    QString pkgDir = cacheRoot + "/packages";
    QDir().mkpath(pkgDir);
    return pkgDir;
}

QString PackageRequirementSolver::computeSha256(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&file)) {
        return hash.result().toHex().toLower();
    }
    return QString();
}

QList<PackageRequirement> PackageRequirementSolver::solve(
    const QList<TargetNode>& nodes,
    const QList<ReleaseItem>& availableReleases,
    const QStringList& searchDirectories
) {
    QStringList searchDirs = searchDirectories;
    QString defDir = defaultPackageCacheDir();
    if (!searchDirs.contains(defDir)) {
        searchDirs.prepend(defDir);
    }

    // Hedef düğümleri component + osTag + arch anahtarıyla grupla
    QMap<QString, PackageRequirement> reqMap;

    for (const TargetNode& node : nodes) {
        QString comp = node.role.toLower().trimmed();
        if (comp.isEmpty()) comp = "agent";
        if (!comp.startsWith("okidia-")) comp = "okidia-" + comp;

        QString tag = node.osTag.toLower().trimmed();
        QString arch = OkidiaUtils::normalizeArch(node.arch);

        QString key = QString("%1_%2_%3").arg(comp, tag, arch);

        if (!reqMap.contains(key)) {
            PackageRequirement req;
            req.component = comp;
            req.osTag = tag;
            req.arch = arch;
            req.status = PackageStatus::Missing;
            reqMap.insert(key, req);
        }

        reqMap[key].targetNodes.append(node);
    }

    // Her gereksinim için ReleaseItem bul ve yerel dosyaları denetle
    QList<PackageRequirement> resultList;

    for (auto it = reqMap.begin(); it != reqMap.end(); ++it) {
        PackageRequirement req = it.value();

        // 1. ReleaseManager havuzundan en uygun ReleaseItem'ı eşleştir
        ReleaseItem matchedRelease;
        for (const ReleaseItem& rel : availableReleases) {
            if (rel.component.toLower() != req.component.toLower()) continue;

            bool isWin = (req.osTag.contains("win") || req.osTag.contains("windows"));
            if (isWin) {
                if (rel.category.toLower() == "windows" || rel.filename.toLower().endsWith(".exe")) {
                    matchedRelease = rel;
                    break;
                }
            } else {
                bool catMatch = req.osTag.isEmpty() || rel.category.toLower().contains(req.osTag) || req.osTag.contains(rel.category.toLower());
                bool archMatch = req.arch.isEmpty() || (OkidiaUtils::normalizeArch(rel.arch) == req.arch);
                if (catMatch && archMatch) {
                    matchedRelease = rel;
                    break;
                }
            }
        }

        if (!matchedRelease.filename.isEmpty()) {
            req.expectedFilename = matchedRelease.filename;
            req.downloadUrl = matchedRelease.downloadUrl();
            req.sha256 = matchedRelease.sha256.toLower();
            req.sizeBytes = matchedRelease.sizeBytes;
            req.sizeHuman = matchedRelease.sizeHuman;
        }

        // 2. Yerel dizinlerde arama yap
        QString foundPath;
        for (const QString& sDir : searchDirs) {
            if (sDir.isEmpty() || !QDir(sDir).exists()) continue;

            // Tam beklenen dosya adı varsa
            if (!req.expectedFilename.isEmpty()) {
                QString fullCandidate = QDir(sDir).filePath(req.expectedFilename);
                if (QFile::exists(fullCandidate)) {
                    foundPath = fullCandidate;
                    break;
                }
            }

            // Regex ile eşleşen dosya var mı tara
            QRegularExpression rx;
            if (req.component == "okidia-broker") {
                rx = QRegularExpression(OkidiaUtils::getBrokerPackageRegex(req.osTag, req.arch));
            } else if (req.component == "okidia-master") {
                rx = QRegularExpression(OkidiaUtils::getMasterPackageRegex(req.osTag, req.arch));
            } else {
                rx = QRegularExpression(OkidiaUtils::getAgentPackageRegex(req.osTag, req.arch));
            }

            QDir dir(sDir);
            QStringList files = dir.entryList(QDir::Files);
            for (const QString& f : files) {
                if (rx.match(f).hasMatch()) {
                    foundPath = dir.filePath(f);
                    break;
                }
            }
            if (!foundPath.isEmpty()) break;
        }

        if (!foundPath.isEmpty()) {
            req.localFilePath = foundPath;
            if (!req.sha256.isEmpty()) {
                QString localSha = computeSha256(foundPath);
                if (localSha == req.sha256) {
                    req.status = PackageStatus::Ready;
                } else {
                    req.status = PackageStatus::HashMismatch;
                    Logger::warning(QString("PackageRequirementSolver: Hash uyuşmazlığı tespit edildi! Dosya: %1 (Beklenen: %2, Yerel: %3)")
                                 .arg(foundPath, req.sha256, localSha));
                }
            } else {
                // Sunucuda SHA-256 yoksa dosyanın varlığı yeterli kabul edilir
                req.status = PackageStatus::Ready;
            }
        } else {
            if (req.expectedFilename.isEmpty() && req.downloadUrl.isEmpty()) {
                req.status = PackageStatus::Unavailable;
            } else {
                req.status = PackageStatus::Missing;
            }
        }

        resultList.append(req);
    }

    return resultList;
}
