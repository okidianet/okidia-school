#include "BrokerUpdateManagerDialog.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QHeaderView>
#include <QDateTime>
#include <QMessageBox>
#include <QRegularExpression>

BrokerUpdateManagerDialog::BrokerUpdateManagerDialog(SessionManager* session, QWidget* parent)
    : QDialog(parent), m_session(session) {
    
    setWindowTitle(tr("Sunucu Güncelleme Deposu"));
    resize(800, 450);
    
    auto* mainLayout = new QVBoxLayout(this);
    
    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({tr("Dosya Adı"), tr("Paket Türü"), tr("Sürüm"), tr("Boyut"), tr("Oluşturulma Tarihi")});
    OkidiaUtils::setupTableHeaders(m_table, "broker_update_table", {200, 110, 80, 90, 140}, true);
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    
    mainLayout->addWidget(m_table);
    
    auto* btnLayout = new QHBoxLayout();
    
    m_refreshBtn = new QPushButton(tr("Tazele"), this);
    m_deleteSelectedBtn = new QPushButton(tr("Seçilileri Sil"), this);
    m_deleteSelectedBtn->setProperty("class", "danger");
    m_keepLatestBtn = new QPushButton(tr("Son Sürüm Hariç Tümünü Sil"), this);
    m_deleteAllBtn = new QPushButton(tr("Tümünü Sil"), this);
    m_deleteAllBtn->setProperty("class", "danger");
    
    m_statusLabel = new QLabel(this);
    
    btnLayout->addWidget(m_refreshBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_statusLabel);
    btnLayout->addStretch();
    btnLayout->addWidget(m_keepLatestBtn);
    btnLayout->addWidget(m_deleteSelectedBtn);
    btnLayout->addWidget(m_deleteAllBtn);
    
    mainLayout->addLayout(btnLayout);
    
    connect(m_refreshBtn, &QPushButton::clicked, this, &BrokerUpdateManagerDialog::onRefreshClicked);
    connect(m_deleteSelectedBtn, &QPushButton::clicked, this, &BrokerUpdateManagerDialog::onDeleteSelectedClicked);
    connect(m_keepLatestBtn, &QPushButton::clicked, this, &BrokerUpdateManagerDialog::onKeepLatestOnlyClicked);
    connect(m_deleteAllBtn, &QPushButton::clicked, this, &BrokerUpdateManagerDialog::onDeleteAllClicked);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &BrokerUpdateManagerDialog::onSelectionChanged);
    
    connect(m_session, &SessionManager::brokerUpdatesListReceived, this, &BrokerUpdateManagerDialog::onBrokerUpdatesListReceived);
    connect(m_session, &SessionManager::brokerUpdatesDeleteResult, this, &BrokerUpdateManagerDialog::onBrokerUpdatesDeleteResult);
    
    updateButtons();
    requestList();
}

void BrokerUpdateManagerDialog::requestList() {
    m_statusLabel->setText(tr("Paket listesi alınıyor..."));
    m_table->setRowCount(0);
    m_session->requestBrokerUpdatesList();
}

void BrokerUpdateManagerDialog::onRefreshClicked() {
    requestList();
}

void BrokerUpdateManagerDialog::onBrokerUpdatesListReceived(const QVariantList& packages) {
    m_statusLabel->clear();
    m_table->setRowCount(packages.size());
    
    for (int i = 0; i < packages.size(); ++i) {
        const QVariantMap pkg = packages[i].toMap();
        QString filename = pkg["filename"].toString();
        
        OkidiaTableItem* fileItem = new OkidiaTableItem(filename);
        fileItem->setData(Qt::UserRole, filename);
        
        QString typeStr = tr("Diğer");
        if (filename.startsWith("okidia-agent")) {
            typeStr = tr("İstemci");
        } else if (filename.startsWith("okidia-broker")) {
            typeStr = tr("Sunucu (Broker)");
        } else if (filename.startsWith("okidia-master")) {
            typeStr = tr("Yönetici (Master)");
        }
        OkidiaTableItem* typeItem = new OkidiaTableItem(typeStr);
        
        OkidiaTableItem* verItem = new OkidiaTableItem(pkg["version"].toString());
        
        qulonglong size = pkg["size"].toULongLong();
        OkidiaTableItem* sizeItem = new OkidiaTableItem(formatSize(size));
        sizeItem->setData(Qt::UserRole + 1, size);
        
        qulonglong ts = pkg["timestamp"].toULongLong();
        OkidiaTableItem* timeItem = new OkidiaTableItem(formatTime(ts));
        timeItem->setData(Qt::UserRole + 1, ts);
        
        m_table->setItem(i, 0, fileItem);
        m_table->setItem(i, 1, typeItem);
        m_table->setItem(i, 2, verItem);
        m_table->setItem(i, 3, sizeItem);
        m_table->setItem(i, 4, timeItem);
    }
    
    // Tarihe göre ters sırala (en yeni en üstte)
    m_table->sortByColumn(4, Qt::DescendingOrder);
    updateButtons();
}

void BrokerUpdateManagerDialog::onBrokerUpdatesDeleteResult(bool success, const QString& errorMsg, const QStringList& deletedFiles) {
    if (success) {
        m_statusLabel->setText(tr("%1 paket silindi.").arg(deletedFiles.size()));
        requestList();
    } else {
        m_statusLabel->setText(tr("Hata: %1").arg(errorMsg));
    }
}

void BrokerUpdateManagerDialog::onSelectionChanged() {
    updateButtons();
}

void BrokerUpdateManagerDialog::updateButtons() {
    int rowCount = m_table->rowCount();
    int selectedCount = m_table->selectionModel()->selectedRows().size();
    
    m_deleteSelectedBtn->setEnabled(selectedCount > 0);
    m_deleteAllBtn->setEnabled(rowCount > 0);
    m_keepLatestBtn->setEnabled(rowCount > 0);
}

void BrokerUpdateManagerDialog::onDeleteSelectedClicked() {
    QList<QTableWidgetItem*> selectedRows = m_table->selectedItems();
    QSet<int> rows;
    for (auto* item : selectedRows) rows.insert(item->row());
    
    if (rows.isEmpty()) return;
    
    if (QMessageBox::question(this, tr("Onay"), QString(tr("%1 paketi silmek istediğinize emin misiniz?")).arg(rows.size())) != QMessageBox::Yes) {
        return;
    }
    
    QStringList filenames;
    for (int row : rows) {
        filenames.append(m_table->item(row, 0)->data(Qt::UserRole).toString());
    }
    
    m_statusLabel->setText(tr("Siliniyor..."));
    m_session->requestBrokerUpdatesDelete(filenames);
}

void BrokerUpdateManagerDialog::onDeleteAllClicked() {
    if (QMessageBox::question(this, tr("Onay"), tr("Sunucu deposundaki TÜM güncelleme paketlerini silmek istediğinize emin misiniz?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    
    QStringList filenames;
    for (int i = 0; i < m_table->rowCount(); ++i) {
        filenames.append(m_table->item(i, 0)->data(Qt::UserRole).toString());
    }
    
    m_statusLabel->setText(tr("Tümü siliniyor..."));
    m_session->requestBrokerUpdatesDelete(filenames);
}

void BrokerUpdateManagerDialog::onKeepLatestOnlyClicked() {
    // Paketleri bileşen ve mimariye göre grupla:
    // Örneğin: okidia-agent_ubuntu24_amd64, okidia-master_ubuntu24_amd64, okidia-agent_windows_x86_64
    QMap<QString, QList<int>> groups;
    for (int i = 0; i < m_table->rowCount(); ++i) {
        QString filename = m_table->item(i, 0)->data(Qt::UserRole).toString();
        
        QString component = "other";
        if (filename.startsWith("okidia-agent")) component = "okidia-agent";
        else if (filename.startsWith("okidia-master")) component = "okidia-master";
        else if (filename.startsWith("okidia-broker")) component = "okidia-broker";

        QString archKey = "generic";
        if (filename.endsWith(".exe", Qt::CaseInsensitive)) {
            archKey = "windows";
        } else {
            QStringList parts = filename.split('_');
            if (parts.size() >= 4) {
                archKey = parts[1] + "_" + parts[3]; // örn. ubuntu24_amd64.deb
            } else if (parts.size() >= 3) {
                archKey = parts.last(); // örn. amd64.deb
            }
        }
        
        QString key = component + "_" + archKey;
        groups[key].append(i);
    }
    
    QStringList toDelete;
    
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        const QList<int>& rows = it.value();
        if (rows.size() <= 1) continue; // Zaten tek versiyon var
        
        // En güncel olanı bul (OkidiaUtils::compareVersions ile SemVer karşılaştırması)
        int keepRow = rows[0];
        QString maxVer = m_table->item(keepRow, 2) ? m_table->item(keepRow, 2)->text().trimmed() : "";
        qulonglong maxTs = m_table->item(keepRow, 4)->data(Qt::UserRole + 1).toULongLong();
        
        for (int i = 1; i < rows.size(); ++i) {
            int row = rows[i];
            QString curVer = m_table->item(row, 2) ? m_table->item(row, 2)->text().trimmed() : "";
            qulonglong curTs = m_table->item(row, 4)->data(Qt::UserRole + 1).toULongLong();
            
            int cmp = OkidiaUtils::compareVersions(curVer, maxVer);
            if (cmp > 0 || (cmp == 0 && curTs > maxTs)) {
                maxVer = curVer;
                maxTs = curTs;
                keepRow = row;
            }
        }
        
        for (int row : rows) {
            if (row != keepRow) {
                toDelete.append(m_table->item(row, 0)->data(Qt::UserRole).toString());
            }
        }
    }
    
    if (toDelete.isEmpty()) {
        QMessageBox::information(this, tr("Bilgi"), tr("Zaten her mimari için sadece en güncel sürümler mevcut. Temizlenecek eski sürüm bulunamadı."));
        return;
    }
    
    if (QMessageBox::question(this, tr("Onay"), QString(tr("Eski sürümlere ait %1 paket silinecek. Onaylıyor musunuz?")).arg(toDelete.size())) != QMessageBox::Yes) {
        return;
    }
    
    m_statusLabel->setText(tr("Eski sürümler siliniyor..."));
    m_session->requestBrokerUpdatesDelete(toDelete);
}

QString BrokerUpdateManagerDialog::formatSize(qulonglong size) const {
    double mb = size / (1024.0 * 1024.0);
    return QString::number(mb, 'f', 2) + " MB";
}

QString BrokerUpdateManagerDialog::formatTime(qulonglong ts) const {
    return QDateTime::fromSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss");
}
