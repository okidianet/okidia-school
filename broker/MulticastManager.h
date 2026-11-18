// broker/MulticastManager.h
#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QFile>
#include <QTimer>
#include "../common/network/Connection.h"
#include "ClientRegistry.h"
#include "messages.pb.h"

class MulticastManager : public QObject {
    Q_OBJECT

public:
    explicit MulticastManager(ClientRegistry* registry, QObject* parent = nullptr);
    ~MulticastManager();

    // Gelen zarfları (Master'dan yükleme) işle
    void handleEnvelope(Connection* conn, const okidia::Envelope& env);

    // İstemci tekrar bağlandığında denemeyi yeniden başlat
    void handleClientReconnect(const QString& clientId);

private:
    struct ClientStream {
        QString   clientId;
        QFile     file;
        quint32   chunkIndex = 0;
        quint64   totalSize  = 0;
        bool      readOnly   = false;
        QString   filename;
        uint64_t  sequenceId = 0;
        int       retryCount = 0;
        QTimer*   timer      = nullptr;
        bool      isAnnounce = false;
        int       repeatCount = 1;
        enum State { PENDING, SENDING, SUCCESS, FAILED };
        State     state      = PENDING;
    };

    struct MulticastTask {
        uint64_t  sequenceId = 0;
        QString   filename;
        quint64   fileSize   = 0;
        bool      readOnly   = false;
        QString   tempFilePath;
        QFile     tempFile;
        bool      uploadComplete = false;
        bool      isAnnounce = false;
        int       repeatCount = 1;
        QMap<QString, ClientStream*> clients;
    };

    ClientRegistry* m_registry = nullptr;
    QMap<uint64_t, MulticastTask*> m_tasks;

    void processUploadBegin(uint64_t seqId, const QString& targetsCsv, const okidia::FileTransferBegin& begin, const QString& masterUserId = "");
    void processUploadChunk(uint64_t seqId, const okidia::FileTransferChunk& chunk);
    void processUploadEnd(uint64_t seqId);

    void startClientStream(MulticastTask* task, ClientStream* stream);
    void stopClientStream(ClientStream* stream);
    void checkAnnouncePlaybackTrigger(MulticastTask* task);
    void checkTaskCompletion(MulticastTask* task);
    void deleteMulticastTask(uint64_t seqId);
};
