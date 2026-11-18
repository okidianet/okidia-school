// master/transfer/FileSender.cpp
#include "FileSender.h"
#include "../../common/utils/Logger.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <QTimer>
#include <QRandomGenerator>
#include <atomic>

static uint64_t generateUniqueTransferId() {
    static std::atomic<uint64_t> s_transferCounter{1000};
    uint64_t low = s_transferCounter.fetch_add(1);
    uint64_t high = static_cast<uint64_t>(QRandomGenerator::global()->generate()) << 32;
    uint64_t res = high | (low & 0xFFFFFFFFULL);
    return (res == 0) ? 1 : res;
}

FileSender::FileSender(Connection* conn, QObject* parent)
    : QObject(parent)
    , m_conn(conn) {
    m_timer = new QTimer(this);
    m_timer->setInterval(10); // chunk'lar arası 10ms → ağı boğmaz
    connect(m_timer, &QTimer::timeout,
            this, &FileSender::sendNextChunk);
}

bool FileSender::sendToClient(const QString& clientId,
                               const QString& filePath,
                               bool readOnly,
                               const QString& destination,
                               const QString& overrideFilename) {
    return startTransfer(filePath, clientId,
                         generateUniqueTransferId(),
                         readOnly,
                         destination,
                         overrideFilename);
}

bool FileSender::sendToAll(const QString& filePath,
                           bool readOnly,
                           const QString& destination,
                           const QString& overrideFilename) {
    return startTransfer(filePath, "",
                         generateUniqueTransferId(),
                         readOnly,
                         destination,
                         overrideFilename);
}

bool FileSender::startTransfer(const QString& filePath,
                                const QString& targetId,
                                uint64_t sequenceId,
                                bool readOnly,
                                const QString& destination,
                                const QString& overrideFilename) {
    if (!m_conn || !m_conn->isConnected()) {
        emit finished(false, "Bağlantı yok");
        return false;
    }

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("FileSender: Dosya açılamadı → ") + filePath);
        emit finished(false, "Dosya açılamadı");
        return false;
    }

    QFileInfo info(filePath);
    m_totalSize  = static_cast<quint64>(info.size());
    m_chunkIndex = 0;
    m_sequenceId = sequenceId;
    m_targetId   = targetId;

    QMimeDatabase mimeDb;
    QString mimeType = mimeDb.mimeTypeForFile(filePath)
                               .name().toStdString().c_str();

    // BEGIN
    okidia::FileTransferBegin begin;
    QString targetName = overrideFilename.trimmed().isEmpty() ? info.fileName() : overrideFilename.trimmed();
    begin.set_filename(targetName.toStdString());
    begin.set_file_size(m_totalSize);
    begin.set_mime_type(mimeType.toStdString());
    begin.set_read_only(readOnly);
    begin.set_destination(destination.toStdString());

    okidia::Envelope env;
    env.set_type(okidia::MessageType::FILE_TRANSFER_BEGIN);
    env.set_sequence_id(m_sequenceId);
    env.set_target_id(m_targetId.toStdString());
    env.set_payload(begin.SerializeAsString());
    m_conn->sendEnvelope(env);

    Logger::info(QObject::tr("FileSender: Transfer başladı → ") +
                 info.fileName() +
                 " (" + QString::number(m_totalSize / 1024) + QObject::tr(" KB)"));

    m_timer->start();
    return true;
}

void FileSender::sendNextChunk() {
    if (!m_file.isOpen()) {
        m_timer->stop();
        return;
    }

    QByteArray data = m_file.read(CHUNK_SIZE);
    if (data.isEmpty()) {
        // Dosya bitti — END gönder
        m_timer->stop();
        m_file.close();

        okidia::Envelope env;
        env.set_type(okidia::MessageType::FILE_TRANSFER_END);
        env.set_sequence_id(m_sequenceId);
        env.set_target_id(m_targetId.toStdString());
        m_conn->sendEnvelope(env);

        Logger::info(QObject::tr("FileSender: Transfer tamamlandı"));
        emit progress(100);
        emit finished(true, QFileInfo(m_file.fileName()).fileName());
        return;
    }

    bool isLast = m_file.atEnd();

    okidia::FileTransferChunk chunk;
    chunk.set_chunk_index(m_chunkIndex++);
    chunk.set_data(data.toStdString());
    chunk.set_is_last(isLast);

    okidia::Envelope env;
    env.set_type(okidia::MessageType::FILE_TRANSFER_CHUNK);
    env.set_sequence_id(m_sequenceId);
    env.set_target_id(m_targetId.toStdString());
    env.set_payload(chunk.SerializeAsString());
    m_conn->sendEnvelope(env);

    // İlerleme yüzdesi
    quint64 sent = static_cast<quint64>(m_chunkIndex) * CHUNK_SIZE;
    int pct = m_totalSize > 0
        ? static_cast<int>(qMin(sent * 100 / m_totalSize,
                                 static_cast<quint64>(99)))
        : 0;
    emit progress(pct);

    if (isLast) {
        m_timer->stop();
        m_file.close();
        okidia::Envelope endEnv;
        endEnv.set_type(okidia::MessageType::FILE_TRANSFER_END);
        endEnv.set_sequence_id(m_sequenceId);
        endEnv.set_target_id(m_targetId.toStdString());
        m_conn->sendEnvelope(endEnv);

        Logger::info(QObject::tr("FileSender: Transfer tamamlandı (isLast)"));
        emit progress(100);
        emit finished(true,
            QFileInfo(m_file.fileName()).fileName());
    }
}
