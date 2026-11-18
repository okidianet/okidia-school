// master/tree/TreeContextMenu.cpp
#include "TreeManager.h"
#include "../../common/ui/ThemeManager.h"
#include <QMenu>

void TreeManager::buildMoveMenu(QMenu* menu, const QStringList& clientIds,
                                std::function<bool(const QString&)> canTarget)
{
    std::function<void(QTreeWidgetItem*, QMenu*)> addItem =
        [&](QTreeWidgetItem* node, QMenu* parent) {
        const QString type = nodeType(node);
        if (type != kTypeClass && type != kTypeGroup) return;
        const QString nodeId = m_nodeToId.value(node);
        if (canTarget && !canTarget(nodeId)) return;
        const QString name = displayName(node);
        auto* act = parent->addAction(node->icon(0), name);
        connect(act, &QAction::triggered, this, [this, clientIds, node]() {
            for (const QString& id : clientIds) moveClient(id, node);
        });
        auto* sub = new QMenu(parent);
        for (int i = 0; i < node->childCount(); ++i) addItem(node->child(i), sub);
        if (!sub->isEmpty()) act->setMenu(sub);
        else delete sub;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        addItem(m_tree->topLevelItem(i), menu);
}
