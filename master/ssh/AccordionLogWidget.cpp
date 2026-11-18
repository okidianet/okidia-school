#include "AccordionLogWidget.h"
#include "../../common/ui/ThemeManager.h"

AccordionLogWidget::AccordionLogWidget(const QString& title, QWidget* parent)
    : QWidget(parent), m_baseTitle(title) {
    
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_headerBtn = new QPushButton(this);
    m_headerBtn->setStyleSheet("text-align: left; font-weight: bold; padding: 5px;");
    m_headerBtn->setCursor(Qt::PointingHandCursor);

    m_contentArea = new QTextEdit(this);
    m_contentArea->setReadOnly(true);
    QFont f = m_contentArea->font();
    f.setFamily("Monospace");
    m_contentArea->setFont(f);
    m_contentArea->setVisible(false);
    m_contentArea->setMinimumHeight(150);

    lay->addWidget(m_headerBtn);
    lay->addWidget(m_contentArea);

    connect(m_headerBtn, &QPushButton::clicked, this, [this]() {
        toggle(!m_contentArea->isVisible());
    });

    setStatus(0);
}

void AccordionLogWidget::appendLog(const QString& text) {
    m_contentArea->moveCursor(QTextCursor::End);
    m_contentArea->insertPlainText(text);
    m_contentArea->moveCursor(QTextCursor::End);
}

void AccordionLogWidget::setStatus(int status) {
    QString iconStr;
    if (status == 0) iconStr = "⏳";
    else if (status == 1) iconStr = "✅";
    else if (status == 2) iconStr = "❌";

    m_headerBtn->setText(QString("[ %1 ] %2").arg(iconStr, m_baseTitle));
}

void AccordionLogWidget::toggle(bool expanded) {
    m_contentArea->setVisible(expanded);
}
