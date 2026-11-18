// master/ui/SettingsDialog/SettingsDialog.cpp
#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"

#include <QSettings>
#include <QFileDialog>
#include <QHideEvent>
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/utils/Logger.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QApplication>
#include <QFormLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QTimeEdit>
#include <QHeaderView>
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/crypto/BackupPackage.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../../common/OkidiaConfig.h"
#include "../UninstallPasswordDialog/UninstallPasswordDialog.h"

// ---------------------------------------------------------------------------
// AppSettings
// ---------------------------------------------------------------------------
AppSettings AppSettings::load() {
    QSettings s("okidia", "master");
    AppSettings cfg;
    cfg.brokerName       = s.value("net/brokerName",       s.value("net/serverBrokerName", s.value("net/mdnsName", OkidiaConfig::instance().brokerName()))).toString();
    cfg.brokerPort       = s.value("net/brokerPort",       s.value("net/serverBrokerPort", OkidiaConfig::instance().brokerPort())).toInt();
    cfg.remoteBrokerHost = s.value("net/remoteBrokerHost", cfg.remoteBrokerHost).toString();
    cfg.autoReconnect    = s.value("net/autoReconnect",    cfg.autoReconnect).toBool();
    cfg.auditRetention   = s.value("sec/auditRetention",   cfg.auditRetention).toInt();
    
    QString defaultBackupDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/OKIDIA-Yedekler";
    cfg.backupDir        = s.value("backup/dir", defaultBackupDir).toString();
    if (cfg.backupDir.isEmpty()) {
        cfg.backupDir = defaultBackupDir;
    }
    cfg.backupRetention  = s.value("backup/retentionCount", cfg.backupRetention).toInt();
    cfg.autoBackupIncludeChat = s.value("backup/autoIncludeChat", cfg.autoBackupIncludeChat).toBool();
    cfg.autoBackupPassword = s.value("backup/autoPassword", "").toString();
    if (cfg.autoBackupPassword.isEmpty()) {
        cfg.autoBackupPassword = OkidiaDatabase::instance().metaValue("backup_password");
    }

    cfg.languageIndex    = s.value("ui/language",          cfg.languageIndex).toInt();
    cfg.idleTimeoutMinutes = s.value("sec/idleTimeoutMinutes", cfg.idleTimeoutMinutes).toInt();
    cfg.maxSessionHours    = s.value("sec/maxSessionHours",    cfg.maxSessionHours).toInt();
    cfg.smartboardLockPolicy = s.value("smartboard/lockPolicy", cfg.smartboardLockPolicy).toInt();
    cfg.schoolName       = s.value("school/name",          cfg.schoolName).toString();
    cfg.schoolLogoPath   = s.value("school/logoPath",      cfg.schoolLogoPath).toString();

    cfg.bellSmartboardEnabled = OkidiaConfig::isBellSmartboardEnabled();
    cfg.bellStudentPcEnabled  = OkidiaConfig::isBellStudentPcEnabled();
    cfg.bellEnabled           = OkidiaConfig::isBellEnabled();
    cfg.bellAutoLockRecess = OkidiaConfig::isBellAutoLockRecess();
    cfg.bellAutoUnlockTeacher = OkidiaConfig::isBellAutoUnlockTeacher();
    cfg.recessLogoutStudentPCs = OkidiaConfig::isRecessLogoutStudentPCs();
    cfg.disableSwitchUserStudentPCs = OkidiaConfig::isDisableSwitchUserStudentPCs();
    cfg.usbProtectionEnabled = OkidiaConfig::isUsbProtectionEnabled();
    cfg.bellGracePeriodSec = OkidiaConfig::bellGracePeriodSec();
    cfg.lastBellLockBehavior = OkidiaConfig::lastBellLockBehavior();
    cfg.bellStudentPath    = s.value("school/bellStudentPath", cfg.bellStudentPath).toString();
    cfg.bellTeacherPath    = s.value("school/bellTeacherPath", cfg.bellTeacherPath).toString();
    cfg.bellRecessPath     = s.value("school/bellRecessPath",  cfg.bellRecessPath).toString();
    cfg.schedule           = OkidiaConfig::schoolSchedule();

    cfg.educationType        = OkidiaConfig::schoolEducationType();
    cfg.dailyLessonCount     = OkidiaConfig::schoolDailyLessonCount();
    cfg.autoShutdown         = OkidiaConfig::isAutoShutdownEnabled();
    cfg.autoShutdownTime     = OkidiaConfig::autoShutdownTime();
    cfg.shutdownSmartboards  = OkidiaConfig::autoShutdownSmartboards();
    cfg.shutdownStudentPCs   = OkidiaConfig::autoShutdownStudentPCs();

    return cfg;
}

void AppSettings::save() const {
    QSettings s("okidia", "master");
    s.setValue("net/brokerName",       brokerName);
    s.setValue("net/brokerPort",       brokerPort);
    // Geriye dönük uyumluluk için eski anahtarları da aynı değerle güncelle
    s.setValue("net/mdnsName",         brokerName);
    s.setValue("net/serverBrokerName", brokerName);
    s.setValue("net/serverBrokerPort", brokerPort);
    s.setValue("net/remoteBrokerHost", remoteBrokerHost);
    s.setValue("net/autoReconnect",    autoReconnect);
    s.setValue("sec/auditRetention",   auditRetention);
    s.setValue("sec/idleTimeoutMinutes", idleTimeoutMinutes);
    s.setValue("sec/maxSessionHours",    maxSessionHours);
    s.setValue("backup/dir",           backupDir);
    s.setValue("backup/retentionCount", backupRetention);
    s.setValue("backup/autoIncludeChat", autoBackupIncludeChat);
    s.setValue("backup/autoPassword",  autoBackupPassword);
    s.setValue("ui/language",          languageIndex);
    s.setValue("smartboard/lockPolicy", smartboardLockPolicy);
    s.setValue("school/name",          schoolName);
    s.setValue("school/logoPath",      schoolLogoPath);

    s.setValue("school/bellStudentPath", bellStudentPath);
    s.setValue("school/bellTeacherPath", bellTeacherPath);
    s.setValue("school/bellRecessPath",  bellRecessPath);
    OkidiaConfig::saveSchoolSchedule(schedule, bellSmartboardEnabled, bellStudentPcEnabled, bellAutoLockRecess, bellAutoUnlockTeacher, recessLogoutStudentPCs, disableSwitchUserStudentPCs, usbProtectionEnabled, bellGracePeriodSec, lastBellLockBehavior);
    OkidiaConfig::saveSchoolPowerAndType(educationType, dailyLessonCount, autoShutdown, autoShutdownTime, shutdownSmartboards, shutdownStudentPCs);
}

// ---------------------------------------------------------------------------
// SettingsDialog
// ---------------------------------------------------------------------------
SettingsDialog::SettingsDialog(const QString& userRole, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
    , m_userRole(userRole) {
    ui->setupUi(this);
    setWindowTitle(tr("Ayarlar — OKIDIA"));

    bool isAdmin = (userRole == "YM");
    if (!isAdmin) {
        // YM olmayan kullanıcılar için Yönetici sekmelerini tamamen gizle
        if (ui->categoryList->count() > 3) {
            ui->categoryList->item(3)->setHidden(true); // Okul ve Kurum Ayarları
        }
        if (ui->categoryList->count() > 2) {
            ui->categoryList->item(2)->setHidden(true); // Sunucu Yapılandırması
        }
        if (ui->categoryList->count() > 1) {
            ui->categoryList->item(1)->setHidden(true); // Yedekleme & Güvenlik
        }
        // Oturum güvenliği (Boşta kalma ekran kilidi / Azami oturum süresi) tüm roller için
        // değiştirilebilir olmalıdır; YM olmayan kullanıcılarda Genel sekmesine taşı.
        if (ui->grpSessionSecurity && ui->generalVBox) {
            ui->grpSessionSecurity->setTitle(tr("Oturum Güvenliği"));
            int insertIdx = qMax(0, ui->generalVBox->count() - 1);
            ui->generalVBox->insertWidget(insertIdx, ui->grpSessionSecurity);
        }
        ui->btnBackupNow->setEnabled(false);
        ui->btnRestore->setEnabled(false);
        ui->btnBrowseBackup->setEnabled(false);
        ui->backupDirEdit->setReadOnly(true);
        ui->auditRetentionSpin->setEnabled(false);
        ui->backupRetentionSpin->setEnabled(false);
        ui->autoBackupChatCheck->setEnabled(false);
        ui->autoBackupPasswordEdit->setEnabled(false);
        ui->btnToggleAutoBackupPw->setEnabled(false);
        ui->grpUninstallProtection->setVisible(false);
        ui->serverBrokerNameEdit->setEnabled(false);
        ui->serverBrokerPortSpin->setEnabled(false);
        ui->btnBackupNow->setToolTip(tr("Bu işlem için Yönetici (YM) yetkisi gereklidir."));
        ui->btnRestore->setToolTip(tr("Bu işlem için Yönetici (YM) yetkisi gereklidir."));
        ui->schoolNameEdit->setEnabled(false);
        ui->smartboardLockPolicyCombo->setEnabled(false);
        ui->btnBrowseLogo->setEnabled(false);
        ui->btnClearLogo->setEnabled(false);
        ui->grpBellSchedule->setEnabled(false);
        ui->grpPowerSaving->setEnabled(false);
        ui->btnToggleScheduleEdit->setEnabled(false);
    } else {
        ui->btnBackupNow->setIcon(ThemeManager::icon(":/ui/shield.svg"));
        ui->btnRestore->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
        ui->btnChangeUninstallPassword->setIcon(ThemeManager::icon(":/ui/shield.svg"));
        ui->btnBrowseLogo->setIcon(ThemeManager::icon(":/ui/folder.svg"));
        ui->btnClearLogo->setIcon(ThemeManager::icon(":/ui/trash.svg"));

        ui->btnToggleScheduleEdit->setIcon(ThemeManager::icon(":/ui/lock.svg"));
        connect(ui->btnToggleScheduleEdit, &QPushButton::clicked, this, &SettingsDialog::onToggleScheduleEdit);

        ui->btnBrowseBellStudent->setIcon(ThemeManager::icon(":/ui/folder.svg"));
        ui->btnPlayBellStudent->setIcon(ThemeManager::icon(":/ui/play.svg"));
        ui->btnBrowseBellTeacher->setIcon(ThemeManager::icon(":/ui/folder.svg"));
        ui->btnPlayBellTeacher->setIcon(ThemeManager::icon(":/ui/play.svg"));
        ui->btnBrowseBellRecess->setIcon(ThemeManager::icon(":/ui/folder.svg"));
        ui->btnPlayBellRecess->setIcon(ThemeManager::icon(":/ui/play.svg"));
        ui->btnAddLesson->setIcon(ThemeManager::icon(":/ui/add.svg"));
        ui->btnRemoveLesson->setIcon(ThemeManager::icon(":/ui/trash.svg"));
        ui->btnDefaultSchedule->setIcon(ThemeManager::icon(":/ui/refresh.svg"));

        bool isDefault = OkidiaDatabase::instance().isDefaultUninstallPassword();
        if (isDefault) {
            ui->lblUninstallStatus->setText(tr("Durum: Varsayılan ('www.okidia.net') — Değiştirilmesi önerilir"));
            ui->lblUninstallStatus->setStyleSheet(QString("color: %1; font-weight: bold;").arg(ThemeManager::instance().warningColor().name()));
        } else {
            ui->lblUninstallStatus->setText(tr("Durum: Özel koruma parolası tanımlı"));
            ui->lblUninstallStatus->setStyleSheet(QString("color: %1; font-weight: bold;").arg(ThemeManager::instance().successColor().name()));
        }
        connect(ui->btnChangeUninstallPassword, &QPushButton::clicked, this, &SettingsDialog::onChangeUninstallPassword);
        connect(ui->btnBrowseLogo, &QPushButton::clicked, this, &SettingsDialog::onBrowseLogo);
        connect(ui->btnClearLogo,  &QPushButton::clicked, this, &SettingsDialog::onClearLogo);

        connect(ui->btnBrowseBellStudent, &QPushButton::clicked, this, &SettingsDialog::onBrowseBellStudent);
        connect(ui->btnPlayBellStudent, &QPushButton::clicked, this, [this]() { onPlayBell(ui->bellStudentEdit->text()); });
        connect(ui->btnBrowseBellTeacher, &QPushButton::clicked, this, &SettingsDialog::onBrowseBellTeacher);
        connect(ui->btnPlayBellTeacher, &QPushButton::clicked, this, [this]() { onPlayBell(ui->bellTeacherEdit->text()); });
        connect(ui->btnBrowseBellRecess, &QPushButton::clicked, this, &SettingsDialog::onBrowseBellRecess);
        connect(ui->btnPlayBellRecess, &QPushButton::clicked, this, [this]() { onPlayBell(ui->bellRecessEdit->text()); });
        connect(ui->btnAddLesson, &QPushButton::clicked, this, &SettingsDialog::onAddLesson);
        connect(ui->btnRemoveLesson, &QPushButton::clicked, this, &SettingsDialog::onRemoveLesson);
        connect(ui->btnDefaultSchedule, &QPushButton::clicked, this, &SettingsDialog::onDefaultSchedule);
        connect(ui->comboEducationType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            onDefaultSchedule();
        });
        connect(ui->spinDailyLessonCount, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
            onDefaultSchedule();
        });
    }

    // Tablo ayarları
    ui->tblSchedule->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tblSchedule->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tblSchedule->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui->tblSchedule->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    // Ses çalar
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0f);
    m_player->setAudioOutput(m_audioOutput);

    connect(m_player, &QMediaPlayer::errorOccurred, this, [](QMediaPlayer::Error err, const QString& errStr) {
        Logger::warning(QObject::tr("SettingsDialog: Ses çalma hatası: ") + errStr);
    });

    // Kategori seçimi → sayfa değiştir
    connect(ui->categoryList, &QListWidget::currentRowChanged,
            this, &SettingsDialog::onCategoryChanged);
    ui->categoryList->setCurrentRow(0);

    // Butonlar
    connect(ui->btnOk,     &QPushButton::clicked, this, &SettingsDialog::onOk);
    connect(ui->btnApply,  &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(ui->btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    connect(ui->btnBrowseBackup, &QPushButton::clicked, this, &SettingsDialog::onBrowseBackup);
    connect(ui->btnBackupNow,    &QPushButton::clicked, this, &SettingsDialog::onBackupNow);
    connect(ui->btnRestore,      &QPushButton::clicked, this, &SettingsDialog::onRestore);

    connect(ui->btnToggleAutoBackupPw, &QPushButton::clicked, this, [this]() {
        if (ui->autoBackupPasswordEdit->echoMode() == QLineEdit::Password) {
            ui->autoBackupPasswordEdit->setEchoMode(QLineEdit::Normal);
            ui->btnToggleAutoBackupPw->setText(tr("Gizle"));
        } else {
            ui->autoBackupPasswordEdit->setEchoMode(QLineEdit::Password);
            ui->btnToggleAutoBackupPw->setText(tr("Göster"));
        }
    });

    connect(ui->autoBackupPasswordEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.trimmed().isEmpty()) {
            ui->lblAutoBackupPasswordStatus->setProperty("status", "warning");
            ui->lblAutoBackupPasswordStatus->setText(tr("Otomatik yedekleriniz sistem kurulum kodu ile korunuyor. Felaket anında harici kurtarmayı kolaylaştırmak için özel bir yedek parolası belirleyebilirsiniz."));
            ui->lblAutoBackupPasswordStatus->setStyleSheet(QString("color: %1; font-size: 11px;").arg(ThemeManager::instance().warningColor().name()));
        } else {
            ui->lblAutoBackupPasswordStatus->setProperty("status", "info");
            ui->lblAutoBackupPasswordStatus->setText(tr("Otomatik sistem yedekleri belirlenen özel parola ile şifrelenmektedir."));
            ui->lblAutoBackupPasswordStatus->setStyleSheet(QString("color: %1; font-size: 11px;").arg(ThemeManager::instance().successColor().name()));
        }
    });

    // Mevcut ayarları yükle
    loadToUi(AppSettings::load());

    // Ders saatleri tablosu başlangıçta salt okunur / kilitlidir
    m_scheduleEditable = false;
    updateScheduleEditState();
    validateSchedule();

    // Pencere geometrisini geri yükle (önceden kullanıcı tarafından ayarlanmışsa)
    QSettings s("okidia", "master");
    QByteArray geo = s.value("ui/settingsDialogGeometry").toByteArray();
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
    }
}

SettingsDialog::~SettingsDialog() {
    QSettings s("okidia", "master");
    s.setValue("ui/settingsDialogGeometry", saveGeometry());
    if (m_player) m_player->stop();
    delete ui;
}

void SettingsDialog::hideEvent(QHideEvent* event) {
    QSettings s("okidia", "master");
    s.setValue("ui/settingsDialogGeometry", saveGeometry());
    QDialog::hideEvent(event);
}

void SettingsDialog::onCategoryChanged(int row) {
    ui->stackedWidget->setCurrentIndex(row);
}

AppSettings SettingsDialog::settings() const {
    return readFromUi();
}

void SettingsDialog::loadToUi(const AppSettings& s) {
    ui->serverBrokerNameEdit->setText(!s.brokerName.isEmpty() ? s.brokerName : "OKIDIA-Broker");
    ui->serverBrokerPortSpin->setValue(s.brokerPort > 0 ? s.brokerPort : 11200);
    ui->remoteBrokerEdit->setText(s.remoteBrokerHost);
    ui->autoReconnectCheck->setChecked(s.autoReconnect);

    ui->auditRetentionSpin->setValue(s.auditRetention);
    ui->idleTimeoutSpin->setValue(s.idleTimeoutMinutes);
    ui->maxSessionSpin->setValue(s.maxSessionHours);
    ui->backupDirEdit->setText(s.backupDir);
    ui->backupRetentionSpin->setValue(s.backupRetention > 0 ? s.backupRetention : 7);
    ui->autoBackupChatCheck->setChecked(s.autoBackupIncludeChat);
    ui->autoBackupPasswordEdit->setText(s.autoBackupPassword);
    if (s.autoBackupPassword.isEmpty()) {
        ui->lblAutoBackupPasswordStatus->setProperty("status", "warning");
        ui->lblAutoBackupPasswordStatus->setText(tr("Otomatik yedekleriniz sistem kurulum kodu ile korunuyor. Felaket anında harici kurtarmayı kolaylaştırmak için özel bir yedek parolası belirleyebilirsiniz."));
        ui->lblAutoBackupPasswordStatus->setStyleSheet(QString("color: %1; font-size: 11px;").arg(ThemeManager::instance().warningColor().name()));
    } else {
        ui->lblAutoBackupPasswordStatus->setProperty("status", "info");
        ui->lblAutoBackupPasswordStatus->setText(tr("Otomatik sistem yedekleri belirlenen özel parola ile şifrelenmektedir."));
        ui->lblAutoBackupPasswordStatus->setStyleSheet(QString("color: %1; font-size: 11px;").arg(ThemeManager::instance().successColor().name()));
    }

    ui->languageCombo->setCurrentIndex(s.languageIndex);
    ui->smartboardLockPolicyCombo->setCurrentIndex(s.smartboardLockPolicy);
    ui->schoolNameEdit->setText(s.schoolName);
    ui->schoolLogoPathEdit->setText(s.schoolLogoPath);
    updateLogoPreview(s.schoolLogoPath);

    ui->comboEducationType->setCurrentIndex(s.educationType);
    ui->spinDailyLessonCount->setValue(s.dailyLessonCount);

    ui->chkAutoShutdown->setChecked(s.autoShutdown);
    QTime sTime = QTime::fromString(s.autoShutdownTime, "HH:mm");
    if (!sTime.isValid()) sTime = QTime(17, 30);
    ui->timeAutoShutdown->setTime(sTime);
    ui->chkShutdownSmartboards->setChecked(s.shutdownSmartboards);
    ui->chkShutdownStudentPCs->setChecked(s.shutdownStudentPCs);

    ui->chkBellSmartboards->setChecked(s.bellSmartboardEnabled);
    ui->chkBellStudentPCs->setChecked(s.bellStudentPcEnabled);
    ui->chkAutoLockRecess->setChecked(s.bellAutoLockRecess);
    ui->chkAutoUnlockTeacher->setChecked(s.bellAutoUnlockTeacher);
    ui->chkRecessLogoutStudentPCs->setChecked(s.recessLogoutStudentPCs);
    ui->chkDisableSwitchUserStudentPCs->setChecked(s.disableSwitchUserStudentPCs);
    ui->chkUsbProtection->setChecked(s.usbProtectionEnabled);
    ui->comboLastBellBehavior->setCurrentIndex(qBound(0, s.lastBellLockBehavior, 2));
    ui->bellStudentEdit->setText(s.bellStudentPath);
    ui->bellTeacherEdit->setText(s.bellTeacherPath);
    ui->bellRecessEdit->setText(s.bellRecessPath);
    populateScheduleTable(s.schedule);
}

AppSettings SettingsDialog::readFromUi() const {
    AppSettings s;
    s.brokerName       = ui->serverBrokerNameEdit->text().trimmed();
    if (s.brokerName.isEmpty()) {
        s.brokerName = "OKIDIA-Broker";
    }
    s.brokerPort       = ui->serverBrokerPortSpin->value();
    s.remoteBrokerHost = ui->remoteBrokerEdit->text().trimmed();
    if (s.remoteBrokerHost.contains(':')) {
        QStringList parts = s.remoteBrokerHost.split(':');
        s.remoteBrokerHost = parts[0].trimmed();
        if (parts.size() > 1 && !parts[1].trimmed().isEmpty()) {
            bool ok = false;
            int p = parts[1].trimmed().toInt(&ok);
            if (ok && p > 0 && p <= 65535) {
                s.brokerPort = p;
            }
        }
    }
    s.autoReconnect    = ui->autoReconnectCheck->isChecked();
    s.auditRetention   = ui->auditRetentionSpin->value();
    s.idleTimeoutMinutes = ui->idleTimeoutSpin->value();
    s.maxSessionHours    = ui->maxSessionSpin->value();
    s.backupDir        = ui->backupDirEdit->text().trimmed();
    s.backupRetention  = ui->backupRetentionSpin->value();
    s.autoBackupIncludeChat = ui->autoBackupChatCheck->isChecked();
    s.autoBackupPassword = ui->autoBackupPasswordEdit->text().trimmed();
    s.languageIndex    = ui->languageCombo->currentIndex();
    s.smartboardLockPolicy = ui->smartboardLockPolicyCombo->currentIndex();
    s.schoolName       = ui->schoolNameEdit->text().trimmed();
    s.schoolLogoPath   = ui->schoolLogoPathEdit->text().trimmed();

    s.educationType        = ui->comboEducationType->currentIndex();
    s.dailyLessonCount     = ui->spinDailyLessonCount->value();
    s.autoShutdown         = ui->chkAutoShutdown->isChecked();
    s.autoShutdownTime     = ui->timeAutoShutdown->time().toString("HH:mm");
    s.shutdownSmartboards  = ui->chkShutdownSmartboards->isChecked();
    s.shutdownStudentPCs   = ui->chkShutdownStudentPCs->isChecked();

    s.bellSmartboardEnabled = ui->chkBellSmartboards->isChecked();
    s.bellStudentPcEnabled  = ui->chkBellStudentPCs->isChecked();
    s.bellEnabled           = s.bellSmartboardEnabled || s.bellStudentPcEnabled;
    s.bellAutoLockRecess    = ui->chkAutoLockRecess->isChecked();
    s.bellAutoUnlockTeacher = ui->chkAutoUnlockTeacher->isChecked();
    s.recessLogoutStudentPCs = ui->chkRecessLogoutStudentPCs->isChecked();
    s.disableSwitchUserStudentPCs = ui->chkDisableSwitchUserStudentPCs->isChecked();
    s.usbProtectionEnabled = ui->chkUsbProtection->isChecked();
    s.lastBellLockBehavior = ui->comboLastBellBehavior->currentIndex();
    s.bellStudentPath    = ui->bellStudentEdit->text().trimmed();
    s.bellTeacherPath    = ui->bellTeacherEdit->text().trimmed();
    s.bellRecessPath     = ui->bellRecessEdit->text().trimmed();
    s.schedule           = readScheduleTable();

    return s;
}

void SettingsDialog::onApply() {
    if (!validateSchedule()) {
        QMessageBox::warning(this, tr("Zaman Çakışması"), tr("Ders saatleri tablosunda çakışan veya hatalı saatler bulunmaktadır. Lütfen kaydetmeden önce işaretli hataları düzeltin."));
        return;
    }

    AppSettings currentSaved = AppSettings::load();
    AppSettings s = readFromUi();

    if (m_userRole == "YM") {
        bool portChanged = (s.brokerPort != currentSaved.brokerPort);
        bool nameChanged = (s.brokerName != currentSaved.brokerName);

        if (portChanged || nameChanged) {
            auto res = QMessageBox::question(this, tr("Sunucu Yapılandırma Değişikliği"),
                tr("Sunucu adını ve/veya portunu güncellemek üzeresiniz:\n"
                   "• Sunucu Adı: %1\n"
                   "• İletişim Portu: %2\n\n"
                   "Bu işlem hem yerel yapılandırmayı güncelleyecek hem de bağlı olan Sunucu (Broker) servisine iletilerek servisi bu yeni ayarlarla yeniden başlatacaktır.\n\n"
                   "Devam etmek istiyor musunuz?").arg(s.brokerName).arg(s.brokerPort),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (res != QMessageBox::Yes) {
                ui->serverBrokerPortSpin->setValue(currentSaved.brokerPort > 0 ? currentSaved.brokerPort : 11200);
                ui->serverBrokerNameEdit->setText(currentSaved.brokerName);
                s.brokerPort = currentSaved.brokerPort;
                s.brokerName = currentSaved.brokerName;
            }
        }
    }

    s.save();
    emit settingsApplied(s);
    if (s.languageIndex != currentSaved.languageIndex) {
        QMessageBox::information(this, tr("Dil Değişikliği"),
            tr("Dil ayarı değiştirildi. Değişikliğin uygulanması için uygulamanın yeniden başlatılması gerekmektedir."));
    }
}

void SettingsDialog::onOk() {
    if (!validateSchedule()) {
        QMessageBox::warning(this, tr("Zaman Çakışması"), tr("Ders saatleri tablosunda çakışan veya hatalı saatler bulunmaktadır. Lütfen kaydetmeden önce işaretli hataları düzeltin."));
        return;
    }
    onApply();
    accept();
}

void SettingsDialog::onBrowseBackup() {
    QString dir = OkidiaUtils::getExistingDirectory(
        this, tr("Yedekleme Dizini"), "backup_dir", QFileDialog::ShowDirsOnly, ui->backupDirEdit->text());
    if (!dir.isEmpty()) ui->backupDirEdit->setText(dir);
}

void SettingsDialog::onBackupNow() {
    if (m_userRole != "YM") {
        QMessageBox::warning(this, tr("Yetkisiz İşlem"), tr("Yalnızca Yönetici (YM) sistem yedeği alabilir."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Sistem Yedeği Al"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(380);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setSpacing(12);

    auto* lblDesc = new QLabel(tr("Alınacak sistem yedeği için parola belirleyin ve yedekleme kapsamını seçin:"), &dlg);
    lblDesc->setWordWrap(true);
    layout->addWidget(lblDesc);

    auto* form = new QFormLayout();
    form->setSpacing(8);
    auto* pwdEdit = new QLineEdit(&dlg);
    pwdEdit->setEchoMode(QLineEdit::Password);
    auto* confirmEdit = new QLineEdit(&dlg);
    confirmEdit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Yedek Parolası:"), pwdEdit);
    form->addRow(tr("Parola Tekrarı:"), confirmEdit);
    layout->addLayout(form);

    auto* grpScope = new QGroupBox(tr("Yedeklenecek Veritabanları"), &dlg);
    auto* scopeLay = new QVBoxLayout(grpScope);
    scopeLay->setSpacing(6);

    auto* chkCore = new QCheckBox(tr("Çekirdek Sistem (okidia.db) — Zorunlu"), grpScope);
    chkCore->setChecked(true);
    chkCore->setEnabled(false);

    auto* chkAudit = new QCheckBox(tr("Adli Denetim Günlükleri (okidia_audit.db)"), grpScope);
    chkAudit->setChecked(true);

    auto* chkChat = new QCheckBox(tr("Sohbet Geçmişi (okidia_chat.db)"), grpScope);
    chkChat->setChecked(false);

    scopeLay->addWidget(chkCore);
    scopeLay->addWidget(chkAudit);
    scopeLay->addWidget(chkChat);
    layout->addWidget(grpScope);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btnBox->button(QDialogButtonBox::Ok)->setText(tr("Yedek Al"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    layout->addWidget(btnBox);

    connect(btnBox, &QDialogButtonBox::accepted, [&]() {
        if (pwdEdit->text().isEmpty()) {
            QMessageBox::warning(&dlg, tr("Hata"), tr("Lütfen bir yedekleme parolası girin."));
            return;
        }
        if (pwdEdit->text().length() < 4) {
            QMessageBox::warning(&dlg, tr("Hata"), tr("Yedekleme parolası en az 4 karakter olmalıdır."));
            return;
        }
        if (pwdEdit->text() != confirmEdit->text()) {
            QMessageBox::warning(&dlg, tr("Hata"), tr("Girilen parolalar eşleşmiyor!"));
            return;
        }
        dlg.accept();
    });
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString targetDir = ui->backupDirEdit->text().trimmed();
    if (targetDir.isEmpty()) {
        targetDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/OKIDIA-Yedekler";
    }

    emit backupRequested(targetDir, pwdEdit->text(), chkAudit->isChecked(), chkChat->isChecked());
}

void SettingsDialog::onRestore() {
    if (m_userRole != "YM") {
        QMessageBox::warning(this, tr("Yetkisiz İşlem"), tr("Yalnızca Yönetici (YM) sistem yedeğini geri yükleyebilir."));
        return;
    }

    QString defaultDir = ui->backupDirEdit->text().trimmed();
    QString path = OkidiaUtils::getOpenFileName(
        this, tr("OKIDIA Sistem Yedeği Seç"), "backup_restore",
        tr("OKIDIA Yedek (*.okbak);;Tüm Dosyalar (*)"),
        nullptr, QFileDialog::Options(), defaultDir);
    if (path.isEmpty()) return;

    if (!BackupPackage::verifyHeaderFile(path)) {
        QMessageBox::critical(this, tr("Geçersiz Dosya"), tr("Seçilen dosya geçerli bir OKIDIA (.okbak) yedek paketi değil."));
        return;
    }

    bool ok = false;
    QString pwd = QInputDialog::getText(
        this, tr("Yedek Şifresi"),
        tr("Yedek dosyasını açmak için parolayı girin:"),
        QLineEdit::Password, QString(), &ok);

    if (!ok || pwd.isEmpty()) return;

    BackupContent content;
    QString err;
    if (!BackupPackage::unpackFromFile(path, pwd, content, &err)) {
        QMessageBox::critical(this, tr("Şifre Çözme Hatası"), err);
        return;
    }

    QString dateStr = content.meta.timestamp > 0 
        ? QDateTime::fromSecsSinceEpoch(content.meta.timestamp).toString("dd.MM.yyyy HH:mm") 
        : tr("Bilinmiyor");
    QString creatorStr = content.meta.creator.isEmpty() ? tr("Belirtilmemiş") : content.meta.creator;

    QString contentSummary = QString(" • %1\n").arg(tr("Çekirdek Sistem Veritabanı (okidia.db)"));
    if (!content.auditData.isEmpty()) {
        contentSummary += QString(" • %1\n").arg(tr("Adli Denetim Veritabanı (okidia_audit.db)"));
    }
    if (!content.chatData.isEmpty()) {
        contentSummary += QString(" • %1\n").arg(tr("Sohbet Veritabanı (okidia_chat.db)"));
    }
    contentSummary += QString(" • %1: %2 %3").arg(tr("Güvenlik Sertifikaları"), QString::number(content.certFiles.size()), tr("adet"));

    int ret = QMessageBox::warning(this, tr("Sistem Yedeğini Geri Yükle"),
        tr("Bu işlem sunucudaki mevcut veritabanını ve güvenlik sertifikalarını bu yedekle değiştirecektir.\n\n"
           "Yedek Tarihi: %1\n"
           "Oluşturan: %2\n\n"
           "Yedek Kapsamı:\n%3\n\n"
           "Devam etmek istiyor musunuz?").arg(dateStr).arg(creatorStr).arg(contentSummary),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        emit restoreRequested(path, pwd);
    }
}

void SettingsDialog::onChangeUninstallPassword() {
    UninstallPasswordDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        QString hash = dlg.passwordHash();
        if (!hash.isEmpty()) {
            emit uninstallPasswordChanged(hash);
            bool isDefault = (hash == OkidiaDatabase::DEFAULT_UNINSTALL_PASS_HASH);
            if (isDefault) {
                ui->lblUninstallStatus->setText(tr("Durum: Varsayılan ('www.okidia.net') — Değiştirilmesi önerilir"));
                ui->lblUninstallStatus->setStyleSheet(QString("color: %1; font-weight: bold;").arg(ThemeManager::instance().warningColor().name()));
            } else {
                ui->lblUninstallStatus->setText(tr("Durum: Özel koruma parolası tanımlı"));
                ui->lblUninstallStatus->setStyleSheet(QString("color: %1; font-weight: bold;").arg(ThemeManager::instance().successColor().name()));
            }
        }
    }
}

void SettingsDialog::updateLogoPreview(const QString& path) {
    if (!path.isEmpty() && QFile::exists(path)) {
        QPixmap pm(path);
        if (!pm.isNull()) {
            ui->lblLogoPreview->setPixmap(pm.scaled(ui->lblLogoPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            ui->lblLogoPreview->setText("");
            return;
        }
    }
    ui->lblLogoPreview->setPixmap(QPixmap());
    ui->lblLogoPreview->setText(tr("Logo Yok"));
}

void SettingsDialog::onBrowseLogo() {
    QString filter = tr("Görseller (*.png *.jpg *.jpeg *.svg *.webp)");
    QString file = QFileDialog::getOpenFileName(this, tr("Okul Logosu Seç"), QDir::homePath(), filter);
    if (!file.isEmpty()) {
        ui->schoolLogoPathEdit->setText(file);
        updateLogoPreview(file);
    }
}

void SettingsDialog::onClearLogo() {
    ui->schoolLogoPathEdit->clear();
    updateLogoPreview(QString());
}

namespace {
class WheelBlockFilter : public QObject {
public:
    explicit WheelBlockFilter(QObject* parent = nullptr) : QObject(parent) {}
protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::Wheel) {
            event->ignore();
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
};
} // namespace

static QTimeEdit* createScheduleCellTimeEdit(const QString& timeStr, const QString& defVal) {
    auto* te = new QTimeEdit();
    te->setDisplayFormat("HH:mm");
    te->setAlignment(Qt::AlignCenter);
    te->setFrame(false);
    te->setFocusPolicy(Qt::StrongFocus);
    te->setStyleSheet("QTimeEdit { background: transparent; padding: 2px; }");
    static auto* filter = new WheelBlockFilter(qApp);
    te->installEventFilter(filter);
    QTime t = QTime::fromString(timeStr.trimmed(), "HH:mm");
    if (!t.isValid()) t = QTime::fromString(timeStr.trimmed(), "H:mm");
    if (!t.isValid()) t = QTime::fromString(defVal, "HH:mm");
    te->setTime(t.isValid() ? t : QTime(8, 0));
    return te;
}

void SettingsDialog::populateScheduleTable(const QList<OkidiaConfig::LessonPeriod>& list) {
    ui->tblSchedule->setRowCount(0);
    for (int i = 0; i < list.size(); ++i) {
        const auto& p = list[i];
        int row = ui->tblSchedule->rowCount();
        ui->tblSchedule->insertRow(row);

        auto* itemLesson = new QTableWidgetItem(tr("%1. Ders").arg(row + 1));
        itemLesson->setData(Qt::UserRole, row + 1);
        itemLesson->setTextAlignment(Qt::AlignCenter);
        itemLesson->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

        ui->tblSchedule->setItem(row, 0, itemLesson);

        auto* teStudent = createScheduleCellTimeEdit(p.studentBell, "08:48");
        auto* teTeacher = createScheduleCellTimeEdit(p.teacherBell, "08:50");
        auto* teRecess  = createScheduleCellTimeEdit(p.recessBell,  "09:30");

        connect(teStudent, &QTimeEdit::timeChanged, this, [this](const QTime&) { validateSchedule(); });
        connect(teTeacher, &QTimeEdit::timeChanged, this, [this](const QTime&) { validateSchedule(); });
        connect(teRecess,  &QTimeEdit::timeChanged, this, [this](const QTime&) { validateSchedule(); });

        ui->tblSchedule->setCellWidget(row, 1, teStudent);
        ui->tblSchedule->setCellWidget(row, 2, teTeacher);
        ui->tblSchedule->setCellWidget(row, 3, teRecess);
    }

    updateScheduleEditState();
    validateSchedule();
}

QList<OkidiaConfig::LessonPeriod> SettingsDialog::readScheduleTable() const {
    QList<OkidiaConfig::LessonPeriod> list;
    for (int r = 0; r < ui->tblSchedule->rowCount(); ++r) {
        OkidiaConfig::LessonPeriod p;
        p.lessonNumber = r + 1;

        auto* teStudent = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, 1));
        auto* teTeacher = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, 2));
        auto* teRecess  = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, 3));

        p.studentBell = teStudent ? teStudent->time().toString("HH:mm") : "08:48";
        p.teacherBell = teTeacher ? teTeacher->time().toString("HH:mm") : "08:50";
        p.recessBell  = teRecess  ? teRecess->time().toString("HH:mm")  : "09:30";

        list.append(p);
    }
    return list;
}

void SettingsDialog::onBrowseBellStudent() {
    QString file = QFileDialog::getOpenFileName(this, tr("Öğrenci Giriş Zili Seç"), QDir::homePath(), tr("Ses Dosyaları (*.mp3 *.wav *.ogg)"));
    if (!file.isEmpty()) {
        ui->bellStudentEdit->setText(file);
    }
}

void SettingsDialog::onBrowseBellTeacher() {
    QString file = QFileDialog::getOpenFileName(this, tr("Öğretmen Giriş Zili Seç"), QDir::homePath(), tr("Ses Dosyaları (*.mp3 *.wav *.ogg)"));
    if (!file.isEmpty()) {
        ui->bellTeacherEdit->setText(file);
    }
}

void SettingsDialog::onBrowseBellRecess() {
    QString file = QFileDialog::getOpenFileName(this, tr("Teneffüs / Çıkış Zili Seç"), QDir::homePath(), tr("Ses Dosyaları (*.mp3 *.wav *.ogg)"));
    if (!file.isEmpty()) {
        ui->bellRecessEdit->setText(file);
    }
}

void SettingsDialog::onPlayBell(const QString& path) {
    if (!m_player) return;

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->stop();
        return;
    }

    QString actualPath = path;
    QUrl sourceUrl;
    if (actualPath.isEmpty() || !QFile::exists(actualPath)) {
        QString fname = QFileInfo(actualPath).fileName();
        QString fallbackAsset = OkidiaConfig::assetsDir() + "/" + fname;
        if (QFile::exists(fallbackAsset) && QFileInfo(fallbackAsset).size() > 0) {
            actualPath = fallbackAsset;
            sourceUrl = QUrl::fromLocalFile(actualPath);
        } else {
            actualPath = ":/sound/dingdong.wav";
            sourceUrl = QUrl("qrc:/sound/dingdong.wav");
        }
    } else {
        sourceUrl = QUrl::fromLocalFile(actualPath);
    }

    if (m_audioOutput) {
        m_audioOutput->setVolume(1.0f);
        m_audioOutput->setMuted(false);
    }
    m_player->setSource(sourceUrl);
    m_player->play();
}

void SettingsDialog::onAddLesson() {
    if (!m_scheduleEditable) return;
    int row = ui->tblSchedule->rowCount();
    ui->tblSchedule->insertRow(row);

    auto* itemLesson = new QTableWidgetItem(tr("%1. Ders").arg(row + 1));
    itemLesson->setData(Qt::UserRole, row + 1);
    itemLesson->setTextAlignment(Qt::AlignCenter);
    itemLesson->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    ui->tblSchedule->setItem(row, 0, itemLesson);

    QString defStudent = "08:50";
    QString defTeacher = "08:52";
    QString defRecess  = "09:30";

    if (row > 0) {
        auto* prevRecessTe = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(row - 1, 3));
        if (prevRecessTe) {
            QTime prevRecess = prevRecessTe->time();
            QTime nextStudent = prevRecess.addSecs(10 * 60); // 10 dk teneffüs
            QTime nextTeacher = nextStudent.addSecs(2 * 60);  // 2 dk öğretmen zili
            QTime nextRecess  = nextTeacher.addSecs(40 * 60); // 40 dk ders
            defStudent = nextStudent.toString("HH:mm");
            defTeacher = nextTeacher.toString("HH:mm");
            defRecess  = nextRecess.toString("HH:mm");
        }
    }

    auto* teStudent = createScheduleCellTimeEdit(defStudent, "08:50");
    auto* teTeacher = createScheduleCellTimeEdit(defTeacher, "08:52");
    auto* teRecess  = createScheduleCellTimeEdit(defRecess,  "09:30");

    connect(teStudent, &QTimeEdit::timeChanged, this, [this](const QTime&) { validateSchedule(); });
    connect(teTeacher, &QTimeEdit::timeChanged, this, [this](const QTime&) { validateSchedule(); });
    connect(teRecess,  &QTimeEdit::timeChanged, this, [this](const QTime&) { validateSchedule(); });

    ui->tblSchedule->setCellWidget(row, 1, teStudent);
    ui->tblSchedule->setCellWidget(row, 2, teTeacher);
    ui->tblSchedule->setCellWidget(row, 3, teRecess);

    for (int r = 0; r < ui->tblSchedule->rowCount(); ++r) {
        auto* it = ui->tblSchedule->item(r, 0);
        if (it) {
            it->setText(tr("%1. Ders").arg(r + 1));
            it->setData(Qt::UserRole, r + 1);
        }
    }

    updateScheduleEditState();
    validateSchedule();
}

void SettingsDialog::onRemoveLesson() {
    if (!m_scheduleEditable) return;
    int cur = ui->tblSchedule->currentRow();
    if (cur >= 0) {
        ui->tblSchedule->removeRow(cur);
    } else if (ui->tblSchedule->rowCount() > 0) {
        ui->tblSchedule->removeRow(ui->tblSchedule->rowCount() - 1);
    }

    for (int r = 0; r < ui->tblSchedule->rowCount(); ++r) {
        auto* it = ui->tblSchedule->item(r, 0);
        if (it) {
            it->setText(tr("%1. Ders").arg(r + 1));
            it->setData(Qt::UserRole, r + 1);
        }
    }

    validateSchedule();
}

void SettingsDialog::onDefaultSchedule() {
    if (m_userRole == "YM" && !m_scheduleEditable) return;
    int count = ui->spinDailyLessonCount->value();
    int type  = ui->comboEducationType->currentIndex();
    if (type == 1) {
        ui->lblLessonUnit->setText(tr("Ders / Devre (Sabah: %1 + Öğle: %1 = Toplam %2 Ders)").arg(count).arg(count * 2));
    } else {
        ui->lblLessonUnit->setText(tr("Ders / Gün (Toplam %1 Ders)").arg(count));
    }
    populateScheduleTable(OkidiaConfig::defaultSchoolSchedule(count, type));
}

void SettingsDialog::updateScheduleEditState() {
    bool canEdit = (m_userRole == "YM") && m_scheduleEditable;

    ui->btnAddLesson->setEnabled(canEdit);
    ui->btnRemoveLesson->setEnabled(canEdit);
    ui->btnDefaultSchedule->setEnabled(canEdit);

    ui->btnToggleScheduleEdit->setIcon(ThemeManager::icon(m_scheduleEditable ? ":/ui/unlock.svg" : ":/ui/lock.svg"));
    ui->btnToggleScheduleEdit->setText(m_scheduleEditable ? tr("Düzenlemeyi Kilitle") : tr("Saatleri Düzenle"));
    ui->btnToggleScheduleEdit->setToolTip(m_scheduleEditable 
        ? tr("Ders saatleri tablosunu kilitleyerek kazara değişiklikleri önler.") 
        : tr("Ders saatleri tablosunu düzenlenebilir hale getirir."));

    for (int r = 0; r < ui->tblSchedule->rowCount(); ++r) {
        for (int c = 1; c <= 3; ++c) {
            auto* te = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, c));
            if (te) {
                te->setReadOnly(!canEdit);
                te->setButtonSymbols(canEdit ? QAbstractSpinBox::UpDownArrows : QAbstractSpinBox::NoButtons);
            }
        }
    }
}

void SettingsDialog::onToggleScheduleEdit() {
    if (m_userRole != "YM") return;
    m_scheduleEditable = !m_scheduleEditable;
    updateScheduleEditState();
    validateSchedule();
}

bool SettingsDialog::validateSchedule() {
    bool hasError = false;
    QString errorMsg;
    int errorRow = -1;
    int errorCol = -1;

    int rowCount = ui->tblSchedule->rowCount();
    if (rowCount == 0) {
        ui->lblScheduleError->setVisible(false);
        ui->btnApply->setEnabled(true);
        ui->btnOk->setEnabled(true);
        return true;
    }

    // Önceki hata vurgularını temizle
    for (int r = 0; r < rowCount; ++r) {
        for (int c = 1; c <= 3; ++c) {
            auto* te = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, c));
            if (te) {
                te->setStyleSheet("QTimeEdit { background: transparent; padding: 2px; }");
            }
        }
    }

    for (int r = 0; r < rowCount; ++r) {
        auto* teStudent = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, 1));
        auto* teTeacher = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, 2));
        auto* teRecess  = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r, 3));
        if (!teStudent || !teTeacher || !teRecess) continue;

        QTime tStudent = teStudent->time();
        QTime tTeacher = teTeacher->time();
        QTime tRecess  = teRecess->time();

        // 1. Kural: Öğrenci zili < Öğretmen zili
        if (tStudent >= tTeacher) {
            hasError = true;
            errorRow = r;
            errorCol = 1;
            errorMsg = tr("⚠️ %1. Ders: Öğrenci giriş zili (%2), öğretmen giriş zilinden (%3) önce olmalıdır!")
                           .arg(r + 1).arg(tStudent.toString("HH:mm"), tTeacher.toString("HH:mm"));
            break;
        }

        // 2. Kural: Öğretmen zili < Teneffüs zili
        if (tTeacher >= tRecess) {
            hasError = true;
            errorRow = r;
            errorCol = 2;
            errorMsg = tr("⚠️ %1. Ders: Öğretmen giriş zili (%2), teneffüs / bitiş zilinden (%3) önce olmalıdır!")
                           .arg(r + 1).arg(tTeacher.toString("HH:mm"), tRecess.toString("HH:mm"));
            break;
        }

        // 3. Kural: Ders süresi kontrolü (en az 5 dakika)
        int lessonMins = tTeacher.secsTo(tRecess) / 60;
        if (lessonMins < 5) {
            hasError = true;
            errorRow = r;
            errorCol = 3;
            errorMsg = tr("⚠️ %1. Ders: Ders süresi çok kısa (%2 dakika). En az 5 dakika olmalıdır.")
                           .arg(r + 1).arg(lessonMins);
            break;
        }

        // 4. Kural: Sonraki dersle sıralama kontrolü
        if (r < rowCount - 1) {
            auto* teNextStudent = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(r + 1, 1));
            if (teNextStudent) {
                QTime tNextStudent = teNextStudent->time();
                if (tRecess > tNextStudent) {
                    hasError = true;
                    errorRow = r;
                    errorCol = 3;
                    errorMsg = tr("⚠️ %1. Ders çıkış saati (%2), %3. Ders giriş saatinden (%4) sonra olamaz!")
                                   .arg(r + 1).arg(tRecess.toString("HH:mm")).arg(r + 2).arg(tNextStudent.toString("HH:mm"));
                    break;
                }
            }
        }
    }

    if (hasError) {
        if (errorRow >= 0 && errorCol >= 1) {
            auto* errTe = qobject_cast<QTimeEdit*>(ui->tblSchedule->cellWidget(errorRow, errorCol));
            if (errTe) {
                errTe->setStyleSheet("QTimeEdit { background-color: rgba(248, 81, 73, 0.25); border: 1.5px solid #f85149; border-radius: 4px; padding: 2px; font-weight: bold; }");
            }
        }
        ui->lblScheduleError->setText(errorMsg);
        ui->lblScheduleError->setVisible(true);
        ui->btnApply->setEnabled(false);
        ui->btnOk->setEnabled(false);
        return false;
    } else {
        ui->lblScheduleError->setVisible(false);
        ui->btnApply->setEnabled(true);
        ui->btnOk->setEnabled(true);
        return true;
    }
}
