#pragma once
#include <QDialog>
#include <QListWidget>
#include <QComboBox>
#include <QByteArray>
#include <QList>
#include <QString>

struct DiscoveryAgentInfo {
    QString targetId;
    QString hostname;
    QByteArray csr;
    class QTableWidgetItem* originalItem = nullptr;
};

class AgentTypeSelectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit AgentTypeSelectionDialog(const QList<DiscoveryAgentInfo>& agents, QWidget* parent = nullptr);

    uint32_t selectedAgentType() const;
    QList<DiscoveryAgentInfo> checkedAgents() const;

private:
    QComboBox* m_cmbType;
    QListWidget* m_list;
    QList<DiscoveryAgentInfo> m_agents;
};
