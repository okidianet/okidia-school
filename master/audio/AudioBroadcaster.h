// master/audio/AudioBroadcaster.h
#pragma once

#include <QObject>
#include "../../common/network/Connection.h"

// Forward declaration — tam include döngüsel bağımlılık yaratır
class SessionManager;

/**
 * Ses yayını — master tarafı.
 *
 * sendAudioFile(path) → Dosyayı okuyup AUDIO_BROADCAST_FILE gönderir.
 * stopBroadcast()     → AUDIO_BROADCAST_STOP gönderir.
 *
 * Canlı mikrofon yayını (AUDIO_BROADCAST_LIVE) Faz 6 sonrasına bırakıldı;
 * QAudioSource gerçek zamanlı PCM akışı gerektiriyor — ayrı bir thread.
 */
class AudioBroadcaster : public QObject {
    Q_OBJECT

public:
    explicit AudioBroadcaster(QObject* parent = nullptr);

    // Tüm istemcilere dosya gönder
    bool sendAudioFile(const QString& filePath,
                       SessionManager* session);

    // Yayını durdur
    void stopBroadcast(SessionManager* session);

signals:
    void finished(bool success, const QString& filename);
};
