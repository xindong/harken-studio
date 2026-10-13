
#include "ShortcutManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QWidget>

#include <utility>

namespace VNX {

ShortcutManager& ShortcutManager::instance()
{
    static ShortcutManager instance;
    return instance;
}

ShortcutManager::ShortcutManager()
{
    setupDefaultShortcuts();
    loadShortcuts();
}

ShortcutManager::~ShortcutManager()
{
    saveShortcuts();
}

void ShortcutManager::setupDefaultShortcuts()
{
    registerShortcut(Shortcuts::SaveProject, tr("Save Project"), 
        tr("Save the current project"), ShortcutCategory::File, QKeySequence::Save);

    registerShortcut(Shortcuts::Undo, tr("Undo"), 
        tr("Undo last action"), ShortcutCategory::Edit, QKeySequence::Undo);
    registerShortcut(Shortcuts::Redo, tr("Redo"), 
        tr("Redo last undone action"), ShortcutCategory::Edit, QKeySequence::Redo);
    registerShortcut(Shortcuts::Cut, tr("Cut"), 
        tr("Cut selection"), ShortcutCategory::Edit, QKeySequence::Cut);
    registerShortcut(Shortcuts::Copy, tr("Copy"), 
        tr("Copy selection"), ShortcutCategory::Edit, QKeySequence::Copy);
    registerShortcut(Shortcuts::Paste, tr("Paste"), 
        tr("Paste from clipboard"), ShortcutCategory::Edit, QKeySequence::Paste);
    registerShortcut(Shortcuts::Delete, tr("Delete"), 
        tr("Delete selection"), ShortcutCategory::Edit, QKeySequence::Delete);
    registerShortcut(Shortcuts::SelectAll, tr("Select All"), 
        tr("Select all items"), ShortcutCategory::Edit, QKeySequence::SelectAll);
    registerShortcut(Shortcuts::Duplicate, tr("Duplicate"), 
        tr("Duplicate selection"), ShortcutCategory::Edit, QKeySequence("Ctrl+D"));

    registerShortcut(Shortcuts::PlayStop, tr("Play/Stop"),
        tr("Toggle playback"), ShortcutCategory::Playback, QKeySequence("Space"));
    registerShortcut(Shortcuts::PlaybackReset, tr("Reset Container"),
        tr("Reset the selected Container's preview selection memory"),
        ShortcutCategory::Playback, QKeySequence());
    registerShortcut(Shortcuts::AudioSettings, tr("Audio Settings"),
        tr("Configure audio settings"), ShortcutCategory::Tools, QKeySequence());
    registerShortcut(Shortcuts::Preferences, tr("Preferences"),
        tr("Open Preferences"), ShortcutCategory::File, QKeySequence("Ctrl+,"));
}

void ShortcutManager::registerShortcut(const QString& id, 
                                        const QString& displayName,
                                        const QString& description,
                                        ShortcutCategory category,
                                        const QKeySequence& defaultShortcut)
{
    ShortcutInfo info;
    info.id = id;
    info.displayName = displayName;
    info.description = description;
    info.category = category;
    info.defaultShortcut = defaultShortcut;
    info.currentShortcut = defaultShortcut;
    info.isCustom = false;
    
    shortcuts_[id] = info;
}

void ShortcutManager::bindAction(const QString& id, QAction* action)
{
    if (!shortcuts_.contains(id) || !action)
        return;

    ShortcutInfo& info = shortcuts_[id];
    if (!info.actions.contains(action))
    {
        info.actions.append(action);
        action->setShortcut(info.currentShortcut);
        connect(action, &QObject::destroyed, this, [this, id, action]() {
            unbindAction(id, action);
        });
    }
}

QAction* ShortcutManager::bindWidgetAction(const QString& id,
                                            QWidget* widget,
                                            std::function<void()> handler,
                                            Qt::ShortcutContext context)
{
    if (!shortcuts_.contains(id) || !widget)
        return nullptr;

    QAction* action = new QAction(widget);
    action->setShortcutContext(context);
    connect(action, &QAction::triggered, widget,
            [handler = std::move(handler)]() { handler(); });
    widget->addAction(action);
    bindAction(id, action);
    return action;
}

void ShortcutManager::unbindAction(const QString& id, QAction* action)
{
    if (!shortcuts_.contains(id) || !action)
        return;

    shortcuts_[id].actions.removeAll(action);
}

QKeySequence ShortcutManager::shortcut(const QString& id) const
{
    if (!shortcuts_.contains(id))
        return QKeySequence();
    return shortcuts_[id].currentShortcut;
}

void ShortcutManager::setShortcut(const QString& id, const QKeySequence& sequence)
{
    if (!shortcuts_.contains(id))
        return;

    ShortcutInfo& info = shortcuts_[id];
    info.currentShortcut = sequence;
    info.isCustom = (sequence != info.defaultShortcut);
    
    applyShortcut(id);
    emit shortcutChanged(id, sequence);
}

void ShortcutManager::resetToDefault(const QString& id)
{
    if (!shortcuts_.contains(id))
        return;

    ShortcutInfo& info = shortcuts_[id];
    info.currentShortcut = info.defaultShortcut;
    info.isCustom = false;
    
    applyShortcut(id);
    emit shortcutChanged(id, info.currentShortcut);
}

void ShortcutManager::resetAllToDefaults()
{
    for (auto it = shortcuts_.begin(); it != shortcuts_.end(); ++it)
    {
        it.value().currentShortcut = it.value().defaultShortcut;
        it.value().isCustom = false;
        applyShortcut(it.key());
    }
    emit shortcutsReset();
}

ShortcutInfo ShortcutManager::info(const QString& id) const
{
    return shortcuts_.value(id);
}

QList<ShortcutInfo> ShortcutManager::allShortcuts() const
{
    return shortcuts_.values();
}

QList<ShortcutInfo> ShortcutManager::shortcutsInCategory(ShortcutCategory category) const
{
    QList<ShortcutInfo> result;
    for (const auto& info : shortcuts_)
    {
        if (info.category == category)
            result.append(info);
    }
    return result;
}

bool ShortcutManager::hasConflict(const QString& id, const QKeySequence& sequence) const
{
    if (sequence.isEmpty())
        return false;

    for (auto it = shortcuts_.begin(); it != shortcuts_.end(); ++it)
    {
        if (it.key() != id && it.value().currentShortcut == sequence)
            return true;
    }
    return false;
}

QString ShortcutManager::conflictingShortcut(const QString& id, const QKeySequence& sequence) const
{
    if (sequence.isEmpty())
        return QString();

    for (auto it = shortcuts_.begin(); it != shortcuts_.end(); ++it)
    {
        if (it.key() != id && it.value().currentShortcut == sequence)
            return it.key();
    }
    return QString();
}

void ShortcutManager::saveShortcuts()
{
    QSettings settings;
    settings.beginGroup("Shortcuts");
    
    for (auto it = shortcuts_.begin(); it != shortcuts_.end(); ++it)
    {
        if (it.value().isCustom)
        {
            settings.setValue(it.key(), it.value().currentShortcut.toString());
        }
        else
        {
            settings.remove(it.key());
        }
    }
    
    settings.endGroup();
}

QString ShortcutManager::canonicalShortcutId(const QString& id)
{
    static const QMap<QString, QString> kRenames = {
        {QStringLiteral("audition.playStop"), Shortcuts::PlayStop},
        {QStringLiteral("audition.reset"), Shortcuts::PlaybackReset},
    };
    return kRenames.value(id, id);
}

void ShortcutManager::loadShortcuts()
{
    QSettings settings;
    settings.beginGroup("Shortcuts");

    QStringList keys = settings.childKeys();
    for (const QString& key : keys)
    {
        const QString id = canonicalShortcutId(key);
        if (shortcuts_.contains(id))
        {
            QString seqStr = settings.value(key).toString();
            QKeySequence seq(seqStr);
            shortcuts_[id].currentShortcut = seq;
            shortcuts_[id].isCustom = true;
            applyShortcut(id);
        }
    }

    settings.endGroup();
}

void ShortcutManager::exportShortcuts(const QString& filePath)
{
    QJsonObject root;
    
    for (auto it = shortcuts_.begin(); it != shortcuts_.end(); ++it)
    {
        QJsonObject shortcutObj;
        shortcutObj["default"] = it.value().defaultShortcut.toString();
        shortcutObj["current"] = it.value().currentShortcut.toString();
        shortcutObj["isCustom"] = it.value().isCustom;
        root[it.key()] = shortcutObj;
    }

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly))
    {
        QJsonDocument doc(root);
        file.write(doc.toJson(QJsonDocument::Indented));
    }
}

bool ShortcutManager::importShortcuts(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it)
    {
        const QString id = canonicalShortcutId(it.key());
        if (!shortcuts_.contains(id))
            continue;

        QJsonObject obj = it.value().toObject();
        QString currentStr = obj["current"].toString();
        QKeySequence seq(currentStr);
        
        shortcuts_[id].currentShortcut = seq;
        shortcuts_[id].isCustom = (seq != shortcuts_[id].defaultShortcut);
        applyShortcut(id);
    }

    emit shortcutsReset();
    return true;
}

QString ShortcutManager::categoryName(ShortcutCategory category)
{
    switch (category)
    {
        case ShortcutCategory::File: return tr("File");
        case ShortcutCategory::Edit: return tr("Edit");
        case ShortcutCategory::View: return tr("View");
        case ShortcutCategory::Playback: return tr("Playback");
        case ShortcutCategory::Audio: return tr("Audio");
        case ShortcutCategory::Scene3D: return tr("3D Scene");
        case ShortcutCategory::Tools: return tr("Tools");
        case ShortcutCategory::Navigation: return tr("Navigation");
        case ShortcutCategory::Custom: return tr("Custom");
        default: return tr("Unknown");
    }
}

QString ShortcutManager::categoryIcon(ShortcutCategory category)
{
    switch (category)
    {
        case ShortcutCategory::File: return QString::fromUtf8("📁");
        case ShortcutCategory::Edit: return QString::fromUtf8("✏️");
        case ShortcutCategory::View: return QString::fromUtf8("👁");
        case ShortcutCategory::Playback: return QString::fromUtf8("▶️");
        case ShortcutCategory::Audio: return QString::fromUtf8("🔊");
        case ShortcutCategory::Scene3D: return QString::fromUtf8("🎮");
        case ShortcutCategory::Tools: return QString::fromUtf8("🔧");
        case ShortcutCategory::Navigation: return QString::fromUtf8("🧭");
        case ShortcutCategory::Custom: return QString::fromUtf8("⚙️");
        default: return QString::fromUtf8("❓");
    }
}

void ShortcutManager::applyShortcut(const QString& id)
{
    if (!shortcuts_.contains(id))
        return;

    const ShortcutInfo& info = shortcuts_[id];
    for (QAction* action : info.actions)
    {
        if (action)
            action->setShortcut(info.currentShortcut);
    }
}

}
