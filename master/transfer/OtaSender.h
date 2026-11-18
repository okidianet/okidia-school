// master/transfer/OtaSender.h
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QFile>
#include <QTimer>
#include "../../common/network/Connection.h"

class OtaSender : public QObject {
    Q_OBJECT
public:
    explicit OtaSender(Connection* conn, const QString& userId, QObject* parent = nullptr);
    ~OtaSender();

    bool sendPackages(const QStringList& filePaths);
    bool sendBrokerSelfUpdate(const QString& filePath);

signals:
    void progress(int currentFileIdx, int totalFiles, qint64 bytesSent, qint64 totalBytes);
    void finished(bool success, const QString& errorMsg);

private slots:
    void sendNextChunk();

private:
    void startNextFile();
    
    Connection* m_conn;
    QStringList m_queue;
    int m_totalFiles;
    int m_currentIdx;
    QString m_userId;
    bool m_isBrokerSelfUpdate = false;

    QFile m_file;
    uint64_t m_fileSize;
    uint32_t m_chunkIndex;
    uint64_t m_bytesSent;
    QTimer* m_timer;
};
