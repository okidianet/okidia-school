// master/tree/TreeManager.h
#pragma once

#include <QBrush>
#include <QMap>
#include <QMenu>
#include <QObject>
#include <QPoint>
#include <QSettings>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <functional>

class SessionManager;

/**
 * TreeManager — iki görünümlü istemci ağacı
 *
 *  SINIF GÖRÜNÜMÜ (ClassView):
 *   🖥 Lab-1                       (type=class)
 *      💻 PC-01 · ahmet            (type=client)
 *      📂 Grup-1                   (type=group)
 *         💻 PC-02 · mehmet        (type=client)
 *   📡 Atanmamış · 192.168.1.x    (type=subnet, silinemez)
 *      💻 PC-03 · ali              (type=client, sınıf ataması yok)
 *
 *  AĞ GÖRÜNÜMÜ (SubnetView):
 *   📡 Bilgisayar Lab (192.168.1.x)  (type=subnet)
 *      💻 PC-01 · Sınıf 3-A         (type=client)
 *      💻 PC-02 · Atanmamış         (type=client)
 *
 *  Toggle: setViewMode(ClassView | SubnetView)
 *  Subnet isimleri: QSettings("okidia","master") → subnets/<cidr>/name
 */
class TreeManager : public QObject {
    Q_OBJECT

public:
    // ── Görünüm modu ──────────────────────────────────────────────────────
    enum ViewMode { ClassView, SubnetView };

    // ── MIME types ────────────────────────────────────────────────────────
    static const char kMimeClientIds[];
    static const char kMimeTreeNode[];

    // ── Node type constants ───────────────────────────────────────────────
    static constexpr const char* kTypeClass      = "class";
    static constexpr const char* kTypeGroup      = "group";
    static constexpr const char* kTypeClient     = "client";
    static constexpr const char* kTypeSubnet     = "subnet";      // dinamik, silinemez
    static constexpr const char* kTypeAllClients = "all_clients"; // Tüm İstemciler kök düğümü
    // Qt::UserRole   → tip (kType*)
    // Qt::UserRole+1 → clientId (client) | cidr (subnet) | uuid (class/group)

    explicit TreeManager(QTreeWidget* tree, QObject* parent = nullptr);
    ~TreeManager() override = default;

    // ── Görünüm değiştirme ────────────────────────────────────────────────
    ViewMode viewMode() const { return m_viewMode; }
    void     setViewMode(ViewMode mode);

    // Aktif kullanıcı (login sonrası set edilir — DB yazımında createdBy için)
    void setActiveUserId(const QString& userId);
    void setSession(SessionManager* session) { m_session = session; }

    // Yetki callback'i — SessionManager::canManageClient bağlanır.
    // Boşsa (YM) tüm işlemler serbest.
    void setPermissionChecker(std::function<bool(const QString& clientId)> fn) {
        m_canManageClient = std::move(fn);
    }

    // Sınıf yetki callback'i — SessionManager::canManageClass bağlanır.
    void setClassPermissionChecker(std::function<bool(const QString& classId)> fn) {
        m_canManageClass = std::move(fn);
    }

    // ── Sınıf / Grup CRUD (ClassView) ─────────────────────────────────────
    /** parent = nullptr → top-level sınıf
     *  parent = class item → grup                    */
    QTreeWidgetItem* addNode(QTreeWidgetItem* parent,
                              const QString&   name,
                              const QString&   icon = "");
    void    deleteNode(QTreeWidgetItem* item);
    void    renameNode(QTreeWidgetItem* item, const QString& newName);
    QString displayName(QTreeWidgetItem* item) const;
    QString nodeType(QTreeWidgetItem* item) const;

    // ── Subnet ismi (SubnetView, QSettings) ───────────────────────────────
    QString subnetDisplayName(const QString& cidr) const;
    void    setSubnetName(const QString& cidr, const QString& name);

    // ── Client yönetimi ───────────────────────────────────────────────────
    /** clientId, displayLabel, ip (subnet için), classNodeId (atanmış sınıf uuid'si) */
    void assignClient(const QString& clientId,
                      const QString& hostname,
                      const QString& username,
                      const QString& ip,
                      const QString& classNodeId = QString(),
                      int agentType = 0);

    void updateClientClass(const QString& clientId, const QString& classNodeId);
    void moveClient(const QString& clientId, QTreeWidgetItem* targetNode);
    void removeClient(const QString& clientId);
    void setClientOnline(const QString& clientId, bool online);
    void setClientAgentType(const QString& clientId, int agentType);
    void setClientDevPin(const QString& clientId, const QString& pin, int remainingSeconds);

    // ── Sorgular ──────────────────────────────────────────────────────────
    QStringList      clientsInNode(QTreeWidgetItem* node) const;
    QTreeWidgetItem* nodeForClient(const QString& clientId) const;

    // ── Yardımcılar ───────────────────────────────────────────────────────
    void buildMoveMenu(QMenu* menu, const QStringList& clientIds,
                       std::function<bool(const QString& nodeId)> canTarget = nullptr);
    bool filterItem(QTreeWidgetItem* item, const QString& text);
    void setOwnershipFilter(bool onlyOwnClasses, const QSet<QString>& managedClassIds);
    void setTextFilter(const QString& text);
    void applyFilters();

    // ── Kalıcı kayıt (ClassView yapısı) ──────────────────────────────────
    void save();
    void load();
    void reloadTree();
    void syncClientMetaFromDb();

    // ── UUID erişimi (ChatRoomManager için) ──────────────────────────────
    QString publicStableId(QTreeWidgetItem* item) { return stableId(item); }
    QTreeWidgetItem* nodeById(const QString& id) const;
    QTreeWidget* treeWidget() const { return m_tree; }

    // ── OM yetki görselleştirme ───────────────────────────────────────────
    // Boş map = YM modu (reset). OM: class/group → canManage bool
    void applyPermissionView(const QMap<QString, bool>& classPermissions);

    // ── ClassView atanmamış bucket erişimi ───────────────────────────────
    // "Atanmamış" subnet node'unu cidr'e göre döndürür (yoksa oluşturur)
    QTreeWidgetItem* unassignedBucketFor(const QString& cidr);

signals:
    void clientMoved(const QString& clientId, QTreeWidgetItem* newNode);
    void structureChanged();
    void viewModeChanged(ViewMode mode);
    void emptyAreaClicked();
    // Subnet ismini değiştirme isteği (çift tık → dialog → buradan emit)
    void subnetRenameRequested(const QString& cidr, const QString& currentName);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void saveExpandedState();
    // ── Görünüm inşa/temizle ─────────────────────────────────────────────
    void buildClassView();
    void buildSubnetView();
    void clearView();   // tüm top-level item'ları sil, client map'leri koru

    // ── Subnet yardımcıları ───────────────────────────────────────────────
    static QString cidrFromIp(const QString& ip);   // "192.168.1.5" → "192.168.1.x"
    QTreeWidgetItem* getOrCreateSubnetNode(const QString& cidr);

    // ── Client item etiketi ───────────────────────────────────────────────
    QString clientLabelClassView(const QString& clientId) const;
    QString clientLabelSubnetView(const QString& clientId) const;
    void    refreshClientLabel(const QString& clientId);

    // ── Drop ─────────────────────────────────────────────────────────────
    void handleNodeDrop(const QString& nodeId, QTreeWidgetItem* target);
    void handleClientDrop(const QStringList& clientIds, QTreeWidgetItem* target);
    bool isAncestorOf(QTreeWidgetItem* ancestor, QTreeWidgetItem* item) const;
    bool isValidClientTarget(QTreeWidgetItem* item) const;
    void setItemOnlineState(QTreeWidgetItem* item, bool online);

    // ── UUID ─────────────────────────────────────────────────────────────
    QString          stableId(QTreeWidgetItem* item);

    // ── State ─────────────────────────────────────────────────────────────
    QTreeWidget*  m_tree;
    SessionManager* m_session = nullptr;
    ViewMode      m_viewMode = ClassView;
    QString       m_activeUserId;
    bool          m_isRenaming = false;
    std::function<bool(const QString&)> m_canManageClient;  // nullptr = tümü serbest
    std::function<bool(const QString&)> m_canManageClass;   // nullptr = tümü serbest

    // ClassView — class/group node'ları
    QTreeWidgetItem*                m_allClientsNode = nullptr;
    QMap<QTreeWidgetItem*, QString> m_nodeToId;
    QMap<QString, QTreeWidgetItem*> m_idToNode;
    QMap<QString, QString>          m_pendingAssign;  // clientId → nodeId

    // SubnetView — cidr → subnet node
    QMap<QString, QTreeWidgetItem*> m_subnetNodes;
    // ClassView — atanmamış bucket'lar: cidr → node
    QMap<QString, QTreeWidgetItem*> m_unassignedNodes;

    // Client meta (her iki görünümde gerekli)
    struct ClientMeta {
        QString hostname;
        QString username;
        QString ip;
        QString classNodeId;   // UUID, boşsa atanmamış
        QString className;     // görüntü için sınıf adı (her iki görünümde kullanılır)
        bool    online = false;
        int     agentType = 0;
    };
    QMap<QString, ClientMeta>       m_clientMeta;    // clientId → meta
    QMap<QString, QTreeWidgetItem*> m_clientItems;   // clientId → item (aktif görünüm)
    QMap<QString, QTreeWidgetItem*> m_clientMap;     // clientId → ebeveyn node

    // Drag state
    QTreeWidgetItem* m_dragItem  = nullptr;
    QPoint           m_dragStart;

    // Filter state
    bool             m_onlyOwnClasses = false;
    QSet<QString>    m_managedClassIds;
    QString          m_filterText;
};
