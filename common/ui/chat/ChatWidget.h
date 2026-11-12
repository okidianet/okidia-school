#pragma once

#include <QWidget>
#include <QString>
#include <QPlainTextEdit>

class QListView;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;

namespace ui {
namespace chat {

class ChatModel;
class ChatMessageDelegate;

class ChatInputEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ChatInputEdit(QWidget* parent = nullptr);
    void adjustHeight();

signals:
    void returnPressed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    const int m_minHeight = 36;
    const int m_maxHeight = 115;
};

class ChatWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatWidget(QWidget* parent = nullptr);
    ~ChatWidget() override = default;

    ChatModel* model() const { return m_model; }
    void setModel(ChatModel* model);

    // Kilit durumu yönetimi
    void setLocked(bool locked, const QString& reason = QString());
    bool isLocked() const { return m_locked; }

signals:
    void messageReadyToSend(const QString& text);
    void messageContextMenuRequested(const QString& messageId, const QPoint& globalPos);

private slots:
    void onSendClicked();
    void onCustomContextMenuRequested(const QPoint& pos);
    void scrollToBottom();

private:
    QListView* m_listView;
    ChatInputEdit* m_inputEdit;
    QPushButton* m_sendBtn;

    ChatModel* m_model;
    ChatMessageDelegate* m_delegate;

    bool m_locked = false;
};

} // namespace chat
} // namespace ui
