#include "ChatModel.h"

namespace ui {
namespace chat {

ChatModel::ChatModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ChatModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return m_messages.count();
}

QVariant ChatModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_messages.count())
        return QVariant();

    const ChatMessage& msg = m_messages.at(index.row());

    switch (role) {
    case IdRole:          return msg.id;
    case SenderNameRole:  return msg.senderName;
    case ContentRole:     return msg.content;
    case TimestampRole:   return QVariant::fromValue(msg.timestamp);
    case IsMineRole:      return msg.isMine;
    case IsSystemRole:    return msg.isSystem;
    case MsgTypeRole:     return QVariant::fromValue(static_cast<int>(msg.msgType));
    case SenderRole:      return QVariant::fromValue(static_cast<int>(msg.senderRole));
    case MetadataRole:    return msg.metadata;
    case Qt::DisplayRole: return msg.content;
    default:              return QVariant();
    }
}

QHash<int, QByteArray> ChatModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]         = "id";
    roles[SenderNameRole] = "senderName";
    roles[ContentRole]    = "content";
    roles[TimestampRole]  = "timestamp";
    roles[IsMineRole]     = "isMine";
    roles[IsSystemRole]   = "isSystem";
    roles[MsgTypeRole]    = "msgType";
    roles[SenderRole]     = "senderRole";
    roles[MetadataRole]   = "metadata";
    return roles;
}

void ChatModel::addMessage(const ChatMessage& msg)
{
    beginInsertRows(QModelIndex(), m_messages.count(), m_messages.count());
    m_messages.append(msg);
    endInsertRows();
}

void ChatModel::setMessages(const QList<ChatMessage>& messages)
{
    beginResetModel();
    m_messages = messages;
    endResetModel();
}

void ChatModel::clearMessages()
{
    beginResetModel();
    m_messages.clear();
    endResetModel();
}

const ChatMessage& ChatModel::messageAt(int row) const
{
    return m_messages.at(row);
}

} // namespace chat
} // namespace ui
