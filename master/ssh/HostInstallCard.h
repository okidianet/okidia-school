// master/ssh/HostInstallCard.h
#pragma once

#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include "DeployWorker.h"

class HostInstallCard : public QFrame {
    Q_OBJECT

public:
    explicit HostInstallCard(const QString& ip,
                            const QString& hostname,
                            const QString& osInfo,
                            const QString& pkgPath,
                            QWidget* parent = nullptr);
    ~HostInstallCard() override = default;

    QString ip() const { return m_ip; }
    QString hostname() const { return m_hostname; }
    QString osInfo() const { return m_osInfo; }
    QString pkgPath() const { return m_pkgPath; }
    QString machineId() const { return m_machineId; }
    void setMachineId(const QString& machineId) { m_machineId = machineId; }
    QString targetVersion() const { return m_targetVersion; }
    void setTargetVersion(const QString& ver) { m_targetVersion = ver; }
    QString statusText() const { return m_statusLabel ? m_statusLabel->text() : QString(); }

    DeployWorker::Step currentStep() const { return m_step; }
    bool isFinished() const { return m_step == DeployWorker::Step::Completed || m_step == DeployWorker::Step::Failed; }
    bool isSuccess() const { return m_step == DeployWorker::Step::Completed; }
    bool isFailed() const { return m_step == DeployWorker::Step::Failed; }
    bool isQueuedOffline() const { return m_step == DeployWorker::Step::QueuedOffline; }
    bool matchesTarget(const QString& target) const;

    QJsonObject toJson() const;
    static HostInstallCard* fromJson(const QJsonObject& obj, QWidget* parent = nullptr);

signals:
    void stateUpdated();
    void remoteLogRequested(const QString& machineId, const QString& ip, const QString& hostname);

public slots:
    void onStepChanged(const QString& ip, int step, const QString& statusText);
    void onUploadProgress(const QString& ip, qint64 bytesSent, qint64 bytesTotal, int percent);
    void onLogMessage(const QString& ip, const QString& msg);
    void onFinished(const QString& ip, bool success, const QString& versionOrError, const QString& fullLog);
    void showLogDialog();

private:
    void setupUi();
    void updateCardStyle();

    QString m_ip;
    QString m_hostname;
    QString m_osInfo;
    QString m_pkgPath;
    QString m_machineId;
    QString m_targetVersion;
    DeployWorker::Step m_step = DeployWorker::Step::Pending;

    QLabel* m_iconLabel = nullptr;
    QLabel* m_hostnameLabel = nullptr;
    QLabel* m_ipLabel = nullptr;
    QLabel* m_osLabel = nullptr;

    QLabel* m_statusIconLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QPushButton* m_logBtn = nullptr;

    QStringList m_logs;
    QString m_fullLog;
};
