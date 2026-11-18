#include "ChatWidget.h"
#include "ChatModel.h"
#include "ChatMessageDelegate.h"
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QKeyEvent>
#include <QAbstractTextDocumentLayout>

#include <QTextBlock>
#include <QTextLayout>

namespace ui {
namespace chat {

// ── ChatInputEdit ─────────────────────────────────────────────────────────────

ChatInputEdit::ChatInputEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTabChangesFocus(true);

    document()->setDocumentMargin(4);
    setFixedHeight(m_minHeight);

    connect(this, &QPlainTextEdit::textChanged, this, &ChatInputEdit::adjustHeight);
}

void ChatInputEdit::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) {
            insertPlainText("\n");
            adjustHeight();
            event->accept();
            return;
        } else {
            emit returnPressed();
            event->accept();
            return;
        }
    }
    QPlainTextEdit::keyPressEvent(event);
}

void ChatInputEdit::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    adjustHeight();
}

void ChatInputEdit::adjustHeight()
{
    document()->setTextWidth(viewport()->width());

    int lineCount = 0;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        QTextLayout* layout = block.layout();
        if (layout) {
            int count = layout->lineCount();
            lineCount += (count > 0 ? count : 1);
        } else {
            lineCount += 1;
        }
    }
    if (lineCount < 1) lineCount = 1;

    int lineSpacing = fontMetrics().lineSpacing();
    if (lineSpacing < 14) lineSpacing = 18;

    int calculated = m_minHeight + (lineCount - 1) * lineSpacing;
    int targetHeight = qBound(m_minHeight, calculated, m_maxHeight);

    if (height() != targetHeight || minimumHeight() != targetHeight || maximumHeight() != targetHeight) {
        setFixedHeight(targetHeight);
        updateGeometry();
    }
}

// ── ChatWidget ────────────────────────────────────────────────────────────────

ChatWidget::ChatWidget(QWidget* parent)
    : QWidget(parent)
    , m_model(nullptr)
{
    QVBoxLayout* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);

    m_listView = new QListView(this);
    m_listView->setSelectionMode(QAbstractItemView::NoSelection);
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    
    m_delegate = new ChatMessageDelegate(this);
    m_listView->setItemDelegate(m_delegate);

    mainLay->addWidget(m_listView);

    QHBoxLayout* inputLay = new QHBoxLayout();
    inputLay->setContentsMargins(4, 4, 4, 4);
    inputLay->setSpacing(6);

    m_inputEdit = new ChatInputEdit(this);
    m_inputEdit->setPlaceholderText(tr("Mesajınızı yazın... (Enter ↵, Satır atla: Shift+Enter)"));
    
    m_sendBtn = new QPushButton(tr("Gönder"), this);
    m_sendBtn->setMinimumHeight(36);
    m_sendBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    inputLay->addWidget(m_inputEdit);
    inputLay->addWidget(m_sendBtn);
    inputLay->setAlignment(m_sendBtn, Qt::AlignBottom);

    mainLay->addLayout(inputLay);

    connect(m_sendBtn, &QPushButton::clicked, this, &ChatWidget::onSendClicked);
    connect(m_inputEdit, &ChatInputEdit::returnPressed, this, &ChatWidget::onSendClicked);
    connect(m_listView, &QListView::customContextMenuRequested, this, &ChatWidget::onCustomContextMenuRequested);
}

void ChatWidget::setModel(ChatModel* model)
{
    m_model = model;
    m_listView->setModel(m_model);
    
    if (m_model) {
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &ChatWidget::scrollToBottom);
        connect(m_model, &QAbstractItemModel::modelReset, this, &ChatWidget::scrollToBottom);
    }
}

void ChatWidget::setLocked(bool locked, const QString& reason)
{
    m_locked = locked;
    m_inputEdit->setDisabled(locked);
    m_sendBtn->setDisabled(locked);

    if (locked) {
        m_inputEdit->setPlaceholderText(reason.isEmpty() ? tr("🔇 Sohbet kilitli — mesaj gönderilemez") : "🔇 " + reason);
    } else {
        m_inputEdit->setPlaceholderText(tr("Mesajınızı yazın... (Enter ↵, Satır atla: Shift+Enter)"));
    }
}

void ChatWidget::onSendClicked()
{
    if (m_locked) return;
    
    QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;

    m_inputEdit->clear();
    m_inputEdit->adjustHeight();
    emit messageReadyToSend(text);
}

void ChatWidget::onCustomContextMenuRequested(const QPoint& pos)
{
    if (!m_model) return;
    
    QModelIndex index = m_listView->indexAt(pos);
    if (!index.isValid()) return;

    QString msgId = index.data(ChatModel::IdRole).toString();
    if (!msgId.isEmpty()) {
        emit messageContextMenuRequested(msgId, m_listView->viewport()->mapToGlobal(pos));
    }
}

void ChatWidget::scrollToBottom()
{
    if (m_model && m_model->rowCount() > 0) {
        m_listView->scrollTo(m_model->index(m_model->rowCount() - 1, 0), QAbstractItemView::PositionAtBottom);
    }
}

} // namespace chat
} // namespace ui
