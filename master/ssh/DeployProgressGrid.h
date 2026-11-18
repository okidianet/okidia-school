// master/ssh/DeployProgressGrid.h
#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QComboBox>
#include <QMap>
#include "HostInstallCard.h"

class DeployProgressGrid : public QWidget {
    Q_OBJECT

public:
    explicit DeployProgressGrid(QWidget* parent = nullptr);
    ~DeployProgressGrid() override = default;

    void clearCards();
    void addCard(HostInstallCard* card);
    HostInstallCard* card(const QString& ip) const;
    QList<HostInstallCard*> allCards() const { return m_cardList; }

    void updateSummary();

    void setStorageKey(const QString& key);
    QString storageKey() const { return m_storageKey; }
    QString historyFilePath() const;
    bool hasSavedHistory() const;
    void saveHistory() const;
    bool loadHistory();
    bool clearHistory(bool askConfirm = true);

signals:
    void backToTableRequested();
    void historyCleared();
    void remoteLogRequested(const QString& machineId, const QString& ip, const QString& hostname);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onFilterChanged(int index);
    void onClearLogsClicked();

private:
    void setupUi();
    void relayoutCards();

    QPushButton* m_backBtn = nullptr;
    QLabel* m_totalLabel = nullptr;
    QLabel* m_successLabel = nullptr;
    QLabel* m_runningLabel = nullptr;
    QLabel* m_queuedLabel = nullptr;
    QLabel* m_failedLabel = nullptr;
    QComboBox* m_filterCombo = nullptr;
    QPushButton* m_clearLogsBtn = nullptr;
    QProgressBar* m_overallProgress = nullptr;

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_gridContainer = nullptr;
    QGridLayout* m_gridLayout = nullptr;

    QMap<QString, HostInstallCard*> m_cards;
    QList<HostInstallCard*> m_cardList; // Eklenme sırasını korumak için
    QString m_storageKey;
};
