// master/ui/ReportPanel/ReportPanel.h
#pragma once

#include <QWidget>
#include <QDateTime>
#include "../../audit/AuditLog.h"

class SessionManager;

QT_BEGIN_NAMESPACE
namespace Ui { class ReportPanel; }
QT_END_NAMESPACE

class ReportPanel : public QWidget {
    Q_OBJECT

public:
    explicit ReportPanel(SessionManager* session, QWidget* parent = nullptr);
    ~ReportPanel() override;

    // Pencereyi aç ve son 24 saati yükle
    void showAndRefresh();

private slots:
    void onFilter();
    void onReset();
    void onQuickRangeChanged(int index);
    void onPageSizeChanged(int index);
    void onFirstPage();
    void onPrevPage();
    void onNextPage();
    void onLastPage();
    void onExportCsv();
    void onExportPdf();
    void onExportHtml();
    void onRowDoubleClicked(int row, int column);
    void onEventsReceived(const QList<AuditEvent>& events, int totalCount);

private:
    Ui::ReportPanel* ui;
    SessionManager* m_session;
    QList<AuditEvent> m_events;

    int m_currentPage = 1;
    int m_pageSize = 50;
    int m_totalCount = 0;
    int m_totalPages = 1;

    void loadEvents();
    void updatePaginationUi();
    QIcon categoryIcon(AuditEvent::Category c) const;
    QColor categoryColor(AuditEvent::Category c) const;

    bool exportToCsv(const QString& filePath) const;
    bool exportToPdf(const QString& filePath) const;
    bool exportToHtml(const QString& filePath) const;
    QString generateHtmlReport(bool forPrint = false) const;
    void showEventDetailDialog(const AuditEvent& event);
};
