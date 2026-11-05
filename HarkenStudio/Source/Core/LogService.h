#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QMutex>
#include <cstdint>
#include <deque>
#include <vector>

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

class LogService
{
public:
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
};

}
