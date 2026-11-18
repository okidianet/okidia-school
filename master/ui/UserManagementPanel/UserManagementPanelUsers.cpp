// master/ui/UserManagementPanel/UserManagementPanelUsers.cpp
#include "UserManagementPanel.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/db/DbRecords.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QMenu>
#include <QDateTime>
#include <QClipboard>
#include <QApplication>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QRandomGenerator>

void UserManagementPanel::setupUserTab() {
    m_userTable = new QTableWidget(0, 5, this);
    m_userTable->setHorizontalHeaderLabels({tr("Kullanıcı Adı"), tr("Ad Soyad"), tr("Rol"), tr("Son Giriş"), tr("İkincil Yedek")});
    OkidiaUtils::setupTableHeaders(m_userTable, "user_mgmt_table", {120, 140, 100, 130, 110}, true);
    m_userTable->setSortingEnabled(true);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_userTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_userTable, &QTableWidget::customContextMenuRequested,
            this, &UserManagementPanel::onUserContextMenu);

    m_btnAdd    = new QPushButton(tr("Yönetici Ekle"), this);
    m_btnAdd->setIcon(ThemeManager::icon(":/ui/add.svg"));
    m_btnEdit   = new QPushButton(tr("Düzenle"),       this);
    m_btnEdit->setIcon(ThemeManager::icon(":/ui/edit.svg"));
    m_btnReset  = new QPushButton(tr("Şifre Sıfırla"),  this);
    m_btnReset->setIcon(ThemeManager::icon(":/ui/key.svg"));
    m_btnDelete = new QPushButton(tr("Sil"),           this);
    m_btnDelete->setIcon(ThemeManager::icon(":/ui/trash.svg"));
    
    m_btnJoinCode = new QPushButton(tr("Davet Kodu Oluştur"), this);
    m_btnJoinCode->setIcon(ThemeManager::icon(":/ui/share.svg"));
    m_btnJoinCode->setToolTip(tr("Yeni bir Master kurulumu için davet kodu üret"));

    m_btnRecovery = new QPushButton(tr("Kurtarma Kodu Yenile"), this);
    m_btnRecovery->setIcon(ThemeManager::icon(":/ui/key.svg"));
    m_btnRecovery->setToolTip(tr("Acil durum şifre sıfırlama kodunu yeniden üret ve kaydet"));

    m_btnReplica = new QPushButton(tr("İkincil Yedek Yetkisi"), this);
    m_btnReplica->setIcon(ThemeManager::icon(":/ui/shield.svg"));
    m_btnReplica->setToolTip(tr("Seçili kullanıcıya otomatik sistem yedeği replikasyon yetkisi ver/kaldır"));

    m_btnAdd->setProperty("class", "primary");
    m_btnJoinCode->setProperty("class", "primary");
    m_btnRecovery->setProperty("class", "primary");

    m_btnEdit->setEnabled(false);
    m_btnReset->setEnabled(false);
    m_btnDelete->setEnabled(false);
    m_btnJoinCode->setEnabled(false);
    m_btnReplica->setEnabled(false);

    connect(m_userTable, &QTableWidget::currentCellChanged,
            this, &UserManagementPanel::onUserSelectionChanged);
    connect(m_btnAdd,    &QPushButton::clicked, this, &UserManagementPanel::onAddUser);
    connect(m_btnEdit,   &QPushButton::clicked, this, &UserManagementPanel::onEditUser);
    connect(m_btnDelete, &QPushButton::clicked, this, &UserManagementPanel::onDeleteUser);
    connect(m_btnReset,  &QPushButton::clicked, this, &UserManagementPanel::onResetPassword);
    connect(m_btnJoinCode, &QPushButton::clicked, this, &UserManagementPanel::onGenerateJoinCode);
    connect(m_btnRecovery, &QPushButton::clicked, this, &UserManagementPanel::onGenerateRecoveryKey);
    connect(m_btnReplica,  &QPushButton::clicked, this, &UserManagementPanel::onToggleReplica);

    if (m_session) {
        connect(m_session, &SessionManager::joinCodeReceived, this, [this](const QString& code, bool success, const QString& error) {
            if (!success) {
                QMessageBox::warning(this, tr("Hata"), tr("Davet kodu üretilemedi: ") + error);
                return;
            }

            QDialog dlg(this);
            dlg.setWindowTitle(tr("Katılım Kodu"));
            dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
            dlg.setMinimumWidth(320);
            auto* lay = new QVBoxLayout(&dlg);
            lay->addWidget(new QLabel(tr("Yeni Master bilgisayarında bu kodu girin:"), &dlg));
            auto* edit = new QLineEdit(code, &dlg);
            edit->setReadOnly(true);
            edit->setAlignment(Qt::AlignCenter);
            edit->setStyleSheet("font-size: 18px; font-weight: bold; letter-spacing: 2px; padding: 6px;");
            lay->addWidget(edit);
            auto* subLbl = new QLabel(tr("Bu kod 24 saat geçerlidir ve tek kullanımlıktır."), &dlg);
            subLbl->setStyleSheet("color: #888; font-size: 11px;");
            lay->addWidget(subLbl);
            auto* copyBtn = new QPushButton(tr("Kodu Kopyala"), &dlg);
            connect(copyBtn, &QPushButton::clicked, [code, copyBtn]() {
                QApplication::clipboard()->setText(code);
                copyBtn->setText(tr("Kopyalandı!"));
            });
            lay->addWidget(copyBtn);
            dlg.exec();
        });

        connect(m_session, &SessionManager::brokerConnected,    this, &UserManagementPanel::onUserSelectionChanged);
        connect(m_session, &SessionManager::brokerDisconnected, this, &UserManagementPanel::onUserSelectionChanged);
    }
}

void UserManagementPanel::loadUsers() {
    const QString prevSelectedUserId = m_userCombo->currentData().toString();
    const QString prevTableUserId = selectedUserId();
    m_users = OkidiaDatabase::instance().allUsers();
    m_userTable->setSortingEnabled(false);

    m_userCombo->blockSignals(true);
    m_userCombo->clear();

    int restoreIndex = -1;
    {
        const QSignalBlocker blocker(m_userTable);
        m_userTable->setRowCount(0);

        for (int i = 0; i < m_users.size(); ++i) {
            const auto& u = m_users[i];
            const int row = m_userTable->rowCount();
            m_userTable->insertRow(row);
            m_userTable->setItem(row, 0, new OkidiaTableItem(u.username));
            m_userTable->setItem(row, 1, new OkidiaTableItem(u.displayName));

            const QString roleStr = (u.role == "YM") ? tr("Yönetici")
                                  : (u.role == "OM") ? tr("Öğretmen")
                                  :                    tr("Gözlemci");
            m_userTable->setItem(row, 2, new OkidiaTableItem(roleStr));

            const QString lastLogin = u.lastLogin > 0
                ? QDateTime::fromSecsSinceEpoch(u.lastLogin)
                  .toString("dd.MM.yyyy HH:mm")
                : "—";
            m_userTable->setItem(row, 3, new OkidiaTableItem(lastLogin));

            QString replicaText = tr("—");
            if (u.role == "YM") {
                const QString hostStr = !u.replicaHostname.trimmed().isEmpty()
                                        ? u.replicaHostname.trimmed()
                                        : tr("İlk Girişte Bağlanır");
                replicaText = tr("Ana Yedek (%1)").arg(hostStr);
            } else if (u.isBackupReplica) {
                const QString hostStr = !u.replicaHostname.trimmed().isEmpty()
                                        ? u.replicaHostname.trimmed()
                                        : tr("İlk Girişte Bağlanır");
                replicaText = tr("Yetkili (%1)").arg(hostStr);
            }
            auto* replicaItem = new OkidiaTableItem(replicaText);
            replicaItem->setTextAlignment(Qt::AlignCenter);
            if (u.role == "YM" || u.isBackupReplica) {
                replicaItem->setToolTip(tr("Şifreli sistem yedekleri (.okbak) yalnızca bu kullanıcı belirtilen bilgisayardan (%1) oturum açtığında o bilgisayara aktarılır.")
                                        .arg(!u.replicaHostname.trimmed().isEmpty() ? u.replicaHostname.trimmed() : tr("kendi ilk bilgisayarı")));
            }
            m_userTable->setItem(row, 4, replicaItem);

            m_userTable->item(row, 0)->setData(Qt::UserRole, u.id);

            if (u.mustChangePw) {
                QColor warn = ThemeManager::instance().warningColor();
                warn.setAlpha(80);
                for (int c = 0; c < 5; ++c)
                    if (m_userTable->item(row, c))
                        m_userTable->item(row, c)->setBackground(warn);
            }

            m_userCombo->addItem(u.displayName + " (" + u.username + ")", u.id);
            if (u.id == prevSelectedUserId) {
                restoreIndex = i;
            }
        }
    }
    m_userTable->setSortingEnabled(true);

    if (!prevTableUserId.isEmpty()) {
        for (int r = 0; r < m_userTable->rowCount(); ++r) {
            if (m_userTable->item(r, 0) && m_userTable->item(r, 0)->data(Qt::UserRole).toString() == prevTableUserId) {
                m_userTable->selectRow(r);
                break;
            }
        }
    }
    onUserSelectionChanged();

    if (restoreIndex >= 0) {
        m_userCombo->setCurrentIndex(restoreIndex);
    } else if (m_userCombo->count() > 0) {
        m_userCombo->setCurrentIndex(0);
    }
    m_userCombo->blockSignals(false);

    // İkincil Master (Replika) Atama Teşviki: Başka öğretmen/kullanıcı varken hiç replika yoksa uyar
    bool hasReplica = false;
    int otherUserCount = 0;
    for (const auto& u : m_users) {
        if (u.role != "YM") otherUserCount++;
        if (u.isBackupReplica) hasReplica = true;
    }
    if (m_replicaNoticeWidget) {
        m_replicaNoticeWidget->setVisible(otherUserCount > 0 && !hasReplica);
    }
}

QString UserManagementPanel::selectedUserId() const {
    const int row = m_userTable->currentRow();
    if (row < 0) return {};
    auto* item = m_userTable->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void UserManagementPanel::onUserSelectionChanged() {
    const QString selId = selectedUserId();
    const bool hasSel   = !selId.isEmpty();
    const bool isYM     = (m_activeUser.role == "YM");
    UserRecord selUser;
    if (hasSel) {
        selUser = OkidiaDatabase::instance().userById(selId);
    }
    const bool isSelYM  = (selUser.role == "YM");

    if (m_btnEdit) m_btnEdit->setEnabled(isYM && hasSel);
    if (m_btnDelete) m_btnDelete->setEnabled(isYM && hasSel);

    if (m_btnReset) {
        const bool canReset = isYM
            ? hasSel
            : (hasSel && selId == m_activeUser.id);
        m_btnReset->setEnabled(canReset);
        if (!isYM)
            m_btnReset->setText(canReset ? tr("Şifremi Değiştir") : tr("Şifre Sıfırla"));
    }

    if (m_btnJoinCode) {
        bool connected = m_session && m_session->isConnected();
        if (!connected) {
            m_btnJoinCode->setEnabled(false);
            m_btnJoinCode->setToolTip(tr("Sunucu online değil. Katılım kodu üretmek için Sunucu bağlantısı gereklidir."));
        } else if (!hasSel) {
            m_btnJoinCode->setEnabled(false);
            m_btnJoinCode->setToolTip(tr("Lütfen önce bir kullanıcı seçin."));
        } else {
            m_btnJoinCode->setEnabled(true);
            if (isSelYM) {
                m_btnJoinCode->setToolTip(tr("Yöneticinin yeni bir bilgisayara (örn: dizüstü veya ikinci işletim sistemi) kurulum yapabilmesi için katılım kodu üret"));
            } else {
                m_btnJoinCode->setToolTip(tr("Seçili kullanıcı için yeni bir Master kurulum davet kodu üret"));
            }
        }
    }

    if (m_btnRecovery) {
        bool connected = m_session && m_session->isConnected();
        m_btnRecovery->setEnabled(connected);
        if (!connected) {
            m_btnRecovery->setToolTip(tr("Sunucu online değil. Kurtarma kodunu yenilemek için Sunucu bağlantısı gereklidir."));
        } else {
            m_btnRecovery->setToolTip(tr("Acil durum şifre sıfırlama kodunu yeniden üret ve kaydet"));
        }
    }

    if (m_btnReplica) {
        bool connected = m_session && m_session->isConnected();
        const bool canReplica = isYM && hasSel && connected;
        m_btnReplica->setEnabled(canReplica);
        if (canReplica) {
            UserRecord u = OkidiaDatabase::instance().userById(selId);
            if (u.role == "YM") {
                m_btnReplica->setText(tr("Yedek Cihazını Değiştir"));
            } else {
                m_btnReplica->setText(u.isBackupReplica ? tr("Replika Yetkisini Kaldır") : tr("İkincil Yedek Yetkisi Ver"));
            }
        } else {
            m_btnReplica->setText(tr("İkincil Yedek Yetkisi"));
        }
    }
}

void UserManagementPanel::onToggleReplica() {
    QString uid = selectedUserId();
    if (uid.isEmpty() || !m_session) return;
    UserRecord u = OkidiaDatabase::instance().userById(uid);

    // Eğer OM kullanıcısının mevcut replika yetkisi kaldırılıyorsa doğrudan kaldır
    if (u.role != "YM" && u.isBackupReplica) {
        m_session->sendDbUserSetReplica(uid, false);
        return;
    }

    // YM için yedek cihazı değiştirme veya OM için replika yetkisi + hedef bilgisayar seçimi
    const QList<MasterDeviceRecord> devices = OkidiaDatabase::instance().allMasterDevices();
    QDialog dlg(this);
    dlg.setWindowTitle(u.role == "YM" ? tr("Ana Yedek Bilgisayarını Seç") : tr("İkincil Yedek (Replika) Bilgisayarını Seç"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(420);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(18, 18, 18, 16);
    lay->setSpacing(10);

    auto* infoLbl = new QLabel(
        tr("<b>%1 (%2)</b> kullanıcısı için veritabanı yedeklerinin (<code>.okbak</code>) aktarılacağı bilgisayarı seçin.<br><br>"
           "Kullanıcı farklı bir Master bilgisayardan oturum açtığında yedekler o bilgisayara <b>kopyalanmaz</b>.")
           .arg(u.displayName, u.username), &dlg);
    infoLbl->setWordWrap(true);
    lay->addWidget(infoLbl);

    auto* devCombo = new QComboBox(&dlg);
    devCombo->addItem(tr("İlk Giriş Yaptığı Bilgisayar (Otomatik Bağla)"), "__AUTO__");

    int preselectIdx = 0;
    for (const auto& d : devices) {
        const QString host = d.hostname.trimmed();
        if (host.isEmpty()) continue;
        QString label = host;
        if (!d.ip.isEmpty()) label += QString(" (%1)").arg(d.ip);
        if (!d.lastUsername.isEmpty()) label += tr(" — Son giriş: %1").arg(d.lastUsername);
        devCombo->addItem(label, host);
        int idx = devCombo->count() - 1;
        devCombo->setItemData(idx, d.machineId, Qt::UserRole + 1);

        if (!u.replicaHostname.trimmed().isEmpty() && host.compare(u.replicaHostname.trimmed(), Qt::CaseInsensitive) == 0) {
            preselectIdx = idx;
        } else if (preselectIdx == 0 && d.lastUserId == u.id) {
            preselectIdx = idx;
        }
    }
    devCombo->setCurrentIndex(preselectIdx);
    lay->addWidget(devCombo);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Kaydet ve Yetkilendir"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    const QString selHost = devCombo->currentData().toString();
    const QString selMid  = devCombo->currentData(Qt::UserRole + 1).toString();
    m_session->sendDbUserSetReplica(uid, true, selMid, selHost);
}

void UserManagementPanel::onGenerateJoinCode() {
    QString uid = selectedUserId();
    if (uid.isEmpty() || !m_session) return;

    m_session->requestJoinCode(uid);
}

void UserManagementPanel::onAddUser() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Kullanıcı Ekle"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(360);

    auto* form = new QFormLayout(&dlg);
    form->setSpacing(10);
    form->setContentsMargins(16, 16, 16, 16);

    auto* usernameEdit    = new QLineEdit(&dlg);
    auto* displayNameEdit = new QLineEdit(&dlg);
    auto* passwordEdit    = new QLineEdit(&dlg);
    auto* roleCombo       = new QComboBox(&dlg);

    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(tr("Geçici şifre (kullanıcı değiştirmek zorunda)"));
    roleCombo->addItem(ThemeManager::icon(":/ui/teacher.svg"), tr("Öğretmen"), "OM");
    roleCombo->addItem(ThemeManager::icon(":/ui/student.svg"), tr("Gözlemci"),   "OBSERVER");

    auto* replicaCheck = new QCheckBox(tr("Bu kullanıcıya ikincil sistem yedeği (replika) alma yetkisi ver"), &dlg);
    auto* replicaDevCombo = new QComboBox(&dlg);
    replicaDevCombo->addItem(tr("İlk Giriş Yaptığı Bilgisayar (Otomatik Bağla)"), "__AUTO__");
    for (const auto& d : OkidiaDatabase::instance().allMasterDevices()) {
        if (d.hostname.trimmed().isEmpty()) continue;
        QString label = d.hostname.trimmed();
        if (!d.ip.isEmpty()) label += QString(" (%1)").arg(d.ip);
        replicaDevCombo->addItem(label, d.hostname.trimmed());
        replicaDevCombo->setItemData(replicaDevCombo->count() - 1, d.machineId, Qt::UserRole + 1);
    }
    replicaDevCombo->setEnabled(false);
    connect(replicaCheck, &QCheckBox::toggled, replicaDevCombo, &QWidget::setEnabled);

    auto* alsoTeacherCheck = new QCheckBox(tr("Bu kullanıcı akıllı tahtası olan sınıflarda derse girecek (Öğretmenler listesine de ekle)"), &dlg);
    alsoTeacherCheck->setChecked(true);

    form->addRow(tr("Kullanıcı adı:"), usernameEdit);
    form->addRow(tr("Ad Soyad:"),       displayNameEdit);
    form->addRow(tr("Geçici Şifre:"),   passwordEdit);
    form->addRow(tr("Rol:"),            roleCombo);
    form->addRow(QString(),             replicaCheck);
    form->addRow(tr("Yedek Cihazı:"),   replicaDevCombo);
    form->addRow(QString(),             alsoTeacherCheck);

    auto* errorLbl = new QLabel(&dlg);
    errorLbl->setProperty("status", "error");
    form->addRow(errorLbl);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Ekle"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, [&]() {
        if (usernameEdit->text().trimmed().isEmpty() ||
            displayNameEdit->text().trimmed().isEmpty() ||
            passwordEdit->text().isEmpty()) {
            errorLbl->setText(tr("Tüm alanları doldurun."));
            return;
        }
        if (passwordEdit->text().length() < 6) {
            errorLbl->setText(tr("Şifre en az 6 karakter olmalı."));
            return;
        }
        if (OkidiaDatabase::instance().userByUsername(
                usernameEdit->text().trimmed()).id != "") {
            errorLbl->setText(tr("Bu kullanıcı adı zaten kullanılıyor."));
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    UserRecord u;
    u.id          = OkidiaUtils::generateUuid();
    u.username    = usernameEdit->text().trimmed();
    u.displayName = displayNameEdit->text().trimmed();
    u.passwordHash = OkidiaUtils::sha256(passwordEdit->text().toUtf8());
    u.role        = roleCombo->currentData().toString();
    u.isBackupReplica = replicaCheck->isChecked();
    QString selRepHost;
    QString selRepMid;
    if (u.isBackupReplica || u.role == "YM") {
        selRepHost = replicaDevCombo->currentData().toString();
        selRepMid  = replicaDevCombo->currentData(Qt::UserRole + 1).toString();
        if (selRepHost != "__AUTO__") {
            u.replicaHostname  = selRepHost;
            u.replicaMachineId = selRepMid;
        }
    }
    u.mustChangePw = true;
    u.isActive    = true;
    u.createdAt   = OkidiaUtils::nowSecs();
    bool isPromotingToYM = (u.role == "YM");
    bool cleanLocalBackups = false;

    if (isPromotingToYM) {
        QDialog confirmDlg(&dlg);
        confirmDlg.setWindowTitle(tr("Yöneticilik Yetki Devri Onayı"));
        confirmDlg.setWindowFlags(confirmDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
        confirmDlg.setFixedWidth(460);
        auto* vLay = new QVBoxLayout(&confirmDlg);

        auto* warnLbl = new QLabel(
            tr("<b style='color:#e74c3c; font-size:14px;'>DİKKAT: Yöneticilik Yetkisi Devrediliyor!</b><br><br>"
               "Bu kullanıcıyı <b>Yönetici (YM)</b> yaptığınızda sistemin tek resmi Yöneticisi bu kullanıcı olacaktır.<br>"
               "Kendi yetkiniz oturumunuz kapandığında otomatik olarak <b>Gözlemci (GM)</b> seviyesine düşürülecektir.<br>"
               "Oturumunuz açık kaldığı sürece geçici yönetici haklarınız korunacaktır."), &confirmDlg);
        warnLbl->setWordWrap(true);
        vLay->addWidget(warnLbl);

        auto* chkClean = new QCheckBox(tr("Bu bilgisayarda saklanan yerel sistem yedeklerini güvenle temizle (Önerilen)"), &confirmDlg);
        chkClean->setChecked(true);
        vLay->addWidget(chkClean);

        auto* bBox = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, &confirmDlg);
        bBox->button(QDialogButtonBox::Yes)->setText(tr("Onaylıyorum ve Devret"));
        bBox->button(QDialogButtonBox::No)->setText(tr("İptal"));
        vLay->addWidget(bBox);

        connect(bBox, &QDialogButtonBox::accepted, &confirmDlg, &QDialog::accept);
        connect(bBox, &QDialogButtonBox::rejected, &confirmDlg, &QDialog::reject);

        if (confirmDlg.exec() != QDialog::Accepted) {
            return;
        }
        cleanLocalBackups = chkClean->isChecked();
    }

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbUserAdd(u.username, u.displayName, u.passwordHash, u.role, u.isBackupReplica, u.id, selRepMid, selRepHost);
        if (ok) {
            // Yerel veritabanına ve UI tablosuna hemen yansıt
            OkidiaDatabase::instance().upsertUser(u);
            loadUsers();
            for (int r = 0; r < m_userTable->rowCount(); ++r) {
                if (m_userTable->item(r, 0) && m_userTable->item(r, 0)->data(Qt::UserRole).toString() == u.id) {
                    m_userTable->selectRow(r);
                    break;
                }
            }
            onUserSelectionChanged();
        }

        if (ok && alsoTeacherCheck->isChecked()) {
            TeacherRecord t;
            t.id          = OkidiaUtils::generateUuid();
            t.username    = u.username;
            t.displayName = u.displayName;
            t.branch      = (u.role == "YM") ? tr("Yönetici") : tr("Öğretmen");
            QByteArray rawSeed = QCryptographicHash::hash(QString(t.id + "_teacher_seed_" + t.username).toUtf8(),
                                                          QCryptographicHash::Sha256);
            t.seedHex     = QString::fromUtf8(rawSeed.toHex());
            t.isActive    = true;
            t.createdAt   = QDateTime::currentSecsSinceEpoch();
            m_session->sendDbTeacherUpsert(t);
            OkidiaDatabase::instance().upsertTeacher(t);
            loadTeachers();
        }

        if (ok && isPromotingToYM) {
            if (cleanLocalBackups) {
                QString targetDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
                QDir bDir(targetDir);
                for (const QFileInfo& fi : bDir.entryInfoList(QStringList() << "*.okbak", QDir::Files)) {
                    QFile::remove(fi.filePath());
                }
                Logger::info(QObject::tr("UserManagement: Eski YM yerel sistem yedekleri temizlendi."));
            }
            onGenerateRecoveryKey();
        }
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Hata"), tr("Kullanıcı ekleme isteği gönderilemedi. Sunucu bağlantısını kontrol edin."));
    }
}

void UserManagementPanel::onEditUser() {
    const QString uid = selectedUserId();
    if (uid.isEmpty()) return;

    const UserRecord existing = OkidiaDatabase::instance().userById(uid);
    if (existing.id.isEmpty()) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Yönetici Düzenle"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(400);

    auto* form = new QFormLayout(&dlg);
    form->setSpacing(10);

    auto* usernameEdit = new QLineEdit(existing.username, &dlg);
    usernameEdit->setReadOnly(true);
    usernameEdit->setStyleSheet("background: rgba(128,128,128,0.15); color: #888;");
    form->addRow(tr("Kullanıcı Adı:"), usernameEdit);

    auto* displayNameEdit = new QLineEdit(existing.displayName, &dlg);
    form->addRow(tr("Ad Soyad:"), displayNameEdit);

    auto* roleCombo = new QComboBox(&dlg);
    roleCombo->addItem(tr("Gözlemci Master (GM)"), "GM");
    roleCombo->addItem(tr("Öğretmen Master (OM)"), "OM");
    roleCombo->addItem(tr("Yönetici Master (YM)"),  "YM");
    int roleIdx = roleCombo->findData(existing.role);
    if (roleIdx >= 0) roleCombo->setCurrentIndex(roleIdx);

    if (existing.role == "YM") {
        roleCombo->setEnabled(false);
        roleCombo->setToolTip(tr("Sistemin tek yöneticisisiniz. Yetkiyi devretmek için başka bir kullanıcıyı YM yapmalısınız."));
    }
    form->addRow(tr("Rol:"), roleCombo);

    auto* passwordEdit = new QLineEdit(&dlg);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(tr("Değiştirmek istemiyorsanız boş bırakın"));
    form->addRow(tr("Yeni Şifre:"), passwordEdit);

    auto* replicaCheck = new QCheckBox(tr("Otomatik sistem yedeği kopyası alabilir (Replika)"), &dlg);
    replicaCheck->setChecked(existing.isBackupReplica || existing.role == "YM");
    if (existing.role == "YM") {
        replicaCheck->setEnabled(false);
    }
    form->addRow(QString(), replicaCheck);

    auto* replicaDevCombo = new QComboBox(&dlg);
    replicaDevCombo->addItem(tr("İlk Giriş Yaptığı Bilgisayar (Otomatik Bağla)"), "__AUTO__");
    int preselectDevIdx = 0;
    for (const auto& d : OkidiaDatabase::instance().allMasterDevices()) {
        const QString host = d.hostname.trimmed();
        if (host.isEmpty()) continue;
        QString label = host;
        if (!d.ip.isEmpty()) label += QString(" (%1)").arg(d.ip);
        if (!d.lastUsername.isEmpty()) label += tr(" — Son giriş: %1").arg(d.lastUsername);
        replicaDevCombo->addItem(label, host);
        int idx = replicaDevCombo->count() - 1;
        replicaDevCombo->setItemData(idx, d.machineId, Qt::UserRole + 1);
        if (!existing.replicaHostname.trimmed().isEmpty() &&
            host.compare(existing.replicaHostname.trimmed(), Qt::CaseInsensitive) == 0) {
            preselectDevIdx = idx;
        } else if (preselectDevIdx == 0 && d.lastUserId == existing.id) {
            preselectDevIdx = idx;
        }
    }
    replicaDevCombo->setCurrentIndex(preselectDevIdx);
    replicaDevCombo->setEnabled(replicaCheck->isChecked());
    connect(replicaCheck, &QCheckBox::toggled, replicaDevCombo, &QWidget::setEnabled);
    connect(roleCombo, &QComboBox::currentIndexChanged, &dlg, [roleCombo, replicaCheck, replicaDevCombo]() {
        const bool isRoleYM = (roleCombo->currentData().toString() == "YM");
        if (isRoleYM) {
            replicaCheck->setChecked(true);
            replicaCheck->setEnabled(false);
            replicaDevCombo->setEnabled(true);
        } else {
            replicaCheck->setEnabled(true);
            replicaDevCombo->setEnabled(replicaCheck->isChecked());
        }
    });
    form->addRow(tr("Yedek Cihazı:"), replicaDevCombo);

    TeacherRecord existingTeacher = OkidiaDatabase::instance().teacherByUsername(existing.username);
    auto* alsoTeacherCheck = new QCheckBox(tr("Akıllı tahtalarda derse giriyor (Öğretmen yetkisi ve QR)"), &dlg);
    alsoTeacherCheck->setChecked(!existingTeacher.id.isEmpty() && existingTeacher.isActive);
    form->addRow(QString(), alsoTeacherCheck);

    auto* errorLbl = new QLabel(&dlg);
    errorLbl->setProperty("status", "error");
    form->addRow(errorLbl);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Kaydet"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, [&]() {
        if (displayNameEdit->text().trimmed().isEmpty()) {
            errorLbl->setText(tr("Ad Soyad boş bırakılamaz."));
            return;
        }
        if (!passwordEdit->text().isEmpty() && passwordEdit->text().length() < 6) {
            errorLbl->setText(tr("Şifre en az 6 karakter olmalı."));
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString newRole = roleCombo->currentData().toString();
    bool isPromotingToYM = (existing.role != "YM" && newRole == "YM");
    bool cleanLocalBackups = false;

    if (isPromotingToYM) {
        QDialog confirmDlg(&dlg);
        confirmDlg.setWindowTitle(tr("Yöneticilik Yetki Devri Onayı"));
        confirmDlg.setWindowFlags(confirmDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
        confirmDlg.setFixedWidth(460);
        auto* vLay = new QVBoxLayout(&confirmDlg);

        auto* warnLbl = new QLabel(
            tr("<b style='color:#e74c3c; font-size:14px;'>DİKKAT: Yöneticilik Yetkisi Devrediliyor!</b><br><br>"
               "<b>%1</b> kullanıcısını <b>Yönetici (YM)</b> yaptığınızda sistemin tek resmi Yöneticisi bu kullanıcı olacaktır.<br>"
               "Kendi yetkiniz oturumunuz kapandığında otomatik olarak <b>Gözlemci (GM)</b> seviyesine düşürülecektir.<br>"
               "Oturumunuz açık kaldığı sürece geçici yönetici haklarınız korunacaktır.").arg(existing.displayName), &confirmDlg);
        warnLbl->setWordWrap(true);
        vLay->addWidget(warnLbl);

        auto* chkClean = new QCheckBox(tr("Bu bilgisayarda saklanan yerel sistem yedeklerini güvenle temizle (Önerilen)"), &confirmDlg);
        chkClean->setChecked(true);
        vLay->addWidget(chkClean);

        auto* bBox = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, &confirmDlg);
        bBox->button(QDialogButtonBox::Yes)->setText(tr("Onaylıyorum ve Devret"));
        bBox->button(QDialogButtonBox::No)->setText(tr("İptal"));
        vLay->addWidget(bBox);

        connect(bBox, &QDialogButtonBox::accepted, &confirmDlg, &QDialog::accept);
        connect(bBox, &QDialogButtonBox::rejected, &confirmDlg, &QDialog::reject);

        if (confirmDlg.exec() != QDialog::Accepted) {
            return;
        }
        cleanLocalBackups = chkClean->isChecked();
    }

    QString pwHash;
    if (!passwordEdit->text().isEmpty()) {
        pwHash = OkidiaUtils::sha256(passwordEdit->text().toUtf8());
    }

    bool isReplica = replicaCheck->isChecked() || (newRole == "YM");
    QString selRepHost;
    QString selRepMid;
    if (isReplica) {
        selRepHost = replicaDevCombo->currentData().toString();
        selRepMid  = replicaDevCombo->currentData(Qt::UserRole + 1).toString();
    }

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbUserUpdate(uid, displayNameEdit->text().trimmed(), newRole, pwHash, isReplica, selRepMid, selRepHost);
        
        if (alsoTeacherCheck->isChecked()) {
            TeacherRecord t = existingTeacher;
            if (t.id.isEmpty()) {
                t.id = OkidiaUtils::generateUuid();
                QByteArray rawSeed = QCryptographicHash::hash(QString(t.id + "_teacher_seed_" + existing.username).toUtf8(),
                                                              QCryptographicHash::Sha256);
                t.seedHex = QString::fromUtf8(rawSeed.toHex());
            }
            t.username    = existing.username;
            t.displayName = displayNameEdit->text().trimmed();
            t.branch      = (newRole == "YM") ? tr("Yönetici") : tr("Öğretmen");
            t.isActive    = true;
            m_session->sendDbTeacherUpsert(t);
        } else if (!existingTeacher.id.isEmpty()) {
            existingTeacher.isActive = false;
            m_session->sendDbTeacherUpsert(existingTeacher);
        }

        if (ok && isPromotingToYM) {
            if (cleanLocalBackups) {
                QString targetDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
                QDir bDir(targetDir);
                for (const QFileInfo& fi : bDir.entryInfoList(QStringList() << "*.okbak", QDir::Files)) {
                    QFile::remove(fi.filePath());
                }
                Logger::info(QObject::tr("UserManagement: Eski YM yerel sistem yedekleri temizlendi."));
            }
            onGenerateRecoveryKey();
        }
    }

    if (!ok) {
        QMessageBox::warning(this, tr("Hata"), tr("Kullanıcı güncelleme isteği gönderilemedi. Sunucu bağlantısını kontrol edin."));
    }
}

void UserManagementPanel::onDeleteUser() {
    const QString uid = selectedUserId();
    if (uid.isEmpty()) return;

    const int row = m_userTable->currentRow();
    const QString uname = m_userTable->item(row, 0)->text();

    const UserRecord u = OkidiaDatabase::instance().userById(uid);
    if (u.role == "YM") {
        QMessageBox::warning(this, tr("Uyarı"),
            tr("Yönetici (YM) hesabı silinemez."));
        return;
    }

    auto ret = QMessageBox::question(this, tr("Kullanıcı Sil"),
        tr("\"%1\" kullanıcısını silmek istiyor musunuz?\n\n"
           "BU İŞLEM:\n"
           " • Kullanıcının tüm yetkilerini iptal eder.\n"
           " • Kullanıcıya özel SSH anahtarlarını siler.\n"
           " • Kullanıcının aktif Master oturumlarını sonlandırır.\n"
           " • Sertifikasını kara listeye alarak girişi engeller.\n\n"
           "Devam etmek istiyor musunuz?").arg(uname),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbUserDelete(uid);
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Hata"), tr("Kullanıcı silme isteği gönderilemedi. Sunucu bağlantısını kontrol edin."));
    } else {
        OkidiaDatabase::instance().removeUser(uid);
        loadUsers();
    }
}

void UserManagementPanel::onResetPassword() {
    const QString uid = selectedUserId();
    if (uid.isEmpty()) return;

    const bool isAdminReset = (m_activeUser.role == "YM") && (uid != m_activeUser.id);

    QDialog dlg(this);
    dlg.setWindowTitle(isAdminReset ? tr("Şifre Sıfırla") : tr("Şifremi Değiştir"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(360);

    auto* mainLay = new QVBoxLayout(&dlg);
    mainLay->setContentsMargins(20, 20, 20, 16);
    mainLay->setSpacing(10);

    auto* titleLbl = new QLabel(isAdminReset ? tr("Şifre Sıfırla") : tr("Şifremi Değiştir"), &dlg);
    titleLbl->setProperty("class", "headerLabel");
    titleLbl->setAlignment(Qt::AlignCenter);
    mainLay->addWidget(titleLbl);

    auto* infoLbl = new QLabel(
        isAdminReset
            ? tr("Kullanıcıya geçici bir şifre atayın.\nKullanıcı bir sonraki girişte yeni şifresini belirlemek zorunda kalacak.")
            : tr("Yeni şifrenizi iki kez girerek onaylayın."),
        &dlg);
    infoLbl->setProperty("status", "info");
    infoLbl->setAlignment(Qt::AlignCenter);
    infoLbl->setWordWrap(true);
    mainLay->addWidget(infoLbl);

    auto* sep = new QFrame(&dlg);
    sep->setFrameShape(QFrame::HLine);
    mainLay->addWidget(sep);

    auto* form = new QFormLayout();
    form->setSpacing(8);

    auto* pwEdit  = new QLineEdit(&dlg);
    auto* pw2Edit = new QLineEdit(&dlg);
    pwEdit->setEchoMode(QLineEdit::Password);
    pw2Edit->setEchoMode(QLineEdit::Password);
    pwEdit->setMinimumHeight(34);
    pw2Edit->setMinimumHeight(34);
    pwEdit->setPlaceholderText(tr("En az 6 karakter"));
    pw2Edit->setPlaceholderText(tr("Şifreyi tekrar girin"));

    form->addRow(isAdminReset ? tr("Geçici Şifre:") : tr("Yeni Şifre:"), pwEdit);
    form->addRow(tr("Tekrar:"), pw2Edit);
    mainLay->addLayout(form);

    auto* errorLbl = new QLabel(&dlg);
    errorLbl->setProperty("status", "error");
    errorLbl->setAlignment(Qt::AlignCenter);
    errorLbl->setWordWrap(true);
    mainLay->addWidget(errorLbl);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(isAdminReset ? tr("Sıfırla") : tr("Kaydet"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    mainLay->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, [&]() {
        const QString pw1 = pwEdit->text();
        const QString pw2 = pw2Edit->text();
        if (pw1.length() < 6) {
            errorLbl->setText(tr("Şifre en az 6 karakter olmalı."));
            return;
        }
        if (pw1 != pw2) {
            errorLbl->setText(tr("Şifreler eşleşmiyor."));
            pw2Edit->clear();
            pw2Edit->setFocus();
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(pw2Edit, &QLineEdit::returnPressed,
            buttons->button(QDialogButtonBox::Ok), &QPushButton::click);

    if (dlg.exec() != QDialog::Accepted) return;

    const QString hash = OkidiaUtils::sha256(pwEdit->text().toUtf8());

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbUserPwReset(uid, hash, isAdminReset);
    }
    if (ok) {
        if (isAdminReset) {
            QMessageBox::information(this, tr("Başarılı"),
                tr("Şifre sıfırlama isteği gönderildi."));
        } else {
            QMessageBox::information(this, tr("Başarılı"), tr("Şifre değiştirme isteği gönderildi."));
        }
    } else {
        QMessageBox::warning(this, tr("Hata"), tr("Şifre güncelleme isteği gönderilemedi. Sunucu bağlantısını kontrol edin."));
    }
}

void UserManagementPanel::onGenerateRecoveryKey() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Hata"), tr("Sunucu bağlı değil. Kurtarma kodunu yenilemek için Sunucu bağlantısı gereklidir."));
        return;
    }

    auto ret = QMessageBox::question(this, tr("Kurtarma Kodunu Yenile"),
        tr("Yeni bir sistem kurtarma kodu üretmek istediğinize emin misiniz?\n\n"
           "ÖNEMLİ: Bu işlem eski kurtarma kodunuzu TAMAMEN geçersiz kılacaktır. Kasadaki veya dosyalarınızdaki eski kopyalar çalışmayacaktır.\n\n"
           "Devam etmek istiyor musunuz?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    QString chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QString key;
    for(int i=0; i<16; ++i) {
        if(i > 0 && i % 4 == 0) key += "-";
        key += chars.at(QRandomGenerator::global()->bounded(chars.length()));
    }

    QString cleanKey = OkidiaUtils::cleanRecoveryKey(key);
    QString hash = OkidiaUtils::sha256(cleanKey.toUtf8());
    
    m_session->requestSetRecoveryKey(hash);
    Logger::info(QObject::tr("UserManagement: Yeni sistem kurtarma kodu hash'i broker'a gönderildi."));

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Yeni Kurtarma Kodu"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setFixedWidth(400);

    auto* mainLay = new QVBoxLayout(&dlg);
    mainLay->setContentsMargins(24, 20, 24, 20);
    mainLay->setSpacing(12);

    auto* titleLbl = new QLabel(tr("Yeni Kurtarma Kodu"), &dlg);
    titleLbl->setProperty("class", "headerLabel");
    titleLbl->setAlignment(Qt::AlignCenter);
    mainLay->addWidget(titleLbl);

    auto* infoLbl = new QLabel(
        tr("Yeni sistem kurtarma kodunuz başarıyla oluşturuldu ve etkinleştirildi. "
           "Eski kodunuz artık geçersizdir.\n\n"
           "Lütfen yeni kodu güvenli bir yerde saklayın:"), &dlg);
    infoLbl->setProperty("status", "info");
    infoLbl->setAlignment(Qt::AlignCenter);
    infoLbl->setWordWrap(true);
    mainLay->addWidget(infoLbl);

    auto* keyLbl = new QLabel(key, &dlg);
    keyLbl->setProperty("class", "recoveryKey");
    keyLbl->setAlignment(Qt::AlignCenter);
    mainLay->addWidget(keyLbl);

    auto* saveBtn = new QPushButton(tr("Dosya Olarak Kaydet"), &dlg);
    saveBtn->setIcon(ThemeManager::icon(":/ui/save.svg"));
    saveBtn->setProperty("class", "primary");
    
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(saveBtn);
    btnRow->addStretch();
    mainLay->addLayout(btnRow);

    auto* sep = new QFrame(&dlg);
    sep->setFrameShape(QFrame::HLine);
    mainLay->addWidget(sep);

    auto* closeBtn = new QPushButton(tr("Kapat"), &dlg);
    mainLay->addWidget(closeBtn, 0, Qt::AlignCenter);

    connect(saveBtn, &QPushButton::clicked, this, [this, &dlg, key]() {
        QString defaultPath = QDir::homePath() + "/" + tr("okidia_kurtarma_kodu.txt");
        QString filePath = OkidiaUtils::getSaveFileName(&dlg,
            tr("Kurtarma Kodunu Kaydet"),
            "recovery_key",
            defaultPath,
            tr("Metin Dosyaları (*.txt)"));

        if (!filePath.isEmpty()) {
            QFile file(filePath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&file);
                out << OkidiaUtils::getRecoveryKeyFileContent(key, tr("OKIDIA GÜVENLİK VE SİSTEM KURTARMA KODU (YENİ)"));
                file.close();

                QMessageBox::information(&dlg, tr("Başarılı"), tr("Yeni kurtarma kodu başarıyla kaydedildi:\n") + filePath);
            } else {
                QMessageBox::warning(&dlg, tr("Hata"), tr("Dosya kaydedilirken bir hata oluştu."));
            }
        }
    });

    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

void UserManagementPanel::onUserContextMenu(const QPoint& pos) {
    QModelIndex idx = m_userTable->indexAt(pos);
    if (!idx.isValid()) return;

    m_userTable->selectRow(idx.row());
    onUserSelectionChanged();

    const QString uid = selectedUserId();
    if (uid.isEmpty()) return;

    const UserRecord u = OkidiaDatabase::instance().userById(uid);
    const bool isYM = (m_activeUser.role == "YM");
    const bool isConnected = m_session && m_session->isConnected();

    QMenu menu(this);

    if (isYM) {
        auto* actEdit = menu.addAction(ThemeManager::icon(":/ui/edit.svg"), tr("Yöneticiyi Düzenle"));
        connect(actEdit, &QAction::triggered, this, &UserManagementPanel::onEditUser);
    }

    auto* actReset = menu.addAction(ThemeManager::icon(":/ui/key.svg"), isYM ? tr("Şifre Sıfırla") : tr("Şifremi Değiştir"));
    actReset->setEnabled(isYM || (uid == m_activeUser.id));
    connect(actReset, &QAction::triggered, this, &UserManagementPanel::onResetPassword);

    if (isYM) {
        const QString replicaMenuText = (u.role == "YM")
            ? tr("Yedek Bilgisayarını Değiştir")
            : (u.isBackupReplica ? tr("İkincil Yedek Yetkisini Kaldır") : tr("İkincil Yedek Yetkisi Ver"));
        auto* actReplica = menu.addAction(ThemeManager::icon(":/ui/shield.svg"), replicaMenuText);
        actReplica->setEnabled(isConnected);
        connect(actReplica, &QAction::triggered, this, &UserManagementPanel::onToggleReplica);

        auto* actJoin = menu.addAction(ThemeManager::icon(":/ui/share.svg"), tr("Davet Kodu Oluştur"));
        actJoin->setEnabled(isConnected);
        connect(actJoin, &QAction::triggered, this, &UserManagementPanel::onGenerateJoinCode);

        auto* actRecovery = menu.addAction(ThemeManager::icon(":/ui/key.svg"), tr("Kurtarma Kodu Yenile"));
        actRecovery->setEnabled(isConnected);
        connect(actRecovery, &QAction::triggered, this, &UserManagementPanel::onGenerateRecoveryKey);

        menu.addSeparator();

        auto* actDel = menu.addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Kullanıcıyı Sil"));
        actDel->setEnabled(u.role != "YM");
        connect(actDel, &QAction::triggered, this, &UserManagementPanel::onDeleteUser);
    }

    menu.exec(m_userTable->viewport()->mapToGlobal(pos));
}
