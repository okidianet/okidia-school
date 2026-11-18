// master/transfer/MasterUpdater.cpp
#include "MasterUpdater.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/PlatformService.h"
#include <QCoreApplication>
#include <QProcess>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>

MasterUpdater::MasterUpdater(SessionManager* session, QObject* parent)
    : QObject(parent), m_session(session) {}

MasterUpdater::~MasterUpdater() {
    if (m_file.isOpen()) {
        m_file.close();
    }
}

bool MasterUpdater::handleEnvelope(const okidia::Envelope& envelope) {
    if (envelope.type() == okidia::MessageType::FILE_TRANSFER_BEGIN) {
        okidia::FileTransferBegin msg;
        if (msg.ParseFromString(envelope.payload())) {
            QString fname = QString::fromStdString(msg.filename());
            if (fname.startsWith("okidia-master")) {
                handleBegin(envelope.sequence_id(), msg);
                return true;
            }
        }
    } else if (envelope.type() == okidia::MessageType::FILE_TRANSFER_CHUNK) {
        if (m_isDownloading && envelope.sequence_id() == m_currentSeqId) {
            okidia::FileTransferChunk msg;
            if (msg.ParseFromString(envelope.payload())) {
                handleChunk(envelope.sequence_id(), msg);
                return true;
            }
        }
    } else if (envelope.type() == okidia::MessageType::FILE_TRANSFER_END) {
        if (m_isDownloading && envelope.sequence_id() == m_currentSeqId) {
            handleEnd(envelope.sequence_id());
            return true;
        }
    }
    return false;
}

void MasterUpdater::handleBegin(uint64_t seqId, const okidia::FileTransferBegin& msg) {
    if (m_file.isOpen()) {
        m_file.close();
    }

    m_currentSeqId = seqId;
    m_expectedSize = msg.file_size();
    m_receivedSize = 0;
#ifdef Q_OS_WIN
    QString fname = QString::fromStdString(msg.filename());
    if (fname.isEmpty()) fname = "okidia-master-setup.exe";
    m_targetFilePath = QDir::tempPath() + "/" + fname;
#else
    QString fname = QString::fromStdString(msg.filename());
    if (fname.isEmpty()) fname = QString("okidia-master-ota-%1.deb").arg(QCoreApplication::applicationPid());
    m_targetFilePath = QDir::tempPath() + "/" + fname;
#endif

    if (QFile::exists(m_targetFilePath)) {
        QFile::remove(m_targetFilePath);
    }

    m_file.setFileName(m_targetFilePath);
    if (!m_file.open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("MasterUpdater: Hedef dosya oluşturulamadı: ") + m_targetFilePath);
        m_isDownloading = false;
        emit downloadFinished(false, tr("Dosya diske yazılamadı: ") + m_targetFilePath);
        return;
    }

    m_isDownloading = true;
    Logger::info(QObject::tr("MasterUpdater: Master güncelleme paketi indiriliyor (") + QString::fromStdString(msg.filename()) + QObject::tr("), boyut: ") + QString::number(m_expectedSize));
    emit downloadStarted(QString::fromStdString(msg.filename()), m_expectedSize);
}

void MasterUpdater::handleChunk(uint64_t seqId, const okidia::FileTransferChunk& msg) {
    Q_UNUSED(seqId);
    if (!m_file.isOpen()) return;

    QByteArray data(msg.data().data(), msg.data().size());
    qint64 written = m_file.write(data);
    if (written > 0) {
        m_receivedSize += written;
        int pct = m_expectedSize > 0 ? static_cast<int>((m_receivedSize * 100) / m_expectedSize) : 0;
        emit downloadProgress(m_receivedSize, m_expectedSize, pct);
    }
}

void MasterUpdater::handleEnd(uint64_t seqId) {
    Q_UNUSED(seqId);
    if (m_file.isOpen()) {
        m_file.close();
    }

    m_isDownloading = false;
    Logger::info(QObject::tr("MasterUpdater: Master güncelleme paketi başarıyla indirildi -> ") + m_targetFilePath);
    emit downloadFinished(true, m_targetFilePath);
}

void MasterUpdater::installAndRestart(const QString& packageFilePath, QWidget* parentWidget) {
    if (packageFilePath.isEmpty() || !QFile::exists(packageFilePath)) {
        if (parentWidget) {
            QMessageBox::warning(parentWidget, tr("Hata"), tr("Kurulacak güncelleme paketi bulunamadı!"));
        }
        return;
    }

#ifdef Q_OS_WIN
    Logger::info(QObject::tr("MasterUpdater: Windows üzerinde UAC yönetici yetkisi ile güncelleme kurulumu başlatılıyor: ") + packageFilePath);
    const QString appExePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString setupFileName = QFileInfo(packageFilePath).completeBaseName();
    bool started = PlatformService::executeElevated(packageFilePath, QStringList() << "/SILENT" << "/SP-", parentWidget);
    if (started) {
        // Kurulum tamamlandıktan sonra (UAC farklı token altında çalışsa dahi) Master'ın
        // mevcut kullanıcı oturumunda otomatik olarak yeniden başlamasını garanti altına al.
        QString psCmd = QString(
            "Start-Sleep -Seconds 2; "
            "$p = Get-Process -Name '%1*' -ErrorAction SilentlyContinue; "
            "if ($p) { $p | Wait-Process -Timeout 120 -ErrorAction SilentlyContinue }; "
            "Start-Sleep -Seconds 2; "
            "if (-not (Get-Process -Name 'okidia-master' -ErrorAction SilentlyContinue)) { "
            "  if (Test-Path '%2') { Start-Process -FilePath '%2' } "
            "  elseif (Test-Path 'C:\\Program Files\\Okidia\\Master\\bin\\okidia-master.exe') { "
            "    Start-Process -FilePath 'C:\\Program Files\\Okidia\\Master\\bin\\okidia-master.exe' "
            "  } "
            "}"
        ).arg(setupFileName, appExePath);
        QProcess::startDetached("powershell.exe", QStringList()
            << "-NoProfile" << "-NonInteractive" << "-WindowStyle" << "Hidden"
            << "-Command" << psCmd);

        Logger::info(QObject::tr("MasterUpdater: Kurulum süreci başlatıldı, uygulama kapatılıyor..."));
        QCoreApplication::quit();
    } else {
        Logger::error(QObject::tr("MasterUpdater: Güncelleme yükleyicisi başlatılamadı: ") + packageFilePath);
    }
#else
    // Master güncellenirken prerm betiği süreci sonlandırır.
    // Dpkg'nin çağrıcı süreçle birlikte yarıda kesilmesini önlemek için:
    // 1) Mevcut Master'ın düzgünce kapanması için 1.5 sn beklenir
    // 2) Kalan kopya varsa pkill ile temizlenir
    // 3) dpkg -i ve apt-get install -f -y çalıştırılır
    // 4) Başarılı olursa yeni Master oturumu açılır
    QString scriptPath = QDir::tempPath() + QString("/okidia_master_install_%1.sh").arg(QCoreApplication::applicationPid());
    QString runnerPath = QDir::tempPath() + QString("/okidia_master_runner_%1.sh").arg(QCoreApplication::applicationPid());

    const QString curUser = QString::fromLocal8Bit(qgetenv("USER")).trimmed();
    const QString curDisplay = QString::fromLocal8Bit(qgetenv("DISPLAY")).trimmed();
    const QString curXauth = QString::fromLocal8Bit(qgetenv("XAUTHORITY")).trimmed();
    const QString curWayland = QString::fromLocal8Bit(qgetenv("WAYLAND_DISPLAY")).trimmed();
    const QString curXdgRuntime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR")).trimmed();
    const QString curDbus = QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")).trimmed();

    QFile scriptFile(scriptPath);
    if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&scriptFile);
        out << "#!/bin/bash\n";
        out << "export DEBIAN_FRONTEND=noninteractive\n";
        out << "sleep 1.5\n";
        out << "pkill -9 -x okidia-master 2>/dev/null || true\n";
        out << "dpkg -i \"" << packageFilePath << "\" >/tmp/okidia_ota_dpkg.log 2>&1\n";
        out << "DPKG_STATUS=$?\n";
        out << "if [ $DPKG_STATUS -ne 0 ]; then\n";
        out << "    apt-get install -f -y >>/tmp/okidia_ota_dpkg.log 2>&1\n";
        out << "    dpkg -i \"" << packageFilePath << "\" >>/tmp/okidia_ota_dpkg.log 2>&1\n";
        out << "fi\n";
        out << "REAL_USER=\"" << curUser << "\"\n";
        out << "if [ -z \"$REAL_USER\" ] || [ \"$REAL_USER\" = \"root\" ]; then\n";
        out << "    REAL_USER=$(who | awk '/:0/{print $1; exit}')\n";
        out << "fi\n";
        out << "if [ -z \"$REAL_USER\" ] || [ \"$REAL_USER\" = \"root\" ]; then\n";
        out << "    REAL_USER=$(getent passwd | awk -F: '$3 >= 1000 && $3 < 65534 {print $1; exit}')\n";
        out << "fi\n";
        out << "DISP_VAL=\"" << (curDisplay.isEmpty() ? QStringLiteral(":0") : curDisplay) << "\"\n";
        out << "XAUTH_VAL=\"" << curXauth << "\"\n";
        out << "if [ -z \"$XAUTH_VAL\" ] && [ -n \"$REAL_USER\" ]; then\n";
        out << "    XAUTH_VAL=\"/home/$REAL_USER/.Xauthority\"\n";
        out << "fi\n";
        out << "if [ -n \"$REAL_USER\" ] && [ \"$REAL_USER\" != \"root\" ]; then\n";
        out << "    su - \"$REAL_USER\" -c \"DISPLAY='$DISP_VAL' XAUTHORITY='$XAUTH_VAL' WAYLAND_DISPLAY='" << curWayland
            << "' XDG_RUNTIME_DIR='" << curXdgRuntime << "' DBUS_SESSION_BUS_ADDRESS='" << curDbus
            << "' nohup /usr/bin/okidia-master >/dev/null 2>&1 &\"\n";
        out << "else\n";
        out << "    DISPLAY=\"$DISP_VAL\" nohup /usr/bin/okidia-master >/dev/null 2>&1 &\n";
        out << "fi\n";
        out << "rm -f \"" << packageFilePath << "\"\n";
        out << "rm -f \"" << scriptPath << "\"\n";
        out << "rm -f \"" << runnerPath << "\"\n";
        scriptFile.close();
        scriptFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                  QFile::ReadGroup | QFile::ExeGroup |
                                  QFile::ReadOther | QFile::ExeOther);
    }

    // Runner: sudo ile scriptPath'i arka planda (daemon/detached) çalıştırır ve hemen biter
    QFile runnerFile(runnerPath);
    if (runnerFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&runnerFile);
        out << "#!/bin/bash\n";
        out << "nohup " << scriptPath << " >/dev/null 2>&1 & disown\n";
        runnerFile.close();
        runnerFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                  QFile::ReadGroup | QFile::ExeGroup |
                                  QFile::ReadOther | QFile::ExeOther);
    }

    Logger::info(QObject::tr("MasterUpdater: Yetkili otonom kurulum başlatılıyor..."));
    bool started = PlatformService::executeElevated(runnerPath, QStringList(), parentWidget);
    if (started) {
        Logger::info(QObject::tr("MasterUpdater: Kurulum süreci arka planda başarıyla başlatıldı, Master kapatılıyor..."));
        QCoreApplication::quit();
    } else {
        Logger::error(QObject::tr("MasterUpdater: Kurulum süreci başlatılamadı!"));
    }
#endif
}
