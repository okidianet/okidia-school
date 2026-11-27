// master/ui/ChatWindow/ChatWindow.cpp
#include "ChatWindow.h"
#include "RoomJoinRequestDialog.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/ui/chat/ChatWidget.h"
#include "../../../common/ui/chat/ChatModel.h"
#include "../../../common/ui/chat/ChatMessage.h"
#include "../../../common/utils/OkidiaUtils.h"

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
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QApplication>
#include <QHeaderView>
#include <QHostInfo>
#include <QIcon>
#include <QMessageBox>
#include <QComboBox>
#include <QMenu>
#include <QAction>
#include <QUuid>
#include <algorithm>

ChatWindow::ChatWindow(RoomManager* rooms, PrivateChatManager* priv, SessionManager* session, QWidget* parent)
    : QMainWindow(parent), m_rooms(rooms), m_priv(priv), m_session(session)
{
    setWindowTitle(tr("OKIDIA Sohbet"));
    setMinimumSize(880, 580);

    auto* central = new QWidget(this);
    auto* mainLay = new QVBoxLayout(central);
    mainLay->setContentsMargins(0,0,0,0);
    mainLay->setSpacing(0);
    setCentralWidget(central);

    // ── Tek ve Birleşik Üst Sekme Çubuğu (Unified TabBar) ───────────────────
    m_tabBar = new QTabBar(central);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setMovable(false);
    m_tabBar->setExpanding(false);
    m_tabBar->setDrawBase(true);
    m_tabBar->setUsesScrollButtons(true);
    mainLay->addWidget(m_tabBar);

    // ── Ana splitter: Sol Oda Ağacı | Orta Mesaj Akışı | Sağ Üye Listesi ──
    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setHandleWidth(4);
    mainLay->addWidget(m_splitter, 1);

    // 1. Sol: Oda Ağacı
    m_leftW = new QWidget(m_splitter);
    auto* leftL = new QVBoxLayout(m_leftW);
    leftL->setContentsMargins(4,4,4,4);
    leftL->setSpacing(4);
    auto* leftTitle = new QLabel(tr("  Odalar"), m_leftW);
    leftTitle->setProperty("class", "headerLabel");
    leftL->addWidget(leftTitle);

    m_roomFilterCombo = new QComboBox(m_leftW);
    m_roomFilterCombo->addItem(ThemeManager::icon(":/ui/group.svg"), tr("Tüm Odalar"));
    m_roomFilterCombo->addItem(ThemeManager::icon(":/ui/school.svg"), tr("Kendi Sınıflarım"));
    m_roomFilterCombo->setStyleSheet(
        "QComboBox { padding: 4px 8px; border-radius: 4px; font-size: 12px; }"
    );
    connect(m_roomFilterCombo, &QComboBox::currentIndexChanged, this, &ChatWindow::onRoomFilterChanged);
    leftL->addWidget(m_roomFilterCombo);

    m_roomTree = new QTreeWidget(m_leftW);
    m_roomTree->setHeaderHidden(true);
    m_roomTree->setRootIsDecorated(true);
    m_roomTree->setIconSize(QSize(16, 16));
    m_roomTree->setColumnCount(3);
    m_roomTree->setColumnWidth(1, 18);
    m_roomTree->setColumnWidth(2, 18);
    m_roomTree->header()->setStretchLastSection(false);
    m_roomTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_roomTree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_roomTree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    leftL->addWidget(m_roomTree, 1);
    m_leftW->setMinimumWidth(150);
    m_splitter->addWidget(m_leftW);

    // 2. Orta: Sohbet Akışı
    auto* midW = new QWidget(m_splitter);
    auto* midL = new QVBoxLayout(midW);
    midL->setContentsMargins(0,0,0,0);
    midL->setSpacing(0);
    m_roomTitle = new QLabel(midW);
    m_roomTitle->setProperty("class", "headerLabel");
    midL->addWidget(m_roomTitle);

    m_stack = new QStackedWidget(midW);
    midL->addWidget(m_stack, 1);
    m_splitter->addWidget(midW);

    // 3. Sağ: Üye Listesi (Nicklist)
    m_rightW = new QWidget(m_splitter);
    auto* rightL = new QVBoxLayout(m_rightW);
    rightL->setContentsMargins(0,0,0,0);
    rightL->setSpacing(0);
    auto* rightTitle = new QLabel(tr("  Üyeler"), m_rightW);
    rightTitle->setProperty("class", "headerLabel");
    rightL->addWidget(rightTitle);

    m_memberList = new QListWidget(m_rightW);
    auto* memberHint = new QLabel(tr("  Çift tık → özel sohbet"), m_rightW);
    memberHint->setProperty("status", "info");
    rightL->addWidget(m_memberList, 1);
    rightL->addWidget(memberHint);
    m_rightW->setMinimumWidth(150);
    m_splitter->addWidget(m_rightW);

    m_splitter->setSizes({200, 520, 160});
    m_splitter->setStretchFactor(1, 1);

    // ── Yanıp Sönme (Flashing / Pulsing) Timer'ı ───────────────────────────
    m_flashTimer = new QTimer(this);
    m_flashTimer->setInterval(500);
    connect(m_flashTimer, &QTimer::timeout, this, &ChatWindow::onFlashTimerTick);

    // ── Üye Listesi Yenileme (Debounce & Flicker Önleme) Timer'ı ───────────
    m_memberRefreshTimer = new QTimer(this);
    m_memberRefreshTimer->setSingleShot(true);
    m_memberRefreshTimer->setInterval(40);
    connect(m_memberRefreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_activeId.isEmpty()) {
            refreshMemberList(m_activeId);
        }
    });

    // ── Sinyal Bağlantıları ────────────────────────────────────────────────
    connect(m_tabBar, &QTabBar::currentChanged,   this, &ChatWindow::onTabChanged);
    connect(m_tabBar, &QTabBar::tabBarClicked,    this, [this](int idx) {
        if (idx >= 0 && idx < m_tabBar->count()) clearUnread(m_tabBar->tabData(idx).toString());
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &ChatWindow::onTabCloseRequested);

    connect(m_roomTree,   &QTreeWidget::itemSelectionChanged, this, &ChatWindow::onRoomSelected);
    connect(m_roomTree,   &QTreeWidget::itemDoubleClicked,    this, &ChatWindow::onRoomDoubleClicked);
    connect(m_roomTree,   &QTreeWidget::itemClicked,          this, &ChatWindow::onRoomItemClicked);
    connect(m_memberList, &QListWidget::itemDoubleClicked,    this, &ChatWindow::onMemberDoubleClicked);
    m_memberList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_memberList, &QWidget::customContextMenuRequested, this, &ChatWindow::onMemberContextMenuRequested);

    connect(m_rooms, &RoomManager::lockStateChanged,     this, &ChatWindow::onLockStateChanged);
    connect(m_rooms, &RoomManager::roomAdded,           this, &ChatWindow::onRoomAdded);
    connect(m_rooms, &RoomManager::roomRemoved,         this, &ChatWindow::onRoomRemoved);
    connect(m_rooms, &RoomManager::roomRenamed,         this, &ChatWindow::onRoomRenamed);
    connect(m_rooms, &RoomManager::messageReceived,     this, &ChatWindow::onMessageReceived);
    connect(m_rooms, &RoomManager::historyLoaded,       this, &ChatWindow::onHistoryLoaded);
    connect(m_rooms, &RoomManager::memberJoined,        this, &ChatWindow::onMemberJoined);
    connect(m_rooms, &RoomManager::memberLeft,          this, &ChatWindow::onMemberLeft);
    connect(m_rooms, &RoomManager::memberOnlineChanged, this, &ChatWindow::onMemberOnlineChanged);
    connect(m_rooms, &RoomManager::memberWindowChanged, this, &ChatWindow::onMemberWindowChanged);

    connect(m_priv, &PrivateChatManager::sessionOpened,   this, &ChatWindow::onPrivateSessionOpened);
    connect(m_priv, &PrivateChatManager::sessionClosed,   this, &ChatWindow::onPrivateSessionClosed);
    connect(m_priv, &PrivateChatManager::messageReceived, this, &ChatWindow::onPrivateMessageReceived);
    connect(m_priv, &PrivateChatManager::requestReceived, this, &ChatWindow::onPrivateRequestReceived);
    connect(m_priv, &PrivateChatManager::ignoredUsersChanged, this, [this]() {
        if (!m_activeId.isEmpty() && m_memberRefreshTimer) m_memberRefreshTimer->start();
    });

    if (m_session) {
        connect(m_session, &SessionManager::roomJoinRequestReceived, this, &ChatWindow::onRoomJoinRequestReceived);
        connect(m_session, &SessionManager::roomJoinResponseReceived, this, &ChatWindow::onRoomJoinResponseReceived);
        connect(m_session, &SessionManager::handRaiseReceived, this, &ChatWindow::onHandRaiseReceived);
        connect(m_session, &SessionManager::chatMuteStateReceived, this, &ChatWindow::onChatMuteStateReceived);
    }

    buildRoomTree();

    // Başlangıçta Yönetici Odası sekmesini aç
    openRoomTab(RoomManager::kAdminRoomId, true);
}

void ChatWindow::showAndRaise() {
    if (m_blockedByLock) {
        m_wantsShowAfterUnlock = true;
        return;
    }
    show();
    raise();
    activateWindow();
    emit chatWindowOpened();
}

void ChatWindow::setBlockedByLock(bool blocked) {
    m_blockedByLock = blocked;
    if (!m_blockedByLock && m_wantsShowAfterUnlock) {
        m_wantsShowAfterUnlock = false;
        showAndRaise();
    }
}

void ChatWindow::showEvent(QShowEvent* ev) {
    QMainWindow::showEvent(ev);
    emit chatWindowOpened();
    if (!m_activeId.isEmpty()) {
        clearUnread(m_activeId);
        refreshMemberList(m_activeId);
    }
}

void ChatWindow::changeEvent(QEvent* ev) {
    QMainWindow::changeEvent(ev);
    if (ev->type() == QEvent::ActivationChange && isActiveWindow()) {
        emit chatWindowOpened();
        if (!m_activeId.isEmpty()) {
            clearUnread(m_activeId);
        }
    }
}

void ChatWindow::closeEvent(QCloseEvent* ev) {
    hide();
    ev->ignore();
}

// ── Oda Ağacı & Sekmeler ──────────────────────────────────────────────────────

void ChatWindow::onRoomFilterChanged(int index) {
    m_filterOnlyMyRooms = (index == 1);
    buildRoomTree();
}

void ChatWindow::buildRoomTree() {
    m_roomTree->clear();
    auto rooms = m_rooms->allRooms();

    // Map: parentRoomId -> children
    QMap<QString, QList<const Room*>> childrenMap;
    QList<const Room*> roots;

    for (const Room* r : rooms) {
        if (r->parentRoomId.isEmpty()) {
            roots.append(r);
        } else {
            childrenMap[r->parentRoomId].append(r);
        }
    }

    std::function<void(const Room*)> addNode = [&](const Room* r) {
        onRoomAdded(r->id);
        for (const Room* child : childrenMap.value(r->id)) {
            addNode(child);
        }
    };

    for (const Room* r : roots) {
        addNode(r);
    }
}

QTreeWidgetItem* ChatWindow::makeRoomItem(const Room* r) const {
    auto* item = new QTreeWidgetItem();
    item->setText(0, r->name);
    item->setData(0, Qt::UserRole, r->id);

    if (r->type == RoomType::AdminRoom) {
        item->setIcon(0, ThemeManager::icon(":/ui/admin.svg"));
    } else if (r->type == RoomType::GroupRoom) {
        item->setIcon(0, ThemeManager::icon(":/ui/group.svg"));
    } else {
        item->setIcon(0, ThemeManager::icon(":/ui/school.svg"));
    }

    if (r->type != RoomType::AdminRoom) {
        const ChatLockRecord state = m_rooms->chatLockState(r->classNodeId);
        item->setIcon(1, ThemeManager::icon(state.chatLocked   ? ":/ui/lock.svg" : ":/ui/unlock.svg"));
        item->setIcon(2, ThemeManager::icon(state.privateLocked ? ":/ui/lock.svg" : ":/ui/unlock.svg"));
        item->setText(1, "");
        item->setText(2, "");
        item->setTextAlignment(1, Qt::AlignCenter);
        item->setTextAlignment(2, Qt::AlignCenter);
        item->setToolTip(1, state.chatLocked
            ? tr("Sohbet kilitli (tıkla: aç)")
            : tr("Sohbet açık (tıkla: kilitle)"));
        item->setToolTip(2, state.privateLocked
            ? tr("Özel sohbet kilitli (tıkla: aç)")
            : tr("Özel sohbet açık (tıkla: kilitle)"));
    }
    return item;
}

QTreeWidgetItem* ChatWindow::roomItem(const QString& roomId) const {
    QTreeWidgetItemIterator it(m_roomTree);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole).toString() == roomId) return *it;
        ++it;
    }
    return nullptr;
}

void ChatWindow::selectRoom(const QString& roomId) {
    openRoomTab(roomId, true);
    if (auto* item = roomItem(roomId)) {
        m_roomTree->setCurrentItem(item);
        m_roomTree->scrollToItem(item);
    }
}

void ChatWindow::switchToTab(const QString& id) {
    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == id) {
            m_tabBar->setCurrentIndex(i);
            onTabChanged(i);
            break;
        }
    }
}

void ChatWindow::openRoomTab(const QString& roomId, bool activate) {
    const Room* r = m_rooms->room(roomId);
    if (!r) return;

    if (m_tabs.contains(roomId)) {
        if (activate) {
            for (int i = 0; i < m_tabBar->count(); ++i) {
                if (m_tabBar->tabData(i).toString() == roomId) {
                    m_tabBar->setCurrentIndex(i);
                    break;
                }
            }
        }
        return;
    }

    auto* tab = new TabItem;
    tab->id = roomId;
    tab->isPrivate = false;
    tab->title = r->name;
    tab->unread = 0;
    tab->model = new ui::chat::ChatModel(this);
    tab->widget = new ui::chat::ChatWidget(this);
    tab->widget->setModel(tab->model);

    connect(tab->widget, &ui::chat::ChatWidget::messageReadyToSend, this, &ChatWindow::onChatMessageReadyToSend);
    connect(tab->widget, &ui::chat::ChatWidget::messageContextMenuRequested, this, &ChatWindow::onChatContextMenuRequested);

    m_stack->addWidget(tab->widget);
    m_tabs[roomId] = tab;

    QIcon icon = (r->type == RoomType::AdminRoom)
                 ? ThemeManager::icon(":/ui/admin.svg")
                 : (r->type == RoomType::GroupRoom ? ThemeManager::icon(":/ui/group.svg") : ThemeManager::icon(":/ui/school.svg"));

    int idx = m_tabBar->addTab(icon, tab->title);
    m_tabBar->setTabData(idx, roomId);

    // Yönetici odası kapatılamaz
    if (r->permanent) {
        m_tabBar->setTabButton(idx, QTabBar::RightSide, nullptr);
    }

    rebuildMessages(roomId);

    if (activate || m_tabBar->count() == 1 || m_activeId.isEmpty()) {
        m_tabBar->setCurrentIndex(idx);
        onTabChanged(idx);
    }
}

void ChatWindow::openPrivateTab(const QString& sessionId, bool activate) {
    const PrivateSession* s = m_priv->session(sessionId);
    if (!s) return;

    if (m_tabs.contains(sessionId)) {
        if (activate) {
            for (int i = 0; i < m_tabBar->count(); ++i) {
                if (m_tabBar->tabData(i).toString() == sessionId) {
                    m_tabBar->setCurrentIndex(i);
                    break;
                }
            }
        }
        return;
    }

    auto* tab = new TabItem;
    tab->id = sessionId;
    tab->isPrivate = true;
    tab->title = tr("[Özel] %1").arg(s->peerDisplay);
    tab->unread = 0;
    tab->model = new ui::chat::ChatModel(this);
    tab->widget = new ui::chat::ChatWidget(this);
    tab->widget->setModel(tab->model);

    connect(tab->widget, &ui::chat::ChatWidget::messageReadyToSend, this, &ChatWindow::onChatMessageReadyToSend);
    connect(tab->widget, &ui::chat::ChatWidget::messageContextMenuRequested, this, &ChatWindow::onChatContextMenuRequested);

    m_stack->addWidget(tab->widget);
    m_tabs[sessionId] = tab;

    // Sistem mesajı ekle
    ui::chat::ChatMessage sysMsg;
    sysMsg.isSystem = true;
    sysMsg.content = tr("%1 ile özel sohbet başladı").arg(s->peerDisplay);
    tab->model->addMessage(sysMsg);

    int idx = m_tabBar->addTab(ThemeManager::icon(":/ui/lock.svg"), tab->title);
    m_tabBar->setTabData(idx, sessionId);

    if (activate || m_tabBar->count() == 1 || m_activeId.isEmpty()) {
        m_tabBar->setCurrentIndex(idx);
        onTabChanged(idx);
    }
}

void ChatWindow::closeTabById(const QString& id) {
    auto* tab = m_tabs.take(id);
    if (!tab) return;

    for (int i = 0; i < m_tabBar->count(); ++i) {
        if (m_tabBar->tabData(i).toString() == id) {
            m_tabBar->removeTab(i);
            break;
        }
    }

    m_stack->removeWidget(tab->widget);
    delete tab->widget;
    delete tab->model;
    if (tab->isPrivate) {
        m_priv->closeSession(id);
    } else {
        // Misafir oda sekmesi kapatıldığında odadan ayrıl
        if (!m_rooms->isPermanentMember(id)) {
            m_rooms->leaveRoom(id);
        }
    }

    delete tab;

    if (m_activeId == id) {
        m_activeId.clear();
        if (m_tabBar->count() > 0) {
            m_tabBar->setCurrentIndex(0);
        }
    }
}

void ChatWindow::onTabChanged(int index) {
    if (index < 0 || index >= m_tabBar->count()) {
        m_activeId.clear();
        m_roomTitle->clear();
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
        const PrivateSession* s = m_priv->session(id);
        m_roomTitle->setText(s ? tr("%1 · Özel Sohbet").arg(s->peerDisplay) : tr("Özel Sohbet"));
        m_memberList->clear();
        m_rightW->setVisible(false);
        m_roomTree->blockSignals(true);
        m_roomTree->clearSelection();
        m_roomTree->setCurrentItem(nullptr);
        m_roomTree->blockSignals(false);
        setInputEnabled(true);
    } else {
        const Room* r = m_rooms->room(id);
        m_roomTitle->setText(r ? r->name : id);
        m_rightW->setVisible(true);
        refreshMemberList(id);

        if (r && !r->historyLoaded) {
            m_rooms->loadHistory(id);
        }

        if (auto* item = roomItem(id)) {
            m_roomTree->blockSignals(true);
            m_roomTree->setCurrentItem(item);
            m_roomTree->scrollToItem(item);
            m_roomTree->blockSignals(false);
        }

        // YM ve OM için giriş kutusu kilit durumundan bağımsız olarak her zaman açık kalır
        setInputEnabled(true);
    }
}

void ChatWindow::onTabCloseRequested(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    const QString id = m_tabBar->tabData(index).toString();
    const Room* r = m_rooms->room(id);
    if (r && r->permanent) return; // Yönetici Odası kapanamaz
    closeTabById(id);
}

void ChatWindow::onFlashTimerTick() {
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

void ChatWindow::notifyUnread(const QString& id) {
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

    if (!isVisible() || !isActiveWindow()) {
        emit unreadMessageArrived();
    }
}

void ChatWindow::clearUnread(const QString& id) {
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

void ChatWindow::updateTabDisplay(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= m_tabBar->count()) return;
    const QString id = m_tabBar->tabData(tabIndex).toString();
    auto* tab = m_tabs.value(id);
    if (!tab) return;

    if (tab->unread > 0) {
        m_tabBar->setTabText(tabIndex, QString("(%1) %2").arg(tab->unread).arg(tab->title));
        m_tabBar->setTabIcon(tabIndex, ThemeManager::icon(":/ui/chat.svg"));
        QFont f = m_tabBar->font();
        f.setBold(true);
        m_tabBar->setFont(f);
    } else {
        m_tabBar->setTabText(tabIndex, tab->title);
        m_tabBar->setTabIcon(tabIndex, QIcon());
        auto mode = ThemeManager::instance().currentMode();
        QColor normalColor = (mode == ThemeManager::ThemeMode::Dark) ? QColor("#e2e8f0") : QColor("#1e293b");
        m_tabBar->setTabTextColor(tabIndex, normalColor);
    }
}

// ── Sol Panel Etkileşimleri ───────────────────────────────────────────────────

void ChatWindow::onRoomSelected() {
    if (m_roomTree->selectedItems().isEmpty()) return;
    auto* item = m_roomTree->currentItem();
    if (!item) return;
    const QString roomId = item->data(0, Qt::UserRole).toString();
    if (roomId.isEmpty()) return;

    // Eğer sekme zaten açıksa sekmesine geçiş yap
    if (m_tabs.contains(roomId)) {
        for (int i = 0; i < m_tabBar->count(); ++i) {
            if (m_tabBar->tabData(i).toString() == roomId) {
                if (m_tabBar->currentIndex() != i) {
                    m_tabBar->setCurrentIndex(i);
                }
                break;
            }
        }
    }
}

void ChatWindow::onRoomDoubleClicked(QTreeWidgetItem* item, int col) {
    if (!item || col != 0) return;
    const QString roomId = item->data(0, Qt::UserRole).toString();
    if (roomId.isEmpty()) return;

    const Room* r = m_rooms->room(roomId);
    if (!r) return;

    if (r->type == RoomType::AdminRoom) {
        openRoomTab(roomId, true);
        return;
    }

    // Eğer sekme zaten açıksa odaklan
    if (m_tabs.contains(roomId)) {
        openRoomTab(roomId, true);
        return;
    }

    // Eğer YM isek veya bu sınıfın daimi sahibiysek doğrudan gir
    if (m_rooms->masterRole() == "YM" || m_rooms->isPermanentMember(roomId)) {
        m_rooms->joinRoom(roomId);
        openRoomTab(roomId, true);
        return;
    }

    // Aksi takdirde OM için izin talebi gönder
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Bağlantı Yok"),
            tr("Sohbet odasına katılım isteği göndermek için sunucuya bağlı olmalısınız."));
        return;
    }

    const QString reqId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingJoinRequests[reqId] = roomId;
    m_session->sendRoomJoinRequest(reqId, roomId, r->name, m_rooms->masterUsername(), m_rooms->masterName());

    QMessageBox::information(this, tr("Katılım İsteği Gönderildi"),
        tr("'%1' sınıfının sohbet odasına katılma talebiniz sınıf öğretmenine ve yöneticilere iletildi.\nOnaylandığında oda sekmesi otomatik olarak açılacaktır.").arg(r->name));
}

void ChatWindow::onRoomItemClicked(QTreeWidgetItem* item, int col) {
    if (!item || col < 1 || col > 2) return;
    const QString roomId = item->data(0, Qt::UserRole).toString();
    if (roomId.isEmpty()) return;

    const Room* r = m_rooms->room(roomId);
    if (!r || r->type != RoomType::ClassRoom) return;
    if (!m_rooms->canManageClass(r->classNodeId)) return;

    ChatLockRecord state = m_rooms->chatLockState(r->classNodeId);

    if (col == 1) {
        const bool newChat = !state.chatLocked;
        m_rooms->setChatLock(r->classNodeId, newChat, newChat ? true : state.privateLocked);
    } else {
        if (state.chatLocked) return;
        m_rooms->setChatLock(r->classNodeId, state.chatLocked, !state.privateLocked);
    }
}

void ChatWindow::onLockStateChanged(const QString& classNodeId,
                                    bool chatLocked, bool privateLocked) {
    const QString roomId = m_rooms->roomIdForNode(classNodeId);
    updateRoomLockEmojis(roomId, chatLocked, privateLocked);

    // YM ve OM için giriş kutusu kilit durumundan etkilenmez
    setInputEnabled(true);
}

void ChatWindow::updateRoomLockEmojis(const QString& roomId,
                                      bool chatLocked, bool privateLocked) {
    if (auto* item = roomItem(roomId)) {
        item->setIcon(1, ThemeManager::icon(chatLocked   ? ":/ui/lock.svg" : ":/ui/unlock.svg"));
        item->setIcon(2, ThemeManager::icon(privateLocked ? ":/ui/lock.svg" : ":/ui/unlock.svg"));
        item->setText(1, "");
        item->setText(2, "");
        item->setTextAlignment(1, Qt::AlignCenter);
        item->setTextAlignment(2, Qt::AlignCenter);
        item->setToolTip(1, chatLocked
            ? tr("Sohbet kilitli (tıkla: aç)")
            : tr("Sohbet açık (tıkla: kilitle)"));
        item->setToolTip(2, privateLocked
            ? tr("Özel sohbet kilitli (tıkla: aç)")
            : tr("Özel sohbet açık (tıkla: kilitle)"));
    }
}

void ChatWindow::onRoomAdded(const QString& roomId) {
    const Room* r = m_rooms->room(roomId);
    if (!r) return;

    if (m_filterOnlyMyRooms && r->type != RoomType::AdminRoom) {
        if (!m_rooms->isPermanentMember(r->id)) return;
    }

    if (roomItem(roomId)) return;

    auto* item = makeRoomItem(r);
    if (!r->parentRoomId.isEmpty()) {
        if (auto* par = roomItem(r->parentRoomId)) {
            par->addChild(item);
            par->setExpanded(true);
        } else {
            m_roomTree->addTopLevelItem(item);
        }
    } else {
        if (r->type == RoomType::AdminRoom) {
            m_roomTree->insertTopLevelItem(0, item);
        } else {
            m_roomTree->addTopLevelItem(item);
        }
    }

    // Eğer bu oda bir üst sınıf ise ve önceden eklenmiş çocukları varsa, hiyerarşiyi toparla
    for (const Room* childRoom : m_rooms->allRooms()) {
        if (childRoom->parentRoomId == roomId) {
            if (auto* childItem = roomItem(childRoom->id)) {
                if (childItem->parent() != item) {
                    if (childItem->parent()) childItem->parent()->removeChild(childItem);
                    else m_roomTree->takeTopLevelItem(m_roomTree->indexOfTopLevelItem(childItem));
                    item->addChild(childItem);
                    item->setExpanded(true);
                }
            }
        }
    }
}

void ChatWindow::onRoomRemoved(const QString& roomId) {
    if (auto* item = roomItem(roomId)) {
        closeTabById(roomId);
        delete item;
    }
}

void ChatWindow::onRoomRenamed(const QString& roomId, const QString& name) {
    if (auto* item = roomItem(roomId)) {
        item->setText(0, name);
    }
    if (m_tabs.contains(roomId)) {
        m_tabs[roomId]->title = name;
        for (int i = 0; i < m_tabBar->count(); ++i) {
            if (m_tabBar->tabData(i).toString() == roomId) {
                updateTabDisplay(i);
                break;
            }
        }
    }
}

// ── Mesajlaşma ────────────────────────────────────────────────────────────────

void ChatWindow::appendMessage(const QString& roomId, const RoomMessage& msg) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;

    ui::chat::ChatMessage m;
    m.isMine = (msg.fromDisplay == m_rooms->masterName() || msg.fromHost == QHostInfo::localHostName());
    m.isSystem = msg.isSystem;
    m.content = msg.content;
    m.timestamp = msg.timestamp;
    m.senderName = msg.fromDisplay;
    m.senderRole = static_cast<okidia::SenderRole>(msg.senderRole);

    tab->model->addMessage(m);

    if (!msg.isSystem && !m.isMine) {
        notifyUnread(roomId);
    }
}

void ChatWindow::rebuildMessages(const QString& roomId) {
    auto* tab = m_tabs.value(roomId);
    if (!tab) return;
    const Room* r = m_rooms->room(roomId);
    if (!r) return;

    tab->model->clearMessages();
    for (const RoomMessage& msg : r->messages) {
        ui::chat::ChatMessage m;
        m.isMine = (msg.fromDisplay == m_rooms->masterName() || msg.fromHost == QHostInfo::localHostName());
        m.isSystem = msg.isSystem;
        m.content = msg.content;
        m.timestamp = msg.timestamp;
        m.senderName = msg.fromDisplay;
        m.senderRole = static_cast<okidia::SenderRole>(msg.senderRole);
        tab->model->addMessage(m);
    }
}

void ChatWindow::onMessageReceived(const QString& roomId, const RoomMessage& msg) {
    appendMessage(roomId, msg);
}

void ChatWindow::onHistoryLoaded(const QString& roomId, const QList<RoomMessage>& msgs) {
    Q_UNUSED(msgs);
    rebuildMessages(roomId);
}

void ChatWindow::onChatMessageReadyToSend(const QString& text) {
    QString targetId = m_activeId;
    if (targetId.isEmpty() && m_tabBar->currentIndex() >= 0) {
        targetId = m_tabBar->tabData(m_tabBar->currentIndex()).toString();
        m_activeId = targetId;
    }
    if (targetId.isEmpty()) return;

    auto* tab = m_tabs.value(targetId);
    if (!tab) return;

    if (tab->isPrivate) {
        m_priv->sendMessage(targetId, text);
    } else {
        m_rooms->sendMessage(targetId, text);
    }
}

void ChatWindow::onChatContextMenuRequested(const QString& msgId, const QPoint& pos) {
    Q_UNUSED(msgId);
    Q_UNUSED(pos);
}

void ChatWindow::setInputEnabled(bool enabled) {
    auto* tab = m_tabs.value(m_activeId);
    if (tab && tab->widget) {
        tab->widget->setLocked(!enabled, tr("Sohbet kilitli"));
    }
}

// ── Sağ Panel Üye Listesi (Rol Hiyerarşisi & Sıralama) ─────────────────────────

// ── Sağ Panel Üye Listesi (Rol Hiyerarşisi, Sıralama & Titreme Önleme) ───────

void ChatWindow::refreshMemberList(const QString& roomId) {
    const Room* r = m_rooms->room(roomId);
    if (!r) {
        m_memberList->clear();
        return;
    }

    auto mode = ThemeManager::instance().currentMode();
    bool isDark = (mode == ThemeManager::ThemeMode::Dark);

    // 1. Üyeleri Grupla: YM (1), Gözlemci (4), OM (2), Öğrenciler (3)
    QList<RoomMember> managers;   // YM
    QList<RoomMember> observers;  // OBSERVER
    QList<RoomMember> teachers;   // OM
    QList<RoomMember> students;   // Öğrenciler

    for (const RoomMember& m : r->members) {
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

    // Öğrencilerin formatlı isimlerini hazırla ("Display Name - hostname")
    struct StudentEntry {
        RoomMember member;
        QString display;
    };
    QList<StudentEntry> sortedStudents;
    for (const RoomMember& m : students) {
        QString sName;
        if (m_session) {
            const ClientState cs = m_session->clientState(m.clientId);
            sName = RoomManager::formatStudentDisplayName(cs.userDisplayName, cs.username, cs.hostname);
        }
        if (sName.isEmpty()) {
            sName = m.displayName;
        }
        sortedStudents.append({m, sName});
    }

    // Öğrencileri alfabetik A-Z sırala
    std::sort(sortedStudents.begin(), sortedStudents.end(), [](const StudentEntry& a, const StudentEntry& b) {
        return QString::localeAwareCompare(a.display, b.display) < 0;
    });

    struct ItemData {
        QString clientId;
        QString displayName;
        QString labelText;
        QString iconPath;
        QColor  fgColor;
        bool    isBold = false;
        QString tooltip;
        bool    isSeparator = false;
    };
    QList<ItemData> targetItems;

    // 1. Yönetici Master (YM)
    for (const RoomMember& m : managers) {
        QString cleanName = m.displayName;
        if (!cleanName.contains(tr("Yönetici"))) {
            cleanName = tr("[Yönetici] ") + cleanName;
        }
        ItemData d;
        d.clientId = m.clientId;
        d.displayName = m.displayName;
        d.labelText = cleanName;
        d.iconPath = m.online ? ":/ui/online.svg" : ":/ui/offline.svg";
        d.fgColor = m.online ? (isDark ? QColor("#fbbf24") : QColor("#b45309")) : (isDark ? QColor("#64748b") : QColor("#94a3b8"));
        d.isBold = true;
        d.tooltip = m.online ? tr("Yönetici · Aktif") : tr("Yönetici · Çevrimdışı");
        targetItems.append(d);
    }

    // 2. Gözlemciler (OBSERVER)
    for (const RoomMember& m : observers) {
        QString cleanName = m.displayName;
        if (!cleanName.contains(tr("Gözlemci"))) {
            cleanName = tr("[Gözlemci] ") + cleanName;
        }
        ItemData d;
        d.clientId = m.clientId;
        d.displayName = m.displayName;
        d.labelText = cleanName;
        d.iconPath = m.online ? ":/ui/online.svg" : ":/ui/offline.svg";
        d.fgColor = m.online ? (isDark ? QColor("#c084fc") : QColor("#7e22ce")) : (isDark ? QColor("#64748b") : QColor("#94a3b8"));
        d.isBold = true;
        d.tooltip = m.online ? tr("Gözlemci · Aktif") : tr("Gözlemci · Çevrimdışı");
        targetItems.append(d);
    }

    // 3. Öğretmen Master (OM)
    for (const RoomMember& m : teachers) {
        QString cleanName = m.displayName;
        if (!cleanName.contains(tr("Öğretmen"))) {
            cleanName = tr("[Öğretmen] ") + cleanName;
        }
        ItemData d;
        d.clientId = m.clientId;
        d.displayName = m.displayName;
        d.labelText = cleanName;
        d.iconPath = m.online ? ":/ui/online.svg" : ":/ui/offline.svg";
        d.fgColor = m.online ? (isDark ? QColor("#60a5fa") : QColor("#0369a1")) : (isDark ? QColor("#64748b") : QColor("#94a3b8"));
        d.isBold = true;
        d.tooltip = m.online ? tr("Öğretmen · Aktif") : tr("Öğretmen · Çevrimdışı");
        targetItems.append(d);
    }

    // Ayırıcı çizgi
    if ((!managers.isEmpty() || !observers.isEmpty() || !teachers.isEmpty()) && !sortedStudents.isEmpty()) {
        ItemData sep;
        sep.isSeparator = true;
        sep.labelText = tr("───── Üyeler ─────");
        sep.fgColor = isDark ? QColor("#64748b") : QColor("#94a3b8");
        targetItems.append(sep);
    }

    // 4. Öğrenciler (Agent - AC) (A-Z)
    const qint64 nowSecs = OkidiaUtils::nowSecs();
    for (const auto& entry : sortedStudents) {
        const RoomMember& m = entry.member;
        const QString studentUser = m_session ? m_session->clientState(m.clientId).username : QString();
        const QString userKey = PrivateChatManager::makeUserKey(m.clientId, studentUser);
        const bool isMuted = (m_mutedUsers.value(userKey, 0) > nowSecs);
        const bool isIgnored = (m_priv && m_priv->isUserIgnored(m.clientId, studentUser));

        QStringList tags;
        if (isMuted) tags << tr("Susturuldu");
        if (isIgnored) tags << tr("Yoksayıldı");

        ItemData d;
        d.clientId = m.clientId;
        d.displayName = entry.display;
        d.labelText = tags.isEmpty() ? entry.display : QString("%1 [%2]").arg(entry.display, tags.join(", "));
        if (m.online && m.windowOpen) {
            d.iconPath = ":/ui/online.svg";
            d.tooltip = tr("Aktif · Sohbet Açık");
            d.fgColor = isDark ? QColor("#f8fafc") : QColor("#0f172a");
        } else {
            d.iconPath = ":/ui/offline.svg";
            d.tooltip = m.online ? tr("Sohbet Kapalı (Özel sohbet başlatılabilir)") : tr("Çevrimdışı");
            d.fgColor = isDark ? QColor("#64748b") : QColor("#94a3b8");
        }
        if (isMuted) {
            const qint64 remMins = qMax<qint64>(1, (m_mutedUsers.value(userKey) - nowSecs + 59) / 60);
            d.tooltip += tr(" · Sohbette susturuldu (~%1 dk kaldı)").arg(remMins);
        }
        if (isIgnored) {
            d.tooltip += tr(" · İstekleri yoksayılıyor");
        }
        targetItems.append(d);
    }

    // ── In-place Diffing (Flicker Önleme) ──────────────────────────────────
    bool canUpdateInPlace = (m_memberList->count() == targetItems.size());
    if (canUpdateInPlace) {
        for (int i = 0; i < targetItems.size(); ++i) {
            QListWidgetItem* item = m_memberList->item(i);
            const auto& d = targetItems[i];
            if (d.isSeparator) {
                if (item->flags() != Qt::NoItemFlags) {
                    canUpdateInPlace = false;
                    break;
                }
            } else {
                if (item->data(Qt::UserRole).toString() != d.clientId) {
                    canUpdateInPlace = false;
                    break;
                }
            }
        }
    }

    if (canUpdateInPlace) {
        // Yapı ve sıra aynı: Sadece değişen görsel alanları güncelle (Flicker = 0)
        for (int i = 0; i < targetItems.size(); ++i) {
            QListWidgetItem* item = m_memberList->item(i);
            const auto& d = targetItems[i];
            if (d.isSeparator) {
                if (item->text() != d.labelText) item->setText(d.labelText);
                if (item->foreground().color() != d.fgColor) item->setForeground(d.fgColor);
                continue;
            }
            if (item->text() != d.labelText) item->setText(d.labelText);
            if (item->data(Qt::UserRole + 2).toString() != d.iconPath) {
                item->setIcon(ThemeManager::icon(d.iconPath));
                item->setData(Qt::UserRole + 2, d.iconPath);
            }
            if (item->toolTip() != d.tooltip) item->setToolTip(d.tooltip);
            if (item->foreground().color() != d.fgColor) item->setForeground(d.fgColor);
            QFont f = item->font();
            if (f.bold() != d.isBold) {
                f.setBold(d.isBold);
                item->setFont(f);
            }
            item->setData(Qt::UserRole + 1, d.displayName);
        }
        return;
    }

    // Yapı veya sıra değiştiğinde: setUpdatesEnabled(false) ile pürüzsüz yeniden kur
    QString selectedClientId;
    if (auto* cur = m_memberList->currentItem()) {
        selectedClientId = cur->data(Qt::UserRole).toString();
    }

    m_memberList->setUpdatesEnabled(false);
    m_memberList->clear();

    QListWidgetItem* itemToSelect = nullptr;
    for (const auto& d : targetItems) {
        auto* item = new QListWidgetItem();
        if (d.isSeparator) {
            item->setText(d.labelText);
            item->setFlags(Qt::NoItemFlags);
            item->setTextAlignment(Qt::AlignCenter);
            item->setForeground(d.fgColor);
        } else {
            item->setText(d.labelText);
            item->setIcon(ThemeManager::icon(d.iconPath));
            item->setData(Qt::UserRole + 2, d.iconPath);
            item->setForeground(d.fgColor);
            if (d.isBold) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
            }
            item->setToolTip(d.tooltip);
            item->setData(Qt::UserRole,     d.clientId);
            item->setData(Qt::UserRole + 1, d.displayName);
            if (!selectedClientId.isEmpty() && d.clientId == selectedClientId) {
                itemToSelect = item;
            }
        }
        m_memberList->addItem(item);
    }

    if (itemToSelect) {
        m_memberList->setCurrentItem(itemToSelect);
    }
    m_memberList->setUpdatesEnabled(true);
}

void ChatWindow::onMemberJoined(const QString& rId, const RoomMember&) {
    if (m_activeId == rId && m_memberRefreshTimer) m_memberRefreshTimer->start();
}

void ChatWindow::onMemberLeft(const QString& rId, const QString&) {
    if (m_activeId == rId && m_memberRefreshTimer) m_memberRefreshTimer->start();
}

void ChatWindow::onMemberOnlineChanged(const QString& rId, const QString&, bool) {
    if (m_activeId == rId && m_memberRefreshTimer) m_memberRefreshTimer->start();
}

void ChatWindow::onMemberWindowChanged(const QString& rId, const QString&, bool) {
    if (m_activeId == rId && m_memberRefreshTimer) m_memberRefreshTimer->start();
}

void ChatWindow::onMemberDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString clientId    = item->data(Qt::UserRole).toString();
    const QString displayName = item->data(Qt::UserRole + 1).toString();
    if (clientId.isEmpty()) return;
    if (m_rooms && clientId == ("master:" + m_rooms->masterUsername())) return;

    m_priv->openDirect(clientId, displayName, {}, {});
}

void ChatWindow::onMemberContextMenuRequested(const QPoint& pos) {
    QListWidgetItem* item = m_memberList->itemAt(pos);
    if (!item) return;

    const QString clientId    = item->data(Qt::UserRole).toString();
    const QString displayName = item->data(Qt::UserRole + 1).toString();
    if (clientId.isEmpty()) return;

    const bool isSelf = (m_rooms && clientId == ("master:" + m_rooms->masterUsername()));
    const bool isStudent = !clientId.startsWith("master:");
    const QString studentUser = (isStudent && m_session) ? m_session->clientState(clientId).username : QString();
    const QString userKey = PrivateChatManager::makeUserKey(clientId, studentUser);
    const bool isMuted = isStudent && (m_mutedUsers.value(userKey, 0) > OkidiaUtils::nowSecs());
    const bool isIgnored = isStudent && m_priv && m_priv->isUserIgnored(clientId, studentUser);

    QMenu menu(this);
    
    QAction* actionPrivateChat = menu.addAction(ThemeManager::icon(":/ui/chat.svg"), tr("Özel Sohbet Başlat"));
    if (isSelf) {
        actionPrivateChat->setEnabled(false);
    }

    QAction* actionMute5 = nullptr;
    QAction* actionMute10 = nullptr;
    QAction* actionUnmute = nullptr;
    QAction* actionToggleIgnore = nullptr;

    if (isStudent) {
        menu.addSeparator();
        if (isMuted) {
            actionUnmute = menu.addAction(ThemeManager::icon(":/ui/unlock.svg"), tr("Sohbet Susturmasını Kaldır"));
        } else {
            actionMute5 = menu.addAction(ThemeManager::icon(":/ui/lock.svg"), tr("Sohbette Sustur (5 dk)"));
            actionMute10 = menu.addAction(ThemeManager::icon(":/ui/lock.svg"), tr("Sohbette Sustur (10 dk)"));
        }
        actionToggleIgnore = menu.addAction(
            ThemeManager::icon(isIgnored ? ":/ui/unlock.svg" : ":/ui/lock.svg"),
            isIgnored ? tr("İstek Engelini Kaldır (Yoksaymayı Kaldır)") : tr("Özel Sohbet / Yardım İsteklerini Yoksay"));
    }

    menu.addSeparator();
    
    QAction* actionVoiceCall = menu.addAction(tr("Sesli Arama (VoIP) [Yakında]"));
    actionVoiceCall->setEnabled(false);
    
    QAction* actionVideoCall = menu.addAction(tr("Görüntülü Arama [Yakında]"));
    actionVideoCall->setEnabled(false);

    QAction* selected = menu.exec(m_memberList->viewport()->mapToGlobal(pos));
    if (selected == actionPrivateChat && !isSelf) {
        m_priv->openDirect(clientId, displayName, {}, {});
    } else if (actionUnmute && selected == actionUnmute && m_session) {
        m_session->sendChatMuteState(clientId, studentUser, false, 0, QString());
    } else if (actionMute5 && selected == actionMute5 && m_session) {
        m_session->sendChatMuteState(clientId, studentUser, true, 300,
            tr("Öğretmen tarafından 5 dakika sohbet kısıtlaması uygulandı."));
    } else if (actionMute10 && selected == actionMute10 && m_session) {
        m_session->sendChatMuteState(clientId, studentUser, true, 600,
            tr("Öğretmen tarafından 10 dakika sohbet kısıtlaması uygulandı."));
    } else if (actionToggleIgnore && selected == actionToggleIgnore && m_priv) {
        if (isIgnored) {
            m_priv->unignoreUser(clientId, studentUser);
        } else {
            m_priv->ignoreUser(clientId, studentUser);
        }
    }
}

// ── Özel Sohbet Olayları ──────────────────────────────────────────────────────

void ChatWindow::onPrivateSessionOpened(const QString& sessionId) {
    openPrivateTab(sessionId, true);
}

void ChatWindow::onPrivateSessionClosed(const QString& sessionId) {
    closeTabById(sessionId);
}

void ChatWindow::onPrivateMessageReceived(const QString& sessionId, const PrivateMessage& msg) {
    auto* tab = m_tabs.value(sessionId);
    if (!tab) {
        openPrivateTab(sessionId, false);
        tab = m_tabs.value(sessionId);
    }
    if (!tab) return;

    ui::chat::ChatMessage m;
    m.isMine = msg.fromMe;
    m.isSystem = false;
    m.content = msg.content;
    m.timestamp = msg.timestamp;

    const PrivateSession* s = m_priv->session(sessionId);
    m.senderName = msg.fromMe ? m_rooms->masterName() : (s ? s->peerDisplay : "");
    m.senderRole = msg.fromMe ? okidia::ROLE_ADMIN : okidia::ROLE_CLIENT;

    tab->model->addMessage(m);

    notifyUnread(sessionId);
}

void ChatWindow::onPrivateRequestReceived(const PendingRequest& req) {
    showAndRaise();
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Özel Sohbet İsteği"));
    msgBox.setText(tr("<b>%1</b> sizinle özel sohbet başlatmak istiyor.").arg(req.fromDisplay.toHtmlEscaped()));
    msgBox.setIcon(QMessageBox::Question);
    QPushButton* btnAccept = msgBox.addButton(tr("Kabul Et"), QMessageBox::AcceptRole);
    QPushButton* btnReject = msgBox.addButton(tr("Reddet"), QMessageBox::RejectRole);
    QPushButton* btnIgnore = msgBox.addButton(tr("Yoksay (Engelle)"), QMessageBox::DestructiveRole);
    msgBox.setDefaultButton(btnAccept);
    msgBox.exec();

    if (msgBox.clickedButton() == btnAccept) {
        m_priv->acceptRequest(req.requestId);
    } else if (msgBox.clickedButton() == btnIgnore) {
        m_priv->ignoreRequest(req.requestId);
    } else {
        m_priv->rejectRequest(req.requestId);
    }
}

void ChatWindow::onHandRaiseReceived(const QString& clientId, const QString& message) {
    // Pencere görünür değilse diyalogu MainWindowChat gösterir (çift modal açılmasını önle)
    if (!isVisible()) return;

    const ClientState s = m_session ? m_session->clientState(clientId) : ClientState{};
    if (m_priv && m_priv->isUserIgnored(clientId, s.username)) {
        return;
    }

    const QString name = RoomManager::formatStudentDisplayName(s.userDisplayName, s.username, s.hostname, clientId);

    // Zaten açık bir özel sohbet varsa tekrar onay sorma; notu doğrudan oturuma ekle
    if (m_priv) {
        const QString existingSessionId = m_priv->sessionIdForPeer(clientId);
        if (!existingSessionId.isEmpty()) {
            const QString noteText = message.isEmpty()
                ? tr("Öğrenci tekrar yardım talebinde bulundu.")
                : tr("Öğrencinin Yardım Notu: \"%1\"").arg(message);
            m_priv->appendSystemMessage(existingSessionId, noteText);
            switchToTab(existingSessionId);
            showAndRaise();
            return;
        }
    }

    showAndRaise();
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Öğrenci Yardım Talebi"));
    QString promptText = tr("<b>%1</b> yardım talebinde bulundu!").arg(name.toHtmlEscaped());
    if (!message.isEmpty()) {
        promptText += QString("<br><i>%1: \"%2\"</i>").arg(tr("Mesaj"), message.toHtmlEscaped());
    }
    promptText += tr("<br><br>Bu öğrenci ile özel sohbet başlatmak ister misiniz?");
    msgBox.setText(promptText);
    msgBox.setIcon(QMessageBox::Information);
    QPushButton* btnChat = msgBox.addButton(tr("Özel Sohbet Başlat"), QMessageBox::AcceptRole);
    QPushButton* btnIgnore = msgBox.addButton(tr("Yoksay (Engelle)"), QMessageBox::DestructiveRole);
    msgBox.addButton(tr("Kapat"), QMessageBox::RejectRole);
    msgBox.setDefaultButton(btnChat);
    msgBox.exec();

    if (msgBox.clickedButton() == btnChat) {
        if (m_priv) {
            m_priv->openDirect(clientId, name, s.hostname, s.username);
            const QString sessionId = m_priv->sessionIdForPeer(clientId);
            if (!sessionId.isEmpty() && !message.isEmpty()) {
                m_priv->appendSystemMessage(sessionId, tr("Öğrencinin İlettiği Not: \"%1\"").arg(message));
            }
        }
    } else if (msgBox.clickedButton() == btnIgnore) {
        if (m_priv) {
            m_priv->ignoreUser(clientId, s.username);
        }
    }
}

void ChatWindow::onChatMuteStateReceived(const QString& clientId, const QString& username,
                                         bool muted, qint64 mutedUntil, int /*violationLevel*/, const QString& /*reason*/) {
    if (clientId.isEmpty()) return;
    QString effUser = username;
    if (effUser.isEmpty() && m_session) {
        effUser = m_session->clientState(clientId).username;
    }
    const QString userKey = PrivateChatManager::makeUserKey(clientId, effUser);
    if (muted && mutedUntil > OkidiaUtils::nowSecs()) {
        m_mutedUsers[userKey] = mutedUntil;
    } else {
        m_mutedUsers.remove(userKey);
    }
    if (!m_activeId.isEmpty() && m_memberRefreshTimer) {
        m_memberRefreshTimer->start();
    }
}

void ChatWindow::onRoomJoinRequestReceived(const QString& requestId,
                                           const QString& roomId,
                                           const QString& roomName,
                                           const QString& fromUsername,
                                           const QString& fromDisplay,
                                           const QString& targetUsername) {
    Q_UNUSED(targetUsername);

    // Kendimiz gönderdiysek yoksay
    if (fromUsername == m_rooms->masterUsername()) return;
    // Sadece bu odanın yetkili öğretmeni (veya YM) isek dialog göster
    if (!m_rooms->isPermanentMember(roomId) && m_rooms->masterRole() != "YM") return;

    RoomJoinRequestDialog dlg(requestId, roomId, roomName, fromUsername, fromDisplay, this);
    if (dlg.exec() == QDialog::Accepted) {
        if (m_session) {
            m_session->sendRoomJoinResponse(requestId, roomId, fromUsername, m_rooms->masterUsername(), true);
        }
    } else {
        if (m_session) {
            m_session->sendRoomJoinResponse(requestId, roomId, fromUsername, m_rooms->masterUsername(), false, tr("Talep reddedildi veya zaman aşımına uğradı."));
        }
    }
}

void ChatWindow::onRoomJoinResponseReceived(const QString& requestId,
                                            const QString& roomId,
                                            const QString& fromUsername,
                                            const QString& targetUsername,
                                            bool accepted,
                                            const QString& reason) {
    Q_UNUSED(fromUsername);
    Q_UNUSED(targetUsername);

    if (!m_pendingJoinRequests.contains(requestId)) return;
    m_pendingJoinRequests.remove(requestId);

    const Room* r = m_rooms->room(roomId);
    const QString rName = r ? r->name : roomId;

    if (accepted) {
        openRoomTab(roomId, true);
        m_rooms->joinRoom(roomId);
    } else {
        QString msg = reason.isEmpty() ? tr("Katılım talebi reddedildi.") : reason;
        QMessageBox::warning(this, tr("Katılım Reddedildi"),
            tr("'%1' odasına katılım talebiniz reddedildi:\n%2").arg(rName, msg));
    }
}
