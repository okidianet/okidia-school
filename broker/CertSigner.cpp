#include "CertSigner.h"
#include "../common/utils/Logger.h"
#include "../common/crypto/KeyManager.h"
#include "../common/OkidiaConfig.h"

QByteArray CertSigner::signCsr(const QByteArray& csrPem, QString& errOut, const QString& subject)
{
    Q_UNUSED(subject);
    const QString caKey = OkidiaConfig::brokerCertDir() + "/ca.key";
    const QString caCrt = OkidiaConfig::brokerCertDir() + "/ca.crt";

    QByteArray cert = KeyManager::signCsr(csrPem, caKey, caCrt, &errOut);
    if (!cert.isEmpty()) {
        Logger::info(QObject::tr("Broker: CSR başarıyla imzalandı (in-memory OpenSSL), boyut=") + QString::number(cert.size()));
        return cert;
    }

    Logger::error(QObject::tr("Broker: CSR imzalama hatası → ") + errOut);
    return {};
}
