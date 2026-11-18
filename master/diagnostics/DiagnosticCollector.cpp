// master/diagnostics/DiagnosticCollector.cpp
#include "DiagnosticCollector.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/version/OkidiaVersion.h"
#include "../session/SessionManager.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSysInfo>
#include <QRandomGenerator>
#include <QBuffer>
#include <QDateTime>

IncidentReportResult DiagnosticCollector::collect(const IncidentReportRequest& req) {
    IncidentReportResult res;
    res.incidentId = generateIncidentId();

    const QString tempDirPath = QDir::tempPath() + "/okidia_diag_" + res.incidentId;
    QDir tempDir(tempDirPath);
    if (!tempDir.exists()) {
        tempDir.mkpath(".");
    }

    // 1. Log Extract ve Error Frame Analizi
    QString rawLogExtract;
    QJsonArray errorFrames = extractErrorFrames(req.timeWindowMinutes, rawLogExtract);

    // 2. diagnostics.json Manifestini Oluştur
    QJsonObject root;
    root["schema_version"] = "2.1.0";
    root["incident_id"] = res.incidentId;
    root["created_at_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["local_time"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    root["build_context"] = buildBuildContext();
    root["host_environment"] = buildHostEnvironment();
    root["cluster_state"] = buildClusterState(req.session, req.category);

    QJsonObject userAnnotation;
    userAnnotation["category"] = req.category;
    userAnnotation["user_comments"] = sanitizeText(req.userComments.trimmed());
    userAnnotation["time_window_minutes"] = req.timeWindowMinutes;
    userAnnotation["has_screenshot"] = req.includeScreenshot && !req.screenshot.isNull();
    root["user_annotation"] = userAnnotation;

    root["extracted_error_frames"] = errorFrames;

    res.diagnosticsJson = root;

    // diagnostics.json dosyasını yaz
    QFile jsonFile(tempDirPath + "/diagnostics.json");
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        jsonFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        jsonFile.close();
    }

    // Filtrelenmiş ham log dosyasını logs/master.log olarak kaydet
    QDir(tempDirPath).mkpath("logs");
    QFile logFile(tempDirPath + "/logs/master.log");
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logFile.write(rawLogExtract.toUtf8());
        logFile.close();
    }

    // 3. Ekran Görüntüsü Varsa Kaydet
    if (req.includeScreenshot && !req.screenshot.isNull()) {
        req.screenshot.save(tempDirPath + "/screenshot.png", "PNG", 85);
    }

    // 4. Arşivi Oluştur (.zip)
    const QString zipPath = QDir::tempPath() + "/okidia_incident_" + res.incidentId + ".zip";
    bool ok = OkidiaUtils::createLocalZipArchive(zipPath, tempDirPath);

    // Geçici klasörü temizle
    tempDir.removeRecursively();

    if (!ok || !QFile::exists(zipPath)) {
        res.success = false;
        res.errorMessage = QObject::tr("Teşhis paketi zip arşivine sıkıştırılamadı.");
        return res;
    }

    res.success = true;
    res.zipFilePath = zipPath;
    Logger::info(QObject::tr("DiagnosticCollector: Teşhis paketi başarıyla hazırlandı -> %1 (%2)")
                 .arg(zipPath, res.incidentId));
    return res;
}

QString DiagnosticCollector::generateIncidentId() {
    // Örnek: "OKI-849201"
    const quint32 randNum = QRandomGenerator::global()->bounded(100000, 999999);
    return QString("OKI-%1").arg(randNum);
}

QJsonObject DiagnosticCollector::buildBuildContext() {
    QJsonObject ctx;
    ctx["component"] = "okidia-master";
    ctx["version"] = Okidia::Version::string();
    ctx["os_tag"] = Okidia::Version::osTag();
    ctx["qt_version"] = qVersion();

#if defined(__clang__)
    ctx["compiler"] = QString("Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__)
    ctx["compiler"] = QString("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    ctx["compiler"] = QString("MSVC %1").arg(_MSC_VER);
#else
    ctx["compiler"] = "Unknown";
#endif

    ctx["build_timestamp"] = QString(__DATE__ " " __TIME__);
    return ctx;
}

QJsonObject DiagnosticCollector::buildHostEnvironment() {
    QJsonObject host;
    host["os_name"] = QSysInfo::prettyProductName();
    host["kernel_type"] = QSysInfo::kernelType();
    host["kernel_version"] = QSysInfo::kernelVersion();
    host["cpu_arch"] = QSysInfo::currentCpuArchitecture();
    host["build_abi"] = QSysInfo::buildAbi();
    host["machine_host"] = QSysInfo::machineHostName();
    return host;
}

QJsonObject DiagnosticCollector::buildClusterState(SessionManager* session, const QString& category) {
    QJsonObject state;
    state["category_context"] = category;
    if (session) {
        state["broker_connected"] = session->isConnected();
        state["broker_connecting"] = session->isConnecting();
        state["active_clients_count"] = session->clientStates().size();
    } else {
        state["broker_connected"] = false;
        state["broker_connecting"] = false;
        state["active_clients_count"] = 0;
    }
    return state;
}

QJsonArray DiagnosticCollector::extractErrorFrames(int timeWindowMinutes, QString& outRawLogExtract) {
    QJsonArray frames;
    const QString logPath = Logger::logFilePath();
    if (logPath.isEmpty() || !QFile::exists(logPath)) {
        outRawLogExtract = QObject::tr("# Log dosyası bulunamadı.");
        return frames;
    }

    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        outRawLogExtract = QObject::tr("# Log dosyası açılamadı: %1").arg(logPath);
        return frames;
    }

    // Son 1500 satırı oku
    QStringList allLines;
    QTextStream in(&file);
    while (!in.atEnd()) {
        allLines << in.readLine();
        if (allLines.size() > 1500) {
            allLines.removeFirst();
        }
    }
    file.close();

    // Zaman eşiği
    const QDateTime cutoff = (timeWindowMinutes > 0)
        ? QDateTime::currentDateTime().addSecs(-timeWindowMinutes * 60)
        : QDateTime::currentDateTime().addDays(-1);

    // Regex ayrıştırıcı: [YYYY-MM-DD HH:MM:SS.zzz] [LEVEL] [file.cpp:123] mesaj
    const QRegularExpression logRegex(
        R"(^\[(?<datetime>[^\]]+)\]\s*\[(?<level>[A-Z]+)\]\s*(?:\[(?<loc>[^\]]+)\])?\s*(?<msg>.*)$)");

    QStringList filteredLines;

    for (const QString& rawLine : allLines) {
        QString line = sanitizeText(rawLine);
        const QRegularExpressionMatch match = logRegex.match(line);

        if (match.hasMatch()) {
            const QString dtStr = match.captured("datetime");
            const QString level = match.captured("level");
            const QString loc   = match.captured("loc");
            const QString msg   = match.captured("msg");

            // Zaman kontrolü (eğer datetime parse edilebiliyorsa)
            QDateTime logTime = QDateTime::fromString(dtStr.left(19), "yyyy-MM-dd HH:mm:ss");
            if (logTime.isValid() && logTime < cutoff) {
                continue; // Zaman penceresi dışında
            }

            filteredLines << line;

            // Yalnızca ERROR, CRITICAL, FATAL ve WARNING'leri çerçeveye al
            if (level == "ERROR" || level == "CRITICAL" || level == "FATAL" || level == "WARNING") {
                QJsonObject frame;
                frame["timestamp"] = dtStr;
                frame["severity"] = level;

                if (!loc.isEmpty()) {
                    int colonIdx = loc.lastIndexOf(':');
                    if (colonIdx > 0) {
                        frame["source_file"] = loc.left(colonIdx);
                        frame["line_number"] = loc.mid(colonIdx + 1).toInt();
                    } else {
                        frame["source_file"] = loc;
                        frame["line_number"] = 0;
                    }
                } else {
                    frame["source_file"] = "unknown";
                    frame["line_number"] = 0;
                }

                frame["trace"] = msg;
                frames.append(frame);
            }
        } else {
            // Eşleşmeyen ham satır (multiline stacktrace vb.)
            filteredLines << line;
        }
    }

    outRawLogExtract = filteredLines.join("\n");
    return frames;
}

QString DiagnosticCollector::sanitizeText(const QString& raw) {
    if (raw.isEmpty()) return raw;

    QString clean = raw;

    // Şifre, parola, secret, token, sha256 gibi hassas anahtar kelimeleri maskele
    static const QRegularExpression passRegex(
        R"((password|passwd|sifre|token|secret|authKey|hash)\s*[:=]\s*["']?([^\s,"']+)["']?)",
        QRegularExpression::CaseInsensitiveOption);
    clean.replace(passRegex, "\\1=***MASKED***");

    // 64 karakterli SHA256 / Hex dizilerini maskele
    static const QRegularExpression hex64Regex(R"(\b[a-fA-F0-9]{64}\b)");
    clean.replace(hex64Regex, "***HASH-64***");

    return clean;
}
