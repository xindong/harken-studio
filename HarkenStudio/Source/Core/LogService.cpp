#include "LogService.h"

#include <QFile>
#include <QTextStream>
#include <QThread>

namespace VNX {

namespace {

constexpr const char* kLogCategoryNames[] = {
    "Engine",
    "Audio",
    "GUI",
    "File",
    "Layout",
    "Import",
    "Pack",
    "MCP",
    "Network",
    "RuntimeSync",
};

}

const char* logCategoryName(LogCategory category)
{
    const auto index = static_cast<int>(category);
    if (index < 0 || index >= logCategoryCount())
        return kLogCategoryNames[0];
    return kLogCategoryNames[index];
}

int logCategoryCount()
{
    return static_cast<int>(sizeof(kLogCategoryNames) / sizeof(kLogCategoryNames[0]));
}

LogCategory logCategoryAt(int index)
{
    if (index < 0 || index >= logCategoryCount())
        return LogCategory::Engine;
    return static_cast<LogCategory>(index);
}

QString LogEntry::levelToString(Level level)
{
    switch (level)
    {
        case Trace:    return "TRACE";
        case Debug:    return "DEBUG";
        case Info:     return "INFO";
        case Warning:  return "WARN";
        case Error:    return "ERROR";
        case Critical: return "CRIT";
        default:       return "???";
    }
}

LogEntry::Level LogEntry::stringToLevel(const QString& str)
{
    QString upper = str.toUpper();
    if (upper == "TRACE") return Trace;
    if (upper == "DEBUG") return Debug;
    if (upper == "INFO")  return Info;
    if (upper == "WARN" || upper == "WARNING") return Warning;
    if (upper == "ERROR") return Error;
    if (upper == "CRIT" || upper == "CRITICAL") return Critical;
    return Info;
}

QString LogEntry::formatted() const
{
    return QString("[%1] [%2] [%3] %4")
        .arg(timestamp.toString("hh:mm:ss.zzz"))
        .arg(levelToString(level), -5)
        .arg(category, -8)
        .arg(message);
}


LogModel::LogModel(QObject* parent)
    : QObject(parent)
{
}

LogModel::~LogModel() = default;

void LogModel::addEntry(const LogEntry& entry)
{
    {
        QMutexLocker lock(&mutex_);

        if (static_cast<int>(entries_.size()) >= maxEntries_)
        {
            entries_.pop_front();
        }

        entries_.push_back(entry);
    }

    emit entryAdded(entry);
}

void LogModel::addLog(LogEntry::Level level, LogCategory category, const QString& message)
{
    addLog(level, QString::fromUtf8(logCategoryName(category)), message);
}

void LogModel::addLog(LogEntry::Level level, const QString& category, const QString& message)
{
    LogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.level = level;
    entry.category = category;
    entry.message = message;
    entry.threadId = static_cast<int>(reinterpret_cast<quintptr>(QThread::currentThreadId()));

    addEntry(entry);
}

std::vector<LogEntry> LogModel::entries(LogEntry::Level minLevel,
                                         const QString& categoryFilter,
                                         const QString& textFilter) const
{
    QMutexLocker lock(&mutex_);

    std::vector<LogEntry> result;
    result.reserve(entries_.size());

    for (const auto& entry : entries_)
    {
        if (entry.level < minLevel)
            continue;

        if (!categoryFilter.isEmpty() && entry.category != categoryFilter)
            continue;

        if (!textFilter.isEmpty() && !entry.message.contains(textFilter, Qt::CaseInsensitive))
            continue;

        result.push_back(entry);
    }

    return result;
}

int LogModel::count() const
{
    QMutexLocker lock(&mutex_);
    return static_cast<int>(entries_.size());
}

void LogModel::clear()
{
    {
        QMutexLocker lock(&mutex_);
        entries_.clear();
    }

    emit cleared();
}

QString LogModel::exportToText(LogEntry::Level minLevel) const
{
    QMutexLocker lock(&mutex_);

    QString result;
    QTextStream stream(&result);

    stream << "=== Harken Studio Log Export ===" << Qt::endl;
    stream << "Exported: " << QDateTime::currentDateTime().toString(Qt::ISODate) << Qt::endl;
    stream << "Total entries: " << entries_.size() << Qt::endl;
    stream << "================================" << Qt::endl << Qt::endl;

    for (const auto& entry : entries_)
    {
        if (entry.level >= minLevel)
        {
            stream << entry.formatted() << Qt::endl;
        }
    }

    return result;
}

bool LogModel::exportToFile(const QString& filePath, LogEntry::Level minLevel) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    stream << exportToText(minLevel);

    return true;
}


LogModel* LogService::globalModel()
{
    static LogModel model;
    return &model;
}

void LogService::log(LogEntry::Level level, LogCategory category, const QString& message)
{
    log(level, QString::fromUtf8(logCategoryName(category)), message);
}

void LogService::log(LogEntry::Level level, const QString& category, const QString& message)
{
    if (auto* model = globalModel())
    {
        model->addLog(level, category, message);
    }
}

LogEntry::Level LogService::engineLevelToPanel(int level)
{
    if (level <= 0)
        return LogEntry::Info;
    if (level == 1)
        return LogEntry::Warning;
    return LogEntry::Error;
}

void LogService::logEngineMessage(int level, const QString& message)
{
    log(engineLevelToPanel(level), LogCategory::Engine, message);
}

void LogService::logInfo(LogCategory category, const QString& message)
{
    log(LogEntry::Info, category, message);
}

void LogService::logInfo(const QString& category, const QString& message)
{
    log(LogEntry::Info, category, message);
}

void LogService::logWarning(LogCategory category, const QString& message)
{
    log(LogEntry::Warning, category, message);
}

void LogService::logWarning(const QString& category, const QString& message)
{
    log(LogEntry::Warning, category, message);
}

void LogService::logError(LogCategory category, const QString& message)
{
    log(LogEntry::Error, category, message);
}

void LogService::logError(const QString& category, const QString& message)
{
    log(LogEntry::Error, category, message);
}

}
