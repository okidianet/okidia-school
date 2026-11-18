// master/tree/TreeManager.cpp
#include "TreeManager.h"
#include "../../common/ui/ThemeManager.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include "../../common/db/DbRecords.h"
#include "../audit/AuditLog.h"

#include <QMessageBox>
#include <QSettings>
#include <functional>

const char TreeManager::kMimeClientIds[] = "application/x-okidia-clientids";
const char TreeManager::kMimeTreeNode[]  = "application/x-okidia-treenode";

static const QColor kColorReadOnly(110, 130, 160);  // OM salt-okunur sınıf

TreeManager::TreeManager(QTreeWidget* tree, QObject* parent)
    : QObject(parent), m_tree(tree)
{
    Q_ASSERT(tree);
    m_tree->setDragEnabled(false);
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->viewport()->installEventFilter(this);
    m_tree->installEventFilter(this);
    
    m_tree->setSortingEnabled(true);
    m_tree->sortItems(0, Qt::AscendingOrder);

    connect(m_tree, &QTreeWidget::itemExpanded, this, &TreeManager::saveExpandedState);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, &TreeManager::saveExpandedState);
    connect(m_tree, &QTreeWidget::itemChanged, this, &TreeManager::onItemChanged);
}

void TreeManager::setActiveUserId(const QString& userId)
{
    m_activeUserId = userId;

    auto refreshSubnets = [&](QMap<QString, QTreeWidgetItem*>& map, bool isUnassigned) {
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            const QString& cidr = it.key();
            QTreeWidgetItem* node = it.value();
            if (!node) continue;
            const QString disp    = subnetDisplayName(cidr);
            const bool    renamed = (disp != cidr);
            const QString label   = isUnassigned
                ? tr("Atanmamış · ") + disp
                : "" + disp;
            node->setText(0, label);
            node->setToolTip(0, renamed ? cidr : QString{});
        }
    };
    refreshSubnets(m_subnetNodes,    false);
    refreshSubnets(m_unassignedNodes, true);
}

void TreeManager::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode) return;
    m_viewMode = mode;
    clearView();
    if (mode == ClassView)
        buildClassView();
    else
        buildSubnetView();
    emit viewModeChanged(mode);
}

void TreeManager::clearView()
{
    m_allClientsNode = nullptr;
    m_clientItems.clear();
    m_clientMap.clear();
    m_subnetNodes.clear();
    m_unassignedNodes.clear();
    m_tree->clear();
    m_idToNode.clear();
    m_nodeToId.clear();
}

void TreeManager::buildClassView()
{
    m_tree->setSortingEnabled(false);
    load();
    syncClientMetaFromDb();

    // En tepeye "Tüm İstemciler" kök düğümü ekle
    m_allClientsNode = new OkidiaTreeItem(m_tree);
    m_allClientsNode->setText(0, tr("Tüm İstemciler"));
    m_allClientsNode->setData(0, Qt::UserRole,     kTypeAllClients);
    m_allClientsNode->setData(0, Qt::UserRole + 1, "__all_clients__");
    m_allClientsNode->setIcon(0, ThemeManager::icon(":/ui/school.svg"));
    m_allClientsNode->setFlags((m_allClientsNode->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
    m_allClientsNode->setFirstColumnSpanned(true);

    for (auto it = m_clientMeta.constBegin(); it != m_clientMeta.constEnd(); ++it) {
        const QString& cid  = it.key();
        const ClientMeta& m = it.value();
        QTreeWidgetItem* target = nullptr;
        if (!m.classNodeId.isEmpty())
            target = m_idToNode.value(m.classNodeId);
        if (!target)
            target = unassignedBucketFor(cidrFromIp(m.ip));

        auto* item = new OkidiaTreeItem(target);
        item->setData(0, Qt::UserRole,     kTypeClient);
        item->setData(0, Qt::UserRole + 1, cid);
        item->setFlags((item->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDropEnabled);
        item->setFirstColumnSpanned(true); // İstemci satırları tek sütun (birleştirilmiş hücre) olsun, kilit sütunlarına taşmasın
        setItemOnlineState(item, m.online);
        m_clientItems[cid] = item;
        m_clientMap[cid]   = target;
        refreshClientLabel(cid);
    }
    m_tree->setSortingEnabled(true);
    m_tree->sortItems(0, Qt::AscendingOrder);
}

void TreeManager::buildSubnetView()
{
    m_tree->setSortingEnabled(false);
    for (auto it = m_clientMeta.constBegin(); it != m_clientMeta.constEnd(); ++it) {
        const QString& cid  = it.key();
        const ClientMeta& m = it.value();
        const QString cidr  = cidrFromIp(m.ip);
        QTreeWidgetItem* subnetNode = getOrCreateSubnetNode(cidr);

        auto* item = new OkidiaTreeItem(subnetNode);
        item->setData(0, Qt::UserRole,     kTypeClient);
        item->setData(0, Qt::UserRole + 1, cid);
        item->setFlags((item->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDropEnabled);
        item->setFirstColumnSpanned(true); // İstemci satırları tek sütun olsun
        setItemOnlineState(item, m.online);
        m_clientItems[cid] = item;
        m_clientMap[cid]   = subnetNode;
        refreshClientLabel(cid);
    }
    m_tree->setSortingEnabled(true);
    m_tree->sortItems(0, Qt::AscendingOrder);
}

QString TreeManager::cidrFromIp(const QString& ip)
{
    if (ip.isEmpty()) return "unknown";
    const QStringList parts = ip.split('.');
    if (parts.size() == 4)
        return parts[0] + "." + parts[1] + "." + parts[2] + ".x";
    return "unknown";
}

QString TreeManager::subnetDisplayName(const QString& cidr) const
{
    QSettings s("okidia", "master");
    const QString userKey   = "subnets/" + m_activeUserId + "/" + cidr + "/name";
    const QString sharedKey = "subnets/shared/" + cidr + "/name";
    const QString saved = s.value(userKey, s.value(sharedKey)).toString();
    return saved.isEmpty() ? cidr : saved;
}

void TreeManager::setSubnetName(const QString& cidr, const QString& name)
{
    QSettings s("okidia", "master");
    const QString key = m_activeUserId.isEmpty()
        ? "subnets/shared/" + cidr + "/name"
        : "subnets/" + m_activeUserId + "/" + cidr + "/name";

    if (name.isEmpty() || name == cidr)
        s.remove(key);
    else
        s.setValue(key, name);

    auto updateNode = [&](QTreeWidgetItem* node) {
        if (!node) return;
        const QString disp    = subnetDisplayName(cidr);
        const bool    renamed = (disp != cidr);
        node->setText(0, "" + disp);
        node->setToolTip(0, renamed ? cidr : QString{});
    };
    updateNode(m_subnetNodes.value(cidr));
    updateNode(m_unassignedNodes.value(cidr));
}

QTreeWidgetItem* TreeManager::getOrCreateSubnetNode(const QString& cidr)
{
    if (m_subnetNodes.contains(cidr))
        return m_subnetNodes[cidr];

    const QString disp    = subnetDisplayName(cidr);
    const bool    renamed = (disp != cidr);

    auto* node = new OkidiaTreeItem(m_tree);
    node->setText(0, disp);
    node->setData(0, Qt::UserRole,     kTypeSubnet);
    node->setData(0, Qt::UserRole + 1, cidr);
    node->setIcon(0, ThemeManager::icon(":/ui/network.svg"));
    node->setFlags((node->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled);
    node->setExpanded(true);
    if (renamed) node->setToolTip(0, cidr);
    m_subnetNodes[cidr] = node;
    return node;
}

QTreeWidgetItem* TreeManager::unassignedBucketFor(const QString& cidr)
{
    if (m_unassignedNodes.contains(cidr))
        return m_unassignedNodes[cidr];

    const QString disp    = subnetDisplayName(cidr);
    const bool    renamed = (disp != cidr);
    const QString label   = tr("Atanmamış · ") + disp;

    auto* node = new OkidiaTreeItem(m_tree);
    node->setText(0, label);
    node->setData(0, Qt::UserRole,     kTypeSubnet);
    node->setData(0, Qt::UserRole + 1, cidr);
    node->setIcon(0, ThemeManager::icon(":/ui/network.svg"));
    node->setFlags((node->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled);
    node->setExpanded(true);
    if (renamed) node->setToolTip(0, cidr + tr(" · Sınıf ataması yapılmamış"));
    m_unassignedNodes[cidr] = node;
    return node;
}

QString TreeManager::clientLabelClassView(const QString& clientId) const
{
    const ClientMeta& m = m_clientMeta.value(clientId);
    const QString host  = m.hostname.isEmpty() ? clientId : m.hostname;
    if (!m.online) return host;
    const QString user  = m.username.isEmpty() ? "" : " · " + m.username;
    return host + user;
}

QString TreeManager::clientLabelSubnetView(const QString& clientId) const
{
    const ClientMeta& m = m_clientMeta.value(clientId);
    const QString host  = m.hostname.isEmpty() ? clientId : m.hostname;
    if (!m.online) return host;
    if (!m.className.isEmpty())
        return host + " · " + m.className;
    return host + tr(" · Atanmamış");
}

void TreeManager::refreshClientLabel(const QString& clientId)
{
    auto* item = m_clientItems.value(clientId);
    if (!item) return;
    const QString label = (m_viewMode == ClassView)
        ? clientLabelClassView(clientId)
        : clientLabelSubnetView(clientId);
    item->setText(0, label);

    const ClientMeta& m = m_clientMeta.value(clientId);
    if (!m.online) {
        item->setIcon(0, ThemeManager::icon(":/ui/offline.svg"));
    } else {
        int agentType = m.agentType;
        if (agentType == 1) {
            item->setIcon(0, ThemeManager::icon(":/ui/student.svg"));
        } else if (agentType == 2) {
            item->setIcon(0, ThemeManager::icon(":/ui/admin.svg"));
        } else if (agentType == 3) {
            item->setIcon(0, ThemeManager::icon(":/ui/smartboard.svg"));
        } else if (agentType >= 100) {
            item->setIcon(0, ThemeManager::icon(":/ui/custom_profile.svg"));
        } else {
            item->setIcon(0, ThemeManager::icon(":/ui/online.svg"));
        }
    }
}

void TreeManager::assignClient(const QString& clientId,
                                const QString& hostname,
                                const QString& username,
                                const QString& ip,
                                const QString& classNodeId,
                                int agentType)
{
    ClientMeta& meta = m_clientMeta[clientId];
    meta.hostname    = hostname;
    meta.username    = username;
    meta.ip          = ip;
    meta.agentType   = agentType;

    if (!classNodeId.isEmpty()) {
        meta.classNodeId = classNodeId;
    } else if (m_pendingAssign.contains(clientId)) {
        meta.classNodeId = m_pendingAssign.take(clientId);
    }

    if (!meta.classNodeId.isEmpty()) {
        auto* node = m_idToNode.value(meta.classNodeId);
        meta.className = node ? displayName(node) : meta.className;
    } else {
        meta.className.clear();
    }

    const QString cidr = cidrFromIp(ip);

    if (m_viewMode == ClassView) {
        QTreeWidgetItem* target = nullptr;
        if (!meta.classNodeId.isEmpty())
            target = m_idToNode.value(meta.classNodeId);
        if (!target)
            target = unassignedBucketFor(cidr);

        if (m_clientItems.contains(clientId)) {
            auto* item = m_clientItems[clientId];
            setItemOnlineState(item, meta.online);
            refreshClientLabel(clientId);
            if (item->parent() != target) {
                if (auto* old = item->parent()) old->removeChild(item);
                target->addChild(item);
                m_clientMap[clientId] = target;
                emit clientMoved(clientId, target);
            }
            return;
        }

        auto* item = new OkidiaTreeItem(target);
        item->setData(0, Qt::UserRole,     kTypeClient);
        item->setData(0, Qt::UserRole + 1, clientId);
        item->setFlags((item->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDropEnabled);
        item->setFirstColumnSpanned(true);
        m_clientItems[clientId] = item;
        m_clientMap[clientId]   = target;
        setItemOnlineState(item, meta.online);
        refreshClientLabel(clientId);
        emit clientMoved(clientId, target);

    } else {
        QTreeWidgetItem* subnetNode = getOrCreateSubnetNode(cidr);

        if (m_clientItems.contains(clientId)) {
            auto* item = m_clientItems[clientId];
            setItemOnlineState(item, meta.online);
            refreshClientLabel(clientId);
            return;
        }

        auto* item = new OkidiaTreeItem(subnetNode);
        item->setData(0, Qt::UserRole,     kTypeClient);
        item->setData(0, Qt::UserRole + 1, clientId);
        item->setFlags((item->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsEditable & ~Qt::ItemIsDropEnabled);
        item->setFirstColumnSpanned(true);
        m_clientItems[clientId] = item;
        m_clientMap[clientId]   = subnetNode;
        setItemOnlineState(item, meta.online);
        refreshClientLabel(clientId);
        emit clientMoved(clientId, subnetNode);
    }
}

void TreeManager::updateClientClass(const QString& clientId, const QString& classNodeId)
{
    if (!m_clientMeta.contains(clientId)) return;
    m_clientMeta[clientId].classNodeId = classNodeId;
    if (m_viewMode == ClassView) {
        QTreeWidgetItem* target = classNodeId.isEmpty()
            ? unassignedBucketFor(cidrFromIp(m_clientMeta[clientId].ip))
            : m_idToNode.value(classNodeId);
        if (target) moveClient(clientId, target);
    }
    refreshClientLabel(clientId);
}

void TreeManager::moveClient(const QString& clientId, QTreeWidgetItem* targetNode)
{
    auto* item = m_clientItems.value(clientId);
    if (!item || !targetNode) return;
    if (!isValidClientTarget(targetNode)) return;

    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(m_tree, tr("Bağlantı Yok"),
            tr("Broker bağlantısı aktif değil. İstemciyi taşımak için lütfen sunucuya bağlanın."));
        return;
    }

    auto* old = item->parent();
    if (old == targetNode) return;
    if (old) old->removeChild(item);
    targetNode->addChild(item);
    m_clientMap[clientId] = targetNode;

    if (old && old->childCount() == 0) {
        if (nodeType(old) == kTypeSubnet) {
            const QString cidr = old->data(0, Qt::UserRole + 1).toString();
            if (m_unassignedNodes.value(cidr) == old) {
                m_unassignedNodes.remove(cidr);
                delete old;
            }
        }
    }

    const QString type = nodeType(targetNode);
    if (type == kTypeClass || type == kTypeGroup) {
        m_clientMeta[clientId].classNodeId = stableId(targetNode);
        m_clientMeta[clientId].className   = displayName(targetNode);
    } else {
        m_clientMeta[clientId].classNodeId.clear();
        m_clientMeta[clientId].className.clear();
    }

    QString machineId = clientId;
    if (m_session) {
        QString sid = m_session->clientState(clientId).machineId;
        if (!sid.isEmpty()) machineId = sid;
    }

    const QString newClassId = m_clientMeta[clientId].classNodeId;
    if (m_session) {
        m_session->setClientGroup(machineId, newClassId);
    }
    if (m_session && m_session->isConnected()) {
        if (!machineId.isEmpty()) {
            m_session->sendDbAgentAssign(machineId, newClassId);
        }
    }
    if (OkidiaDatabase::instance().isOpen() && !machineId.isEmpty()) {
        OkidiaDatabase::instance().updateAgentClass(machineId, newClassId);
    }

    refreshClientLabel(clientId);
    const QString dest = m_clientMeta[clientId].className.isEmpty()
        ? tr("Atanmamış") : m_clientMeta[clientId].className;
    AuditLog::instance().recordCommand({}, clientId,
        tr("İstemci taşındı → ") + dest);
    emit clientMoved(clientId, targetNode);
    save();
}

void TreeManager::removeClient(const QString& clientId)
{
    if (auto* item = m_clientItems.take(clientId)) {
        if (auto* par = item->parent()) par->removeChild(item);
        delete item;
    }
    m_clientMap.remove(clientId);
    m_clientMeta.remove(clientId);
    m_pendingAssign.remove(clientId);
}

void TreeManager::setClientOnline(const QString& clientId, bool online)
{
    m_clientMeta[clientId].online = online;
    if (auto* item = m_clientItems.value(clientId)) {
        setItemOnlineState(item, online);
        refreshClientLabel(clientId);
    }
}

void TreeManager::setClientAgentType(const QString& clientId, int agentType)
{
    m_clientMeta[clientId].agentType = agentType;
    if (m_clientItems.contains(clientId)) {
        refreshClientLabel(clientId);
    }
}

void TreeManager::setClientDevPin(const QString& clientId, const QString& pin, int remainingSeconds)
{
    if (auto* item = m_clientItems.value(clientId)) {
        if (pin.isEmpty()) {
            item->setToolTip(0, QString());
        } else {
            item->setToolTip(0, tr("🔑 Akıllı Tahta Test PIN: [ %1 ] (Kalan: %2s)").arg(pin).arg(remainingSeconds));
        }
    }
}

QString TreeManager::nodeType(QTreeWidgetItem* item) const {
    return item ? item->data(0, Qt::UserRole).toString() : QString{};
}

QString TreeManager::displayName(QTreeWidgetItem* item) const {
    if (!item) return {};
    const QString saved = item->data(0, Qt::UserRole + 2).toString();
    return saved.isEmpty() ? item->text(0) : saved;
}

QTreeWidgetItem* TreeManager::addNode(QTreeWidgetItem* parent,
                                       const QString&   name,
                                       const QString&   icon)
{
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(m_tree, tr("Bağlantı Yok"),
            tr("Broker bağlantısı aktif değil. Yeni sınıf/grup oluşturmak için lütfen sunucuya bağlanın."));
        return nullptr;
    }

    if (parent && nodeType(parent) == kTypeGroup) {
        parent = parent->parent();
    }

    const bool makeClass = (parent == nullptr);

    QTreeWidgetItem* node;
    if (makeClass) {
        node = new OkidiaTreeItem(m_tree);
    } else {
        node = new OkidiaTreeItem(parent);
    }

    node->setText(0, name);
    node->setData(0, Qt::UserRole, makeClass ? kTypeClass : kTypeGroup);
    node->setData(0, Qt::UserRole + 2, name);
    node->setIcon(0, ThemeManager::icon(makeClass  ? ":/ui/school.svg" : ":/ui/group.svg"));
    node->setExpanded(true);
    node->setFlags(node->flags() | Qt::ItemIsEditable | Qt::ItemIsSelectable);

    const QString id = stableId(node);

    if (m_session && m_session->isConnected()) {
        m_session->sendDbClassAdd(name, icon, (makeClass || !parent) ? QString{} : stableId(parent), makeClass ? "class" : "group", id);
    }
    if (OkidiaDatabase::instance().isOpen()) {
        ClassRecord rec;
        rec.id        = id;
        rec.name      = name;
        rec.icon      = icon;
        rec.type      = makeClass ? "class" : "group";
        rec.parentId  = (makeClass || !parent)
                        ? QString{}
                        : stableId(parent);
        rec.createdBy = m_activeUserId;
        rec.createdAt = OkidiaUtils::nowSecs();
        rec.updatedAt = rec.createdAt;
        OkidiaDatabase::instance().upsertClass(rec);

        if (!m_activeUserId.isEmpty()) {
            OkidiaDatabase::instance().grantPermission(
                m_activeUserId, id, "full");
        }
    }

    save();
    AuditLog::instance().recordCommand({}, id,
        (makeClass ? tr("Sınıf oluşturuldu: ") : tr("Grup oluşturuldu: ")) + name);
    emit structureChanged();
    return node;
}

void TreeManager::deleteNode(QTreeWidgetItem* item)
{
    if (!item) return;
    const QString type = nodeType(item);
    if (type == kTypeSubnet) return;

    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(m_tree, tr("Bağlantı Yok"),
            tr("Broker bağlantısı aktif değil. Sınıf/grup silmek için lütfen sunucuya bağlanın."));
        return;
    }

    QStringList idsToDelete;
    std::function<void(QTreeWidgetItem*)> collectIds = [&](QTreeWidgetItem* n) {
        const QString t = nodeType(n);
        if (t == kTypeClass || t == kTypeGroup) {
            const QString id = m_nodeToId.value(n);
            if (!id.isEmpty()) idsToDelete << id;
        }
        for (int i = 0; i < n->childCount(); ++i) collectIds(n->child(i));
    };
    collectIds(item);

    std::function<void(QTreeWidgetItem*)> moveClientsUp = [&](QTreeWidgetItem* n) {
        for (int i = n->childCount() - 1; i >= 0; --i) {
            auto* child = n->child(i);
            const QString ct = nodeType(child);
            if (ct == kTypeClient) {
                const QString cid = child->data(0, Qt::UserRole + 1).toString();
                m_clientMeta[cid].classNodeId.clear();
                m_clientMeta[cid].className.clear();
                const QString cidr = cidrFromIp(m_clientMeta.value(cid).ip);
                n->removeChild(child);
                QTreeWidgetItem* bucket = unassignedBucketFor(cidr);
                bucket->addChild(child);
                m_clientMap[cid]  = bucket;
                if (!cid.isEmpty()) {
                    if (m_session && m_session->isConnected())
                        m_session->sendDbAgentAssign(cid, {});
                    if (OkidiaDatabase::instance().isOpen())
                        OkidiaDatabase::instance().updateAgentClass(cid, {});
                }
                emit clientMoved(cid, bucket);
            } else if (ct == kTypeGroup || ct == kTypeClass) {
                moveClientsUp(child);
            }
        }
    };
    moveClientsUp(item);

    std::function<void(QTreeWidgetItem*)> cleanIds = [&](QTreeWidgetItem* n) {
        if (m_nodeToId.contains(n)) {
            m_idToNode.remove(m_nodeToId.value(n));
            m_nodeToId.remove(n);
        }
        for (int i = 0; i < n->childCount(); ++i) cleanIds(n->child(i));
    };
    cleanIds(item);

    if (auto* par = item->parent()) par->removeChild(item);
    else m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(item));
    delete item;

    if (m_session && m_session->isConnected()) {
        for (const QString& id : idsToDelete)
            m_session->sendDbClassDelete(id);
    } else if (OkidiaDatabase::instance().isOpen()) {
        for (const QString& id : idsToDelete)
            OkidiaDatabase::instance().removeClass(id);
    }

    AuditLog::instance().recordCommand({}, idsToDelete.join(", "),
        tr("Sınıf/grup silindi (") + QString::number(idsToDelete.size()) + " node)");
    save();
    emit structureChanged();
}

void TreeManager::renameNode(QTreeWidgetItem* item, const QString& newName)
{
    if (!item || newName.trimmed().isEmpty()) return;
    const QString type = nodeType(item);
    if (type == kTypeSubnet) {
        const QString cidr = item->data(0, Qt::UserRole + 1).toString();
        setSubnetName(cidr, newName.trimmed());
        return;
    }

    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(m_tree, tr("Bağlantı Yok"),
            tr("Broker bağlantısı aktif değil. Sınıf/grup adını değiştirmek için lütfen sunucuya bağlanın."));
        return;
    }

    const QString cleanName = newName.trimmed();
    const bool wasRenaming = m_isRenaming;
    m_isRenaming = true;
    item->setText(0, cleanName);
    item->setData(0, Qt::UserRole + 2, cleanName);
    m_isRenaming = wasRenaming;

    const QString nodeId = m_nodeToId.value(item);
    if (!nodeId.isEmpty()) {
        for (auto it = m_clientMeta.begin(); it != m_clientMeta.end(); ++it) {
            if (it->classNodeId == nodeId) {
                it->className = cleanName;
                refreshClientLabel(it.key());
            }
        }
        if (m_session && m_session->isConnected()) {
            m_session->sendDbClassRename(nodeId, cleanName);
        } else if (OkidiaDatabase::instance().isOpen()) {
            ClassRecord rec = OkidiaDatabase::instance().classById(nodeId);
            if (!rec.id.isEmpty()) {
                rec.name      = cleanName;
                rec.updatedAt = OkidiaUtils::nowSecs();
                OkidiaDatabase::instance().upsertClass(rec);
            }
        }
    }
    save();
    emit structureChanged();
}

QStringList TreeManager::clientsInNode(QTreeWidgetItem* node) const
{
    if (!node || nodeType(node) == kTypeAllClients) {
        if (m_onlyOwnClasses) {
            QStringList ownClients;
            for (auto it = m_clientMeta.constBegin(); it != m_clientMeta.constEnd(); ++it) {
                const QString& cNodeId = it->classNodeId;
                if (!cNodeId.isEmpty()) {
                    if (m_managedClassIds.contains(cNodeId)) {
                        ownClients << it.key();
                    } else if (auto* item = m_idToNode.value(cNodeId)) {
                        if (auto* par = item->parent()) {
                            const QString parId = m_nodeToId.value(par);
                            if (!parId.isEmpty() && m_managedClassIds.contains(parId)) {
                                ownClients << it.key();
                            }
                        }
                    }
                }
            }
            return ownClients;
        }
        QStringList all;
        for (auto it = m_clientItems.constBegin(); it != m_clientItems.constEnd(); ++it)
            all << it.key();
        return all;
    }
    QStringList result;
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* n) {
        for (int i = 0; i < n->childCount(); ++i) {
            auto* ch = n->child(i);
            if (nodeType(ch) == kTypeClient)
                result << ch->data(0, Qt::UserRole + 1).toString();
            else
                collect(ch);
        }
    };
    collect(node);
    return result;
}

QTreeWidgetItem* TreeManager::nodeForClient(const QString& clientId) const {
    return m_clientMap.value(clientId, nullptr);
}

void TreeManager::setOwnershipFilter(bool onlyOwnClasses, const QSet<QString>& managedClassIds)
{
    m_onlyOwnClasses = onlyOwnClasses;
    m_managedClassIds = managedClassIds;
    applyFilters();
}

void TreeManager::setTextFilter(const QString& text)
{
    m_filterText = text.trimmed();
    applyFilters();
}

void TreeManager::applyFilters()
{
    const bool wasSorting = m_tree->isSortingEnabled();
    m_tree->setSortingEnabled(false);
    std::function<bool(QTreeWidgetItem*)> eval = [&](QTreeWidgetItem* item) -> bool {
        if (!item) return false;

        const QString type = nodeType(item);

        // 1. "Kendi Sınıflarım" (m_onlyOwnClasses) devredeyken hiyerarşik filtreleme ve budama
        if (m_onlyOwnClasses) {
            // Sınıfsız ("Atanmamış") düğümler Kendi Sınıflarım modunda doğrudan gizlenir
            if (type == kTypeSubnet) {
                item->setHidden(true);
                return false;
            }

            // "Tüm İstemciler" kök düğümü her zaman görünür kalır (metin filtresine tabi)
            if (type == kTypeAllClients) {
                bool matchText = true;
                if (!m_filterText.isEmpty()) {
                    matchText = item->text(0).contains(m_filterText, Qt::CaseInsensitive);
                }
                item->setHidden(!matchText);
                return matchText;
            }

            // Sınıf ve gruplar için sahiplik kontrolü
            if (type == kTypeClass || type == kTypeGroup) {
                const QString id = m_nodeToId.value(item);
                bool isOwned = false;
                if (!id.isEmpty() && id != "unassigned") {
                    if (m_managedClassIds.contains(id)) {
                        isOwned = true;
                    } else if (QTreeWidgetItem* par = item->parent()) {
                        const QString parId = m_nodeToId.value(par);
                        if (!parId.isEmpty() && m_managedClassIds.contains(parId)) {
                            isOwned = true;
                        }
                    }
                }

                // Yetkili olunmayan sınıf/grup içindeki istemciler ne olursa olsun budanır ve gizlenir
                if (!isOwned) {
                    item->setHidden(true);
                    return false;
                }
            }
        }

        // 2. Alt düğümleri değerlendir (özyineleme)
        bool anyChildVisible = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (eval(item->child(i))) {
                anyChildVisible = true;
            }
        }

        // 3. Metin filtreleme
        bool matchText = true;
        if (!m_filterText.isEmpty()) {
            matchText = item->text(0).contains(m_filterText, Qt::CaseInsensitive);
        }

        bool visible = matchText || anyChildVisible;
        item->setHidden(!visible);
        return visible;
    };

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        eval(m_tree->topLevelItem(i));
    }
    m_tree->setSortingEnabled(wasSorting);
}

bool TreeManager::filterItem(QTreeWidgetItem* item, const QString& text)
{
    Q_UNUSED(item);
    setTextFilter(text);
    return true;
}

void TreeManager::applyPermissionView(const QMap<QString, bool>& classPermissions)
{
    const bool resetAll = classPermissions.isEmpty();
    std::function<void(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* item) {
        if (!item) return;
        const QString type = nodeType(item);
        if (type == kTypeClass || type == kTypeGroup) {
            const bool can = resetAll
                ? true
                : classPermissions.value(m_nodeToId.value(item), false);
            if (can) {
                item->setForeground(0, QBrush{});
                QFont f = item->font(0); f.setItalic(false); item->setFont(0, f);
                item->setToolTip(0, {});
            } else {
                item->setForeground(0, QBrush{kColorReadOnly});
                QFont f = item->font(0); f.setItalic(true); item->setFont(0, f);
                item->setToolTip(0,
                    "Bu sınıfı görüntüleme yetkiniz var ancak yönetemezsiniz.\n"
                    "Yönetmek için ilgili öğretmenden veya yöneticiden yetki isteyin.");
            }
        }
        for (int i = 0; i < item->childCount(); ++i) apply(item->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        apply(m_tree->topLevelItem(i));
}

void TreeManager::save()
{
    if (OkidiaDatabase::instance().isOpen()) return;

    QSettings s("okidia", "master");
    s.beginGroup("tree");
    s.remove("nodes");
    s.beginGroup("nodes");
    std::function<void(QTreeWidgetItem*)> saveNode = [&](QTreeWidgetItem* item) {
        const QString type = nodeType(item);
        if (type != kTypeClass && type != kTypeGroup) return;
        const QString id   = stableId(item);
        s.beginGroup(id);
        s.setValue("name", displayName(item));
        s.setValue("icon", "");
        s.setValue("type", type);
        auto* par = item->parent();
        if (par && (nodeType(par) == kTypeClass || nodeType(par) == kTypeGroup))
            s.setValue("parent", stableId(par));
        else
            s.remove("parent");
        s.endGroup();
        for (int i = 0; i < item->childCount(); ++i) saveNode(item->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        saveNode(m_tree->topLevelItem(i));
    s.endGroup();

    s.remove("clients");
    s.beginGroup("clients");
    for (auto it = m_clientMeta.constBegin(); it != m_clientMeta.constEnd(); ++it) {
        if (!it.value().classNodeId.isEmpty())
            s.setValue(it.key(), it.value().classNodeId);
    }
    s.endGroup();
    s.endGroup();
}

void TreeManager::reloadTree()
{
    // Seçili düğümü sakla (Sınıf, grup, subnet veya tekil istemci)
    QString selectedType;
    QString selectedId;
    const auto selected = m_tree->selectedItems();
    if (!selected.isEmpty() && selected.first()) {
        selectedType = nodeType(selected.first());
        if (selectedType == kTypeClient) {
            selectedId = selected.first()->data(0, Qt::UserRole + 1).toString();
        } else if (selectedType == kTypeSubnet) {
            selectedId = selected.first()->data(0, Qt::UserRole + 1).toString();
        } else {
            selectedId = publicStableId(selected.first());
        }
    }

    QSettings s("okidia", "master");
    QStringList collapsedList = s.value("Tree/CollapsedNodes").toStringList();
    QSet<QString> collapsedNodes(collapsedList.begin(), collapsedList.end());

    // Yeniden kurma sırasında aradaki sıfırlanma (null) sinyallerini engelle
    {
        const QSignalBlocker blocker(m_tree);
        clearView();

        if (m_viewMode == ClassView)
            buildClassView();
        else
            buildSubnetView();

        QTreeWidgetItemIterator it2(m_tree);
        while (*it2) {
            const QString type = nodeType(*it2);
            if (type == kTypeClass || type == kTypeGroup || type == kTypeSubnet) {
                if (collapsedNodes.contains(stableId(*it2))) {
                    (*it2)->setExpanded(false);
                } else {
                    (*it2)->setExpanded(true);
                }
            }
            ++it2;
        }

        // Önceki seçimi geri yükle
        if (!selectedId.isEmpty()) {
            QTreeWidgetItem* restoreItem = nullptr;
            if (selectedType == kTypeClient) {
                restoreItem = m_clientItems.value(selectedId, nullptr);
            } else if (selectedType == kTypeSubnet) {
                restoreItem = m_subnetNodes.value(selectedId, m_unassignedNodes.value(selectedId, nullptr));
            } else {
                restoreItem = nodeById(selectedId);
            }

            if (restoreItem) {
                restoreItem->setSelected(true);
                m_tree->setCurrentItem(restoreItem);
            }
        }
    }
    emit structureChanged();
}

void TreeManager::load()
{
    if (OkidiaDatabase::instance().isOpen()) {
        const QList<ClassRecord> classes = OkidiaDatabase::instance().allClasses();

        QMap<QString, QTreeWidgetItem*> created;
        for (const ClassRecord& c : classes) {
            auto* item = new OkidiaTreeItem;
            item->setText(0, c.name);
            item->setExpanded(true);
            item->setData(0, Qt::UserRole, c.type == "group" ? kTypeGroup : kTypeClass);
            item->setData(0, Qt::UserRole + 2, c.name);
            item->setIcon(0, ThemeManager::icon(c.type == "group"  ? ":/ui/group.svg" : ":/ui/school.svg"));
            item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsSelectable);
            m_nodeToId[item] = c.id;
            m_idToNode[c.id] = item;
            created[c.id]    = item;
        }

        QMap<QString, ClassRecord> remaining;
        for (const ClassRecord& c : classes) remaining[c.id] = c;

        for (int pass = 0; !remaining.isEmpty() && pass < 30; ++pass) {
            QStringList resolved;
            for (auto it = remaining.constBegin(); it != remaining.constEnd(); ++it) {
                const ClassRecord& c = it.value();
                if (c.parentId.isEmpty()) {
                    m_tree->addTopLevelItem(created[c.id]);
                    resolved << c.id;
                } else if (created.contains(c.parentId)) {
                    created[c.parentId]->addChild(created[c.id]);
                    resolved << c.id;
                }
            }
            for (const QString& r : resolved) remaining.remove(r);
        }

        m_pendingAssign.clear();
        const QList<AgentRecord> agents = OkidiaDatabase::instance().allAgents();
        for (const AgentRecord& a : agents) {
            if (!a.classId.isEmpty())
                m_pendingAssign[a.machineId] = a.classId;
        }
        return;
    }

    QSettings s("okidia", "master");
    s.beginGroup("tree");
    s.beginGroup("nodes");

    const QStringList ids = s.childGroups();
    QMap<QString, QTreeWidgetItem*> created;
    QMap<QString, QString>           parentMap;

    for (const QString& id : ids) {
        s.beginGroup(id);
        const QString name   = s.value("name").toString();
        const QString icon   = s.value("icon", "").toString();
        const QString parent = s.value("parent", "").toString();
        const QString type   = s.value("type", kTypeClass).toString();
        s.endGroup();

        auto* item = new OkidiaTreeItem;
        item->setText(0, name);
        item->setExpanded(true);
        item->setData(0, Qt::UserRole, type);
        item->setData(0, Qt::UserRole + 2, name);
        item->setIcon(0, ThemeManager::icon(type == "group"  ? ":/ui/group.svg" : ":/ui/school.svg"));
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsSelectable);
        m_nodeToId[item] = id;
        m_idToNode[id]   = item;
        created[id]      = item;
        parentMap[id]    = parent;
    }

    QMap<QString, QString> remaining = parentMap;
    for (int pass = 0; !remaining.isEmpty() && pass < 30; ++pass) {
        QStringList resolved;
        for (auto it = remaining.constBegin(); it != remaining.constEnd(); ++it) {
            const QString& nodeId   = it.key();
            const QString& parentId = it.value();
            if (parentId.isEmpty()) {
                m_tree->addTopLevelItem(created[nodeId]);
                resolved << nodeId;
            } else {
                auto* par = created.value(parentId);
                if (!par) continue;
                par->addChild(created[nodeId]);
                resolved << nodeId;
            }
        }
        for (const QString& r : resolved) remaining.remove(r);
    }
    s.endGroup();

    s.beginGroup("clients");
    m_pendingAssign.clear();
    for (const QString& clientId : s.childKeys())
        m_pendingAssign[clientId] = s.value(clientId).toString();
    s.endGroup();
    s.endGroup();
}

QString TreeManager::stableId(QTreeWidgetItem* item) {
    if (!item) return {};
    if (nodeType(item) == kTypeSubnet)
        return item->data(0, Qt::UserRole + 1).toString();
    if (nodeType(item) == kTypeAllClients)
        return "__all_clients__";
    if (!m_nodeToId.contains(item)) {
        const QString id = OkidiaUtils::generateUuid();
        m_nodeToId[item] = id;
        m_idToNode[id]   = item;
    }
    return m_nodeToId.value(item);
}

QTreeWidgetItem* TreeManager::nodeById(const QString& id) const {
    if (id == "__all_clients__") return m_allClientsNode;
    return m_idToNode.value(id, nullptr);
}

void TreeManager::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (m_isRenaming || !item || column != 0) return;

    const QString type = nodeType(item);
    if (type != kTypeClass && type != kTypeGroup) return;

    const QString oldName = item->data(0, Qt::UserRole + 2).toString();
    const QString newName = item->text(0).trimmed();

    // Değişiklik yoksa
    if (newName == oldName) {
        if (item->text(0) != oldName) {
            const bool wasRenaming = m_isRenaming;
            m_isRenaming = true;
            item->setText(0, oldName);
            m_isRenaming = wasRenaming;
        }
        return;
    }

    auto rollback = [this, item, oldName]() {
        const bool wasRenaming = m_isRenaming;
        m_isRenaming = true;
        item->setText(0, oldName);
        m_isRenaming = wasRenaming;
    };

    // Boş isim kontrolü
    if (newName.isEmpty()) {
        rollback();
        QMessageBox::warning(m_tree, tr("Geçersiz İsim"),
            tr("Sınıf veya grup adı boş bırakılamaz."));
        return;
    }

    // Yetki kontrolü (Örn. OM yetkisi)
    const QString nodeId = m_nodeToId.value(item);
    if (m_canManageClass && !m_canManageClass(nodeId)) {
        rollback();
        QMessageBox::warning(m_tree, tr("Yetersiz Yetki"),
            tr("Bu sınıf veya grubu yeniden adlandırma yetkiniz yok."));
        return;
    }

    // Broker bağlantısı kontrolü
    if (!m_session || !m_session->isConnected()) {
        rollback();
        QMessageBox::warning(m_tree, tr("Bağlantı Yok"),
            tr("Broker bağlantısı aktif değil. Sınıf/grup adını değiştirmek için lütfen sunucuya bağlanın."));
        return;
    }

    // Her şey uygun, kalıcı olarak yeniden adlandır
    renameNode(item, newName);
}

void TreeManager::syncClientMetaFromDb()
{
    if (!OkidiaDatabase::instance().isOpen()) return;
    const auto allAgents = OkidiaDatabase::instance().allAgents();
    for (const auto& a : allAgents) {
        if (m_session) {
            m_session->setClientGroup(a.machineId, a.classId);
        }
        if (m_clientMeta.contains(a.machineId)) {
            m_clientMeta[a.machineId].classNodeId = a.classId;
            if (!a.classId.isEmpty()) {
                auto* node = m_idToNode.value(a.classId);
                m_clientMeta[a.machineId].className = node ? displayName(node) : QString{};
            } else {
                m_clientMeta[a.machineId].className.clear();
            }
        } else {
            ClientMeta meta;
            meta.hostname = a.hostname;
            meta.ip = a.ip;
            meta.classNodeId = a.classId;
            meta.agentType = a.agentType;
            meta.online = false;
            if (!a.classId.isEmpty()) {
                auto* node = m_idToNode.value(a.classId);
                meta.className = node ? displayName(node) : QString{};
            }
            m_clientMeta[a.machineId] = meta;
        }
    }
}

