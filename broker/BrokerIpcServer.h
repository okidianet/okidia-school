#pragma once
#include <QObject>
#include <QLocalServer>
#include "messages.pb.h"
#include "../common/network/LocalConnection.h"

class BrokerIpcServer : public QObject {
    Q_OBJECT
public:
    explicit BrokerIpcServer(QObject* parent = nullptr);
    bool listen(const QString& path);
    void broadcastStatus(const okidia::BrokerStatus& st);

signals:
    void cmdReceived(const okidia::BrokerCommand& cmd);

private slots:
    void onNewConnection();

private:
    QLocalServer*           m_server;
    QList<LocalConnection*> m_trayConns;
};
