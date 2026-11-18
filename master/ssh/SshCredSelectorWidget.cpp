// master/ssh/SshCredSelectorWidget.cpp
#include "SshCredSelectorWidget.h"
#include "SshCredentialDialog.h"
#include "../../common/ui/ThemeManager.h"
#include "../common/crypto/AesGcm.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include "../../common/utils/OkidiaUtils.h"
#include <QDir>
#include <QSettings>
#include <QApplication>

SshCredSelectorWidget::SshCredSelectorWidget(SessionManager* session, QWidget* parent)
    : QWidget(parent), m_session(session) {
    setupUi();
    refreshCredentials();
}

void SshCredSelectorWidget::setupUi() {
    auto* formLayout = new QFormLayout(this);
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->setSpacing(6);

    // 1. Kayıtlı Kimlik
    m_credCombo = new QComboBox(this);
    m_manageCredsBtn = new QPushButton(tr("Yönet..."), this);
    m_manageCredsBtn->setFixedWidth(80);
    m_manageCredsBtn->setCursor(Qt::PointingHandCursor);

    auto* credRow = new QHBoxLayout();
    credRow->addWidget(m_credCombo, 1);
    credRow->addWidget(m_manageCredsBtn);
    formLayout->addRow(tr("Kayıtlı Kimlik:"), credRow);

    // 2. Kullanıcı Adı
    m_userEdit = new QLineEdit(this);
    m_userEdit->setPlaceholderText(tr("Kullanıcı adı (örn: root, okidia)"));
    formLayout->addRow(tr("Kullanıcı:"), m_userEdit);

    // 3. Kimlik Doğrulama Yöntemi
    m_authCombo = new QComboBox(this);
    m_authCombo->addItems({tr("Parola (Şifre)"), tr("Anahtar Dosyası"), "SSH Agent"});
    formLayout->addRow(tr("Yöntem:"), m_authCombo);

    // 4. Parola
    m_passEdit = new QLineEdit(this);
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_passEdit->setPlaceholderText(tr("SSH parolası"));
    m_passLabel = new QLabel(tr("Parola:"), this);
    formLayout->addRow(m_passLabel, m_passEdit);

    // 5. Anahtar Dosyası
    m_keyPathEdit = new QLineEdit(this);
    m_keyPathEdit->setPlaceholderText("~/.ssh/id_ed25519");
    m_keyBrowseBtn = new QPushButton("...", this);
    m_keyBrowseBtn->setFixedWidth(32);

    auto* keyRow = new QHBoxLayout();
    keyRow->addWidget(m_keyPathEdit);
    keyRow->addWidget(m_keyBrowseBtn);
    m_keyRowWidget = new QWidget(this);
    m_keyRowWidget->setLayout(keyRow);
    m_keyRowWidget->setContentsMargins(0, 0, 0, 0);
    m_keyLabel = new QLabel(tr("Anahtar:"), this);
    formLayout->addRow(m_keyLabel, m_keyRowWidget);

    // Başlangıç görünürlük ayarları
    m_keyRowWidget->setVisible(false);
    m_keyLabel->setVisible(false);

    // Bağlantılar
    connect(m_credCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SshCredSelectorWidget::onCredComboChanged);
    connect(m_authCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SshCredSelectorWidget::onAuthComboChanged);
    connect(m_manageCredsBtn, &QPushButton::clicked,
            this, &SshCredSelectorWidget::onManageCredentials);
    connect(m_keyBrowseBtn, &QPushButton::clicked,
            this, &SshCredSelectorWidget::onBrowseKey);

    // Değişiklik sinyalleri
    connect(m_userEdit, &QLineEdit::textChanged, this, &SshCredSelectorWidget::configChanged);
    connect(m_passEdit, &QLineEdit::textChanged, this, &SshCredSelectorWidget::configChanged);
    connect(m_keyPathEdit, &QLineEdit::textChanged, this, &SshCredSelectorWidget::configChanged);
    connect(m_authCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SshCredSelectorWidget::configChanged);
}

void SshCredSelectorWidget::refreshCredentials() {
    if (!m_session || !m_credCombo) return;

    m_credCombo->blockSignals(true);
    QString currentId = m_credCombo->currentData().toString();
    m_credCombo->clear();
    m_credCombo->addItem(tr("El ile Gir (Manuel)"), "");

    for (const auto& c : m_session->sshCredentials()) {
        if (c.targetType != "agent") {
            QString label = c.name + " (" + c.username + ")";
            if (c.ownerUserId.isEmpty()) label += tr(" [Ortak]");
            m_credCombo->addItem(label, c.id);
        }
    }

    int idx = m_credCombo->findData(currentId);
    if (idx >= 0) m_credCombo->setCurrentIndex(idx);
    else m_credCombo->setCurrentIndex(0);

    m_credCombo->blockSignals(false);
    onCredComboChanged(m_credCombo->currentIndex());
}

void SshCredSelectorWidget::onCredComboChanged(int index) {
    QString id = m_credCombo->itemData(index).toString();
    const bool isManual = id.isEmpty();

    m_userEdit->setEnabled(isManual);
    m_authCombo->setEnabled(isManual);
    m_passEdit->setEnabled(isManual);
    m_keyPathEdit->setEnabled(isManual);
    m_keyBrowseBtn->setEnabled(isManual);

    if (isManual) {
        onAuthComboChanged(m_authCombo->currentIndex());
        return;
    }

    // Seçili kimliği bulup alanları doldur
    if (m_session) {
        for (const auto& c : m_session->sshCredentials()) {
            if (c.id == id) {
                m_userEdit->setText(c.username);
                QByteArray sec = c.secret;
                if (!sec.isEmpty() && sec.size() >= 28) {
                    QByteArray decrypted = AesGcm::decrypt(sec, AesGcm::defaultSystemKey());
                    if (!decrypted.isEmpty()) sec = decrypted;
                }

                if (c.isKey) {
                    m_authCombo->setCurrentIndex(1);
                    m_keyPathEdit->setText(QString::fromUtf8(sec));
                    m_passEdit->clear();
                } else {
                    m_authCombo->setCurrentIndex(0);
                    m_passEdit->setText(QString::fromUtf8(sec));
                    m_keyPathEdit->clear();
                }
                break;
            }
        }
    }
}

void SshCredSelectorWidget::onAuthComboChanged(int index) {
    // Sadece manuel moddaysak veya veri gösterimi için görünürlükleri ayarla
    const bool showPass = (index == 0);
    const bool showKey  = (index == 1);

    m_passEdit->setVisible(showPass);
    m_passLabel->setVisible(showPass);

    m_keyRowWidget->setVisible(showKey);
    m_keyLabel->setVisible(showKey);
}

void SshCredSelectorWidget::onManageCredentials() {
    if (!m_session) return;
    SshCredentialDialog dlg(m_session, this);
    
    QString lastSavedId;
    connect(&dlg, &SshCredentialDialog::credentialSaved, [&](const QString& id) {
        lastSavedId = id;
    });

    dlg.exec();
    m_session->fetchSshCredentials();
    refreshCredentials();

    if (!lastSavedId.isEmpty()) {
        int idx = m_credCombo->findData(lastSavedId);
        if (idx >= 0) m_credCombo->setCurrentIndex(idx);
    }
}

void SshCredSelectorWidget::onBrowseKey() {
    QString p = OkidiaUtils::getOpenFileName(this, tr("SSH Anahtar Dosyası Seç"),
                                             "ssh_keys",
                                             tr("Tüm Dosyalar (*)"),
                                             nullptr, QFileDialog::Options(),
                                             QDir::homePath() + "/.ssh");
    if (!p.isEmpty()) {
        m_keyPathEdit->setText(p);
    }
}

QString SshCredSelectorWidget::username() const {
    return m_userEdit->text().trimmed();
}

QString SshCredSelectorWidget::secret() const {
    if (m_authCombo->currentIndex() == 1) {
        return m_keyPathEdit->text().trimmed();
    }
    return m_passEdit->text();
}

bool SshCredSelectorWidget::isPrivateKey() const {
    return m_authCombo->currentIndex() == 1;
}

bool SshCredSelectorWidget::isSshAgent() const {
    return m_authCombo->currentIndex() == 2;
}

QString SshCredSelectorWidget::currentCredentialId() const {
    return m_credCombo->currentData().toString();
}

SshClient::Config SshCredSelectorWidget::getConfig(const QString& host) const {
    SshClient::Config cfg;
    cfg.host = host;
    cfg.port = 22;
    cfg.connectTimeoutMs = 6000;
    cfg.hostKeyCheck = false;
    cfg.username = username();

    if (isPrivateKey()) {
        cfg.authMethod = SshClient::AuthMethod::PrivateKey;
        cfg.privateKeyPath = secret();
        if (cfg.privateKeyPath.startsWith("~/")) {
            QString home = qgetenv("HOME");
            cfg.privateKeyPath.replace(0, 1, home);
        }
        cfg.password.clear();
    } else if (isSshAgent()) {
        cfg.authMethod = SshClient::AuthMethod::Agent;
    } else {
        cfg.authMethod = SshClient::AuthMethod::Password;
        cfg.password = secret();
        cfg.privateKeyPath.clear();
    }

    return cfg;
}

void SshCredSelectorWidget::loadSettings(const QString& groupName) {
    QSettings s;
    s.beginGroup(groupName);
    
    // Eğer kaydedilmiş bir kimlik ID'si varsa onu seçmeye çalış
    QString savedCredId = s.value("credentialId").toString();
    if (!savedCredId.isEmpty()) {
        int idx = m_credCombo->findData(savedCredId);
        if (idx >= 0) {
            m_credCombo->setCurrentIndex(idx);
            s.endGroup();
            return;
        }
    }

    // Manuel ayarları geri yükle
    m_credCombo->setCurrentIndex(0);
    m_userEdit->setText(s.value("username", "root").toString());
    m_authCombo->setCurrentIndex(s.value("authType", 0).toInt());
    m_keyPathEdit->setText(s.value("keyPath").toString());
    QString p64 = s.value("password").toString();
    if (!p64.isEmpty()) {
        m_passEdit->setText(QString::fromUtf8(QByteArray::fromBase64(p64.toUtf8())));
    }
    s.endGroup();
}

void SshCredSelectorWidget::saveSettings(const QString& groupName) {
    QSettings s;
    s.beginGroup(groupName);
    
    QString credId = currentCredentialId();
    s.setValue("credentialId", credId);
    
    if (credId.isEmpty()) {
        // Manuel ayarları kaydet
        s.setValue("username", m_userEdit->text());
        s.setValue("authType", m_authCombo->currentIndex());
        s.setValue("keyPath", m_keyPathEdit->text());
        s.setValue("password", m_passEdit->text().toUtf8().toBase64());
    }
    s.endGroup();
}

void SshCredSelectorWidget::setReadOnly(bool readOnly) {
    m_credCombo->setEnabled(!readOnly);
    m_manageCredsBtn->setEnabled(!readOnly);
    m_userEdit->setEnabled(!readOnly);
    m_authCombo->setEnabled(!readOnly);
    m_passEdit->setEnabled(!readOnly);
    m_keyPathEdit->setEnabled(!readOnly);
    m_keyBrowseBtn->setEnabled(!readOnly);
}
