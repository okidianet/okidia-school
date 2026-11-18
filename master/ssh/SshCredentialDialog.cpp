#include "SshCredentialDialog.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QUuid>
#include <QDateTime>
#include <QLabel>
#include <QFileDialog>
#include <QFile>
#include <QTimer>
#include "../common/crypto/AesGcm.h"

SshCredentialDialog::SshCredentialDialog(SessionManager* session, QWidget* parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(tr("SSH Kimlik Yöneticisi"));
    resize(700, 450);
    setupUi();
    
    connect(m_session, &SessionManager::sshCredentialsLoaded, this, &SshCredentialDialog::refreshList);
    m_session->fetchSshCredentials();
    refreshList();
}

void SshCredentialDialog::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    
    m_tabWidget = new QTabWidget(this);
    mainLay->addWidget(m_tabWidget);
    
    // --- TAB 1: Kayıtlı Kimlik Listesi (Genel/Sınıf) ---
    m_tabGlobal = new QWidget(this);
    auto* tab1Lay = new QHBoxLayout(m_tabGlobal);
    
    auto* leftSide = new QVBoxLayout();
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({tr("Ad"), tr("Kullanıcı"), tr("Tür")});
    OkidiaUtils::setupTableHeaders(m_table, "ssh_credential_global_table", {130, 100, 80}, true);
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    leftSide->addWidget(m_table);
    
    auto* listBtnRow = new QHBoxLayout();
    m_addBtn = new QPushButton(tr("Yeni Ekle"), this);
    m_deleteBtn = new QPushButton(tr("Sil"), this);
    m_deleteBtn->setEnabled(false);
    listBtnRow->addWidget(m_addBtn);
    listBtnRow->addWidget(m_deleteBtn);
    leftSide->addLayout(listBtnRow);
    
    tab1Lay->addLayout(leftSide, 3);
    
    auto* rightSide = new QGroupBox(tr("Kimlik Detayları"), this);
    auto* form = new QFormLayout(rightSide);
    
    m_nameEdit = new QLineEdit(this);
    m_userEdit = new QLineEdit(this);
    m_secretEdit = new QLineEdit(this);
    m_secretEdit->setEchoMode(QLineEdit::Password);
    
    m_browseBtn = new QPushButton(tr("Dosyadan Oku..."), this);
    m_browseBtn->setVisible(false);
    
    auto* secretLay = new QHBoxLayout();
    secretLay->addWidget(m_secretEdit);
    secretLay->addWidget(m_browseBtn);
    
    m_isKeyCheck = new QCheckBox(tr("SSH Anahtarı (Key)"), this);
    m_isSharedCheck = new QCheckBox(tr("Ortak Kimlik (Tüm Masterlar Görür)"), this);
    
    m_targetIdLabel = new QLabel(tr("IP / Hedef Adresi:"), this);
    m_targetIdEdit = new QLineEdit(this);
    m_targetIdLabel->setVisible(false);
    m_targetIdEdit->setVisible(false);

    form->addRow(tr("Görünen Ad:"), m_nameEdit);
    form->addRow(tr("Kullanıcı Adı:"), m_userEdit);
    form->addRow(m_targetIdLabel, m_targetIdEdit);
    
    m_secretLabel = new QLabel(tr("Parola:"));
    form->addRow(m_secretLabel, secretLay);
    form->addRow("", m_isKeyCheck);
    if (m_session->isYM()) {
        form->addRow("", m_isSharedCheck);
    } else {
        m_isSharedCheck->setVisible(false);
    }
    
    m_saveBtn = new QPushButton(tr("Kaydet"), this);
    m_saveBtn->setEnabled(true);  // Başlangıçta yeni ekle modu — aktif
    form->addRow(m_saveBtn);
    
    tab1Lay->addWidget(rightSide, 2);
    
    m_tabWidget->addTab(m_tabGlobal, tr("SSH Şablonları"));
    
    // --- TAB 2: Ayrıntılı Kayıtlar ---
    m_tabDetailed = new QWidget(this);
    auto* tab2Lay = new QVBoxLayout(m_tabDetailed);
    
    m_detailedTable = new QTableWidget(0, 6, this);
    m_detailedTable->setHorizontalHeaderLabels({tr("IP"), tr("Bilgisayar Adı"), tr("Kayıt Biçimi"), tr("Kullanıcı Adı"), tr("Tür"), tr("UUID")});
    OkidiaUtils::setupTableHeaders(m_detailedTable, "ssh_credential_detailed_table", {110, 130, 100, 100, 80, 120}, true);
    m_detailedTable->setSortingEnabled(true);
    m_detailedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailedTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tab2Lay->addWidget(m_detailedTable);
    
    auto* bulkRow = new QHBoxLayout();
    m_editBtn = new QPushButton(tr("Düzenle"), this);
    m_editBtn->setEnabled(false);
    bulkRow->addWidget(m_editBtn);
    bulkRow->addStretch();
    m_bulkDeleteBtn = new QPushButton(tr("Seçilenleri Sil"), this);
    m_bulkDeleteBtn->setEnabled(false);
    m_bulkDeleteBtn->setProperty("class", "primary");
    bulkRow->addWidget(m_bulkDeleteBtn);
    tab2Lay->addLayout(bulkRow);
    
    m_tabWidget->addTab(m_tabDetailed, tr("Kayıtlı Cihazlar"));
    
    // Bağlantılar
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &SshCredentialDialog::onSelectionChanged);
    connect(m_addBtn, &QPushButton::clicked, this, &SshCredentialDialog::onAdd);
    connect(m_saveBtn, &QPushButton::clicked, this, &SshCredentialDialog::onSave);
    connect(m_deleteBtn, &QPushButton::clicked, this, &SshCredentialDialog::onDelete);
    connect(m_isKeyCheck, &QCheckBox::toggled, this, &SshCredentialDialog::onKeyToggled);
    connect(m_browseBtn, &QPushButton::clicked, this, &SshCredentialDialog::onBrowseKey);
    
    connect(m_editBtn, &QPushButton::clicked, this, [this]() {
        m_tabWidget->setCurrentWidget(m_tabGlobal);
        m_targetIdEdit->setFocus();
    });
    connect(m_detailedTable, &QTableWidget::itemSelectionChanged, this, &SshCredentialDialog::onDetailedSelectionChanged);
    connect(m_detailedTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        if (item) {
            m_tabWidget->setCurrentWidget(m_tabGlobal);
            m_targetIdEdit->setFocus();
        }
    });
    connect(m_bulkDeleteBtn, &QPushButton::clicked, this, &SshCredentialDialog::onBulkDelete);
}

void SshCredentialDialog::refreshList() {
    QString prevId = m_currentId;
    m_table->blockSignals(true);
    m_detailedTable->blockSignals(true);
    
    m_table->setSortingEnabled(false);
    m_detailedTable->setSortingEnabled(false);
    
    m_table->setRowCount(0);
    m_detailedTable->setRowCount(0);
    
    const auto creds = m_session->sshCredentials();
    int rowToSelectGlobal = -1;
    int rowToSelectDetailed = -1;
    
    for (const auto& c : creds) {
        if (c.targetType == "agent") {
            int row = m_detailedTable->rowCount();
            m_detailedTable->insertRow(row);
            
            QString compName = m_session->clientState(c.targetId).hostname;
            if (compName.isEmpty()) {
                int lastColon = c.name.lastIndexOf(": ");
                if (lastColon != -1) {
                    compName = c.name.mid(lastColon + 2);
                } else {
                    compName = "-";
                }
            }
            
            QString kayitBicimi = c.name.startsWith(tr("Şablon")) ? tr("Şablon") : "El ile";
            
            auto* item0 = new OkidiaTableItem(c.targetId);
            item0->setData(Qt::UserRole, c.id);
            m_detailedTable->setItem(row, 0, item0);
            m_detailedTable->setItem(row, 1, new OkidiaTableItem(compName));
            m_detailedTable->setItem(row, 2, new OkidiaTableItem(kayitBicimi));
            m_detailedTable->setItem(row, 3, new OkidiaTableItem(c.username));
            m_detailedTable->setItem(row, 4, new OkidiaTableItem(c.isKey ? "Anahtar" : tr("Şifre")));
            m_detailedTable->setItem(row, 5, new OkidiaTableItem(c.machineId.isEmpty() ? "-" : c.machineId));
            
            if (c.id == prevId) {
                rowToSelectDetailed = row;
            }
        } else {
            int row = m_table->rowCount();
            m_table->insertRow(row);
            
            auto* item0 = new OkidiaTableItem(c.name);
            item0->setData(Qt::UserRole, c.id);
            m_table->setItem(row, 0, item0);
            m_table->setItem(row, 1, new OkidiaTableItem(c.username));
            m_table->setItem(row, 2, new OkidiaTableItem(c.isKey ? "Anahtar" : tr("Şifre")));
            
            if (c.id == prevId) {
                rowToSelectGlobal = row;
            }
        }
    }
    
    if (rowToSelectGlobal >= 0) {
        m_table->selectRow(rowToSelectGlobal);
    }
    if (rowToSelectDetailed >= 0) {
        m_detailedTable->selectRow(rowToSelectDetailed);
    }
    
    m_table->blockSignals(false);
    m_detailedTable->blockSignals(false);
    
    m_table->setSortingEnabled(true);
    m_detailedTable->setSortingEnabled(true);
    
    if (rowToSelectGlobal < 0 && rowToSelectDetailed < 0) {
        // Eğer kullanıcı yeni ekleme modundaysa ve formda henüz kaydedilmemiş veri varsa
        // arka plandan gelen liste tazelemesi kullanıcının yazdıklarını silmesin!
        const bool isFormDirty = m_currentId.isEmpty() &&
            (!m_nameEdit->text().trimmed().isEmpty() || !m_userEdit->text().trimmed().isEmpty() || !m_secretEdit->text().isEmpty());
        if (!isFormDirty) {
            if (m_tabWidget->currentWidget() == m_tabGlobal) {
                onSelectionChanged();
            } else {
                onDetailedSelectionChanged();
            }
        }
    }
}

void SshCredentialDialog::onSelectionChanged() {
    auto items = m_table->selectedItems();
    if (items.isEmpty()) {
        // Eğer zaten yeni ekleme modundaysak ve kullanıcı veri yazıyorsa formu ezme!
        if (m_currentId.isEmpty() && (!m_nameEdit->text().trimmed().isEmpty() || !m_userEdit->text().trimmed().isEmpty() || !m_secretEdit->text().isEmpty())) {
            return;
        }
        // Seçim yok → Yeni Ekle modu: form aktif, alanlar temizlenmiş
        m_currentId.clear();
        m_currentTargetId.clear();
        m_currentTargetType.clear();
        m_currentMachineId.clear();
        m_nameEdit->clear();
        m_userEdit->clear();
        m_secretEdit->clear();
        m_targetIdEdit->clear();
        m_targetIdLabel->setVisible(false);
        m_targetIdEdit->setVisible(false);
        m_isKeyCheck->setChecked(false);
        m_isSharedCheck->setChecked(false);
        m_saveBtn->setEnabled(true);   // Yeni ekle modu — kaydet aktif
        m_deleteBtn->setEnabled(false);
        return;
    }
    
    m_currentId = items[0]->data(Qt::UserRole).toString();
    const auto creds = m_session->sshCredentials();
    for (const auto& c : creds) {
        if (c.id == m_currentId) {
            m_currentTargetId = c.targetId;
            m_currentTargetType = c.targetType;
            m_currentMachineId = c.machineId;
            m_nameEdit->setText(c.name);
            m_userEdit->setText(c.username);
            
            QByteArray sec = c.secret;
            if (!sec.isEmpty() && sec.size() >= 28) {
                QByteArray decrypted = AesGcm::decrypt(sec, AesGcm::defaultSystemKey());
                if (!decrypted.isEmpty()) sec = decrypted;
            }
            m_secretEdit->setText(QString::fromUtf8(sec));
            
            m_targetIdLabel->setVisible(false);
            m_targetIdEdit->setVisible(false);
            m_targetIdEdit->clear();
            m_isKeyCheck->setChecked(c.isKey);
            m_isSharedCheck->setChecked(c.ownerUserId.isEmpty());
            break;
        }
    }
    m_saveBtn->setEnabled(true);
    m_deleteBtn->setEnabled(true);
    
    m_detailedTable->blockSignals(true);
    m_detailedTable->clearSelection();
    m_bulkDeleteBtn->setEnabled(false);
    m_editBtn->setEnabled(false);
    m_detailedTable->blockSignals(false);
}

void SshCredentialDialog::onDetailedSelectionChanged() {
    auto items = m_detailedTable->selectedItems();
    if (items.isEmpty()) {
        m_bulkDeleteBtn->setEnabled(false);
        m_editBtn->setEnabled(false);
        m_deleteBtn->setEnabled(false);
        m_saveBtn->setEnabled(false);
        return;
    }
    m_bulkDeleteBtn->setEnabled(true);
    
    QList<int> selectedRows;
    for (auto* it : items) {
        if (!selectedRows.contains(it->row())) selectedRows << it->row();
    }
    
    if (selectedRows.size() == 1) {
        m_editBtn->setEnabled(true);
        m_currentId = m_detailedTable->item(selectedRows[0], 0)->data(Qt::UserRole).toString();
        const auto creds = m_session->sshCredentials();
        for (const auto& c : creds) {
            if (c.id == m_currentId) {
                m_currentTargetId = c.targetId;
                m_currentTargetType = c.targetType;
                m_currentMachineId = c.machineId;
                m_nameEdit->setText(c.name);
                m_userEdit->setText(c.username);
                
                QByteArray sec = c.secret;
                if (!sec.isEmpty() && sec.size() >= 28) {
                    QByteArray decrypted = AesGcm::decrypt(sec, AesGcm::defaultSystemKey());
                    if (!decrypted.isEmpty()) sec = decrypted;
                }
                m_secretEdit->setText(QString::fromUtf8(sec));
                
                m_targetIdLabel->setVisible(true);
                m_targetIdEdit->setVisible(true);
                m_targetIdEdit->setText(c.targetId);
                m_isKeyCheck->setChecked(c.isKey);
                m_isSharedCheck->setChecked(c.ownerUserId.isEmpty());
                break;
            }
        }
        m_saveBtn->setEnabled(true);
        m_deleteBtn->setEnabled(true);
        
        m_table->blockSignals(true);
        m_table->clearSelection();
        m_table->blockSignals(false);
    } else {
        m_editBtn->setEnabled(false);
        m_saveBtn->setEnabled(false);
        m_deleteBtn->setEnabled(false);
    }
}

void SshCredentialDialog::onAdd() {
    m_tabWidget->setCurrentWidget(m_tabGlobal);
    m_table->clearSelection();
    m_detailedTable->clearSelection();
    
    m_currentId.clear();
    m_currentTargetId.clear();
    m_currentTargetType.clear();
    m_currentMachineId.clear();
    
    m_nameEdit->clear();
    m_userEdit->clear();
    m_secretEdit->clear();
    m_targetIdLabel->setVisible(false);
    m_targetIdEdit->setVisible(false);
    m_targetIdEdit->clear();
    m_isKeyCheck->setChecked(false);
    m_isSharedCheck->setChecked(false);
    m_nameEdit->setFocus();
    m_saveBtn->setEnabled(true);
}

void SshCredentialDialog::onSave() {
    if (m_nameEdit->text().trimmed().isEmpty() || m_userEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Hata"), tr("Lütfen ad ve kullanıcı adı girin."));
        return;
    }

    bool isNew = m_currentId.isEmpty();
    if (isNew) {
        m_currentId = OkidiaUtils::generateUuid();
    }

    SshCredentialRecord rec;
    rec.id = m_currentId;
    if (m_targetIdEdit->isVisible() && !m_targetIdEdit->text().trimmed().isEmpty()) {
        rec.targetId = m_targetIdEdit->text().trimmed();
        rec.targetType = "agent";
    } else {
        rec.targetId = m_currentTargetId;
        rec.targetType = m_currentTargetType;
    }
    rec.machineId = m_currentMachineId;
    rec.name = m_nameEdit->text().trimmed();
    rec.username = m_userEdit->text().trimmed();
    rec.secret = m_secretEdit->text().toUtf8();
    rec.isKey = m_isKeyCheck->isChecked();
    rec.ownerUserId = m_isSharedCheck->isChecked() ? "" : m_session->activeUser().id;
    rec.createdAt = OkidiaUtils::nowSecs();
    
    m_session->addSshCredential(rec);
    
    QMessageBox::information(this, tr("Başarılı"), tr("SSH Kimliği başarıyla kaydedildi."));
    
    emit credentialSaved(rec.id);
    
    if (isNew) {
        m_table->clearSelection();
    }
    
    QTimer::singleShot(500, m_session, &SessionManager::fetchSshCredentials);
}

void SshCredentialDialog::onDelete() {
    if (m_currentId.isEmpty()) return;
    if (QMessageBox::question(this, tr("Sil"), tr("Bu kimliği silmek istediğinize emin misiniz?")) != QMessageBox::Yes) return;
    
    m_session->deleteSshCredential(m_currentId);
    QTimer::singleShot(500, m_session, &SessionManager::fetchSshCredentials);
}

void SshCredentialDialog::onBulkDelete() {
    auto items = m_detailedTable->selectedItems();
    QSet<QString> idsToDelete;
    for (auto* it : items) {
        idsToDelete.insert(m_detailedTable->item(it->row(), 0)->data(Qt::UserRole).toString());
    }
    
    if (idsToDelete.isEmpty()) return;
    
    if (QMessageBox::question(this, tr("Toplu Silme"), 
        QString::number(idsToDelete.size()) + tr(" adet kimliği silmek istediğinize emin misiniz?")) != QMessageBox::Yes) {
        return;
    }
    
    for (const QString& id : idsToDelete) {
        m_session->deleteSshCredential(id);
    }
    
    QTimer::singleShot(500, m_session, &SessionManager::fetchSshCredentials);
}

void SshCredentialDialog::onKeyToggled(bool checked) {
    bool isKey = checked;
    m_secretLabel->setText(isKey ? tr("Özel Anahtar (Private Key):") : "Parola:");
    m_browseBtn->setVisible(isKey);
    m_secretEdit->setPlaceholderText(isKey ? "-----BEGIN OPENSSH PRIVATE KEY-----" : "");
}

void SshCredentialDialog::onBrowseKey() {
    QString path = OkidiaUtils::getOpenFileName(this, tr("SSH Özel Anahtarı Seç"), "ssh_keys",
                                                QString(), nullptr, QFileDialog::Options(),
                                                QDir::homePath() + "/.ssh");
    if (path.isEmpty()) return;
    
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        m_secretEdit->setText(f.readAll());
        f.close();
    }
}
