#ifndef ACCORDIONLOGWIDGET_H
#define ACCORDIONLOGWIDGET_H

#include <QWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

class AccordionLogWidget : public QWidget {
    Q_OBJECT
public:
    explicit AccordionLogWidget(const QString& title, QWidget* parent = nullptr);

    void appendLog(const QString& text);
    void setStatus(int status); // 0: running, 1: success, 2: error
    void toggle(bool expanded);

private:
    QPushButton* m_headerBtn;
    QTextEdit* m_contentArea;
    QString m_baseTitle;
};

#endif // ACCORDIONLOGWIDGET_H
