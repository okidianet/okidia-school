// master/diagnostics/DiagnosticCollector.h
#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QPixmap>
#include <QDateTime>

class SessionManager;

struct IncidentReportRequest {
    QString category;           // Örn: "audio_broadcast", "screen_broadcast", "lock_screen", "remote_control", "file_transfer", "other"
    QString userComments;       // Kullanıcının yazdığı açıklama
    int timeWindowMinutes = 15; // Son 15 dk / 60 dk / 1440 dk
    bool includeScreenshot = true;
    QPixmap screenshot;
    SessionManager* session = nullptr;
};

struct IncidentReportResult {
    bool success = false;
    QString incidentId;          // Örn: "OKI-849201"
    QString zipFilePath;         // /tmp/okidia_incident_OKI-849201.zip
    QString errorMessage;
    QJsonObject diagnosticsJson; // Hazırlanan kurumsal JSON manifesti
};

/**
 * @brief Okidia Teşhis ve Olay Manifesti Toplayıcı.
 * Tamamen kurumsal telemetri şeması (schema_version: "2.1.0") ile sistem,
 * çalışma zamanı künyesi ve maskelenmiş log hata çerçevelerini toplar ve .zip arşivine paketler.
 */
class DiagnosticCollector {
public:
    static IncidentReportResult collect(const IncidentReportRequest& req);

private:
    static QString generateIncidentId();
    static QJsonObject buildBuildContext();
    static QJsonObject buildHostEnvironment();
    static QJsonObject buildClusterState(SessionManager* session, const QString& category);
    static QJsonArray extractErrorFrames(int timeWindowMinutes, QString& outRawLogExtract);
    static QString sanitizeText(const QString& raw);
};
