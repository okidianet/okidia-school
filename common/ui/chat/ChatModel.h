#pragma once

#include <QAbstractListModel>
#include <QList>
#include "ChatMessage.h"

namespace ui {
namespace chat {

class ChatModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum ChatRoles {
        IdRole = Qt::UserRole + 1,
        SenderNameRole,
        ContentRole,
        TimestampRole,
        IsMineRole,
        IsSystemRole,
        MsgTypeRole,
        SenderRole,
        MetadataRole
    };

    explicit ChatModel(QObject* parent = nullptr);

    // QAbstractListModel overrides
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // API
    void addMessage(const ChatMessage& msg);
    void setMessages(const QList<ChatMessage>& messages);
    void clearMessages();
    const ChatMessage& messageAt(int row) const;

private:
    QList<ChatMessage> m_messages;
};

} // namespace chat
} // namespace ui
