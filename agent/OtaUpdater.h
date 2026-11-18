#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <QObject>
#include <QString>

class OtaUpdater : public QObject {
    Q_OBJECT
public:
    explicit OtaUpdater(QObject* parent = nullptr);

    // Yerel kararlı paket yolu
    static QString cachedPackagePath();

    // Yerel kararlı paketi kurarak geri alma işlemini tetikle
    static bool executeRollback(QString* errorMsg = nullptr);

public slots:
    void installUpdate(const QString& debPath, const QByteArray& signature, const QByteArray& expectedHash, const QByteArray& masterCert);
};

#endif // OTA_UPDATER_H

