// agent/ui/ScreenLockOverlay/ScreenLockOverlay.h
#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class ScreenLockOverlay; }
QT_END_NAMESPACE

/**
 * Öğrenci ekranını kapatan tam ekran siyah overlay.
 *
 * - Her monitör için ayrı bir overlay penceresi oluşturulur.
 * - showLock() → tüm ekranları kapla, klavye/mouse focus'u al.
 * - hideLock() → overlay'leri gizle.
 * - setMessage() → öğrenciye gösterilecek metni güncelle.
 *
 * Kullanım:
 *   ScreenLockOverlay::instance().showLock("Sınav başladı, bilgisayarınızı kullanmayın.");
 *   ScreenLockOverlay::instance().hideLock();
 */
class ScreenLockOverlay : public QWidget {
    Q_OBJECT

public:
    // Singleton erişimi (tek overlay koordinatörü)
    static ScreenLockOverlay& instance();

    // Tüm monitörleri kapla
    void showLock(const QString& message = QString());

    // Kilidi kaldır
    void hideLock();

    // Mesajı güncelle (kilitteyken de çağrılabilir)
    void setMessage(const QString& message);

    bool isLocked() const { return m_locked; }

protected:
    // Overlay üzerinde klavye/mouse/touch olaylarını yut
    bool event(QEvent* event)              override;
    void keyPressEvent(QKeyEvent* event)   override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event)    override;

private:
    explicit ScreenLockOverlay(QWidget* parent = nullptr);
    ~ScreenLockOverlay();

    // Singleton kopyalanamaz
    ScreenLockOverlay(const ScreenLockOverlay&)            = delete;
    ScreenLockOverlay& operator=(const ScreenLockOverlay&) = delete;

    Ui::ScreenLockOverlay* ui;
    bool m_locked = false;
    QString m_currentMessage;

    // Çoklu monitör: ekran başına bir pencere
    QList<ScreenLockOverlay*> m_extraScreens;

    class QTimer* m_topTimer = nullptr;

    void coverAllScreens(const QString& message);
    void coverScreen(int screenIndex, const QString& message);

private slots:
    void onScreenAdded(QScreen* screen);
    void onScreenRemoved(QScreen* screen);
    void updateScreens();
};
