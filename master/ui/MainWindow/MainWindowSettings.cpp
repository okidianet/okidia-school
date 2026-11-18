#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QScrollBar>
// master/ui/MainWindow/MainWindowSettings.cpp
#include "MainWindow.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../common/db/OkidiaDatabase.h"
#include <QTableWidget>
#include <QHeaderView>
#include <QHostAddress>
#include <QDateTime>
#include "ui_MainWindow.h"
#include <QDialogButtonBox>
#include "../../common/utils/Logger.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QTimer>
#include "ssh/SshClient.h"
#include "ssh/DeployPanel.h"
#include "ssh/OtaUpdatePanel.h"
#include "../UninstallPasswordDialog/UninstallPasswordDialog.h"
#include "../UserManagementPanel/UserManagementPanel.h"
#include "ui_MainWindow.h"
void MainWindow::filterThumbnailsByNode(QTreeWidgetItem* node) {
    rebuildGroupLayout(node);
}

void MainWindow::onClassFilterChanged(int index) {
    if (!m_treeManager || !m_session) return;
    if (m_activeUser.role != "OM") {
        m_treeManager->setOwnershipFilter(false, {});
        return;
    }
    bool onlyOwn = (index == 1);
    m_treeManager->setOwnershipFilter(onlyOwn, m_session->managedClassIds());

    if (!m_activeUser.username.isEmpty()) {
        QSettings s("okidia", "master");
        s.setValue("users/" + m_activeUser.username + "/ui/classFilterMode", index);
    }
}

void MainWindow::applyOMPermissions() {
    if (!m_session || !m_treeManager) return;

    // 1. TreeManager için class uuid → canManage haritası oluştur
    QMap<QString, bool> classPermissions;
    std::function<void(QTreeWidgetItem*)> collectNodes =
        [&](QTreeWidgetItem* item) {
        if (!item) return;
        const QString type = m_treeManager->nodeType(item);
        if (type == TreeManager::kTypeClass || type == TreeManager::kTypeGroup) {
            const QString id = m_treeManager->publicStableId(item);
            if (!id.isEmpty())
                classPermissions[id] = m_session->canManageClass(id);
        }
        for (int i = 0; i < item->childCount(); ++i)
            collectNodes(item->child(i));
    };
    for (int i = 0; i < ui->classTree->topLevelItemCount(); ++i)
        collectNodes(ui->classTree->topLevelItem(i));

    // TreeManager görsel güncelleme (yönetilmeyen sınıflar gri/italik)
    m_treeManager->applyPermissionView(classPermissions);

    // 2. Sınıf filtresini uygula
    if (ui->classFilterCombo && m_activeUser.role == "OM") {
        bool onlyOwn = (ui->classFilterCombo->currentIndex() == 1);
        m_treeManager->setOwnershipFilter(onlyOwn, m_session->managedClassIds());
    } else if (m_treeManager) {
        m_treeManager->setOwnershipFilter(false, {});
    }

    // 3. YM harici rollerde (OM ve OBSERVER) Deploy ve OTA panellerini gizle
    if (m_activeUser.role != "YM") {
        if (m_deployPanel) m_deployPanel->hide();
        if (m_otaUpdatePanel) m_otaUpdatePanel->hide();
    }

    // 4. Thumbnail grid'ini güncelle
    updateThumbnailVisibility();
}

void MainWindow::updateThumbnailVisibility() {
    if (!m_treeManager) return;
    const auto sel = ui->classTree ? ui->classTree->selectedItems() : QList<QTreeWidgetItem*>{};
    QTreeWidgetItem* node = sel.isEmpty() ? nullptr : sel.first();
    filterThumbnailsByNode(node);
    updateSelectionTargetBadge();
}

// ---------------------------------------------------------------------------
// Tree sağ tık menüsü
// ---------------------------------------------------------------------------
void MainWindow::onTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = ui->classTree->itemAt(pos);
    const QString type = item ? m_treeManager->nodeType(item) : QString{};

    // Client item → thumbnail ile aynı sağ tık menüsü
    if (type == TreeManager::kTypeClient) {
        const QString cId = item->data(0, Qt::UserRole + 1).toString();
        onThumbnailContextMenu(cId,
            ui->classTree->viewport()->mapToGlobal(pos));
        return;
    }

    // Subnet node → sadece isim değiştirme
    if (type == TreeManager::kTypeSubnet) {
        QMenu menu(this);
        const QString cidr = item->data(0, Qt::UserRole + 1).toString();
        QAction* renameAct = menu.addAction(ThemeManager::icon(":/ui/edit.svg"), tr("Ağ İsmini Düzenle"));
        connect(renameAct, &QAction::triggered, [this, cidr, item]() {
            const QString cur = m_treeManager->subnetDisplayName(cidr);
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Ağ İsmini Düzenle"),
                tr("Ağ adı (") + cidr + "):",
                QLineEdit::Normal, cur, &ok);
            if (ok) m_treeManager->setSubnetName(cidr, name.trimmed());
        });
        menu.exec(ui->classTree->viewport()->mapToGlobal(pos));
        return;
    }

    // SubnetView modunda iken sınıf/grup işlemleri sadece ClassView'da yapılır
    if (m_treeManager->viewMode() == TreeManager::SubnetView) return;

    // Gözlemci rolü ağaç yapısını değiştiremez
    if (m_activeUser.role == "OBSERVER") return;

    // OM yetki kontrolü: seçili node üzerinde yetkisi var mı?
    const bool isYM = m_activeUser.role == "YM";
    const bool canManageItem = item
        ? (isYM || m_session->canManageClass(m_treeManager->publicStableId(item)))
        : isYM;

    QMenu menu(this);

    // Sınıf ekle — YM ve OM her zaman yeni sınıf ekleyebilir
    QAction* addClassAct = menu.addAction(ThemeManager::icon(":/ui/school.svg"), tr("Sınıf / Lab Ekle"));
    connect(addClassAct, &QAction::triggered,
            this, &MainWindow::onTreeAddClass);

    // Alt grup ekle — sadece yetkili sınıf seçiliyken
    if (item && type == TreeManager::kTypeClass && canManageItem) {
        QAction* addGroupAct = menu.addAction(ThemeManager::icon(":/ui/group.svg"), tr("Alt Grup Ekle"));
        connect(addGroupAct, &QAction::triggered,
                this, &MainWindow::onTreeAddGroup);
    }

    // Yeniden adlandır / sil — sadece yetkili node için
    if (item && (type == TreeManager::kTypeClass
                 || type == TreeManager::kTypeGroup) && canManageItem) {
        menu.addSeparator();
        QAction* renameAct = menu.addAction(ThemeManager::icon(":/ui/edit.svg"), tr("Yeniden Adlandır"));
        connect(renameAct, &QAction::triggered,
                this, &MainWindow::onTreeRename);

        QAction* deleteAct = menu.addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Sil"));
        connect(deleteAct, &QAction::triggered,
                this, &MainWindow::onTreeDelete);
    }

    // Sınıf/Grup için toplu tip atama
    if (item && (type == TreeManager::kTypeClass || type == TreeManager::kTypeGroup) && canManageItem) {
        menu.addSeparator();
        QMenu* groupTypeMenu = menu.addMenu(ThemeManager::icon(":/ui/student.svg"), tr("Grup İstemci Tipini Değiştir"));
        const QStringList clients = m_treeManager->clientsInNode(item);
        groupTypeMenu->addAction(tr("Tümünü Öğrenci PC Yap"), [this, clients]() {
            for (const QString& id : clients) m_session->sendAgentTypeSet(id, 1);
        });
        groupTypeMenu->addAction(tr("Tümünü İdari PC Yap"), [this, clients]() {
            if (!m_session || clients.isEmpty()) return;
            int ret = QMessageBox::warning(this, tr("İdari PC Yapılandırması"),
                QString(tr("Gruptaki %1 istemcinin tamamını 'İdari PC' olarak değiştirmek istediğinize emin misiniz?\n\n"
                           "ÖNEMLİ: İdari PC'ler gizlilik odaklıdır. Tekrar Öğrenci PC veya Akıllı Tahta tipine geri dönmek için hedef bilgisayarlardan (İstemci) fiziksel onay alınması gerekecektir."))
                        .arg(clients.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret == QMessageBox::Yes) {
                for (const QString& id : clients) m_session->sendAgentTypeSet(id, 2);
            }
        });
        groupTypeMenu->addAction(tr("Tümünü Akıllı Tahta Yap"), [this, clients]() {
            for (const QString& id : clients) m_session->sendAgentTypeSet(id, 3);
        });

        // Özel Tipler
        const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
        if (!customProfiles.isEmpty()) {
            groupTypeMenu->addSeparator();
            for (const auto& cp : customProfiles) {
                groupTypeMenu->addAction(ThemeManager::icon(":/ui/custom_profile.svg"), tr("Tümünü %1 Yap").arg(cp.name), [this, clients, cp]() {
                    for (const QString& id : clients) m_session->sendAgentTypeSet(id, cp.id);
                });
            }
        }
    }

    // Seçili client'ları taşı
    QStringList selClients = selectedClientIds();
    if (!selClients.isEmpty()) {
        menu.addSeparator();
        QMenu* moveMenu = menu.addMenu(ThemeManager::icon(":/ui/school.svg"), tr("Seçili İstemcileri Taşı →"));
        m_treeManager->buildMoveMenu(moveMenu, selClients, [this](const QString& nodeId) -> bool {
        if (m_activeUser.role == "YM" || !m_session) return true;
        return m_session->canManageClass(nodeId);
    });
    }

    menu.exec(ui->classTree->viewport()->mapToGlobal(pos));
}

void MainWindow::onTreeAddClass() {
    bool ok;
    QString name = QInputDialog::getText(
        this, tr("Sınıf / Lab Ekle"),
        tr("Sınıf adı:"),
        QLineEdit::Normal, "Lab-", &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    m_treeManager->addNode(nullptr, name.trimmed(), "");
}

void MainWindow::onTreeAddGroup() {
    QList<QTreeWidgetItem*> sel = ui->classTree->selectedItems();
    QTreeWidgetItem* parent = sel.isEmpty() ? nullptr : sel.first();
    const QString pt = parent ? m_treeManager->nodeType(parent) : QString{};

    if (!parent || pt == TreeManager::kTypeSubnet || pt == TreeManager::kTypeClient) {
        QMessageBox::information(this, tr("Grup Ekle"), tr("Grup eklemek için önce bir Sınıf seçin."));
        return;
    }
    if (pt == TreeManager::kTypeGroup) {
        QMessageBox::information(this, tr("Grup Ekle"),
            tr("Gruplar yalnızca sınıfın direkt altında olabilir (en fazla 1 seviye)."));
        return;
    }

    // OM yetki kontrolü
    if (m_activeUser.role != "YM" && m_session &&
        !m_session->canManageClass(m_treeManager->publicStableId(parent))) {
        QMessageBox::warning(this, tr("Yetersiz Yetki"),
            tr("Bu sınıfa grup eklemek için yetkiniz yok."));
        return;
    }

    const QString parentId = m_treeManager->publicStableId(parent);

    bool ok;
    QString name = QInputDialog::getText(
        this, tr("Alt Grup Ekle"), tr("Grup adı:"), QLineEdit::Normal, "Grup-", &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    QTreeWidgetItem* currentParent = m_treeManager->nodeById(parentId);
    if (!currentParent) {
        Logger::warning(QObject::tr("onTreeAddGroup: Düğüm eklenmeye çalışılan sınıf arka planda silinmiş."));
        return;
    }

    m_treeManager->addNode(currentParent, name.trimmed(), "");
    currentParent->setExpanded(true);
}

void MainWindow::onTreeRename() {
    QList<QTreeWidgetItem*> sel = ui->classTree->selectedItems();
    if (sel.isEmpty()) return;
    QTreeWidgetItem* item = sel.first();
    const QString rt = m_treeManager->nodeType(item);
    if (rt != TreeManager::kTypeClass && rt != TreeManager::kTypeGroup) return;

    if (m_activeUser.role != "YM" && m_session &&
        !m_session->canManageClass(m_treeManager->publicStableId(item))) {
        QMessageBox::warning(this, tr("Yetersiz Yetki"), tr("Bu sınıfı yeniden adlandırma yetkiniz yok."));
        return;
    }

    const QString itemId = m_treeManager->publicStableId(item);
    const QString curName = m_treeManager->displayName(item);

    bool ok;
    QString name = QInputDialog::getText(
        this, tr("Yeniden Adlandır"), tr("Yeni ad:"),
        QLineEdit::Normal, curName, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    QTreeWidgetItem* currentItem = m_treeManager->nodeById(itemId);
    if (!currentItem) {
        Logger::warning(QObject::tr("onTreeRename: Yeniden adlandırılacak sınıf arka planda silinmiş."));
        return;
    }

    m_treeManager->renameNode(currentItem, name.trimmed());
}

void MainWindow::onTreeDelete() {
    QList<QTreeWidgetItem*> sel = ui->classTree->selectedItems();
    if (sel.isEmpty()) return;
    QTreeWidgetItem* item = sel.first();
    const QString dt = m_treeManager->nodeType(item);
    if (dt != TreeManager::kTypeClass && dt != TreeManager::kTypeGroup) return;

    if (m_activeUser.role != "YM" && m_session &&
        !m_session->canManageClass(m_treeManager->publicStableId(item))) {
        QMessageBox::warning(this, tr("Yetersiz Yetki"), tr("Bu sınıfı silme yetkiniz yok."));
        return;
    }

    const QString itemId = m_treeManager->publicStableId(item);
    const QString curName = m_treeManager->displayName(item);
    const QStringList affected = m_treeManager->clientsInNode(item);

    if (!affected.isEmpty()) {
        int ret = QMessageBox::question(
            this, tr("Düğüm Sil"),
            QString(tr("'%1' silinecek.\nİçindeki %2 istemci atanmamış listeye taşınacak.\nDevam edilsin mi?"))
                .arg(curName).arg(affected.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }

    QTreeWidgetItem* currentItem = m_treeManager->nodeById(itemId);
    if (!currentItem) {
        Logger::warning(QObject::tr("onTreeDelete: Silinecek sınıf arka planda zaten silinmiş."));
        return;
    }

    m_treeManager->deleteNode(currentItem);
    filterThumbnailsByNode(nullptr);
    if (ui->classTree->topLevelItemCount() > 0)
        ui->classTree->setCurrentItem(ui->classTree->topLevelItem(0));
}

// ============================================================================
// Çoklu seçim: Ctrl / Shift / Rubber Band
// ============================================================================

void MainWindow::handleThumbnailClick(const QString& clientId,
                                       Qt::KeyboardModifiers mods) {
    // Görünür thumbnailları görsel sıraya göre al (Shift için)
    QList<ClientThumbnail*> visible;
    if (ui && ui->groupsLayout) {
        for (int i = 0; i < ui->groupsLayout->count(); ++i) {
            auto* frame = qobject_cast<ThumbnailGroupFrame*>(ui->groupsLayout->itemAt(i)->widget());
            if (frame && frame->isVisible()) {
                QList<ClientThumbnail*> sorted = frame->sortedThumbnails();
                for (auto* t : sorted) {
                    if (t->isVisible()) visible << t;
                }
            }
        }
    }

    if (mods & Qt::ControlModifier) {
        // Ctrl: toggle — sadece tıklananı tersine çevir
        if (m_thumbnails.contains(clientId))
            m_thumbnails[clientId]->setSelected(
                !m_thumbnails[clientId]->isSelected());
        m_lastClickedId = clientId;

    } else if (mods & Qt::ShiftModifier && !m_lastClickedId.isEmpty()) {
        // Shift: aralık seçimi — son tıklanan ile bu arasındaki hepsini seç
        int from = -1, to = -1;
        for (int i = 0; i < visible.size(); ++i) {
            if (visible[i]->clientId() == m_lastClickedId) from = i;
            if (visible[i]->clientId() == clientId)        to   = i;
        }
        if (from > to) qSwap(from, to);
        if (from >= 0 && to >= 0) {
            for (int i = 0; i < visible.size(); ++i)
                visible[i]->setSelected(i >= from && i <= to);
        }
    } else {
        // Modifier yok: sadece bu seçili, diğerleri temizle
        for (auto* t : m_thumbnails)
            t->setSelected(t->clientId() == clientId);
        m_lastClickedId = clientId;
    }

    // Son Seçim Önceliği (Last-Active Selection Context):
    // Kullanıcı thumbnail panelinde seçim yaptığında, aktif komut kaynağı Thumbnail olur.
    // TreeView'daki aktif sınıf görünümü/filtresi bozulmaz, yalnızca komut hedefi önceliği değişir.
    m_selectionSource = SelectionSource::Thumbnail;

    updateLockToggleState();
    updateSelectionTargetBadge();
}

// Toolbar komutları için hedef: Son aktif seçim kaynağı önceliği (Last-Active Selection Context)
// 1. En son Thumbnail panelinde seçim yapıldıysa: Seçilen istemciler hedef alınır.
// 2. En son TreeView panelinde seçim yapıldıysa: Seçili sınıf/grup veya istemci hedef alınır.
// 3. Açık bir seçim yoksa: Ekranda görünen tüm istemciler hedef alınır.
QStringList MainWindow::commandTargets() const {
    if (m_activeUser.role == "OBSERVER") return {}; // Gözlemci komut gönderemez

    QStringList rawTargets;

    if (m_selectionSource == SelectionSource::Thumbnail) {
        QStringList sel = selectedClientIds();
        if (!sel.isEmpty()) {
            rawTargets = sel;
        }
    } else if (m_selectionSource == SelectionSource::Tree) {
        QTreeWidgetItem* node = currentClassScopeNode();
        if (node && m_treeManager) {
            if (m_treeManager->nodeType(node) == TreeManager::kTypeClient) {
                rawTargets = { node->data(0, Qt::UserRole + 1).toString() };
            } else {
                rawTargets = m_treeManager->clientsInNode(node);
            }
        }
    }

    // Fallback: Eğer yukarıdakilerden hedef çıkmadıysa ekrandaki tüm görünür istemciler
    if (rawTargets.isEmpty()) {
        QStringList visible = visibleClientIds();
        if (!visible.isEmpty()) {
            rawTargets = visible;
        } else {
            QTreeWidgetItem* node = currentClassScopeNode();
            rawTargets = m_treeManager ? m_treeManager->clientsInNode(node) : QStringList{};
        }
    }

    // OM için yetki filtresi (Gözlemci modunda başka sınıfa komut gitmesin)
    if (m_activeUser.role == "OM" && m_session) {
        const bool onlyOwn = (ui->classFilterCombo && ui->classFilterCombo->currentIndex() == 1);
        QStringList filtered;
        for (const QString& id : rawTargets) {
            const QString gId = m_session->clientGroup(id);
            const bool isAssigned = (!gId.isEmpty() && gId != "__root__");
            if (onlyOwn) {
                // Kendi Sınıflarım modunda atanmamış cihazlara da komut gidemez, sadece yetkili sınıflar
                if (isAssigned && m_session->canManageClass(gId)) {
                    filtered << id;
                }
            } else {
                // Tüm Sınıflar modunda genel kural (yetkili sınıf + atanmamış)
                if (m_session->canManageClient(id)) {
                    filtered << id;
                }
            }
        }
        return filtered;
    }

    return rawTargets;
}

QString MainWindow::commandTargetsDescription() const {
    if (m_targetBadge && !m_targetBadge->text().trimmed().isEmpty()) {
        return m_targetBadge->text().trimmed();
    }
    return tr("Tüm İstemciler");
}

// ---------------------------------------------------------------------------
// Rubber band: thumbnailContainer üzerinde tıkla-sürükle seçim
// ---------------------------------------------------------------------------
bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Kullanıcı aktivitesi (fare hareketi, tıklama, tuş basımı) varsa son aktivite zamanını güncelle
    if (!isScreenLocked()) {
        QEvent::Type t = event->type();
        if (t == QEvent::MouseMove || t == QEvent::MouseButtonPress ||
            t == QEvent::KeyPress || t == QEvent::Wheel) {
            m_lastActivityTime = QDateTime::currentSecsSinceEpoch();
        }
    }

    if (obj == ui->thumbnailScrollArea && event->type() == QEvent::Resize) {
        // Gecikmeli relayout, widget boyutlarının tam oturduğundan emin olmak için
        QTimer::singleShot(0, this, &MainWindow::relayoutThumbnails);
        updateFloatingDiscoveryPillGeometry();
    }

    if (event->type() == QEvent::Wheel && (obj == ui->thumbnailScrollArea || obj == ui->thumbnailContainer)) {
        ClientThumbnail::hideLiveTooltip();
    }

    // Escape veya pencere inaktif olduğunda rubber band'i güvenle iptal et
    if (m_rubberBand && m_rubberBand->isVisible()) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = dynamic_cast<QKeyEvent*>(event);
            if (ke && ke->key() == Qt::Key_Escape) {
                if (ui && ui->thumbnailContainer && ui->thumbnailContainer->mouseGrabber() == ui->thumbnailContainer) {
                    ui->thumbnailContainer->releaseMouse();
                }
                m_rubberBand->hide();
                m_rubberBand->setGeometry(QRect());
                m_cachedThumbRects.clear();
                return true;
            }
        } else if (event->type() == QEvent::WindowDeactivate) {
            if (ui && ui->thumbnailContainer && ui->thumbnailContainer->mouseGrabber() == ui->thumbnailContainer) {
                ui->thumbnailContainer->releaseMouse();
            }
            m_rubberBand->hide();
            m_rubberBand->setGeometry(QRect());
            m_cachedThumbRects.clear();
        }
    }

    auto* me = dynamic_cast<QMouseEvent*>(event);

    // Rubber band başlangıç kontrolü
    if (event->type() == QEvent::MouseButtonPress && me && me->button() == Qt::LeftButton &&
        !(me->modifiers() & Qt::ControlModifier) && !(me->modifiers() & Qt::ShiftModifier)) {

        QWidget* clickedWidget = qobject_cast<QWidget*>(obj);
        if (clickedWidget && ui && ui->thumbnailContainer) {
            // Kaydırma çubuklarına tıklandıysa rubber band başlatma
            if (ui->thumbnailScrollArea &&
                (clickedWidget == ui->thumbnailScrollArea->verticalScrollBar() ||
                 clickedWidget == ui->thumbnailScrollArea->horizontalScrollBar() ||
                 clickedWidget->inherits("QScrollBar"))) {
                return QMainWindow::eventFilter(obj, event);
            }

            // Thumbnail kartına veya alt bileşenine tıklandıysa kartın kendi seçimi çalışır
            QWidget* checkThumb = clickedWidget;
            bool isThumbnailClick = false;
            while (checkThumb && checkThumb != ui->thumbnailContainer) {
                if (qobject_cast<ClientThumbnail*>(checkThumb)) {
                    isThumbnailClick = true;
                    break;
                }
                checkThumb = checkThumb->parentWidget();
            }
            if (isThumbnailClick) {
                return QMainWindow::eventFilter(obj, event);
            }

            // Grup başlığına tıklandıysa grubun kendi seçimi çalışır
            QWidget* checkHeader = clickedWidget;
            bool isHeaderClick = false;
            while (checkHeader && checkHeader != ui->thumbnailContainer) {
                if (checkHeader->property("class").toString() == "groupHeader") {
                    isHeaderClick = true;
                    break;
                }
                if (auto* frame = qobject_cast<ThumbnailGroupFrame*>(checkHeader)) {
                    if (frame->isHeaderAt(frame->mapFromGlobal(me->globalPosition().toPoint()))) {
                        isHeaderClick = true;
                        break;
                    }
                }
                checkHeader = checkHeader->parentWidget();
            }
            if (isHeaderClick) {
                return QMainWindow::eventFilter(obj, event);
            }

            // Tıklanan alan thumbnailContainer veya thumbnailScrollArea içinde mi?
            bool isInThumbnailArea = false;
            QWidget* checkArea = clickedWidget;
            while (checkArea) {
                if (checkArea == ui->thumbnailContainer ||
                    (ui->thumbnailScrollArea && checkArea == ui->thumbnailScrollArea->viewport())) {
                    isInThumbnailArea = true;
                    break;
                }
                checkArea = checkArea->parentWidget();
            }

            if (isInThumbnailArea) {
                ClientThumbnail::hideLiveTooltip();
                m_rubberOrigin = ui->thumbnailContainer->mapFromGlobal(me->globalPosition().toPoint());
                m_rubberBand->setGeometry(QRect(m_rubberOrigin, QSize()));
                m_rubberBand->show();
                m_rubberBand->raise();

                // Tüm fare olaylarını container'a kilitle (kesintisiz sürükleme)
                ui->thumbnailContainer->grabMouse();

                // O(1) sürükleme performansı: Görünür thumbnail koordinatlarını tek seferde önbelleğe al
                m_cachedThumbRects.clear();
                for (auto* t : m_thumbnails) {
                    if (t && t->isVisible()) {
                        QRect thumbRect(t->mapTo(ui->thumbnailContainer, QPoint(0, 0)), t->size());
                        m_cachedThumbRects.append({t, thumbRect});
                    }
                }
                return true;
            }
        }
    }

    // Sürükleme hareketi
    if (event->type() == QEvent::MouseMove && me) {
        if (m_rubberBand && m_rubberBand->isVisible() && ui && ui->thumbnailContainer) {
            QPoint containerPos = ui->thumbnailContainer->mapFromGlobal(me->globalPosition().toPoint());

            QRect rect = QRect(m_rubberOrigin, containerPos).normalized();
            m_rubberBand->setGeometry(rect);

            // Önbellek üzerinden tamsayı dikdörtgen kesişimi (widget mapTo gerektirmez, sıfır CPU yükü)
            for (const auto& pair : m_cachedThumbRects) {
                if (pair.first) {
                    pair.first->setSelected(rect.intersects(pair.second));
                }
            }
            return true;
        }
    }

    // Sürüklemeyi tamamlama
    if (event->type() == QEvent::MouseButtonRelease && me) {
        if (m_rubberBand && m_rubberBand->isVisible()) {
            if (ui && ui->thumbnailContainer && ui->thumbnailContainer->mouseGrabber() == ui->thumbnailContainer) {
                ui->thumbnailContainer->releaseMouse();
            }
            m_rubberBand->hide();
            m_rubberBand->setGeometry(QRect());

            QPoint containerPos = (ui && ui->thumbnailContainer)
                ? ui->thumbnailContainer->mapFromGlobal(me->globalPosition().toPoint())
                : m_rubberOrigin;

            m_cachedThumbRects.clear();

            // Çok küçük sürükleme (tek tıklama): seçimi temizle
            QRect finalRect = QRect(m_rubberOrigin, containerPos).normalized();
            if (finalRect.width() < 5 && finalRect.height() < 5) {
                if (!(me->modifiers() & Qt::ControlModifier)) {
                    for (auto* t : m_thumbnails) {
                        if (t) t->setSelected(false);
                    }
                }
            }

            // Son Seçim Önceliği:
            QStringList curSel = selectedClientIds();
            if (curSel.isEmpty()) {
                m_selectionSource = (currentClassScopeNode()) ? SelectionSource::Tree : SelectionSource::None;
            } else {
                m_selectionSource = SelectionSource::Thumbnail;
            }

            updateLockToggleState();
            updateSelectionTargetBadge();
            return true;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

// ============================================================================
// Dock yardımcıları
// ============================================================================

// updateHandRaiseBadge / updateRoomList — ChatRoomManager'a taşındı

// (filterTreeItem → TreeManager::filterItem'a taşındı)

// ============================================================================
// SSH ile agent yeniden başlatma
// ============================================================================
void MainWindow::onClientAgentRestart(const QString& clientId) {
    if (!m_session) return;

    const ClientState state = m_session->clientState(clientId);
    const QString     host  = state.hostname;
    const QString     ip    = state.ip;
    const QString     machineId = state.machineId;

    bool foundCred = false;
    QString sshUser;
    QString sshSecretStr;
    bool sshIsKey = false;

    const auto brokerCreds = m_session->sshCredentials();
    for (const auto& c : brokerCreds) {
        if (c.targetId == ip && c.targetType == "agent") {
            sshUser = c.username; sshSecretStr = QString::fromUtf8(c.secret); sshIsKey = c.isKey; foundCred = true; break;
        }
    }
    if (!foundCred && !machineId.isEmpty()) {
        for (const auto& c : brokerCreds) {
            if (c.targetId == machineId && c.targetType == "agent") {
                sshUser = c.username; sshSecretStr = QString::fromUtf8(c.secret); sshIsKey = c.isKey; foundCred = true; break;
            }
        }
    }
    if (!foundCred && !machineId.isEmpty()) {
        QString classId = m_session->clientGroup(machineId);
        if (!classId.isEmpty()) {
            for (const auto& c : brokerCreds) {
                if (c.targetId == classId && c.targetType == "class") {
                    sshUser = c.username; sshSecretStr = QString::fromUtf8(c.secret); sshIsKey = c.isKey; foundCred = true; break;
                }
            }
        }
    }

    QString sshHost = ip.isEmpty() ? host : ip;
    QString user = "root";
    SshClient::AuthMethod authMethod = SshClient::AuthMethod::Password;
    QString passStr = "";
    QString privKeyStr = "";

    if (foundCred) {
        user = sshUser;
        if (sshIsKey) {
            authMethod = SshClient::AuthMethod::PrivateKey;
            privKeyStr = sshSecretStr;
        } else {
            authMethod = SshClient::AuthMethod::Password;
            passStr = sshSecretStr;
        }
    } else {
        QSettings s("okidia", "master");
        const QString lastUser = s.value("users/" + m_activeUser.username + "/ssh/agent_restart_user", "root").toString();

        // ── Kimlik bilgileri iletişim kutusu ─────────────────────────────────────
        QDialog dlg(this);
        dlg.setWindowTitle(tr("SSH — İstemci'yi Yeniden Başlat"));
        dlg.setFixedWidth(340);
        auto* lay = new QVBoxLayout(&dlg);
        lay->setSpacing(8);

        auto* hostLabel = new QLabel(QString("<b>Hedef:</b> %1").arg(host), &dlg);
        lay->addWidget(hostLabel);

        lay->addWidget(new QLabel(tr("SSH Adresi (boş → hostname):"), &dlg));
        auto* hostEdit = new QLineEdit(&dlg);
        hostEdit->setPlaceholderText(host);
        lay->addWidget(hostEdit);

        lay->addWidget(new QLabel(tr("Kullanıcı:"), &dlg));
        auto* userEdit = new QLineEdit(lastUser, &dlg);
        lay->addWidget(userEdit);

        lay->addWidget(new QLabel(tr("Parola:"), &dlg));
        auto* passEdit = new QLineEdit(&dlg);
        passEdit->setEchoMode(QLineEdit::Password);
        passEdit->setPlaceholderText(tr("Boş → SSH-agent / anahtar denenir"));
        lay->addWidget(passEdit);

        const bool isTargetWindows = state.osTag.toLower().contains("windows") || state.osPrettyName.toLower().contains("windows");
        auto* hint = new QLabel(
            isTargetWindows
                ? tr("<small style='color:gray'>powershell -Command \"Restart-Service OkidiaAgent\" çalıştırılır</small>")
                : tr("<small style='color:gray'>systemctl restart okidia-agent çalıştırılır</small>"),
            &dlg);
        hint->setTextFormat(Qt::RichText);
        lay->addWidget(hint);

        auto* bbox = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        lay->addWidget(bbox);
        connect(bbox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bbox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() != QDialog::Accepted) return;

        sshHost = hostEdit->text().trimmed().isEmpty()
                                    ? host
                                    : hostEdit->text().trimmed();
        user    = userEdit->text().trimmed().isEmpty()
                                    ? QStringLiteral("root")
                                    : userEdit->text().trimmed();
        s.setValue("users/" + m_activeUser.username + "/ssh/agent_restart_user", user);

        if (passEdit->text().isEmpty()) {
            authMethod = SshClient::AuthMethod::Agent;
        } else {
            authMethod = SshClient::AuthMethod::Password;
        }
    }

    // ── SshClient ──────────────────────────────────────────────────────────
    auto* ssh = new SshClient(this);

    SshClient::Config cfg;
    cfg.host         = sshHost;
    cfg.port         = 22;
    cfg.username     = user;
    cfg.hostKeyCheck = false;   // okul ortamı — CA doğrulaması yok

    cfg.authMethod = authMethod;
    if (authMethod == SshClient::AuthMethod::Password) {
        cfg.password = passStr;
    } else if (authMethod == SshClient::AuthMethod::PrivateKey) {
        cfg.privateKeyPath = privKeyStr;
    }

    const bool isTargetWindows = state.osTag.toLower().contains("windows") || state.osPrettyName.toLower().contains("windows");
    QString restartCmd;
    if (isTargetWindows) {
        restartCmd = "powershell -Command \"Restart-Service OkidiaAgent\"";
    } else {
        if (user == "root") {
            restartCmd = "systemctl restart okidia-agent";
        } else if (authMethod == SshClient::AuthMethod::Password && !passStr.isEmpty()) {
            QString escPass = passStr;
            escPass.replace("'", "'\\''");
            restartCmd = QString("echo '%1' | sudo -S systemctl restart okidia-agent").arg(escPass);
        } else {
            restartCmd = "sudo systemctl restart okidia-agent";
        }
    }

    showCommandStatus(true, QString(tr("SSH bağlanıyor → %1…")).arg(sshHost));

    connect(ssh, &SshClient::connected, this, [this, ssh, sshHost, restartCmd]() {
        showCommandStatus(true,
            QString(tr("SSH bağlandı → %1, komut gönderiliyor…")).arg(sshHost));
        const int chId = ssh->execute(restartCmd);

        connect(ssh, &SshClient::commandFinished, this,
            [this, ssh, sshHost, chId](int id, int exitCode) {
                if (id != chId) return;
                if (exitCode == 0)
                    showCommandStatus(true,
                        QString(tr("İstemci yeniden başlatıldı: %1")).arg(sshHost));
                else
                    showCommandStatus(false,
                        QString(tr("İstemci restart başarısız (exit %1): %2"))
                            .arg(exitCode).arg(sshHost));
                ssh->disconnectFromHost();
                ssh->deleteLater();
            });

        connect(ssh, &SshClient::commandError, this,
            [this, ssh, sshHost, chId](int id, const QByteArray& data) {
                if (id != chId) return;
                showCommandStatus(false,
                    QString("SSH stderr [%1]: %2")
                        .arg(sshHost, QString::fromUtf8(data).trimmed()));
                ssh->disconnectFromHost();
                ssh->deleteLater();
            });
    });

    connect(ssh, &SshClient::errorOccurred, this,
        [this, ssh](const QString& msg) {
            showCommandStatus(false, "SSH Hata: " + msg);
            ssh->deleteLater();
        });

    connect(ssh, &SshClient::authenticationFailed, this,
        [this, ssh](const QString& msg) {
            showCommandStatus(false, tr("SSH Kimlik hatası: ") + msg);
            ssh->deleteLater();
        });

    ssh->connectToHost(cfg);
}

void MainWindow::onClientRevoke(const QString& clientId)
{
    if (!m_session) return;

    const auto& st = m_session->clientState(clientId);
    const QString name = st.hostname.isEmpty() ? clientId : st.hostname;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("İstemciyi Sistemden Çıkar"));
    msgBox.setText(QString(tr("'%1' adlı istemci sistemden çıkarılacak.\n\n"
                              "İstemcinin sertifikası iptal edilir; tekrar sisteme "
                              "katılmak için yeniden onay gerekir.\n\n"
                              "Devam etmek istiyor musunuz?")).arg(name));
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);
    
    QCheckBox* cbUninstall = new QCheckBox(tr("Aynı zamanda uzak bilgisayardan tamamen sil (Uninstall)"));
    cbUninstall->setChecked(false); // Varsayılan olarak false (güvenlik için)
    msgBox.setCheckBox(cbUninstall);

    if (msgBox.exec() != QMessageBox::Yes) return;
    bool uninstall = cbUninstall->isChecked();

    m_session->revokeClient(clientId, uninstall);
    {
        AuditEvent e;
        e.category = AuditEvent::Security;
        e.target   = clientId;
        e.detail   = tr("İstemci sistemden çıkarıldı: ") + name;
        AuditLog::instance().record(e);
    }

    // Thumbnail'i kaldır
    // Önce grup frame'lerinden çıkar (dangling pointer crash'ini önlemek için)
    for (auto* frame : m_groupFrames) {
        if (frame->thumbnail(clientId)) {
            frame->removeThumbnail(clientId);
        }
    }

    if (m_thumbnails.contains(clientId)) {
        ClientThumbnail* thumb = m_thumbnails.take(clientId);
        thumb->deleteLater();
        updateStatusBar(m_thumbnails.keys());
    }
    if (m_treeManager) m_treeManager->removeClient(clientId);

    showCommandStatus(true, name + tr(" sistemden çıkarıldı"));
}

void MainWindow::onManageClients()
{
    if (m_clientMgmtDialog) {
        if (m_clientMgmtDialog->isMinimized()) m_clientMgmtDialog->showNormal();
        m_clientMgmtDialog->raise();
        m_clientMgmtDialog->activateWindow();
        return;
    }

    auto* dlg = new QDialog(nullptr, Qt::Window);
    m_clientMgmtDialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("İstemci Yönetimi"));
    dlg->setWindowIcon(ThemeManager::icon(":/ui/settings.svg"));
    dlg->resize(900, 520);
    dlg->setMinimumSize(750, 420);
    dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* mainLay = new QVBoxLayout(dlg);
    mainLay->setContentsMargins(16, 14, 16, 14);
    mainLay->setSpacing(10);

    // Tab widget — iki sekme: Kayıtlı İstemciler ve Kara Liste
    auto* tabs = new QTabWidget(dlg);
    mainLay->addWidget(tabs, 1);

    // ── Tab 1: Kayıtlı İstemciler ──────────────────────────────────────────
    auto* tab1 = new QWidget;
    auto* lay1 = new QVBoxLayout(tab1);
    lay1->setContentsMargins(12, 12, 12, 12);
    lay1->setSpacing(8);

    auto* lbl1 = new QLabel(tr("Sisteme kayıtlı istemciler. Seçili istemcileri sistemden çıkarabilir veya tümünü sıfırlayabilirsiniz."), tab1);
    lbl1->setWordWrap(true);
    lbl1->setProperty("status", "info");
    lay1->addWidget(lbl1);

    auto* clientTable = new QTableWidget(0, 7, tab1);
    clientTable->setHorizontalHeaderLabels({
        tr("Durum"),
        tr("Bilgisayar Adı"),
        tr("Kullanıcı"),
        tr("İstemci Tipi"),
        tr("İşletim Sistemi"),
        tr("IP Adresi"),
        tr("MAC Adresi")
    });
    clientTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    clientTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    clientTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    clientTable->setAlternatingRowColors(true);
    clientTable->verticalHeader()->setVisible(false);
    clientTable->setShowGrid(false);
    OkidiaUtils::setupTableHeaders(clientTable, "manage_clients_registered_table", {95, 150, 100, 120, 140, 115, 130}, true);
    clientTable->setSortingEnabled(true);
    clientTable->sortByColumn(1, Qt::AscendingOrder);
    lay1->addWidget(clientTable, 1);

    const auto states = m_session ? m_session->allClientStates() : QList<ClientState>{};
    for (const auto& st : states) {
        int r = clientTable->rowCount();
        clientTable->insertRow(r);

        // Col 0: Durum
        auto* statusItem = new OkidiaTableItem(
            ThemeManager::icon(st.connected ? ":/ui/online.svg" : ":/ui/offline.svg"),
            st.connected ? tr("Bağlı") : tr("Çevrimdışı")
        );
        statusItem->setData(Qt::UserRole, st.clientId);
        statusItem->setData(Qt::UserRole + 2, st.machineId);
        statusItem->setTextAlignment(Qt::AlignCenter);

        // Col 1: Bilgisayar Adı
        QString hostDisplay = st.hostname.isEmpty() ? st.clientId : st.hostname;
        auto* hostItem = new OkidiaTableItem(ThemeManager::icon(":/ui/monitor.svg"), hostDisplay);
        hostItem->setData(Qt::UserRole, st.clientId);
        hostItem->setData(Qt::UserRole + 2, st.machineId);
        hostItem->setToolTip("Machine-ID: " + st.machineId);

        // Col 2: Kullanıcı
        auto* userItem = new OkidiaTableItem(st.username.isEmpty() ? "-" : st.username);

        // Col 3: İstemci Tipi
        QString typeStr = tr("Standart");
        QIcon typeIcon = ThemeManager::icon(":/ui/monitor.svg");
        if (st.agentType == 1) {
            typeStr = tr("Öğrenci PC");
            typeIcon = ThemeManager::icon(":/ui/student.svg");
        } else if (st.agentType == 2) {
            typeStr = tr("İdari PC");
            typeIcon = ThemeManager::icon(":/ui/admin.svg");
        } else if (st.agentType == 3) {
            typeStr = tr("Akıllı Tahta");
            typeIcon = ThemeManager::icon(":/ui/smartboard.svg");
        } else if (st.agentType >= 100) {
            typeStr = OkidiaDatabase::instance().customProfile(st.agentType).name;
            if (typeStr.isEmpty()) typeStr = tr("Özel Profil (%1)").arg(st.agentType);
            typeIcon = ThemeManager::icon(":/ui/custom_profile.svg");
        }
        auto* typeItem = new OkidiaTableItem(typeIcon, typeStr);

        // Col 4: İşletim Sistemi
        QString osStr = st.osPrettyName.isEmpty() ? st.osTag : st.osPrettyName;
        QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
        if (osStr.contains("win", Qt::CaseInsensitive)) {
            osIcon = ThemeManager::icon(":/ui/windows.svg");
        } else if (osStr.contains("linux", Qt::CaseInsensitive) || osStr.contains("ubuntu", Qt::CaseInsensitive) ||
                   osStr.contains("debian", Qt::CaseInsensitive) || osStr.contains("pardus", Qt::CaseInsensitive) ||
                   osStr.contains("raspberry", Qt::CaseInsensitive)) {
            osIcon = ThemeManager::icon(":/ui/linux.svg");
        }
        auto* osItem = new OkidiaTableItem(osIcon, osStr.isEmpty() ? "-" : osStr);

        // Col 5: IP Adresi
        auto* ipItem = new OkidiaTableItem(st.ip.isEmpty() ? "-" : st.ip);
        QHostAddress addr(st.ip);
        if (!addr.isNull() && addr.protocol() == QAbstractSocket::IPv4Protocol) {
            ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(addr.toIPv4Address()));
        }

        // Col 6: MAC Adresi
        auto* macItem = new OkidiaTableItem(st.mac.isEmpty() ? "-" : st.mac);

        clientTable->setItem(r, 0, statusItem);
        clientTable->setItem(r, 1, hostItem);
        clientTable->setItem(r, 2, userItem);
        clientTable->setItem(r, 3, typeItem);
        clientTable->setItem(r, 4, osItem);
        clientTable->setItem(r, 5, ipItem);
        clientTable->setItem(r, 6, macItem);
    }

    auto* row1 = new QHBoxLayout();
    auto* btnChangeType = new QPushButton(tr("İstemci Tipi Değiştir"), tab1);
    btnChangeType->setIcon(ThemeManager::icon(":/ui/settings.svg"));
    btnChangeType->setEnabled(false);
    btnChangeType->setToolTip(tr("Seçili istemcilerin tipini değiştirir (Öğrenci PC / İdari PC / Akıllı Tahta)"));

    auto* btnRevoke = new QPushButton(tr("Seçiliyi Çıkar"), tab1);
    btnRevoke->setIcon(ThemeManager::icon(":/ui/trash.svg"));
    btnRevoke->setEnabled(false);

    auto* btnReset  = new QPushButton(tr("Tümünü Sıfırla"), tab1);
    btnReset->setIcon(ThemeManager::icon(":/ui/warning.svg"));

    row1->addWidget(btnChangeType);
    row1->addWidget(btnRevoke);
    row1->addWidget(btnReset);
    row1->addStretch();
    lay1->addLayout(row1);

    // Sağ tık context menüsü — hızlı tip değiştirme
    clientTable->setContextMenuPolicy(Qt::CustomContextMenu);

    // Lambda: Seçili satırların tipini değiştir (buton ve context menüsü ortak)
    auto changeTypeForSelected = [this, clientTable]() {
        const auto selectedIndices = clientTable->selectionModel()->selectedRows();
        if (selectedIndices.isEmpty()) return;

        QStringList types = {tr("Öğrenci PC"), tr("İdari PC (Gizlilik)"), tr("Akıllı Tahta")};
        QList<uint32_t> typeIds = {1, 2, 3};
        const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
        for (const auto& cp : customProfiles) {
            types.append(cp.name);
            typeIds.append(static_cast<uint32_t>(cp.id));
        }

        bool ok;
        QString selected = QInputDialog::getItem(clientTable, tr("İstemci Tipi Değiştir"),
                                                 tr("Seçili istemciler için yeni tip:"),
                                                 types, 0, false, &ok);
        if (!ok) return;

        int selIdx = types.indexOf(selected);
        if (selIdx < 0) return;
        uint32_t newType = typeIds[selIdx];

        if (newType == 2) {
            int ret = QMessageBox::warning(clientTable, tr("İdari PC Yapılandırması"),
                tr("Seçili istemcileri 'İdari PC' olarak değiştirmek istediğinize emin misiniz?\n\n"
                   "ÖNEMLİ: İdari PC'ler gizlilik odaklıdır. Tekrar Öğrenci PC veya "
                   "Akıllı Tahta tipine geri dönmek için hedef bilgisayardan (İstemci) "
                   "fiziksel onay alınması gerekecektir."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes) return;
        }

        QIcon newTypeIcon = ThemeManager::icon(":/ui/monitor.svg");
        if (newType == 1) newTypeIcon = ThemeManager::icon(":/ui/student.svg");
        else if (newType == 2) newTypeIcon = ThemeManager::icon(":/ui/admin.svg");
        else if (newType == 3) newTypeIcon = ThemeManager::icon(":/ui/smartboard.svg");
        else if (newType >= 100) newTypeIcon = ThemeManager::icon(":/ui/custom_profile.svg");

        for (const auto& idx : selectedIndices) {
            auto* item0 = clientTable->item(idx.row(), 0);
            if (!item0) continue;
            QString clientId = item0->data(Qt::UserRole).toString();

            if (m_session) {
                bool success = m_session->sendAgentTypeSet(clientId, newType);
                if (success) {
                    // Tablodaki tip sütununu (Col 3) güncelle
                    auto* typeItem = clientTable->item(idx.row(), 3);
                    if (typeItem) {
                        typeItem->setText(selected.contains(tr("Gizlilik")) ? tr("İdari PC") : selected);
                        typeItem->setIcon(newTypeIcon);
                    }
                    AuditLog::instance().recordSecurity(
                        m_activeUser.username, clientId,
                        tr("İstemci tipi değiştirildi → ") + selected);
                }
            }
        }
        showCommandStatus(true, tr("İstemci tipi güncellendi: ") + selected);
    };

    connect(btnChangeType, &QPushButton::clicked, changeTypeForSelected);

    connect(clientTable, &QWidget::customContextMenuRequested, [this, clientTable, changeTypeForSelected](const QPoint& pos) {
        if (clientTable->selectionModel()->selectedRows().isEmpty()) return;

        QMenu menu(clientTable);
        auto* actType = menu.addAction(ThemeManager::icon(":/ui/settings.svg"), tr("İstemci Tipi Değiştir"));
        menu.addSeparator();
        auto* actRevoke = menu.addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Seçiliyi Çıkar"));

        QAction* chosen = menu.exec(clientTable->viewport()->mapToGlobal(pos));
        if (chosen == actType) {
            changeTypeForSelected();
        } else if (chosen == actRevoke) {
            // Revoke butonunun click sinyalini tetikle
            if (auto* btn = clientTable->parentWidget()->findChild<QPushButton*>()) {
                // Direkt revoke mantığını çalıştır
                const auto selectedIndices = clientTable->selectionModel()->selectedRows();
                QStringList clientIdsToRevoke;
                for (const auto& idx : selectedIndices) {
                    if (auto* item0 = clientTable->item(idx.row(), 0)) {
                        clientIdsToRevoke.append(item0->data(Qt::UserRole).toString());
                    }
                }
                for (const QString& cid : clientIdsToRevoke) {
                    onClientRevoke(cid);
                    for (int r = clientTable->rowCount() - 1; r >= 0; --r) {
                        if (clientTable->item(r, 0) && clientTable->item(r, 0)->data(Qt::UserRole).toString() == cid) {
                            clientTable->removeRow(r);
                            break;
                        }
                    }
                }
            }
        }
    });

    connect(clientTable, &QTableWidget::itemSelectionChanged, [clientTable, btnRevoke, btnChangeType]() {
        const bool hasSel = !clientTable->selectionModel()->selectedRows().isEmpty();
        btnRevoke->setEnabled(hasSel);
        btnChangeType->setEnabled(hasSel);
    });

    connect(clientTable, &QTableWidget::itemDoubleClicked, [btnChangeType](QTableWidgetItem*) {
        if (btnChangeType->isEnabled()) {
            btnChangeType->click();
        }
    });

    connect(btnRevoke, &QPushButton::clicked, [this, clientTable]() {
        const auto selectedIndices = clientTable->selectionModel()->selectedRows();
        QStringList clientIdsToRevoke;
        for (const auto& idx : selectedIndices) {
            if (auto* item0 = clientTable->item(idx.row(), 0)) {
                clientIdsToRevoke.append(item0->data(Qt::UserRole).toString());
            }
        }
        for (const QString& cid : clientIdsToRevoke) {
            onClientRevoke(cid);
            for (int r = clientTable->rowCount() - 1; r >= 0; --r) {
                if (clientTable->item(r, 0) && clientTable->item(r, 0)->data(Qt::UserRole).toString() == cid) {
                    clientTable->removeRow(r);
                    break;
                }
            }
        }
    });

    connect(btnReset, &QPushButton::clicked, [this, clientTable, dlg]() {
        auto ret = QMessageBox::warning(dlg, tr("Tüm İstemcileri Sıfırla"),
            tr("Tüm kayıtlı istemciler sistemden çıkarılacak.\nEmin misiniz?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
        if (m_session) m_session->resetAllClients();
        for (auto it = m_thumbnails.begin(); it != m_thumbnails.end(); ++it)
            it.value()->deleteLater();
        m_thumbnails.clear();
        updateStatusBar({});
        clientTable->setRowCount(0);
        showCommandStatus(true, tr("Tüm istemciler sıfırlandı"));
    });

    tabs->addTab(tab1, ThemeManager::icon(":/ui/student.svg"), tr("Kayıtlı İstemciler"));

    // ── Tab 2: Kara Liste (Yoksayılanlar) ─────────────────────────────────
    auto* tab2 = new QWidget;
    auto* lay2 = new QVBoxLayout(tab2);
    lay2->setContentsMargins(12, 12, 12, 12);
    lay2->setSpacing(8);

    auto* lbl2 = new QLabel(
        tr("Yoksayılan bilgisayarlar — bu listede olanların bağlantı istekleri otomatik olarak reddedilir.\n"
           "Listeden çıkarmak için seçip 'Kara Listeden Çıkar' butonuna tıklayın."),
        tab2);
    lbl2->setWordWrap(true);
    lbl2->setProperty("status", "info");
    lay2->addWidget(lbl2);

    auto* ignoreTable = new QTableWidget(0, 6, tab2);
    ignoreTable->setHorizontalHeaderLabels({
        tr("Bilgisayar Adı"),
        tr("Donanım Kimliği (Machine-ID)"),
        tr("IP Adresi"),
        tr("MAC Adresi"),
        tr("İşletim Sistemi"),
        tr("Engellenme Tarihi")
    });
    ignoreTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ignoreTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ignoreTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ignoreTable->setAlternatingRowColors(true);
    ignoreTable->verticalHeader()->setVisible(false);
    ignoreTable->setShowGrid(false);
    OkidiaUtils::setupTableHeaders(ignoreTable, "manage_clients_blacklist_table", {150, 200, 110, 130, 140, 130}, true);
    ignoreTable->setSortingEnabled(true);
    ignoreTable->sortByColumn(0, Qt::AscendingOrder);
    lay2->addWidget(ignoreTable, 1);

    // Kara listeyi birleştirilmiş kayıtlardan doldur
    QMap<QString, AgentRecord> blacklistedMap;
    for (const auto& rec : OkidiaDatabase::instance().blacklist()) {
        blacklistedMap[rec.machineId] = rec;
    }
    if (m_session) {
        for (const QString& mid : m_session->ignoredMachineIds()) {
            if (!blacklistedMap.contains(mid)) {
                AgentRecord dummy;
                dummy.machineId = mid;
                dummy.hostname = mid;
                blacklistedMap[mid] = dummy;
            }
        }
    }

    for (const auto& rec : blacklistedMap) {
        int r = ignoreTable->rowCount();
        ignoreTable->insertRow(r);

        QString hostDisplay = rec.hostname.isEmpty() ? rec.machineId : rec.hostname;
        auto* hostItem = new OkidiaTableItem(ThemeManager::icon(":/ui/forbidden.svg"), hostDisplay);
        hostItem->setData(Qt::UserRole, rec.machineId);

        auto* midItem = new OkidiaTableItem(rec.machineId);
        midItem->setData(Qt::UserRole, rec.machineId);

        auto* ipItem = new OkidiaTableItem(rec.ip.isEmpty() ? "-" : rec.ip);
        QHostAddress addr(rec.ip);
        if (!addr.isNull() && addr.protocol() == QAbstractSocket::IPv4Protocol) {
            ipItem->setData(Qt::UserRole + 1, static_cast<quint64>(addr.toIPv4Address()));
        }

        auto* macItem = new OkidiaTableItem(rec.mac.isEmpty() ? "-" : rec.mac);

        QString osStr = rec.osPrettyName.isEmpty() ? rec.osInfo : rec.osPrettyName;
        QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
        if (osStr.contains("win", Qt::CaseInsensitive)) {
            osIcon = ThemeManager::icon(":/ui/windows.svg");
        } else if (osStr.contains("linux", Qt::CaseInsensitive) || osStr.contains("ubuntu", Qt::CaseInsensitive) ||
                   osStr.contains("debian", Qt::CaseInsensitive) || osStr.contains("pardus", Qt::CaseInsensitive) ||
                   osStr.contains("raspberry", Qt::CaseInsensitive)) {
            osIcon = ThemeManager::icon(":/ui/linux.svg");
        }
        auto* osItem = new OkidiaTableItem(osIcon, osStr.isEmpty() ? "-" : osStr);

        qint64 ts = (rec.approvedAt > 0) ? rec.approvedAt : rec.lastSeen;
        QString dateStr = (ts > 0) ? QDateTime::fromSecsSinceEpoch(ts).toString("dd.MM.yyyy HH:mm") : "-";
        auto* dateItem = new OkidiaTableItem(dateStr);
        if (ts > 0) {
            dateItem->setData(Qt::UserRole + 1, static_cast<quint64>(ts));
        }

        ignoreTable->setItem(r, 0, hostItem);
        ignoreTable->setItem(r, 1, midItem);
        ignoreTable->setItem(r, 2, ipItem);
        ignoreTable->setItem(r, 3, macItem);
        ignoreTable->setItem(r, 4, osItem);
        ignoreTable->setItem(r, 5, dateItem);
    }

    auto* row2 = new QHBoxLayout();
    auto* btnUnblock = new QPushButton(tr("Kara Listeden Çıkar"), tab2);
    btnUnblock->setIcon(ThemeManager::icon(":/ui/unlock.svg"));
    btnUnblock->setEnabled(false);
    row2->addWidget(btnUnblock);
    row2->addStretch();
    lay2->addLayout(row2);

    connect(ignoreTable, &QTableWidget::itemSelectionChanged, [ignoreTable, btnUnblock]() {
        btnUnblock->setEnabled(!ignoreTable->selectionModel()->selectedRows().isEmpty());
    });

    connect(ignoreTable, &QTableWidget::itemDoubleClicked, [btnUnblock](QTableWidgetItem*) {
        if (btnUnblock->isEnabled()) {
            btnUnblock->click();
        }
    });

    connect(btnUnblock, &QPushButton::clicked, [this, ignoreTable]() {
        const auto selectedIndices = ignoreTable->selectionModel()->selectedRows();
        QStringList toUnblock;
        for (const auto& idx : selectedIndices) {
            if (auto* item0 = ignoreTable->item(idx.row(), 0)) {
                toUnblock.append(item0->data(Qt::UserRole).toString());
            }
        }
        for (const QString& mid : toUnblock) {
            if (m_session)
                m_session->removeFromIgnoreList(mid);
            for (int r = ignoreTable->rowCount() - 1; r >= 0; --r) {
                if (ignoreTable->item(r, 0) && ignoreTable->item(r, 0)->data(Qt::UserRole).toString() == mid) {
                    ignoreTable->removeRow(r);
                    break;
                }
            }
        }
        showCommandStatus(true, tr("Kara listeden çıkarıldı"));
    });

    tabs->addTab(tab2, ThemeManager::icon(":/ui/forbidden.svg"), tr("Kara Liste"));

    // ── Alt buton: Kapat ───────────────────────────────────────────────────
    auto* botRow = new QHBoxLayout();
    auto* btnClose = new QPushButton(tr("Kapat"), dlg);
    connect(btnClose, &QPushButton::clicked, dlg, &QDialog::accept);
    botRow->addStretch();
    botRow->addWidget(btnClose);
    mainLay->addLayout(botRow);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::checkDefaultUninstallPasswordWarning() {
    if (m_activeUser.role != "YM") return;
    if (m_session && (m_session->isOfflineMode() || !m_session->isConnected())) return;

    if (OkidiaDatabase::instance().isDefaultUninstallPassword()) {
        QMessageBox msg(this);
        msg.setIcon(QMessageBox::Warning);
        msg.setWindowTitle(tr("Güvenlik Uyarısı — OKIDIA"));
        msg.setText(tr("<h3>İstemci Kaldırma Parolası Varsayılan Değerde!</h3>"
                       "Windows istemcilerinde OKIDIA İstemcisi'nin yetkisiz kaldırılmasını önleyen koruma parolası "
                       "şu anda varsayılan <b>'www.okidia.net'</b> olarak ayarlıdır.<br><br>"
                       "Öğrencilerin veya yetkisiz kişilerin istemciyi kaldırmasını önlemek için "
                       "lütfen bu parolayı yeni ve güvenli bir parola ile değiştirin."));
        QPushButton* btnChange = msg.addButton(tr("Şimdi Değiştir"), QMessageBox::AcceptRole);
        btnChange->setIcon(ThemeManager::icon(":/ui/shield.svg"));
        QPushButton* btnLater = msg.addButton(tr("Daha Sonra"), QMessageBox::RejectRole);
        btnLater->setIcon(ThemeManager::icon(":/ui/clock.svg"));
        msg.setDefaultButton(btnChange);
        msg.exec();

        if (msg.clickedButton() == btnChange) {
            onChangeUninstallPassword();
        }
    }
}

void MainWindow::onChangeUninstallPassword() {
    if (m_activeUser.role != "YM") {
        QMessageBox::warning(this, tr("Yetkisiz İşlem"), tr("Bu işlemi sadece Yönetici Master (YM) yapabilir."));
        return;
    }

    if (m_session && (m_session->isOfflineMode() || !m_session->isConnected())) {
        QMessageBox::warning(this, tr("Sunucu Bağlantısı Yok"),
                             tr("Çevrimdışı SSH modunda sunucu bağlantısı olmadığı için Windows istemci kaldırma parolası değiştirilemez ve istemcilere dağıtılamaz."));
        return;
    }

    UninstallPasswordDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        QString hash = dlg.passwordHash();
        if (hash.isEmpty()) return;

        // Yerel veritabanını güncelle
        if (OkidiaDatabase::instance().isOpen()) {
            OkidiaDatabase::instance().setUninstallPasswordHash(hash);
        }

        // Broker'a gönder (Broker tüm bağlı Windows istemcilerine anında dağıtır)
        if (m_session) {
            m_session->sendAgentUninstallPassword(hash);
        }

        if (dlg.isResetToDefault()) {
            QMessageBox::information(
                this,
                tr("Parola Sıfırlandı"),
                tr("İstemci kaldırma parolası varsayılan ('www.okidia.net') değerine sıfırlandı."));
        } else {
            QMessageBox::information(
                this,
                tr("Parola Güncellendi"),
                tr("İstemci kaldırma parolası başarıyla güncellendi ve tüm Windows istemcilerine dağıtıldı."));
        }
    }
}

// ---------------------------------------------------------------------------
// Oturum Güvenliği ve Kilit Ekranı (Idle Lock & Session Timeout)
// ---------------------------------------------------------------------------
#include "../Widgets/MasterLockOverlay.h"

void MainWindow::setupSessionSecurity() {
    m_lastActivityTime = QDateTime::currentSecsSinceEpoch();

    // Ayarları oku
    QSettings s("okidia", "master");
    m_idleTimeoutMinutes = s.value("sec/idleTimeoutMinutes", 15).toInt();
    m_maxSessionHours = s.value("sec/maxSessionHours", 8).toInt();

    // 1. Boşta Kalma Zamanlayıcısı (Her 10 saniyede bir kontrol eder)
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        connect(m_idleTimer, &QTimer::timeout, this, [this]() {
            if (isScreenLocked() || m_idleTimeoutMinutes <= 0) return;

            qint64 now = QDateTime::currentSecsSinceEpoch();
            qint64 idleSecs = now - m_lastActivityTime;
            if (idleSecs >= (m_idleTimeoutMinutes * 60)) {
                Logger::info(QObject::tr("MainWindow: %1 dakika hareketsizlik algılandı, ekran kilitleniyor.")
                             .arg(m_idleTimeoutMinutes));
                lockScreen();
            }
        });
        m_idleTimer->start(10000); // 10 saniyede bir kontrol
    }

    // 2. Azami Oturum Süresi Zamanlayıcısı
    if (!m_maxSessionTimer) {
        m_maxSessionTimer = new QTimer(this);
        connect(m_maxSessionTimer, &QTimer::timeout, this, [this]() {
            if (m_maxSessionHours <= 0) return;

            Logger::info(QObject::tr("MainWindow: Azami oturum süresi doldu (%1 saat), oturum kapatılıyor.")
                         .arg(m_maxSessionHours));
            QMessageBox::information(this, tr("Oturum Süresi Doldu"),
                tr("Güvenlik politikası gereği azami oturum süreniz (%1 saat) dolmuştur.\n\nLütfen tekrar giriş yapınız.")
                .arg(m_maxSessionHours));
            emit logoutRequested();
        });

        if (m_maxSessionHours > 0) {
            m_maxSessionTimer->start(m_maxSessionHours * 3600 * 1000);
        }
    }
}

void MainWindow::applySessionTimeoutSettings(int idleMinutes, int maxHours) {
    m_idleTimeoutMinutes = idleMinutes;
    m_maxSessionHours = maxHours;
    m_lastActivityTime = QDateTime::currentSecsSinceEpoch();

    if (m_maxSessionTimer) {
        m_maxSessionTimer->stop();
        if (m_maxSessionHours > 0) {
            m_maxSessionTimer->start(m_maxSessionHours * 3600 * 1000);
        }
    }

    Logger::info(QObject::tr("MainWindow: Oturum güvenlik süreleri güncellendi → Boşta kalma: %1 dk, Azami oturum: %2 saat")
                 .arg(m_idleTimeoutMinutes).arg(m_maxSessionHours));
}

void MainWindow::lockScreen() {
    if (isScreenLocked()) return;

    // 1. Açık olan tüm üst düzey pencereleri ve diyalogları güvenli bir şekilde kapat/gizle
    m_hiddenWindowsOnLock.clear();
    const auto topWidgets = QApplication::topLevelWidgets();
    for (QWidget* w : topWidgets) {
        if (!w || w == this || w == m_lockOverlay || !w->isVisible()) continue;

        if (auto* dlg = qobject_cast<QDialog*>(w)) {
            // Açık modal veya modeless diyalogları güvenli şekilde reject ederek kapat
            dlg->reject();
        } else {
            // ChatWindow veya diğer pencereleri gizle ve kilit açıldığında geri getirmek üzere sakla
            m_hiddenWindowsOnLock.append(w);
            w->hide();
        }
    }

    if (m_chatWindow) {
        m_chatWindow->setBlockedByLock(true);
    }

    if (!m_lockOverlay) {
        m_lockOverlay = new MasterLockOverlay(m_session, this);
        connect(m_lockOverlay, &MasterLockOverlay::unlocked, this, &MainWindow::unlockScreen);
        connect(m_lockOverlay, &MasterLockOverlay::logoutRequested, this, [this]() {
            if (m_lockOverlay) m_lockOverlay->hide();
            if (m_sysMgmtWindow) m_sysMgmtWindow->close();
            if (m_userMgmtPanel) m_userMgmtPanel->close();
            if (m_reportPanel)   m_reportPanel->close();
            if (m_chatWindow)    m_chatWindow->close();
            m_hiddenWindowsOnLock.clear();
            emit logoutRequested();
        });
    }

    // Ana pencereyi öne getir
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();

    m_lockOverlay->setUser(m_activeUser);
    m_lockOverlay->resetState();
    m_lockOverlay->setGeometry(rect());
    m_lockOverlay->show();
    m_lockOverlay->raise();
    m_lockOverlay->setFocus();

    // Master kilitlendiğinde canlı akışı duraklat (arka planda ağı meşgul etmesin)
    if (m_session) {
        m_session->setVisibleClients({});
    }

    AuditLog::instance().recordAuth(m_activeUser.username, tr("Master ekranı kilitlendi"));
    Logger::info(QObject::tr("MainWindow: Ekran kilitlendi → ") + m_activeUser.username);
}

void MainWindow::unlockScreen() {
    if (m_lockOverlay) {
        m_lockOverlay->hide();
    }
    m_lastActivityTime = QDateTime::currentSecsSinceEpoch();
    Logger::info(QObject::tr("MainWindow: Ekran kilidi açıldı → ") + m_activeUser.username);

    if (m_chatWindow) {
        m_chatWindow->setBlockedByLock(false);
    }

    // Kilit öncesi gizlenen pencereleri geri getir
    for (const QPointer<QWidget>& w : m_hiddenWindowsOnLock) {
        if (w) {
            w->show();
            w->raise();
        }
    }
    m_hiddenWindowsOnLock.clear();

    // Kilit açıldığında mevcut layout ve widget'ları bozmadan görünür istemciler için akışı yeniden başlat
    if (m_session) {
        QStringList visibleList;
        for (auto* frame : m_groupFrames) {
            if (frame && frame->isVisible()) {
                visibleList.append(frame->clientIds());
            }
        }
        if (visibleList.isEmpty()) {
            visibleList = m_thumbnails.keys();
        }
        updateStatusBar(visibleList);
    }
}

bool MainWindow::isScreenLocked() const {
    return (m_lockOverlay != nullptr && m_lockOverlay->isVisible());
}
