// master/audit/AuditLog.cpp
// AuditLog: tüm kayıtlar broker DB'deki audit_log tablosuna yazılır.
// m_activeUser (actor + user_id) ve m_masterId setActiveUser'dan set edilir.
#include "AuditLog.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QFile>
#include <QTextStream>
#include <QHostInfo>

AuditLog& AuditLog::instance() {
    static AuditLog s;
    return s;
}

AuditLog::AuditLog(QObject* parent)
    : QObject(parent)
    , m_masterId(QHostInfo::localHostName())
{}

bool AuditLog::init(const QString& /*dbPath*/) {
    m_ready = true;
    Logger::info(QObject::tr("AuditLog: Ağ loglama modu aktif"));
    return true;
}

void AuditLog::setSessionManager(SessionManager* session) {
    m_session = session;
}

void AuditLog::setActiveUser(const QString& userId, const QString& username) {
    m_userId   = userId;
    m_actor    = username;
}

void AuditLog::record(const AuditEvent& event) {
    const QString actor  = event.actor.isEmpty()  ? m_actor    : event.actor;
    const QString userId = event.userId.isEmpty()  ? m_userId   : event.userId;
    qint64 timestamp = event.timestamp.isValid() ? event.timestamp.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();

    if (m_session && m_session->isConnected() && m_session->isAuthenticated()) {
        m_session->sendAuditLogRecord(
            actor,
            event.target,
            event.detail,
            categoryToString(event.category),
            userId,
            m_masterId,
            timestamp);
    } else {
        Logger::warning(QObject::tr("AuditLog: Broker bağlantısı aktif değil veya session set edilmemiş. Kayıt atlandı: ") + event.detail);
    }

    Logger::audit(QObject::tr("[%1] %2 → %3: %4")
        .arg(categoryToString(event.category))
        .arg(actor).arg(event.target).arg(event.detail));
}

void AuditLog::recordCommand(const QString& actor,
                               const QString& target,
                               const QString& command) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Command;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = command;
    record(e);
}

void AuditLog::recordAuth(const QString& actor, const QString& detail) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Auth;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.detail    = detail;
    record(e);
}

void AuditLog::recordFile(const QString& actor,
                           const QString& target,
                           const QString& filename) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::FileTransfer;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = tr("Dosya: ") + filename;
    record(e);
}

void AuditLog::recordPower(const QString& actor,
                           const QString& target,
                           const QString& action) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Power;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = action;
    record(e);
}

void AuditLog::recordExam(const QString& actor,
                          const QString& target,
                          const QString& detail) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Exam;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = detail;
    record(e);
}

void AuditLog::recordDeployment(const QString& actor,
                                const QString& target,
                                const QString& detail) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Deployment;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = detail;
    record(e);
}

void AuditLog::recordChat(const QString& actor,
                          const QString& target,
                          const QString& detail) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Chat;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = detail;
    record(e);
}

void AuditLog::recordSecurity(const QString& actor,
                              const QString& target,
                              const QString& detail) {
    AuditEvent e;
    e.timestamp = QDateTime::currentDateTime();
    e.category  = AuditEvent::Security;
    e.actor     = actor.isEmpty() ? m_actor : actor;
    e.userId    = m_userId;
    e.target    = target;
    e.detail    = detail;
    record(e);
}

QList<AuditEvent> AuditLog::query(const QDateTime& /*from*/,
                                    const QDateTime& /*to*/,
                                    AuditEvent::Category /*category*/,
                                    int /*limit*/) const {
    // KVKK İzolasyonu: Master yerel veritabanında denetim kaydı tutmaz.
    // Denetim logları merkezi olarak Broker mTLS sorgusu (SessionManager::sendAuditLogQuery) ile ReportPanel üzerinden listelenir.
    return {};
}

bool AuditLog::exportCsv(const QString& filePath,
                          const QDateTime& from,
                          const QDateTime& to) const {
    auto events = query(from, to);
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << tr("Zaman,Kategori,Aktör,Hedef,Açıklama\n");
    for (const auto& e : events) {
        QString actor  = e.actor;
        QString target = e.target;
        QString detail = e.detail;
        out << OkidiaUtils::formatDateTime(e.timestamp) << ","
            << categoryToString(e.category) << ","
            << "\"" << actor.replace('"', "'")  << "\","
            << "\"" << target.replace('"', "'") << "\","
            << "\"" << detail.replace('"', "'") << "\"\n";
    }
    return true;
}

QString AuditLog::categoryToString(AuditEvent::Category c) {
    switch (c) {
    case AuditEvent::Auth:         return "Auth";
    case AuditEvent::Command:      return "Command";
    case AuditEvent::FileTransfer: return "FileTransfer";
    case AuditEvent::Chat:         return "Chat";
    case AuditEvent::System:       return "System";
    case AuditEvent::Security:     return "Security";
    case AuditEvent::Power:        return "Power";
    case AuditEvent::Deployment:   return "Deployment";
    case AuditEvent::Exam:         return "Exam";
    default:                       return "Unknown";
    }
}

AuditEvent::Category AuditLog::stringToCategory(const QString& s) {
    if (s == "Auth")         return AuditEvent::Auth;
    if (s == "Command")      return AuditEvent::Command;
    if (s == "FileTransfer") return AuditEvent::FileTransfer;
    if (s == "Chat")         return AuditEvent::Chat;
    if (s == "System")       return AuditEvent::System;
    if (s == "Security")     return AuditEvent::Security;
    if (s == "Power")        return AuditEvent::Power;
    if (s == "Deployment")   return AuditEvent::Deployment;
    if (s == "Exam")         return AuditEvent::Exam;
    return AuditEvent::System;
}

QString AuditLog::categoryDisplayName(AuditEvent::Category c) {
    switch (c) {
    case AuditEvent::Auth:         return tr("Kimlik Doğrulama");
    case AuditEvent::Command:      return tr("Komut");
    case AuditEvent::FileTransfer: return tr("Dosya & Anons");
    case AuditEvent::Chat:         return tr("Sohbet");
    case AuditEvent::System:       return tr("Sistem");
    case AuditEvent::Security:     return tr("Güvenlik & Kilit");
    case AuditEvent::Power:        return tr("Güç & WOL");
    case AuditEvent::Deployment:   return tr("Dağıtım & OTA");
    case AuditEvent::Exam:         return tr("Sınav Yönetimi");
    default:                       return tr("Bilinmeyen");
    }
}
