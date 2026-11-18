// master/ui/MainWindow/MainWindowLayout.cpp
#include "MainWindow.h"
#include "../../../common/ui/ThemeManager.h"
#include "../Widgets/MasterLockOverlay.h"
#include "ui_MainWindow.h"
#include "../../common/utils/Logger.h"
#include <QTimer>
void MainWindow::showEmptyStateMessage(const QString& msg) {
    if (!m_emptyStateLabel) {
        m_emptyStateLabel = new QLabel(ui->thumbnailContainer);
        m_emptyStateLabel->setAlignment(Qt::AlignCenter);
        m_emptyStateLabel->setWordWrap(true);
        m_emptyStateLabel->setProperty("status", "info");
        m_emptyStateLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_emptyStateLabel->setStyleSheet(
            "font-size: 15px; "
            "color: palette(placeholder-text); "
            "padding: 16px 32px; "
            "background: transparent;"
        );
    }
    m_emptyStateLabel->setText(msg);
    if (ui->groupsLayout->indexOf(m_emptyStateLabel) < 0) {
        ui->groupsLayout->addStretch(1);
        ui->groupsLayout->addWidget(m_emptyStateLabel);
        ui->groupsLayout->addStretch(1);
    }
    m_emptyStateLabel->show();
}

void MainWindow::hideEmptyStateMessage() {
    if (m_emptyStateLabel) {
        ui->groupsLayout->removeWidget(m_emptyStateLabel);
        m_emptyStateLabel->hide();
    }
}

void MainWindow::clearGroupLayout() {
    hideEmptyStateMessage();
    // Frame'leri layout'tan çıkar ve sil.
    // Thumbnail'lar m_thumbnails map'inde yaşıyor — silinmiyor, sadece frame'den ayrılıyor.
    for (auto* frame : m_groupFrames) {
        for (const QString& cid : frame->clientIds()) {
            auto* t = frame->thumbnail(cid);
            if (t) {
                frame->removeThumbnail(cid);
                // thumbnail artık parent'sız ve gizli — rebuildGroupLayout'ta tekrar frame'e eklenecek
            }
        }
        ui->groupsLayout->removeWidget(frame);
        delete frame;
    }
    m_groupFrames.clear();

    // Güvenlik önlemi: m_thumbnails içindeki TÜM thumbnail'ların gizli ve frame dışı olduğundan emin ol
    for (auto* t : m_thumbnails) {
        if (t) {
            t->hide();
        }
    }

    // Stretch item'ları da temizle
    while (ui->groupsLayout->count() > 0) {
        auto* item = ui->groupsLayout->takeAt(0);
        delete item;
    }
}

ThumbnailGroupFrame* MainWindow::getOrCreateGroupFrame(const QString& nodeId,
                                                         const QString& name,
                                                         bool isUnassigned) {
    if (m_groupFrames.contains(nodeId)) return m_groupFrames[nodeId];

    auto* frame = new ThumbnailGroupFrame(nodeId, name, isUnassigned,
                                           ui->thumbnailContainer);
    frame->installEventFilter(this);
    if (frame->thumbArea()) {
        frame->thumbArea()->installEventFilter(this);
    }
    connect(frame, &ThumbnailGroupFrame::headerClicked,
            [this](const QString& nid) {
        for (auto* f : m_groupFrames) f->setAllSelected(false);
        if (m_groupFrames.contains(nid)) m_groupFrames[nid]->setAllSelected(true);
    });
    connect(frame, &ThumbnailGroupFrame::headerRightClicked,
            [this](const QString& nid, const QPoint& globalPos) {
        auto* f = m_groupFrames.value(nid);
        if (!f || f->clientIds().isEmpty()) return;
        const QStringList gc = f->clientIds();
        QMenu menu(this);
        menu.addAction(ThemeManager::icon(":/ui/group.svg"), f->groupName() + tr(" — tümünü seç"),
            [this, f]() { for (auto* gf : m_groupFrames) gf->setAllSelected(false); f->setAllSelected(true); });
        
        const bool canManageThisGroup = (m_activeUser.role == "YM") ||
                                       (m_session && m_session->canManageClass(nid));
        if (canManageThisGroup) {
            menu.addSeparator();
            menu.addAction(ThemeManager::icon(":/ui/lock.svg"),   tr("Tümünü Kilitle"),  [this, gc]() { for (const QString& id : gc) m_session->sendScreenLock(id); });
            menu.addAction(ThemeManager::icon(":/ui/unlock.svg"), tr("Kilidi Aç"),       [this, gc]() { for (const QString& id : gc) m_session->sendScreenUnlock(id); });
            auto* moveMenu = menu.addMenu(ThemeManager::icon(":/ui/group.svg"), tr("Grubu Taşı →"));
            m_treeManager->buildMoveMenu(moveMenu, gc, [this](const QString& nodeId2) -> bool {
                if (m_activeUser.role == "YM" || !m_session) return true;
                return m_session->canManageClass(nodeId2);
            });
        }
        menu.exec(globalPos);
    });
    ui->groupsLayout->addWidget(frame);
    m_groupFrames[nodeId] = frame;
    return frame;
}

QTreeWidgetItem* MainWindow::currentClassScopeNode() const {
    if (m_currentScopeNode) {
        if (ui && ui->classTree && (ui->classTree->indexOfTopLevelItem(m_currentScopeNode) >= 0 || m_currentScopeNode->treeWidget() == ui->classTree)) {
            return m_currentScopeNode;
        }
    }
    if (ui && ui->classTree) {
        const auto sel = ui->classTree->selectedItems();
        if (!sel.isEmpty() && sel.first()) {
            auto* node = sel.first();
            if (m_treeManager && m_treeManager->nodeType(node) == TreeManager::kTypeClient) {
                return node->parent();
            }
            return node;
        }
        return nullptr;
    }
    return nullptr;
}

void MainWindow::scheduleRebuildGroupLayout(QTreeWidgetItem* classNode) {
    if (classNode) {
        m_currentScopeNode = classNode;
    }
    if (!m_rebuildLayoutTimer) {
        m_rebuildLayoutTimer = new QTimer(this);
        m_rebuildLayoutTimer->setSingleShot(true);
        m_rebuildLayoutTimer->setInterval(80); // 80 ms debounce: 18 istemci akarken paneli sadece 1 kez yeniden kur
        connect(m_rebuildLayoutTimer, &QTimer::timeout, this, [this]() {
            if (m_rubberBand && m_rubberBand->isVisible()) {
                // Sürükleme devam ederken layout'u bozma, 100 ms sonra tekrar dene
                m_rebuildLayoutTimer->start(100);
                return;
            }
            rebuildGroupLayout(currentClassScopeNode());
        });
    }
    m_rebuildLayoutTimer->start();
}

void MainWindow::rebuildGroupLayout(QTreeWidgetItem* classNode) {
    if (classNode && m_treeManager && m_treeManager->nodeType(classNode) == TreeManager::kTypeClient) {
        classNode = classNode->parent();
    }

    clearGroupLayout();
    if (!m_treeManager) return;

    const int thumbW = m_thumbPx;
    const int thumbH = static_cast<int>(m_thumbPx * 0.78);
    const int contW  = ui->thumbnailScrollArea->viewport()->width();

    // 1. KULLANICI AĞAÇTA HİÇBİR ŞEY SEÇMEDİYSE (DESELECT):
    if (!classNode) {
        if (m_classHeaderLabel) m_classHeaderLabel->hide();
        showEmptyStateMessage(tr("Canlı izleme başlatmak için sınıflar panelinden bir sınıf veya 'Tüm İstemciler'i seçin."));
        updateStatusBar({}); // 0 istemci görünür -> m_session->setVisibleClients({}) -> 0 KB/s trafik!
        return;
    }

    if (m_treeManager->viewMode() == TreeManager::SubnetView) {
        if (m_classHeaderLabel) m_classHeaderLabel->hide();
        QList<QTreeWidgetItem*> subnets;
        if (m_treeManager->nodeType(classNode) == TreeManager::kTypeSubnet)
            subnets << classNode;
        else
            for (int i = 0; i < ui->classTree->topLevelItemCount(); ++i)
                subnets << ui->classTree->topLevelItem(i);
        for (auto* sn : subnets) {
            const QString cidr = sn->data(0, Qt::UserRole + 1).toString();
            auto* frame = getOrCreateGroupFrame(cidr, m_treeManager->subnetDisplayName(cidr));
            for (const QString& cid : m_treeManager->clientsInNode(sn))
                if (m_thumbnails.contains(cid)) frame->addThumbnail(m_thumbnails[cid]);
            frame->relayout(thumbW, thumbH, contW);
        }
        ui->groupsLayout->addStretch(1);
        updateStatusBar(m_treeManager->clientsInNode(classNode));
        return;
    }

    // 2. KULLANICI AÇIKÇA "TÜM İSTEMCİLER" DÜĞMESİNİ SEÇTİYSE:
    if (m_treeManager->nodeType(classNode) == TreeManager::kTypeAllClients) {
        if (m_classHeaderLabel) {
            m_classHeaderLabel->setPixmap(QPixmap(":/ui/school.svg").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_classHeaderLabel->setText("  " + tr("Tüm İstemciler"));
            m_classHeaderLabel->show();
        }
        auto* frame = getOrCreateGroupFrame("__all__", tr("Tüm İstemciler"));
        const QStringList clientIds = m_treeManager->clientsInNode(classNode);
        for (const QString& cid : clientIds) {
            if (m_thumbnails.contains(cid)) {
                frame->addThumbnail(m_thumbnails[cid]);
            }
        }
        frame->relayout(thumbW, thumbH, contW);
        ui->groupsLayout->addStretch(1);
        updateStatusBar(clientIds);
        return;
    }

    const QString ntype = m_treeManager->nodeType(classNode);

    if (ntype == TreeManager::kTypeClass || ntype == TreeManager::kTypeGroup) {
        // Başlık her zaman üst sınıf adını gösterir
        QTreeWidgetItem* classItem = classNode;
        if (ntype == TreeManager::kTypeGroup && classNode->parent())
            classItem = classNode->parent();

        if (m_classHeaderLabel) {
            m_classHeaderLabel->setPixmap(QPixmap(":/ui/monitor.svg").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_classHeaderLabel->setText("  " + m_treeManager->displayName(classItem));
            m_classHeaderLabel->show();
        }
        bool hasGroups = false;
        for (int i = 0; i < classNode->childCount(); ++i) {
            auto* child = classNode->child(i);
            if (m_treeManager->nodeType(child) != TreeManager::kTypeGroup) continue;
            hasGroups = true;
            const QString gId = m_treeManager->publicStableId(child);
            auto* frame = getOrCreateGroupFrame(gId, m_treeManager->displayName(child));
            for (const QString& cid : m_treeManager->clientsInNode(child))
                if (m_thumbnails.contains(cid)) frame->addThumbnail(m_thumbnails[cid]);
            frame->relayout(thumbW, thumbH, contW);
        }
        const QString classId = m_treeManager->publicStableId(classNode);
        QStringList ungrouped;
        for (const QString& cid : m_treeManager->clientsInNode(classNode))
            if (m_treeManager->nodeForClient(cid) == classNode) ungrouped << cid;
        if (!ungrouped.isEmpty() || !hasGroups) {
            auto* frame = getOrCreateGroupFrame(classId + "_direct",
                hasGroups ? "Grupsuz" : m_treeManager->displayName(classNode));
            for (const QString& cid : ungrouped)
                if (m_thumbnails.contains(cid)) frame->addThumbnail(m_thumbnails[cid]);
            frame->relayout(thumbW, thumbH, contW);
        }
        updateStatusBar(m_treeManager->clientsInNode(classNode));
    } else if (ntype == TreeManager::kTypeSubnet) {
        if (m_classHeaderLabel) m_classHeaderLabel->hide();
        const QString cidr = classNode->data(0, Qt::UserRole + 1).toString();
        auto* frame = getOrCreateGroupFrame(cidr,
            "Atanmam\u0131\u015f · " + m_treeManager->subnetDisplayName(cidr), true);
        for (const QString& cid : m_treeManager->clientsInNode(classNode))
            if (m_thumbnails.contains(cid)) frame->addThumbnail(m_thumbnails[cid]);
        frame->relayout(thumbW, thumbH, contW);
        updateStatusBar(m_treeManager->clientsInNode(classNode));
    }
    ui->groupsLayout->addStretch(1);
}

void MainWindow::updateTreeLockEmojis(const QString& classNodeId,
                                       bool chatLocked, bool privateLocked) {
    if (!m_treeManager) return;
    const bool wasSorting = ui->classTree->isSortingEnabled();
    ui->classTree->setSortingEnabled(false);

    QTreeWidgetItemIterator it(ui->classTree);
    while (*it) {
        const QString type = m_treeManager->nodeType(*it);
        if (type == TreeManager::kTypeClass || type == TreeManager::kTypeGroup) {
            const QString nid = m_treeManager->publicStableId(*it);
            if (nid == classNodeId) {
                (*it)->setIcon(1, ThemeManager::icon(chatLocked     ? ":/ui/lock.svg" : ":/ui/unlock.svg"));
                (*it)->setIcon(2, ThemeManager::icon(privateLocked  ? ":/ui/lock.svg" : ":/ui/unlock.svg"));
                (*it)->setText(1, "");
                (*it)->setText(2, "");
                (*it)->setTextAlignment(1, Qt::AlignCenter);
                (*it)->setTextAlignment(2, Qt::AlignCenter);
                // Tıklama imleci hissi için araç ipucu
                (*it)->setToolTip(1, chatLocked
                    ? tr("Sohbet kilitli (tıkla: aç)")
                    : tr("Sohbet açık (tıkla: kilitle)"));
                (*it)->setToolTip(2, privateLocked
                    ? tr("Özel sohbet kilitli (tıkla: aç)")
                    : tr("Özel sohbet açık (tıkla: kilitle)"));
                break;
            }
        }
        ++it;
    }
    ui->classTree->setSortingEnabled(wasSorting);
}

void MainWindow::refreshAllTreeLockEmojis() {
    if (!m_roomMgr) return;
    const bool wasSorting = ui->classTree->isSortingEnabled();
    ui->classTree->setSortingEnabled(false);

    const auto states = m_roomMgr->allLockStates();
    for (auto it = states.constBegin(); it != states.constEnd(); ++it)
        updateTreeLockEmojis(it.key(), it->chatLocked, it->privateLocked);

    // Durumu olmayan node'lar için varsayılan kilit simgesi
    QTreeWidgetItemIterator tit(ui->classTree);
    while (*tit) {
        const QString type = m_treeManager->nodeType(*tit);
        if (type == TreeManager::kTypeClass || type == TreeManager::kTypeGroup) {
            if ((*tit)->icon(1).isNull()) {
                (*tit)->setIcon(1, ThemeManager::icon(":/ui/unlock.svg"));
                (*tit)->setIcon(2, ThemeManager::icon(":/ui/unlock.svg"));
                (*tit)->setText(1, "");
                (*tit)->setText(2, "");
                (*tit)->setTextAlignment(1, Qt::AlignCenter);
                (*tit)->setTextAlignment(2, Qt::AlignCenter);
            }
        }
        ++tit;
    }
    ui->classTree->setSortingEnabled(wasSorting);
}

void MainWindow::relayoutThumbnails() {
    const int thumbW = m_thumbPx;
    const int thumbH = static_cast<int>(m_thumbPx * 0.78);
    const int contW  = ui->thumbnailScrollArea->viewport()->width();
    for (auto* f : m_groupFrames) f->relayout(thumbW, thumbH, contW);
}

void MainWindow::onTreeSelectionChanged() {
    const auto selected = ui->classTree->selectedItems();
    QTreeWidgetItem* node = selected.isEmpty() ? nullptr : selected.first();
    
    // Her durumda, grup değiştiği veya yeni bir seçim yapıldığı için tüm thumbnail seçimlerini temizle
    for (auto* t : m_thumbnails) {
        t->setSelected(false);
    }

    m_selectionSource = node ? SelectionSource::Tree : SelectionSource::None;

    if (node && m_treeManager->nodeType(node) == TreeManager::kTypeClient) {
        const QString clientId = node->data(0, Qt::UserRole + 1).toString();
        node = node->parent();
        m_currentScopeNode = node;
        
        rebuildGroupLayout(node);
        
        if (m_thumbnails.contains(clientId)) {
            m_thumbnails[clientId]->setSelected(true);
        }
    } else {
        m_currentScopeNode = node;
        rebuildGroupLayout(node);
    }

    if (node) {
        const QString nid = m_treeManager->publicStableId(node);
        const bool locked = m_nodeLockState.value(nid, false);
    }
    // İdari PC (Tip 2) kontrolü: Seçili grupta/tüm istemcilerde kilit durumu güncelle
    updateLockToggleState();
    updateSelectionTargetBadge();
}

void MainWindow::onQualityChanged(int value) {
    m_thumbPx = value;
    if (auto* lbl = findChild<QLabel*>("thumbSizeLabel"))
        lbl->setText(QString::number(value) + "px");
    applyThumbnailSize(value);
}

void MainWindow::applyThumbnailSize(int px) {
    m_thumbPx = px;
    const int thumbH = static_cast<int>(px * 0.78);
    for (auto* thumb : m_thumbnails)
        thumb->setFixedSize(px, thumbH);
    relayoutThumbnails();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_lockOverlay) {
        m_lockOverlay->setGeometry(rect());
    }
    updateFloatingDiscoveryPillGeometry();
    // Pencere boyutu değişince sütun sayısını yeniden hesapla
    // Küçük bir gecikme ile çağır — viewport boyutu güncellenmeden önce
    QTimer::singleShot(0, this, &MainWindow::relayoutThumbnails);
}

