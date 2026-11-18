// master/ssh/DeployWorker.h
#pragma once

#include <QObject>
#include <QRunnable>
#include <QString>
#include "SshClient.h"

class DeployWorker : public QObject, public QRunnable {
    Q_OBJECT

public:
    enum class Step {
        Pending = 0,
        Connecting = 1,
        Uploading = 2,
        Installing = 3,
        Verifying = 4,
        Completed = 5,
        Failed = 6,
        QueuedOffline = 7
    };

    struct TargetInfo {
        QString ip;
        QString hostname;
        QString osInfo;
        QString pkgPath;
        SshClient::Config sshConfig;
        QString brokerIp;
        int brokerPort = 11200;
        QStringList brokerCandidateIps;
    };

    explicit DeployWorker(const TargetInfo& target, QObject* parent = nullptr);
    ~DeployWorker() override = default;

    void run() override;

    const TargetInfo& target() const { return m_target; }

signals:
    void stepChanged(const QString& ip, int step, const QString& statusText);
    void uploadProgress(const QString& ip, qint64 bytesSent, qint64 bytesTotal, int percent);
    void logMessage(const QString& ip, const QString& msg);
    void finished(const QString& ip, bool success, const QString& versionOrError, const QString& fullLog);

private:
    TargetInfo m_target;
};
