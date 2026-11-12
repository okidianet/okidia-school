// agent/transfer/FileReceiver.cpp
#include "FileReceiver.h"
#include "../../common/utils/Logger.h"
#include "../../common/OkidiaConfig.h"
#include "../system/SystemInfo.h"

#include <QDir>
#include <QSystemTrayIcon>
#include <QApplication>
#include <QStandardPaths>
#include <QObject>

#if !defined(Q_OS_WIN)
#  include <pwd.h>
#  include <unistd.h>
#endif

FileReceiver::FileReceiver(QObject* parent)
    : QObject(parent) {}

// ---------------------------------------------------------------------------
void FileReceiver::handleEnvelope(const okidia::Envelope& envelope) {
    uint64_t seqId = envelope.sequence_id();
    switch (envelope.type()) {
    case okidia::MessageType::FILE_TRANSFER_BEGIN: {
        okidia::FileTransferBegin msg;
        if (msg.ParseFromString(envelope.payload()))
            handleBegin(seqId, msg);
        break;
    }
    case okidia::MessageType::FILE_TRANSFER_CHUNK: {
        okidia::FileTransferChunk msg;
        if (msg.ParseFromString(envelope.payload()))
            handleChunk(seqId, msg);
        break;
    }
    case okidia::MessageType::FILE_TRANSFER_END:
        handleEnd(seqId);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
void FileReceiver::handleBegin(uint64_t seqId, const okidia::FileTransferBegin& msg) {
    if (m_transfers.contains(seqId)) {
        Logger::warning(QObject::tr("FileReceiver: seqId %1 için devam eden transfer varken yeni BEGIN geldi").arg(seqId));
        auto* old = m_transfers.take(seqId);
        if (old->file.isOpen())
            old->file.close();
        delete old;
    }

    QString filename = QString::fromStdString(msg.filename());
    filename = QFileInfo(filename).fileName();
    if (filename.isEmpty()) filename = "dosya";

    // destination="tmp" → /tmp, "assets" → assetsDir, aksi halde masaüstü
    const QString dest = QString::fromStdString(msg.destination());
    QString targetDir;
    bool isTmp = (dest == "tmp");
    bool isAsset = (dest == "assets");
    bool isDirect = dest.isEmpty();
    if (isTmp) {
        targetDir = QDir::tempPath();
    } else if (isAsset) {
        targetDir = OkidiaConfig::assetsDir();
    } else {
        targetDir = resolveTargetDir();
    }

    QString targetPath = targetDir + "/" + filename;

    // Akıllı Varlık Kontrolü (Smart Asset Check):
    // Eğer bu bir varlık transferi ise ve hedef dosya diskte zaten mevcutsa ve boyutu aynıysa indirme pas geçilir.
    if (isAsset && QFile::exists(targetPath)) {
        quint64 currentSize = static_cast<quint64>(QFileInfo(targetPath).size());
        if (currentSize == msg.file_size() && currentSize > 0) {
            Logger::info(QObject::tr("FileReceiver: Varlık dosyası (%1) diskte zaten güncel ve aynı boyutta (%2 KB), indirme pas geçiliyor.")
                         .arg(filename).arg(currentSize / 1024));
            auto* t = new Transfer();
            t->sequenceId   = seqId;
            t->filename     = filename;
            t->expectedSize = msg.file_size();
            t->receivedSize = currentSize;
            t->readOnly     = msg.read_only();
            t->destIsTmp    = isTmp;
            t->destIsAsset  = isAsset;
            t->isDirectTransfer = isDirect;
            t->finalPath    = targetPath;
            t->isSkipped    = true;
            m_transfers[seqId] = t;
            return;
        }
    }

    auto* t = new Transfer();
    t->sequenceId   = seqId;
    t->filename     = filename;
    t->expectedSize = msg.file_size();
    t->receivedSize = 0;
    t->readOnly     = msg.read_only();
    t->destIsTmp    = isTmp;
    t->destIsAsset  = isAsset;
    t->isDirectTransfer = isDirect;
    t->finalPath    = targetPath;
    t->isSkipped    = false;

    // Varlık dosyaları (.part) geçici uzantısıyla yazılır, tamamlanınca atomik taşınır
    QString writePath = isAsset ? (targetPath + ".part") : targetPath;
    t->file.setFileName(writePath);

    if (!t->file.open(QIODevice::WriteOnly)) {
        Logger::error(QObject::tr("FileReceiver: Dosya açılamadı → %1").arg(writePath));
        emit receiveError(QObject::tr("Dosya yazılamadı: %1").arg(writePath));
        delete t;
        return;
    }

    m_transfers[seqId] = t;
    Logger::info(QObject::tr("FileReceiver: Transfer başladı (seqId=%1) → %2 (%3 KB)")
                 .arg(seqId).arg(filename).arg(msg.file_size() / 1024));
}

void FileReceiver::handleChunk(uint64_t seqId, const okidia::FileTransferChunk& msg) {
    auto* t = m_transfers.value(seqId, nullptr);
    if (!t) return;
    if (t->isSkipped) {
        if (msg.is_last()) {
            handleEnd(seqId);
        }
        return;
    }
    if (!t->file.isOpen()) return;

    QByteArray data = QByteArray::fromStdString(msg.data());
    t->file.write(data);
    t->receivedSize += static_cast<quint64>(data.size());

    if (msg.is_last()) {
        handleEnd(seqId);
    }
}

void FileReceiver::handleEnd(uint64_t seqId) {
    auto* t = m_transfers.take(seqId);
    if (!t) return;

    if (t->isSkipped) {
        Logger::info(QObject::tr("FileReceiver: Varlık transferi tamamlandı (Diskteki güncel dosya korundu) → %1").arg(t->finalPath));
        delete t;
        return;
    }

    if (t->file.isOpen()) {
        t->file.close();
    }

    QString path = t->file.fileName();
    if (t->destIsAsset && path.endsWith(".part")) {
        QFile::remove(t->finalPath);
        if (QFile::rename(path, t->finalPath)) {
            path = t->finalPath;
            Logger::info(QObject::tr("FileReceiver: Varlık dosyası atomik olarak kaydedildi → %1").arg(path));
        } else {
            Logger::warning(QObject::tr("FileReceiver: Varlık dosyası atomik taşınamadı → %1").arg(t->finalPath));
        }
    }

    qint64 size  = static_cast<qint64>(t->receivedSize);

    if (!t->destIsTmp) {
#if !defined(Q_OS_WIN)
        // ── Sahiplik: dosyayı gerçek kullanıcıya aktar (Linux) ──────────────
        const QString realUser = SystemInfo::username();
        if (!realUser.isEmpty() && realUser != "root") {
            struct passwd* pw = ::getpwnam(realUser.toLocal8Bit().constData());
            if (pw) {
                if (::chown(path.toLocal8Bit().constData(), pw->pw_uid, pw->pw_gid) != 0) {
                    Logger::warning(QObject::tr("FileReceiver: Sahiplik değiştirilemedi -> %1").arg(path));
                }
            }
        }
#endif

        // ── Dosya izinleri ──────────────────────────────────────────
        QFile::Permissions perms;
        if (t->readOnly) {
            perms = QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther;
            Logger::info(QObject::tr("FileReceiver: Salt okunur kaydedildi → %1").arg(path));
        } else {
            perms = QFile::ReadOwner | QFile::WriteOwner |
                    QFile::ReadGroup | QFile::ReadOther;
            Logger::info(QObject::tr("FileReceiver: Okuma/yazma kaydedildi → %1").arg(path));
        }
        QFile::setPermissions(path, perms);

#if !defined(Q_OS_WIN)
        if (!realUser.isEmpty() && realUser != "root") {
            struct passwd* pw = ::getpwnam(realUser.toLocal8Bit().constData());
            if (pw) {
                const QString dir = QFileInfo(path).absolutePath();
                if (::chown(dir.toLocal8Bit().constData(), pw->pw_uid, pw->pw_gid) != 0) {
                    Logger::warning(QObject::tr("FileReceiver: Klasör sahipliği değiştirilemedi -> %1").arg(dir));
                }
            }
        }
#endif
    } else {
        // /tmp dosyası
        QFile::setPermissions(path,
            QFile::ReadOwner | QFile::WriteOwner |
            QFile::ReadGroup | QFile::ReadOther);
        Logger::info(QObject::tr("FileReceiver: Geçici dizine kaydedildi → %1").arg(path));
        
        if (t->filename.startsWith("okidia-agent") && (t->filename.endsWith(".deb") || t->filename.endsWith(".exe"))) {
            Logger::info(QObject::tr("FileReceiver: OTA paketi algılandı, kurulum tetikleniyor → %1").arg(t->filename));
            emit otaUpdateReady(path);
        }
        
        delete t;
        return;
    }

    if (t->destIsAsset) {
        QFile::setPermissions(path,
            QFile::ReadOwner | QFile::WriteOwner |
            QFile::ReadGroup | QFile::ReadOther);
        Logger::info(QObject::tr("FileReceiver: Varlık dosyası (zil/arka plan) kaydedildi → %1").arg(path));
        // Arka plan varlık dosyaları için kullanıcıya diyalog/açma penceresi gösterilmez!
        delete t;
        return;
    }

    if (!t->isDirectTransfer) {
        // Sistem/iç transferler sessizce kaydedilir
        Logger::info(QObject::tr("FileReceiver: Sistem dosyası sessizce tamamlandı → %1").arg(path));
        delete t;
        return;
    }

    // YALNIZCA doğrudan kullanıcı dosya gönderimleri (Toolbar -> Dosya Gönder) için bildirim ve açma penceresi tetiklenir
    emit fileReceived(path, size);

    delete t;
}

// ---------------------------------------------------------------------------
QString FileReceiver::resolveTargetDir() const {
#if defined(Q_OS_WIN)
    QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty()) desktop = QDir::homePath() + "/Desktop";
    QString dir = desktop + "/OKIDIA_Dosyalar";
    QDir().mkpath(dir);
    Logger::info(QObject::tr("FileReceiver: Hedef klasör → %1").arg(dir));
    return dir;
#else
    const QString user = SystemInfo::username();
    const QString home = (user.isEmpty() || user == "root")
                       ? QDir::homePath()
                       : "/home/" + user;

    QString desktop;
    for (const QString& c : {home + QObject::tr("/Masaüstü"), home + "/Desktop"})
        if (QDir(c).exists()) { desktop = c; break; }

    const QString dir = (desktop.isEmpty() ? home : desktop) + "/OKIDIA_Dosyalar";
    const bool justCreated = !QDir(dir).exists();
    QDir().mkpath(dir);

    if (justCreated && !user.isEmpty() && user != "root") {
        struct passwd* pw = ::getpwnam(user.toLocal8Bit().constData());
        if (pw) {
            if (::chown(dir.toLocal8Bit().constData(), pw->pw_uid, pw->pw_gid) != 0) {
                Logger::warning(QObject::tr("FileReceiver: Hedef klasör sahipliği değiştirilemedi -> %1").arg(dir));
            }
        }
    }

    Logger::info(QObject::tr("FileReceiver: Hedef klasör → %1").arg(dir));
    return dir;
#endif
}
