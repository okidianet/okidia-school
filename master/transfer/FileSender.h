// master/transfer/FileSender.h
#pragma once

#include <QObject>
#include <QFile>
#include <QTimer>
#include "../../common/network/Connection.h"

/**
 * Seçilen dosyayı chunk'lara bölerek agent(ler)e gönderir.
 *
 * Kullanım:
 *   FileSender* sender = new FileSender(conn);
 *   sender->sendToClient(clientId, "/path/to/file.pdf");
 *   sender->sendToAll("/path/to/file.pdf");  // broadcast
 */
class FileSender : public QObject {
    Q_OBJECT

public:
    // Chunk boyutu: 64 KB — tek frame'e sığar, ağı yormaz
    static constexpr int CHUNK_SIZE = 64 * 1024;

    explicit FileSender(Connection* conn, QObject* parent = nullptr);

    // Belirli bir istemciye gönder (sequence_id = qHash(clientId))
    bool sendToClient(const QString& clientId,
                      const QString& filePath,
                      bool readOnly = false,
                      const QString& destination = "",
                      const QString& overrideFilename = "");

    // Tüm istemcilere gönder (sequence_id = 0)
    bool sendToAll(const QString& filePath,
                   bool readOnly = false,
                   const QString& destination = "",
                   const QString& overrideFilename = "");

    // Devam eden transferi anında iptal et
    void cancel();
    uint64_t sequenceId() const { return m_sequenceId; }

signals:
    void progress(int percent);
    void finished(bool success, const QString& filename);

private slots:
    void sendNextChunk();

private:
    Connection* m_conn       = nullptr;
    QFile       m_file;
    quint32     m_chunkIndex = 0;
    quint64     m_totalSize  = 0;
    uint64_t    m_sequenceId = 0;   // 0 = broadcast
    QString     m_targetId;
    QTimer*     m_timer      = nullptr;

    bool startTransfer(const QString& filePath, const QString& targetId, uint64_t sequenceId, bool readOnly = false, const QString& destination = "", const QString& overrideFilename = "");
};
