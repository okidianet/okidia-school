// agent/ui/SmartboardLockOverlay/SmartboardLockOverlay.h
#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>

/**
 * SmartboardLockOverlay — Akıllı Tahta (Tip 3) Dinamik QR + Dokunmatik Numpad Kilit Ekranı
 *
 * Sıfır ağ (Air-Gapped) öğretmen kimlik doğrulamasını sağlar.
 * Ekranda dinamik QR kod, kalan süre geri sayım göstergesi ve dokunmatik Numpad sunar.
 */
class SmartboardLockOverlay : public QWidget {
    Q_OBJECT

public:
    static SmartboardLockOverlay& instance();

    void showSmartboardLock(const QString& message = QString());
    void hideSmartboardLock();
    void setAlias(const QString& alias);

    bool isLocked() const { return m_locked; }

signals:
    void unlockRequestedByPin(const QString& pin);
    void unlockedByTeacher(const QString& username, const QString& displayName,
                           const QString& password = QString());
    void lockScreenClosed();

protected:
    void keyPressEvent(QKeyEvent* event)   override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event)  override;
    void closeEvent(QCloseEvent* event)    override;

private slots:
    void onNumpadClicked(const QString& digit);
    void onClearClicked();
    void onOkClicked();
    void onTickTimer();
    void updateGeometryToScreen();
    void onScreenAdded(QScreen* screen);
    void onScreenRemoved(QScreen* screen);
    void updateAllScreens();
    void onEjectUsb();

private:
    explicit SmartboardLockOverlay(QWidget* parent = nullptr);
    ~SmartboardLockOverlay() override = default;

    SmartboardLockOverlay(const SmartboardLockOverlay&) = delete;
    SmartboardLockOverlay& operator=(const SmartboardLockOverlay&) = delete;

    void setupUi();
    void updatePinDisplay();
    void refreshQrCode();

    bool m_locked = false;
    QString m_enteredPin;
    int m_remainingSeconds = 60;
    int m_xsetResetCounter = 0;

    // UI Bileşenleri
    QFrame* m_bannerFrame      = nullptr;
    QLabel* m_logoLabel        = nullptr;
    QLabel* m_schoolLabel      = nullptr;
    QLabel* m_aliasLabel       = nullptr;
    QLabel* m_qrLabel          = nullptr;
    QLabel* m_timerLabel       = nullptr;
    QLabel* m_pinDotsLabel     = nullptr;
    QLabel* m_statusLabel      = nullptr;
    QPushButton* m_btnEjectUsb = nullptr;
    QWidget* m_numpadContainer = nullptr;

    QTimer* m_tickTimer        = nullptr;
    QTimer* m_topTimer         = nullptr;

    // İkincil ekranlar (Projeksiyon / Yan Monitör) için karartma perdeleri
    QList<QWidget*> m_extraOverlays;
};
