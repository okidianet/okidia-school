#include "AgentTypeSelectionDialog.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/db/OkidiaDatabase.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

AgentTypeSelectionDialog::AgentTypeSelectionDialog(const QList<DiscoveryAgentInfo>& agents, QWidget* parent)
    : QDialog(parent), m_agents(agents)
{
    setWindowTitle(tr("İstemci Tipi ve Onay Seçimi"));
    resize(450, 300);

    auto* lay = new QVBoxLayout(this);

    lay->addWidget(new QLabel(tr("Lütfen seçili istemciler için atanacak tipi belirleyin:"), this));

    m_cmbType = new QComboBox(this);
    m_cmbType->addItem(ThemeManager::icon(":/ui/student.svg"), tr("Öğrenci PC"), 1);
    m_cmbType->addItem(ThemeManager::icon(":/ui/admin.svg"), tr("İdari PC (Gizlilik)"), 2);
    m_cmbType->addItem(ThemeManager::icon(":/ui/smartboard.svg"), tr("Akıllı Tahta"), 3);

    const auto customProfiles = OkidiaDatabase::instance().allCustomProfiles();
    for (const auto& cp : customProfiles) {
        m_cmbType->addItem(ThemeManager::icon(":/ui/custom_profile.svg"), cp.name, cp.id);
    }
    lay->addWidget(m_cmbType);

    lay->addSpacing(10);
    lay->addWidget(new QLabel(tr("Onaylanacak İstemciler (İptal etmek istediklerinizi işaretsiz bırakın):"), this));

    m_list = new QListWidget(this);
    for (int i = 0; i < m_agents.size(); ++i) {
        const auto& agent = m_agents[i];
        QString label = QString("%1  -  %2").arg(agent.hostname, agent.targetId);
        auto* item = new QListWidgetItem(label, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(Qt::UserRole, i); // store index
    }
    lay->addWidget(m_list);

    auto* bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bbox);
    connect(bbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

uint32_t AgentTypeSelectionDialog::selectedAgentType() const {
    return m_cmbType->currentData().toUInt();
}

QList<DiscoveryAgentInfo> AgentTypeSelectionDialog::checkedAgents() const {
    QList<DiscoveryAgentInfo> checked;
    for (int i = 0; i < m_list->count(); ++i) {
        auto* item = m_list->item(i);
        if (item->checkState() == Qt::Checked) {
            int index = item->data(Qt::UserRole).toInt();
            checked.append(m_agents[index]);
        }
    }
    return checked;
}
