// master/audio/AudioBroadcaster.cpp
#include "AudioBroadcaster.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"

#include <QFile>
#include <QFileInfo>

AudioBroadcaster::AudioBroadcaster(QObject* parent)
    : QObject(parent) {}

bool AudioBroadcaster::sendAudioFile(const QString& filePath,
                                      SessionManager* session) {
    if (!session || !session->isConnected()) {
        emit finished(false, "Bağlantı yok");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::error(QObject::tr("AudioBroadcaster: Dosya açılamadı → ") + filePath);
        emit finished(false, "Dosya açılamadı");
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    if (data.isEmpty()) {
        emit finished(false, "Dosya boş");
        return false;
    }

    QFileInfo info(filePath);
    QString   ext = info.suffix().toLower();

    Logger::info(QObject::tr("AudioBroadcaster: Ses gönderiliyor → ") +
                 info.fileName() +
                 " (" + QString::number(data.size() / 1024) + QObject::tr(" KB)"));

    okidia::AudioBroadcastCommand cmd;
    cmd.set_source(okidia::AudioBroadcastCommand::FILE);
    cmd.set_audio_data(data.toStdString());
    cmd.set_format(ext.toStdString());
    cmd.set_volume(0);  // 0 = mevcut ses seviyesini değiştirme

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_BROADCAST_FILE);
    env.set_sequence_id(0);  // broadcast
    env.set_payload(cmd.SerializeAsString());
    session->broadcastCommand(env);

    emit finished(true, info.fileName());
    return true;
}

void AudioBroadcaster::stopBroadcast(SessionManager* session) {
    if (!session) return;

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_BROADCAST_STOP);
    env.set_sequence_id(0);
    session->broadcastCommand(env);

    Logger::info(QObject::tr("AudioBroadcaster: Ses yayını durduruldu"));
}
