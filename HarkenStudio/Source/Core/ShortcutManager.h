
#pragma once

#include <QObject>
#include <QKeySequence>
#include <QAction>
#include <QString>
#include <QMap>
#include <QList>
#include <QSettings>
#include <QPointer>
#include <functional>
#include <memory>

class QWidget;
class QMenu;

namespace VNX {

enum class ShortcutCategory
{
    File,
    Edit,
    View,
    Playback,
    Audio,
    Scene3D,
    Tools,
    Navigation,
    Custom
};

struct ShortcutInfo
{
    QString id;
    QString displayName;
    QString description;
    ShortcutCategory category;
    QKeySequence defaultShortcut;
    QKeySequence currentShortcut;
    bool isCustom = false;
    QList<QAction*> actions;
};

class ShortcutManager : public QObject
{
    Q_OBJECT

public:
    static ShortcutManager& instance();

    void registerShortcut(const QString& id, 
                         const QString& displayName,
                         const QString& description,
                         ShortcutCategory category,
                         const QKeySequence& defaultShortcut);
    
    void bindAction(const QString& id, QAction* action);
    QAction* bindWidgetAction(const QString& id,
                              QWidget* widget,
                              std::function<void()> handler,
                              Qt::ShortcutContext context = Qt::WidgetShortcut);
    QAction* createMenuAction(const QString& id, QMenu* menu);
    void unbindAction(const QString& id, QAction* action);

    QKeySequence shortcut(const QString& id) const;
    void setShortcut(const QString& id, const QKeySequence& sequence);
    void resetToDefault(const QString& id);
    void resetAllToDefaults();

    ShortcutInfo info(const QString& id) const;
    QList<ShortcutInfo> allShortcuts() const;
    QList<ShortcutInfo> shortcutsInCategory(ShortcutCategory category) const;
    
    bool hasConflict(const QString& id, const QKeySequence& sequence) const;
    QString conflictingShortcut(const QString& id, const QKeySequence& sequence) const;

    void saveShortcuts();
    void loadShortcuts();
    void exportShortcuts(const QString& filePath);
    bool importShortcuts(const QString& filePath);

    static QString categoryName(ShortcutCategory category);
    static QString categoryIcon(ShortcutCategory category);

signals:
    void shortcutChanged(const QString& id, const QKeySequence& newShortcut);
    void shortcutsReset();

private:
    struct MenuCommand
    {
        std::function<void()> handler;
        QKeySequence shortcut;
    };

    ShortcutManager();
    ~ShortcutManager() override;
    Q_DISABLE_COPY(ShortcutManager)

    void setupDefaultShortcuts();
    void applyShortcut(const QString& id);
    MenuCommand menuCommand(const QString& id, QWidget* focus) const;
    static QString canonicalShortcutId(const QString& id);

private:
    QMap<QString, ShortcutInfo> shortcuts_;
    QPointer<QWidget> focusedWidget_;
    QPointer<QWidget> menuTarget_;
};

namespace Shortcuts {

inline const QString SaveProject = "file.save";

inline const QString Undo = "edit.undo";
inline const QString Redo = "edit.redo";
inline const QString Cut = "edit.cut";
inline const QString Copy = "edit.copy";
inline const QString Paste = "edit.paste";
inline const QString Delete = "edit.delete";
inline const QString SelectAll = "edit.selectAll";
inline const QString Duplicate = "edit.duplicate";

inline const QString PlayStop = "playback.playStop";
inline const QString PlaybackReset = "playback.reset";

inline const QString AudioSettings = "tools.audioSettings";
inline const QString Preferences = "file.preferences";

}

}
