// master/transfer/MasterUpdater.h
#pragma once

#include <QObject>
#include <QFile>
#include <QString>
#include "messages.pb.h"

class SessionManager;

class MasterUpdater : public QObject {
    Q_OBJECT

public:
    explicit MasterUpdater(SessionManager* session, QObject* parent = nullptr);
    ~MasterUpdater() override;

    bool handleEnvelope(const okidia::Envelope& envelope);
    bool isDownloading() const { return m_isDownloading; }

    static void installAndRestart(const QString& debFilePath, QWidget* parentWidget = nullptr);

signals:
    void downloadStarted(const QString& filename, quint64 totalBytes);
    void downloadProgress(quint64 receivedBytes, quint64 totalBytes, int percent);
    void downloadFinished(bool success, const QString& filePathOrError);

private:
    void handleBegin(uint64_t seqId, const okidia::FileTransferBegin& msg);
    void handleChunk(uint64_t seqId, const okidia::FileTransferChunk& msg);
    void handleEnd(uint64_t seqId);

    SessionManager* m_session;
    bool m_isDownloading = false;
    uint64_t m_currentSeqId = 0;
    QString m_targetFilePath;
    quint64 m_expectedSize = 0;
    quint64 m_receivedSize = 0;
    QFile m_file;
};
