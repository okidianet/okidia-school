// master/ui/SettingsDialog/SettingsDialog.h
#pragma once

#include <QDialog>
#include <QSettings>
#include <QMediaPlayer>
#include <QAudioOutput>
#include "../../common/OkidiaConfig.h"

QT_BEGIN_NAMESPACE
namespace Ui { class SettingsDialog; }
QT_END_NAMESPACE

struct AppSettings {
    QString brokerName      = "OKIDIA-Broker";
    int     brokerPort      = 11200;
    QString remoteBrokerHost= "";
    bool    autoReconnect   = true;
    int     auditRetention  = 30;
    QString backupDir       = "";
    int     backupRetention = 7;
    bool    autoBackupIncludeChat = false;
    QString autoBackupPassword = "";
    int     languageIndex   = 0;
    int     idleTimeoutMinutes = 15;  // 0 = Devre dışı, varsayılan 15 dk
    int     maxSessionHours    = 8;   // 0 = Devre dışı, varsayılan 8 saat
    int     smartboardLockPolicy = 0; // 0 = Klasik Ekran Perdesi, 1 = Dinamik QR & Dokunmatik PIN
    QString schoolName      = "";
    QString schoolLogoPath  = "";

    // Zil ve Ders Saatleri
    bool    bellSmartboardEnabled = false;
    bool    bellStudentPcEnabled = false;
    bool    bellEnabled          = false;
    bool    bellAutoLockRecess   = false;
    bool    bellAutoUnlockTeacher = false;
    bool    recessLogoutStudentPCs = false;
    bool    disableSwitchUserStudentPCs = false;
    bool    usbProtectionEnabled = false;
    int     bellGracePeriodSec   = 30;
    int     lastBellLockBehavior = 0; // 0: Akıllı Hibrit, 1: Kurs/Etüt Serbest, 2: Her Zaman Kilitle
    QString bellStudentPath      = "";
    QString bellTeacherPath      = "";
    QString bellRecessPath       = "";
    QList<OkidiaConfig::LessonPeriod> schedule;

    // Öğretim Biçimi ve Enerji Tasarrufu
    int     educationType        = 0;  // 0: Normal/Tam Gün, 1: İkili Eğitim
    int     dailyLessonCount     = 8;  // 6, 7, 8 saat vb.
    bool    autoShutdown         = true;
    QString autoShutdownTime     = "17:30";
    bool    shutdownSmartboards  = true;
    bool    shutdownStudentPCs   = true;

    static AppSettings load();
    void save() const;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const QString& userRole, QWidget* parent = nullptr);
    ~SettingsDialog();

    AppSettings settings() const;

signals:
    void settingsApplied(const AppSettings& s);
    void backupRequested(const QString& targetDir, const QString& password, bool includeAudit, bool includeChat);
    void restoreRequested(const QString& filePath, const QString& password);
    void uninstallPasswordChanged(const QString& hash);

private slots:
    void onCategoryChanged(int row);
    void onApply();
    void onOk();
    void onBrowseBackup();
    void onBackupNow();
    void onRestore();
    void onChangeUninstallPassword();
    void onBrowseLogo();
    void onClearLogo();

    // Zil & Ders Saatleri Slotları
    void onBrowseBellStudent();
    void onBrowseBellTeacher();
    void onBrowseBellRecess();
    void onPlayBell(const QString& path);
    void onAddLesson();
    void onRemoveLesson();
    void onDefaultSchedule();
    void onToggleScheduleEdit();

protected:
    void hideEvent(QHideEvent* event) override;

private:
    Ui::SettingsDialog* ui;
    QString m_userRole;
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
    bool m_scheduleEditable = false;

    void loadToUi(const AppSettings& s);
    AppSettings readFromUi() const;
    void updateLogoPreview(const QString& path);
    void populateScheduleTable(const QList<OkidiaConfig::LessonPeriod>& list);
    QList<OkidiaConfig::LessonPeriod> readScheduleTable() const;
    void updateScheduleEditState();
    bool validateSchedule();
};
