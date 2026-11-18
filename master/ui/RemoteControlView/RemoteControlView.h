// master/ui/RemoteControlView/RemoteControlView.h
#pragma once

#include <QWidget>
#include <QRect>
#include <QPixmap>
#include <QElapsedTimer>
#include <QTimer>
#include <QLabel>
#include <QMap>
#include "../../session/SessionManager.h"

#include "messages.pb.h"

class SshFileManagerWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class RemoteControlView; }
QT_END_NAMESPACE

class RemoteControlView : public QWidget {
    Q_OBJECT

public:
    enum class Mode { ViewOnly, Control };

    explicit RemoteControlView(SessionManager* session,
                                const QString& clientId,
                                QWidget* parent = nullptr,
                                Mode mode = Mode::Control);
    ~RemoteControlView();

    void updateFrame(const QByteArray& jpegData);
    void updateTile(int x, int y, int w, int h,
                    const QByteArray& jpegData);
    void updateRemoteFrame(const QByteArray& jpegData, int w, int h);
    void updateRemoteCursor(int cursorId, int x, int y);
    void refreshDisplay();
    void setStreamSize(int w, int h);

    QString clientId() const { return m_clientId; }
    void    setTitle(const QString& title);
    void    updateMonitorList(const okidia::ScreenInfoResponse& info);
    Mode    mode()     const { return m_mode; }
    void    setMode(Mode mode);
    void    setCropRect(const QRect& rect, const QSize& nativeTotalSize = QSize());
    void    clearCropRect() { m_cropRect = QRect(); m_nativeCropRect = QRect(); m_nativeTotalSize = QSize(); }
    void    setBroadcastNotice(bool active, const QString& teacherName = QString());

signals:
    void stopped(const QString& clientId);
    void requestSshTerminal(const QString& clientId);

protected:
    bool eventFilter(QObject* obj, QEvent* event)   override;
    void keyPressEvent(QKeyEvent* event)        override;
    void keyReleaseEvent(QKeyEvent* event)      override;
    void mouseMoveEvent(QMouseEvent* event)     override;
    void mousePressEvent(QMouseEvent* event)    override;
    void mouseReleaseEvent(QMouseEvent* event)  override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event)         override;
    void resizeEvent(QResizeEvent* event)       override;
    void changeEvent(QEvent* event)             override;
    void closeEvent(QCloseEvent* event)         override;
    void focusOutEvent(QFocusEvent* event)      override;
    bool focusNextPrevChild(bool next)          override;

private slots:
    void onStopClicked();
    void onFullscreenClicked();
    void onViewOnlyClicked();
    void onControlClicked();
    void onLockAgentInputClicked();
    void onSourceChanged(int index);
    void onQualityChanged(int index);
    void onSshClicked();

private:
    Ui::RemoteControlView* ui;
    SessionManager* m_session  = nullptr;
    QString         m_clientId;
    Mode            m_mode     = Mode::Control;
    bool            m_isFullscreen = false;
    QRect           m_savedGeometry;   // Tam ekran öncesi geometri

    int     m_remoteW = 1920;
    int     m_remoteH = 1080;
    QRect   m_cropRect;        // Panoramic görüntüden hangi parçayı göstereceğiz (Scaled)
    QRect   m_nativeCropRect;  // Orijinal çözünürlükteki kırpma alanı
    QSize   m_nativeTotalSize; // Orijinal çözünürlükteki toplam boyut
    QPixmap m_frameBuffer;

    class QLabel* m_remoteCursorLabel = nullptr;
    class QLabel* m_broadcastNoticeLabel = nullptr;
    QElapsedTimer m_mouseThrottleTimer;
    QElapsedTimer m_rightClickCooldown;  // Sağ tık sonrası fare hareketi engeli
    Qt::MouseButtons m_pressedButtons = Qt::NoButton;

    QPoint toRemote(const QPoint& widgetPos) const;
    QRect imageRect() const;
    static int qtButtonToX11(Qt::MouseButton btn);
    void renderFrame();
    void updateModeButtons();
    void showStreamOverlay(bool show);
    void releaseAllButtons();
    void releaseAllKeys();
    void sendMouseRelease(Qt::MouseButton btn);
    void setupShortcutsMenu();
    void sendKeyCombination(const QList<uint32_t>& keysyms);

    // Yeniden bağlanma overlay'i
    QLabel*  m_streamOverlay   = nullptr;
    QTimer*  m_frameWatchdog   = nullptr;  // Frame timeout tespiti (3 sn)
    bool     m_streamConnected = true;

    class QComboBox* m_cbQuality = nullptr; // Kalite seçici
    QString          m_baseTitle;
    void             updateTitle();

    SshFileManagerWidget* m_fileManagerWindow = nullptr;
    QMap<int, uint32_t>  m_pressedKeysyms;
};

