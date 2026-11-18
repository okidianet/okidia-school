// master/session/SessionManagerAudio.cpp
// Ses kaydı, anons işleme/temizleme, ses yayın ve ses komutları metodlarını içerir.
// startRecording, stopRecording, cleanRecordedWav, sendAnons,
// broadcastAudioFile, broadcastAudioStop, broadcastAudioMute, broadcastAudioUnmute.

#include "SessionManager.h"
#include "../common/utils/Logger.h"
#include "../common/db/OkidiaDatabase.h"
#include "../transfer/FileSender.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QProcess>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <QDataStream>
#include <QAudioSource>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>

// ---------------------------------------------------------------------------
// WAV Temizleme Yardımcısı: Donanım başlatma pikini budar ve fade-in/out ekler
// ---------------------------------------------------------------------------
static void cleanRecordedWav(const QString& wavPath) {
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadWrite)) {
        return;
    }

    QByteArray content = file.readAll();
    if (content.size() < 44) return;

    if (!content.startsWith("RIFF") || content.mid(8, 4) != "WAVE") {
        return;
    }

    // RIFF chunk'larını parse et
    int idx = 12;
    int fmtOffset = 0;
    int dataOffset = 0;
    quint32 dataSize = 0;
    quint16 audioFormat = 0;
    quint16 numChannels = 0;
    quint32 sampleRate = 0;
    quint16 blockAlign = 0;
    quint16 bitsPerSample = 0;

    while (idx + 8 <= content.size()) {
        QByteArray chunkId = content.mid(idx, 4);
        quint32 chunkSz = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(content.constData() + idx + 4));

        if (chunkId == "fmt " && chunkSz >= 16) {
            fmtOffset = idx + 8;
            audioFormat = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset));
            numChannels = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 2));
            sampleRate = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 4));
            blockAlign = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 12));
            bitsPerSample = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 14));
        } else if (chunkId == "data") {
            dataOffset = idx + 8;
            dataSize = chunkSz;
            break;
        }

        idx += 8 + chunkSz;
    }

    // Sadece standart 16-bit PCM WAV dosyalarını işle
    if (dataOffset <= 0 || audioFormat != 1 || bitsPerSample != 16 || blockAlign == 0 || numChannels == 0 || sampleRate == 0) {
        return;
    }

    // Donanım başlatma pikini (turn-on transient / click) yok etmek için ilk 100ms'yi atla
    int skipFrames = static_cast<int>(sampleRate * 0.10); // 100 ms
    int skipBytes = skipFrames * blockAlign;

    // Minimum dosya boyutu kontrolü (en az 250ms olmalı)
    if (static_cast<int>(dataSize) <= skipBytes + static_cast<int>(sampleRate * 0.15) * blockAlign) {
        return;
    }

    int newDataSize = dataSize - skipBytes;
    char* dataPtr = content.data() + dataOffset;

    // Veriyi başa kaydır (ilk 100ms atlanır)
    memmove(dataPtr, dataPtr + skipBytes, newDataSize);

    // İlk 30ms için yumuşak fade-in (0'dan başlayarak yumuşak yükselme)
    int fadeInFrames = static_cast<int>(sampleRate * 0.030);
    qint16* samples = reinterpret_cast<qint16*>(dataPtr);
    for (int f = 0; f < fadeInFrames; ++f) {
        float gain = static_cast<float>(f) / static_cast<float>(fadeInFrames);
        for (int c = 0; c < numChannels; ++c) {
            int sIdx = f * numChannels + c;
            qint16 val = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[sIdx]));
            qint16 cleanVal = static_cast<qint16>(val * gain);
            qToLittleEndian<qint16>(cleanVal, reinterpret_cast<uchar*>(&samples[sIdx]));
        }
    }

    // Son 30ms için yumuşak fade-out (kayıt durduğundaki tık sesini engeller)
    int fadeOutFrames = static_cast<int>(sampleRate * 0.030);
    int totalFrames = newDataSize / blockAlign;
    for (int f = 0; f < fadeOutFrames; ++f) {
        float gain = static_cast<float>(f) / static_cast<float>(fadeOutFrames);
        int targetFrame = totalFrames - 1 - f;
        for (int c = 0; c < numChannels; ++c) {
            int sIdx = targetFrame * numChannels + c;
            qint16 val = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[sIdx]));
            qint16 cleanVal = static_cast<qint16>(val * gain);
            qToLittleEndian<qint16>(cleanVal, reinterpret_cast<uchar*>(&samples[sIdx]));
        }
    }

    // WAV başlığındaki boyutları güncelle
    // 1. data chunk boyutu
    qToLittleEndian<quint32>(static_cast<quint32>(newDataSize), reinterpret_cast<uchar*>(content.data() + dataOffset - 4));
    // 2. RIFF boyutu
    quint32 riffSize = (dataOffset - 8) + newDataSize;
    qToLittleEndian<quint32>(riffSize, reinterpret_cast<uchar*>(content.data() + 4));

    // Dosyayı yeni boyutuyla diske yaz
    file.seek(0);
    file.write(content.constData(), dataOffset + newDataSize);
    file.resize(dataOffset + newDataSize);
    file.close();

    Logger::info(QObject::tr("SessionManager: Ses kaydı temizlendi (100ms donanım piki budandı, 30ms fade-in/out uygulandı)."));
}

// ---------------------------------------------------------------------------
// startRecording
// ---------------------------------------------------------------------------
bool SessionManager::startRecording(const QAudioDevice& device) {
    if (m_audioSource) return false;

    {
        QTemporaryFile tmp(QDir::tempPath() + "/okidia_anons_XXXXXX.wav");
        tmp.setAutoRemove(false);
        if (!tmp.open()) return false;
        m_audioWavPath = tmp.fileName();
    }

    QAudioDevice targetDev = device;
    if (targetDev.isNull()) {
        targetDev = QMediaDevices::defaultAudioInput();
    }

    if (targetDev.isNull()) {
        Logger::error(QObject::tr("SessionManager: Kullanılabilir ses giriş aygıtı (mikrofon) bulunamadı!"));
        return false;
    }

    // Standart Ses Formatı: 44.1 kHz, 16-bit PCM, Mono
    QAudioFormat format;
    format.setSampleRate(44100);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!targetDev.isFormatSupported(format)) {
        format = targetDev.preferredFormat();
        format.setSampleFormat(QAudioFormat::Int16);
        if (format.channelCount() > 2) format.setChannelCount(2);
        if (format.sampleRate() <= 0)  format.setSampleRate(44100);
    }
    m_recordFormat = format;

    m_audioRecordFile = new QFile(m_audioWavPath, this);
    if (!m_audioRecordFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::error(QObject::tr("SessionManager: Ses dosyası açılamadı: ") + m_audioWavPath);
        delete m_audioRecordFile;
        m_audioRecordFile = nullptr;
        return false;
    }

    // 44 baytlık WAV başlığı yerini boşlukla rezerve et
    const QByteArray dummyHeader(44, 0);
    m_audioRecordFile->write(dummyHeader);

    m_audioSource = new QAudioSource(targetDev, m_recordFormat, this);
    m_audioSource->start(m_audioRecordFile);

    if (m_audioSource->error() != QAudio::NoError) {
        Logger::error(QObject::tr("SessionManager: Mikrofon kaydı başlatılamadı: %1")
                     .arg(static_cast<int>(m_audioSource->error())));
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_audioRecordFile->close();
        delete m_audioRecordFile;
        m_audioRecordFile = nullptr;
        return false;
    }

    Logger::info(QObject::tr("SessionManager: Kayıt başladı (Saf Qt6 QAudioSource) → Aygıt: %1 | Format: %2Hz %3ch | Dosya: %4")
                 .arg(targetDev.description())
                 .arg(m_recordFormat.sampleRate())
                 .arg(m_recordFormat.channelCount())
                 .arg(m_audioWavPath));
    return true;
}

// ---------------------------------------------------------------------------
// stopRecording
// ---------------------------------------------------------------------------
QString SessionManager::stopRecording() {
    if (!m_audioSource || !m_audioRecordFile) return {};

    m_audioSource->stop();
    delete m_audioSource;
    m_audioSource = nullptr;

    // Dosyayı flush et ve standart WAV (RIFF) başlığını yaz
    m_audioRecordFile->flush();
    const qint64 totalSize = m_audioRecordFile->size();
    const quint32 dataSize = (totalSize > 44) ? static_cast<quint32>(totalSize - 44) : 0;

    const quint32 sampleRate = static_cast<quint32>(m_recordFormat.sampleRate());
    const quint16 channels   = static_cast<quint16>(m_recordFormat.channelCount());
    const quint16 bitsPerSample = 16;
    const quint16 blockAlign = channels * (bitsPerSample / 8);
    const quint32 byteRate   = sampleRate * blockAlign;

    m_audioRecordFile->seek(0);
    QDataStream out(m_audioRecordFile);
    out.setByteOrder(QDataStream::LittleEndian);

    // RIFF chunk
    m_audioRecordFile->write("RIFF", 4);
    out << quint32(dataSize + 36);
    m_audioRecordFile->write("WAVE", 4);

    // fmt chunk
    m_audioRecordFile->write("fmt ", 4);
    out << quint32(16);             // Subchunk1Size (PCM = 16)
    out << quint16(1);              // AudioFormat (PCM = 1)
    out << quint16(channels);
    out << quint32(sampleRate);
    out << quint32(byteRate);
    out << quint16(blockAlign);
    out << quint16(bitsPerSample);

    // data chunk
    m_audioRecordFile->write("data", 4);
    out << quint32(dataSize);

    m_audioRecordFile->close();
    delete m_audioRecordFile;
    m_audioRecordFile = nullptr;

    // Kayıt başındaki donanım başlatma çıtırtısını ve buton tıklama sesini temizle
    cleanRecordedWav(m_audioWavPath);

    qint64 sz = QFileInfo(m_audioWavPath).size();
    Logger::info(QObject::tr("SessionManager: Kayıt tamamlandı → %1 (%2 KB)")
                 .arg(m_audioWavPath)
                 .arg(sz / 1024));
    return m_audioWavPath;
}

// ---------------------------------------------------------------------------
// Anons — file transfer → /tmp → AUDIO_PLAY_LOCAL
// ---------------------------------------------------------------------------
void SessionManager::sendAnons(const QString& wavPath,
                                const QStringList& targets,
                                int repeatCount) {
    if (targets.isEmpty()) return;

    if (!m_conn || !m_conn->isConnected()) {
        Logger::error(QObject::tr("SessionManager::sendAnons: Broker bağlantısı yok!"));
        return;
    }

    // İdari Bilgisayarları (Tip 2) anonstan tamamen soyutla
    QStringList filteredTargets;
    for (const QString& id : targets) {
        if (OkidiaDatabase::instance().isOpen()) {
            auto rec = OkidiaDatabase::instance().agent(id);
            if (rec.agentType == 2) {
                continue;
            }
        }
        filteredTargets << id;
    }

    if (filteredTargets.isEmpty()) {
        Logger::info(QObject::tr("SessionManager::sendAnons: Hedef listede anons yapılacak istemci kalmadı (İdari bilgisayarlar filtrelendi)."));
        return;
    }

    // Multicast anons hedef formatı: __multicast_anons__:id1,id2,id3...?repeat=X
    QString multicastTarget = QString("__multicast_anons__:%1?repeat=%2")
                              .arg(filteredTargets.join(","))
                              .arg(repeatCount);

    FileSender* sender = new FileSender(m_conn, this);
    connect(sender, &FileSender::finished, this, [sender](bool ok, const QString& msg) {
        if (ok) {
            Logger::info(QObject::tr("SessionManager::sendAnons: Asenkron multicast anons yüklemesi tamamlandı → ") + msg);
        } else {
            Logger::error(QObject::tr("SessionManager::sendAnons: Asenkron multicast anons yüklemesi başarısız → ") + msg);
        }
        sender->deleteLater();
    });

    if (!sender->sendToClient(multicastTarget, wavPath, false, "tmp")) {
        Logger::error(QObject::tr("SessionManager::sendAnons: Transfer başlatılamadı!"));
        sender->deleteLater();
    }
}

// ---------------------------------------------------------------------------
// broadcastAudioFile
// ---------------------------------------------------------------------------
bool SessionManager::broadcastAudioFile(const QString& filePath,
                                        const QStringList& targets,
                                        int repeatCount) {
    QStringList finalTargets = targets.isEmpty() ? m_clientStates.keys() : targets;
    if (finalTargets.isEmpty()) {
        Logger::warning(QObject::tr("SessionManager::broadcastAudioFile: Çevrimiçi hedef bulunamadı!"));
        return false;
    }

    sendAnons(filePath, finalTargets, repeatCount);
    return true;
}

// ---------------------------------------------------------------------------
// broadcastAudioStop
// ---------------------------------------------------------------------------
bool SessionManager::broadcastAudioStop() {
    stopRecording();
    stopLiveAudioBroadcast();
    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_BROADCAST_STOP);
    env.set_sequence_id(0);
    return broadcastCommand(env);
}

// ---------------------------------------------------------------------------
// Canlı Ses Yayını (Live Audio Streaming / Push-to-Talk)
// ---------------------------------------------------------------------------
bool SessionManager::startLiveAudioBroadcast(const QStringList& targets, const QAudioDevice& device) {
    if (m_liveAudioSource) return false;

    QAudioDevice targetDev = device;
    if (targetDev.isNull()) {
        targetDev = QMediaDevices::defaultAudioInput();
    }
    if (targetDev.isNull()) {
        Logger::error(QObject::tr("SessionManager: Canlı ses için mikrofon bulunamadı!"));
        return false;
    }

    // Konuşma kalitesi için 22050 Hz Mono 16-bit PCM (Ağ yükünü ~44 KB/s'ye optimize eder)
    QAudioFormat format;
    format.setSampleRate(22050);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!targetDev.isFormatSupported(format)) {
        format = targetDev.preferredFormat();
        format.setSampleFormat(QAudioFormat::Int16);
        if (format.channelCount() > 2) format.setChannelCount(2);
        if (format.sampleRate() <= 0)  format.setSampleRate(22050);
    }
    m_liveFormat = format;
    m_liveBroadcastTargets = targets;

    m_liveAudioSource = new QAudioSource(targetDev, m_liveFormat, this);
    // Küçük tampon: gecikmeyi en aza indirmek için (100 ms)
    const int bufferBytes = m_liveFormat.bytesForDuration(100000);
    if (bufferBytes > 0) m_liveAudioSource->setBufferSize(bufferBytes);

    m_liveIoDevice = m_liveAudioSource->start();
    if (!m_liveIoDevice) {
        Logger::error(QObject::tr("SessionManager: Canlı ses aygıtı başlatılamadı"));
        delete m_liveAudioSource;
        m_liveAudioSource = nullptr;
        return false;
    }

    connect(m_liveIoDevice, &QIODevice::readyRead, this, [this]() {
        if (!m_liveIoDevice || !m_liveAudioSource) return;
        const QByteArray pcm = m_liveIoDevice->readAll();
        if (pcm.isEmpty()) return;

        okidia::AudioBroadcastCommand cmd;
        cmd.set_source(okidia::AudioBroadcastCommand::LIVE);
        cmd.set_audio_data(pcm.constData(), pcm.size());
        cmd.set_sample_rate(static_cast<uint32_t>(m_liveFormat.sampleRate()));
        cmd.set_channels(static_cast<uint32_t>(m_liveFormat.channelCount()));
        cmd.set_volume(0);

        okidia::Envelope env;
        env.set_type(okidia::MessageType::AUDIO_BROADCAST_LIVE);
        env.set_payload(cmd.SerializeAsString());

        if (m_liveBroadcastTargets.isEmpty()) {
            broadcastCommand(env);
        } else {
            for (const auto& targetId : m_liveBroadcastTargets) {
                sendCommand(targetId, env);
            }
        }
    });

    Logger::info(QObject::tr("SessionManager: Canlı anons yayını başladı → Aygıt: %1 | %2Hz %3ch | Hedef: %4")
                 .arg(targetDev.description())
                 .arg(m_liveFormat.sampleRate())
                 .arg(m_liveFormat.channelCount())
                 .arg(targets.isEmpty() ? tr("Tüm İstemciler") : QString::number(targets.size()) + tr(" istemci")));
    return true;
}

void SessionManager::stopLiveAudioBroadcast() {
    if (!m_liveAudioSource) return;

    m_liveAudioSource->stop();
    delete m_liveAudioSource;
    m_liveAudioSource = nullptr;
    m_liveIoDevice = nullptr;

    // Hedeflere durdurma paketi gönder
    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_BROADCAST_STOP);

    if (m_liveBroadcastTargets.isEmpty()) {
        broadcastCommand(env);
    } else {
        for (const auto& targetId : m_liveBroadcastTargets) {
            sendCommand(targetId, env);
        }
    }
    m_liveBroadcastTargets.clear();

    Logger::info(QObject::tr("SessionManager: Canlı anons yayını durduruldu"));
}

// ---------------------------------------------------------------------------
// broadcastAudioMute / broadcastAudioUnmute
// ---------------------------------------------------------------------------
bool SessionManager::broadcastAudioMute() {
    return broadcastCommand(makeEnv(okidia::MessageType::AUDIO_MUTE));
}

bool SessionManager::broadcastAudioUnmute() {
    return broadcastCommand(makeEnv(okidia::MessageType::AUDIO_UNMUTE));
}
