// master/ui/LogViewerDialog/LogViewerDialog.h
#pragma once

#include <QDialog>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QTimeEdit>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTimer>
#include <QScrollBar>
#include "messages.pb.h"

class SessionManager;

class LogSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit LogSyntaxHighlighter(QTextDocument* parent = nullptr);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<HighlightingRule> m_rules;
};

class LogViewerDialog : public QDialog {
    Q_OBJECT
public:
    explicit LogViewerDialog(SessionManager* session, QWidget* parent = nullptr);
    ~LogViewerDialog() override = default;

    // Belirli bir hedef seçili olarak aç (örneğin "local", "broker", veya clientId)
    void setTarget(const QString& targetId);

private slots:
    void onSourceChanged(int index);
    void onFileChanged(int index);
    void onFilterChanged();
    void onRefresh();
    void onOpenLogFolder();
    void onClearCurrentLog();
    void onExportLogs();
    void onRemoteLogReceived(const QString& sourceId, const okidia::LogTailResponse& resp);

private:
    void setupUi();
    void loadSources();
    void loadLocalFiles();
    void loadLocalLogs();
    void requestRemoteLogs();
    void updateStatus(const QString& msg, bool isBusy = false);

    SessionManager* m_session = nullptr;

    QComboBox*    m_sourceCombo     = nullptr;
    QComboBox*    m_fileCombo       = nullptr;
    QComboBox*    m_levelCombo      = nullptr;
    QComboBox*    m_lineCountCombo  = nullptr;
    QCheckBox*    m_timeFilterCheck = nullptr;
    QTimeEdit*    m_timeFromEdit    = nullptr;
    QTimeEdit*    m_timeToEdit      = nullptr;
    QLineEdit*    m_searchEdit      = nullptr;

    QPlainTextEdit*       m_logView       = nullptr;
    LogSyntaxHighlighter* m_highlighter   = nullptr;
    QLabel*               m_statusLabel   = nullptr;
    QProgressBar*         m_progressBar   = nullptr;

    QPushButton* m_btnRefresh    = nullptr;
    QPushButton* m_btnOpenFolder = nullptr;
    QPushButton* m_btnClear      = nullptr;
    QPushButton* m_btnExport     = nullptr;
    QPushButton* m_btnClose      = nullptr;

    QTimer*      m_searchDebounceTimer = nullptr;
    QString      m_currentTargetId;
};
