#ifndef TRANSFER_CONFLICT_DIALOG_H
#define TRANSFER_CONFLICT_DIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QCheckBox;

class TransferConflictDialog : public QDialog {
    Q_OBJECT

public:
    enum class Resolution {
        Overwrite,
        Merge,
        Copy,
        Skip,
        Cancel
    };

    TransferConflictDialog(const QString& itemName, bool isDir, QWidget* parent = nullptr);

    Resolution getResolution() const { return m_resolution; }
    bool applyToAll() const;

private:
    void setupUi(const QString& itemName, bool isDir);

    Resolution m_resolution = Resolution::Cancel;
    QCheckBox* m_applyToAllCheckBox;
};

#endif // TRANSFER_CONFLICT_DIALOG_H
