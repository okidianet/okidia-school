// master/tree/TreeManagerUi.cpp
#include "TreeManager.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/Logger.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QMessageBox>

void TreeManager::setItemOnlineState(QTreeWidgetItem* item, bool online) {
    if (!item) return;
    static const QColor kColorOffline(140, 140, 140);
    item->setForeground(0, online ? QBrush{} : QBrush{kColorOffline});
    QFont f = item->font(0);
    f.setItalic(!online);
    item->setFont(0, f);
}

bool TreeManager::isValidClientTarget(QTreeWidgetItem* item) const {
    if (!item) return false;
    const QString t = nodeType(item);
    return t == kTypeClass || t == kTypeGroup || t == kTypeSubnet;
}

bool TreeManager::isAncestorOf(QTreeWidgetItem* ancestor, QTreeWidgetItem* item) const {
    auto* p = item->parent();
    while (p) {
        if (p == ancestor) return true;
        p = p->parent();
    }
    return false;
}

bool TreeManager::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_tree) {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                m_tree->selectionModel()->clear();
                m_tree->clearSelection();
                m_tree->setCurrentItem(nullptr);
                m_tree->viewport()->update();
                emit emptyAreaClicked();
                return true;
            }
        }
        return false;
    }

    if (obj != m_tree->viewport()) return false;

    if (ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        const QPoint pos = me->position().toPoint();
        QTreeWidgetItem* item = m_tree->itemAt(pos);

        if (me->button() == Qt::LeftButton) {
            m_dragItem  = item;
            m_dragStart = pos;

            if (!item) {
                m_tree->setFocus();
                m_tree->selectionModel()->clear();
                m_tree->clearSelection();
                m_tree->setCurrentItem(nullptr);
                m_tree->viewport()->update();
                emit emptyAreaClicked();
                return true;
            }
        } else if (me->button() == Qt::RightButton) {
            if (!item) {
                m_tree->selectionModel()->clear();
                m_tree->clearSelection();
                m_tree->setCurrentItem(nullptr);
                m_tree->viewport()->update();
            }
        }
    }

    if (ev->type() == QEvent::MouseMove && m_dragItem) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if ((me->buttons() & Qt::LeftButton) &&
            (me->position().toPoint() - m_dragStart).manhattanLength() > QApplication::startDragDistance())
        {
            const QString type = nodeType(m_dragItem);
            if (type == kTypeClient) {
                auto* drag  = new QDrag(m_tree);
                auto* mime  = new QMimeData;

                const auto sel = m_tree->selectedItems();
                QStringList ids;
                for (auto* it : sel)
                    if (nodeType(it) == kTypeClient)
                        ids << it->data(0, Qt::UserRole + 1).toString();
                if (!ids.contains(m_dragItem->data(0, Qt::UserRole + 1).toString()))
                    ids << m_dragItem->data(0, Qt::UserRole + 1).toString();
                mime->setData(kMimeClientIds, ids.join('\n').toUtf8());

                drag->setMimeData(mime);
                drag->exec(Qt::MoveAction);
            }
            m_dragItem = nullptr;
            return true;
        }
    }

    if (ev->type() == QEvent::DragEnter) {
        auto* de = static_cast<QDragEnterEvent*>(ev);
        if (de->mimeData()->hasFormat(kMimeClientIds)) {
            de->acceptProposedAction();
        } else {
            de->ignore();
        }
        return true;
    }
    if (ev->type() == QEvent::DragMove) {
        auto* de = static_cast<QDragMoveEvent*>(ev);
        if (de->mimeData()->hasFormat(kMimeClientIds)) {
            auto* target = m_tree->itemAt(de->position().toPoint());
            if (target && isValidClientTarget(target)) {
                de->acceptProposedAction();
            } else if (!target) {
                de->acceptProposedAction();
            } else {
                de->ignore();
            }
        } else {
            de->ignore();
        }
        return true;
    }

    if (ev->type() == QEvent::Drop) {
        auto* de = static_cast<QDropEvent*>(ev);
        auto* target = m_tree->itemAt(de->position().toPoint());
        if (de->mimeData()->hasFormat(kMimeClientIds)) {
            const QStringList ids = QString::fromUtf8(
                de->mimeData()->data(kMimeClientIds)).split('\n', Qt::SkipEmptyParts);
            
            if (!target) {
                QMessageBox::StandardButton reply;
                reply = QMessageBox::question(m_tree, tr("Sınıf Atamasını Kaldır"),
                    QString(tr("Seçili %1 istemcinin sınıf atamasını kaldırıp 'Atanmamış' durumuna döndürmek istediğinize emin misiniz?"))
                    .arg(ids.size()),
                    QMessageBox::Yes | QMessageBox::No);
                
                if (reply == QMessageBox::Yes) {
                    for (const QString& id : ids) {
                        if (m_canManageClient && !m_canManageClient(id)) {
                            Logger::info(QObject::tr("TreeManager: Taşıma engellendi (yetki yok) → ") + id);
                            continue;
                        }
                        QTreeWidgetItem* bucket = unassignedBucketFor(cidrFromIp(m_clientMeta[id].ip));
                        if (bucket) moveClient(id, bucket);
                    }
                }
            } else {
                handleClientDrop(ids, target);
            }
        }
        de->acceptProposedAction();
        return true;
    }

    if (ev->type() == QEvent::MouseButtonRelease)
        m_dragItem = nullptr;

    return false;
}

void TreeManager::handleClientDrop(const QStringList& clientIds, QTreeWidgetItem* target)
{
    if (!isValidClientTarget(target)) return;

    const QString targetType = nodeType(target);
    if (targetType == kTypeClass || targetType == kTypeGroup) {
        const QString targetClassId = publicStableId(target);
        if (m_canManageClass && !m_canManageClass(targetClassId)) {
            Logger::info(QObject::tr("TreeManager: Taşıma engellendi (hedef sınıf yetkisi yok) → ") + targetClassId);
            return;
        }
    }

    for (const QString& id : clientIds) {
        if (m_canManageClient && !m_canManageClient(id)) {
            Logger::info(QObject::tr("TreeManager: Taşıma engellendi (yetki yok) → ") + id);
            continue;
        }
        moveClient(id, target);
    }
}

void TreeManager::handleNodeDrop(const QString& nodeId, QTreeWidgetItem* target)
{
    auto* dragged = nodeById(nodeId);
    if (!dragged || !target || dragged == target) return;
    if (isAncestorOf(dragged, target)) return;
    if (nodeType(target) == kTypeClient || nodeType(target) == kTypeSubnet) return;

    if (auto* oldPar = dragged->parent()) oldPar->removeChild(dragged);
    else m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(dragged));

    const QString targetType = nodeType(target);
    if (targetType == kTypeClass) {
        dragged->setData(0, Qt::UserRole, kTypeGroup);
        dragged->setIcon(0, ThemeManager::icon(":/ui/group.svg"));
        target->addChild(dragged);
    } else if (targetType == kTypeGroup) {
        dragged->setData(0, Qt::UserRole, kTypeGroup);
        dragged->setIcon(0, ThemeManager::icon(":/ui/group.svg"));
        if (auto* par = target->parent()) par->addChild(dragged);
        else m_tree->addTopLevelItem(dragged);
    } else {
        dragged->setData(0, Qt::UserRole, kTypeClass);
        dragged->setIcon(0, ThemeManager::icon(":/ui/school.svg"));
        m_tree->addTopLevelItem(dragged);
    }
    target->setExpanded(true);
    m_tree->setCurrentItem(dragged);
    emit structureChanged();
    save();
}

void TreeManager::saveExpandedState()
{
    QSettings s("okidia", "master");
    QStringList collapsedIds;
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        if (!(*it)->isExpanded()) {
            const QString type = nodeType(*it);
            if (type == kTypeClass || type == kTypeGroup || type == kTypeSubnet) {
                collapsedIds << stableId(*it);
            }
        }
        ++it;
    }
    s.setValue("Tree/CollapsedNodes", collapsedIds);
}
