// master/ui/UserManagementPanel/UserManagementPanel.cpp
#include "UserManagementPanel.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/db/DbRecords.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLabel>

UserManagementPanel::UserManagementPanel(SessionManager* session, QWidget* parent) 
    : QWidget(parent), m_session(session) {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &UserManagementPanel::globalContextMenuRequested);

    auto* tabs = new QTabWidget(this);

    mainLay->addWidget(tabs);

    // Tab 1: Yöneticiler
    auto* tab1 = new QWidget;
    setupUserTab();
    auto* lay1 = new QVBoxLayout(tab1);
    lay1->setContentsMargins(8, 8, 8, 8);
    lay1->addWidget(m_userTable, 1);
    m_replicaNoticeWidget = new QWidget(tab1);
    auto* noticeLay = new QHBoxLayout(m_replicaNoticeWidget);
    noticeLay->setContentsMargins(8, 4, 8, 4);
    noticeLay->setSpacing(8);
    auto* noticeIconLbl = new QLabel(m_replicaNoticeWidget);
    noticeIconLbl->setPixmap(ThemeManager::icon(":/ui/shield.svg").pixmap(16, 16));
    noticeLay->addWidget(noticeIconLbl);
    m_replicaNoticeLabel = new QLabel(m_replicaNoticeWidget);
    m_replicaNoticeLabel->setText(tr("Tavsiye: Sunucu arızalarına karşı en az bir öğretmene veya yedek bilgisayara 'İkincil Yedek Alıcısı' yetkisi vermeniz önerilir."));
    m_replicaNoticeLabel->setWordWrap(true);
    m_replicaNoticeLabel->setStyleSheet(QString("color: %1; font-size: 11px;").arg(ThemeManager::instance().warningColor().name()));
    noticeLay->addWidget(m_replicaNoticeLabel, 1);
    m_replicaNoticeWidget->setVisible(false);
    lay1->addWidget(m_replicaNoticeWidget);

    auto* row1 = new QHBoxLayout();
    row1->addWidget(m_btnAdd);
    row1->addWidget(m_btnEdit);
    row1->addWidget(m_btnReset);
    row1->addWidget(m_btnJoinCode);
    row1->addWidget(m_btnRecovery);
    row1->addWidget(m_btnReplica);
    row1->addStretch();
    row1->addWidget(m_btnDelete);
    lay1->addLayout(row1);
    tabs->addTab(tab1, ThemeManager::icon(":/ui/key.svg"), tr(" Yöneticiler"));

    // Tab 2: Öğretmenler (Akıllı Tahta Branş Öğretmenleri)
    auto* tab2 = new QWidget;
    setupTeacherTab();
    auto* layTeacher = new QVBoxLayout(tab2);
    layTeacher->setContentsMargins(8, 8, 8, 8);
    layTeacher->addWidget(m_teacherTable, 1);
    auto* rowTeacher = new QHBoxLayout();
    rowTeacher->addWidget(m_btnTeacherAdd);
    rowTeacher->addWidget(m_btnTeacherEdit);
    rowTeacher->addWidget(m_btnTeacherMobileQr);
    rowTeacher->addWidget(m_btnTeacherSync);
    rowTeacher->addStretch();
    rowTeacher->addWidget(m_btnTeacherDelete);
    layTeacher->addLayout(rowTeacher);
    tabs->addTab(tab2, ThemeManager::icon(":/ui/teacher.svg"), tr(" Öğretmenler"));

    // Tab 3: Sınıf atamaları
    auto* tab3 = new QWidget;
    setupAssignTab();
    auto* lay2 = new QVBoxLayout(tab3);
    lay2->setContentsMargins(8, 8, 8, 8);
    auto* comboRow = new QHBoxLayout();
    comboRow->addWidget(new QLabel(tr("Kullanıcı:"), tab3));
    comboRow->addWidget(m_userCombo, 1);
    lay2->addLayout(comboRow);
    auto* listsRow = new QHBoxLayout();
    listsRow->setSpacing(8);
    auto* leftBox = new QVBoxLayout();
    leftBox->addWidget(new QLabel(tr("Tüm sınıflar:"), tab3));
    leftBox->addWidget(m_classList, 1);
    leftBox->addWidget(m_btnGrant);
    auto* rightBox = new QVBoxLayout();
    rightBox->addWidget(new QLabel(tr("Atanmış sınıflar:"), tab3));
    rightBox->addWidget(m_assignList, 1);
    rightBox->addWidget(m_btnRevoke);
    listsRow->addLayout(leftBox);
    listsRow->addLayout(rightBox);
    lay2->addLayout(listsRow, 1);
    tabs->addTab(tab3, ThemeManager::icon(":/ui/school.svg"), tr(" Sınıf Atamaları"));

    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) loadUsers();
        else if (index == 1) loadTeachers();
        else if (index == 2) {
            loadClasses();
            if (m_userCombo && m_userCombo->currentIndex() >= 0)
                loadAssignments(m_userCombo->currentData().toString());
        }
    });

    if (m_session) {
        connect(m_session, &SessionManager::databaseReloaded, this, &UserManagementPanel::refresh);
        connect(m_session, &SessionManager::dbRestoreResult, this, [this](bool success, const QString&) {
            if (success) {
                refresh();
            }
        });
    }

    refresh();
}

void UserManagementPanel::refresh() {
    loadUsers();
    loadTeachers();
    loadClasses();
    if (m_userCombo && m_userCombo->currentIndex() >= 0)
        loadAssignments(m_userCombo->currentData().toString());
}

void UserManagementPanel::setActiveUser(const UserRecord& user) {
    m_activeUser = user;
    const bool isYM = (user.role == "YM");

    // Yöneticiler Sekmesi: OM Kullanıcı Ekle ve Sil butonlarını göremez
    if (m_btnAdd)    m_btnAdd->setVisible(isYM);
    if (m_btnDelete) m_btnDelete->setVisible(isYM);

    if (m_btnReset) {
        m_btnReset->setVisible(true);
        m_btnReset->setToolTip(isYM
            ? tr("Seçili yöneticinin şifresini sıfırla")
            : tr("Kendi şifreni değiştir"));
    }

    if (m_btnJoinCode) m_btnJoinCode->setVisible(isYM);
    if (m_btnRecovery) m_btnRecovery->setVisible(isYM);
    if (m_btnReplica)  m_btnReplica->setVisible(isYM);

    // Öğretmenler Sekmesi: Sadece YM öğretmen ekleyip silebilir
    if (m_btnTeacherAdd)    m_btnTeacherAdd->setVisible(isYM);
    if (m_btnTeacherEdit)   m_btnTeacherEdit->setVisible(isYM);
    if (m_btnTeacherDelete) m_btnTeacherDelete->setVisible(isYM);
    if (m_btnTeacherSync)   m_btnTeacherSync->setVisible(isYM);

    // Sınıf atama sekmesi: OM sadece görebilir, grant/revoke yapamaz
    if (m_btnGrant)  m_btnGrant->setVisible(isYM);
    if (m_btnRevoke) m_btnRevoke->setVisible(isYM);

    refresh();
}
