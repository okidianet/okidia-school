#include "SetupWizard.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QDateTime>
#include <QRegularExpression>
#include <QSysInfo>
#include <QEventLoop>
#include <QTimer>
#include <QSettings>
#include <QInputDialog>

#include <QRadioButton>
#include <QButtonGroup>
#include <QRandomGenerator>

#include "../../../common/OkidiaConfig.h"
#include "../../../common/crypto/BackupPackage.h"
#include "../../../common/crypto/KeyManager.h"
#include "messages.pb.h"
#include "../SettingsDialog/SettingsDialog.h"

// ─────────────────────────────────────────────────────────────────────────────
// Sabitler
// ─────────────────────────────────────────────────────────────────────────────

#define kCertDir  (OkidiaConfig::userCertDir())
#define kKeyPath  (OkidiaConfig::userKeyPath())
#define kCertPath (OkidiaConfig::userCertPath())
#define kCaPath   (OkidiaConfig::userCertDir() + "/ca.crt")

// ─────────────────────────────────────────────────────────────────────────────
// WelcomePage
// ─────────────────────────────────────────────────────────────────────────────

WelcomePage::WelcomePage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("OKIDIA Kurulum Sihirbazı"));
    setSubTitle(tr("Sistemin ilk kurulumu için aşağıdaki adımları takip edin."));

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(12);

    auto* lbl = new QLabel(
        tr("<p>Bu sihirbaz şu işlemleri gerçekleştirecek:</p>"
        "<ol>"
        "<li>Ağdaki OKIDIA broker'larını otomatik bulur</li>"
        "<li>Sunucu'nun kurulum kodunu doğrular</li>"
        "<li>Yönetici (YM) hesabı oluşturur</li>"
        "<li>Master sertifikanızı broker'dan alır</li>"
        "</ol>"
        "<p><b>Başlamadan önce:</b> Sunucu'nun kurulduğu bilgisayarda terminalde "
        "gösterilen kurulum kodunu hazır bulundurun.</p>"),
        this);
    lbl->setWordWrap(true);
    lbl->setTextFormat(Qt::RichText);
    lay->addWidget(lbl);

    // Fiziksel Güvenlik ve Disk Şifreleme Durumu Bilgi Kartı
    bool isEncrypted = OkidiaUtils::checkDiskEncryption();
    auto* securityFrame = new QFrame(this);
    securityFrame->setObjectName("securityFrame");
    securityFrame->setFrameShape(QFrame::StyledPanel);
    securityFrame->setStyleSheet(
        "QFrame#securityFrame {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 6px;"
        "  padding: 8px;"
        "  background: palette(window);"
        "}"
    );
    auto* secLay = new QHBoxLayout(securityFrame);
    secLay->setContentsMargins(10, 8, 10, 8);
    secLay->setSpacing(10);

    auto* iconLabel = new QLabel(securityFrame);
    auto* textLabel = new QLabel(securityFrame);
    textLabel->setWordWrap(true);
    textLabel->setTextFormat(Qt::RichText);

    if (isEncrypted) {
        iconLabel->setPixmap(ThemeManager::icon(":/ui/shield.svg").pixmap(24, 24));
        textLabel->setText(tr("<b>Fiziksel Güvenlik:</b> Sistem sürücüsünde tam disk şifreleme (LUKS / BitLocker) aktif durumda."));
    } else {
        iconLabel->setPixmap(ThemeManager::icon(":/ui/info.svg").pixmap(24, 24));
        textLabel->setText(tr("<b>Güvenlik Önerisi:</b> Sistem sürücünüzde disk şifrelemesi tespit edilemedi. "
                              "Sunucu ve yönetim verilerinizin tam korunması için BitLocker / LUKS tam disk şifrelemesi önerilir."));
    }

    secLay->addWidget(iconLabel, 0, Qt::AlignTop);
    secLay->addWidget(textLabel, 1);
    lay->addWidget(securityFrame);

    lay->addStretch();
}

// ─────────────────────────────────────────────────────────────────────────────
// ChoicePage
// ─────────────────────────────────────────────────────────────────────────────

ChoicePage::ChoicePage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Kurulum Tipi"));
    setSubTitle(tr("Sistemi nasıl kurmak istediğinizi seçin."));

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(20);

    m_group = new QButtonGroup(this);

    auto* rbYM = new QRadioButton(tr("Yeni bir sistem kur (Yönetici Master - YM)"), this);
    auto* rbOM = new QRadioButton(tr("Mevcut bir sisteme katıl (Katılım Kodu ile)"), this);
    auto* rbRestore = new QRadioButton(tr("Yedekten Sistemi Kurtar (Disaster Recovery)"), this);

    rbYM->setChecked(true);
    
    m_group->addButton(rbYM, SetupWizard::Mode_NewYM);
    m_group->addButton(rbOM, SetupWizard::Mode_JoinOM);
    m_group->addButton(rbRestore, SetupWizard::Mode_RestoreIdentity);

    lay->addWidget(rbYM);
    lay->addWidget(new QLabel(tr("<small>Sunucu ayarlarını yapabilir, sınıflar oluşturabilir ve tam yetkiye sahip olursunuz.</small>"), this));
    
    lay->addWidget(rbOM);
    lay->addWidget(new QLabel(tr("<small>Mevcut Master veya Yöneticiniz tarafından oluşturulan bir 'Katılım Kodu' ile sisteme dahil olursunuz. Yetkileriniz giriş yapılan kullanıcı hesabına (YM, OM, GM) göre belirlenir.</small>"), this));
    
    lay->addWidget(rbRestore);
    lay->addWidget(new QLabel(tr("<small>Daha önce aldığınız şifreli .okbak yedek dosyası ile Sunucu veritabanını ve orijinal Kök CA kimliğini geri yüklersiniz.</small>"), this));

    lay->addStretch();

    registerField("setupMode", this, "modeValue");
}

int ChoicePage::nextId() const {
    int mode = m_group->checkedId();
    if (mode == SetupWizard::Mode_RestoreIdentity) {
        return SetupWizard::Page_RestoreSelect; 
    }
    return SetupWizard::Page_Discovery;
}

int ChoicePage::modeValue() const {
    return m_group->checkedId();
}

// ─────────────────────────────────────────────────────────────────────────────
// RestoreSelectPage
// ─────────────────────────────────────────────────────────────────────────────

RestoreSelectPage::RestoreSelectPage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Sistem Yedeği ve Kurtarma"));
    setSubTitle(tr("Broker sunucusunu kurtarmak için .okbak uzantılı yedek dosyasını ve parolasını girin."));

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(14);

    lay->addWidget(new QLabel(tr("Yedek Dosyası (.okbak):"), this));
    auto* row = new QHBoxLayout();
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("Yedek dosyası seçilmedi..."));
    
    auto* btnBrowse = new QPushButton(tr("Gözat…"), this);
    connect(btnBrowse, &QPushButton::clicked, this, &RestoreSelectPage::onBrowse);

    row->addWidget(m_pathEdit);
    row->addWidget(btnBrowse);
    lay->addLayout(row);

    lay->addWidget(new QLabel(tr("Yedekleme Parolası:"), this));
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Yedek alınırken belirlenen şifre..."));
    connect(m_passwordEdit, &QLineEdit::textChanged, this, &RestoreSelectPage::onFieldChanged);
    lay->addWidget(m_passwordEdit);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet("color: #ef4444; font-size: 12px;");
    m_errorLabel->setWordWrap(true);
    lay->addWidget(m_errorLabel);

    lay->addWidget(new QLabel(tr("<small>Not: Yeni kurulan Broker sunucusunun kurulum kodunu (setup_code.txt) bir sonraki adımda doğrulayacaksınız.</small>"), this));
    lay->addStretch();

    registerField("restoreBackupPath*", m_pathEdit);
    registerField("restorePassword*", m_passwordEdit);
}

void RestoreSelectPage::onBrowse() {
    QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/OKIDIA-Yedekler";
    QString path = OkidiaUtils::getOpenFileName(
        this, tr("Yedek Dosyası Seç"), "backup_restore", tr("OKIDIA Yedek (*.okbak);;Tüm Dosyalar (*)"),
        nullptr, QFileDialog::Options(), defaultDir);
    if (!path.isEmpty()) {
        m_pathEdit->setText(path);
        onFieldChanged();
    }
}

void RestoreSelectPage::onFieldChanged() {
    m_errorLabel->clear();
    emit completeChanged();
}

bool RestoreSelectPage::isComplete() const {
    return !m_pathEdit->text().isEmpty() && !m_passwordEdit->text().isEmpty();
}

bool RestoreSelectPage::validatePage() {
    QString path = m_pathEdit->text().trimmed();
    QString pwd = m_passwordEdit->text();

    if (!BackupPackage::verifyHeaderFile(path)) {
        m_errorLabel->setText(tr("Seçilen dosya geçerli bir OKIDIA (.okbak) yedek dosyası değil."));
        return false;
    }

    BackupContent content;
    QString err;
    if (!BackupPackage::unpackFromFile(path, pwd, content, &err)) {
        m_errorLabel->setText(tr("Şifre çözülemedi: %1").arg(err));
        return false;
    }

    m_errorLabel->clear();
    return true;
}

int RestoreSelectPage::nextId() const {
    return SetupWizard::Page_Discovery;
}

QString RestoreSelectPage::backupPath() const {
    return m_pathEdit->text().trimmed();
}

QString RestoreSelectPage::backupPassword() const {
    return m_passwordEdit->text();
}

// ─────────────────────────────────────────────────────────────────────────────
// JoinCodePage (Katılım Kodu Sayfası)
// ─────────────────────────────────────────────────────────────────────────────

JoinCodePage::JoinCodePage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Ağa Katılım Kodu"));
    setSubTitle(tr("Mevcut Master'dan aldığınız Katılım Kodunu ve bu bilgisayarda kullanacağınız hesap şifrenizi girin."));

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(10);

    lay->addWidget(new QLabel(tr("Katılım Kodu:"), this));
    m_codeEdit = new QLineEdit(this);
    m_codeEdit->setPlaceholderText("JOIN-XXXX-YYYY");
    m_codeEdit->setMaxLength(16);
    m_codeEdit->setAlignment(Qt::AlignCenter);
    m_codeEdit->setFont(QFont("monospace", 18));
    lay->addWidget(m_codeEdit);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setProperty("status", "error");
    lay->addWidget(m_hintLabel);

    lay->addSpacing(8);

    auto* sepLine = new QFrame(this);
    sepLine->setFrameShape(QFrame::HLine);
    lay->addWidget(sepLine);

    lay->addWidget(new QLabel(tr("Hesap Şifresi:"), this));
    auto* pwInfo = new QLabel(tr("<small>Giriş yapacağınız hesabın mevcut şifresini girin (veya yeni şifre belirleyin, en az 8 karakter).</small>"), this);
    pwInfo->setWordWrap(true);
    lay->addWidget(pwInfo);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("En az 8 karakter"));
    m_passwordEdit->setMinimumHeight(32);
    lay->addWidget(m_passwordEdit);

    m_password2Edit = new QLineEdit(this);
    m_password2Edit->setEchoMode(QLineEdit::Password);
    m_password2Edit->setPlaceholderText(tr("Şifreyi tekrar girin"));
    m_password2Edit->setMinimumHeight(32);
    lay->addWidget(m_password2Edit);

    m_pwErrorLabel = new QLabel(this);
    m_pwErrorLabel->setProperty("status", "error");
    m_pwErrorLabel->setWordWrap(true);
    lay->addWidget(m_pwErrorLabel);

    lay->addStretch();

    registerField("joinCode*", m_codeEdit);
    connect(m_codeEdit,    &QLineEdit::textChanged, this, &JoinCodePage::onCodeChanged);
    connect(m_passwordEdit, &QLineEdit::textChanged, this, &JoinCodePage::onPasswordChanged);
    connect(m_password2Edit, &QLineEdit::textChanged, this, &JoinCodePage::onPasswordChanged);
}

void JoinCodePage::onCodeChanged(const QString& text) {
    const QString upper = text.toUpper();
    if (upper != text) {
        m_codeEdit->setText(upper);
    }
    const bool valid = QRegularExpression("^JOIN-[A-Z0-9]{4}-[A-Z0-9]{4}$").match(upper).hasMatch();
    m_hintLabel->setText(valid ? "" : tr("Format: JOIN-XXXX-YYYY"));
    emit completeChanged();
}

void JoinCodePage::onPasswordChanged() {
    const QString pw1 = m_passwordEdit->text();
    const QString pw2 = m_password2Edit->text();
    if (!pw2.isEmpty() && pw1 != pw2) {
        m_pwErrorLabel->setText(tr("Şifreler eşleşmiyor."));
    } else if (!pw1.isEmpty() && pw1.length() < 8) {
        m_pwErrorLabel->setText(tr("Şifre en az 8 karakter olmalı."));
    } else {
        m_pwErrorLabel->clear();
    }
    emit completeChanged();
}

QString JoinCodePage::password() const {
    return m_passwordEdit->text();
}

bool JoinCodePage::isComplete() const {
    const bool codeOk = QRegularExpression("^JOIN-[A-Z0-9]{4}-[A-Z0-9]{4}$").match(m_codeEdit->text()).hasMatch();
    const QString pw1 = m_passwordEdit->text();
    const QString pw2 = m_password2Edit->text();
    const bool pwOk = pw1.length() >= 8 && pw1 == pw2;
    return codeOk && pwOk;
}

int JoinCodePage::nextId() const {
    return SetupWizard::Page_Finish;
}

// ─────────────────────────────────────────────────────────────────────────────
// BrokerDiscoveryPage
// ─────────────────────────────────────────────────────────────────────────────

BrokerDiscoveryPage::BrokerDiscoveryPage(QWidget* parent)
    : QWizardPage(parent)
    , m_mdns(new MdnsManager(this))
    , m_scanTimer(new QTimer(this))
{
    setTitle(tr("Sunucu Keşfi"));
    setSubTitle(tr("Ağdaki OKIDIA broker'ları aranıyor..."));

    auto* lay = new QVBoxLayout(this);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setMaximumHeight(6);
    lay->addWidget(m_progress);

    m_statusLabel = new QLabel(tr("Ağ taranıyor..."), this);
    m_statusLabel->setProperty("status", "info");
    lay->addWidget(m_statusLabel);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    lay->addWidget(m_list, 1);

    m_rescanBtn = new QPushButton(tr("Yeniden Tara"), this);
    m_rescanBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_rescanBtn->setEnabled(false);
    
    m_manualAddBtn = new QPushButton(tr("El ile Ekle"), this);
    m_manualAddBtn->setIcon(ThemeManager::icon(":/ui/edit.svg"));

    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(m_manualAddBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_rescanBtn);
    lay->addLayout(btnRow);

    m_scanTimer->setSingleShot(true);
    m_scanTimer->setInterval(8000);

    connect(m_mdns,      &MdnsManager::serviceFound,
            this,        &BrokerDiscoveryPage::onServiceFound);
    connect(m_mdns,      &MdnsManager::serviceLost,
            this,        &BrokerDiscoveryPage::onServiceLost);
    connect(m_scanTimer, &QTimer::timeout,
            this,        &BrokerDiscoveryPage::onScanTimeout);
    connect(m_rescanBtn, &QPushButton::clicked,
            this,        &BrokerDiscoveryPage::startScan);
    connect(m_manualAddBtn, &QPushButton::clicked,
            this,        &BrokerDiscoveryPage::onManualAdd);
    connect(m_list,      &QListWidget::itemSelectionChanged,
            this,        &BrokerDiscoveryPage::onSelectionChanged);
}

BrokerDiscoveryPage::~BrokerDiscoveryPage() {
    m_mdns->stopBrowsing();
}

void BrokerDiscoveryPage::initializePage() { startScan(); }

void BrokerDiscoveryPage::startScan() {
    m_brokers.clear();
    m_list->clear();
    m_rescanBtn->setEnabled(false);
    m_progress->setRange(0, 0);
    m_statusLabel->setText(tr("Ağ taranıyor..."));
    setSubTitle(tr("Ağdaki OKIDIA broker'ları aranıyor..."));
    m_mdns->stopBrowsing();
    m_mdns->startBrowsing("_okidia._tcp");
    m_scanTimer->start();
    emit completeChanged();
}

void BrokerDiscoveryPage::onServiceFound(const QString& name,
                                          const QString& host,
                                          quint16 port,
                                          const QString& role) {
    auto ipScore = [](const QString& ip) -> int {
        if (ip.isEmpty() || ip.startsWith("127.")) return 1;
        if (ip.startsWith("192.168.56.")) return 2; // VirtualBox Host-Only
        if (ip.startsWith("172.17.") || ip.startsWith("172.18.") || ip.startsWith("172.19.") || ip.startsWith("172.20.")) return 2; // Docker
        if (ip.startsWith("10.0.2.")) return 2; // VirtualBox NAT
        if (ip.startsWith("192.168.") || ip.startsWith("10.") || ip.startsWith("172.")) return 10; // Fiziksel LAN
        return 8;
    };

    // Aynı broker adı zaten listede mi? → allHosts'a ekle ve önceliğe göre sırala
    for (auto& b : m_brokers) {
        if (b.name == name && b.port == port) {
            if (!b.allHosts.contains(host)) {
                b.allHosts << host;
                std::sort(b.allHosts.begin(), b.allHosts.end(), [ipScore](const QString& a, const QString& bStr) {
                    return ipScore(a) > ipScore(bStr);
                });
                b.host = b.allHosts.first();
            }
            return;
        }
    }

    BrokerEntry entry;
    entry.name = name;
    entry.host = host;
    entry.port = port;
    entry.role = role;
    entry.allHosts << host;
    m_brokers << entry;
    addBroker(entry);
    emit completeChanged();
}

void BrokerDiscoveryPage::onServiceLost(const QString& name) {
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == name) {
            delete m_list->takeItem(i);
            break;
        }
    }
    m_brokers.erase(
        std::remove_if(m_brokers.begin(), m_brokers.end(),
                       [&](const BrokerEntry& b){ return b.name == name; }),
        m_brokers.end());
    emit completeChanged();
}

void BrokerDiscoveryPage::onManualAdd() {
    bool ok;
    QString ip = QInputDialog::getText(this, tr("El ile Sunucu Ekle"),
                                       tr("Sunucu'nun IP adresini girin (örn. Tailscale IP'si):"),
                                       QLineEdit::Normal, "", &ok);
    if (ok && !ip.trimmed().isEmpty()) {
        QString host = ip.trimmed();
        quint16 port = OkidiaConfig::instance().brokerPort();
        
        // Zaten var mı kontrol et
        for (auto& b : m_brokers) {
            if (b.host == host || b.allHosts.contains(host)) {
                return;
            }
        }
        
        BrokerEntry entry;
        entry.name = tr("Manuel Sunucu (%1)").arg(host);
        entry.host = host;
        entry.port = port;
        entry.role = "Broker";
        entry.allHosts << host;
        
        m_brokers << entry;
        addBroker(entry);
        emit completeChanged();
    }
}

void BrokerDiscoveryPage::onScanTimeout() {
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    m_rescanBtn->setEnabled(true);
    if (m_brokers.isEmpty()) {
        m_statusLabel->setText(tr("Sunucu bulunamadı. Sunucu çalışıyor mu?"));
        setSubTitle(tr("Ağda OKIDIA broker bulunamadı."));
    } else {
        m_statusLabel->setText(
            QString(tr("%1 broker bulundu. Birini seçip devam edin."))
            .arg(m_brokers.size()));
        setSubTitle(tr("Bağlanmak istediğiniz broker'ı seçin."));
    }
}

void BrokerDiscoveryPage::onSelectionChanged() { emit completeChanged(); }

void BrokerDiscoveryPage::addBroker(const BrokerEntry& b) {
    QString displayName = b.name.trimmed();
    int cafpPos = displayName.indexOf("cafp=", 0, Qt::CaseInsensitive);
    if (cafpPos >= 0) {
        if (cafpPos > 0 && (displayName[cafpPos - 1] == 'd' || displayName[cafpPos - 1] == 'D' || displayName[cafpPos - 1].isSpace())) {
            cafpPos--;
        }
        displayName = displayName.left(cafpPos).trimmed();
    }
    if (displayName.isEmpty()) displayName = tr("OKIDIA Sunucu");

    const QString roleTag = (b.role == "MB") ? tr("  [Ana Sunucu]") : tr("  [Alt Sunucu]");
    const QString label   = QString("%1%2\n    %3:%4")
        .arg(displayName, roleTag, b.host).arg(b.port);

    auto* item = new QListWidgetItem(label, m_list);
    item->setData(Qt::UserRole,     displayName);
    item->setData(Qt::UserRole + 1, b.host);
    item->setData(Qt::UserRole + 2, b.port);
    item->setData(Qt::UserRole + 3, b.role);

    if (b.role == "MB") {
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
    }
}

bool BrokerDiscoveryPage::isComplete() const {
    return m_list->currentItem() != nullptr;
}

int BrokerDiscoveryPage::nextId() const {
    int mode = wizard()->field("setupMode").toInt();
    if (mode == SetupWizard::Mode_JoinOM) {
        return SetupWizard::Page_JoinCode;
    }
    return SetupWizard::Page_Code;
}

BrokerEntry BrokerDiscoveryPage::selectedBroker() const {
    auto* item = m_list->currentItem();
    if (!item) return {};
    // allHosts'u m_brokers'tan al
    const QString name = item->data(Qt::UserRole).toString();
    for (const auto& b : m_brokers)
        if (b.name == name) return b;
    BrokerEntry b;
    b.name = name;
    b.host = item->data(Qt::UserRole + 1).toString();
    b.port = static_cast<quint16>(item->data(Qt::UserRole + 2).toInt());
    b.role = item->data(Qt::UserRole + 3).toString();
    b.allHosts << b.host;
    return b;
}

// ─────────────────────────────────────────────────────────────────────────────
// SetupCodePage
// ─────────────────────────────────────────────────────────────────────────────

SetupCodePage::SetupCodePage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Kurulum Kodu"));
    setSubTitle(tr("Sunucu'nun kurulum kodu terminalde gösterilmiştir."));

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(12);

    auto* infoLbl = new QLabel(
        tr("Sunucu'nun kurulduğu bilgisayarda terminalde şunu görmüş olmalısınız:\n\n"
           "  OKIDIA — İLK KURULUM KODU\n"
           "  Kod: OKIDIA-XXXX-YYYY\n\n"
           "Aşağıdaki alana bu kodu girin."),
        this);
    infoLbl->setWordWrap(true);
    infoLbl->setProperty("status", "info");
    lay->addWidget(infoLbl);

    auto* form = new QFormLayout();
    m_codeEdit = new QLineEdit(this);
    m_codeEdit->setPlaceholderText("OKIDIA-XXXX-YYYY");
    m_codeEdit->setMaxLength(16);
    m_codeEdit->setAlignment(Qt::AlignCenter);
    m_codeEdit->setFont(QFont("monospace", 18));
    form->addRow(tr("Kurulum kodu:"), m_codeEdit);
    lay->addLayout(form);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setProperty("status", "error");
    lay->addWidget(m_hintLabel);
    lay->addStretch();

    registerField("setupCode*", m_codeEdit);
    connect(m_codeEdit, &QLineEdit::textChanged, this, &SetupCodePage::onCodeChanged);
}

void SetupCodePage::onCodeChanged(const QString& text) {
    const QString upper = text.toUpper();
    if (upper != text) {
        const int pos = m_codeEdit->cursorPosition();
        m_codeEdit->setText(upper);
        m_codeEdit->setCursorPosition(pos);
    }
    const bool valid = QRegularExpression(
        "^OKIDIA-[A-Z0-9]{4}-[A-Z0-9]{4}$").match(upper).hasMatch();
    m_hintLabel->setText(valid ? "" : "Format: OKIDIA-XXXX-YYYY");
    emit completeChanged();
}

bool SetupCodePage::isComplete() const {
    return QRegularExpression("^OKIDIA-[A-Z0-9]{4}-[A-Z0-9]{4}$")
        .match(m_codeEdit->text()).hasMatch();
}

bool SetupCodePage::validatePage() {
    int mode = wizard()->field("setupMode").toInt();
    if (mode == SetupWizard::Mode_RestoreIdentity) {
        return true; // Restore modunda doğrulamayı atla (FinishPage kendi hallediyor)
    }

    const auto* discPage = qobject_cast<BrokerDiscoveryPage*>(
        wizard()->page(SetupWizard::Page_Discovery));
    if (!discPage) return false;

    BrokerEntry broker = discPage->selectedBroker();
    if (broker.host.isEmpty() || broker.port == 0) return false;

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);

    QStringList hostsToTry;
    if (!broker.allHosts.isEmpty()) {
        hostsToTry = broker.allHosts;
    } else {
        hostsToTry << broker.host;
    }

    bool success = false;
    QString errorMsg = tr("Bağlantı zaman aşımına uğradı.");

    for (const QString& targetHost : hostsToTry) {
        if (success) break;

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(3000); // Her IP için 3 saniye

        TlsClient tls(this);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&tls, &TlsClient::disconnected, &loop, &QEventLoop::quit);

        connect(&tls, &TlsClient::connected, [&](Connection* conn) {
            connect(conn, &Connection::envelopeReceived, [&](const okidia::Envelope& env) {
                if (env.type() == okidia::MessageType::VERIFY_SETUP_CODE_RESPONSE) {
                    okidia::VerifySetupCodeResponse resp;
                    if (resp.ParseFromString(env.payload())) {
                        success = resp.success();
                        if (!success) {
                            errorMsg = QString::fromStdString(resp.error_msg());
                        } else {
                            errorMsg = "";
                        }
                    } else {
                        errorMsg = tr("Sunucu yanıtı anlaşılamadı.");
                    }
                    loop.quit();
                }
            });

            okidia::VerifySetupCodeRequest req;
            req.set_setup_code(m_codeEdit->text().toStdString());

            okidia::Envelope env;
            env.set_type(okidia::MessageType::VERIFY_SETUP_CODE_REQUEST);
            env.set_payload(req.SerializeAsString());
            conn->sendEnvelope(env);
        });

        tls.connectToServerAnonymous(targetHost, broker.port);
        timeout.start();
        loop.exec();
        tls.disconnectFromServer();

        // Eğer sunucu reddettiyse (success=false ama errorMsg değiştiyse) diğer IP'ye geçmeye gerek yok.
        // Ancak errorMsg hala "Bağlantı zaman aşımına uğradı." ise diğer IP'yi dene.
        if (errorMsg != tr("Bağlantı zaman aşımına uğradı.")) {
            break;
        }
    }

    if (success) {
        m_hintLabel->setText("");
        return true;
    } else {
        m_hintLabel->setText(errorMsg);
        return false;
    }
}

int SetupCodePage::nextId() const {
    int mode = wizard()->field("setupMode").toInt();
    if (mode == SetupWizard::Mode_RestoreIdentity) {
        return SetupWizard::Page_Finish;
    }
    return SetupWizard::Page_Account;
}

// ─────────────────────────────────────────────────────────────────────────────
// AdminAccountPage
// ─────────────────────────────────────────────────────────────────────────────

AdminAccountPage::AdminAccountPage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Yönetici Hesabı"));
    setSubTitle(tr("Sisteme ilk giriş için Yönetici (YM) hesabı oluşturun."));

    auto* lay  = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setSpacing(10);

    m_schoolNameEdit  = new QLineEdit(this);
    m_usernameEdit    = new QLineEdit(this);
    m_displayNameEdit = new QLineEdit(this);
    m_passwordEdit    = new QLineEdit(this);
    m_password2Edit   = new QLineEdit(this);

    m_schoolNameEdit->setPlaceholderText(tr("Örn: Atatürk Anadolu Lisesi veya Bilgisayar Laboratuvarı"));
    m_usernameEdit->setPlaceholderText("admin");
    m_displayNameEdit->setPlaceholderText(tr("Ahmet Yılmaz"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_password2Edit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("En az 8 karakter"));
    m_password2Edit->setPlaceholderText(tr("Şifreyi tekrar girin"));

    form->addRow(tr("Okul / Kurum Adı:"), m_schoolNameEdit);
    form->addRow(tr("Kullanıcı adı:"),   m_usernameEdit);
    form->addRow(tr("Ad Soyad:"),        m_displayNameEdit);
    form->addRow(tr("Şifre:"),           m_passwordEdit);
    form->addRow(tr("Şifre (tekrar):"),  m_password2Edit);
    lay->addLayout(form);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setProperty("status", "error");
    m_errorLabel->setWordWrap(true);
    lay->addWidget(m_errorLabel);
    lay->addStretch();

    registerField("schoolName*",       m_schoolNameEdit);
    registerField("adminUsername*",    m_usernameEdit);
    registerField("adminDisplayName*", m_displayNameEdit);
    registerField("adminPassword*",    m_passwordEdit);

    for (auto* e : {m_schoolNameEdit, m_usernameEdit, m_displayNameEdit, m_passwordEdit, m_password2Edit})
        connect(e, &QLineEdit::textChanged, this, &AdminAccountPage::onFieldChanged);
}

void AdminAccountPage::onFieldChanged() {
    m_errorLabel->clear();
    emit completeChanged();
}

bool AdminAccountPage::isComplete() const {
    return !m_schoolNameEdit->text().trimmed().isEmpty()
        && !m_usernameEdit->text().trimmed().isEmpty()
        && !m_displayNameEdit->text().trimmed().isEmpty()
        && m_passwordEdit->text().length() >= 8
        && !m_password2Edit->text().isEmpty();
}

bool AdminAccountPage::validatePage() {
    if (m_schoolNameEdit->text().trimmed().isEmpty()) {
        m_errorLabel->setText(tr("Lütfen bir okul veya kurum adı girin.")); return false;
    }
    if (m_passwordEdit->text() != m_password2Edit->text()) {
        m_errorLabel->setText(tr("Şifreler eşleşmiyor.")); return false;
    }
    if (m_usernameEdit->text().contains(' ')) {
        m_errorLabel->setText(tr("Kullanıcı adı boşluk içeremez.")); return false;
    }
    if (m_passwordEdit->text().length() < 8) {
        m_errorLabel->setText(tr("Şifre en az 8 karakter olmalı.")); return false;
    }
    return true;
}

int AdminAccountPage::nextId() const {
    return SetupWizard::Page_RecoveryKey;
}

// ─────────────────────────────────────────────────────────────────────────────
// RecoveryKeyPage
// ─────────────────────────────────────────────────────────────────────────────

RecoveryKeyPage::RecoveryKeyPage(QWidget* parent) : QWizardPage(parent) {
    setTitle(tr("Sistem Kurtarma Kodu"));
    setSubTitle(tr("Bu kodu güvenli bir yerde saklayın."));
    
    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(15);
    
    auto* info = new QLabel(
        tr("Aşağıdaki kod, yönetici şifrenizi unutmanız durumunda sisteminizi kurtarmanızı "
           "sağlayacak <b>tek</b> anahtardır. Lütfen bir kağıda not alın veya güvenli bir dijital "
           "kasada saklayın."), this);
    info->setWordWrap(true);
    lay->addWidget(info);
    
    m_keyLabel = new QLabel(this);
    m_keyLabel->setAlignment(Qt::AlignCenter);
    m_keyLabel->setProperty("class", "recoveryKey");
    lay->addWidget(m_keyLabel);
    
    m_saveBtn = new QPushButton(tr("Dosya Olarak Kaydet"), this);
    m_saveBtn->setIcon(ThemeManager::icon(":/ui/file.svg"));
    m_saveBtn->setProperty("class", "primary");
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(m_saveBtn);
    btnRow->addStretch();
    lay->addLayout(btnRow);
    
    lay->addStretch();

    registerField("recoveryKey", this, "recoveryKey");

    connect(m_saveBtn, &QPushButton::clicked, this, &RecoveryKeyPage::onSaveClicked);
}

void RecoveryKeyPage::initializePage() {
    // 16 karakterlik rastgele kod üret: XXXX-YYYY-ZZZZ-WWWW
    QString chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QString key;
    for(int i=0; i<16; ++i) {
        if(i > 0 && i % 4 == 0) key += "-";
        key += chars.at(QRandomGenerator::global()->bounded(chars.length()));
    }
    m_key = key;
    m_keyLabel->setText(m_key);
}

void RecoveryKeyPage::onSaveClicked() {
    QString defaultPath = QDir::homePath() + "/" + tr("okidia_kurtarma_kodu.txt");
    QString filePath = OkidiaUtils::getSaveFileName(this,
        tr("Kurtarma Kodunu Kaydet"),
        "recovery_key",
        defaultPath,
        tr("Metin Dosyaları (*.txt)"));

    if (!filePath.isEmpty()) {
        QFile file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << OkidiaUtils::getRecoveryKeyFileContent(m_key, tr("OKIDIA GÜVENLİK VE SİSTEM KURTARMA KODU"));
            file.close();

            QMessageBox::information(this, tr("Başarılı"), tr("Kurtarma kodu başarıyla kaydedildi:\n") + filePath);
        } else {
            QMessageBox::warning(this, tr("Hata"), tr("Dosya kaydedilirken bir hata oluştu."));
        }
    }
}

int RecoveryKeyPage::nextId() const {
    return SetupWizard::Page_Finish;
}

// ─────────────────────────────────────────────────────────────────────────────
// FinishPage
// ─────────────────────────────────────────────────────────────────────────────

FinishPage::FinishPage(QWidget* parent)
    : QWizardPage(parent)
    , m_tls(new TlsClient(this))
{
    setTitle(tr("Kurulum Tamamlanıyor"));
    setSubTitle(tr("Sunucu ile bağlantı kuruluyor ve sertifika alınıyor..."));
    setFinalPage(true);

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(12);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setMaximumHeight(8);
    lay->addWidget(m_progress);

    m_statusLabel = new QLabel(tr("Sunucu'ya bağlanılıyor..."), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setProperty("status", "info");
    lay->addWidget(m_statusLabel);

    m_retryBtn = new QPushButton(tr("↺  Yeniden Dene"), this);
    m_retryBtn->setVisible(false);
    lay->addWidget(m_retryBtn, 0, Qt::AlignCenter);

    lay->addStretch();

    connect(m_tls, &TlsClient::connected,    this, &FinishPage::onBrokerConnected);
    connect(m_tls, &TlsClient::disconnected, this, &FinishPage::onBrokerDisconnected);
    connect(m_retryBtn, &QPushButton::clicked, this, [this]() {
        m_retryBtn->setVisible(false);
        m_failed = false;
        m_progress->setRange(0, 0);  // belirsiz
        setStatus(tr("Sunucu'ya yeniden bağlanılıyor..."));
        // Sihirbazı yeniden kapatılamaz ve geri gidilemez yap
        if (wizard()) {
            wizard()->setOption(QWizard::NoBackButtonOnLastPage, true);
            wizard()->setOption(QWizard::NoCancelButtonOnLastPage, true);
            int mode = wizard()->field("setupMode").toInt();
            if (mode == SetupWizard::Mode_NewYM) {
                wizard()->setOption(QWizard::NoCancelButton, true);
            }
        }
        QTimer::singleShot(300, this, &FinishPage::connectToBroker);
    });
}

void FinishPage::initializePage() {
    m_done   = false;
    m_failed = false;
    m_retryBtn->setVisible(false);
    // Başlangıçta/Hata yokken Cancel ve Back butonlarını gizle
    if (wizard()) {
        wizard()->setOption(QWizard::NoBackButtonOnLastPage, true);
        wizard()->setOption(QWizard::NoCancelButtonOnLastPage, true);
        int mode = wizard()->field("setupMode").toInt();
        if (mode == SetupWizard::Mode_NewYM) {
            wizard()->setOption(QWizard::NoCancelButton, true);
        }
    }
    setStatus(tr("Sunucu'ya bağlanılıyor..."));
    QTimer::singleShot(500, this, &FinishPage::connectToBroker);
}

void FinishPage::connectToBroker() {
    m_generatedKeyPath = kKeyPath;
    // master.key yoksa, boşsa veya açılamıyorsa yeniden üretmeyi dene
    bool needGenerate = false;
    if (!QFile::exists(m_generatedKeyPath)) {
        needGenerate = true;
    } else {
        QFile kf(kKeyPath);
        if (!kf.open(QIODevice::ReadOnly) || kf.size() == 0) {
            needGenerate = true;
        }
    }

    if (needGenerate) {
        QDir().mkpath(kCertDir);
        // Eğer dosya varsa ama bozuksa kaldır
        if (QFile::exists(kKeyPath)) {
            QFile::remove(kKeyPath);
        }

        if (!KeyManager::generateKeyPair(kKeyPath, kKeyPath + ".pub")) {
            setStatus(tr("master.key bulunamadı ve otomatik üretilemedi.\n"
                         "Lütfen dizin izinlerini kontrol edin veya paketi yeniden yükleyin.\n"
                         "Dizin: ") + kCertDir, true);
            m_failed = true;
            emit completeChanged();
            return;
        }
        
        OkidiaUtils::securePrivateKeyFile(kKeyPath, false);
    }

    // Her durumda okunabilirliği doğrula
    {
        QFile kf(kKeyPath);
        if (!kf.open(QIODevice::ReadOnly)) {
            setStatus(tr("master.key okunamadı — izin hatası.\n"
                         "Dosya: ") + kKeyPath, true);
            m_failed = true;
            emit completeChanged();
            return;
        }
    }

    // CSR'ı bağlantıdan ÖNCE üret — bağlantı sırasında event loop bloke olmasın (Saf C++ OpenSSL)
    const QString csrPath = QDir::tempPath() + "/okidia-master-setup.csr";
    if (QFile::exists(csrPath)) {
        QFile::remove(csrPath);
    }
    
    if (!KeyManager::generateCsr(kKeyPath, csrPath, "okidia-master-" + QSysInfo::machineHostName())) {
        setStatus(tr("CSR üretilemedi: ") + kKeyPath, true);
        m_failed = true;
        emit completeChanged();
        return;
    }

    setStatus(tr("Sunucu'ya bağlanılıyor..."));

    const auto* discPage = qobject_cast<BrokerDiscoveryPage*>(
        wizard()->page(SetupWizard::Page_Discovery));
    if (!discPage) { m_failed = true; emit completeChanged(); return; }

    const BrokerEntry broker = discPage->selectedBroker();
    m_candidateHosts = broker.allHosts.isEmpty()
        ? QStringList{broker.host} : broker.allHosts;
    m_candidatePort  = broker.port;
    m_currentHostIdx = 0;
    tryNextHost();
}

void FinishPage::tryNextHost() {
    if (m_currentHostIdx >= m_candidateHosts.size()) {
        setStatus(tr("Sunucu'ya ulaşılamadı."), true);
        m_failed = true;
        emit completeChanged();
        return;
    }
    const QString host = m_candidateHosts.at(m_currentHostIdx);
    setStatus(QString(tr("Bağlanılıyor: %1 (%2/%3)..."))
              .arg(host).arg(m_currentHostIdx + 1)
              .arg(m_candidateHosts.size()));
    m_tls->connectToServerAnonymous(host, m_candidatePort);
}

void FinishPage::onBrokerConnected(Connection* conn) {
    Logger::info(QObject::tr("SetupWizard: onBrokerConnected tetiklendi"));
    
    // Bağlanılan IP'yi kaydet
    m_connectedHost = m_candidateHosts.at(m_currentHostIdx);
    
    setStatus(tr("Bağlantı kuruldu. İstek gönderiliyor..."));
    connect(conn, &Connection::envelopeReceived,
            this, &FinishPage::onMessageReceived);
    
    int mode = wizard()->field("setupMode").toInt();
    if (mode == SetupWizard::Mode_JoinOM) {
        sendJoinRequest(conn);
    } else if (mode == SetupWizard::Mode_RestoreIdentity) {
        sendRestoreRequest(conn);
    } else {
        sendSetupRequest(conn);
    }
}

void FinishPage::onBrokerDisconnected() {
    if (m_done) return;
    m_currentHostIdx++;
    if (m_currentHostIdx < m_candidateHosts.size()) {
        setStatus(QString(tr("Bağlantı kurulamadı, deneniyor (%1/%2)..."))
                  .arg(m_currentHostIdx + 1).arg(m_candidateHosts.size()));
        QTimer::singleShot(800, this, &FinishPage::tryNextHost);
    } else {
        setStatus(tr("Hiçbir IP üzerinden bağlantı kurulamadı.\n"
                     "Sunucu'nun çalıştığından ve aynı ağda olduğunuzdan emin olun."), true);
        m_failed = true;
        m_retryBtn->setVisible(true);  // Yeniden Dene butonu göster
        // Hata durumunda Cancel butonu göster (kapatılabilsin)
        if (wizard())
            wizard()->setOption(QWizard::NoCancelButtonOnLastPage, false);
        emit completeChanged();
    }
}

void FinishPage::sendSetupRequest(Connection* conn) {
    Logger::info(QObject::tr("SetupWizard: sendSetupRequest başladı"));
    const QString csrPath = QDir::tempPath() + "/okidia-master-setup.csr";

    QFile csrFile(csrPath);
    if (!csrFile.open(QIODevice::ReadOnly)) {
        setStatus(tr("CSR dosyası okunamadı."), true);
        m_failed = true;
        emit completeChanged();
        return;
    }
    const QByteArray csrPem = csrFile.readAll();
    if (csrPem.isEmpty()) {
        setStatus(tr("CSR dosyası boş."), true);
        m_failed = true;
        emit completeChanged();
        return;
    }

    const QString pwHash = OkidiaUtils::sha256(
        field("adminPassword").toString().toUtf8());

    okidia::SetupRequest req;
    req.set_setup_code(field("setupCode").toString().toStdString());
    req.set_school_name(field("schoolName").toString().trimmed().toStdString());
    req.set_csr_pem(csrPem.toStdString());
    req.set_hostname(QSysInfo::machineHostName().toStdString());
    req.set_username(field("adminUsername").toString().toStdString());
    req.set_display_name(field("adminDisplayName").toString().toStdString());
    req.set_password_hash(pwHash.toStdString());

    const QString recKey = field("recoveryKey").toString();
    if (!recKey.isEmpty()) {
        const QString cleanKey = OkidiaUtils::cleanRecoveryKey(recKey);
        const QString recHash = OkidiaUtils::sha256(cleanKey.toUtf8());
        req.set_recovery_key_hash(recHash.toStdString());
    }

    Logger::info(QObject::tr("SetupWizard: setup_code → ") + QString::fromStdString(req.setup_code()));
    Logger::info(QObject::tr("SetupWizard: username → ") + QString::fromStdString(req.username()));
    Logger::info(QObject::tr("SetupWizard: csr boyutu → ") + QString::number(req.csr_pem().size()));

    okidia::Envelope env;
    env.set_type(okidia::MessageType::SETUP_REQUEST);
    env.set_payload(req.SerializeAsString());  // tek serialize
    Logger::info(QObject::tr("SetupWizard: payload boyutu → ") + QString::number(env.payload().size()));

    const bool sent = conn->sendEnvelope(env);
    Logger::info(QObject::tr("SetupWizard: sendEnvelope → ") + QString(sent ? QObject::tr("OK") : QObject::tr("BAŞARISIZ")));
    setStatus(tr("Sertifika isteği gönderildi, yanıt bekleniyor..."));
}

void FinishPage::sendJoinRequest(Connection* conn) {
    Logger::info(QObject::tr("SetupWizard: sendJoinRequest başladı"));
    const QString csrPath = QDir::tempPath() + "/okidia-master-setup.csr";
    QFile csrFile(csrPath);
    if (!csrFile.open(QIODevice::ReadOnly)) {
        setStatus(tr("CSR dosyası okunamadı."), true);
        m_failed = true; emit completeChanged(); return;
    }
    const QByteArray csrPem = csrFile.readAll();

    // JoinCodePage'den şifreyi al ve hash'le
    const auto* joinPage = qobject_cast<JoinCodePage*>(
        wizard()->page(SetupWizard::Page_JoinCode));
    QString pwHash;
    if (joinPage && !joinPage->password().isEmpty()) {
        pwHash = OkidiaUtils::sha256(joinPage->password().toUtf8());
    }

    okidia::JoinRequest req;
    req.set_join_code(field("joinCode").toString().toStdString());
    req.set_csr_pem(csrPem.toStdString());
    req.set_hostname(QSysInfo::machineHostName().toStdString());
    if (!pwHash.isEmpty()) {
        req.set_password_hash(pwHash.toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::JOIN_REQUEST);
    env.set_payload(req.SerializeAsString());

    conn->sendEnvelope(env);
    setStatus(tr("Katılım isteği gönderildi, yanıt bekleniyor..."));
}

void FinishPage::sendRestoreRequest(Connection* conn) {
    Logger::info(QObject::tr("SetupWizard: sendRestoreRequest başladı"));
    QString backupPath = field("restoreBackupPath").toString();
    QString password = field("restorePassword").toString();

    BackupContent content;
    QString err;
    if (!BackupPackage::unpackFromFile(backupPath, password, content, &err)) {
        setStatus(tr("Yedek paketi açılamadı: ") + err, true);
        m_failed = true; emit completeChanged(); return;
    }

    okidia::DbRestoreRequest req;
    req.set_encrypted_db(content.databaseData.toStdString());
    
    QString setupCode = field("setupCode").toString();
    if (!setupCode.isEmpty()) {
        req.set_setup_code(setupCode.toStdString());
    }

    for (auto it = content.certFiles.begin(); it != content.certFiles.end(); ++it) {
        auto* idFile = req.add_identity_bundle();
        idFile->set_name(it.key().toStdString());
        idFile->set_content(it.value().toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::DB_RESTORE_REQUEST);
    env.set_payload(req.SerializeAsString());
    conn->sendEnvelope(env);

    setStatus(tr("Kurtarma isteği gönderildi, yanıt bekleniyor..."));
}

void FinishPage::onMessageReceived(const okidia::Envelope& env) {
    if (env.type() == okidia::MessageType::SETUP_RESPONSE) {
        okidia::SetupResponse resp;
        if (!resp.ParseFromString(env.payload())) {
            setStatus(tr("Yanıt ayrıştırılamadı."), true);
            m_failed = true; emit completeChanged(); return;
        }
        if (!resp.success()) {
            setStatus(tr("Hata: ") + QString::fromStdString(resp.error_msg()), true);
            m_failed = true; emit completeChanged(); return;
        }

        QString adminUser = field("adminUsername").toString().trimmed();
        QString schoolName = QString::fromStdString(resp.school_name()).trimmed();
        if (schoolName.isEmpty()) {
            schoolName = field("schoolName").toString().trimmed();
        }

        QSslCertificate caCert(QByteArray::fromStdString(resp.ca_cert()));
        QString caFp = QString::fromUtf8(caCert.digest(QCryptographicHash::Sha256).toHex()).toLower();
        QString networkKey = OkidiaConfig::networkKey(schoolName, caFp);

        QSettings s("okidia", "master");
        s.setValue("school/name", schoolName);
        s.setValue("net/activeNetworkKey", networkKey);
        s.setValue("net/activeCaFingerprint", caFp);
        if (!adminUser.isEmpty()) {
            s.setValue("last_username", adminUser);
        }

        const QString profileName = "DefaultAdmin";
        if (!saveCerts(QString::fromStdString(resp.master_cert()),
                       QString::fromStdString(resp.ca_cert()),
                       profileName,
                       networkKey)) {
            setStatus(tr("Sertifikalar kaydedilemedi."), true);
            m_failed = true; emit completeChanged(); return;
        }
        completeSetup(profileName, networkKey);
    } else if (env.type() == okidia::MessageType::JOIN_RESPONSE) {
        okidia::JoinResponse resp;
        if (!resp.ParseFromString(env.payload())) {
            setStatus(tr("Yanıt ayrıştırılamadı."), true);
            m_failed = true; emit completeChanged(); return;
        }
        if (!resp.success()) {
            setStatus(tr("Hata: ") + QString::fromStdString(resp.error_msg()), true);
            m_failed = true; emit completeChanged(); return;
        }

        QString schoolName = QString::fromStdString(resp.school_name()).trimmed();
        QSslCertificate caCert(QByteArray::fromStdString(resp.ca_cert()));
        QString caFp = QString::fromUtf8(caCert.digest(QCryptographicHash::Sha256).toHex()).toLower();
        QString networkKey = OkidiaConfig::networkKey(schoolName, caFp);

        QSettings s("okidia", "master");
        if (!schoolName.isEmpty()) {
            s.setValue("school/name", schoolName);
        }
        s.setValue("net/activeNetworkKey", networkKey);
        s.setValue("net/activeCaFingerprint", caFp);

        // Broker'dan dönen kullanıcı adını QSettings'e kaydet → LoginDialog otomatik doldursun
        const QString joinUsername = QString::fromStdString(resp.username());
        if (!joinUsername.isEmpty()) {
            s.setValue("last_username", joinUsername);
            Logger::info(QObject::tr("SetupWizard: Katılım kullanıcı adı kaydedildi → ") + joinUsername);
        }

        const QString profileName = "DefaultAdmin";
        if (!saveCerts(QString::fromStdString(resp.master_cert()),
                       QString::fromStdString(resp.ca_cert()),
                       profileName,
                       networkKey)) {
            setStatus(tr("Sertifikalar kaydedilemedi."), true);
            m_failed = true; emit completeChanged(); return;
        }
        completeSetup(profileName, networkKey);
    } else if (env.type() == okidia::MessageType::DB_RESTORE_RESPONSE) {
        okidia::DbRestoreResponse resp;
        if (!resp.ParseFromString(env.payload())) {
            setStatus(tr("Yanıt ayrıştırılamadı."), true);
            m_failed = true; emit completeChanged(); return;
        }
        if (!resp.success()) {
            setStatus(tr("Hata: ") + QString::fromStdString(resp.error_msg()), true);
            m_failed = true; emit completeChanged(); return;
        }

        QString backupPath = field("restoreBackupPath").toString();
        QString password = field("restorePassword").toString();

        BackupContent content;
        if (BackupPackage::unpackFromFile(backupPath, password, content)) {
            QString caFp;
            if (content.certFiles.contains("ca.crt")) {
                QSslCertificate caCert(content.certFiles["ca.crt"]);
                caFp = QString::fromUtf8(caCert.digest(QCryptographicHash::Sha256).toHex()).toLower();
            }
            QString schoolName = content.meta.description.trimmed();
            QString networkKey = OkidiaConfig::networkKey(schoolName, caFp);

            QSettings s("okidia", "master");
            if (!schoolName.isEmpty()) {
                s.setValue("school/name", schoolName);
            }
            if (!networkKey.isEmpty()) {
                s.setValue("net/activeNetworkKey", networkKey);
                s.setValue("net/activeCaFingerprint", caFp);
            }

            QString userId;
            for (auto it = content.certFiles.begin(); it != content.certFiles.end(); ++it) {
                if (it.key().startsWith("master_") && it.key().endsWith(".crt")) {
                    userId = it.key().mid(7, it.key().length() - 11);
                    break;
                }
            }
            if (userId.isEmpty()) userId = "DefaultAdmin";

            QString netCertDir = OkidiaConfig::userCertDir(userId, networkKey);
            QDir().mkpath(netCertDir);

            if (content.certFiles.contains("ca.crt")) {
                QFile f(OkidiaConfig::userCaPath(userId, networkKey));
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(content.certFiles["ca.crt"]);
                    f.close();
                    Logger::info(QObject::tr("SetupWizard: Kurtarılan Kök CA sertifikası yerleştirildi → ") + f.fileName());
                }
            }

            for (auto it = content.certFiles.begin(); it != content.certFiles.end(); ++it) {
                if (it.key().startsWith("master_") && it.key().endsWith(".crt")) {
                    QFile f(OkidiaConfig::userCertPath(userId, networkKey));
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(it.value());
                        f.close();
                    }
                }
                if (it.key().startsWith("master_") && it.key().endsWith(".key")) {
                    QFile f(OkidiaConfig::userKeyPath(userId, networkKey));
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(it.value());
                        f.close();
                        OkidiaUtils::securePrivateKeyFile(f.fileName(), false);
                    }
                }
            }

            if (!content.databaseData.isEmpty()) {
                QString dbPath = OkidiaConfig::masterDbPath(userId, networkKey);
                QDir().mkpath(QFileInfo(dbPath).path());
                QFile f(dbPath);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(content.databaseData);
                    f.close();
                    Logger::info(QObject::tr("SetupWizard: Veritabanı geri yüklendi → ") + dbPath);
                }
            }

            completeSetup(userId, networkKey);
        } else {
            completeSetup();
        }
    }
}

void FinishPage::completeSetup(const QString& userId, const QString& networkKey) {
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    setStatus(tr("Kurulum tamamlandı! OKIDIA başlatılıyor..."));
    m_done = true;

    // Kurulum kodu kullanıldığı için masaüstünde varsa dosyayı temizle
    const QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!desktopPath.isEmpty()) {
        QFile::remove(desktopPath + "/OKIDIA_Kurulum_Kodu.txt");
        QFile::remove(desktopPath + "/OKIDIA_Setup_Code.txt");
    }
    
    emit completeChanged();
    emit setupComplete(OkidiaConfig::userCertPath(userId, networkKey),
                       OkidiaConfig::userKeyPath(userId, networkKey),
                       OkidiaConfig::userCaPath(userId, networkKey));
}

bool FinishPage::saveCerts(const QString& certPem, const QString& caPem, const QString& userId, const QString& networkKey) {
    QString netKey = networkKey.trimmed();
    if (netKey.isEmpty()) {
        netKey = OkidiaConfig::currentNetworkKey();
    }
    const QString profile = userId.isEmpty() ? "DefaultAdmin" : userId;
    QString targetCert   = OkidiaConfig::userCertPath(profile, netKey);
    QString targetKey    = OkidiaConfig::userKeyPath(profile, netKey);
    QString targetCa     = OkidiaConfig::userCaPath(profile, netKey);

    QDir().mkpath(QFileInfo(targetCert).path());
    QDir().mkpath(QFileInfo(targetKey).path());
    QDir().mkpath(QFileInfo(targetCa).path());

    auto write = [](const QString& path, const QString& data,
                    QFile::Permissions perms) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            Logger::error(QObject::tr("SetupWizard: Dosya yazılamadı → ") + path);
            return false;
        }
        f.write(data.toUtf8());
        f.close();
        f.setPermissions(perms);
        return true;
    };

    const auto pubPerms = QFile::ReadOwner | QFile::WriteOwner |
                          QFile::ReadGroup | QFile::ReadOther;

    // Üretilen geçici private key'i hedef ağ dizinine taşı
    QString sourceKey = m_generatedKeyPath;
    if (sourceKey.isEmpty() || !QFile::exists(sourceKey)) {
        QString fallbackSource = OkidiaConfig::userKeyPath("DefaultAdmin", "Network");
        if (QFile::exists(fallbackSource)) {
            sourceKey = fallbackSource;
        }
    }

    bool keyMoved = false;
    if (QFile::exists(sourceKey) && sourceKey != targetKey) {
        QFile::remove(targetKey);
        if (QFile::copy(sourceKey, targetKey)) {
            OkidiaUtils::securePrivateKeyFile(targetKey, false);
            keyMoved = true;
            if (QFile::exists(sourceKey + ".pub")) {
                QFile::remove(targetKey + ".pub");
                QFile::copy(sourceKey + ".pub", targetKey + ".pub");
            }
        } else {
            Logger::error(QObject::tr("SetupWizard: Private key hedef yola taşınamadı: ") + sourceKey + " -> " + targetKey);
        }
    } else if (QFile::exists(targetKey)) {
        keyMoved = true;
    }

    bool wCert = write(targetCert, certPem, pubPerms);
    bool wCa   = write(targetCa,   caPem,   pubPerms);

    // Atomik doğrulama: 3 dosyanın da diskte var ve boyutu > 0 olduğunu teyit et
    bool certOk = QFile::exists(targetCert) && QFile(targetCert).size() > 0;
    bool keyOk  = QFile::exists(targetKey) && QFile(targetKey).size() > 0;
    bool caOk   = QFile::exists(targetCa) && QFile(targetCa).size() > 0;

    if (!wCert || !wCa || !certOk || !keyOk || !caOk) {
        Logger::error(QObject::tr("SetupWizard: Sertifika doğrulama başarısız! cert=%1 (w=%2), key=%3 (moved=%4, src=%5), ca=%6 (w=%7)")
            .arg(certOk).arg(wCert).arg(keyOk).arg(keyMoved).arg(sourceKey).arg(caOk).arg(wCa));
        return false;
    }

    // Doğrulama başarılı olduktan sonra eski geçici anahtarları temizle
    if (keyMoved && QFile::exists(sourceKey) && sourceKey != targetKey) {
        QFile::remove(sourceKey);
        QFile::remove(sourceKey + ".pub");
    }

    OkidiaConfig::cleanupGhostNetworks();
    return true;
}

void FinishPage::setStatus(const QString& msg, bool error) {
    m_statusLabel->setText(msg);
    m_statusLabel->setProperty("status", error ? "error" : "info");
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
    if (error) { 
        m_progress->setRange(0, 1); 
        m_progress->setValue(0); 
        m_retryBtn->setVisible(true);
        if (wizard()) {
            wizard()->setOption(QWizard::NoBackButtonOnLastPage, false);
            wizard()->setOption(QWizard::NoCancelButtonOnLastPage, false);
            wizard()->setOption(QWizard::NoCancelButton, false);
        }
    }
}

bool FinishPage::isComplete() const { return m_done; }

// ─────────────────────────────────────────────────────────────────────────────
// SetupWizard
// ─────────────────────────────────────────────────────────────────────────────

SetupWizard::SetupWizard(QWidget* parent) : QWizard(parent) {
    setWindowTitle(tr("OKIDIA — Kurulum Sihirbazı"));
    setMinimumSize(580, 500);
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnLastPage);
    setOption(QWizard::NoCancelButtonOnLastPage);

    setPage(Page_Welcome,   new WelcomePage(this));
    setPage(Page_Choice,    new ChoicePage(this));
    setPage(Page_Discovery, new BrokerDiscoveryPage(this));
    setPage(Page_Code,      new SetupCodePage(this));
    setPage(Page_JoinCode,  new JoinCodePage(this));
    setPage(Page_Account,   new AdminAccountPage(this));
    setPage(Page_RecoveryKey, new RecoveryKeyPage(this));
    setPage(Page_RestoreSelect, new RestoreSelectPage(this));

    auto* finish = new FinishPage(this);
    setPage(Page_Finish, finish);

    connect(finish, &FinishPage::setupComplete,
            this,   &SetupWizard::onSetupComplete);
    connect(this, &QWizard::currentIdChanged,
            this, &SetupWizard::onPageChanged);
}

void SetupWizard::reject() {
    if (currentId() == Page_Finish) {
        FinishPage* finishPage = qobject_cast<FinishPage*>(currentPage());
        if (finishPage && !finishPage->isComplete() && !finishPage->hasError()) {
            return; // Hata yoksa ve devam ediyorsa iptali engelle
        }
    }
    QWizard::reject();
}

void SetupWizard::onPageChanged(int id) {
    int mode = field("setupMode").toInt();
    if (mode == Mode_NewYM && id >= Page_Code) {
        setOption(QWizard::NoCancelButton, true);
    } else {
        setOption(QWizard::NoCancelButton, false);
    }
}

void SetupWizard::onSetupComplete(const QString& certPath,
                                  const QString& keyPath,
                                  const QString& caPath) {
    m_masterCertPath = certPath;
    m_masterKeyPath  = keyPath;
    m_caCertPath     = caPath;
}

// ─────────────────────────────────────────────────────────────────────────────
// RecoveryWizard
// ─────────────────────────────────────────────────────────────────────────────

RecoveryWizard::RecoveryWizard(QWidget* parent) : QWizard(parent) {
    setWindowTitle(tr("OKIDIA — Geri Yükleme Kurtarma Sihirbazı"));
    setMinimumSize(580, 480);
    setWizardStyle(QWizard::ModernStyle);

    setPage(Page_Discovery, new BrokerDiscoveryPage(this));
    setPage(Page_Code,      new SetupCodePage(this));
}

BrokerEntry RecoveryWizard::selectedBroker() const {
    auto* page = qobject_cast<BrokerDiscoveryPage*>(this->page(Page_Discovery));
    return page ? page->selectedBroker() : BrokerEntry();
}

QString RecoveryWizard::setupCode() const {
    return field("setupCode").toString();
}