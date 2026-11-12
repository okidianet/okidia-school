#include "Logger.h"
#include "OkidiaUtils.h"

#include <QDateTime>
#include <QTextStream>
#include <QMutexLocker>
#include <QDir>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QTimer>
#include <iostream>

// Statik üyeler
QFile           Logger::s_logFile;
QString         Logger::s_logFilePath;
QString         Logger::s_activeLogDate;
int             Logger::s_retentionDays = Logger::DEFAULT_RETENTION_DAYS;
QRecursiveMutex Logger::s_mutex;
LogLevel        Logger::s_minLogLevel = LogLevel::Info;

// Throttling
QString         Logger::s_lastMsg;
LogLevel        Logger::s_lastLevel = LogLevel::Info;
int             Logger::s_repeatCount = 0;
qint64          Logger::s_firstRepeatSecs = 0;
qint64          Logger::s_lastRepeatSecs = 0;

void Logger::flush() {
    QMutexLocker lock(&s_mutex);
    flushSuppressedMessageLocked();
    if (s_logFile.isOpen()) {
        s_logFile.flush();
    }
}

void Logger::flushSuppressedMessageLocked() {
    if (s_repeatCount > THROTTLE_THRESHOLD && s_logFile.isOpen()) {
        int suppressedCount = s_repeatCount - THROTTLE_THRESHOLD;
        QString summary = QString("%1 [INFO] [Önceki mesaj son %2 saniyede %3 kez tekrarlandı, disk şişmesini önlemek için bastırıldı]")
            .arg(OkidiaUtils::formatDateTime())
            .arg(qMax<qint64>(1, s_lastRepeatSecs - s_firstRepeatSecs + 1))
            .arg(suppressedCount);

        std::cout << summary.toStdString() << std::endl;
        QTextStream out(&s_logFile);
        out << summary << "\n";
        s_logFile.flush();
    }
    s_repeatCount = 0;
    s_lastMsg.clear();
    s_firstRepeatSecs = 0;
    s_lastRepeatSecs = 0;
}

void Logger::init(const QString& logFilePath, int retentionDays) {
    QMutexLocker lock(&s_mutex);

    s_logFilePath = logFilePath;
    s_retentionDays = retentionDays;
    s_activeLogDate = QDate::currentDate().toString("yyyy-MM-dd");

    // Dizin yoksa oluştur
    QDir().mkpath(QFileInfo(logFilePath).absolutePath());

    // Eğer aktif dosya zaten varsa ve önceki bir günden kalmışsa veya >= 10MB ise başlatırken rotasyon yap
    if (QFile::exists(logFilePath)) {
        QFileInfo fi(logFilePath);
        QDate fileDate = fi.lastModified().date();
        if (fileDate < QDate::currentDate() || fi.size() >= MAX_LOG_SIZE) {
            QString baseDir = fi.absolutePath();
            QString baseName = fi.baseName();
            QString suffix = fi.completeSuffix();
            QString datePrefix = fileDate.toString("yyyy-MM-dd");

            // Sıralı parça numarası belirle (_1, _2, ...)
            int part = 1;
            QString rotatedName;
            do {
                rotatedName = QString("%1/%2_%3_%4.%5").arg(baseDir, baseName, datePrefix, QString::number(part), suffix);
                part++;
            } while (QFile::exists(rotatedName));

            QFile::rename(logFilePath, rotatedName);
        }
    }

    s_logFile.setFileName(logFilePath);
    if (!s_logFile.open(QIODevice::Append | QIODevice::Text)) {
        std::cerr << "[Logger] Log dosyası açılamadı: "
                  << logFilePath.toUtf8().constData() << std::endl;
        return;
    }

    // Başlangıç başlığı yaz
    {
        QTextStream out(&s_logFile);
        out << "\n========== OKIDIA başlatıldı: "
            << OkidiaUtils::formatDateTime()
            << " ==========\n";
        s_logFile.flush();
    }

    // Başlangıçta arka planda eski log temizliğini tetikle
    purgeOldLogsAsync(s_retentionDays);
}

QString Logger::logDir() {
    QMutexLocker lock(&s_mutex);
    if (!s_logFilePath.isEmpty()) {
        return QFileInfo(s_logFilePath).absolutePath();
    }
    return QString();
}

void Logger::rotateIfNeededLocked() {
    if (!s_logFile.isOpen()) return;

    QString todayStr = QDate::currentDate().toString("yyyy-MM-dd");
    bool dateChanged = (!s_activeLogDate.isEmpty() && s_activeLogDate != todayStr);
    bool sizeExceeded = (s_logFile.size() >= MAX_LOG_SIZE);

    if (!dateChanged && !sizeExceeded) {
        return;
    }

    flushSuppressedMessageLocked();

    s_logFile.flush();
    s_logFile.close();

    QFileInfo fi(s_logFilePath);
    QString baseDir = fi.absolutePath();
    QString baseName = fi.baseName();
    QString suffix = fi.completeSuffix();

    int part = 1;
    QString rotatedPath;
    do {
        rotatedPath = QString("%1/%2_%3_%4.%5").arg(baseDir, baseName, s_activeLogDate, QString::number(part), suffix);
        part++;
    } while (QFile::exists(rotatedPath));

    QFile::rename(s_logFilePath, rotatedPath);

    s_logFile.setFileName(s_logFilePath);
    if (!s_logFile.open(QIODevice::Append | QIODevice::Text)) {
        std::cerr << "[Logger] Log rotasyonu sonrasi dosya acilamadi: "
                  << s_logFilePath.toUtf8().constData() << std::endl;
    }
    s_activeLogDate = todayStr;

    // Arka planda 28 günden eski logları temizle
    purgeOldLogsAsync(s_retentionDays);
}

void Logger::log(LogLevel level, const QString& msg) {
    QMutexLocker lock(&s_mutex);

    // Seviye Filtresi: Audit ve Error her zaman loglanır; Debug ve Info minLogLevel kontrolüne tabidir.
    if (level != LogLevel::Audit && level != LogLevel::Error) {
        if (static_cast<int>(level) < static_cast<int>(s_minLogLevel)) {
            return;
        }
    }

    qint64 nowSecs = QDateTime::currentSecsSinceEpoch();

    // Hata ve Mesaj Tekrar Bastırma (Throttling)
    if (level == s_lastLevel && msg == s_lastMsg && (nowSecs - s_firstRepeatSecs) < THROTTLE_WINDOW_SECS) {
        s_repeatCount++;
        s_lastRepeatSecs = nowSecs;
        if (s_repeatCount > THROTTLE_THRESHOLD) {
            // Eşik aşıldı: konsola veya diske yazmayı baskıla
            return;
        }
    } else {
        // Yeni bir mesaj geldiğinde varsa önceki bastırılan tekrarların özetini dök
        if (s_repeatCount > THROTTLE_THRESHOLD) {
            flushSuppressedMessageLocked();
        }
        s_lastMsg = msg;
        s_lastLevel = level;
        s_repeatCount = 1;
        s_firstRepeatSecs = nowSecs;
        s_lastRepeatSecs = nowSecs;
    }

    rotateIfNeededLocked();

    QString prefix;
    switch (level) {
    case LogLevel::Debug:   prefix = "[DEBUG]"; break;
    case LogLevel::Info:    prefix = "[INFO] "; break;
    case LogLevel::Warning: prefix = "[WARN] "; break;
    case LogLevel::Error:   prefix = "[ERROR]"; break;
    case LogLevel::Audit:   prefix = "[AUDIT]"; break;
    }

    QString line = QString("%1 %2 %3")
        .arg(OkidiaUtils::formatDateTime())
        .arg(prefix)
        .arg(msg);

    // Terminale
    std::cout << line.toStdString() << std::endl;
    if (level == LogLevel::Error)
        std::cerr << line.toStdString() << std::endl;

    // Dosyaya
    if (s_logFile.isOpen()) {
        QTextStream out(&s_logFile);
        out << line << "\n";
        // Audit ve Error'ı anında flush et
        if (level == LogLevel::Audit || level == LogLevel::Error)
            s_logFile.flush();
    }
}

QStringList Logger::lastLines(int n) {
    return readLogLines(s_logFilePath, n);
}

QStringList Logger::readLogLines(const QString& filePath,
                                 int maxLines,
                                 const QDateTime& fromTime,
                                 const QDateTime& toTime,
                                 const QString& levelFilter,
                                 const QString& textFilter) {
    QString targetPath = filePath.isEmpty() ? s_logFilePath : filePath;
    if (targetPath.isEmpty()) return {};

    // Eğer aktif dosyayı okuyorsak içeriği diske flush et
    {
        QMutexLocker lock(&s_mutex);
        if (s_logFile.isOpen() && targetPath == s_logFilePath) {
            s_logFile.flush();
        }
    }

    QFile f(targetPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList matched;
    QTextStream in(&f);

    bool checkTime = fromTime.isValid() || toTime.isValid();
    bool checkLevel = !levelFilter.isEmpty();
    bool checkText = !textFilter.isEmpty();

    QString levelPattern = checkLevel ? ("[" + levelFilter.toUpper()) : QString();
    bool inTimeWindow = true;

    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;

        if (checkTime && line.length() >= 19) {
            QDateTime lineDt = QDateTime::fromString(line.left(19), "yyyy-MM-dd hh:mm:ss");
            if (lineDt.isValid()) {
                inTimeWindow = true;
                if (fromTime.isValid() && lineDt < fromTime) inTimeWindow = false;
                if (toTime.isValid() && lineDt > toTime)     inTimeWindow = false;
            }
        }

        if (checkTime && !inTimeWindow) {
            continue;
        }

        if (checkLevel && !line.contains(levelPattern, Qt::CaseInsensitive)) {
            continue;
        }

        if (checkText && !line.contains(textFilter, Qt::CaseInsensitive)) {
            continue;
        }

        matched.append(line);
    }

    f.close();

    if (maxLines > 0 && matched.size() > maxLines) {
        return matched.mid(matched.size() - maxLines);
    }

    return matched;
}

QList<LogFileInfo> Logger::availableLogFiles(const QString& logDirPath) {
    QString dirPath = logDirPath.isEmpty() ? s_logFilePath : logDirPath;
    if (!dirPath.isEmpty() && !QFileInfo(dirPath).isDir()) {
        dirPath = QFileInfo(dirPath).absolutePath();
    }
    QDir dir(dirPath);
    if (!dir.exists()) return {};

    QStringList filters;
    filters << "okidia-*.log" << "okidia-*.log.*";

    QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Time); // Yeniden eskiye
    QList<LogFileInfo> list;
    for (const QFileInfo& fi : entries) {
        LogFileInfo info;
        info.fileName = fi.fileName();
        info.filePath = fi.absoluteFilePath();
        info.sizeBytes = fi.size();
        info.lastModified = fi.lastModified();
        list.append(info);
    }
    return list;
}

void Logger::purgeOldLogsAsync(int retentionDays) {
    QString dirPath;
    {
        QMutexLocker lock(&s_mutex);
        if (!s_logFilePath.isEmpty()) {
            dirPath = QFileInfo(s_logFilePath).absolutePath();
        }
    }
    if (dirPath.isEmpty()) return;

    // MinGW POSIX std::thread (libwinpthread) çıkışı sırasında emutls_destroy'un
    // Qt6Core!__dyn_tls_dtor'dan önce çalışarak thread_local (QCollator/QSystemLocale)
    // Use-After-Free çökmesi yaratmasını önlemek için std::thread yerine Qt olay döngüsü kullanılır.
    if (QCoreApplication::instance()) {
        QTimer::singleShot(5000, QCoreApplication::instance(), [dirPath, retentionDays]() {
            Logger::purgeOldLogs(dirPath, retentionDays);
        });
    } else {
        Logger::purgeOldLogs(dirPath, retentionDays);
    }
}

int Logger::purgeOldLogs(const QString& logDirPath, int retentionDays) {
    if (logDirPath.isEmpty()) return 0;
    QDir dir(logDirPath);
    if (!dir.exists()) return 0;

    QStringList filters;
    filters << "okidia-*.log" << "okidia-*.log.*";
    // QDir::NoSort kullanılarak gereksiz thread_local QCollator / QSystemLocale tahsisi önlenir
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::NoSort);

    int deletedCount = 0;
    QDateTime now = QDateTime::currentDateTime();
    QDate today = now.date();

    static const QRegularExpression dateRx("(\\d{4}-\\d{2}-\\d{2})");

    QString activePath;
    {
        QMutexLocker lock(&s_mutex);
        activePath = s_logFilePath;
    }

    for (const QFileInfo& fi : files) {
        if (!activePath.isEmpty() && fi.absoluteFilePath() == activePath) {
            continue; // Aktif dosyayı asla silme
        }

        bool shouldDelete = false;

        auto match = dateRx.match(fi.fileName());
        if (match.hasMatch()) {
            QDate logDate = QDate::fromString(match.captured(1), "yyyy-MM-dd");
            if (logDate.isValid() && logDate.daysTo(today) > retentionDays) {
                shouldDelete = true;
            }
        }

        if (!shouldDelete && fi.lastModified().daysTo(now) > retentionDays) {
            shouldDelete = true;
        }

        if (shouldDelete) {
            if (QFile::remove(fi.absoluteFilePath())) {
                deletedCount++;
                std::cout << "[Logger] Eski log dosyası temizlendi ("
                          << retentionDays << " gün kuralı): "
                          << fi.fileName().toStdString() << std::endl;
            }
        }
    }

    return deletedCount;
}

bool Logger::clearCurrentLog() {
    QMutexLocker lock(&s_mutex);
    if (s_logFilePath.isEmpty()) return false;

    if (s_logFile.isOpen()) {
        s_logFile.close();
    }

    if (!s_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (!s_logFile.open(QIODevice::Append | QIODevice::Text)) {
            std::cerr << "[Logger] Log dosyasi yeniden acilamadi: "
                      << s_logFilePath.toUtf8().constData() << std::endl;
        }
        return false;
    }

    QTextStream out(&s_logFile);
    out << "========== OKIDIA log temizlendi: "
        << OkidiaUtils::formatDateTime()
        << " ==========\n";
    s_logFile.flush();
    s_logFile.close();

    if (!s_logFile.open(QIODevice::Append | QIODevice::Text)) {
        std::cerr << "[Logger] Log dosyasi temizleme sonrasi acilamadi: "
                  << s_logFilePath.toUtf8().constData() << std::endl;
    }
    return true;
}
