#pragma once

#include "BrokerContext.h"
#include "../../common/network/Connection.h"
#include "messages.pb.h"

class ChatHandler {
public:
    explicit ChatHandler(BrokerContext* ctx);
    bool handle(const okidia::Envelope& env, Connection* conn, const QString& peerId, bool* isMaster);

private:
    BrokerContext* m_ctx;
};
