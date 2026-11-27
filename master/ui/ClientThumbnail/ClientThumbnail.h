// master/ui/ClientThumbnail/ClientThumbnail.h
#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPainter>
#include <QPoint>      // ← YENİ: drag başlangıç noktası için

QT_BEGIN_NAMESPACE
namespace Ui { class ClientThumbnail; }
QT_END_NAMESPACE

struct ClientState {
    QString clientId;
    QString machineId;   // machine-id (revoke için gerekli)
    QString hostname;
    QString username;
    QString userDisplayName;
    QString alias;
    QString ip;          // IPv4 adresi (subnet görünümü için)
    QString mac;         // MAC adresi (Wake-on-LAN için)
    bool    connected    = false;
    bool    screenLocked = false;
    bool    inputLocked  = false;
    bool    webFiltered  = false;
    bool    appBlocked   = false;
    bool    chatActive   = false;
    bool    examActive   = false;
    bool    handRaised   = false;
    QString handRaiseMessage;
    bool    audioMuted   = false;
    bool    selected     = false;
    bool    sshConfigured = false; // SSH yetkisi var mı?
    bool    rtcBatteryLow = false; // BIOS pili bitik / sistem saati hatalı uyarısı
    int     agentType    = 0;
    QString osTag;
    QString osPrettyName;
    QString osArch;
    QString agentVersion;
    QByteArray initialFrame; // İlk tam ekran karesini sakla
    QString lockType;
    QString lockedBy;
    QString lockedByDisplayName;

    uint32_t cpuUsage = 0;
    uint32_t ramUsage = 0;
    uint32_t diskUsage = 0;
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    QString screens; // örn: "1920x1080 / 1366x768"
};

class ClientThumbnail : public QWidget {
    Q_OBJECT

public:
    explicit ClientThumbnail(const QString& clientId,
                              QWidget* parent = nullptr);
    ~ClientThumbnail();

    // Frame güncelleme
    void updateFrame(const QByteArray& jpegData);
    void updateTile(int x, int y, int w, int h,
                    const QByteArray& jpegData,
                    bool showNow = true);  // false → batch modda render atlanır
    void refreshDisplay();                 // Batch sonrası tek seferde ekrana yansıt
    void clearFrameBuffer();               // KVKK: Ekran karesi ve bellek önbelleğini derhal sıfırla
    void updateState(const ClientState& state);

    // MainWindow ve SessionManager erişimi
    void    setSelected(bool selected);
    void    setFrameSize(int width, int height);
    void    setStreamingMode(bool streaming);
    void    setBrokerVersion(const QString& ver);
    bool    isSelected()  const { return m_state.selected; }
    bool    hasValidFrame() const { return m_hasValidFrame; }
    int     frameWidth()  const { return m_frameBuffer.width(); }
    QString clientId()    const { return m_state.clientId; }
    ClientState state()   const { return m_state; }

    static void hideLiveTooltip();

signals:
    void clicked(const QString& clientId);
    void doubleClicked(const QString& clientId);
    void metricsStreamRequested(const QString& clientId, bool start);
    void contextMenuRequested(const QString& clientId,
                               const QPoint& globalPos);
    void clickedWithModifiers(const QString& clientId,
                               Qt::KeyboardModifiers modifiers);
    void dragInitiated(const QString& clientId);   // ← YENİ: sürükleme başladı
    void handRaiseClicked(const QString& clientId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;      // ← YENİ
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void showPixmap();
    void updateStatusIcons();
    void updateSelectionStyle();

    Ui::ClientThumbnail* ui;
    class QTimer* m_hoverTimer = nullptr;
    static class LiveTooltip* s_liveTooltip;
    static ClientThumbnail*   s_currentTooltipOwner;
    ClientState          m_state;
    QString              m_brokerVersion;
    QPixmap              m_frameBuffer;
    bool                 m_hovered       = false;
    int                  m_remoteWidth   = 0;
    int                  m_remoteHeight  = 0;
    bool                 m_isStreaming   = false;
    bool                 m_hasValidFrame = false;
    QPoint               m_dragStartPos;   // ← YENİ: drag eşiği için
};

