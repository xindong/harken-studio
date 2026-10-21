
#include "ShortcutManager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

#include <utility>

namespace VNX {

namespace {

template<typename Edit>
std::function<void()> textEditCommand(const QString& id, Edit* edit)
{
    const bool selected = edit->textCursor().hasSelection();
    if (id == Shortcuts::Copy && selected)
        return [edit]() { edit->copy(); };
    if (id == Shortcuts::SelectAll && !edit->document()->isEmpty())
        return [edit]() { edit->selectAll(); };
    if (edit->isReadOnly())
        return {};
    if (id == Shortcuts::Cut && selected)
        return [edit]() { edit->cut(); };
    if (id == Shortcuts::Paste && edit->canPaste())
        return [edit]() { edit->paste(); };
    if (id == Shortcuts::Delete && selected)
        return [edit]() {
            auto cursor = edit->textCursor();
            cursor.removeSelectedText();
            edit->setTextCursor(cursor);
        };
    return {};
}

}

ShortcutManager& ShortcutManager::instance()
{
    static ShortcutManager instance;
    return instance;
}

ShortcutManager::ShortcutManager()
{
    setupDefaultShortcuts();
    loadShortcuts();
    focusedWidget_ = QApplication::focusWidget();
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* focus) {
        if (focus && !qobject_cast<QMenu*>(focus) && !qobject_cast<QMenuBar*>(focus))
            focusedWidget_ = focus;
    });
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

QAction* ShortcutManager::createMenuAction(const QString& id, QMenu* menu)
{
    if (!shortcuts_.contains(id) || !menu)
        return nullptr;

    QAction* action = menu->addAction(shortcuts_[id].displayName);
    action->setObjectName(id);
    action->setMenuRole(QAction::NoRole);
    const auto refresh = [this, id, action]() {
        const auto& info = shortcuts_[id];
        const auto command = menuCommand(id, menuTarget_);
        const QString hint = command.shortcut.toString(QKeySequence::NativeText);
        action->setText(hint.isEmpty() ? info.displayName
                                      : info.displayName + QLatin1Char('\t') + hint);
        action->setEnabled(bool(command.handler));
    };
    refresh();
    connect(this, &ShortcutManager::shortcutChanged, action,
            [id, refresh](const QString& changedId, const QKeySequence&) {
        if (changedId == id)
            refresh();
    });
    connect(this, &ShortcutManager::shortcutsReset, action, refresh);
    connect(menu, &QMenu::aboutToShow, action, [this, refresh]() {
        menuTarget_ = focusedWidget_;
        refresh();
    });
    connect(action, &QAction::triggered, action, [this, id]() {
        if (auto command = menuCommand(id, menuTarget_); command.handler)
            command.handler();
    });
    return action;
}

ShortcutManager::MenuCommand ShortcutManager::menuCommand(const QString& id, QWidget* focus) const
{
    const auto& info = shortcuts_[id];
    const MenuCommand unavailable{{}, info.currentShortcut};
    if (!focus || !focus->isVisible() || !focus->isEnabled())
        return unavailable;

    const auto nativeCommand = [&info](std::function<void()> handler) -> MenuCommand {
        return {std::move(handler), info.defaultShortcut};
    };
    for (QWidget* widget = focus; widget; widget = widget->parentWidget()) {
        if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
            const bool selected = edit->hasSelectedText();
            const bool canCopy = selected && edit->echoMode() == QLineEdit::Normal;
            if (id == Shortcuts::Copy && canCopy)
                return nativeCommand([edit]() { edit->copy(); });
            if (id == Shortcuts::SelectAll && !edit->text().isEmpty())
                return nativeCommand([edit]() { edit->selectAll(); });
            if (edit->isReadOnly())
                return unavailable;
            if (id == Shortcuts::Cut && canCopy)
                return nativeCommand([edit]() { edit->cut(); });
            if (id == Shortcuts::Paste && !QApplication::clipboard()->text().isEmpty())
                return nativeCommand([edit]() { edit->paste(); });
            if (id == Shortcuts::Delete && selected)
                return nativeCommand([edit]() { edit->del(); });
            return unavailable;
        }
        if (auto* edit = qobject_cast<QTextEdit*>(widget))
            return nativeCommand(textEditCommand(id, edit));
        if (auto* edit = qobject_cast<QPlainTextEdit*>(widget))
            return nativeCommand(textEditCommand(id, edit));

        for (QAction* action : widget->actions()) {
            if (!info.actions.contains(action))
                continue;
            if (action->shortcutContext() == Qt::WidgetShortcut
                && widget != focus && widget->focusProxy() != focus)
                continue;
            if (!action->isEnabled() || !action->isVisible())
                return unavailable;
            return {[action]() { action->trigger(); }, action->shortcut()};
        }
        if (id == Shortcuts::SelectAll) {
            if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
                if (view->selectionMode() != QAbstractItemView::NoSelection
                    && view->selectionMode() != QAbstractItemView::SingleSelection
                    && view->model() && view->model()->rowCount(view->rootIndex()) > 0)
                    return nativeCommand([view]() { view->selectAll(); });
                return unavailable;
            }
        }
        if (widget->isWindow())
            break;
    }
    return unavailable;
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
