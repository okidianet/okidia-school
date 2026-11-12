#ifndef BROKER_TRANSFER_MANAGER_H
#define BROKER_TRANSFER_MANAGER_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QDir>
#include <QMap>
#include "messages.pb.h"
#include "ClientRegistry.h"
#include "../common/network/Connection.h"

class BrokerTransferManager : public QObject {
    Q_OBJECT
public:
    explicit BrokerTransferManager(ClientRegistry* registry, QObject* parent = nullptr);
    ~BrokerTransferManager();

    bool handleOtaBegin(Connection* sender, const okidia::OtaUpdateBegin& begin);
    bool handleOtaChunk(Connection* sender, const okidia::OtaUpdateChunk& chunk);
    bool handleOtaEnd(Connection* sender, const okidia::Envelope& env); // Returns true if finished

    bool handleBrokerOtaBegin(Connection* sender, const okidia::OtaUpdateBegin& begin);
    bool handleBrokerOtaChunk(Connection* sender, const okidia::OtaUpdateChunk& chunk);
    bool handleBrokerOtaEnd(Connection* sender, const okidia::Envelope& env);

    void checkAndSendUpdateToAgent(Connection* agentConn, const QString& osTag, const QString& osArch, const QString& currentVersion, const QString& clientId = QString(), bool force = false);
    void checkAndNotifyMasterUpdate(Connection* masterConn, const QString& masterVersion, const QString& osTag, const QString& osArch);
    void handleMasterDownloadReq(Connection* sender, const okidia::MasterUpdateDownloadRequest& req);

    void handleUpdatesListReq(Connection* sender, const okidia::Envelope& env);
    void handleUpdatesDeleteReq(Connection* sender, const okidia::Envelope& env);

    void cleanOldOtaPackages(int maxAgeDays = 90);
    void notifyConnectedMastersIfUpdateAvailable();

    // ── Merkezi Varlık Depolama & Otonom İstemci Dağıtımı ─────────────────────
    bool handleAssetBegin(uint64_t seqId, const okidia::FileTransferBegin& begin);
    bool handleAssetChunk(uint64_t seqId, const okidia::FileTransferChunk& chunk);
    bool handleAssetEnd(uint64_t seqId);
    void syncAssetsToClient(Connection* agentConn, int agentType);

private slots:
    void onClientRegistered(const QString& clientId);
    void onOtaQueueTimeout();

private:
    void broadcastOtaPush(const QString& osTag, const QString& version, uint64_t fileSize);
    void broadcastMasterUpdateAvailable(const QString& osTag, const QString& version, const QString& filename, uint64_t fileSize,
                                        const QByteArray& sig, const QByteArray& hash, const QByteArray& cert);

    struct QueuedOtaTarget {
        QString clientId;
        QString osTag;
        QString osArch;
        QString agentVersion;
        QString hostname;
    };

    void processOtaQueue();
    void sendBrokerReportToMasters(const QString& message, bool success);

    struct ActiveTransfer {
        QString osTag;
        QString version;
        QString filename;
        uint64_t expectedSize;
        uint64_t receivedSize;
        QFile* file;
        QByteArray signature;
        QByteArray file_hash;
        QByteArray master_cert;
        bool isBrokerSelfUpdate = false;
    };

    struct ActiveAssetTransfer {
        QString filename;
        QString targetPath;
        uint64_t expectedSize = 0;
        uint64_t receivedSize = 0;
        QFile* file = nullptr;
    };

    struct SentUpdateRecord {
        QString targetVersion;
        qint64  timestampSec = 0;
        int     attemptCount = 0;
    };

    QMap<Connection*, ActiveTransfer*>   m_transfers;
    QMap<uint64_t, ActiveAssetTransfer*> m_assetTransfers;
    QMap<QString, SentUpdateRecord>    m_sentAgentUpdates;
    QList<QueuedOtaTarget>             m_pendingOtaQueue;
    QMap<QString, qint64>              m_activeOtaClients;
    QTimer*                            m_otaQueueTimer = nullptr;
    int                                m_totalOtaTargets = 0;
    static constexpr int               kMaxConcurrentOta = 4; // 3-4 istemci grubu
    static constexpr int               kOtaWaveTimeoutSec = 25;
    ClientRegistry* m_registry;
    QString m_updatesDir;
    QTimer* m_cleanupTimer = nullptr;
    QTimer* m_periodicMasterCheckTimer = nullptr;
};

#endif // BROKER_TRANSFER_MANAGER_H

