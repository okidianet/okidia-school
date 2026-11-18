// master/ui/BroadcastSourceDialog/BroadcastSourceDialog.h
#pragma once

#include <QDialog>
#include <QScreen>
#include <QList>
#include <QAbstractButton>

QT_BEGIN_NAMESPACE
namespace Ui { class BroadcastSourceDialog; }
QT_END_NAMESPACE

// Seçilen yayın kaynağı
struct BroadcastSource {
    enum Type { Monitor, AllMonitors };
    Type    type              = AllMonitors;
    int     monitorIndex      = 0;
    QString label;
    int     quality           = 65;   // JPEG kalitesi (20-95)
    int     fps               = 10;   // Saniyedeki kare sayısı
    bool    targetStudents    = true; // Öğrenci PC (Tip 1)
    bool    targetSmartboards = false;// Akıllı Tahta (Tip 3)
};

/**
 * Ekran yayını için kaynak seçim diyaloğu.
 *
 * Sekmeler:
 *   - Monitörler: mevcut ekranların önizlemeli listesi
 *   - Tümü:       tüm monitörler birleşik
 */
class BroadcastSourceDialog : public QDialog {
    Q_OBJECT

public:
    explicit BroadcastSourceDialog(int targetCount = 0, QWidget* parent = nullptr);
    ~BroadcastSourceDialog();

    BroadcastSource selectedSource() const;

private slots:
    void onTabChanged(int idx);
    void onMonitorSelected(int row);

private:
    Ui::BroadcastSourceDialog* ui;
    BroadcastSource m_result;

    void populateMonitors();
    void updatePreview(const BroadcastSource& src);
    void updateAllPreview();
    QPixmap grabMonitor(int idx) const;
};
