// master/ssh/SshTerminalWidget.cpp
//
// Grid-tabanlı VT100/xterm terminal emülatörü.
// QTextEdit tamamen kaldırıldı; tüm render QPainter ile yapılır.
//
#include "SshTerminalWidget.h"
#include "../../common/utils/Logger.h"
#include "../../common/ui/ThemeManager.h"

#include <QApplication>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QFocusEvent>
#include <QScrollBar>
#include <QClipboard>
#include <QThread>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShowEvent>
#include <cstring>  // memset

// ============================================================================
// Constructor / Destructor
// ============================================================================

SshTerminalWidget::SshTerminalWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::IBeamCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Font
    m_font = QFont("Monospace", 11);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    setFont(m_font);

    // Layout: widget itself + scrollbar
    auto* hlay = new QHBoxLayout(this);
    hlay->setContentsMargins(0, 0, 0, 0);
    hlay->setSpacing(0);

    // Terminal area — the widget itself is the painting canvas;
    // we create an inner "canvas" widget so scrollbar sits right of it.
    // Actually simpler: widget IS the canvas, scrollbar floats.
    // We just add the scrollbar directly here.
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setRange(0, 0);
    m_scrollBar->setValue(0);
    hlay->addStretch(1);
    hlay->addWidget(m_scrollBar, 0);

    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int val) {
        int maxOff = (int)m_scrollback.size();
        m_scrollOffset = maxOff - val;
        if (m_scrollOffset < 0) m_scrollOffset = 0;
        update();
    });

    // Poll timer
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(20);  // ~50 FPS
    connect(m_pollTimer, &QTimer::timeout, this, &SshTerminalWidget::pollPty);

    // Cursor blink timer
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(530);
    connect(m_blinkTimer, &QTimer::timeout, this, &SshTerminalWidget::blinkCursor);
    m_blinkTimer->start();

    // Theme
    updateThemeColors();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SshTerminalWidget::updateThemeColors);

    // Init screen
    resizeScreen(m_cols, m_rows);
    recalcCellSize();
}

SshTerminalWidget::~SshTerminalWidget() {
    stopSession();
}

// ============================================================================
// Session management
// ============================================================================

void SshTerminalWidget::startSession(SshClient* client) {
    if (!client || !client->isConnected()) return;
    m_client = client;

    updatePtySize();  // compute cols/rows from widget size

    m_channel = m_client->openPtyChannel(m_cols, m_rows);
    if (!m_channel) {
        Logger::error(QObject::tr("SshTerminalWidget: PTY kanalı açılamadı"));
        return;
    }

    m_client->enableKeepalive(5);

    // İlk MOTD/banner paketleri PTY açılır açılmaz gelir.
    // Timer henüz başlamadan önce bu verileri yakalamak için 50ms sonra ilk poll.
    QTimer::singleShot(50, this, &SshTerminalWidget::pollPty);
    m_pollTimer->start();
    Logger::info(QObject::tr("SshTerminalWidget: Terminal başladı"));
    emit sessionStarted();
}

void SshTerminalWidget::stopSession() {
    m_pollTimer->stop();
    if (m_channel) {
        libssh2_channel_close(m_channel);
        libssh2_channel_free(m_channel);
        m_channel = nullptr;
    }
    m_client = nullptr;
}

void SshTerminalWidget::safeDetachSession() {
    // SIGHUP almadan arka planda devam etmesi için önce disown/nohup mesajı gönderilir
    if (m_channel) {
        // bash/zsh içinde çalışan işleri disown etmeyi dene ve terminalden güvenle ayrıl
        writeToChannel("\n disown -a 2>/dev/null; exit\n");
        QThread::msleep(80);
    }
    stopSession();
}

bool SshTerminalWidget::isBusy() const {
    if (!m_channel) return false;
    // Eğer son 3 saniye içinde veri aktıysa meşguldür
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    return (now - m_lastDataReceivedTime < 3000);
}

// ============================================================================
// PTY polling
// ============================================================================

void SshTerminalWidget::pollPty() {
    if (!m_channel) return;

    char buf[8192];
    bool hasData = false;

    while (m_channel) {
        int rc = libssh2_channel_read(m_channel, buf, sizeof(buf));

        if (rc > 0) {
            hasData = true;
            m_lastDataReceivedTime = QDateTime::currentMSecsSinceEpoch();
            processBytes(QByteArray(buf, rc));
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            break;
        } else if (rc < 0) {
            // Gerçek okuma hatası — bağlantı koptu
            int exitCode = libssh2_channel_get_exit_status(m_channel);
            stopSession();
            processBytes("\r\n" + tr("[Bağlantı kapandı]").toUtf8() + "\r\n");
            emit sessionFinished(exitCode);
            update();
            return;
        } else if (rc == 0 || libssh2_channel_eof(m_channel)) {
            // Sunucu tarafı kanalı kapattı (EOF)
            int exitCode = libssh2_channel_get_exit_status(m_channel);
            stopSession();
            processBytes("\r\n" + tr("[Bağlantı kapandı]").toUtf8() + "\r\n");
            emit sessionFinished(exitCode);
            update();
            return;
        }
    }

    if (hasData) {
        int maxVal = (int)m_scrollback.size();
        m_scrollBar->blockSignals(true);
        m_scrollBar->setRange(0, maxVal);
        if (m_scrollOffset == 0) {
            m_scrollBar->setValue(maxVal);
        } else {
            m_scrollBar->setValue(qMax(0, maxVal - m_scrollOffset));
        }
        m_scrollBar->blockSignals(false);
        update();
    } else if (m_client) {
        // Veri akışı yokken periyodik keepalive kontrolü
        if (!m_client->sendKeepalive()) {
            int exitCode = libssh2_channel_get_exit_status(m_channel);
            stopSession();
            processBytes("\r\n" + tr("[Bağlantı kapandı]").toUtf8() + "\r\n");
            emit sessionFinished(exitCode);
            update();
        }
    }
}

// ============================================================================
// External output (status messages before session starts)
// ============================================================================

void SshTerminalWidget::appendOutput(const QByteArray& data) {
    processBytes(data);
    update();
}

// ============================================================================
// recalcCellSize — call after font change
// ============================================================================

void SshTerminalWidget::recalcCellSize() {
    QFontMetrics fm(m_font);
    m_cellW = fm.horizontalAdvance('W');
    m_cellH = fm.height();
    if (m_cellW < 1) m_cellW = 1;
    if (m_cellH < 1) m_cellH = 1;
}

// ============================================================================
// Screen buffer management
// ============================================================================

void SshTerminalWidget::resizeScreen(int newCols, int newRows) {
    newCols = qMax(1, newCols);
    newRows = qMax(1, newRows);

    if (newCols == m_cols && newRows == m_rows &&
        m_screen.size() == newRows && (!m_screen.isEmpty() && m_screen[0].size() == newCols)) {
        return;
    }

    Cell blank;
    blank.fg = m_defaultFg;
    blank.bg = m_defaultBg;

    int oldRows = m_rows;

    // Normal ekranda satır sayısı azaldığında üst satırları scrollback'e aktararak
    // alt satırların (prompt ve son komut çıktılarının) ekranda kalmasını sağla.
    if (!m_usingAltScreen && oldRows > 0) {
        if (newRows < oldRows) {
            int diff = oldRows - newRows;
            for (int i = 0; i < diff && !m_screen.isEmpty(); ++i) {
                m_scrollback.append(m_screen.takeFirst());
                if (m_scrollback.size() > kMaxScrollback) {
                    m_scrollback.removeFirst();
                }
            }
            m_cursor.row = qBound(0, m_cursor.row - diff, newRows - 1);
        } else if (newRows > oldRows) {
            int diff = newRows - oldRows;
            int pulled = 0;
            while (pulled < diff && !m_scrollback.isEmpty()) {
                m_screen.prepend(m_scrollback.takeLast());
                pulled++;
            }
            m_cursor.row = qBound(0, m_cursor.row + pulled, newRows - 1);
        }
    }

    auto resizeBuf = [&](QVector<QVector<Cell>>& buf) {
        buf.resize(newRows);
        for (auto& row : buf) {
            row.resize(newCols, blank);
        }
    };

    resizeBuf(m_screen);
    resizeBuf(m_altScreen);

    // Clamp cursor
    m_cursor.row = qBound(0, m_cursor.row, newRows - 1);
    m_cursor.col = qBound(0, m_cursor.col, newCols - 1);

    m_cols = newCols;
    m_rows = newRows;
    m_scrollTop    = 0;
    m_scrollBottom = m_rows - 1;

    // Scrollbar aralığını scrollback boyutuna göre senkronize et
    if (m_scrollBar) {
        int maxVal = (int)m_scrollback.size();
        m_scrollBar->blockSignals(true);
        m_scrollBar->setRange(0, maxVal);
        if (m_scrollOffset == 0) {
            m_scrollBar->setValue(maxVal);
        } else {
            m_scrollOffset = qBound(0, m_scrollOffset, maxVal);
            m_scrollBar->setValue(maxVal - m_scrollOffset);
        }
        m_scrollBar->blockSignals(false);
    }
}

SshTerminalWidget::Cell& SshTerminalWidget::cellAt(int row, int col) {
    static Cell dummy;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols) return dummy;
    return m_screen[row][col];
}

SshTerminalWidget::Cell& SshTerminalWidget::altCellAt(int row, int col) {
    static Cell dummy;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols) return dummy;
    return m_altScreen[row][col];
}

// ============================================================================
// Painting
// ============================================================================

void SshTerminalWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setFont(m_font);

    // Scrollbar width
    int sbw = m_scrollBar->isVisible() ? m_scrollBar->width() : 0;
    int drawW = width() - sbw;

    // Fill background
    p.fillRect(0, 0, drawW, height(), m_defaultBg);

    QFontMetrics fm(m_font);
    int ascent = fm.ascent();

    // Which buffer to render?
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;

    // Scrollback rows to show (only in normal screen)
    int sbRows = 0;
    if (!m_usingAltScreen && m_scrollOffset > 0) {
        sbRows = qMin(m_scrollOffset, (int)m_scrollback.size());
    }

    // We render m_rows rows total. If scrollOffset > 0, show scrollback lines
    // at top and fewer live rows at bottom.
    int liveStart = qMax(0, m_rows - (m_rows - sbRows));  // first live row index
    (void)liveStart;

    for (int r = 0; r < m_rows; r++) {
        int y = r * m_cellH;

        // Determine which data to render for this screen row
        const QVector<Cell>* rowData = nullptr;
        QVector<Cell> sbRowData;

        if (!m_usingAltScreen && sbRows > 0) {
            // r < sbRows → scrollback
            if (r < sbRows) {
                int sbIdx = (int)m_scrollback.size() - m_scrollOffset + r;
                if (sbIdx >= 0 && sbIdx < (int)m_scrollback.size()) {
                    sbRowData = m_scrollback[sbIdx];
                    sbRowData.resize(m_cols, Cell{' ', m_defaultFg, m_defaultBg, 0});
                    rowData = &sbRowData;
                }
            } else {
                int liveRow = r - sbRows;
                if (liveRow < (int)buf.size()) {
                    rowData = &buf[liveRow];
                }
            }
        } else {
            if (r < (int)buf.size()) {
                rowData = &buf[r];
            }
        }

        if (!rowData) continue;

        // Determine cursor row in view coordinates
        bool isCursorRow = (!m_usingAltScreen && sbRows == 0 && r == m_cursor.row)
                        || ( m_usingAltScreen && r == m_cursor.row);
        bool showCursor  = m_channel && m_cursor.visible && m_cursorBlink && hasFocus();

        for (int c = 0; c < m_cols && c < (int)rowData->size(); c++) {
            const Cell& cell = (*rowData)[c];
            int x = c * m_cellW;
            bool isCursorCell = showCursor && isCursorRow && c == m_cursor.col;

            // Map row and col to absolute BufferPos
            BufferPos pos;
            if (!m_usingAltScreen && sbRows > 0 && r < sbRows) {
                pos.row = (int)m_scrollback.size() - m_scrollOffset + r;
            } else if (!m_usingAltScreen) {
                pos.row = (int)m_scrollback.size() + (r - sbRows);
            } else {
                pos.row = r;
            }
            pos.col = c;

            bool isSelected = hasSelection() && isPosSelected(pos);

            QColor fg = cell.fg.isValid() ? cell.fg : m_defaultFg;
            QColor bg = cell.bg.isValid() ? cell.bg : m_defaultBg;

            if ((cell.flags & CellFlag::Reverse) || isCursorCell || isSelected) {
                qSwap(fg, bg);
            }

            // Background
            if (bg != m_defaultBg || isCursorCell || isSelected) {
                p.fillRect(x, y, m_cellW, m_cellH, bg);
            }

            // Text
            if (!cell.ch.isSpace()) {
                QFont f = m_font;
                if (cell.flags & CellFlag::Bold)      { f.setBold(true);      p.setFont(f); }
                if (cell.flags & CellFlag::Italic)     { f.setItalic(true);    p.setFont(f); }
                if (cell.flags & CellFlag::Underline)  { f.setUnderline(true); p.setFont(f); }

                p.setPen(fg);
                p.drawText(x, y + ascent, cell.ch);

                if (cell.flags & (CellFlag::Bold | CellFlag::Italic | CellFlag::Underline)) {
                    p.setFont(m_font);
                }
            }
        }
    }
}

// ============================================================================
// Character writing & DEC ACS line drawing
// ============================================================================

static QChar mapDecLineDrawing(QChar ch) {
    ushort u = ch.unicode();
    switch (u) {
    case '`': return QChar(0x25C6); // ◆ diamond
    case 'a': return QChar(0x2592); // ▒ checkerboard
    case 'f': return QChar(0x00B0); // ° degree
    case 'g': return QChar(0x00B1); // ± plus-minus
    case 'h': return QChar(0x2591); // ░ light shade
    case 'i': return QChar(0x2502); // │ vertical line
    case 'j': return QChar(0x2518); // ┘ lower right corner
    case 'k': return QChar(0x2510); // ┐ upper right corner
    case 'l': return QChar(0x250C); // ┌ upper left corner
    case 'm': return QChar(0x2514); // └ lower left corner
    case 'n': return QChar(0x253C); // ┼ cross / plus
    case 'o': return QChar(0x23BA); // ⎺ horizontal line scan 1
    case 'p': return QChar(0x23BB); // ⎻ horizontal line scan 3
    case 'q': return QChar(0x2500); // ─ horizontal line scan 5
    case 'r': return QChar(0x23BC); // ⎼ horizontal line scan 7
    case 's': return QChar(0x23BD); // ⎽ horizontal line scan 9
    case 't': return QChar(0x251C); // ├ left T
    case 'u': return QChar(0x2524); // ┤ right T
    case 'v': return QChar(0x2534); // ┴ bottom T
    case 'w': return QChar(0x252C); // ┬ top T
    case 'x': return QChar(0x2502); // │ vertical line
    case 'y': return QChar(0x2264); // ≤ less-than-or-equal
    case 'z': return QChar(0x2265); // ≥ greater-than-or-equal
    case '{': return QChar(0x03C0); // π pi
    case '|': return QChar(0x2260); // ≠ not-equal
    case '}': return QChar(0x00A3); // £ pound
    case '~': return QChar(0x00B7); // · bullet / dot
    default:  return ch;
    }
}

void SshTerminalWidget::writeChar(QChar ch) {
    if (m_decLineDrawing && ch.unicode() >= 0x5F && ch.unicode() <= 0x7E) {
        ch = mapDecLineDrawing(ch);
    }

    m_lastChar = ch;

    // Delayed wrap (DECAWM): only wrap when the next character actually arrives
    if (m_wrapPending) {
        m_cursor.col = 0;
        lineFeed();
        m_wrapPending = false;
    }

    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    clampCursor();

    Cell& cell = buf[m_cursor.row][m_cursor.col];
    cell.ch    = ch;
    cell.fg    = m_cursor.fg.isValid() ? m_cursor.fg : m_defaultFg;
    cell.bg    = m_cursor.bg.isValid() ? m_cursor.bg : m_defaultBg;
    cell.flags = m_cursor.flags;

    if (m_cursor.col >= m_cols - 1) {
        m_wrapPending = true;
    } else {
        m_cursor.col++;
    }
}

void SshTerminalWidget::lineFeed() {
    m_wrapPending = false;
    if (m_cursor.row < m_scrollBottom) {
        m_cursor.row++;
    } else {
        scrollUp(1);
    }
}

void SshTerminalWidget::carriageReturn() {
    m_wrapPending = false;
    m_cursor.col = 0;
}

void SshTerminalWidget::backspace() {
    m_wrapPending = false;
    if (m_cursor.col > 0) m_cursor.col--;
}

void SshTerminalWidget::tab() {
    m_wrapPending = false;
    int next = ((m_cursor.col / 8) + 1) * 8;
    m_cursor.col = qMin(next, m_cols - 1);
}

void SshTerminalWidget::clampCursor() {
    m_cursor.row = qBound(0, m_cursor.row, m_rows - 1);
    m_cursor.col = qBound(0, m_cursor.col, m_cols - 1);
}

// ============================================================================
// Scroll
// ============================================================================

void SshTerminalWidget::scrollUp(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;

    Cell blank;
    blank.fg = m_defaultFg;
    blank.bg = m_defaultBg;

    for (int i = 0; i < n; i++) {
        // Push top row into scrollback (normal screen only)
        if (!m_usingAltScreen) {
            m_scrollback.append(buf[m_scrollTop]);
            if (m_scrollback.size() > kMaxScrollback)
                m_scrollback.removeFirst();
        }
        // Shift rows up within scroll region
        for (int r = m_scrollTop; r < m_scrollBottom; r++) {
            buf[r] = buf[r + 1];
        }
        // Clear bottom row
        buf[m_scrollBottom].fill(blank);
    }
}

void SshTerminalWidget::scrollDown(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;

    Cell blank;
    blank.fg = m_defaultFg;
    blank.bg = m_defaultBg;

    for (int i = 0; i < n; i++) {
        for (int r = m_scrollBottom; r > m_scrollTop; r--) {
            buf[r] = buf[r - 1];
        }
        buf[m_scrollTop].fill(blank);
    }
}

// ============================================================================
// Erase operations
// ============================================================================

void SshTerminalWidget::eraseDisplay(int mode) {
    m_wrapPending = false;
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank;
    blank.fg = m_defaultFg;
    blank.bg = m_defaultBg;

    clampCursor();
    if (mode == 0) {
        // Erase from cursor to end of screen
        for (int c = m_cursor.col; c < m_cols; c++) buf[m_cursor.row][c] = blank;
        for (int r = m_cursor.row + 1; r < m_rows; r++) buf[r].fill(blank);
    } else if (mode == 1) {
        // Erase from start to cursor
        for (int r = 0; r < m_cursor.row; r++) buf[r].fill(blank);
        for (int c = 0; c <= m_cursor.col; c++) buf[m_cursor.row][c] = blank;
    } else if (mode == 2 || mode == 3) {
        for (auto& row : buf) row.fill(blank);
        // Note: In ANSI/VT100 standard, ED mode 2 does not move the cursor;
        // cursor movement is explicitly performed by CUP (ESC [ H).
    }
}

void SshTerminalWidget::eraseLine(int mode) {
    m_wrapPending = false;
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank;
    blank.fg = m_defaultFg;
    blank.bg = m_defaultBg;
    clampCursor();

    if (mode == 0) {
        for (int c = m_cursor.col; c < m_cols; c++) buf[m_cursor.row][c] = blank;
    } else if (mode == 1) {
        for (int c = 0; c <= m_cursor.col; c++) buf[m_cursor.row][c] = blank;
    } else if (mode == 2) {
        buf[m_cursor.row].fill(blank);
    }
}

void SshTerminalWidget::eraseChars(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank;
    blank.fg = m_defaultFg;
    blank.bg = m_defaultBg;
    clampCursor();
    for (int c = m_cursor.col; c < qMin(m_cursor.col + n, m_cols); c++)
        buf[m_cursor.row][c] = blank;
}

void SshTerminalWidget::insertLines(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank; blank.fg = m_defaultFg; blank.bg = m_defaultBg;
    for (int i = 0; i < n; i++) {
        for (int r = m_scrollBottom; r > m_cursor.row; r--) buf[r] = buf[r-1];
        buf[m_cursor.row].fill(blank);
    }
}

void SshTerminalWidget::deleteLines(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank; blank.fg = m_defaultFg; blank.bg = m_defaultBg;
    for (int i = 0; i < n; i++) {
        for (int r = m_cursor.row; r < m_scrollBottom; r++) buf[r] = buf[r+1];
        buf[m_scrollBottom].fill(blank);
    }
}

void SshTerminalWidget::insertChars(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank; blank.fg = m_defaultFg; blank.bg = m_defaultBg;
    auto& row = buf[m_cursor.row];
    for (int i = 0; i < n && m_cursor.col < m_cols; i++) {
        row.insert(m_cursor.col, blank);
        if (row.size() > m_cols) row.resize(m_cols);
    }
}

void SshTerminalWidget::deleteChars(int n) {
    auto& buf = m_usingAltScreen ? m_altScreen : m_screen;
    Cell blank; blank.fg = m_defaultFg; blank.bg = m_defaultBg;
    auto& row = buf[m_cursor.row];
    for (int i = 0; i < n && m_cursor.col < (int)row.size(); i++) {
        row.removeAt(m_cursor.col);
    }
    while (row.size() < m_cols) row.append(blank);
}

// ============================================================================
// Alternate screen
// ============================================================================

void SshTerminalWidget::switchToAltScreen() {
    if (m_usingAltScreen) return;
    m_altSavedCursor = m_cursor;
    Cell blank; blank.fg = m_defaultFg; blank.bg = m_defaultBg;
    for (auto& row : m_altScreen) row.fill(blank);
    m_cursor.row = 0;
    m_cursor.col = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
    m_usingAltScreen = true;
}

void SshTerminalWidget::switchToNormalScreen() {
    if (!m_usingAltScreen) return;
    m_usingAltScreen = false;
    m_cursor = m_altSavedCursor;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
}

// ============================================================================
// VT100 / xterm parser
// ============================================================================

void SshTerminalWidget::processBytes(const QByteArray& data) {
    for (uchar c : data) {
        switch (m_parseState) {
        // ── Normal ─────────────────────────────────────────────────────────
        case ParseState::Normal:
            if (c == 0x1B) {
                m_utf8Buf.clear();  // ESC flushes any incomplete UTF-8 sequence
                m_parseState = ParseState::Escape;
            } else if (c == '\r') {
                m_utf8Buf.clear();
                carriageReturn();
            } else if (c == '\n' || c == 0x0B || c == 0x0C) {
                m_utf8Buf.clear();
                lineFeed();
            } else if (c == '\b' || c == 0x7F) {
                m_utf8Buf.clear();
                backspace();
            } else if (c == '\t') {
                m_utf8Buf.clear();
                tab();
            } else if (c == 0x07) {
                // BEL — ignore
            } else if (c == 0x0E || c == 0x0F) {
                // SO/SI charset switching — ignore
            } else if (c >= 0x80 && c <= 0xBF) {
                // UTF-8 continuation byte
                m_utf8Buf.append((char)c);
                // Check if we have a complete sequence
                if (!m_utf8Buf.isEmpty()) {
                    uchar lead = (uchar)m_utf8Buf[0];
                    int expected = 1;
                    if      ((lead & 0xE0) == 0xC0) expected = 2;  // 110xxxxx
                    else if ((lead & 0xF0) == 0xE0) expected = 3;  // 1110xxxx
                    else if ((lead & 0xF8) == 0xF0) expected = 4;  // 11110xxx
                    if (m_utf8Buf.size() >= expected) {
                        QString decoded = QString::fromUtf8(m_utf8Buf);
                        for (QChar ch : decoded) {
                            if (!ch.isNull()) writeChar(ch);
                        }
                        m_utf8Buf.clear();
                    }
                }
            } else if (c >= 0xC0) {
                // UTF-8 lead byte (start of multi-byte sequence)
                m_utf8Buf.clear();
                m_utf8Buf.append((char)c);
            } else if (c >= 0x20) {
                // Plain ASCII
                m_utf8Buf.clear();
                writeChar(QChar(c));
            }
            break;

        // ── ESC received ───────────────────────────────────────────────────
        case ParseState::Escape:
            handleEscapeChar(c);
            break;

        // ── CSI params ─────────────────────────────────────────────────────
        case ParseState::CsiParam:
            handleCsiParam(c);
            break;

        // ── OSC ────────────────────────────────────────────────────────────
        case ParseState::Osc:
            if (c == 0x07 || c == 0x9C) {
                // ST (String Terminator) or BEL
                handleOscData();
                m_oscBuf.clear();
                m_parseState = ParseState::Normal;
            } else if (c == 0x1B) {
                // Might be ESC \ (ST)
                m_parseState = ParseState::Escape;
            } else {
                m_oscBuf.append((char)c);
            }
            break;

        // ── DCS (ignore completely) ─────────────────────────────────────────
        case ParseState::Dcs:
            if (c == 0x9C || c == 0x07) m_parseState = ParseState::Normal;
            break;

        // ── Designate G0 / G1 ──────────────────────────────────────────────
        case ParseState::DesignateG0:
            if (c == '0') {
                m_decLineDrawing = true;
            } else if (c == 'B' || c == 'A') {
                m_decLineDrawing = false;
            }
            m_parseState = ParseState::Normal;
            break;

        case ParseState::DesignateG1:
            m_parseState = ParseState::Normal;
            break;
        }
    }
}

void SshTerminalWidget::handleEscapeChar(uchar c) {
    switch (c) {
    case '[':  // CSI
        m_paramBuf.clear();
        m_csiPrivate = false;
        m_parseState = ParseState::CsiParam;
        break;
    case ']':  // OSC
        m_oscBuf.clear();
        m_parseState = ParseState::Osc;
        break;
    case 'P':  // DCS
        m_parseState = ParseState::Dcs;
        break;
    case '\\': // ST (ends OSC/DCS)
        if (m_parseState == ParseState::Osc) {
            handleOscData();
        }
        m_oscBuf.clear();
        m_parseState = ParseState::Normal;
        break;
    case '7':  // DECSC — save cursor
        m_savedCursor = m_cursor;
        m_parseState = ParseState::Normal;
        break;
    case '8':  // DECRC — restore cursor
        m_cursor = m_savedCursor;
        m_wrapPending = false;
        m_parseState = ParseState::Normal;
        break;
    case 'M':  // RI — reverse index (scroll down)
        m_wrapPending = false;
        if (m_cursor.row == m_scrollTop) scrollDown(1);
        else if (m_cursor.row > 0) m_cursor.row--;
        m_parseState = ParseState::Normal;
        break;
    case 'D':  // IND — index (line feed without CR)
        lineFeed();
        m_parseState = ParseState::Normal;
        break;
    case 'E':  // NEL — next line
        carriageReturn();
        lineFeed();
        m_parseState = ParseState::Normal;
        break;
    case 'c':  // RIS — reset
        m_cursor = Cursor{};
        m_cursor.fg = m_defaultFg;
        m_cursor.bg = m_defaultBg;
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
        m_appCursorKeys = false;
        m_decLineDrawing = false;
        m_wrapPending = false;
        eraseDisplay(2);
        m_parseState = ParseState::Normal;
        break;
    case '(':  // G0 charset designate
        m_parseState = ParseState::DesignateG0;
        break;
    case ')':  // G1 charset designate
    case '*':
    case '+':
        m_parseState = ParseState::DesignateG1;
        break;
    default:
        m_parseState = ParseState::Normal;
        break;
    }
}

// ── Parse helpers ────────────────────────────────────────────────────────────

static QList<int> parseParams(const QByteArray& raw, char stripPrefix = 0) {
    QByteArray src = raw;
    if (stripPrefix && !src.isEmpty() && src[0] == stripPrefix)
        src = src.mid(1);

    // Remove '?' prefix if present
    if (!src.isEmpty() && (src[0] == '?' || src[0] == '>' || src[0] == '!'))
        src = src.mid(1);

    // Support colon-separated sub-parameters (e.g. TrueColor 38:2:r:g:b)
    src.replace(':', ';');

    QList<int> result;
    for (const QByteArray& part : src.split(';')) {
        bool ok;
        int v = part.trimmed().toInt(&ok);
        result.append(ok ? v : 0);
    }
    if (result.isEmpty()) result.append(0);
    return result;
}

void SshTerminalWidget::handleCsiParam(uchar c) {
    if (c == '?' || c == '>' || c == '!') {
        m_csiPrivate = true;
        m_paramBuf.append((char)c);
        return;
    }
    if ((c >= 0x30 && c <= 0x3F) || (c >= 0x20 && c <= 0x2F)) {
        m_paramBuf.append((char)c);
        return;
    }
    if (c >= 0x40 && c <= 0x7E) {
        // Final byte — dispatch
        char cmd = (char)c;
        QList<int> p = parseParams(m_paramBuf);
        auto P = [&](int idx, int def = 0) -> int {
            return (idx < p.size() && p[idx] != 0) ? p[idx] : def;
        };
        auto P0 = [&](int idx, int def = 0) -> int {
            return (idx < p.size()) ? p[idx] : def;
        };

        if (m_csiPrivate) {
            // Private CSI (ESC [ ? ...)
            switch (cmd) {
            case 'h':  // Set mode
                for (int mode : p) {
                    if (mode == 1049) switchToAltScreen();
                    else if (mode == 25)  m_cursor.visible = true;
                    else if (mode == 1)   m_appCursorKeys = true;
                    else if (mode == 47 || mode == 1047) switchToAltScreen();
                }
                break;
            case 'l':  // Reset mode
                for (int mode : p) {
                    if (mode == 1049) switchToNormalScreen();
                    else if (mode == 25)  m_cursor.visible = false;
                    else if (mode == 1)   m_appCursorKeys = false;
                    else if (mode == 47 || mode == 1047) switchToNormalScreen();
                }
                break;
            case 'J': eraseDisplay(P0(0, 0)); break;
            case 'K': eraseLine(P0(0, 0));    break;
            default: break;
            }
        } else {
            switch (cmd) {
            // ── Cursor movement ──────────────────────────────────────────
            case 'A': m_wrapPending = false; m_cursor.row = qMax(0, m_cursor.row - P(0,1));    break;
            case 'B': m_wrapPending = false; m_cursor.row = qMin(m_rows-1, m_cursor.row + P(0,1)); break;
            case 'C': m_wrapPending = false; m_cursor.col = qMin(m_cols-1, m_cursor.col + P(0,1));       break;
            case 'D': m_wrapPending = false; m_cursor.col = qMax(0, m_cursor.col - P(0,1));              break;
            case 'E': m_wrapPending = false; m_cursor.col = 0; m_cursor.row = qMin(m_rows-1, m_cursor.row + P(0,1)); break;
            case 'F': m_wrapPending = false; m_cursor.col = 0; m_cursor.row = qMax(0, m_cursor.row - P(0,1));        break;
            case 'G': m_wrapPending = false; m_cursor.col = qBound(0, P(0,1) - 1, m_cols-1);            break;
            case 'H': // CUP — cursor position
            case 'f':
                m_wrapPending = false;
                m_cursor.row = qBound(0, P(0,1) - 1, m_rows-1);
                m_cursor.col = qBound(0, P(1,1) - 1, m_cols-1);
                break;
            case 'd': m_wrapPending = false; m_cursor.row = qBound(0, P(0,1) - 1, m_rows-1);            break;

            // ── Erase ────────────────────────────────────────────────────
            case 'J': eraseDisplay(P0(0, 0)); break;
            case 'K': eraseLine(P0(0, 0));    break;
            case 'X': eraseChars(P(0,1));     break;

            // ── Scroll ───────────────────────────────────────────────────
            case 'S': scrollUp(P(0,1));   break;
            case 'T': scrollDown(P(0,1)); break;

            // ── Insert / delete ──────────────────────────────────────────
            case '@': insertChars(P(0,1));  break;
            case 'P': deleteChars(P(0,1));  break;
            case 'L': insertLines(P(0,1));  break;
            case 'M': deleteLines(P(0,1));  break;

            // ── Scroll region ────────────────────────────────────────────
            case 'r':
                m_wrapPending  = false;
                m_scrollTop    = qMax(0, P(0,1) - 1);
                m_scrollBottom = qMin(m_rows-1, P(1,m_rows) - 1);
                m_cursor.row   = 0;
                m_cursor.col   = 0;
                break;

            // ── SGR ──────────────────────────────────────────────────────
            case 'm': applySgr(p); break;

            // ── Mode set/reset (non-private) ─────────────────────────────
            case 'h': break;  // SM
            case 'l': break;  // RM

            // ── Cursor shape (DECSCUSR) ───────────────────────────────────
            case 'q': break;

            // ── Status / device attributes ────────────────────────────────
            case 'n': { // DSR (Device Status Report)
                int param = P0(0, 0);
                if (param == 6) {
                    // CPR (Cursor Position Report): ESC [ row ; col R (1-indexed)
                    clampCursor();
                    QByteArray resp = QString("\x1b[%1;%2R")
                        .arg(m_cursor.row + 1)
                        .arg(m_cursor.col + 1)
                        .toUtf8();
                    writeToChannel(resp);
                } else if (param == 5) {
                    // Status report: ESC [ 0 n (Device OK)
                    writeToChannel("\x1b[0n");
                }
                break;
            }
            case 'c': { // DA (Device Attributes)
                if (m_csiPrivate) {
                    // Secondary DA: ESC [ > c -> xterm response
                    writeToChannel("\x1b[>0;10;0c");
                } else {
                    // Primary DA: ESC [ c -> VT100 with Advanced Video Option
                    writeToChannel("\x1b[?1;2c");
                }
                break;
            }
            case 't': { // Window / terminal size queries
                int param = P0(0, 0);
                if (param == 18) {
                    // Report terminal size in characters: ESC [ 8 ; rows ; cols t
                    QByteArray resp = QString("\x1b[8;%1;%2t")
                        .arg(m_rows)
                        .arg(m_cols)
                        .toUtf8();
                    writeToChannel(resp);
                }
                break;
            }

            // ── Save / Restore cursor (ANSI.SYS SCP / RCP) ────────────────
            case 's':
                m_savedCursor = m_cursor;
                break;
            case 'u':
                m_cursor = m_savedCursor;
                m_wrapPending = false;
                break;

            // ── Repeat character (REP) ────────────────────────────────────
            case 'b': {
                int count = P(0, 1);
                for (int rep = 0; rep < count; rep++) {
                    writeChar(m_lastChar);
                }
                break;
            }

            // ── Position relative (HPR / VPR) ─────────────────────────────
            case 'a': // HPR — Horizontal Position Relative
                m_wrapPending = false;
                m_cursor.col = qMin(m_cols - 1, m_cursor.col + P(0, 1));
                break;
            case 'e': // VPR — Vertical Position Relative
                m_wrapPending = false;
                m_cursor.row = qMin(m_rows - 1, m_cursor.row + P(0, 1));
                break;

            // ── Tabulation ────────────────────────────────────────────────
            case 'I': { // CHT — Cursor Forward Tabulation
                int count = P(0, 1);
                for (int t = 0; t < count; t++) tab();
                break;
            }
            case 'Z': { // CBT — Cursor Backward Tabulation
                m_wrapPending = false;
                int count = P(0, 1);
                for (int t = 0; t < count; t++) {
                    if (m_cursor.col > 0) {
                        int prev = ((m_cursor.col - 1) / 8) * 8;
                        m_cursor.col = qMax(0, prev);
                    }
                }
                break;
            }

            default: break;
            }
        }

        m_paramBuf.clear();
        m_csiPrivate = false;
        m_parseState = ParseState::Normal;
        return;
    }

    // Unexpected
    m_parseState = ParseState::Normal;
}

// ============================================================================
// SGR — Select Graphic Rendition
// ============================================================================

void SshTerminalWidget::applySgr(const QList<int>& params) {
    int i = 0;
    while (i < params.size()) {
        int n = params[i++];

        if (n == 0) {
            m_cursor.fg    = m_defaultFg;
            m_cursor.bg    = m_defaultBg;
            m_cursor.flags = 0;
        } else if (n == 1)  { m_cursor.flags |=  CellFlag::Bold; }
        else if (n == 2)    { m_cursor.flags &= ~CellFlag::Bold; }  // faint ~ dim
        else if (n == 3)    { m_cursor.flags |=  CellFlag::Italic; }
        else if (n == 4)    { m_cursor.flags |=  CellFlag::Underline; }
        else if (n == 5 || n == 6) { m_cursor.flags |= CellFlag::Blink; }
        else if (n == 7)    { m_cursor.flags |=  CellFlag::Reverse; }
        else if (n == 8)    { m_cursor.flags |=  CellFlag::Invisible; }
        else if (n == 21)   { m_cursor.flags |=  CellFlag::Underline; }
        else if (n == 22)   { m_cursor.flags &= ~CellFlag::Bold; }
        else if (n == 23)   { m_cursor.flags &= ~CellFlag::Italic; }
        else if (n == 24)   { m_cursor.flags &= ~CellFlag::Underline; }
        else if (n == 25)   { m_cursor.flags &= ~CellFlag::Blink; }
        else if (n == 27)   { m_cursor.flags &= ~CellFlag::Reverse; }
        else if (n == 28)   { m_cursor.flags &= ~CellFlag::Invisible; }
        else if (n >= 30 && n <= 37) { m_cursor.fg = ansiColor(n-30, false); }
        else if (n == 38) {
            // Extended foreground
            if (i < params.size() && params[i] == 5 && i+1 < params.size()) {
                m_cursor.fg = xterm256Color(params[i+1]);
                i += 2;
            } else if (i < params.size() && params[i] == 2) {
                if (i + 4 < params.size() && params[i+1] == 0) {
                    // ITU T.416 with color space ID: 38;2;<cs>;r;g;b
                    m_cursor.fg = QColor(qBound(0, params[i+2], 255),
                                         qBound(0, params[i+3], 255),
                                         qBound(0, params[i+4], 255));
                    i += 5;
                } else if (i + 3 < params.size()) {
                    // Standard 24-bit: 38;2;r;g;b
                    m_cursor.fg = QColor(qBound(0, params[i+1], 255),
                                         qBound(0, params[i+2], 255),
                                         qBound(0, params[i+3], 255));
                    i += 4;
                }
            }
        }
        else if (n == 39) { m_cursor.fg = m_defaultFg; }
        else if (n >= 40 && n <= 47) { m_cursor.bg = ansiColor(n-40, false); }
        else if (n == 48) {
            // Extended background
            if (i < params.size() && params[i] == 5 && i+1 < params.size()) {
                m_cursor.bg = xterm256Color(params[i+1]);
                i += 2;
            } else if (i < params.size() && params[i] == 2) {
                if (i + 4 < params.size() && params[i+1] == 0) {
                    // ITU T.416 with color space ID: 48;2;<cs>;r;g;b
                    m_cursor.bg = QColor(qBound(0, params[i+2], 255),
                                         qBound(0, params[i+3], 255),
                                         qBound(0, params[i+4], 255));
                    i += 5;
                } else if (i + 3 < params.size()) {
                    // Standard 24-bit: 48;2;r;g;b
                    m_cursor.bg = QColor(qBound(0, params[i+1], 255),
                                         qBound(0, params[i+2], 255),
                                         qBound(0, params[i+3], 255));
                    i += 4;
                }
            }
        }
        else if (n == 49) { m_cursor.bg = m_defaultBg; }
        else if (n >= 90 && n <= 97)  { m_cursor.fg = ansiColor(n-90, true); }
        else if (n >= 100 && n <= 107){ m_cursor.bg = ansiColor(n-100, true); }
    }
}

// ============================================================================
// OSC handler
// ============================================================================

void SshTerminalWidget::handleOscData() {
    // OSC 0/1/2: set window title — we ignore but could emit a signal
    // OSC 4: color definition — ignore
}

// ============================================================================
// Color tables
// ============================================================================

QColor SshTerminalWidget::ansiColor(int code, bool bright) const {
    // Catppuccin Mocha for dark, Solarized Light for light
    bool isLight = (m_defaultBg.value() > 128);

    if (isLight) {
        static const QColor norm[8] = {
            "#073642", "#dc322f", "#859900", "#b58900",
            "#268bd2", "#d33682", "#2aa198", "#eee8d5"
        };
        static const QColor bri[8] = {
            "#002b36", "#cb4b16", "#586e75", "#657b83",
            "#839496", "#6c71c4", "#93a1a1", "#fdf6e3"
        };
        if (code < 0 || code > 7) return m_defaultFg;
        return bright ? bri[code] : norm[code];
    } else {
        static const QColor norm[8] = {
            "#45475a", "#f38ba8", "#a6e3a1", "#f9e2af",
            "#89b4fa", "#f5c2e7", "#94e2d5", "#cdd6f4"
        };
        static const QColor bri[8] = {
            "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af",
            "#89b4fa", "#f5c2e7", "#94e2d5", "#ffffff"
        };
        if (code < 0 || code > 7) return m_defaultFg;
        return bright ? bri[code] : norm[code];
    }
}

QColor SshTerminalWidget::xterm256Color(int idx) const {
    if (idx < 0 || idx > 255) return m_defaultFg;

    // First 16: standard ANSI
    if (idx < 8)  return ansiColor(idx, false);
    if (idx < 16) return ansiColor(idx - 8, true);

    // 16-231: 6x6x6 color cube
    if (idx < 232) {
        int i  = idx - 16;
        int b  = i % 6;
        int g  = (i / 6) % 6;
        int r  = i / 36;
        auto c6 = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(c6(r), c6(g), c6(b));
    }

    // 232-255: grayscale
    int gray = 8 + (idx - 232) * 10;
    return QColor(gray, gray, gray);
}

// ============================================================================
// Keyboard input
// ============================================================================

void SshTerminalWidget::keyPressEvent(QKeyEvent* event) {
    if (!m_channel) { QWidget::keyPressEvent(event); return; }

    // Snap to bottom on any keypress
    m_scrollOffset = 0;
    int maxVal = (int)m_scrollback.size();
    m_scrollBar->blockSignals(true);
    m_scrollBar->setRange(0, maxVal);
    m_scrollBar->setValue(maxVal);
    m_scrollBar->blockSignals(false);

    QByteArray toSend;

    if (event->modifiers() & Qt::ControlModifier) {
        int key = event->key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            char ctrl = (char)(key - Qt::Key_A + 1);
            toSend.append(ctrl);
        } else if (key == Qt::Key_BracketLeft)  { toSend.append('\x1b'); }
        else if (key == Qt::Key_Backslash)       { toSend.append('\x1c'); }
        else if (key == Qt::Key_BracketRight)    { toSend.append('\x1d'); }
        else {
            toSend = event->text().toUtf8();
        }
    } else if (event->modifiers() & Qt::AltModifier) {
        toSend.append('\x1b');
        toSend.append(event->text().toUtf8());
    } else {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:    toSend = "\r";        break;
        case Qt::Key_Backspace:toSend = "\x7f";      break;
        case Qt::Key_Tab:      toSend = "\t";         break;
        case Qt::Key_Backtab:  toSend = "\x1b[Z";     break; // Shift+Tab
        case Qt::Key_Escape:   toSend = "\x1b";       break;
        case Qt::Key_Up:       toSend = m_appCursorKeys ? "\x1bOA" : "\x1b[A"; break;
        case Qt::Key_Down:     toSend = m_appCursorKeys ? "\x1bOB" : "\x1b[B"; break;
        case Qt::Key_Right:    toSend = m_appCursorKeys ? "\x1bOC" : "\x1b[C"; break;
        case Qt::Key_Left:     toSend = m_appCursorKeys ? "\x1bOD" : "\x1b[D"; break;
        case Qt::Key_Home:     toSend = m_appCursorKeys ? "\x1bOH" : "\x1b[H"; break;
        case Qt::Key_End:      toSend = m_appCursorKeys ? "\x1bOF" : "\x1b[F"; break;
        case Qt::Key_Delete:   toSend = "\x1b[3~";    break;
        case Qt::Key_Insert:   toSend = "\x1b[2~";    break;
        case Qt::Key_PageUp:   toSend = "\x1b[5~";    break;
        case Qt::Key_PageDown: toSend = "\x1b[6~";    break;
        case Qt::Key_F1:       toSend = "\x1bOP";     break;
        case Qt::Key_F2:       toSend = "\x1bOQ";     break;
        case Qt::Key_F3:       toSend = "\x1bOR";     break;
        case Qt::Key_F4:       toSend = "\x1bOS";     break;
        case Qt::Key_F5:       toSend = "\x1b[15~";   break;
        case Qt::Key_F6:       toSend = "\x1b[17~";   break;
        case Qt::Key_F7:       toSend = "\x1b[18~";   break;
        case Qt::Key_F8:       toSend = "\x1b[19~";   break;
        case Qt::Key_F9:       toSend = "\x1b[20~";   break;
        case Qt::Key_F10:      toSend = "\x1b[21~";   break;
        case Qt::Key_F11:      toSend = "\x1b[23~";   break;
        case Qt::Key_F12:      toSend = "\x1b[24~";   break;
        default:
            toSend = event->text().toUtf8();
        }
    }

    if (!toSend.isEmpty()) writeToChannel(toSend);
}

// ============================================================================
// Channel write
// ============================================================================

void SshTerminalWidget::writeToChannel(const QByteArray& data) {
    if (!m_channel) return;
    const char* ptr = data.constData();
    int remaining   = data.size();
    while (remaining > 0) {
        int n = libssh2_channel_write(m_channel, ptr, remaining);
        if (n == LIBSSH2_ERROR_EAGAIN) { QThread::msleep(5); continue; }
        if (n <= 0) break;
        ptr       += n;
        remaining -= n;
    }
}

// ============================================================================
// Resize / PTY
// ============================================================================

void SshTerminalWidget::updatePtySize() {
    recalcCellSize();
    if (m_cellW <= 0 || m_cellH <= 0) return;

    int sbw  = (m_scrollBar && m_scrollBar->isVisible()) ? m_scrollBar->width() : 0;
    int availW = width() - sbw;
    int availH = height();

    // Widget henüz layout'a oturmamışsa (örn. 0x0 veya çok küçük başlangıç geometrisi)
    // standart güvenli boyutları (80x24) koru, PTY'yi bozuk açma.
    if (availW < 100 || availH < 50) {
        if (m_cols < 20 || m_rows < 4) {
            resizeScreen(80, 24);
        }
        return;
    }

    int computedCols = qMax(20, availW / m_cellW);
    int computedRows = qMax(4, availH / m_cellH);

    if (computedCols != m_cols || computedRows != m_rows) {
        resizeScreen(computedCols, computedRows);
    }
}

void SshTerminalWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    int oldCols = m_cols;
    int oldRows = m_rows;
    updatePtySize();
    if (m_channel && m_client && (m_cols != oldCols || m_rows != oldRows)) {
        m_client->resizePty(m_channel, m_cols, m_rows);
    }
}

void SshTerminalWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    int oldCols = m_cols;
    int oldRows = m_rows;
    updatePtySize();
    if (m_channel && m_client && (m_cols != oldCols || m_rows != oldRows)) {
        m_client->resizePty(m_channel, m_cols, m_rows);
    }
}

// ============================================================================
// Scroll wheel
// ============================================================================

void SshTerminalWidget::wheelEvent(QWheelEvent* event) {
    if (m_usingAltScreen) return;  // alt screen (vim etc.) handles scroll itself

    int delta = event->angleDelta().y();
    int lines = delta / 40;  // each 40 units ≈ 1 line
    if (lines == 0) lines = delta > 0 ? 1 : -1;

    int maxOff = (int)m_scrollback.size();
    m_scrollOffset = qBound(0, m_scrollOffset + lines, maxOff);

    // Sync scrollbar
    m_scrollBar->blockSignals(true);
    m_scrollBar->setRange(0, maxOff);
    m_scrollBar->setValue(maxOff - m_scrollOffset);
    m_scrollBar->blockSignals(false);

    update();
}

// ============================================================================
// Focus — cursor blink restart
// ============================================================================

void SshTerminalWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    m_cursorBlink = true;
    m_blinkTimer->start();
    update();
}

void SshTerminalWidget::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    m_cursorBlink = true;  // show cursor solid when unfocused
    m_blinkTimer->stop();
    update();
}

// Tab / Shift+Tab tuşlarının Qt focus traversal yapmasını engelle.
// Terminal aktifken bu tuşlar doğrudan SSH PTY'ye gönderilmeli.
bool SshTerminalWidget::focusNextPrevChild(bool /*next*/) {
    // Aktif terminal oturumu varsa Tab'ı yutup keyPressEvent'e bırak.
    // Oturum yoksa varsayılan Qt davranışı (false = engelle, biz hep engelliyoruz).
    return false;
}

// ============================================================================
// Blink
// ============================================================================

void SshTerminalWidget::blinkCursor() {
    m_cursorBlink = !m_cursorBlink;
    // Only repaint the cursor cell
    if (m_cursor.row >= 0 && m_cursor.row < m_rows &&
        m_cursor.col >= 0 && m_cursor.col < m_cols) {
        update(m_cursor.col * m_cellW, m_cursor.row * m_cellH, m_cellW, m_cellH);
    }
}

// ============================================================================
// Font size
// ============================================================================

void SshTerminalWidget::setFontSize(int pt) {
    m_font.setPointSize(pt);
    setFont(m_font);
    recalcCellSize();
    int oldCols = m_cols;
    int oldRows = m_rows;
    updatePtySize();
    if (m_channel && m_client && (m_cols != oldCols || m_rows != oldRows)) {
        m_client->resizePty(m_channel, m_cols, m_rows);
    }
    update();
}

// ============================================================================
// Clear screen
// ============================================================================

void SshTerminalWidget::clearScreen() {
    eraseDisplay(2);
    m_cursor.row = 0;
    m_cursor.col = 0;
    m_wrapPending = false;
    m_scrollback.clear();
    m_scrollBar->blockSignals(true);
    m_scrollBar->setRange(0, 0);
    m_scrollBar->setValue(0);
    m_scrollBar->blockSignals(false);
    m_scrollOffset = 0;
    update();

    // Aktif SSH PTY oturumu varsa Form Feed (\x0c = Ctrl+L) gönder.
    // Bu sayede uzak kabuk (bash, zsh vb.) ekranı en üstten temizler ve
    // prompt'u (user@hostname:~$ ) en üst satıra yeniden çizer.
    if (m_channel) {
        writeToChannel("\x0c");
    }
}

// ============================================================================
// Helpers
// ============================================================================

void SshTerminalWidget::sendText(const QString& text) {
    writeToChannel(text.toUtf8());
}

void SshTerminalWidget::sendCommand(const QString& command) {
    writeToChannel((command + "\n").toUtf8());
}

QSize SshTerminalWidget::sizeHint() const {
    int sbw = (m_scrollBar && m_scrollBar->isVisible()) ? m_scrollBar->width() : 16;
    return QSize(80 * m_cellW + sbw, 24 * m_cellH);
}

QSize SshTerminalWidget::minimumSizeHint() const {
    int sbw = (m_scrollBar && m_scrollBar->isVisible()) ? m_scrollBar->width() : 16;
    return QSize(20 * m_cellW + sbw, 4 * m_cellH);
}

// ============================================================================
// Theme
// ============================================================================

void SshTerminalWidget::updateThemeColors() {
    QColor oldBg = m_defaultBg;
    QColor oldFg = m_defaultFg;

    QPalette pal = qApp ? qApp->palette() : palette();
    m_defaultBg = pal.color(QPalette::Base);
    m_defaultFg = pal.color(QPalette::Text);

    m_cursor.fg = m_defaultFg;
    m_cursor.bg = m_defaultBg;

    auto updateCellColors = [&](Cell& c) {
        if (!c.fg.isValid() || c.fg == oldFg) {
            c.fg = m_defaultFg;
        }
        if (!c.bg.isValid() || c.bg == oldBg) {
            c.bg = m_defaultBg;
        }
    };

    for (auto& row : m_screen) {
        for (auto& c : row) {
            updateCellColors(c);
        }
    }
    for (auto& row : m_altScreen) {
        for (auto& c : row) {
            updateCellColors(c);
        }
    }
    for (auto& row : m_scrollback) {
        for (auto& c : row) {
            updateCellColors(c);
        }
    }

    setStyleSheet("background: transparent;");
    update();
}

// ============================================================================
// Selection & Clipboard
// ============================================================================

#include <QClipboard>
#include <QApplication>
#include <QMenu>

SshTerminalWidget::BufferPos SshTerminalWidget::coordsToBufferPos(const QPoint& pos) const {
    BufferPos bpos;
    int rowOffset = pos.y() / m_cellH;
    int colOffset = pos.x() / m_cellW;

    bpos.col = qBound(0, colOffset, m_cols - 1);

    if (m_usingAltScreen) {
        bpos.row = qBound(0, rowOffset, m_rows - 1);
    } else {
        int sbRows = 0;
        if (m_scrollOffset > 0) {
            sbRows = qMin(m_scrollOffset, (int)m_scrollback.size());
        }

        if (rowOffset < sbRows) {
            bpos.row = (int)m_scrollback.size() - m_scrollOffset + rowOffset;
        } else {
            bpos.row = (int)m_scrollback.size() + (rowOffset - sbRows);
        }
        bpos.row = qBound(0, bpos.row, (int)m_scrollback.size() + m_rows - 1);
    }
    return bpos;
}

bool SshTerminalWidget::hasSelection() const {
    return m_selStart != m_selEnd;
}

bool SshTerminalWidget::isPosSelected(const BufferPos& pos) const {
    if (!hasSelection()) return false;
    BufferPos start = m_selStart;
    BufferPos end = m_selEnd;
    if (end < start) {
        std::swap(start, end);
    }
    // Check if pos is within [start, end)
    if (pos.row < start.row || pos.row > end.row) return false;
    if (pos.row == start.row && pos.row == end.row) {
        return pos.col >= start.col && pos.col < end.col;
    }
    if (pos.row == start.row) {
        return pos.col >= start.col;
    }
    if (pos.row == end.row) {
        return pos.col < end.col;
    }
    return true;
}

QString SshTerminalWidget::selectedText() const {
    if (!hasSelection()) return QString();
    BufferPos start = m_selStart;
    BufferPos end = m_selEnd;
    if (end < start) {
        std::swap(start, end);
    }

    QString text;
    for (int r = start.row; r <= end.row; ++r) {
        int cStart = (r == start.row) ? start.col : 0;
        int cEnd = (r == end.row) ? end.col : m_cols;

        // Retrieve row cells
        QVector<Cell> rowCells;
        if (m_usingAltScreen) {
            if (r >= 0 && r < m_altScreen.size()) rowCells = m_altScreen[r];
        } else {
            int sbSize = (int)m_scrollback.size();
            if (r < sbSize) {
                rowCells = m_scrollback[r];
            } else {
                int screenRow = r - sbSize;
                if (screenRow >= 0 && screenRow < m_screen.size()) rowCells = m_screen[screenRow];
            }
        }

        QString lineText;
        for (int c = cStart; c < cEnd && c < rowCells.size(); ++c) {
            lineText.append(rowCells[c].ch);
        }
        // Trim trailing space but keep layout
        while (!lineText.isEmpty() && lineText.endsWith(' ')) {
            lineText.chop(1);
        }
        text.append(lineText);
        if (r < end.row) {
            text.append('\n');
        }
    }
    return text;
}

void SshTerminalWidget::copyToClipboard() {
    QString txt = selectedText();
    if (!txt.isEmpty()) {
        QGuiApplication::clipboard()->setText(txt);
    }
}

void SshTerminalWidget::pasteFromClipboard() {
    QString txt = QGuiApplication::clipboard()->text();
    if (!txt.isEmpty()) {
        sendText(txt);
    }
}

void SshTerminalWidget::mousePressEvent(QMouseEvent* event) {
    setFocus();
    if (event->button() == Qt::LeftButton) {
        m_selecting = true;
        m_selStart = coordsToBufferPos(event->pos());
        m_selEnd = m_selStart;
        update();
    }
    QWidget::mousePressEvent(event);
}

void SshTerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_selecting) {
        m_selEnd = coordsToBufferPos(event->pos());
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void SshTerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_selecting = false;
        m_selEnd = coordsToBufferPos(event->pos());
        if (m_selStart == m_selEnd) {
            // Clear selection on single click
            m_selStart = BufferPos();
            m_selEnd = BufferPos();
        } else {
            // Copy selected text directly to selection clipboard (Unix style)
            #ifdef Q_OS_LINUX
            QString txt = selectedText();
            if (!txt.isEmpty()) {
                QGuiApplication::clipboard()->setText(txt, QClipboard::Selection);
            }
            #endif
        }
        update();
    }
    QWidget::mouseReleaseEvent(event);
}

void SshTerminalWidget::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);

    // Apply theme stylesheet styling to QMenu so it does not look completely black and unreadable
    QString style = QString(
        "QMenu {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "}"
        "QMenu::item {"
        "  background-color: transparent;"
        "  padding: 6px 24px 6px 24px;"
        "}"
        "QMenu::item:selected {"
        "  background-color: %4;"
        "  color: %5;"
        "}"
        "QMenu::item:disabled {"
        "  color: %6;"
        "}"
    ).arg(m_defaultBg.name())
     .arg(m_defaultFg.name())
     .arg(m_defaultFg.lighter(150).name())
     .arg(palette().color(QPalette::Highlight).name())
     .arg(palette().color(QPalette::HighlightedText).name())
     .arg(m_defaultFg.darker(150).name());

    menu.setStyleSheet(style);

    QAction* copyAct = menu.addAction(tr("Kopyala"));
    copyAct->setEnabled(hasSelection());
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, &SshTerminalWidget::copyToClipboard);

    QAction* pasteAct = menu.addAction(tr("Yapıştır"));
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, this, &SshTerminalWidget::pasteFromClipboard);

    menu.exec(event->globalPos());
}
