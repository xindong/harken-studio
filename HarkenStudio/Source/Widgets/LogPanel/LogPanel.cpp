
#include "LogPanel.h"
#include "Monitor/MonitorTypes.h"
#include "../../Core/AppSettings.h"
#include "../../Core/FontManager.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/ThemeAssetCatalog.h"
#include "../Common/SearchField.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QApplication>
#include <QFileDialog>
#include <QScrollBar>
#include <QTextBlock>
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

QColor LogEntry::levelColor(Level level)
{
    const auto theme = ThemeManager::instance().snapshot();
    const auto& logColors = theme->domains.log;
    switch (level)
    {
        case Trace:    return logColors.debug;
        case Debug:    return logColors.debug;
        case Info:     return logColors.info;
        case Warning:  return logColors.warning;
        case Error:    return logColors.error;
        case Critical: return logColors.critical;
        default:       return logColors.info;
    }
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


LogTextEdit::LogTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setObjectName("logTextView");
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMaximumBlockCount(10000);

    int fontSize = AppSettings::instance().panelFontSize(PanelId::Log);
    setFont(FontManager::instance().getFont(FontRole::Body, fontSize));
}

void LogTextEdit::appendEntry(const LogEntry& entry)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    
    cursor.insertText(QString("[%1] ").arg(entry.timestamp.toString("hh:mm:ss.zzz")),
                      formatForLevel(LogEntry::Debug));
    
    cursor.insertText(QString("[%1] ").arg(LogEntry::levelToString(entry.level), -5),
                      formatForLevel(entry.level));
    
    QTextCharFormat catFormat;
    catFormat.setForeground(ThemeManager::instance().colors().info);
    cursor.insertText(QString("[%1] ").arg(entry.category, -8), catFormat);
    
    cursor.insertText(entry.message + "\n", formatForLevel(entry.level));
    
    if (autoScroll_)
    {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }
}

void LogTextEdit::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = createStandardContextMenu();
    menu->addSeparator();
    
    QAction* clearAction = menu->addAction(tr("Clear Log"));
    connect(clearAction, &QAction::triggered, this, &QPlainTextEdit::clear);
    
    QAction* autoScrollAction = menu->addAction(tr("Auto Scroll"));
    autoScrollAction->setCheckable(true);
    autoScrollAction->setChecked(autoScroll_);
    connect(autoScrollAction, &QAction::toggled, this, &LogTextEdit::setAutoScroll);
    
    menu->exec(event->globalPos());
    delete menu;
}

QTextCharFormat LogTextEdit::formatForLevel(LogEntry::Level level) const
{
    QTextCharFormat format;
    format.setForeground(LogEntry::levelColor(level));
    
    if (level >= LogEntry::Error)
    {
        format.setFontWeight(QFont::Bold);
    }
    
    return format;
}


LogPanel* LogPanel::instance_ = nullptr;

LogPanel::LogPanel(QWidget* parent)
    : QWidget(parent)
    , model_(new LogModel(this))
    , flushTimer_(new QTimer(this))
{
    setObjectName("LogPanel");
    instance_ = this;
    
    setupUI();
    setupConnections();
    refreshIcons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() { refreshIcons(); });
    
    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged,
            this, &LogPanel::onFontSettingsChanged);
    
    int fontSize = AppSettings::instance().panelFontSize(PanelId::Log);
    logTextEdit_->setFont(FontManager::instance().getFont(FontRole::Body, fontSize));
    
    flushTimer_->setInterval(50);
    connect(flushTimer_, &QTimer::timeout, this, &LogPanel::onFlushTimer);
    flushTimer_->start();
    
    model_->info(LogCategory::GUI, "Log panel initialized");
}

LogPanel::~LogPanel()
{
    if (instance_ == this)
        instance_ = nullptr;
}

LogPanel* LogPanel::instance()
{
    return instance_;
}

LogModel* LogPanel::globalModel()
{
    return instance_ ? instance_->model_ : nullptr;
}

void LogPanel::log(LogEntry::Level level, LogCategory category, const QString& message)
{
    log(level, QString::fromUtf8(logCategoryName(category)), message);
}

void LogPanel::log(LogEntry::Level level, const QString& category, const QString& message)
{
    if (auto* model = globalModel())
    {
        model->addLog(level, category, message);
    }
}

LogEntry::Level LogPanel::engineLevelToPanel(int level)
{
    // VnLogFn levels: VNX::Monitor::kLogInfo=0, VNX::Monitor::kLogWarn=1, VNX::Monitor::kLogError=2.
    if (level <= 0)
        return LogEntry::Info;
    if (level == 1)
        return LogEntry::Warning;
    return LogEntry::Error;
}

void LogPanel::logEngineMessage(int level, const QString& message)
{
    log(engineLevelToPanel(level), LogCategory::Engine, message);
}

void LogPanel::logInfo(LogCategory category, const QString& message)
{
    log(LogEntry::Info, category, message);
}

void LogPanel::logInfo(const QString& category, const QString& message)
{
    log(LogEntry::Info, category, message);
}

void LogPanel::logWarning(LogCategory category, const QString& message)
{
    log(LogEntry::Warning, category, message);
}

void LogPanel::logWarning(const QString& category, const QString& message)
{
    log(LogEntry::Warning, category, message);
}

void LogPanel::logError(LogCategory category, const QString& message)
{
    log(LogEntry::Error, category, message);
}

void LogPanel::logError(const QString& category, const QString& message)
{
    log(LogEntry::Error, category, message);
}

void LogPanel::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);

    levelCombo_ = new QComboBox();
    levelCombo_->setObjectName("logLevelCombo");
    levelCombo_->setToolTip(tr("Filter by level"));
    levelCombo_->addItem(tr("All Levels"), static_cast<int>(LogEntry::Trace));
    levelCombo_->addItem(tr("Debug+"), static_cast<int>(LogEntry::Debug));
    levelCombo_->addItem(tr("Info+"), static_cast<int>(LogEntry::Info));
    levelCombo_->addItem(tr("Warning+"), static_cast<int>(LogEntry::Warning));
    levelCombo_->addItem(tr("Error+"), static_cast<int>(LogEntry::Error));
    levelCombo_->setCurrentIndex(0);
    toolbarLayout->addWidget(levelCombo_);

    categoryCombo_ = new QComboBox();
    categoryCombo_->setObjectName("logCategoryCombo");
    categoryCombo_->setToolTip(tr("Filter by category"));
    categoryCombo_->addItem(tr("All Categories"), QString());
    for (int i = 0; i < logCategoryCount(); ++i)
    {
        const char* name = logCategoryName(logCategoryAt(i));
        categoryCombo_->addItem(QString::fromUtf8(name), QString::fromUtf8(name));
    }
    toolbarLayout->addWidget(categoryCombo_);

    searchEdit_ = new SearchField();
    searchEdit_->setObjectName("logSearchEdit");
    searchEdit_->setMaximumWidth(240);
    toolbarLayout->addWidget(searchEdit_, 1);

    toolbarLayout->addStretch();

    countLabel_ = new QLabel(tr("0 entries"));
    countLabel_->setObjectName("logCountLabel");
    toolbarLayout->addWidget(countLabel_);

    auto makeToolButton = [](const QString& objName, const QString& tooltip) {
        QToolButton* btn = new QToolButton();
        btn->setObjectName(objName);
        btn->setProperty("flatIconButton", true);
        btn->setToolTip(tooltip);
        btn->setIconSize(QSize(16, 16));
        btn->setFixedSize(26, 26);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setAttribute(Qt::WA_Hover, true);
        return btn;
    };

    scrollBtn_ = makeToolButton("logAutoScrollBtn", tr("Auto-scroll to bottom"));
    scrollBtn_->setCheckable(true);
    scrollBtn_->setChecked(true);
    toolbarLayout->addWidget(scrollBtn_);

    copyBtn_ = makeToolButton("logCopyBtn", tr("Copy to clipboard"));
    toolbarLayout->addWidget(copyBtn_);

    exportBtn_ = makeToolButton("logExportBtn", tr("Export to file"));
    toolbarLayout->addWidget(exportBtn_);

    clearBtn_ = makeToolButton("logClearBtn", tr("Clear all logs"));
    toolbarLayout->addWidget(clearBtn_);

    mainLayout->addLayout(toolbarLayout);

    logTextEdit_ = new LogTextEdit();
    mainLayout->addWidget(logTextEdit_);
}

void LogPanel::refreshIcons()
{
    auto& theme = ThemeManager::instance();
    scrollBtn_->setIcon(theme.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::LogScrollToBottom)).icon);
    copyBtn_->setIcon(theme.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::LogCopy)).icon);
    exportBtn_->setIcon(theme.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::LogExport)).icon);
    clearBtn_->setIcon(theme.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::LogClear)).icon);
}

void LogPanel::setupConnections()
{
    connect(model_, &LogModel::entryAdded, this, &LogPanel::onEntryAdded, Qt::QueuedConnection);
    connect(model_, &LogModel::cleared, logTextEdit_, &QPlainTextEdit::clear, Qt::QueuedConnection);
    connect(model_, &LogModel::cleared, this, &LogPanel::updateCountLabel, Qt::QueuedConnection);
    
    connect(levelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogPanel::onLevelChanged);
    connect(categoryCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogPanel::onCategoryChanged);
    connect(searchEdit_, &QLineEdit::textChanged,
            this, &LogPanel::onSearchTextChanged);
    
    connect(clearBtn_, &QToolButton::clicked, this, &LogPanel::clear);
    connect(copyBtn_, &QToolButton::clicked, this, &LogPanel::copyToClipboard);
    connect(exportBtn_, &QToolButton::clicked, this, &LogPanel::exportLog);
    connect(scrollBtn_, &QToolButton::toggled, [this](bool checked) {
        logTextEdit_->setAutoScroll(checked);
        if (checked) scrollToBottom();
    });
}

void LogPanel::onEntryAdded(const LogEntry& entry)
{
    QMutexLocker lock(&pendingMutex_);
    pendingEntries_.push_back(entry);
    
    if (entry.level == LogEntry::Error || entry.level == LogEntry::Critical)
    {
        emit errorLogged(entry.message);
    }
    else if (entry.level == LogEntry::Warning)
    {
        emit warningLogged(entry.message);
    }
}

void LogPanel::onFlushTimer()
{
    std::vector<LogEntry> entries;
    {
        QMutexLocker lock(&pendingMutex_);
        entries.swap(pendingEntries_);
    }
    
    if (entries.empty())
        return;
    
    for (const auto& entry : entries)
    {
        if (matchesFilter(entry))
        {
            logTextEdit_->appendEntry(entry);
        }
    }
    
    updateCountLabel();
}

void LogPanel::onFilterChanged()
{
    applyFilter();
}

void LogPanel::onLevelChanged(int index)
{
    minLevel_ = static_cast<LogEntry::Level>(levelCombo_->itemData(index).toInt());
    applyFilter();
}

void LogPanel::onCategoryChanged(int index)
{
    categoryFilter_ = categoryCombo_->itemData(index).toString();
    applyFilter();
}

void LogPanel::onSearchTextChanged(const QString& text)
{
    textFilter_ = text;
    applyFilter();
}

void LogPanel::clear()
{
    model_->clear();
    logTextEdit_->clear();
    updateCountLabel();
}

void LogPanel::copyToClipboard()
{
    QString text = logTextEdit_->toPlainText();
    if (!text.isEmpty())
    {
        QApplication::clipboard()->setText(text);
        model_->info(LogCategory::GUI, "Log copied to clipboard");
    }
}

void LogPanel::exportLog()
{
    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Export Log"),
        QString("harken_log_%1.txt").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")),
        tr("Text Files (*.txt);;All Files (*)")
    );
    
    if (fileName.isEmpty())
        return;
    
    if (model_->exportToFile(fileName, minLevel_))
    {
        model_->info(LogCategory::File, QString("Log exported to: %1").arg(fileName));
    }
    else
    {
        model_->error(LogCategory::File, QString("Failed to export log to: %1").arg(fileName));
    }
}

void LogPanel::setFilter(const QString& filter)
{
    searchEdit_->setText(filter);
}

void LogPanel::setMinLevel(LogEntry::Level level)
{
    for (int i = 0; i < levelCombo_->count(); ++i)
    {
        if (levelCombo_->itemData(i).toInt() == static_cast<int>(level))
        {
            levelCombo_->setCurrentIndex(i);
            return;
        }
    }
}

void LogPanel::setCategory(const QString& category)
{
    for (int i = 0; i < categoryCombo_->count(); ++i)
    {
        if (categoryCombo_->itemData(i).toString() == category)
        {
            categoryCombo_->setCurrentIndex(i);
            return;
        }
    }
}

void LogPanel::scrollToBottom()
{
    logTextEdit_->verticalScrollBar()->setValue(
        logTextEdit_->verticalScrollBar()->maximum()
    );
}

void LogPanel::refreshDisplay()
{
    applyFilter();
}

void LogPanel::applyFilter()
{
    logTextEdit_->clear();
    
    auto entries = model_->entries(minLevel_, categoryFilter_, textFilter_);
    
    for (const auto& entry : entries)
    {
        logTextEdit_->appendEntry(entry);
    }
    
    updateCountLabel();
}

bool LogPanel::matchesFilter(const LogEntry& entry) const
{
    if (entry.level < minLevel_)
        return false;
    
    if (!categoryFilter_.isEmpty() && entry.category != categoryFilter_)
        return false;
    
    if (!textFilter_.isEmpty() && 
        !entry.message.contains(textFilter_, Qt::CaseInsensitive))
        return false;
    
    return true;
}

void LogPanel::updateCountLabel()
{
    int total = model_->count();
    int displayed = logTextEdit_->document()->blockCount() - 1;
    
    if (total == displayed || displayed < 0)
    {
        countLabel_->setText(tr("%1 entries").arg(total));
    }
    else
    {
        countLabel_->setText(tr("%1 / %2 entries").arg(displayed).arg(total));
    }
}

void LogPanel::onFontSettingsChanged(PanelId panelId, int size)
{
    if (panelId == PanelId::Log)
    {
        logTextEdit_->setFont(FontManager::instance().getFont(FontRole::Body, size));
    }
}

}
