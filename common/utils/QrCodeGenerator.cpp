// common/utils/QrCodeGenerator.cpp
#include "QrCodeGenerator.h"
#include "qrcodegen.hpp"
#include <QPainter>
#include <cmath>

using qrcodegen::QrCode;

QPixmap QrCodeGenerator::generateQrPixmap(const QString& text, int size,
                                         const QColor& darkColor,
                                         const QColor& lightColor) {
    if (size <= 0) size = 256;

    // ISO/IEC 18004 uyumlu endüstri standardı Nayuki QR Code motoru
    // Orta seviye (ECC Medium) hata düzeltme ile sağlam tarama
    const QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
    int modCount = qr.getSize();

    // 4 modül standart sessiz alan (Quiet Zone)
    int quietZone = 4;
    int totalModules = modCount + quietZone * 2;

    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(lightColor);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    p.setBrush(darkColor);

    qreal modSize = static_cast<qreal>(size) / totalModules;

    for (int y = 0; y < modCount; ++y) {
        for (int x = 0; x < modCount; ++x) {
            if (qr.getModule(x, y)) {
                qreal px = (x + quietZone) * modSize;
                qreal py = (y + quietZone) * modSize;
                p.drawRect(QRectF(px, py, std::ceil(modSize), std::ceil(modSize)));
            }
        }
    }
    p.end();

    return QPixmap::fromImage(img);
}
