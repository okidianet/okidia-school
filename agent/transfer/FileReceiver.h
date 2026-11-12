// agent/transfer/FileReceiver.h
#pragma once

#include <QObject>
#include <QFile>
#include "../../common/network/Connection.h"

#include <QMap>

/**
 * Master'dan gelen dosya transferini alır ve diske yazar.
 * Protokol: FILE_TRANSFER_BEGIN → N × FILE_TRANSFER_CHUNK → FILE_TRANSFER_END
 * Hedef dizin: ~/Masaüstü/OKIDIA_Dosyalar/ (yoksa ~/OKIDIA_Dosyalar/)
 */
class FileReceiver : public QObject {
    Q_OBJECT

public:
    explicit FileReceiver(QObject* parent = nullptr);

    void handleEnvelope(const okidia::Envelope& envelope);

signals:
    void fileReceived(const QString& filePath, qint64 size);
    void receiveError(const QString& reason);
    void otaUpdateReady(const QString& path);

private:
    // Devam eden transfer
    struct Transfer {
        uint64_t sequenceId = 0;
        QString  filename;
        quint64  expectedSize = 0;
        quint64  receivedSize = 0;
        QFile    file;
        bool     readOnly    = false;
        bool     destIsTmp   = false;  // true → /tmp, chown/chmod yapma
        bool     destIsAsset = false;  // true → assetsDir, ortak okuma izinleri
        bool     isDirectTransfer = true; // true → doğrudan kullanıcı masaüstü transferi (açma diyaloğu çıkartılır)
        QString  finalPath;
        bool     isSkipped   = false;  // true → dosya zaten mevcut ve güncel, indirme pas geçildi
    };

    QMap<uint64_t, Transfer*> m_transfers;

    void handleBegin(uint64_t seqId, const okidia::FileTransferBegin& msg);
    void handleChunk(uint64_t seqId, const okidia::FileTransferChunk& msg);
    void handleEnd(uint64_t seqId);

    QString resolveTargetDir() const;
};
