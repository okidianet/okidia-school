// master/ui/UserManagementPanel/UserManagementPanelClasses.cpp
#include "UserManagementPanel.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/db/DbRecords.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>

void UserManagementPanel::setupAssignTab() {
    m_userCombo  = new QComboBox(this);
    m_classList  = new QListWidget(this);
    m_assignList = new QListWidget(this);

    m_classList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_assignList->setSelectionMode(QAbstractItemView::ExtendedSelection);

    m_classList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_classList, &QListWidget::customContextMenuRequested,
            this, &UserManagementPanel::onClassListContextMenu);
    connect(m_classList, &QListWidget::itemDoubleClicked,
            this, &UserManagementPanel::onGrantPermission);

    m_assignList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_assignList, &QListWidget::customContextMenuRequested,
            this, &UserManagementPanel::onAssignListContextMenu);
    connect(m_assignList, &QListWidget::itemDoubleClicked,
            this, &UserManagementPanel::onRevokePermission);

    m_btnGrant  = new QPushButton(tr("Seçili Sınıfları Ata →"), this);
    m_btnRevoke = new QPushButton(tr("← Seçili Atamaları Kaldır"), this);

    connect(m_userCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UserManagementPanel::onUserForAssignChanged);
    connect(m_btnGrant,  &QPushButton::clicked, this, &UserManagementPanel::onGrantPermission);
    connect(m_btnRevoke, &QPushButton::clicked, this, &UserManagementPanel::onRevokePermission);
}

void UserManagementPanel::loadClasses() {
    const QList<ClassRecord> all = OkidiaDatabase::instance().allClasses();
    m_classes.clear();
    for (const auto& c : all) {
        // Yalnızca ana sınıfları al; alt gruplar hiyerarşik miras ile otomatik yönetilir
        if (c.type != "group" && c.parentId.isEmpty()) {
            m_classes.append(c);
        }
    }
    m_classList->clear();
    for (const auto& c : m_classes) {
        auto* item = new QListWidgetItem(c.icon + "  " + c.name, m_classList);
        item->setData(Qt::UserRole, c.id);
    }
}

void UserManagementPanel::loadAssignments(const QString& userId) {
    m_assignList->clear();
    if (userId.isEmpty()) return;
    const QStringList assigned = OkidiaDatabase::instance().classIdsForUser(userId);
    for (const auto& c : m_classes) {
        if (assigned.contains(c.id)) {
            auto* item = new QListWidgetItem(c.icon + "  " + c.name, m_assignList);
            item->setData(Qt::UserRole, c.id);
        }
    }
}

void UserManagementPanel::onUserForAssignChanged(int index) {
    if (index < 0) return;
    loadAssignments(m_userCombo->itemData(index).toString());
}

void UserManagementPanel::onGrantPermission() {
    if (m_activeUser.role != "YM") return;  // sadece YM
    const QString userId = m_userCombo->currentData().toString();
    if (userId.isEmpty()) return;

    const auto items = m_classList->selectedItems();
    bool sentAny = false;
    for (auto* item : items) {
        const QString classId = item->data(Qt::UserRole).toString();
        if (OkidiaDatabase::instance().isOpen()) {
            OkidiaDatabase::instance().grantPermission(userId, classId, "full");
        }
        if (m_session && m_session->sendDbUserGrant(userId, classId, "full")) {
            sentAny = true;
        }
    }
    loadAssignments(userId);
    if (!sentAny && !items.isEmpty()) {
        QMessageBox::warning(this, tr("Hata"), tr("Yetki atama isteği gönderilemedi. Sunucu bağlantısını kontrol edin."));
    }
}

void UserManagementPanel::onRevokePermission() {
    if (m_activeUser.role != "YM") return;  // sadece YM
    const QString userId = m_userCombo->currentData().toString();
    if (userId.isEmpty()) return;

    // OM'nin kendi oluşturduğu sınıflarını silme: sadece YM
    // (createdBy kontrolü — YM tüm atamaları kaldırabilir)
    const auto items = m_assignList->selectedItems();
    bool sentAny = false;
    for (auto* item : items) {
        const QString classId = item->data(Qt::UserRole).toString();
        if (OkidiaDatabase::instance().isOpen()) {
            OkidiaDatabase::instance().revokePermission(userId, classId);
        }
        if (m_session && m_session->sendDbUserRevoke(userId, classId)) {
            sentAny = true;
        }
    }
    loadAssignments(userId);
    if (!sentAny && !items.isEmpty()) {
        QMessageBox::warning(this, tr("Hata"), tr("Yetki kaldırma isteği gönderilemedi. Sunucu bağlantısını kontrol edin."));
    }
}

void UserManagementPanel::onClassListContextMenu(const QPoint& pos) {
    if (m_classList->selectedItems().isEmpty()) return;
    if (m_activeUser.role != "YM") return;

    QMenu menu(this);
    auto* actGrant = menu.addAction(ThemeManager::icon(":/ui/add.svg"), tr("Seçili Sınıfları Kullanıcıya Ata"));
    connect(actGrant, &QAction::triggered, this, &UserManagementPanel::onGrantPermission);
    menu.exec(m_classList->viewport()->mapToGlobal(pos));
}

void UserManagementPanel::onAssignListContextMenu(const QPoint& pos) {
    if (m_assignList->selectedItems().isEmpty()) return;
    if (m_activeUser.role != "YM") return;

    QMenu menu(this);
    auto* actRevoke = menu.addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Seçili Sınıf Atamalarını Kaldır"));
    connect(actRevoke, &QAction::triggered, this, &UserManagementPanel::onRevokePermission);
    menu.exec(m_assignList->viewport()->mapToGlobal(pos));
}
