// master/ssh/SshTerminalWidget.h
#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QKeyEvent>
#include <QFont>
#include <QTimer>
#include <QColor>
#include <QVector>
#include <QChar>
#include <QDateTime>

#include <libssh2.h>
#include "SshClient.h"

/**
 * Gömülü SSH terminal widget'ı — Character Grid + QPainter tabanlı.
 *
 * QTextEdit yerine gerçek bir VT100/xterm terminal emülatörü:
 * - 2D karakter ızgarası (screen buffer)
 * - Alternate screen buffer (vim, htop, mc)
 * - Tam cursor positioning (CSI H, CSI f)
 * - Scroll region (DECSTBM)
 * - SGR renk/format desteği (256 renk dahil)
 * - Cursor blink
 * - PTY resize → SIGWINCH
 */
class SshTerminalWidget : public QWidget {
    Q_OBJECT

public:
    // ── Cell: tek bir terminal karakteri ────────────────────────────────────
    enum CellFlag : uint8_t {
        Bold      = 0x01,
        Italic    = 0x02,
        Underline = 0x04,
        Blink     = 0x08,
        Reverse   = 0x10,
        Invisible = 0x20,
    };

    struct Cell {
        QChar    ch    = ' ';
        QColor   fg;
        QColor   bg;
        uint8_t  flags = 0;

        bool operator==(const Cell& o) const {
            return ch == o.ch && fg == o.fg && bg == o.bg && flags == o.flags;
        }
        bool operator!=(const Cell& o) const { return !(*this == o); }
    };

    // ── Cursor state ─────────────────────────────────────────────────────────
    struct Cursor {
        int     row     = 0;
        int     col     = 0;
        bool    visible = true;
        // SGR attributes for incoming characters
        QColor  fg;
        QColor  bg;
        uint8_t flags   = 0;
    };

    explicit SshTerminalWidget(QWidget* parent = nullptr);
    ~SshTerminalWidget();

    // Açık bir SshClient ile terminal başlat
    void startSession(SshClient* client);
    void stopSession();
    void safeDetachSession();

    bool isRunning() const { return m_channel != nullptr; }
    bool isBusy() const;

    void setFontSize(int pt);
    void clearScreen();

    // Copy / Paste
    void copyToClipboard();
    void pasteFromClipboard();
    QString selectedText() const;
    bool hasSelection() const;

    // Dışarıdan yazı ekle (örn. bağlantı durum mesajı)
    void appendOutput(const QByteArray& data);

public slots:
    void sendText(const QString& text);
    void sendCommand(const QString& command);
    void updateThemeColors();

signals:
    void sessionStarted();
    void sessionFinished(int exitCode);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;  // Tab key'in focus traversal yapmasını engelle
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Mouse events for selection
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
    void pollPty();
    void blinkCursor();

private:
    // Helper to map screen coordinates or line index to scrollback or live buffers
    struct BufferPos {
        int row = 0; // 0-indexed across entire virtual buffer (scrollback + screen)
        int col = 0;
        bool operator==(const BufferPos& o) const { return row == o.row && col == o.col; }
        bool operator!=(const BufferPos& o) const { return !(*this == o); }
        bool operator<(const BufferPos& o) const {
            if (row != o.row) return row < o.row;
            return col < o.col;
        }
    };
    BufferPos coordsToBufferPos(const QPoint& pos) const;
    bool isPosSelected(const BufferPos& pos) const;

    // ── Screen helpers ───────────────────────────────────────────────────────
    int  cols() const { return m_cols; }
    int  rows() const { return m_rows; }
    void resizeScreen(int newCols, int newRows);
    Cell& cellAt(int row, int col);
    Cell& altCellAt(int row, int col);

    void writeChar(QChar ch);
    void lineFeed();
    void carriageReturn();
    void backspace();
    void tab();

    void scrollUp(int n = 1);
    void scrollDown(int n = 1);
    void eraseDisplay(int mode);  // 0=below, 1=above, 2=all, 3=scrollback
    void eraseLine(int mode);     // 0=right, 1=left, 2=all
    void eraseChars(int n);
    void insertLines(int n);
    void deleteLines(int n);
    void insertChars(int n);
    void deleteChars(int n);

    void clampCursor();
    void switchToAltScreen();
    void switchToNormalScreen();

    // ── VT100/xterm parser ──────────────────────────────────────────────────
    void processBytes(const QByteArray& data);
    void handleEscapeChar(uchar c);
    void handleCsiParam(uchar c);
    void handleOscData();
    void applySgr(const QList<int>& params);
    QColor ansiColor(int code, bool bright) const;
    QColor xterm256Color(int idx) const;

    // ── Channel I/O ──────────────────────────────────────────────────────────
    void writeToChannel(const QByteArray& data);
    void updatePtySize();
    void recalcCellSize();

    // ── Members ─────────────────────────────────────────────────────────────
    SshClient*       m_client  = nullptr;
    LIBSSH2_CHANNEL* m_channel = nullptr;
    QTimer*          m_pollTimer    = nullptr;
    QTimer*          m_blinkTimer   = nullptr;
    bool             m_cursorBlink  = true;  // blink toggle (true = visible phase)

    // Screen buffers
    int  m_cols = 80;
    int  m_rows = 24;
    QVector<QVector<Cell>> m_screen;     // normal screen
    QVector<QVector<Cell>> m_altScreen;  // alternate screen (vim etc.)
    bool m_usingAltScreen = false;

    // Scrollback buffer (normal screen only)
    static constexpr int kMaxScrollback = 5000;
    QVector<QVector<Cell>> m_scrollback;
    int  m_scrollOffset = 0;  // 0 = at bottom (live view)

    // Selection
    bool      m_selecting = false;
    BufferPos m_selStart;
    BufferPos m_selEnd;

    // Cursor
    Cursor  m_cursor;
    Cursor  m_savedCursor;       // for ESC 7 / ESC 8 (DECSC/DECRC)
    Cursor  m_altSavedCursor;

    // Scroll region (1-indexed internally stored as 0-indexed)
    int m_scrollTop    = 0;
    int m_scrollBottom = 0;   // = m_rows - 1

    // VT parser state
    enum class ParseState {
        Normal,
        Escape,         // received ESC
        CsiParam,       // ESC [ ...
        Osc,            // ESC ] ...
        Dcs,            // ESC P ... (ignore)
        DesignateG0,    // ESC ( ...
        DesignateG1,    // ESC ) ...
    };
    ParseState  m_parseState = ParseState::Normal;
    QByteArray  m_paramBuf;  // accumulates CSI params
    QByteArray  m_oscBuf;    // accumulates OSC data
    bool        m_csiPrivate = false;  // ESC [ ? prefix
    QByteArray  m_utf8Buf;   // accumulates multi-byte UTF-8 sequence
    bool        m_appCursorKeys  = false; // DECCKM mode 1 (application cursor keys)
    bool        m_decLineDrawing = false; // VT100 DEC Alternate Character Set (line drawing)
    bool        m_wrapPending    = false; // DECAWM autowrap delay
    QChar       m_lastChar       = ' ';   // Preceding graphic character for CSI b (REP)

    // SGR 256/truecolor sub-state
    enum class SgrColorMode { None, Fg256, Bg256, FgRgbR, FgRgbG, FgRgbB, BgRgbR, BgRgbG, BgRgbB };
    SgrColorMode m_sgrColorMode  = SgrColorMode::None;
    int          m_sgrColorAccum = 0;
    int          m_sgrRgbR = 0, m_sgrRgbG = 0;

    // Rendering
    QFont   m_font;
    int     m_cellW = 1;
    int     m_cellH = 1;
    QColor  m_defaultFg;
    QColor  m_defaultBg;

    // Scrollbar
    QScrollBar* m_scrollBar = nullptr;

    // Traffic / Busy detection
    qint64 m_lastDataReceivedTime = 0;
};
