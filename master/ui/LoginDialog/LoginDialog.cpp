// master/ui/LoginDialog/LoginDialog.cpp
#include "LoginDialog.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../session/SessionManager.h"


#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QStackedWidget>

#include <QSettings>
#include <QMessageBox>
#include <QInputDialog>
#include <QListWidget>
#include <QEvent>

#include "../SettingsDialog/SettingsDialog.h"
#include "../SetupWizard/SetupWizard.h"
#include "../../../common/ui/ThemeManager.h"


LoginDialog::LoginDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("OKIDIA — Giriş"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setFixedWidth(360);
    setWindowModality(Qt::ApplicationModal);

    m_stack = new QStackedWidget(this);
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->addWidget(m_stack);

    // ── Login sayfası ─────────────────────────────────────────────────────
    m_loginPage = new QWidget;
    auto* lay   = new QVBoxLayout(m_loginPage);
    lay->setContentsMargins(32, 28, 32, 24);
    lay->setSpacing(10);

    // Logo
    auto* logo = new QLabel(tr("OKIDIA"), m_loginPage);
    logo->setAlignment(Qt::AlignCenter);
    logo->setProperty("class", "headerLabel");
    lay->addWidget(logo);

    auto* sub = new QLabel(tr("Okul İçi Denetim ve İletişim Ağı"), m_loginPage);
    sub->setAlignment(Qt::AlignCenter);
    sub->setProperty("status", "info");
    lay->addWidget(sub);

    m_schoolLabel = new QLabel(m_loginPage);
    m_schoolLabel->setAlignment(Qt::AlignCenter);
    m_schoolLabel->setProperty("status", "info");
    m_schoolLabel->setStyleSheet("font-weight: bold; padding: 4px 8px; border-radius: 4px;");
    m_schoolLabel->setCursor(Qt::PointingHandCursor);
    m_schoolLabel->setToolTip(tr("Bağlanmak istediğiniz okulu seçmek veya yeni okul eklemek için tıklayın"));
    m_schoolLabel->installEventFilter(this);
    lay->addWidget(m_schoolLabel);

    auto* sep = new QFrame(m_loginPage);
    sep->setFrameShape(QFrame::HLine);
    lay->addWidget(sep);
    lay->addSpacing(4);

    auto* form = new QFormLayout();
    form->setSpacing(8);

    m_usernameEdit = new QLineEdit(m_loginPage);
    m_usernameEdit->setPlaceholderText(tr("Kullanıcı adı"));
    m_usernameEdit->setMinimumHeight(34);

    m_passwordEdit = new QLineEdit(m_loginPage);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Şifre"));
    m_passwordEdit->setMinimumHeight(34);

    form->addRow(tr("Kullanıcı:"), m_usernameEdit);
    form->addRow(tr("Şifre:"),     m_passwordEdit);
    lay->addLayout(form);

    m_errorLabel = new QLabel(m_loginPage);
    m_errorLabel->setProperty("status", "error");
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setOpenExternalLinks(true);
    lay->addWidget(m_errorLabel);

    m_rememberMe = new QCheckBox(tr("Beni bu bilgisayarda hatırla"), m_loginPage);
    m_rememberMe->setChecked(true); // Varsayılan açık
    lay->addWidget(m_rememberMe);

    m_btnLogin = new QPushButton(tr("Giriş Yap"), m_loginPage);
    m_btnLogin->setProperty("class", "primary");
    m_btnLogin->setDefault(true);
    lay->addWidget(m_btnLogin);

    auto* btnForgot = new QPushButton(tr("Şifremi Unuttum"), m_loginPage);
    btnForgot->setProperty("class", "link");
    
    auto* btnNetSettings = new QPushButton(tr("Okul / Ağ Seçimi"), m_loginPage);
    btnNetSettings->setProperty("class", "link");
    btnNetSettings->setIcon(ThemeManager::icon(":/ui/school.svg"));
    btnNetSettings->setToolTip(tr("Okul değiştir veya yeni bir okula katıl"));
    
    auto* hLay = new QHBoxLayout();
    hLay->addWidget(btnForgot);
    hLay->addStretch();
    hLay->addWidget(btnNetSettings);
    lay->addLayout(hLay);

    m_btnOfflineSsh = new QPushButton(tr("Çevrimdışı SSH Modu"), m_loginPage);
    m_btnOfflineSsh->setProperty("class", "link");
    m_btnOfflineSsh->setIcon(ThemeManager::icon(":/ui/monitor.svg"));
    m_btnOfflineSsh->setToolTip(tr("Sunucu olmadan doğrudan SSH ve İstemci Dağıtımını kullanın"));
    lay->addWidget(m_btnOfflineSsh, 0, Qt::AlignCenter);

    lay->addStretch();

    m_stack->addWidget(m_loginPage);

    // ── Şifre değişim sayfası ─────────────────────────────────────────────
    m_changePwPage = new QWidget;
    auto* lay2 = new QVBoxLayout(m_changePwPage);
    lay2->setContentsMargins(32, 28, 32, 24);
    lay2->setSpacing(10);

    auto* logo2 = new QLabel(tr("Şifre Değişimi"), m_changePwPage);
    logo2->setAlignment(Qt::AlignCenter);
    logo2->setProperty("class", "headerLabel");
    lay2->addWidget(logo2);

    auto* info = new QLabel(
        tr("İlk girişiniz. Güvenliğiniz için\nşifrenizi değiştirmeniz gerekmektedir."),
        m_changePwPage);
    info->setAlignment(Qt::AlignCenter);
    info->setProperty("status", "info");
    lay2->addWidget(info);

    auto* sep2 = new QFrame(m_changePwPage);
    sep2->setFrameShape(QFrame::HLine);
    lay2->addWidget(sep2);
    lay2->addSpacing(4);

    auto* form2 = new QFormLayout();
    form2->setSpacing(8);

    m_newPwEdit = new QLineEdit(m_changePwPage);
    m_newPwEdit->setEchoMode(QLineEdit::Password);
    m_newPwEdit->setPlaceholderText(tr("En az 8 karakter"));
    m_newPwEdit->setMinimumHeight(34);

    m_newPw2Edit = new QLineEdit(m_changePwPage);
    m_newPw2Edit->setEchoMode(QLineEdit::Password);
    m_newPw2Edit->setPlaceholderText(tr("Şifreyi tekrar girin"));
    m_newPw2Edit->setMinimumHeight(34);

    form2->addRow(tr("Yeni Şifre:"),        m_newPwEdit);
    form2->addRow(tr("Yeni Şifre (tekrar):"), m_newPw2Edit);
    lay2->addLayout(form2);

    m_changePwError = new QLabel(m_changePwPage);
    m_changePwError->setProperty("status", "error");
    m_changePwError->setWordWrap(true);
    m_changePwError->setAlignment(Qt::AlignCenter);
    lay2->addWidget(m_changePwError);

    m_btnChangePw = new QPushButton(tr("Şifreyi Kaydet"), m_changePwPage);
    m_btnChangePw->setProperty("class", "primary");
    m_btnChangePw->setDefault(true);
    lay2->addWidget(m_btnChangePw);
    lay2->addStretch();

    m_stack->addWidget(m_changePwPage);

    // ── Bağlantılar ───────────────────────────────────────────────────────
    connect(m_btnLogin,    &QPushButton::clicked, this, &LoginDialog::onLogin);
    connect(m_btnChangePw, &QPushButton::clicked, this, &LoginDialog::onChangePassword);
    connect(m_btnOfflineSsh, &QPushButton::clicked, this, [this]() {
        m_isOfflineSshMode = true;
        m_user.id = "offline_admin";
        m_user.username = "OfflineAdmin";
        m_user.role = "YM";
        m_user.displayName = tr("Çevrimdışı Yönetici");
        m_user.isActive = true;
        m_user.mustChangePw = false;
        Logger::info(QObject::tr("LoginDialog: Çevrimdışı SSH Modu seçildi, ana ekrana geçiliyor."));
        accept();
    });
    connect(btnForgot,     &QPushButton::clicked, this, &LoginDialog::onForgotPassword);
    connect(btnNetSettings,&QPushButton::clicked, this, &LoginDialog::onNetworkSettingsClicked);
    connect(m_usernameEdit, &QLineEdit::returnPressed, this, &LoginDialog::onLogin);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::onLogin);
    connect(m_newPw2Edit,   &QLineEdit::returnPressed, this, &LoginDialog::onChangePassword);

    connect(m_usernameEdit, &QLineEdit::textChanged, this, [this]() {
        if (m_btnLogin->isEnabled()) m_errorLabel->clear();
    });
    connect(m_passwordEdit, &QLineEdit::textChanged, this, [this]() {
        if (m_btnLogin->isEnabled()) m_errorLabel->clear();
    });

    showLoginPage();
}

void LoginDialog::setSessionManager(SessionManager* session) {
    m_session = session;
    if (m_session) {
        connect(m_session, &SessionManager::loginResult, this, [this](bool success, const QString& userId, const QString& role, const QString& displayName, const QString& errorMsg, bool mustChangePw) {
            cancelLoginTimeout();
            m_btnLogin->setEnabled(true);
            if (success) {
                m_user.id = userId;
                m_user.username = m_usernameEdit->text().trimmed();
                m_user.role = role;
                m_user.displayName = displayName;
                m_user.isActive = true;
                m_user.mustChangePw = mustChangePw;
                m_user.passwordHash = OkidiaUtils::sha256(m_passwordEdit->text().toUtf8());

                if (m_session) {
                    m_session->setPendingCredentials(m_user.username, m_user.passwordHash);
                    m_session->updateCachedPassword(m_user.passwordHash);
                }
                
                QSettings s("okidia", "master");
                s.setValue("last_username", m_user.username);
                s.setValue("login/remember_me", m_rememberMe->isChecked());

                Logger::info(QObject::tr("LoginDialog: Ağ login başarılı → ") + m_user.username + QObject::tr(", mustChangePw=") + (mustChangePw ? QObject::tr("true") : QObject::tr("false")));
                if (mustChangePw) {
                    showChangePasswordPage();
                } else {
                    accept();
                }
            } else {
                setError(errorMsg);
                Logger::warning(QObject::tr("LoginDialog: Ağ login başarısız → ") + errorMsg);
            }
        });

        connect(m_session, &SessionManager::brokerDisconnected, this, [this]() {
            if (m_loginTimeoutTimer && m_loginTimeoutTimer->isActive()) {
                cancelLoginTimeout();
                m_btnLogin->setEnabled(true);
                setError(tr("Sunucu bağlantısı kurulamadı veya kesildi."));
            }
        });

        connect(m_session, &SessionManager::dbUserPwResetResult, this, [this](bool success, const QString& errorMsg){
            m_btnChangePw->setEnabled(true);
            if (success) {
                m_user.mustChangePw = false;
                m_user.passwordHash = m_pendingPasswordHash;
                Logger::info(QObject::tr("LoginDialog: Şifre merkezde başarıyla değiştirildi → ") + m_user.username);
                m_session->updateCachedPassword(m_pendingPasswordHash);
                accept();
            } else {
                m_changePwError->setText(tr("Şifre değiştirilemedi: ") + QObject::tr(errorMsg.toUtf8().constData()));
                Logger::warning(QObject::tr("LoginDialog: Merkezde şifre değişimi başarısız → ") + errorMsg);
            }
        });
    }
}

bool LoginDialog::rememberMe() const {
    return m_rememberMe->isChecked();
}

void LoginDialog::updateSchoolLabel() {
    OkidiaConfig::cleanupGhostNetworks(OkidiaDatabase::instance().path());

    struct SchoolProfileInfo {
        QString netKey;
        QString name;
        QString shortHash;
    };
    QMap<QString, SchoolProfileInfo> profiles;
    QMap<QString, int> nameCounts;

    QDir baseDir(OkidiaConfig::masterCertBaseDir());
    if (baseDir.exists()) {
        for (const QString& userDir : baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir netDir(baseDir.filePath(userDir + "/Networks"));
            if (!netDir.exists()) continue;
            for (const QString& netKey : netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (!OkidiaConfig::isValidNetworkDir(netDir.filePath(netKey), netKey)) continue;
                if (!profiles.contains(netKey)) {
                    QString hash;
                    QString parsedName = OkidiaConfig::parseNetworkKey(netKey, &hash);
                    profiles.insert(netKey, {netKey, parsedName, hash});
                    nameCounts[parsedName.toLower()]++;
                }
            }
        }
    }

    QString curKey = OkidiaConfig::currentNetworkKey();
    if (!profiles.contains(curKey) && !profiles.isEmpty()) {
        curKey = profiles.firstKey();
        QSettings s("okidia", "master");
        s.setValue("net/activeNetworkKey", curKey);
        s.setValue("school/name", profiles.value(curKey).name);
    }

    if (profiles.contains(curKey)) {
        const auto& info = profiles.value(curKey);
        QString displayName = info.name;
        if (nameCounts.value(info.name.toLower()) > 1 && !info.shortHash.isEmpty()) {
            displayName = QString("%1 (#%2)").arg(info.name, info.shortHash);
        }
        m_schoolLabel->setText(displayName);
        m_schoolLabel->setVisible(true);
    } else {
        QString schoolName = OkidiaConfig::currentNetworkName();
        int cafpPos = schoolName.indexOf("cafp=", 0, Qt::CaseInsensitive);
        if (cafpPos >= 0) {
            if (cafpPos > 0 && (schoolName[cafpPos - 1] == 'd' || schoolName[cafpPos - 1] == 'D' || schoolName[cafpPos - 1].isSpace())) {
                cafpPos--;
            }
            schoolName = schoolName.left(cafpPos).trimmed();
            QSettings s("okidia", "master");
            s.setValue("school/name", schoolName);
        }
        if (!schoolName.isEmpty() && schoolName != "Network" && schoolName != "DefaultNetwork") {
            m_schoolLabel->setText(schoolName);
            m_schoolLabel->setVisible(true);
        } else {
            m_schoolLabel->clear();
            m_schoolLabel->setVisible(false);
        }
    }
}

void LoginDialog::showLoginPage() {
    m_stack->setCurrentWidget(m_loginPage);
    adjustSize();

    updateSchoolLabel();

    QSettings s("okidia", "master");
    const QString lastUser = s.value("last_username").toString();
    m_rememberMe->setChecked(s.value("login/remember_me", true).toBool());

    if (!lastUser.isEmpty()) {
        m_usernameEdit->setText(lastUser);
        m_passwordEdit->setFocus();
    } else {
        m_usernameEdit->setFocus();
    }
}

void LoginDialog::showChangePasswordPage() {
    m_stack->setCurrentWidget(m_changePwPage);
    adjustSize();
    m_newPwEdit->setFocus();
}

void LoginDialog::setError(const QString& msg) {
    if (msg.isEmpty()) {
        m_errorLabel->setText("");
    } else {
        m_errorLabel->setText(QObject::tr(msg.toUtf8().constData()));
    }
}

void LoginDialog::startLoginTimeout() {
    cancelLoginTimeout();
    if (!m_loginTimeoutTimer) {
        m_loginTimeoutTimer = new QTimer(this);
        m_loginTimeoutTimer->setSingleShot(true);
        connect(m_loginTimeoutTimer, &QTimer::timeout, this, [this]() {
            m_btnLogin->setEnabled(true);
            setError(tr("Sunucu yanıt vermedi. Lütfen ağ bağlantınızı ve sunucunun çalıştığını kontrol edin."));
            Logger::warning(QObject::tr("LoginDialog: Giriş isteği zaman aşımına uğradı"));
        });
    }
    m_loginTimeoutTimer->start(12000);
}

void LoginDialog::cancelLoginTimeout() {
    if (m_loginTimeoutTimer && m_loginTimeoutTimer->isActive()) {
        m_loginTimeoutTimer->stop();
    }
}

void LoginDialog::onLogin() {
    const QString username = m_usernameEdit->text().trimmed();
    const QString password = m_passwordEdit->text();

    if (username.isEmpty()) {
        setError(tr("Kullanıcı adı boş olamaz."));
        return;
    }
    if (password.isEmpty()) {
        setError(tr("Şifre boş olamaz."));
        return;
    }

    const QString hash = OkidiaUtils::sha256(password.toUtf8());

    // 1. Ag Login denemesi
    if (m_session && (m_session->isConnected() || m_session->isConnecting())) {
        m_btnLogin->setEnabled(false);
        setError(tr("Ağ üzerinden giriş yapılıyor..."));
        startLoginTimeout();
        m_session->requestLogin(username, hash);
    } else {
        // Çevrimdışı mod: Sadece yerel DB'de doğrulanırsa kabul et
        UserRecord u = OkidiaDatabase::instance().userByUsername(username);
        if (!u.id.isEmpty() && u.isActive && u.passwordHash == hash) {
            m_session->setPendingCredentials(username, hash);
            m_session->updateCachedPassword(hash);
            
            m_user = u; // Rol ve ID bilgilerini yerel DB'den tam ve güvenli olarak alıyoruz
            m_user.mustChangePw = false; // Çevrimdışı modda şifre değişimi zaten çalışmaz

            QSettings s("okidia", "master");
            s.setValue("last_username", m_user.username);
            s.setValue("login/remember_me", m_rememberMe->isChecked());

            Logger::info(QObject::tr("LoginDialog: Sunucu çevrimdışı. Yerel doğrulama başarılı. Çevrimdışı modda açılıyor → ") + username);
            accept();
        } else if (m_session) {
            // Yerel doğrulama başarısız (şifre değişmiş olabilir veya yeni kayıt) -> Sunucuya bağlanmayı tetikle
            m_session->triggerReconnect();
            m_btnLogin->setEnabled(false);
            setError(tr("Sunucuya bağlanılıyor ve giriş yapılıyor..."));
            startLoginTimeout();
            m_session->requestLogin(username, hash);
        } else {
            setError(tr("Sunucu çevrimdışı ve yerel doğrulama başarısız."));
            Logger::warning(QObject::tr("LoginDialog: Sunucu çevrimdışı ve yerel giriş reddedildi → ") + username);
        }
    }
}

void LoginDialog::onChangePassword() {
    const QString pw1 = m_newPwEdit->text();
    const QString pw2 = m_newPw2Edit->text();

    if (pw1.length() < 8) {
        m_changePwError->setText(tr("Şifre en az 8 karakter olmalı."));
        return;
    }
    if (pw1 != pw2) {
        m_changePwError->setText(tr("Şifreler eşleşmiyor."));
        return;
    }

    const QString hash = OkidiaUtils::sha256(pw1.toUtf8());
    m_pendingPasswordHash = hash;

    if (m_session && m_session->isConnected()) {
        m_btnChangePw->setEnabled(false);
        m_changePwError->setText(tr("Şifre güncelleniyor..."));
        m_session->sendDbUserPwReset(m_user.id, hash, false);
    } else {
        m_changePwError->setText(tr("Sunucu bağlantısı yok. Şifre değiştirilemedi."));
        Logger::warning(QObject::tr("Login: Sunucu baglantisi olmadigindan sifre degistirilemedi -> ") + m_user.username);
    }
}
void LoginDialog::onForgotPassword() {
    if (!m_session) return;
    ForgotPasswordDialog dlg(m_session, this);
    dlg.exec();
}

// ─────────────────────────────────────────────────────────────────────────────
// ForgotPasswordDialog Implementation
// ─────────────────────────────────────────────────────────────────────────────

#include <QFrame>
#include <QStyle>

ForgotPasswordDialog::ForgotPasswordDialog(SessionManager* session, QWidget* parent)
    : QDialog(parent)
    , m_session(session)
{
    setWindowTitle(tr("Şifre Kurtarma"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setFixedWidth(380);
    setWindowModality(Qt::ApplicationModal);

    m_stack = new QStackedWidget(this);
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->addWidget(m_stack);

    // ── Aşama 1: Kurtarma Kodu Doğrulama ────────────────────────────────────
    m_verifyPage = new QWidget;
    auto* lay1 = new QVBoxLayout(m_verifyPage);
    lay1->setContentsMargins(32, 28, 32, 24);
    lay1->setSpacing(12);

    auto* title1 = new QLabel(tr("Kurtarma veya Kurulum Kodu"), m_verifyPage);
    title1->setAlignment(Qt::AlignCenter);
    title1->setProperty("class", "headerLabel");
    lay1->addWidget(title1);

    auto* info1 = new QLabel(
        tr("Yönetici (YM) şifrenizi sıfırlamak için\n16 haneli kurtarma kodunuzu veya sunucu Kurulum Kodunu girin."),
        m_verifyPage);
    info1->setAlignment(Qt::AlignCenter);
    info1->setProperty("status", "info");
    lay1->addWidget(info1);

    auto* sep1 = new QFrame(m_verifyPage);
    sep1->setFrameShape(QFrame::HLine);
    lay1->addWidget(sep1);

    auto* form1 = new QFormLayout();
    form1->setSpacing(8);

    m_codeEdit = new QLineEdit(m_verifyPage);
    m_codeEdit->setPlaceholderText(tr("XXXX-XXXX-XXXX-XXXX veya OKIDIA-XXXX-XXXX"));
    m_codeEdit->setMinimumHeight(34);
    m_codeEdit->setAlignment(Qt::AlignCenter);
    m_codeEdit->setFont(QFont("monospace", 12));

    form1->addRow(tr("Kurtarma / Kurulum Kodu:"), m_codeEdit);
    lay1->addLayout(form1);

    m_verifyErrorLabel = new QLabel(m_verifyPage);
    m_verifyErrorLabel->setProperty("status", "error");
    m_verifyErrorLabel->setWordWrap(true);
    m_verifyErrorLabel->setAlignment(Qt::AlignCenter);
    lay1->addWidget(m_verifyErrorLabel);

    m_btnVerify = new QPushButton(tr("Kodu Doğrula"), m_verifyPage);
    m_btnVerify->setProperty("class", "primary");
    m_btnVerify->setDefault(true);
    lay1->addWidget(m_btnVerify);
    lay1->addStretch();

    m_stack->addWidget(m_verifyPage);

    // ── Aşama 2: Yeni Şifre Belirleme ───────────────────────────────────────
    m_resetPage = new QWidget;
    auto* lay2 = new QVBoxLayout(m_resetPage);
    lay2->setContentsMargins(32, 28, 32, 24);
    lay2->setSpacing(12);

    auto* title2 = new QLabel(tr("Yeni Şifre"), m_resetPage);
    title2->setAlignment(Qt::AlignCenter);
    title2->setProperty("class", "headerLabel");
    lay2->addWidget(title2);

    auto* info2 = new QLabel(
        tr("Kod doğrulandı. Lütfen yeni yönetici\n(YM) şifresini belirleyin (en az 8 karakter)."),
        m_resetPage);
    info2->setAlignment(Qt::AlignCenter);
    info2->setProperty("status", "info");
    lay2->addWidget(info2);

    auto* sep2 = new QFrame(m_resetPage);
    sep2->setFrameShape(QFrame::HLine);
    lay2->addWidget(sep2);

    auto* form2 = new QFormLayout();
    form2->setSpacing(8);

    m_newPwEdit = new QLineEdit(m_resetPage);
    m_newPwEdit->setEchoMode(QLineEdit::Password);
    m_newPwEdit->setPlaceholderText(tr("En az 8 karakter"));
    m_newPwEdit->setMinimumHeight(34);

    m_newPwConfirmEdit = new QLineEdit(m_resetPage);
    m_newPwConfirmEdit->setEchoMode(QLineEdit::Password);
    m_newPwConfirmEdit->setPlaceholderText(tr("Şifreyi tekrar girin"));
    m_newPwConfirmEdit->setMinimumHeight(34);

    form2->addRow(tr("Yeni Şifre:"),        m_newPwEdit);
    form2->addRow(tr("Yeni Şifre (tekrar):"), m_newPwConfirmEdit);
    lay2->addLayout(form2);

    m_resetErrorLabel = new QLabel(m_resetPage);
    m_resetErrorLabel->setProperty("status", "error");
    m_resetErrorLabel->setWordWrap(true);
    m_resetErrorLabel->setAlignment(Qt::AlignCenter);
    lay2->addWidget(m_resetErrorLabel);

    m_btnSave = new QPushButton(tr("Şifreyi Kaydet"), m_resetPage);
    m_btnSave->setProperty("class", "primary");
    m_btnSave->setDefault(true);
    lay2->addWidget(m_btnSave);
    lay2->addStretch();

    m_stack->addWidget(m_resetPage);

    m_stack->setCurrentWidget(m_verifyPage);

    // Bağlantılar
    connect(m_btnVerify, &QPushButton::clicked, this, &ForgotPasswordDialog::onVerifyClicked);
    connect(m_btnSave,    &QPushButton::clicked, this, &ForgotPasswordDialog::onSaveClicked);
    connect(m_codeEdit,   &QLineEdit::textChanged, this, &ForgotPasswordDialog::onCodeTextChanged);
    connect(m_codeEdit,   &QLineEdit::returnPressed, this, &ForgotPasswordDialog::onVerifyClicked);
    connect(m_newPwConfirmEdit, &QLineEdit::returnPressed, this, &ForgotPasswordDialog::onSaveClicked);

    if (m_session) {
        connect(m_session, &SessionManager::passwordResetResult, this, &ForgotPasswordDialog::onResetResult);
    }
}

void ForgotPasswordDialog::onVerifyClicked() {
    if (m_isVerifying || !m_session) return;

    QString rawKey = m_codeEdit->text().trimmed();
    QString cleanKey = OkidiaUtils::cleanRecoveryKey(rawKey);

    if (cleanKey.length() < 8) {
        m_verifyErrorLabel->setText(tr("Lütfen 16 haneli kurtarma kodunu veya Kurulum Kodunu eksiksiz girin."));
        return;
    }

    m_isVerifying = true;
    m_btnVerify->setEnabled(false);
    m_verifyErrorLabel->setProperty("status", "info");
    m_verifyErrorLabel->setText(tr("Kod doğrulanıyor..."));
    m_verifyErrorLabel->style()->unpolish(m_verifyErrorLabel);
    m_verifyErrorLabel->style()->polish(m_verifyErrorLabel);

    // Sadece kod doğrulama isteği
    m_session->requestPasswordReset(rawKey.toUpper(), "");
}

void ForgotPasswordDialog::onSaveClicked() {
    if (!m_session || m_verifiedKey.isEmpty()) return;

    QString pw1 = m_newPwEdit->text();
    QString pw2 = m_newPwConfirmEdit->text();

    if (pw1.length() < 8) {
        m_resetErrorLabel->setText(tr("Şifre en az 8 karakter olmalı."));
        return;
    }
    if (pw1 != pw2) {
        m_resetErrorLabel->setText(tr("Şifreler eşleşmiyor."));
        return;
    }

    m_btnSave->setEnabled(false);
    m_resetErrorLabel->setProperty("status", "info");
    m_resetErrorLabel->setText(tr("Şifreniz güncelleniyor..."));
    m_resetErrorLabel->style()->unpolish(m_resetErrorLabel);
    m_resetErrorLabel->style()->polish(m_resetErrorLabel);

    QString newHash = OkidiaUtils::sha256(pw1.toUtf8());
    m_session->requestPasswordReset(m_verifiedKey, newHash);
}

void ForgotPasswordDialog::onResetResult(bool success, const QString& errorMsg) {
    if (m_isVerifying) {
        m_isVerifying = false;
        m_btnVerify->setEnabled(true);
        m_verifyErrorLabel->setProperty("status", "error");
        m_verifyErrorLabel->style()->unpolish(m_verifyErrorLabel);
        m_verifyErrorLabel->style()->polish(m_verifyErrorLabel);

        if (success) {
            m_verifiedKey = m_codeEdit->text().trimmed().toUpper();
            m_verifyErrorLabel->clear();
            m_stack->setCurrentWidget(m_resetPage);
            adjustSize();
            m_newPwEdit->setFocus();
        } else {
            m_verifyErrorLabel->setText(tr("Doğrulama başarısız: ") + QObject::tr(errorMsg.toUtf8().constData()));
        }
    } else {
        m_btnSave->setEnabled(true);
        m_resetErrorLabel->setProperty("status", "error");
        m_resetErrorLabel->style()->unpolish(m_resetErrorLabel);
        m_resetErrorLabel->style()->polish(m_resetErrorLabel);

        if (success) {
            QMessageBox::information(this, tr("Başarılı"), tr("Şifreniz sıfırlandı. Yeni şifrenizle giriş yapabilirsiniz."));
            accept();
        } else {
            m_resetErrorLabel->setText(tr("Hata: ") + QObject::tr(errorMsg.toUtf8().constData()));
        }
    }
}

void ForgotPasswordDialog::onCodeTextChanged(const QString& text) {
    const QString upper = text.toUpper();
    if (upper != text) {
        const int pos = m_codeEdit->cursorPosition();
        m_codeEdit->setText(upper);
        m_codeEdit->setCursorPosition(pos);
    }
}

bool LoginDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_schoolLabel && event->type() == QEvent::MouseButtonRelease) {
        onNetworkSettingsClicked();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void LoginDialog::onNetworkSettingsClicked() {
    OkidiaConfig::cleanupGhostNetworks(OkidiaDatabase::instance().path());

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Okul ve Ağ Yönetimi"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(400);

    auto* mainLay = new QVBoxLayout(&dlg);
    mainLay->setContentsMargins(20, 18, 20, 16);
    mainLay->setSpacing(10);

    auto* lblHeader = new QLabel(tr("Kayıtlı Okul Profilleri:"), &dlg);
    lblHeader->setStyleSheet("font-weight: bold;");
    mainLay->addWidget(lblHeader);

    auto* listWidget = new QListWidget(&dlg);
    listWidget->setMinimumHeight(130);
    listWidget->setAlternatingRowColors(true);
    listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    mainLay->addWidget(listWidget);

    auto* profileBtnRow = new QHBoxLayout();
    profileBtnRow->setSpacing(8);

    auto* btnSwitch = new QPushButton(ThemeManager::icon(":/ui/check_circle.svg"), tr("Seçili Okula Bağlan"), &dlg);
    btnSwitch->setProperty("class", "primary");

    auto* btnRemove = new QPushButton(ThemeManager::icon(":/ui/trash.svg"), tr("Profili Kaldır"), &dlg);
    btnRemove->setToolTip(tr("Seçili okula ait güvenlik sertifikalarını ve yerel veritabanını bu bilgisayardan siler"));

    profileBtnRow->addWidget(btnSwitch, 1);
    profileBtnRow->addWidget(btnRemove);
    mainLay->addLayout(profileBtnRow);

    auto* sep = new QFrame(&dlg);
    sep->setFrameShape(QFrame::HLine);
    mainLay->addWidget(sep);

    auto* btnAddSchool = new QPushButton(ThemeManager::icon(":/ui/add.svg"), tr("Yeni Okula Katıl / Kurulum Yap..."), &dlg);
    auto* btnRemoteIp  = new QPushButton(ThemeManager::icon(":/ui/network.svg"), tr("Uzak Sunucu IP Ayarla..."), &dlg);
    mainLay->addWidget(btnAddSchool);
    mainLay->addWidget(btnRemoteIp);

    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch();
    auto* btnClose = new QPushButton(tr("Kapat"), &dlg);
    bottomRow->addWidget(btnClose);
    mainLay->addLayout(bottomRow);

    struct ProfileEntry {
        QString netKey;
        QString name;
        QString shortHash;
    };

    auto populateList = [&]() {
        listWidget->clear();
        QMap<QString, ProfileEntry> entries;
        QMap<QString, int> nameCounts;

        QDir baseDir(OkidiaConfig::masterCertBaseDir());
        if (baseDir.exists()) {
            for (const QString& userDir : baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir netDir(baseDir.filePath(userDir + "/Networks"));
                if (!netDir.exists()) continue;
                for (const QString& netKey : netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    if (!OkidiaConfig::isValidNetworkDir(netDir.filePath(netKey), netKey)) continue;
                    if (!entries.contains(netKey)) {
                        QString hash;
                        QString name = OkidiaConfig::parseNetworkKey(netKey, &hash);
                        entries.insert(netKey, {netKey, name, hash});
                        nameCounts[name.toLower()]++;
                    }
                }
            }
        }

        const QString curKey = OkidiaConfig::currentNetworkKey();
        int activeRow = -1;
        int row = 0;

        for (auto it = entries.cbegin(); it != entries.cend(); ++it, ++row) {
            const ProfileEntry& e = it.value();
            // Seçenek 2: Yalnızca aynı isimli birden fazla okul varsa parantez içinde #hash göster
            QString displayLabel = e.name;
            if (nameCounts.value(e.name.toLower()) > 1 && !e.shortHash.isEmpty()) {
                displayLabel = QString("%1 (#%2)").arg(e.name, e.shortHash);
            }

            const bool isCurrent = (e.netKey == curKey);
            QString itemText = isCurrent ? QString("%1 — [%2]").arg(displayLabel, tr("Aktif")) : displayLabel;

            auto* item = new QListWidgetItem(ThemeManager::icon(":/ui/school.svg"), itemText, listWidget);
            item->setData(Qt::UserRole, e.netKey);
            item->setData(Qt::UserRole + 1, e.name);
            item->setData(Qt::UserRole + 2, displayLabel);
            item->setToolTip(tr("Ağ Kimliği: %1").arg(e.netKey));
            if (isCurrent) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
                activeRow = row;
            }
        }

        if (listWidget->count() == 0) {
            auto* emptyItem = new QListWidgetItem(tr("Kayıtlı okul profili bulunamadı."), listWidget);
            emptyItem->setFlags(Qt::NoItemFlags);
            btnSwitch->setEnabled(false);
            btnRemove->setEnabled(false);
        } else {
            listWidget->setCurrentRow(activeRow >= 0 ? activeRow : 0);
            btnSwitch->setEnabled(true);
            btnRemove->setEnabled(true);
        }
    };

    populateList();

    connect(listWidget, &QListWidget::itemSelectionChanged, &dlg, [&]() {
        auto* item = listWidget->currentItem();
        const bool valid = item && !item->data(Qt::UserRole).toString().isEmpty();
        btnSwitch->setEnabled(valid);
        btnRemove->setEnabled(valid);
    });

    auto applySelectedSchool = [&]() {
        auto* item = listWidget->currentItem();
        if (!item) return;
        const QString netKey = item->data(Qt::UserRole).toString();
        const QString cleanName = item->data(Qt::UserRole + 1).toString();
        if (netKey.isEmpty()) return;

        QSettings s("okidia", "master");
        s.setValue("net/activeNetworkKey", netKey);
        s.setValue("school/name", cleanName);
        s.sync();

        if (m_session) {
            m_session->switchNetwork(netKey);
        }
        updateSchoolLabel();

        const QString lastUser = s.value("last_username").toString().trimmed();
        if (!lastUser.isEmpty()) {
            m_usernameEdit->setText(lastUser);
            m_passwordEdit->setFocus();
        }
        dlg.accept();
    };

    connect(btnSwitch, &QPushButton::clicked, &dlg, applySelectedSchool);
    connect(listWidget, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem* item) {
        if (item && !item->data(Qt::UserRole).toString().isEmpty()) {
            applySelectedSchool();
        }
    });

    connect(btnRemove, &QPushButton::clicked, &dlg, [&]() {
        auto* item = listWidget->currentItem();
        if (!item) return;
        const QString targetKey = item->data(Qt::UserRole).toString();
        const QString displayLabel = item->data(Qt::UserRole + 2).toString();
        if (targetKey.isEmpty()) return;

        int ret = QMessageBox::question(&dlg, tr("Okul Profilini Kaldır"),
            tr("'%1' okul profilini bu bilgisayardan kaldırmak istediğinize emin misiniz?\n\n"
               "Bu işlem, bu okula ait kayıtlı güvenlik sertifikalarını, kimlik anahtarlarını ve yerel önbellek veritabanını kalıcı olarak silecektir.")
               .arg(displayLabel),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (ret != QMessageBox::Yes) return;

        const QString prevActiveKey = OkidiaConfig::currentNetworkKey();

        // Silinecek dizindeki veritabanı açıksa dosya kilidini serbest bırakmak için kapat
        if (OkidiaDatabase::instance().isOpen() &&
            OkidiaDatabase::instance().path().contains(targetKey, Qt::CaseInsensitive)) {
            OkidiaDatabase::instance().close();
        }

        OkidiaConfig::removeNetworkProfile(targetKey);
        Logger::info(QObject::tr("LoginDialog: Okul profili kullanıcı tarafından kaldırıldı → ") + targetKey);

        QSettings s("okidia", "master");
        if (prevActiveKey == targetKey) {
            s.remove("net/activeNetworkKey");
            s.remove("net/activeCaFingerprint");
            s.remove("school/name");
            s.sync();
        }

        populateList();

        // Eğer aktif okul silindiyse ve listede başka bir okul varsa ona geç, yoksa bağlantıyı sıfırla
        auto* firstItem = listWidget->item(0);
        const QString nextKey = firstItem ? firstItem->data(Qt::UserRole).toString() : QString();
        const QString nextName = firstItem ? firstItem->data(Qt::UserRole + 1).toString() : QString();

        if (!nextKey.isEmpty()) {
            if (prevActiveKey == targetKey) {
                s.setValue("net/activeNetworkKey", nextKey);
                s.setValue("school/name", nextName);
                s.sync();
                if (m_session) {
                    m_session->switchNetwork(nextKey);
                }
                populateList();
            }
        } else {
            if (m_session) {
                m_session->connectToBroker("", "", "");
                if (m_session->tlsClient()) {
                    m_session->tlsClient()->disconnectFromServer();
                }
            }
        }
        updateSchoolLabel();
    });

    bool openSetupWizard = false;
    connect(btnAddSchool, &QPushButton::clicked, &dlg, [&]() {
        openSetupWizard = true;
        dlg.accept();
    });

    connect(btnRemoteIp, &QPushButton::clicked, &dlg, [&]() {
        AppSettings cfg = AppSettings::load();
        bool ipOk = false;
        QString ip = QInputDialog::getText(&dlg, tr("Ağ Ayarları"),
                                           tr("Uzak Sunucu IP (Örn: 100.78.36.38):"),
                                           QLineEdit::Normal,
                                           cfg.remoteBrokerHost, &ipOk);
        if (ipOk) {
            cfg.remoteBrokerHost = ip.trimmed();
            cfg.save();
            QMessageBox::information(&dlg, tr("Ağ Ayarları"),
                                     tr("Ayarlar kaydedildi.\nDeğişikliklerin etkili olması için lütfen uygulamayı kapatıp yeniden açın veya birkaç saniye bekleyin."));
        }
    });

    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::reject);

    dlg.exec();

    if (openSetupWizard) {
        SetupWizard wiz(this);
        if (wiz.exec() == QDialog::Accepted) {
            OkidiaConfig::instance().reload();
            QString newNetKey = OkidiaConfig::currentNetworkKey();
            if (m_session) {
                m_session->switchNetwork(newNetKey);
            }
            OkidiaConfig::cleanupGhostNetworks(OkidiaDatabase::instance().path());
            updateSchoolLabel();

            QSettings s("okidia", "master");
            const QString lastUser = s.value("last_username").toString().trimmed();
            if (!lastUser.isEmpty()) {
                m_usernameEdit->setText(lastUser);
                m_passwordEdit->setFocus();
            }
        }
    }
}
