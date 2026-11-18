// common/utils/QrCodeGenerator.h
#pragma once

#include <QString>
#include <QPixmap>
#include <QColor>

/**
 * QrCodeGenerator — Bağımsız, ultra hafif QR Kod üretici ve görsel çizici
 *
 * Harici C kütüphanesi bağımlılığı olmadan standart QR Kod (Version 1-4, ECC Low/Medium)
 * matrisini hesaplar ve QPixmap / QImage olarak rasterize eder.
 */
class QrCodeGenerator {
public:
    static QPixmap generateQrPixmap(const QString& text, int size = 256,
                                   const QColor& darkColor = Qt::black,
                                   const QColor& lightColor = Qt::white);
};

