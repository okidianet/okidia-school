#pragma once
#include <QByteArray>
#include <QString>

class CertSigner {
public:
    static QByteArray signCsr(const QByteArray& csrPem, QString& errOut, const QString& subject = QString());
};
