// common/utils/Logger.h
#pragma once

#include <QString>
#include <QStringList>
#include <QFile>
#include <QMutex>
#include <QRecursiveMutex>
#include <QDateTime>
#include <QFileInfo>
#include <QList>

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Audit   // Güvenlik/yetki olayları — her zaman loglanır
};

struct LogFileInfo {
    QString fileName;
    QString filePath;
    qint64  sizeBytes = 0;
    QDateTime lastModified;
};

class Logger {
public:
    static constexpr qint64 MAX_LOG_SIZE = 10 * 1024 * 1024; // 10 MB
    static constexpr int DEFAULT_RETENTION_DAYS = 28;

    // Log dosyasını başlat (program başında çağrılır)
    static void init(const QString& logFilePath, int retentionDays = DEFAULT_RETENTION_DAYS);

    static void debug(const QString& msg)   { log(LogLevel::Debug,   msg); }
    static void info(const QString& msg)    { log(LogLevel::Info,    msg); }
    static void warning(const QString& msg) { log(LogLevel::Warning, msg); }
    static void error(const QString& msg)   { log(LogLevel::Error,   msg); }
    static void audit(const QString& msg)   { log(LogLevel::Audit,   msg); }

    // Log dosyasını döndür (raporlama için)
    static QString logFilePath() { return s_logFilePath; }
    static QString logDir();

    // Son N satırı döndür (raporlama paneli için)
    static QStringList lastLines(int n = 200);

    // Gelişmiş satır ve zaman filtreli okuma
    static QStringList readLogLines(const QString& filePath = QString(),
                                    int maxLines = 500,
                                    const QDateTime& fromTime = QDateTime(),
                                    const QDateTime& toTime = QDateTime(),
                                    const QString& levelFilter = QString(),
                                    const QString& textFilter = QString());

    // Mevcut log dosyalarını listele (en yeniden eskiye)
    static QList<LogFileInfo> availableLogFiles(const QString& logDirPath = QString());

    // Eski logları temizle
    static void purgeOldLogsAsync(int retentionDays = DEFAULT_RETENTION_DAYS);
    static int  purgeOldLogs(const QString& logDirPath, int retentionDays = DEFAULT_RETENTION_DAYS);

    // Aktif log dosyasını sıfırla / temizle
    static bool clearCurrentLog();

    // Dinamik log filtre seviyesi
    static void setMinLogLevel(LogLevel level) { s_minLogLevel = level; }
    static LogLevel minLogLevel() { return s_minLogLevel; }

    // Bekleyen bastırılmış log özetlerini diske boşalt
    static void flush();

private:
    static void log(LogLevel level, const QString& msg);
    static void rotateIfNeededLocked();
    static void flushSuppressedMessageLocked();

    static QFile           s_logFile;
    static QString         s_logFilePath;
    static QString         s_activeLogDate; // yyyy-MM-dd
    static int             s_retentionDays;
    static QRecursiveMutex s_mutex;
    static LogLevel        s_minLogLevel;

    // Hata ve mesaj tekrar bastırma (Throttling)
    static QString         s_lastMsg;
    static LogLevel        s_lastLevel;
    static int             s_repeatCount;
    static qint64          s_firstRepeatSecs;
    static qint64          s_lastRepeatSecs;
    static constexpr int   THROTTLE_THRESHOLD = 5;
    static constexpr qint64 THROTTLE_WINDOW_SECS = 60;
};
