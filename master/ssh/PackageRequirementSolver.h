#ifndef PACKAGE_REQUIREMENT_SOLVER_H
#define PACKAGE_REQUIREMENT_SOLVER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include "../network/ReleaseManager.h"

enum class PackageStatus {
    Ready,          // Dosya yerelde var ve SHA-256 doğrulandı
    Missing,        // Dosya yerelde yok, indirilmeli
    HashMismatch,   // Dosya yerelde var fakat boyutu veya SHA-256 uyuşmuyor
    Unavailable     // Resmi portalda buna uygun paket bulunamadı
};

struct TargetNode {
    QString id;       // IP veya Hostname veya Agent UUID
    QString name;     // Görünen isim
    QString osTag;    // "ubuntu24", "debian12", "windows", vs.
    QString arch;     // "amd64", "arm64"
    QString role;     // "agent", "broker", "master"
};

struct PackageRequirement {
    QString component;          // "okidia-agent", "okidia-broker", "okidia-master"
    QString osTag;              // "ubuntu24", "debian12", "windows"
    QString arch;               // "amd64", "arm64"
    QString expectedFilename;
    QString downloadUrl;
    QString sha256;
    qint64 sizeBytes = 0;
    QString sizeHuman;
    QString localFilePath;
    PackageStatus status = PackageStatus::Missing;
    QList<TargetNode> targetNodes;

    QString statusText() const;
};

class PackageRequirementSolver : public QObject {
    Q_OBJECT
public:
    explicit PackageRequirementSolver(QObject* parent = nullptr);
    ~PackageRequirementSolver() override = default;

    /**
     * Taranan hedef makineleri ve yerel klasördeki dosyaları ReleaseManager verileriyle
     * karşılaştırarak tekilleştirilmiş gereksinim listesi oluşturur.
     */
    static QList<PackageRequirement> solve(
        const QList<TargetNode>& nodes,
        const QList<ReleaseItem>& availableReleases,
        const QStringList& searchDirectories = QStringList()
    );

    /**
     * Belirtilen dosyanın SHA-256 hash'ini hesaplar.
     */
    static QString computeSha256(const QString& filePath);

    /**
     * Standart yerel paket önbellek dizinini döner (~/.cache/okidia/packages/)
     */
    static QString defaultPackageCacheDir();
};

#endif // PACKAGE_REQUIREMENT_SOLVER_H
