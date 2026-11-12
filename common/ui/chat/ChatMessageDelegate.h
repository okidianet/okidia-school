#pragma once

#include <QStyledItemDelegate>
#include <QObject>
#include <QPainter>

namespace ui {
namespace chat {

class ChatMessageDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ChatMessageDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    QSize calculateSize(const QStyleOptionViewItem& option, const QModelIndex& index) const;
    void drawBubble(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                    const QRect& bubbleRect, bool isMine, int senderRole, bool isSystem) const;
    void drawText(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                  const QRect& textRect, const QString& senderName, const QString& content, 
                  qint64 timestamp, bool isMine, int senderRole, bool isSystem) const;

    int m_padding = 10;
    int m_margin = 10;
    int m_maxWidth = 400; // Will be dynamic based on view width if possible
};

} // namespace chat
} // namespace ui
