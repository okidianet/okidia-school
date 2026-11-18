// master/ui/BroadcastOverlay/BroadcastOverlayWidget.h
#pragma once

#include <QWidget>
#include <QTimer>

class QLabel;
class QSlider;
class QPushButton;
class QScreen;
class QHBoxLayout;

/**
 * Canlı ekran yayını sırasında öğretmenin ekranının üst-orta kısmında duran,
 * fare üzerine geldiğinde aşağı doğru sarkan (auto-hide / drop-down) açılır-kapanır
 * canlı yayın kontrol ve anlık kalite değiştirme çubuğu.
 */
class BroadcastOverlayWidget : public QWidget {
    Q_OBJECT

public:
    explicit BroadcastOverlayWidget(QWidget* parent = nullptr);
    ~BroadcastOverlayWidget() override = default;

    void setTargetScreen(QScreen* screen);
    void setInitialQuality(int quality);
    void showOverlay();
    void hideOverlay();

signals:
    void qualityChanged(int quality);
    void stopRequested();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void onSliderValueChanged(int value);
    void onPinToggled(bool checked);
    void onCollapseTimeout();

private:
    void setupUi();
    void expand();
    void collapse();
    void updateQualityLabel(int value);
    void reposition();

    QScreen*     m_targetScreen = nullptr;
    QTimer*      m_collapseTimer = nullptr;
    bool         m_isExpanded = false;
    bool         m_isPinned = false;
    bool         m_isSliderDragging = false;

    QWidget*     m_container = nullptr;
    QHBoxLayout* m_mainLayout = nullptr;

    // Collapsed (Daraltılmış) durum bileşenleri
    QWidget*     m_collapsedBar = nullptr;
    QLabel*      m_lblCollapsed = nullptr;

    // Expanded (Genişletilmiş) durum bileşenleri
    QWidget*     m_expandedBar = nullptr;
    QLabel*      m_lblLive = nullptr;
    QLabel*      m_lblQualityTitle = nullptr;
    QSlider*     m_sliderQuality = nullptr;
    QLabel*      m_lblQualityVal = nullptr;
    QPushButton* m_btnPin = nullptr;
    QPushButton* m_btnStop = nullptr;
};

