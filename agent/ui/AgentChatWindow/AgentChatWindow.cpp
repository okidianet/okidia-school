// agent/ui/AgentChatWindow/AgentChatWindow.cpp
#include "AgentChatWindow.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/ui/chat/ChatWidget.h"
#include "../../../common/ui/chat/ChatModel.h"
#include "../../../common/ui/chat/ChatMessage.h"

#include <QCloseEvent>
#include <QShowEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSplitter>
#include <QTabBar>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QIcon>
#include <algorithm>

AgentChatWindow::AgentChatWindow(const QString& myClientId,
                                 const QString& myDisplay,
                                 QWidget* parent)
    : QMainWindow(parent)
    , m_myClientId(myClientId)
    , m_myDisplay(myDisplay)
{
    setWindowTitle(tr("OKIDIA Sohbet"));
    {
        QIcon winIcon = QIcon::fromTheme("okidia-agent");
        winIcon.addFile(":/icons/agent/okidia-agent-connected-16.png", QSize(16, 16));
        winIcon.addFile(":/icons/agent/okidia-agent-connected-22.png", QSize(22, 22));
        winIcon.addFile(":/icons/agent/okidia-agent-connected-32.png", QSize(32, 32));
        setWindowIcon(winIcon);
    }
    setMinimumSize(600, 440);

    auto* central = new QWidget(this);
    auto* mainLay = new QVBoxLayout(central);
    mainLay->setContentsMargins(0,0,0,0);
    mainLay->setSpacing(0);
    setCentralWidget(central);

    // ── Tek ve Birleşik Üst Sekme Çubuğu + Yardım İste Butonu ──────────────
    auto* topHeader = new QWidget(central);
    auto* topLay = new QHBoxLayout(topHeader);
    topLay->setContentsMargins(0, 0, 8, 0);
    topLay->setSpacing(8);

    m_tabBar = new QTabBar(topHeader);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setMovable(false);
    m_tabBar->setExpanding(false);
    m_tabBar->setDrawBase(true);
    m_tabBar->setUsesScrollButtons(true);
    topLay->addWidget(m_tabBar, 1);

    auto* helpBtn = new QPushButton(tr("Yardım İste"), topHeader);
    helpBtn->setIcon(ThemeManager::icon(":/ui/hand.svg"));
    helpBtn->setCursor(Qt::PointingHandCursor);
    helpBtn->setProperty("class", "accentButton");
    helpBtn->setToolTip(tr("Öğretmeninizden yardım istemek için tıklayın"));
    connect(helpBtn, &QPushButton::clicked, this, &AgentChatWindow::onHelpButtonClicked);
    topLay->addWidget(helpBtn, 0, Qt::AlignVCenter);

    mainLay->addWidget(topHeader);

    // ── Ana splitter: Mesaj Alanı | Sağ Üye Listesi ─────────────────────────
    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setHandleWidth(6);
    m_splitter->setChildrenCollapsible(false);
    mainLay->addWidget(m_splitter, 1);

    // 1. Orta: Mesaj Stack
    m_stack = new QStackedWidget(m_splitter);
    m_splitter->addWidget(m_stack);

    // 2. Sağ: Üye Listesi
    m_memberPanel = new QWidget(m_splitter);
    auto* memberL = new QVBoxLayout(m_memberPanel);
    memberL->setContentsMargins(0,0,0,0);
    memberL->setSpacing(0);
    m_memberTitle = new QLabel(tr("  Üyeler"), m_memberPanel);
    m_memberTitle->setProperty("class", "headerLabel");
    m_memberList = new QListWidget(m_memberPanel);

    auto* memberHint = new QLabel(tr("  Çift tık: Öğretmenden yardım iste"), m_memberPanel);
    memberHint->setProperty("status", "info");
    memberHint->setWordWrap(true);
    memberL->addWidget(m_memberTitle);
    memberL->addWidget(m_memberList, 1);
    memberL->addWidget(memberHint);
    m_memberPanel->setMinimumWidth(150);
    m_splitter->addWidget(m_memberPanel);

    m_splitter->setStretchFactor(0, 4);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({460, 180});

    // ── Yanıp Sönme (Flashing / Pulsing) Timer'ı ───────────────────────────
    m_flashTimer = new QTimer(this);
    m_flashTimer->setInterval(500);
    connect(m_flashTimer, &QTimer::timeout, this, &AgentChatWindow::onFlashTimerTick);

    // ── Sinyal Bağlantıları ────────────────────────────────────────────────
    connect(m_tabBar, &QTabBar::currentChanged,    this, &AgentChatWindow::onTabChanged);
    connect(m_tabBar, &QTabBar::tabBarClicked,     this, [this](int idx) {
        if (idx >= 0 && idx < m_tabBar->count()) clearUnread(m_tabBar->tabData(idx).toString());
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &AgentChatWindow::onTabCloseRequested);
    connect(m_memberList, &QListWidget::itemDoubleClicked, this, &AgentChatWindow::onMemberDoubleClicked);
}

// ── Oda Ekle / Çıkar ──────────────────────────────────────────────────────────

void AgentChatWindow::addRoom(const AgentRoomInfo& room) {
    if (m_tabs.contains(room.id)) return;

    auto* tab = new TabItem;
    tab->id = room.id;
    tab->isPrivate = false;
    tab->title = room.name;
    tab->unread = 0;
    tab->model = new ui::chat::ChatModel(this);
    tab->widget = new ui::chat::ChatWidget(this);
    tab->widget->setModel(tab->model);

    connect(tab->widget, &ui::chat::ChatWidget::messageReadyToSend, this, &AgentChatWindow::onChatMessageReadyToSend);
    connect(tab->widget, &ui::chat::ChatWidget::messageContextMenuRequested, this, &AgentChatWindow::onChatContextMenuRequested);

    m_stack->addWidget(tab->widget);
    m_tabs[room.id] = tab;

    const QIcon icon = room.parentRoomId.isEmpty() ? ThemeManager::icon(":/ui/monitor.svg") : ThemeManager::icon(":/ui/group.svg");
    const int idx = m_tabBar->addTab(icon, tab->title);
    m_tabBar->setTabData(idx, room.id);

    // Oda sekmeleri kapatılamaz
    m_tabBar->setTabButton(idx, QTabBar::RightSide, nullptr);

    // Sistem mesajı ekle
    ui::chat::ChatMessage sysMsg;
    sysMsg.isSystem = true;
    sysMsg.content = QString(tr("Sohbete katıldınız · %1")).arg(QDateTime::currentDateTime().toString("hh:mm"));
    tab->model->addMessage(sysMsg);

    if (m_tabBar->count() == 1 || m_activeId.isEmpty()) {
        m_tabBar->setCurrentIndex(idx);
        onTabChanged(idx);
    }
}

void AgentChatWindow::removeRoom(const QString& roomId) {
    auto* tab = m_tabs.take(roomId);
    if (!tab) return;

    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == roomId) {
            m_tabBar->removeTab(i);
            break;
        }
    }

    m_stack->removeWidget(tab->widget);
    delete tab->widget;
    delete tab->model;
    delete tab;

    if (m_activeId == roomId) {
        m_activeId.clear();
        if (m_tabBar->count() > 0) {
            m_tabBar->setCurrentIndex(0);
        }
    }

    if (m_tabBar->count() == 0) {
        emit windowClosed();
        hide();
    }
}

// ── Üye Yönetimi ──────────────────────────────────────────────────────────────

void AgentChatWindow::addMember(const QString& roomId, const AgentMemberInfo& member) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;
    tab->memberMap[member.clientId] = member;
    if (m_activeId == roomId) {
        rebuildMemberList(tab);
    }
}

void AgentChatWindow::removeMember(const QString& roomId, const QString& clientId) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;
    tab->memberMap.remove(clientId);
    if (m_activeId == roomId) {
        rebuildMemberList(tab);
    }
}

void AgentChatWindow::setMemberOnline(const QString& roomId, const QString& clientId, bool online) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;
    if (tab->memberMap.contains(clientId)) {
        tab->memberMap[clientId].online = online;
        if (!online) tab->memberMap[clientId].windowOpen = false;
    }
    if (m_activeId == roomId) {
        rebuildMemberList(tab);
    }
}

void AgentChatWindow::setMemberWindowState(const QString& roomId, const QString& clientId, bool windowOpen) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;
    if (tab->memberMap.contains(clientId)) {
        tab->memberMap[clientId].windowOpen = windowOpen;
        if (windowOpen) tab->memberMap[clientId].online = true;
    }
    if (m_activeId == roomId) {
        rebuildMemberList(tab);
    }
}

void AgentChatWindow::rebuildMemberList(TabItem* tab) {
    if (!tab || tab->isPrivate) {
        m_memberList->clear();
        m_memberPanel->setVisible(false);
        return;
    }

    m_memberPanel->setVisible(true);
    m_memberList->setUpdatesEnabled(false);
    m_memberList->clear();

    auto mode = ThemeManager::instance().currentMode();
    bool isDark = (mode == ThemeManager::ThemeMode::Dark);

    // 1. Gruplara Ayır
    QList<AgentMemberInfo> managers;
    QList<AgentMemberInfo> observers;
    QList<AgentMemberInfo> teachers;
    QList<AgentMemberInfo> students;

    for (const AgentMemberInfo& m : tab->memberMap) {
        if (m.role == 1 || (m.clientId.startsWith("master:") && m.role == 0)) {
            managers.append(m);
        } else if (m.role == 4) {
            observers.append(m);
        } else if (m.role == 2) {
            teachers.append(m);
        } else {
            students.append(m);
        }
    }

    // Öğrencileri alfabetik sırala
    std::sort(students.begin(), students.end(), [](const AgentMemberInfo& a, const AgentMemberInfo& b) {
        return QString::localeAwareCompare(a.displayName, b.displayName) < 0;
    });

    // 1. Yöneticiler
    for (const AgentMemberInfo& m : managers) {
        auto* item = new QListWidgetItem();
        QString cleanName = m.displayName;
        if (!cleanName.contains(tr("Yönetici"))) {
            cleanName = tr("[Yönetici] ") + cleanName;
        }
        item->setText(cleanName);
        item->setIcon(ThemeManager::icon(m.online ? ":/ui/online.svg" : ":/ui/offline.svg"));
        item->setForeground(m.online ? (isDark ? QColor("#fbbf24") : QColor("#b45309")) : (isDark ? QColor("#64748b") : QColor("#94a3b8")));
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        item->setToolTip(m.online ? tr("Yönetici · Aktif (Çift tık: Yardım İste)") : tr("Yönetici · Çevrimdışı"));
        item->setData(Qt::UserRole,     m.clientId);
        item->setData(Qt::UserRole + 1, m.displayName);
        item->setData(Qt::UserRole + 2, m.role == 0 ? 1 : m.role);
        m_memberList->addItem(item);
    }

    // 2. Gözlemciler
    for (const AgentMemberInfo& m : observers) {
        auto* item = new QListWidgetItem();
        QString cleanName = m.displayName;
        if (!cleanName.contains(tr("Gözlemci"))) {
            cleanName = tr("[Gözlemci] ") + cleanName;
        }
        item->setText(cleanName);
        item->setIcon(ThemeManager::icon(m.online ? ":/ui/online.svg" : ":/ui/offline.svg"));
        item->setForeground(m.online ? (isDark ? QColor("#c084fc") : QColor("#7e22ce")) : (isDark ? QColor("#64748b") : QColor("#94a3b8")));
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        item->setToolTip(m.online ? tr("Gözlemci · Aktif") : tr("Gözlemci · Çevrimdışı"));
        item->setData(Qt::UserRole,     m.clientId);
        item->setData(Qt::UserRole + 1, m.displayName);
        item->setData(Qt::UserRole + 2, m.role);
        m_memberList->addItem(item);
    }

    // 3. Öğretmenler
    for (const AgentMemberInfo& m : teachers) {
        auto* item = new QListWidgetItem();
        QString cleanName = m.displayName;
        if (!cleanName.contains(tr("Öğretmen"))) {
            cleanName = tr("[Öğretmen] ") + cleanName;
        }
        item->setText(cleanName);
        item->setIcon(ThemeManager::icon(m.online ? ":/ui/online.svg" : ":/ui/offline.svg"));
        item->setForeground(m.online ? (isDark ? QColor("#60a5fa") : QColor("#0369a1")) : (isDark ? QColor("#64748b") : QColor("#94a3b8")));
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        item->setToolTip(m.online ? tr("Öğretmen · Aktif (Çift tık: Yardım İste)") : tr("Öğretmen · Çevrimdışı"));
        item->setData(Qt::UserRole,     m.clientId);
        item->setData(Qt::UserRole + 1, m.displayName);
        item->setData(Qt::UserRole + 2, m.role);
        m_memberList->addItem(item);
    }

    // Ayırıcı
    if ((!managers.isEmpty() || !observers.isEmpty() || !teachers.isEmpty()) && !students.isEmpty()) {
        auto* sep = new QListWidgetItem(tr("───── Üyeler ─────"));
        sep->setFlags(Qt::NoItemFlags);
        sep->setTextAlignment(Qt::AlignCenter);
        sep->setForeground(isDark ? QColor("#64748b") : QColor("#94a3b8"));
        m_memberList->addItem(sep);
    }

    // 3. Üyeler (Önce "Ben", ardından diğer üyeler A-Z)
    AgentMemberInfo myInfo;
    bool hasMe = false;
    QList<AgentMemberInfo> otherStudents;

    for (const AgentMemberInfo& m : students) {
        if (m.clientId == m_myClientId) {
            myInfo = m;
            hasMe = true;
        } else {
            otherStudents.append(m);
        }
    }

    // Eğer oda üye listesinde kendisi henüz yoksa bile ekle
    if (!hasMe && !m_myClientId.isEmpty()) {
        myInfo.clientId = m_myClientId;
        myInfo.displayName = m_myDisplay.isEmpty() ? tr("Ben") : m_myDisplay;
        myInfo.online = true;
        myInfo.windowOpen = true;
        myInfo.role = 3;
        hasMe = true;
    }

    // A) Önce KENDİSİ (Ben) en tepede, belirgin renk ve online ikonla
    if (hasMe) {
        auto* item = new QListWidgetItem();
        item->setText(myInfo.displayName + tr(" (Ben)"));
        item->setIcon(ThemeManager::icon(":/ui/online.svg"));
        item->setToolTip(tr("Siz (Aktif)"));

        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);

        // Koyu temada açık/parlak mavi (#38bdf8), açık temada koyu/belirgin okyanus mavisi (#0284c7)
        item->setForeground(isDark ? QColor("#38bdf8") : QColor("#0284c7"));

        item->setData(Qt::UserRole,     myInfo.clientId);
        item->setData(Qt::UserRole + 1, myInfo.displayName);
        item->setData(Qt::UserRole + 2, 3);
        m_memberList->addItem(item);
    }

    // B) Diğer üyeler A-Z alfabetik sırada
    for (const AgentMemberInfo& m : otherStudents) {
        auto* item = new QListWidgetItem();
        item->setText(m.displayName);

        if (m.online && m.windowOpen) {
            item->setIcon(ThemeManager::icon(":/ui/online.svg"));
            item->setToolTip(tr("Aktif · Sohbet Açık (Çift tık: Özel Sohbet İste)"));
            item->setForeground(isDark ? QColor("#f8fafc") : QColor("#0f172a"));
        } else {
            item->setIcon(ThemeManager::icon(":/ui/offline.svg"));
            item->setToolTip(m.online ? tr("Sohbet Kapalı") : tr("Çevrimdışı"));
            item->setForeground(isDark ? QColor("#64748b") : QColor("#94a3b8"));
        }

        item->setData(Qt::UserRole,     m.clientId);
        item->setData(Qt::UserRole + 1, m.displayName);
        item->setData(Qt::UserRole + 2, m.role == 0 ? 3 : m.role);
        m_memberList->addItem(item);
    }
    m_memberList->setUpdatesEnabled(true);
}

// ── Mesaj Ekleme ──────────────────────────────────────────────────────────────

void AgentChatWindow::appendMessage(const QString& roomId, const AgentMessage& msg) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;

    ui::chat::ChatMessage m;
    m.isMine = msg.fromMe;
    m.isSystem = msg.isSystem;
    m.content = msg.content;
    m.timestamp = msg.timestamp;
    m.senderName = msg.fromDisplay;
    m.senderRole = static_cast<okidia::SenderRole>(msg.senderRole);

    tab->model->addMessage(m);

    if (!msg.isSystem && !msg.fromMe) {
        notifyUnread(roomId);
    }
}

// ── Özel Sohbet Sekmeleri ─────────────────────────────────────────────────────

void AgentChatWindow::openPrivateTab(const QString& sessionId, const QString& peerDisplay) {
    if (m_tabs.contains(sessionId)) {
        for (int i = 0; i < m_tabBar->count(); ++i) {
            if (m_tabBar->tabData(i).toString() == sessionId) {
                m_tabBar->setCurrentIndex(i);
                break;
            }
        }
        return;
    }

    auto* tab = new TabItem;
    tab->id = sessionId;
    tab->isPrivate = true;
    QString cleanDisplay = peerDisplay.trimmed();
    if (cleanDisplay.isEmpty()) {
        cleanDisplay = tr("Öğretmen");
    }
    tab->title = tr("[Özel] ") + cleanDisplay;
    tab->unread = 0;
    tab->model = new ui::chat::ChatModel(this);
    tab->widget = new ui::chat::ChatWidget(this);
    tab->widget->setModel(tab->model);

    connect(tab->widget, &ui::chat::ChatWidget::messageReadyToSend, this, &AgentChatWindow::onChatMessageReadyToSend);
    connect(tab->widget, &ui::chat::ChatWidget::messageContextMenuRequested, this, &AgentChatWindow::onChatContextMenuRequested);

    m_stack->addWidget(tab->widget);
    m_tabs[sessionId] = tab;

    ui::chat::ChatMessage sysMsg;
    sysMsg.isSystem = true;
    sysMsg.content = QString(tr("%1 ile özel sohbet başladı · %2")).arg(cleanDisplay).arg(QDateTime::currentDateTime().toString("hh:mm"));
    tab->model->addMessage(sysMsg);

    const int idx = m_tabBar->addTab(ThemeManager::icon(":/ui/lock.svg"), tab->title);
    m_tabBar->setTabData(idx, sessionId);

    m_tabBar->setCurrentIndex(idx);
    onTabChanged(idx);
}

void AgentChatWindow::closePrivateTab(const QString& sessionId) {
    auto* tab = m_tabs.take(sessionId);
    if (!tab) return;

    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == sessionId) {
            m_tabBar->removeTab(i);
            break;
        }
    }

    m_stack->removeWidget(tab->widget);
    delete tab->widget;
    delete tab->model;
    delete tab;

    if (m_activeId == sessionId) {
        m_activeId.clear();
        if (m_tabBar->count() > 0) {
            m_tabBar->setCurrentIndex(0);
        }
    }

    if (m_tabBar->count() == 0) {
        emit windowClosed();
        hide();
    }
}

void AgentChatWindow::appendPrivateMsg(const QString& sessionId,
                                       const QString& content,
                                       bool fromMe,
                                       qint64 timestamp) {
    auto* tab = m_tabs.value(sessionId);
    if (!tab) return;

    ui::chat::ChatMessage m;
    m.isMine = fromMe;
    m.isSystem = false;
    m.content = content;
    m.timestamp = timestamp;
    
    QString senderDisplay = fromMe ? m_myDisplay : tab->title;
    if (!fromMe && senderDisplay.startsWith(tr("[Özel] "))) {
        senderDisplay = senderDisplay.mid(7);
    }
    m.senderName = senderDisplay;
    m.senderRole = fromMe ? okidia::ROLE_CLIENT : okidia::ROLE_ADMIN;

    tab->model->addMessage(m);

    if (!fromMe) {
        notifyUnread(sessionId);
    }
}

void AgentChatWindow::showPrivateRequest(const QString& requestId,
                                         const QString& fromDisplay) {
    Q_UNUSED(requestId);
    Q_UNUSED(fromDisplay);
}

void AgentChatWindow::setChatLocked(const QString& roomId, bool locked) {
    auto* tab = m_tabs.value(roomId);
    if (tab && tab->widget) {
        tab->widget->setLocked(locked, tr("Sohbet kilitli — mesaj gönderilemez"));
    }

    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == roomId) {
            QString cleanName = tab ? tab->title : m_tabBar->tabText(i);
            cleanName.remove(" (Kilitli)");
            m_tabBar->setTabText(i, cleanName + (locked ? " (Kilitli)" : ""));
            break;
        }
    }
}

void AgentChatWindow::setPrivateLocked(bool locked) {
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        if (it.value()->isPrivate) {
            it.value()->widget->setLocked(locked, tr("Özel sohbet kilitli"));
        }
    }
}

// ── Tab Değişim & Yanıp Sönme ─────────────────────────────────────────────────

void AgentChatWindow::onTabChanged(int index) {
    if (index < 0 || index >= m_tabBar->count()) {
        m_activeId.clear();
        m_memberList->clear();
        return;
    }

    const QString id = m_tabBar->tabData(index).toString();
    if (id.isEmpty()) return;

    m_activeId = id;
    clearUnread(id);

    auto* tab = m_tabs.value(id);
    if (!tab) return;

    m_stack->setCurrentWidget(tab->widget);

    if (tab->isPrivate) {
        m_memberPanel->setVisible(false);
    } else {
        m_memberPanel->setVisible(true);
        rebuildMemberList(tab);
    }
}

void AgentChatWindow::onTabCloseRequested(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    const QString id = m_tabBar->tabData(index).toString();
    auto* tab = m_tabs.value(id);
    if (!tab || !tab->isPrivate) return;

    emit privateSessionCloseRequested(id);
    closePrivateTab(id);
}

void AgentChatWindow::onFlashTimerTick() {
    m_flashState = !m_flashState;
    bool anyUnread = false;

    for (int i = 0; i < m_tabBar->count(); ++i) {
        const QString id = m_tabBar->tabData(i).toString();
        auto* tab = m_tabs.value(id);
        if (tab && tab->unread > 0) {
            anyUnread = true;
            if (m_flashState) {
                m_tabBar->setTabTextColor(i, QColor("#f59e0b")); // Parlak amber
            } else {
                m_tabBar->setTabTextColor(i, QColor("#38bdf8")); // Parlak cyan
            }
        }
    }

    if (!anyUnread) {
        m_flashTimer->stop();
    }
}

void AgentChatWindow::notifyUnread(const QString& id) {
    auto* tab = m_tabs.value(id);
    if (!tab) return;

    if (id == m_activeId && isVisible()) {
        return;
    }

    tab->unread++;
    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == id) {
            updateTabDisplay(i);
            break;
        }
    }

    if (!m_flashTimer->isActive()) {
        m_flashTimer->start();
    }
}

void AgentChatWindow::clearUnread(const QString& id) {
    auto* tab = m_tabs.value(id);
    if (!tab) return;

    tab->unread = 0;
    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == id) {
            updateTabDisplay(i);
            break;
        }
    }
}

void AgentChatWindow::updateTabDisplay(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= m_tabBar->count()) return;
    const QString id = m_tabBar->tabData(tabIndex).toString();
    auto* tab = m_tabs.value(id);
    if (!tab) return;

    if (tab->unread > 0) {
        m_tabBar->setTabText(tabIndex, QString("💬 (%1) %2").arg(tab->unread).arg(tab->title));
        QFont f = m_tabBar->font();
        f.setBold(true);
        m_tabBar->setFont(f);
    } else {
        m_tabBar->setTabText(tabIndex, tab->title);
        auto mode = ThemeManager::instance().currentMode();
        QColor normalColor = (mode == ThemeManager::ThemeMode::Dark) ? QColor("#e2e8f0") : QColor("#1e293b");
        m_tabBar->setTabTextColor(tabIndex, normalColor);
    }
}

// ── Gönder & Sinyaller ────────────────────────────────────────────────────────

void AgentChatWindow::onChatMessageReadyToSend(const QString& text) {
    QString targetId = m_activeId;
    if (targetId.isEmpty() && m_tabBar->currentIndex() >= 0) {
        targetId = m_tabBar->tabData(m_tabBar->currentIndex()).toString();
        m_activeId = targetId;
    }
    if (targetId.isEmpty()) return;

    auto* tab = m_tabs.value(targetId);
    if (!tab) return;

    if (tab->isPrivate) {
        emit privateMessageSent(targetId, text);
    } else {
        emit messageSent(targetId, text);
    }
}

void AgentChatWindow::onChatContextMenuRequested(const QString& msgId, const QPoint& pos) {
    Q_UNUSED(msgId);
    Q_UNUSED(pos);
}

void AgentChatWindow::onMemberDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString clientId    = item->data(Qt::UserRole).toString();
    const QString displayName = item->data(Qt::UserRole + 1).toString();
    const int     role        = item->data(Qt::UserRole + 2).toInt();
    if (clientId.isEmpty() || clientId == m_myClientId) return;

    bool isTeacher = (role == 1 || role == 2 || clientId.startsWith("master:"));
    if (isTeacher) {
        emit privateRequested(clientId, displayName);
    } else {
        QMessageBox::information(this, tr("Özel Sohbet"),
            tr("Sadece yöneticilerle veya öğretmeninizle özel sohbet yapabilirsiniz."));
    }
}

void AgentChatWindow::closeEvent(QCloseEvent* ev) {
    emit windowClosed();
    hide();
    ev->ignore();
}

void AgentChatWindow::showEvent(QShowEvent* ev) {
    QMainWindow::showEvent(ev);
    if (!m_activeId.isEmpty()) {
        clearUnread(m_activeId);
        if (auto* tab = m_tabs.value(m_activeId)) {
            rebuildMemberList(tab);
        }
    }
}

void AgentChatWindow::changeEvent(QEvent* ev) {
    QMainWindow::changeEvent(ev);
    if (ev->type() == QEvent::ActivationChange && isActiveWindow()) {
        if (!m_activeId.isEmpty()) {
            clearUnread(m_activeId);
        }
    }
}

void AgentChatWindow::onHelpButtonClicked() {
    bool ok = false;
    QString note = QInputDialog::getText(this, tr("Yardım İste (El Kaldır)"),
        tr("Öğretmeninize iletmek istediğiniz soru veya notu yazın (opsiyonel):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) return;

    emit handRaiseRequested(note);

    QMessageBox::information(this, tr("Yardım Talebi"),
        tr("Yardım talebiniz öğretmeninize iletildi."));
}
