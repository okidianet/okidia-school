#pragma once

#include <QString>
#include "messages.pb.h"

namespace ui {
namespace chat {

struct ChatMessage {
    QString id;                 // Mesajın UUID'si (veya sunucu ID'si)
    QString senderName;         // Gösterilecek ad
    QString content;            // Metin içeriği
    qint64  timestamp = 0;      // Zaman damgası
    bool    isMine    = false;  // Kendi mesajımız mı (sağa yaslanacak)
    bool    isSystem  = false;  // Sistem mesajı mı (ortada bilgi olarak gösterilecek)
    
    // Mesaj içeriğinin türü (TEXT, IMAGE, FILE, vb.)
    okidia::ChatMessageType msgType = okidia::CHAT_MSG_TEXT;
    
    // Gönderenin rolü (Balon rengi için)
    okidia::SenderRole senderRole = okidia::ROLE_UNKNOWN;
    
    // Zengin içerikler için JSON verisi (örn. resim boyutu, dosya yolu)
    QString metadata;
};

} // namespace chat
} // namespace ui
