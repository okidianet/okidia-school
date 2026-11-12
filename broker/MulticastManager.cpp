// broker/MulticastManager.cpp
#include "MulticastManager.h"
#include "../common/utils/Logger.h"
#include "../common/db/OkidiaDatabase.h"
#include <QDir>
#include <QDateTime>

MulticastManager::MulticastManager(ClientRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry) {
    Logger::info(QObject::tr("MulticastManager: Başlatıldı"));
}

MulticastManager::~MulticastManager() {
    for (auto* task : m_tasks.values()) {
        if (task->tempFile.isOpen()) {
            task->tempFile.close();
        }
        QFile::remove(task->tempFilePath);
        for (auto* stream : task->clients.values()) {
            if (stream->timer) {
                stream->timer->stop();
            }
            delete stream;
        }
        delete task;
    }
    m_tasks.clear();
}

void MulticastManager::handleEnvelope(Connection* conn, const okidia::Envelope& env) {
    uint64_t seqId = env.sequence_id();

    if (env.type() == okidia::MessageType::FILE_TRANSFER_BEGIN) {
        okidia::FileTransferBegin begin;
        if (begin.ParseFromString(env.payload())) {
            QString targetCsv = QString::fromStdString(env.target_id());
            QString masterUserId = m_registry->authenticatedUserId(conn);
            processUploadBegin(seqId, targetCsv, begin, masterUserId);
        }
    } else if (env.type() == okidia::MessageType::FILE_TRANSFER_CHUNK) {
        okidia::FileTransferChunk chunk;
        if (chunk.ParseFromString(env.payload())) {
            processUploadChunk(seqId, chunk);
        }
    } else if (env.type() == okidia::MessageType::FILE_TRANSFER_END) {
        processUploadEnd(seqId);
    }
}

void MulticastManager::processUploadBegin(uint64_t seqId, const QString& targetsCsv, const okidia::FileTransferBegin& begin, const QString& masterUserId) {
    QString listStr = targetsCsv;
    bool isAnnounce = false;
    int repeatCount = 1;

    if (listStr.startsWith("__multicast_anons__:")) {
        isAnnounce = true;
        listStr = listStr.mid(20);
        int qIdx = listStr.indexOf('?');
        if (qIdx != -1) {
            QString query = listStr.mid(qIdx + 1);
            listStr = listStr.left(qIdx);
            
            QStringList params = query.split("&", Qt::SkipEmptyParts);
            for (const QString& param : params) {
                if (param.startsWith("repeat=")) {
                    repeatCount = param.mid(7).toInt();
                    if (repeatCount < 1) repeatCount = 1;
                }
            }
        }
    } else if (listStr.startsWith("__multicast__:")) {
        listStr = listStr.mid(14);
    }
    
    QStringList targets = listStr.split(",", Qt::SkipEmptyParts);

    if (targets.isEmpty()) {
        Logger::warning(QObject::tr("MulticastManager: Hedef istemci listesi boş!"));
        return;
    }

    if (m_tasks.contains(seqId)) {
        deleteMulticastTask(seqId);
    }

    auto* task = new MulticastTask();
    task->sequenceId = seqId;
    task->filename = QString::fromStdString(begin.filename());
    task->fileSize = begin.file_size();
    task->readOnly = begin.read_only();
    task->isAnnounce = isAnnounce;
    task->repeatCount = repeatCount;
    task->tempFilePath = QDir::tempPath() + QString("/okidia_multicast_%1.tmp").arg(seqId);

    task->tempFile.setFileName(task->tempFilePath);
    if (!task->tempFile.open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("MulticastManager: Geçici dosya oluşturulamadı → ") + task->tempFilePath);
        delete task;
        return;
    }

    for (const QString& clientId : targets) {
        if (task->isAnnounce) {
            auto agentRec = OkidiaDatabase::instance().agent(clientId);
            if (agentRec.agentType == 2) {
                Logger::info(QObject::tr("MulticastManager: İdari bilgisayar anons yayınından hariç tutuldu → %1 (seqId=%2)")
                                .arg(clientId)
                                .arg(task->sequenceId));
                continue;
            }
        }

        if (m_registry->isAgentLockedByOther(clientId, masterUserId)) {
            Logger::warning(QObject::tr("MulticastManager: İstemci %1 başka bir master tarafından kilitlenmiş, dosya transferine eklenmiyor (seqId=%2)")
                            .arg(clientId)
                            .arg(task->sequenceId));
            continue;
        }
        auto* stream = new ClientStream();
        stream->clientId = clientId;
        stream->filename = task->filename;
        stream->totalSize = task->fileSize;
        stream->readOnly = task->readOnly;
        stream->sequenceId = seqId;
        stream->isAnnounce = task->isAnnounce;
        stream->repeatCount = task->repeatCount;
        stream->state = ClientStream::PENDING;
        task->clients[clientId] = stream;
    }

    m_tasks[seqId] = task;
    Logger::info(QObject::tr("MulticastManager: Yeni dosya yükleme başladı (seqId=%1) → %2 (%3 KB, %4 İstemci, isAnnounce=%5, repeat=%6)")
                 .arg(seqId)
                 .arg(task->filename)
                 .arg(task->fileSize / 1024)
                 .arg(targets.size())
                 .arg(task->isAnnounce ? "Evet" : "Hayır")
                 .arg(task->repeatCount));
}

void MulticastManager::processUploadChunk(uint64_t seqId, const okidia::FileTransferChunk& chunk) {
    auto* task = m_tasks.value(seqId, nullptr);
    if (!task || !task->tempFile.isOpen()) return;

    QByteArray data = QByteArray::fromStdString(chunk.data());
    task->tempFile.write(data);
}

void MulticastManager::processUploadEnd(uint64_t seqId) {
    auto* task = m_tasks.value(seqId, nullptr);
    if (!task) return;

    task->tempFile.close();
    task->uploadComplete = true;

    Logger::info(QObject::tr("MulticastManager: Dosya yükleme bitti (seqId=%1). Dağıtım başlıyor...").arg(seqId));

    // Her bir istemciye asenkron akışı başlat
    for (auto* stream : task->clients.values()) {
        startClientStream(task, stream);
    }
}

void MulticastManager::startClientStream(MulticastTask* task, ClientStream* stream) {
    if (stream->state == ClientStream::SENDING) return;

    if (!m_registry->hasClient(stream->clientId)) {
        if (stream->isAnnounce) {
            Logger::warning(QObject::tr("MulticastManager: Anons yayını için istemci çevrimdışı, pas geçiliyor → %1 (seqId=%2)")
                            .arg(stream->clientId)
                            .arg(task->sequenceId));
            stream->state = ClientStream::FAILED;
            // Görev tamamlanma/tetikleme durumunu kontrol et
            checkAnnouncePlaybackTrigger(task);
            checkTaskCompletion(task);
            return;
        }
        Logger::warning(QObject::tr("MulticastManager: İstemci çevrimdışı, beklemeye alınıyor → %1 (seqId=%2)")
                        .arg(stream->clientId)
                        .arg(task->sequenceId));
        stream->state = ClientStream::PENDING;
        return;
    }

    stream->file.setFileName(task->tempFilePath);
    if (!stream->file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("MulticastManager: İstemci için geçici dosya okunamadı → %1 (seqId=%2)")
                      .arg(stream->clientId)
                      .arg(task->sequenceId));
        stream->state = ClientStream::FAILED;
        checkAnnouncePlaybackTrigger(task);
        checkTaskCompletion(task);
        return;
    }

    stream->chunkIndex = 0;
    stream->state = ClientStream::SENDING;

    if (!stream->timer) {
        stream->timer = new QTimer(this);
        stream->timer->setInterval(10); // 10ms aralıklarla gönder (CPU ve ağı yormaz)
    } else {
        stream->timer->disconnect();
    }

    connect(stream->timer, &QTimer::timeout, this, [this, task, stream]() {
        // Bağlantı kesildi mi kontrol et
        if (!m_registry->hasClient(stream->clientId)) {
            if (stream->isAnnounce) {
                Logger::warning(QObject::tr("MulticastManager: Anons yayını sırasında istemci koptu! Pas geçiliyor → %1 (seqId=%2)")
                                .arg(stream->clientId)
                                .arg(task->sequenceId));
                stream->state = ClientStream::FAILED;
                stopClientStream(stream);
                checkAnnouncePlaybackTrigger(task);
                checkTaskCompletion(task);
                return;
            }

            Logger::warning(QObject::tr("MulticastManager: Dağıtım sırasında istemci koptu! → %1 (seqId=%2)")
                            .arg(stream->clientId)
                            .arg(task->sequenceId));
            stream->retryCount++;
            stopClientStream(stream);

            if (stream->retryCount < 3) {
                stream->state = ClientStream::PENDING;
                Logger::info(QObject::tr("MulticastManager: İstemci %1 için yeniden deneme hakkı kalan: %2/3")
                             .arg(stream->clientId)
                             .arg(3 - stream->retryCount));
            } else {
                stream->state = ClientStream::FAILED;
                Logger::error(QObject::tr("MulticastManager: İstemci %1 için maksimum deneme sınırına ulaşıldı (3/3)!")
                              .arg(stream->clientId));
                checkTaskCompletion(task);
            }
            return;
        }

        // İlk chunk ise BEGIN zarfı gönder
        if (stream->chunkIndex == 0) {
            okidia::FileTransferBegin begin;
            begin.set_filename(stream->filename.toStdString());
            begin.set_file_size(stream->totalSize);
            begin.set_read_only(stream->readOnly);
            if (stream->isAnnounce) {
                begin.set_destination("tmp");
            }

            okidia::Envelope env;
            env.set_type(okidia::MessageType::FILE_TRANSFER_BEGIN);
            env.set_sequence_id(stream->sequenceId);
            env.set_payload(begin.SerializeAsString());
            m_registry->forwardToClient(stream->clientId, env);
        }

        // Chunk oku
        QByteArray chunkData = stream->file.read(64 * 1024); // 64 KB
        bool isLast = stream->file.atEnd() || chunkData.isEmpty();

        if (!chunkData.isEmpty()) {
            okidia::FileTransferChunk chunk;
            chunk.set_chunk_index(stream->chunkIndex++);
            chunk.set_data(chunkData.toStdString());
            chunk.set_is_last(isLast);

            okidia::Envelope env;
            env.set_type(okidia::MessageType::FILE_TRANSFER_CHUNK);
            env.set_sequence_id(stream->sequenceId);
            env.set_payload(chunk.SerializeAsString());
            m_registry->forwardToClient(stream->clientId, env);
        }

        if (isLast) {
            Logger::info(QObject::tr("MulticastManager: Dağıtım tamamlandı → %1 (seqId=%2)")
                         .arg(stream->clientId)
                         .arg(task->sequenceId));
            
            okidia::Envelope env;
            env.set_type(okidia::MessageType::FILE_TRANSFER_END);
            env.set_sequence_id(stream->sequenceId);
            m_registry->forwardToClient(stream->clientId, env);

            stream->state = ClientStream::SUCCESS;
            stopClientStream(stream);

            if (stream->isAnnounce) {
                checkAnnouncePlaybackTrigger(task);
            }
            checkTaskCompletion(task);
        }
    });

    stream->timer->start();
}

void MulticastManager::stopClientStream(ClientStream* stream) {
    if (stream->timer) {
        stream->timer->stop();
    }
    if (stream->file.isOpen()) {
        stream->file.close();
    }
}

void MulticastManager::checkTaskCompletion(MulticastTask* task) {
    if (!task->uploadComplete) return;

    bool allFinished = true;
    for (auto* stream : task->clients.values()) {
        if (stream->state == ClientStream::PENDING || stream->state == ClientStream::SENDING) {
            allFinished = false;
            break;
        }
    }

    if (allFinished) {
        Logger::info(QObject::tr("MulticastManager: Multicast görevi bitti (seqId=%1). Temizlik yapılıyor...").arg(task->sequenceId));
        deleteMulticastTask(task->sequenceId);
    }
}

void MulticastManager::checkAnnouncePlaybackTrigger(MulticastTask* task) {
    if (!task->uploadComplete || !task->isAnnounce) return;

    bool anySendingOrPending = false;
    for (auto* stream : task->clients.values()) {
        if (stream->state == ClientStream::SENDING || stream->state == ClientStream::PENDING) {
            anySendingOrPending = true;
            break;
        }
    }

    if (anySendingOrPending) return;

    Logger::info(QObject::tr("MulticastManager: Anons için tüm aktif dağıtımlar bitti. Başarılı olanlara toplu oynatma tetikleniyor... (seqId=%1)")
                 .arg(task->sequenceId));

    for (auto* stream : task->clients.values()) {
        if (stream->state == ClientStream::SUCCESS) {
            okidia::AudioPlayLocalCommand playCmd;
            playCmd.set_anons_path(("/tmp/" + stream->filename).toStdString());
            playCmd.set_repeat_count(static_cast<int32_t>(stream->repeatCount));

            okidia::Envelope playEnv;
            playEnv.set_type(okidia::MessageType::AUDIO_PLAY_LOCAL);
            playEnv.set_sequence_id(stream->sequenceId);
            playEnv.set_payload(playCmd.SerializeAsString());
            
            m_registry->forwardToClient(stream->clientId, playEnv);
            Logger::info(QObject::tr("MulticastManager: Senkronize anons çalma komutu gönderildi → %1")
                         .arg(stream->clientId));
        }
    }
}

void MulticastManager::handleClientReconnect(const QString& clientId) {
    for (auto* task : m_tasks.values()) {
        if (!task->uploadComplete) continue;

        auto* stream = task->clients.value(clientId, nullptr);
        if (stream && stream->state == ClientStream::PENDING && stream->retryCount < 3) {
            Logger::info(QObject::tr("MulticastManager: İstemci %1 tekrar bağlandı! Multicast transferi yeniden başlatılıyor (Deneme %2/3)...")
                         .arg(clientId)
                         .arg(stream->retryCount + 1));
            startClientStream(task, stream);
        }
    }
}

void MulticastManager::deleteMulticastTask(uint64_t seqId) {
    auto* task = m_tasks.take(seqId);
    if (!task) return;

    if (task->tempFile.isOpen()) {
        task->tempFile.close();
    }
    QFile::remove(task->tempFilePath);

    for (auto* stream : task->clients.values()) {
        stopClientStream(stream);
        delete stream;
    }
    delete task;
}
