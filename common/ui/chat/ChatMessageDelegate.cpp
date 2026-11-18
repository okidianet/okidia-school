#include "ChatMessageDelegate.h"
#include "ChatModel.h"
#include "../ThemeManager.h"
#include <QDateTime>
#include <QPainterPath>
#include <QApplication>
#include <QFontMetrics>

namespace ui {
namespace chat {

ChatMessageDelegate::ChatMessageDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize ChatMessageDelegate::calculateSize(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QString content = index.data(ChatModel::ContentRole).toString();
    bool isSystem = index.data(ChatModel::IsSystemRole).toBool();

    if (isSystem) {
        QFont font = option.font;
        font.setPointSize(font.pointSize() > 9 ? font.pointSize() - 1 : 8);
        font.setBold(true);
        QFontMetrics fm(font);
        QRect br = fm.boundingRect(QRect(0, 0, option.rect.width() - 40, 1000), Qt::TextWordWrap | Qt::AlignCenter, content);
        return QSize(option.rect.width(), br.height() + 24);
    }

    QFontMetrics fm(option.font);
    int maxWidth = static_cast<int>(option.rect.width() * 0.72);
    if (maxWidth < 220) maxWidth = 220;

    QRect textRect = fm.boundingRect(QRect(0, 0, maxWidth - (m_padding * 2), 10000), Qt::TextWordWrap, content);
    
    // Space for sender name and timestamp
    bool isMine = index.data(ChatModel::IsMineRole).toBool();
    int headerHeight = isMine ? 0 : 18;
    int height = textRect.height() + m_padding * 2 + headerHeight + 14; 
    return QSize(option.rect.width(), height + m_margin * 2);
}

QSize ChatMessageDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    return calculateSize(option, index);
}

void ChatMessageDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    bool isMine = index.data(ChatModel::IsMineRole).toBool();
    bool isSystem = index.data(ChatModel::IsSystemRole).toBool();
    int senderRole = index.data(ChatModel::SenderRole).toInt();
    QString senderName = index.data(ChatModel::SenderNameRole).toString();
    QString content = index.data(ChatModel::ContentRole).toString();
    qint64 timestamp = index.data(ChatModel::TimestampRole).toLongLong();

    auto mode = ThemeManager::instance().currentMode();
    bool isDark = (mode == ThemeManager::ThemeMode::Dark);

    if (isSystem) {
        QFont font = option.font;
        font.setPointSize(font.pointSize() > 9 ? font.pointSize() - 1 : 8);
        font.setBold(true);
        painter->setFont(font);

        QFontMetrics fm(font);
        QRect textBounding = fm.boundingRect(QRect(0, 0, option.rect.width() - 60, 1000), Qt::TextWordWrap | Qt::AlignCenter, content);
        
        int pillWidth = textBounding.width() + 28;
        int pillHeight = textBounding.height() + 10;
        QRect pillRect((option.rect.width() - pillWidth) / 2, option.rect.top() + (option.rect.height() - pillHeight) / 2, pillWidth, pillHeight);

        QColor pillBg = isDark ? QColor(45, 55, 72, 180) : QColor(226, 232, 240, 200);
        QColor pillBorder = isDark ? QColor(74, 85, 104, 120) : QColor(203, 213, 225, 180);
        QColor textColor = isDark ? QColor("#CBD5E1") : QColor("#475569");

        painter->setPen(QPen(pillBorder, 1));
        painter->setBrush(pillBg);
        painter->drawRoundedRect(pillRect, pillHeight / 2, pillHeight / 2);

        painter->setPen(textColor);
        painter->drawText(pillRect, Qt::AlignCenter | Qt::TextWordWrap, content);
        painter->restore();
        return;
    }

    // Measure text
    QFontMetrics fm(option.font);
    int maxTextWidth = static_cast<int>(option.rect.width() * 0.72);
    if (maxTextWidth < 220) maxTextWidth = 220;

    QRect textBoundingRect = fm.boundingRect(QRect(0, 0, maxTextWidth - (m_padding * 2), 10000), Qt::TextWordWrap, content);
    
    // Sender name width measurement if applicable
    int headerHeight = isMine ? 0 : 18;
    int senderWidth = 0;
    if (!isMine && !senderName.isEmpty()) {
        QFont nameFont = option.font;
        nameFont.setBold(true);
        QFontMetrics nfm(nameFont);
        senderWidth = nfm.horizontalAdvance(senderName) + 10;
    }

    int bubbleWidth = qMax(textBoundingRect.width() + m_padding * 2, senderWidth + m_padding * 2);
    if (bubbleWidth < 140) bubbleWidth = 140;
    
    int bubbleHeight = textBoundingRect.height() + m_padding * 2 + headerHeight + 14;

    QRect bubbleRect;
    if (isMine) {
        bubbleRect = QRect(option.rect.right() - bubbleWidth - m_margin, option.rect.top() + m_margin, bubbleWidth, bubbleHeight);
    } else {
        bubbleRect = QRect(option.rect.left() + m_margin, option.rect.top() + m_margin, bubbleWidth, bubbleHeight);
    }

    drawBubble(painter, option, index, bubbleRect, isMine, senderRole, isSystem);
    
    QRect textRect = bubbleRect.adjusted(m_padding, m_padding, -m_padding, -m_padding);
    drawText(painter, option, index, textRect, senderName, content, timestamp, isMine, senderRole, isSystem);

    painter->restore();
}

void ChatMessageDelegate::drawBubble(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                                     const QRect& bubbleRect, bool isMine, int senderRole, bool isSystem) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    Q_UNUSED(isSystem);

    QColor bgColor;
    QColor borderColor = Qt::transparent;
    int borderWidth = 0;

    auto mode = ThemeManager::instance().currentMode();
    bool isDark = (mode == ThemeManager::ThemeMode::Dark);

    if (isMine) {
        // Kendi mesajımız (Modern Emerald/Teal)
        bgColor = isDark ? QColor("#0d5c46") : QColor("#10b981");
    } else {
        // Karşı taraf
        if (senderRole == 1) { 
            // 👑 YM - Yönetici (Altın sarısı / Amber vurgu)
            bgColor = isDark ? QColor("#382d14") : QColor("#fef3c7");
            borderColor = isDark ? QColor("#f59e0b") : QColor("#d97706");
            borderWidth = 1;
        } else if (senderRole == 4) {
            // 🔍 Gözlemci (Mor / Violet vurgu)
            bgColor = isDark ? QColor("#2e1065") : QColor("#f5f3ff");
            borderColor = isDark ? QColor("#a855f7") : QColor("#7e22ce");
            borderWidth = 1;
        } else if (senderRole == 2) {
            // 🎓 OM - Öğretmen (Mavi / Indigo vurgu)
            bgColor = isDark ? QColor("#172e4d") : QColor("#e0f2fe");
            borderColor = isDark ? QColor("#38bdf8") : QColor("#0284c7");
            borderWidth = 1;
        } else {
            // Agent / Öğrenci
            bgColor = isDark ? QColor("#262b33") : QColor("#f1f5f9");
            borderColor = isDark ? QColor("#374151") : QColor("#e2e8f0");
            borderWidth = 1;
        }
    }

    painter->setBrush(bgColor);
    if (borderWidth > 0) {
        painter->setPen(QPen(borderColor, borderWidth));
    } else {
        painter->setPen(Qt::NoPen);
    }

    painter->drawRoundedRect(bubbleRect, 12, 12);
}

void ChatMessageDelegate::drawText(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                                   const QRect& textRect, const QString& senderName, const QString& content, 
                                   qint64 timestamp, bool isMine, int senderRole, bool isSystem) const
{
    Q_UNUSED(index);
    Q_UNUSED(isSystem);

    bool isDark = (ThemeManager::instance().currentMode() == ThemeManager::ThemeMode::Dark);

    // Gönderici Adı ve Rozeti
    if (!isMine) {
        QFont nameFont = option.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        
        QColor nameColor;
        QString displaySender = senderName;
        if (senderRole == 1) {
            nameColor = isDark ? QColor("#fbbf24") : QColor("#b45309"); // YM Altın
            if (!displaySender.startsWith("👑") && !displaySender.contains("Yönetici")) {
                displaySender = "👑 " + displaySender;
            }
        } else if (senderRole == 4) {
            nameColor = isDark ? QColor("#c084fc") : QColor("#7e22ce"); // Gözlemci Mor
            if (!displaySender.contains("Gözlemci")) {
                displaySender = "[" + QObject::tr("Gözlemci") + "] " + displaySender;
            }
        } else if (senderRole == 2) {
            nameColor = isDark ? QColor("#60a5fa") : QColor("#0369a1"); // OM Mavi
            if (!displaySender.startsWith("🎓") && !displaySender.contains("Öğretmen")) {
                displaySender = "🎓 " + displaySender;
            }
        } else {
            nameColor = isDark ? QColor("#94a3b8") : QColor("#475569"); // Agent
        }

        painter->setPen(nameColor);
        painter->drawText(QRect(textRect.x(), textRect.y(), textRect.width(), 16), Qt::AlignLeft | Qt::AlignVCenter, displaySender);
    }

    // Mesaj içeriği
    painter->setFont(option.font);
    if (isMine) {
        painter->setPen(Qt::white);
    } else {
        painter->setPen(isDark ? QColor("#f8fafc") : QColor("#0f172a"));
    }
    
    int contentYOffset = isMine ? 0 : 18;
    int contentHeight = textRect.height() - contentYOffset - 14;
    QRect contentRect = QRect(textRect.x(), textRect.y() + contentYOffset, textRect.width(), contentHeight);
    painter->drawText(contentRect, Qt::TextWordWrap | Qt::AlignLeft, content);

    // Zaman Damgası
    QFont timeFont = option.font;
    timeFont.setPointSize(qMax(timeFont.pointSize() - 2, 7));
    painter->setFont(timeFont);
    
    if (isMine) {
        painter->setPen(QColor(255, 255, 255, 180));
    } else {
        painter->setPen(isDark ? QColor("#64748b") : QColor("#94a3b8"));
    }

    QDateTime dt = (timestamp > 0) ? QDateTime::fromSecsSinceEpoch(timestamp) : QDateTime::currentDateTime();
    QString timeStr = dt.toString("HH:mm");

    QRect timeRect(textRect.x(), textRect.bottom() - 12, textRect.width(), 12);
    painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, timeStr);
}

} // namespace chat
} // namespace ui
