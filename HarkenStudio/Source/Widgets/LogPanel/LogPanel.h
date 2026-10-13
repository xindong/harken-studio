
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

namespace VNX {

enum class LogCategory : uint8_t
{
    Engine = 0,
    Audio,
    GUI,
    File,
    Layout,
    Import,
    Pack,
    MCP,
    Network,
    RuntimeSync
};

const char* logCategoryName(LogCategory category);
int logCategoryCount();
LogCategory logCategoryAt(int index);

struct LogEntry
{
    enum Level {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warning = 3,
        Error = 4,
        Critical = 5
    };
    
    QDateTime timestamp;
    Level level = Info;
    QString category;
    QString message;
    QString source;
    int threadId = 0;
    
    static QString levelToString(Level level);
    static Level stringToLevel(const QString& str);
    static QColor levelColor(Level level);
    QString formatted() const;
};

class LogModel : public QObject
{
    Q_OBJECT

public:
    explicit LogModel(QObject* parent = nullptr);
    ~LogModel() override;
    
    void addEntry(const LogEntry& entry);
    void addLog(LogEntry::Level level, LogCategory category, const QString& message);
    void addLog(LogEntry::Level level, const QString& category, const QString& message);

    void trace(LogCategory category, const QString& msg) { addLog(LogEntry::Trace, category, msg); }
    void debug(LogCategory category, const QString& msg) { addLog(LogEntry::Debug, category, msg); }
    void info(LogCategory category, const QString& msg) { addLog(LogEntry::Info, category, msg); }
    void warning(LogCategory category, const QString& msg) { addLog(LogEntry::Warning, category, msg); }
    void error(LogCategory category, const QString& msg) { addLog(LogEntry::Error, category, msg); }
    void critical(LogCategory category, const QString& msg) { addLog(LogEntry::Critical, category, msg); }
    void info(const QString& category, const QString& msg) { addLog(LogEntry::Info, category, msg); }
    void warning(const QString& category, const QString& msg) { addLog(LogEntry::Warning, category, msg); }
    void error(const QString& category, const QString& msg) { addLog(LogEntry::Error, category, msg); }
    
    std::vector<LogEntry> entries(LogEntry::Level minLevel = LogEntry::Trace,
                                   const QString& categoryFilter = QString(),
                                   const QString& textFilter = QString()) const;
    int count() const;
    void clear();
    
    void setMaxEntries(int max) { maxEntries_ = max; }
    int maxEntries() const { return maxEntries_; }
    
    QString exportToText(LogEntry::Level minLevel = LogEntry::Trace) const;
    bool exportToFile(const QString& filePath, LogEntry::Level minLevel = LogEntry::Trace) const;

signals:
    void entryAdded(const LogEntry& entry);
    void cleared();

private:
    mutable QMutex mutex_;
    std::deque<LogEntry> entries_;
    int maxEntries_ = 10000;
};

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
