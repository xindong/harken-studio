
#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QLineEdit>
#include <QToolButton>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QMutex>
#include <cstdint>
#include <deque>
#include <vector>
#include "../../Core/AppSettings.h"
#include "../../Core/LogService.h"

namespace VNX {

class LogTextEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit LogTextEdit(QWidget* parent = nullptr);
    
    void appendEntry(const LogEntry& entry);
    void setAutoScroll(bool enabled) { autoScroll_ = enabled; }
    bool autoScroll() const { return autoScroll_; }

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    bool autoScroll_ = true;
    QTextCharFormat formatForLevel(LogEntry::Level level) const;
};

class LogPanel : public QWidget
{
    Q_OBJECT

public:
    explicit LogPanel(QWidget* parent = nullptr);
    ~LogPanel() override;
    
    LogModel* model() { return model_; }
    
    static LogPanel* instance();
    static LogModel* globalModel();
    
    static void log(LogEntry::Level level, LogCategory category, const QString& message);
    static void log(LogEntry::Level level, const QString& category, const QString& message);
    static LogEntry::Level engineLevelToPanel(int level);
    static void logEngineMessage(int level, const QString& message);
    static void logInfo(LogCategory category, const QString& message);
    static void logInfo(const QString& category, const QString& message);
    static void logWarning(LogCategory category, const QString& message);
    static void logWarning(const QString& category, const QString& message);
    static void logError(LogCategory category, const QString& message);
    static void logError(const QString& category, const QString& message);

public slots:
    void clear();
    void copyToClipboard();
    void exportLog();
    void setFilter(const QString& filter);
    void setMinLevel(LogEntry::Level level);
    void setCategory(const QString& category);
    void scrollToBottom();
    void refreshDisplay();

signals:
    void errorLogged(const QString& message);
    void warningLogged(const QString& message);

private slots:
    void onEntryAdded(const LogEntry& entry);
    void onFilterChanged();
    void onLevelChanged(int index);
    void onCategoryChanged(int index);
    void onSearchTextChanged(const QString& text);
    void onFlushTimer();
    void onFontSettingsChanged(PanelId panelId, int size);

private:
    void setupUI();
    void setupConnections();
    void refreshIcons();
    void applyFilter();
    bool matchesFilter(const LogEntry& entry) const;
    void updateCountLabel();

private:
    LogModel* model_;
    
    LogTextEdit* logTextEdit_;
    QComboBox* levelCombo_;
    QComboBox* categoryCombo_;
    QLineEdit* searchEdit_;
    QToolButton* clearBtn_;
    QToolButton* exportBtn_;
    QToolButton* copyBtn_;
    QToolButton* scrollBtn_;
    QLabel* countLabel_;
    
    LogEntry::Level minLevel_ = LogEntry::Trace;
    QString categoryFilter_;
    QString textFilter_;
    
    std::vector<LogEntry> pendingEntries_;
    QTimer* flushTimer_;
    QMutex pendingMutex_;
    
    static LogPanel* instance_;
};

#define XDLOG_TRACE(cat, msg) VNX::LogPanel::log(VNX::LogEntry::Trace, cat, msg)
#define XDLOG_DEBUG(cat, msg) VNX::LogPanel::log(VNX::LogEntry::Debug, cat, msg)
#define XDLOG_INFO(cat, msg) VNX::LogPanel::logInfo(cat, msg)
#define XDLOG_WARNING(cat, msg) VNX::LogPanel::logWarning(cat, msg)
#define XDLOG_ERROR(cat, msg) VNX::LogPanel::logError(cat, msg)
#define XDLOG_CRITICAL(cat, msg) VNX::LogPanel::log(VNX::LogEntry::Critical, cat, msg)

}
