// master/transfer/OtaSender.cpp
#include "OtaSender.h"
#include "../../common/utils/Logger.h"
#include "messages.pb.h"
#include "../../common/crypto/KeyManager.h"
#include "../../common/OkidiaConfig.h"
#include <QFileInfo>
#include <QCryptographicHash>
#include <QRegularExpression>

OtaSender::OtaSender(Connection* conn, const QString& userId, QObject* parent) 
    : QObject(parent), m_conn(conn), m_userId(userId) {
    m_timer = new QTimer(this);
    m_timer->setInterval(10);
    connect(m_timer, &QTimer::timeout, this, &OtaSender::sendNextChunk);
}

OtaSender::~OtaSender() {}

bool OtaSender::sendPackages(const QStringList& filePaths) {
    if (!m_conn || !m_conn->isConnected()) {
        emit finished(false, tr("Broker bağlantısı yok"));
        return false;
    }
    if (filePaths.isEmpty()) {
        emit finished(false, tr("Gönderilecek dosya seçilmedi"));
        return false;
    }

    m_isBrokerSelfUpdate = false;
    m_queue = filePaths;
    m_totalFiles = filePaths.size();
    m_currentIdx = 0;

    startNextFile();
    return true;
}

bool OtaSender::sendBrokerSelfUpdate(const QString& filePath) {
    if (!m_conn || !m_conn->isConnected()) {
        emit finished(false, tr("Broker bağlantısı yok"));
        return false;
    }
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        emit finished(false, tr("Broker güncelleme paketi bulunamadı"));
        return false;
    }

    m_isBrokerSelfUpdate = true;
    m_queue = QStringList() << filePath;
    m_totalFiles = 1;
    m_currentIdx = 0;

    startNextFile();
    return true;
}

void OtaSender::startNextFile() {
    if (m_queue.isEmpty()) {
        emit finished(true, "");
        return;
    }

    if (m_file.isOpen()) {
        m_file.close();
    }

    QString path = m_queue.takeFirst();
    m_currentIdx++;

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        emit finished(false, tr("Dosya okunamadı: ") + path);
        return;
    }

    m_fileSize = m_file.size();
    m_chunkIndex = 0;
    m_bytesSent = 0;

    QFileInfo fi(path);
    QString filename = fi.fileName(); // okidia-agent_ubuntu24_0.1.316_amd64.deb or okidia-broker-setup-v0.1.568.exe
    
    QString osTag = "unknown";
    QString version = "0.0.0";
    
    QRegularExpression verRe(R"((\d+\.\d+(?:\.\d+)?))");
    auto vm = verRe.match(filename);
    if (vm.hasMatch()) {
        version = vm.captured(1);
    }

    if (filename.endsWith(".exe", Qt::CaseInsensitive)) {
        osTag = "windows";
    } else {
        QStringList parts = filename.split('_');
        if (parts.size() >= 4) {
            osTag = parts[1];
        } else {
            osTag = "linux";
        }
    }

    // Hash hesapla
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(&m_file);
    QByteArray fileHash = hasher.result();
    m_file.seek(0); // Okuma için başa dön

    // İmza at
    QString profileName = m_userId.isEmpty() ? "DefaultAdmin" : m_userId;
    QString networkKey = OkidiaConfig::currentNetworkKey();
    QString keyPath = OkidiaConfig::userKeyPath(profileName, networkKey);
    QByteArray signature = KeyManager::signChallenge(fileHash, keyPath);

    if (signature.isEmpty()) {
        Logger::error(QObject::tr("OtaSender: İmza atılamadı, işlem iptal ediliyor."));
        m_file.close();
        emit finished(false, tr("İmzalama hatası"));
        return;
    }

    // Master sertifikasını oku
    QString certPath = OkidiaConfig::userCertPath(profileName, networkKey);
    QFile certFile(certPath);
    QByteArray masterCert;
    if (certFile.open(QIODevice::ReadOnly)) {
        masterCert = certFile.readAll();
        certFile.close();
    } else {
        Logger::error(QObject::tr("OtaSender: Master sertifikası okunamadı: ") + certPath);
        m_file.close();
        emit finished(false, tr("Sertifika okuma hatası"));
        return;
    }

    okidia::OtaUpdateBegin begin;
    begin.set_filename(filename.toStdString());
    begin.set_file_size(m_fileSize);
    begin.set_os_tag(osTag.toStdString());
    begin.set_version(version.toStdString());
    begin.set_file_hash(fileHash.constData(), fileHash.size());
    begin.set_signature(signature.constData(), signature.size());
    begin.set_master_cert(masterCert.constData(), masterCert.size());

    okidia::Envelope env;
    env.set_type(m_isBrokerSelfUpdate ? okidia::MessageType::BROKER_OTA_UPDATE_BEGIN : okidia::MessageType::OTA_UPDATE_BEGIN);
    env.set_payload(begin.SerializeAsString());
    m_conn->sendEnvelope(env);

    m_timer->start();
}

void OtaSender::sendNextChunk() {
    if (!m_file.isOpen() || m_file.atEnd()) {
        m_timer->stop();
        okidia::Envelope env;
        env.set_type(m_isBrokerSelfUpdate ? okidia::MessageType::BROKER_OTA_UPDATE_END : okidia::MessageType::OTA_UPDATE_END);
        m_conn->sendEnvelope(env);
        
        m_file.close();
        startNextFile();
        return;
    }

    QByteArray chunk = m_file.read(512 * 1024); // 512 KB
    bool isLast = m_file.atEnd();
    
    okidia::OtaUpdateChunk msg;
    msg.set_chunk_index(m_chunkIndex++);
    msg.set_data(chunk.data(), chunk.size());
    msg.set_is_last(isLast);

    okidia::Envelope env;
    env.set_type(m_isBrokerSelfUpdate ? okidia::MessageType::BROKER_OTA_UPDATE_CHUNK : okidia::MessageType::OTA_UPDATE_CHUNK);
    env.set_payload(msg.SerializeAsString());
    m_conn->sendEnvelope(env);

    m_bytesSent += chunk.size();
    emit progress(m_currentIdx, m_totalFiles, m_bytesSent, m_fileSize);

    if (isLast) {
        m_timer->stop();
        m_file.close();
        startNextFile();
    }
}
