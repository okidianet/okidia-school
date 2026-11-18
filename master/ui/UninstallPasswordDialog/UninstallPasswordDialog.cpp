// master/ui/UninstallPasswordDialog/UninstallPasswordDialog.cpp
#include "UninstallPasswordDialog.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../../common/ui/ThemeManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QGroupBox>
#include <QFrame>

UninstallPasswordDialog::UninstallPasswordDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
    updateStatusIndicator();
}

void UninstallPasswordDialog::setupUi() {
    setWindowTitle(tr("İstemci Kaldırma Parolası (Windows)"));
    setWindowIcon(ThemeManager::icon(":/ui/shield.svg"));
    setMinimumWidth(480);
    setModal(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(14);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Açıklama Başlığı (İkon + Metin)
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(12);

    auto* lblHeaderIcon = new QLabel(this);
    lblHeaderIcon->setPixmap(ThemeManager::icon(":/ui/shield.svg").pixmap(32, 32));
    lblHeaderIcon->setAlignment(Qt::AlignTop);
    headerLayout->addWidget(lblHeaderIcon);

    auto* lblHeader = new QLabel(
        tr("<b>Windows İstemci Kaldırma Koruma Parolası</b><br>"
           "<span style='color: palette(placeholderText); font-size: 11px;'>"
           "Bu parola, Windows istemcilerde OKIDIA İstemcisi Denetim Masası (Program Ekle/Kaldır) "
           "veya uninstaller aracı üzerinden kaldırılmak istendiğinde zorunlu olarak sorulur."
           "</span>"), this);
    lblHeader->setWordWrap(true);
    headerLayout->addWidget(lblHeader, 1);
    mainLayout->addLayout(headerLayout);

    // Durum Göstergesi Paneli (Tema uyumlu QFrame)
    auto* frameStatus = new QFrame(this);
    frameStatus->setObjectName("statusFrame");
    frameStatus->setStyleSheet(
        "#statusFrame { "
        "  border: 1px solid palette(mid); "
        "  border-radius: 6px; "
        "  background-color: palette(alternateBase); "
        "}");
    auto* statusLayout = new QHBoxLayout(frameStatus);
    statusLayout->setContentsMargins(12, 10, 12, 10);
    statusLayout->setSpacing(10);

    m_lblStatusIcon = new QLabel(frameStatus);
    statusLayout->addWidget(m_lblStatusIcon);

    m_lblStatusText = new QLabel(frameStatus);
    m_lblStatusText->setWordWrap(true);
    statusLayout->addWidget(m_lblStatusText, 1);

    mainLayout->addWidget(frameStatus);

    // Form Grubu
    auto* grpForm = new QGroupBox(tr("Yeni Parola Tanımla"), this);
    auto* formLayout = new QFormLayout(grpForm);
    formLayout->setSpacing(10);
    formLayout->setContentsMargins(15, 15, 15, 15);

    m_txtNewPassword = new QLineEdit(this);
    m_txtNewPassword->setEchoMode(QLineEdit::Password);
    m_txtNewPassword->setPlaceholderText(tr("Yeni koruma parolası"));
    formLayout->addRow(tr("Yeni Parola:"), m_txtNewPassword);

    m_txtConfirmPassword = new QLineEdit(this);
    m_txtConfirmPassword->setEchoMode(QLineEdit::Password);
    m_txtConfirmPassword->setPlaceholderText(tr("Yeni parolayı tekrar girin"));
    formLayout->addRow(tr("Parola (Tekrar):"), m_txtConfirmPassword);

    m_chkShowPassword = new QCheckBox(tr("Parolaları göster"), this);
    connect(m_chkShowPassword, &QCheckBox::toggled, this, &UninstallPasswordDialog::onToggleShowPassword);
    formLayout->addRow(QString(), m_chkShowPassword);

    mainLayout->addWidget(grpForm);

    // Bilgi Satırı (SVG ikon + metin)
    auto* infoLayout = new QHBoxLayout();
    infoLayout->setSpacing(8);

    auto* lblInfoIcon = new QLabel(this);
    lblInfoIcon->setPixmap(ThemeManager::icon(":/ui/shield.svg").pixmap(16, 16));
    infoLayout->addWidget(lblInfoIcon);

    auto* lblInfo = new QLabel(
        tr("Parola kaydedildiğinde açık olan tüm Windows istemcilerine anında iletilecek "
           "ve yeni kurulan istemcilere ilk bağlantıda otomatik gönderilecektir."), this);
    lblInfo->setStyleSheet("font-size: 11px; color: palette(placeholderText);");
    lblInfo->setWordWrap(true);
    infoLayout->addWidget(lblInfo, 1);
    mainLayout->addLayout(infoLayout);

    // Alt Butonlar
    auto* bottomLayout = new QHBoxLayout();
    m_btnResetDefault = new QPushButton(tr("Varsayılana Dön ('www.okidia.net')"), this);
    m_btnResetDefault->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    connect(m_btnResetDefault, &QPushButton::clicked, this, &UninstallPasswordDialog::onResetDefaultClicked);
    bottomLayout->addWidget(m_btnResetDefault);

    bottomLayout->addStretch();

    m_btnCancel = new QPushButton(tr("İptal"), this);
    m_btnCancel->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    bottomLayout->addWidget(m_btnCancel);

    m_btnSave = new QPushButton(tr("Kaydet ve Dağıt"), this);
    m_btnSave->setIcon(ThemeManager::icon(":/ui/save.svg"));
    m_btnSave->setDefault(true);
    m_btnSave->setStyleSheet("font-weight: bold; padding: 6px 14px;");
    connect(m_btnSave, &QPushButton::clicked, this, &UninstallPasswordDialog::onSaveClicked);
    bottomLayout->addWidget(m_btnSave);

    mainLayout->addLayout(bottomLayout);
}

void UninstallPasswordDialog::updateStatusIndicator() {
    bool isDefault = OkidiaDatabase::instance().isDefaultUninstallPassword();
    if (isDefault) {
        m_lblStatusIcon->setPixmap(ThemeManager::icon(":/ui/warning.svg").pixmap(20, 20));
        m_lblStatusText->setText(
            tr("<b>Güvenlik Durumu:</b> Kaldırma parolası varsayılan değerde (<b>'www.okidia.net'</b>) bırakılmıştır. "
               "Lütfen yeni ve güvenli bir parola belirleyin."));
        m_btnResetDefault->setEnabled(false);
    } else {
        m_lblStatusIcon->setPixmap(ThemeManager::icon(":/ui/check_circle.svg").pixmap(20, 20));
        m_lblStatusText->setText(
            tr("<b>Güvenlik Durumu:</b> Özel güvenli kaldırma parolası tanımlıdır."));
        m_btnResetDefault->setEnabled(true);
    }
}

void UninstallPasswordDialog::onToggleShowPassword(bool show) {
    auto mode = show ? QLineEdit::Normal : QLineEdit::Password;
    m_txtNewPassword->setEchoMode(mode);
    m_txtConfirmPassword->setEchoMode(mode);
}

void UninstallPasswordDialog::onResetDefaultClicked() {
    auto ret = QMessageBox::question(
        this,
        tr("Varsayılana Sıfırla"),
        tr("İstemci kaldırma parolasını varsayılan '<b>www.okidia.net</b>' değerine sıfırlamak istediğinize emin misiniz?<br><br>"
           "Bu durum güvenlik risklerine yol açabilir."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        m_passwordHash = OkidiaDatabase::DEFAULT_UNINSTALL_PASS_HASH;
        m_isResetToDefault = true;
        accept();
    }
}

void UninstallPasswordDialog::onSaveClicked() {
    QString p1 = m_txtNewPassword->text();
    QString p2 = m_txtConfirmPassword->text();

    if (p1.isEmpty()) {
        QMessageBox::warning(this, tr("Eksik Bilgi"), tr("Lütfen yeni bir kaldırma parolası girin."));
        m_txtNewPassword->setFocus();
        return;
    }

    if (p1 != p2) {
        QMessageBox::warning(this, tr("Parola Uyuşmuyor"), tr("Girdiğiniz parolalar birbiriyle uyuşmuyor. Lütfen tekrar kontrol edin."));
        m_txtConfirmPassword->setFocus();
        return;
    }

    if (p1.length() < 4) {
        QMessageBox::warning(this, tr("Zayıf Parola"), tr("Kaldırma parolası en az 4 karakter uzunluğunda olmalıdır."));
        m_txtNewPassword->setFocus();
        return;
    }

    m_passwordHash = QString::fromUtf8(QCryptographicHash::hash(p1.toUtf8(), QCryptographicHash::Sha256).toHex()).toLower();
    m_isResetToDefault = (p1 == OkidiaDatabase::DEFAULT_UNINSTALL_PASS);
    accept();
}
