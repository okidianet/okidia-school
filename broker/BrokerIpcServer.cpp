#include "BrokerIpcServer.h"
#include "../common/utils/Logger.h"

BrokerIpcServer::BrokerIpcServer(QObject* parent) : QObject(parent) {
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::WorldAccessOption);
    connect(m_server, &QLocalServer::newConnection,
            this,     &BrokerIpcServer::onNewConnection);
}

bool BrokerIpcServer::listen(const QString& path) {
    QLocalServer::removeServer(path);
    if (!m_server->listen(path)) {
        Logger::error(QObject::tr("BrokerIpc: Socket açılamadı → ") + path
                      + " : " + m_server->errorString());
        return false;
    }
    Logger::info(QObject::tr("BrokerIpc: Tray bekleniyor → ") + path);
    return true;
}

void BrokerIpcServer::broadcastStatus(const okidia::BrokerStatus& st) {
    okidia::Envelope env;
    env.set_type(okidia::MessageType::BROKER_STATUS);
    env.set_payload(st.SerializeAsString());
    for (auto* c : m_trayConns)
        c->sendEnvelope(env);
}

void BrokerIpcServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        auto* sock = m_server->nextPendingConnection();
        auto* conn = new LocalConnection(sock, this);
        m_trayConns.append(conn);
        Logger::info(QObject::tr("BrokerIpc: Tray bağlandı"));
        connect(conn, &LocalConnection::envelopeReceived,
                this, [this](const okidia::Envelope& env) {
            if (env.type() == okidia::MessageType::BROKER_CMD) {
                okidia::BrokerCommand cmd;
                if (cmd.ParseFromString(env.payload()))
                    emit cmdReceived(cmd);
            }
        });
        connect(conn, &LocalConnection::disconnected,
                this, [this, conn]() {
            m_trayConns.removeAll(conn);
            conn->deleteLater();
            Logger::info(QObject::tr("BrokerIpc: Tray bağlantısı kesildi"));
        });
    }
}
