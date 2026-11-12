// agent/ui/BellToastBanner/BellToastBanner.h
#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QTimer;

/**
 * @brief BellToastBanner — Zil çaldığında (veya anons esnasında zil sesi bastırıldığında)
 * ekranın sağ üst köşesinde zarif, koyu temalı ve cam efektli bildirim gösteren Toast penceresi.
 */
class BellToastBanner : public QWidget {
    Q_OBJECT

public:
    static void showBanner(const QString& title, const QString& message, int durationMs = 8000);
    static void closeActive();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    explicit BellToastBanner(const QString& title, const QString& message, int durationMs, QWidget* parent = nullptr);
    ~BellToastBanner() override;

    void setupUi(const QString& title, const QString& message);
    void positionToTopRight();

    QTimer* m_dismissTimer = nullptr;
    static BellToastBanner* s_activeBanner;
};
