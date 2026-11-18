// master/ui/CustomProfilesDialog/CustomProfilesDialog.cpp
#include "CustomProfilesDialog.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/db/OkidiaDatabase.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QIcon>

CustomProfilesDialog::CustomProfilesDialog(SessionManager* session, QWidget* parent)
    : QDialog(parent), m_session(session)
{
    setupUi();

    if (m_session) {
        connect(m_session, &SessionManager::customProfileUpsertResult, this, [this](bool success, const QString& errorMsg, int id) {
            if (success) {
                m_lblStatus->setText(tr("Profil başarıyla kaydedildi."));
                m_lblStatus->setStyleSheet("color: #27ae60; font-weight: bold;");
                refreshProfileList(id);
            } else {
                m_lblStatus->setText(tr("Kayıt hatası: ") + errorMsg);
                m_lblStatus->setStyleSheet("color: #e74c3c; font-weight: bold;");
            }
        });

        connect(m_session, &SessionManager::customProfileDeleteResult, this, [this](bool success, const QString& errorMsg) {
            if (success) {
                m_lblStatus->setText(tr("Profil silindi."));
                m_lblStatus->setStyleSheet("color: #27ae60; font-weight: bold;");
                refreshProfileList();
            } else {
                m_lblStatus->setText(tr("Silme hatası: ") + errorMsg);
                m_lblStatus->setStyleSheet("color: #e74c3c; font-weight: bold;");
            }
        });

        connect(m_session, &SessionManager::databaseReloaded, this, [this]() {
            refreshProfileList(m_currentProfileId);
        });
    }

    connect(&OkidiaDatabase::instance(), &OkidiaDatabase::customProfilesChanged, this, [this]() {
        refreshProfileList(m_currentProfileId);
    });

    refreshProfileList();
}

void CustomProfilesDialog::setupUi() {
    setWindowTitle(tr("Özel İstemci Tipleri Yönetimi"));
    setWindowIcon(ThemeManager::icon(":/ui/custom_profile.svg"));
    resize(700, 480);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Başlık ve Açıklama Alanı
    auto* headerLayout = new QHBoxLayout();
    auto* headerIcon = new QLabel(this);
    headerIcon->setPixmap(ThemeManager::icon(":/ui/custom_profile.svg").pixmap(32, 32));
    headerLayout->addWidget(headerIcon);

    auto* titleLayout = new QVBoxLayout();
    auto* lblTitle = new QLabel(tr("Kullanıcı Tanımlı Özel Öğrenci Profilleri"), this);
    QFont fTitle = lblTitle->font();
    fTitle.setPointSize(12);
    fTitle.setBold(true);
    lblTitle->setFont(fTitle);

    auto* lblDesc = new QLabel(tr("Zil & Amfi PC veya Özel Eğitim gibi okul zilinden ya da teneffüs kilidinden muaf hibrit profilleri yapılandırın."), this);
    lblDesc->setWordWrap(true);
    lblDesc->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");

    titleLayout->addWidget(lblTitle);
    titleLayout->addWidget(lblDesc);
    headerLayout->addLayout(titleLayout, 1);
    mainLayout->addLayout(headerLayout);

    // Ana Gövde (Sol: Liste, Sağ: Detay)
    auto* bodyLayout = new QHBoxLayout();
    bodyLayout->setSpacing(16);

    // Sol Panel
    auto* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(new QLabel(tr("Tanımlı Özel Tipler:"), this));

    m_listProfiles = new QListWidget(this);
    m_listProfiles->setIconSize(QSize(20, 20));
    leftLayout->addWidget(m_listProfiles, 1);

    auto* listBtnLayout = new QHBoxLayout();
    m_btnAdd = new QPushButton(ThemeManager::icon(":/ui/add.svg"), tr("Yeni Tip Ekle"), this);
    m_btnDelete = new QPushButton(ThemeManager::icon(":/ui/trash.svg"), tr("Sil"), this);
    listBtnLayout->addWidget(m_btnAdd);
    listBtnLayout->addWidget(m_btnDelete);
    leftLayout->addLayout(listBtnLayout);

    bodyLayout->addLayout(leftLayout, 2);

    // Sağ Panel (Form Detayı)
    m_detailGroup = new QGroupBox(tr("Profil Özellikleri"), this);
    auto* rightLayout = new QVBoxLayout(m_detailGroup);
    rightLayout->setContentsMargins(14, 16, 14, 14);
    rightLayout->setSpacing(12);

    auto* formLayout = new QFormLayout();
    formLayout->setSpacing(10);
    formLayout->setLabelAlignment(Qt::AlignLeft);

    m_txtName = new QLineEdit(this);
    m_txtName->setPlaceholderText(tr("Örn: Zil & Amfi PC, Özel Eğitim"));
    formLayout->addRow(tr("Görünür İsim:"), m_txtName);

    m_chkPlayBell = new QCheckBox(tr("Okul Zili Çalsın (Amfi / Hoparlör)"), this);
    auto* lblBellHint = new QLabel(tr("İşaretliyse ders ve teneffüs saatlerinde okul zili bu bilgisayarda çalınır."), this);
    lblBellHint->setStyleSheet("color: palette(placeholder-text); font-size: 11px; margin-left: 20px;");
    auto* bellBox = new QVBoxLayout();
    bellBox->setSpacing(2);
    bellBox->addWidget(m_chkPlayBell);
    bellBox->addWidget(lblBellHint);
    formLayout->addRow("", bellBox);

    m_chkRecessLock = new QCheckBox(tr("Teneffüste Kilitlensin / Kapatılsın"), this);
    auto* lblLockHint = new QLabel(tr("İşaretsiz bırakılırsa teneffüs kilit ve oturum kapatma süreçlerinden muaf tutulur."), this);
    lblLockHint->setStyleSheet("color: palette(placeholder-text); font-size: 11px; margin-left: 20px;");
    auto* lockBox = new QVBoxLayout();
    lockBox->setSpacing(2);
    lockBox->addWidget(m_chkRecessLock);
    lockBox->addWidget(lblLockHint);
    formLayout->addRow("", lockBox);

    m_chkChatEnabled = new QCheckBox(tr("Sohbete Katılabilir"), this);
    auto* lblChatHint = new QLabel(tr("Öğretmen ve sınıf içi sohbet odalarına dahil olma izni."), this);
    lblChatHint->setStyleSheet("color: palette(placeholder-text); font-size: 11px; margin-left: 20px;");
    auto* chatBox = new QVBoxLayout();
    chatBox->setSpacing(2);
    chatBox->addWidget(m_chkChatEnabled);
    chatBox->addWidget(lblChatHint);
    formLayout->addRow("", chatBox);

    rightLayout->addLayout(formLayout);

    m_lblAssignedCount = new QLabel(this);
    m_lblAssignedCount->setStyleSheet("font-weight: bold; color: palette(highlight); font-size: 11px;");
    rightLayout->addWidget(m_lblAssignedCount);

    m_lblStatus = new QLabel(this);
    m_lblStatus->setWordWrap(true);
    rightLayout->addWidget(m_lblStatus);

    rightLayout->addStretch(1);

    auto* actionLayout = new QHBoxLayout();
    actionLayout->addStretch();
    m_btnSave = new QPushButton(ThemeManager::icon(":/ui/save.svg"), tr("Değişiklikleri Kaydet"), this);
    m_btnSave->setDefault(true);
    actionLayout->addWidget(m_btnSave);
    rightLayout->addLayout(actionLayout);

    bodyLayout->addWidget(m_detailGroup, 3);
    mainLayout->addLayout(bodyLayout, 1);

    // Kapat Butonu
    auto* bbox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    mainLayout->addWidget(bbox);

    // Olay Bağlantıları
    connect(m_listProfiles, &QListWidget::currentRowChanged, this, &CustomProfilesDialog::onProfileSelected);
    connect(m_btnAdd, &QPushButton::clicked, this, &CustomProfilesDialog::onAddNewClicked);
    connect(m_btnDelete, &QPushButton::clicked, this, &CustomProfilesDialog::onDeleteClicked);
    connect(m_btnSave, &QPushButton::clicked, this, &CustomProfilesDialog::onSaveClicked);
}

void CustomProfilesDialog::refreshProfileList(int selectId) {
    m_listProfiles->blockSignals(true);
    m_listProfiles->clear();

    m_profiles = OkidiaDatabase::instance().allCustomProfiles();
    int targetRow = -1;

    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& prof = m_profiles[i];
        QString label = QString("%1 (ID: %2)").arg(prof.name).arg(prof.id);
        auto* item = new QListWidgetItem(ThemeManager::icon(":/ui/custom_profile.svg"), label, m_listProfiles);
        item->setData(Qt::UserRole, prof.id);

        if (selectId > 0 && prof.id == selectId) {
            targetRow = i;
        }
    }

    m_listProfiles->blockSignals(false);

    if (m_profiles.isEmpty()) {
        onAddNewClicked();
    } else {
        if (targetRow < 0) targetRow = 0;
        m_listProfiles->setCurrentRow(targetRow);
    }
}

void CustomProfilesDialog::onProfileSelected(int row) {
    if (row < 0 || row >= m_profiles.size()) {
        m_btnDelete->setEnabled(false);
        return;
    }

    const auto& prof = m_profiles[row];
    m_currentProfileId = prof.id;

    m_txtName->setText(prof.name);
    m_chkPlayBell->setChecked(prof.playBell);
    m_chkRecessLock->setChecked(prof.recessLock);
    m_chkChatEnabled->setChecked(prof.chatEnabled);

    int count = OkidiaDatabase::instance().countAgentsWithCustomProfile(prof.id);
    if (count > 0) {
        m_lblAssignedCount->setText(tr("Bu profile atanmış %1 adet bilgisayar bulunmaktadır.").arg(count));
    } else {
        m_lblAssignedCount->setText(tr("Bu profile henüz atanmış bir bilgisayar bulunmuyor."));
    }

    m_btnDelete->setEnabled(true);
    m_lblStatus->clear();
}

void CustomProfilesDialog::onAddNewClicked() {
    m_currentProfileId = 0;
    m_listProfiles->clearSelection();

    m_txtName->clear();
    m_txtName->setFocus();
    m_chkPlayBell->setChecked(true);
    m_chkRecessLock->setChecked(false);
    m_chkChatEnabled->setChecked(true);

    m_lblAssignedCount->setText(tr("Yeni özel profil tanımlanıyor."));
    m_btnDelete->setEnabled(false);
    m_lblStatus->clear();
}

void CustomProfilesDialog::onSaveClicked() {
    QString name = m_txtName->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Eksik Bilgi"), tr("Lütfen profil için bir isim girin."));
        m_txtName->setFocus();
        return;
    }

    CustomProfileRecord rec;
    rec.id = m_currentProfileId;
    rec.name = name;
    rec.playBell = m_chkPlayBell->isChecked();
    rec.recessLock = m_chkRecessLock->isChecked();
    rec.chatEnabled = m_chkChatEnabled->isChecked();
    rec.iconName = "custom_profile";

    if (m_session) {
        m_lblStatus->setText(tr("Kaydediliyor..."));
        m_lblStatus->setStyleSheet("color: palette(highlight);");
        m_session->sendCustomProfileUpsert(rec);
    } else {
        // Çevrimdışı / doğrudan DB erişimi
        if (OkidiaDatabase::instance().upsertCustomProfile(rec)) {
            m_lblStatus->setText(tr("Yerel olarak kaydedildi."));
            m_lblStatus->setStyleSheet("color: #27ae60;");
            refreshProfileList(rec.id);
        }
    }
}

void CustomProfilesDialog::onDeleteClicked() {
    if (m_currentProfileId <= 0) return;

    QString name = m_txtName->text().trimmed();
    int count = OkidiaDatabase::instance().countAgentsWithCustomProfile(m_currentProfileId);

    if (count > 0) {
        int ret = QMessageBox::warning(this, tr("Özel Profil Silme"),
            tr("'%1' profiline atanmış %2 adet bilgisayar bulunmaktadır.\n\n"
               "Profil silindiğinde bu bilgisayarlar otomatik olarak varsayılan 'Öğrenci PC (Tip 1)' tipine dönüştürülecektir.\n\n"
               "Silme işlemini onaylıyor musunuz?").arg(name).arg(count),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    } else {
        int ret = QMessageBox::question(this, tr("Özel Profil Silme"),
            tr("'%1' özel profilini silmek istediğinize emin misiniz?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }

    if (m_session) {
        m_lblStatus->setText(tr("Siliniyor..."));
        m_lblStatus->setStyleSheet("color: palette(highlight);");
        m_session->sendCustomProfileDelete(m_currentProfileId);
    } else {
        if (OkidiaDatabase::instance().deleteCustomProfile(m_currentProfileId)) {
            m_lblStatus->setText(tr("Yerel olarak silindi."));
            m_lblStatus->setStyleSheet("color: #27ae60;");
            refreshProfileList();
        }
    }
}

