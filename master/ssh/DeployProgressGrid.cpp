// master/ssh/DeployProgressGrid.cpp
#include "DeployProgressGrid.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/OkidiaConfig.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QMessageBox>

DeployProgressGrid::DeployProgressGrid(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

void DeployProgressGrid::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(6);

    // ── Üst Yönetim ve Özet Çubuğu ────────────────────────────────
    auto* headerFrame = new QFrame(this);
    headerFrame->setFrameShape(QFrame::StyledPanel);
    auto* headerLay = new QVBoxLayout(headerFrame);
    headerLay->setContentsMargins(10, 8, 10, 8);
    headerLay->setSpacing(8);

    auto* topControls = new QHBoxLayout();
    topControls->setSpacing(12);

    m_backBtn = new QPushButton(tr("Geri Dön"), headerFrame);
    m_backBtn->setIcon(ThemeManager::icon(":/ui/arrow_left.svg"));
    m_backBtn->setProperty("class", "primary");
    m_backBtn->setFixedHeight(32);
    m_backBtn->setCursor(Qt::PointingHandCursor);
    topControls->addWidget(m_backBtn);

    // Sayaç Rozetleri
    m_totalLabel = new QLabel(tr("Toplam: 0"), headerFrame);
    m_totalLabel->setProperty("status", "info");
    QFont badgeFont = m_totalLabel->font();
    badgeFont.setBold(true);
    m_totalLabel->setFont(badgeFont);

    m_successLabel = new QLabel(tr("Başarılı: 0"), headerFrame);
    m_successLabel->setFont(badgeFont);
    m_successLabel->setStyleSheet("color: #2ecc71;");

    m_runningLabel = new QLabel(tr("Kuruluyor: 0"), headerFrame);
    m_runningLabel->setFont(badgeFont);
    m_runningLabel->setStyleSheet("color: #3498db;");

    m_queuedLabel = new QLabel(tr("Bekleyen (Kapalı): 0"), headerFrame);
    m_queuedLabel->setFont(badgeFont);
    m_queuedLabel->setStyleSheet("color: #f39c12;");
    m_queuedLabel->setToolTip(tr("Şu anda çevrimdışı olan ve açıldığında otonom olarak güncellenecek istemciler"));

    m_failedLabel = new QLabel(tr("Hatalı: 0"), headerFrame);
    m_failedLabel->setFont(badgeFont);
    m_failedLabel->setStyleSheet("color: #e74c3c;");

    topControls->addWidget(m_totalLabel);
    topControls->addWidget(m_successLabel);
    topControls->addWidget(m_runningLabel);
    topControls->addWidget(m_queuedLabel);
    topControls->addWidget(m_failedLabel);

    topControls->addStretch();

    // Filtreleme Seçici
    auto* filterLbl = new QLabel(tr("Filtre:"), headerFrame);
    topControls->addWidget(filterLbl);

    m_filterCombo = new QComboBox(headerFrame);
    m_filterCombo->addItem(ThemeManager::icon(":/ui/monitor.svg"), tr("Tümü"), "all");
    m_filterCombo->addItem(ThemeManager::icon(":/ui/hourglass.svg"), tr("Kurulanlar (Açık)"), "running");
    m_filterCombo->addItem(ThemeManager::icon(":/ui/clock.svg"), tr("Bekleyenler (Kapalı)"), "queued");
    m_filterCombo->addItem(ThemeManager::icon(":/ui/error_circle.svg"), tr("Hatalılar"), "failed");
    m_filterCombo->addItem(ThemeManager::icon(":/ui/check_circle.svg"), tr("Başarılılar"), "success");
    m_filterCombo->setFixedHeight(30);
    topControls->addWidget(m_filterCombo);

    m_clearLogsBtn = new QPushButton(tr("Günlükleri Temizle"), headerFrame);
    m_clearLogsBtn->setIcon(ThemeManager::icon(":/ui/clear.svg"));
    m_clearLogsBtn->setProperty("class", "danger");
    m_clearLogsBtn->setFixedHeight(30);
    m_clearLogsBtn->setCursor(Qt::PointingHandCursor);
    m_clearLogsBtn->setToolTip(tr("Kayıtlı kurulum/güncelleme günlüklerini ve geçmiş kartları yerel diskten siler."));
    topControls->addWidget(m_clearLogsBtn);

    headerLay->addLayout(topControls);

    // Genel İlerleme Çubuğu
    m_overallProgress = new QProgressBar(headerFrame);
    m_overallProgress->setRange(0, 100);
    m_overallProgress->setValue(0);
    m_overallProgress->setFixedHeight(8);
    m_overallProgress->setTextVisible(false);
    headerLay->addWidget(m_overallProgress);

    mainLay->addWidget(headerFrame);

    // ── Izgara (Grid) Alanı ────────────────────────────────────────
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_gridContainer = new QWidget(m_scrollArea);
    m_gridLayout = new QGridLayout(m_gridContainer);
    m_gridLayout->setContentsMargins(4, 4, 4, 4);
    m_gridLayout->setSpacing(8);
    m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_scrollArea->setWidget(m_gridContainer);
    mainLay->addWidget(m_scrollArea, 1);

    connect(m_backBtn, &QPushButton::clicked, this, &DeployProgressGrid::backToTableRequested);
    connect(m_filterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeployProgressGrid::onFilterChanged);
    connect(m_clearLogsBtn, &QPushButton::clicked, this, &DeployProgressGrid::onClearLogsClicked);
}

void DeployProgressGrid::clearCards() {
    m_cards.clear();
    m_cardList.clear();

    QLayoutItem* child;
    while ((child = m_gridLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->deleteLater();
        }
        delete child;
    }

    updateSummary();
}

void DeployProgressGrid::addCard(HostInstallCard* card) {
    if (!card) return;

    m_cards.insert(card->ip(), card);
    m_cardList.append(card);

    connect(card, &HostInstallCard::remoteLogRequested,
            this, &DeployProgressGrid::remoteLogRequested, Qt::UniqueConnection);
    connect(card, &HostInstallCard::stateUpdated,
            this, &DeployProgressGrid::saveHistory, Qt::UniqueConnection);

    relayoutCards();
    updateSummary();
}

HostInstallCard* DeployProgressGrid::card(const QString& ip) const {
    if (m_cards.contains(ip)) return m_cards.value(ip);
    for (auto* c : m_cardList) {
        if (c->ip() == ip || c->hostname().toLower() == ip.toLower() ||
            (!c->machineId().isEmpty() && c->machineId() == ip)) {
            return c;
        }
    }
    return nullptr;
}

void DeployProgressGrid::updateSummary() {
    int total = m_cardList.size();
    int success = 0;
    int failed = 0;
    int running = 0;
    int queued = 0;

    for (auto* c : m_cardList) {
        if (c->isSuccess()) {
            success++;
        } else if (c->isFailed()) {
            failed++;
        } else if (c->isQueuedOffline()) {
            queued++;
        } else {
            running++;
        }
    }

    m_totalLabel->setText(tr("Toplam: %1").arg(total));
    m_successLabel->setText(tr("Başarılı: %1").arg(success));
    m_runningLabel->setText(tr("Kuruluyor: %1").arg(running));
    if (m_queuedLabel) {
        m_queuedLabel->setText(tr("Bekleyen (Kapalı): %1").arg(queued));
        m_queuedLabel->setVisible(queued > 0 || m_storageKey == "ota_update_history");
    }
    m_failedLabel->setText(tr("Hatalı: %1").arg(failed));

    if (m_clearLogsBtn) {
        m_clearLogsBtn->setEnabled(total > 0 || hasSavedHistory());
    }

    int activeTotal = total - queued;
    int finished = success + failed;
    int pct = 0;
    if (activeTotal > 0) {
        pct = (finished * 100) / activeTotal;
    } else if (total > 0 && queued == total) {
        pct = 100;
    }
    m_overallProgress->setValue(qBound(0, pct, 100));
    saveHistory();
}

void DeployProgressGrid::onFilterChanged(int) {
    relayoutCards();
}

void DeployProgressGrid::onClearLogsClicked() {
    clearHistory(true);
}

void DeployProgressGrid::setStorageKey(const QString& key) {
    m_storageKey = key;
    if (m_queuedLabel) {
        m_queuedLabel->setVisible(m_storageKey == "ota_update_history");
    }
}

QString DeployProgressGrid::historyFilePath() const {
    if (m_storageKey.isEmpty()) return QString();
    QString dirPath = OkidiaConfig::logDir();
    QDir d(dirPath);
    if (!d.exists()) d.mkpath(".");
    return d.filePath(m_storageKey + ".json");
}

bool DeployProgressGrid::hasSavedHistory() const {
    QString path = historyFilePath();
    return !path.isEmpty() && QFile::exists(path);
}

void DeployProgressGrid::saveHistory() const {
    QString path = historyFilePath();
    if (path.isEmpty()) return;

    if (m_cardList.isEmpty()) {
        return;
    }

    QJsonArray arr;
    for (const HostInstallCard* c : m_cardList) {
        if (c) arr.append(c->toJson());
    }

    QJsonObject root;
    root["updatedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["cards"] = arr;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

bool DeployProgressGrid::loadHistory() {
    QString path = historyFilePath();
    if (path.isEmpty() || !QFile::exists(path)) return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return false;

    QJsonArray arr = doc.object()["cards"].toArray();
    if (arr.isEmpty()) return false;

    m_cards.clear();
    m_cardList.clear();
    QLayoutItem* child;
    while ((child = m_gridLayout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    for (const QJsonValue& val : arr) {
        if (!val.isObject()) continue;
        HostInstallCard* c = HostInstallCard::fromJson(val.toObject(), this);
        if (c) {
            m_cards.insert(c->ip(), c);
            m_cardList.append(c);
            connect(c, &HostInstallCard::remoteLogRequested,
                    this, &DeployProgressGrid::remoteLogRequested, Qt::UniqueConnection);
            connect(c, &HostInstallCard::stateUpdated,
                    this, &DeployProgressGrid::saveHistory, Qt::UniqueConnection);
        }
    }

    relayoutCards();
    updateSummary();
    return !m_cardList.isEmpty();
}

bool DeployProgressGrid::clearHistory(bool askConfirm) {
    if (askConfirm) {
        int res = QMessageBox::question(
            this,
            tr("Kurulum Günlüklerini Sil"),
            tr("Kayıtlı kurulum/güncelleme günlüklerini ve geçmiş durum kartlarını bu bilgisayardan silmek istiyor musunuz?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (res != QMessageBox::Yes) return false;
    }

    QString path = historyFilePath();
    if (!path.isEmpty() && QFile::exists(path)) {
        QFile::remove(path);
    }

    clearCards();
    emit historyCleared();
    emit backToTableRequested();
    return true;
}

void DeployProgressGrid::relayoutCards() {
    // Mevcut layout öğelerini layout'tan çıkar (widget'ları silmeden)
    QLayoutItem* item;
    while ((item = m_gridLayout->takeAt(0)) != nullptr) {
        delete item;
    }

    QString filterType = m_filterCombo->currentData().toString();

    // Genişliğe göre sütun sayısını belirle (her kart min 260px)
    int availWidth = m_scrollArea->viewport()->width();
    if (availWidth <= 0) availWidth = width();
    int numColumns = qMax(1, availWidth / 280);

    int row = 0;
    int col = 0;

    for (auto* card : m_cardList) {
        bool visible = true;
        if (filterType == "running") {
            visible = !card->isFinished() && !card->isQueuedOffline();
        } else if (filterType == "queued") {
            visible = card->isQueuedOffline();
        } else if (filterType == "failed") {
            visible = card->isFailed();
        } else if (filterType == "success") {
            visible = card->isSuccess();
        }

        card->setVisible(visible);

        if (visible) {
            m_gridLayout->addWidget(card, row, col);
            col++;
            if (col >= numColumns) {
                col = 0;
                row++;
            }
        }
    }
}

void DeployProgressGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayoutCards();
}
