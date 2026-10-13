#include "Core/ShortcutManager.h"

#include <QAction>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QWidget>
#include <gtest/gtest.h>

#include <utility>

namespace {

class ShortcutRestore
{
public:
    ShortcutRestore(VNX::ShortcutManager& manager, QString id)
        : manager_(manager), id_(std::move(id)), shortcut_(manager.shortcut(id_))
    {
    }

    ~ShortcutRestore()
    {
        manager_.setShortcut(id_, shortcut_);
    }

private:
    VNX::ShortcutManager& manager_;
    QString id_;
    QKeySequence shortcut_;
};

}

TEST(ShortcutManager, RegistersSupportedCommandsOnly)
{
    VNX::ShortcutManager& manager = VNX::ShortcutManager::instance();
    QSet<QString> ids;
    for (const VNX::ShortcutInfo& info : manager.allShortcuts())
        ids.insert(info.id);

    EXPECT_EQ(ids, QSet<QString>({
        VNX::Shortcuts::SaveProject,
        VNX::Shortcuts::Undo,
        VNX::Shortcuts::Redo,
        VNX::Shortcuts::Cut,
        VNX::Shortcuts::Copy,
        VNX::Shortcuts::Paste,
        VNX::Shortcuts::Delete,
        VNX::Shortcuts::SelectAll,
        VNX::Shortcuts::Duplicate,
        VNX::Shortcuts::PlayStop,
        VNX::Shortcuts::PlaybackReset,
        VNX::Shortcuts::AudioSettings,
        VNX::Shortcuts::Preferences
    }));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.new")));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.open")));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.saveAs")));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.quit")));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Preferences).defaultShortcut,
              QKeySequence(QStringLiteral("Ctrl+,")));
    EXPECT_EQ(manager.info(VNX::Shortcuts::SaveProject).defaultShortcut,
              QKeySequence(QKeySequence::Save));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Undo).defaultShortcut,
              QKeySequence(QKeySequence::Undo));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Redo).defaultShortcut,
              QKeySequence(QKeySequence::Redo));
    EXPECT_EQ(manager.info(VNX::Shortcuts::PlayStop).defaultShortcut,
              QKeySequence(QStringLiteral("Space")));
    EXPECT_EQ(manager.info(VNX::Shortcuts::PlayStop).category,
              VNX::ShortcutCategory::Playback);
    EXPECT_EQ(manager.info(VNX::Shortcuts::PlaybackReset).category,
              VNX::ShortcutCategory::Playback);
    EXPECT_EQ(manager.info(VNX::Shortcuts::Cut).defaultShortcut,
              QKeySequence(QKeySequence::Cut));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Copy).defaultShortcut,
              QKeySequence(QKeySequence::Copy));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Paste).defaultShortcut,
              QKeySequence(QKeySequence::Paste));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Delete).defaultShortcut,
              QKeySequence(QKeySequence::Delete));
}

TEST(ShortcutManager, ImportMapsLegacyAuditionIds)
{
    VNX::ShortcutManager& manager = VNX::ShortcutManager::instance();
    ShortcutRestore playStop(manager, VNX::Shortcuts::PlayStop);
    ShortcutRestore reset(manager, VNX::Shortcuts::PlaybackReset);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("shortcuts.json"));

    QJsonObject root;
    root.insert(QStringLiteral("audition.playStop"),
                QJsonObject{{QStringLiteral("current"), QStringLiteral("F5")}});
    root.insert(QStringLiteral("audition.reset"),
                QJsonObject{{QStringLiteral("current"), QStringLiteral("F6")}});
    root.insert(QStringLiteral("removed.command"),
                QJsonObject{{QStringLiteral("current"), QStringLiteral("F7")}});

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QJsonDocument(root).toJson()), 0);
    file.close();

    ASSERT_TRUE(manager.importShortcuts(path));
    EXPECT_EQ(manager.shortcut(VNX::Shortcuts::PlayStop), QKeySequence(QStringLiteral("F5")));
    EXPECT_TRUE(manager.info(VNX::Shortcuts::PlayStop).isCustom);
    EXPECT_EQ(manager.shortcut(VNX::Shortcuts::PlaybackReset), QKeySequence(QStringLiteral("F6")));
    EXPECT_TRUE(manager.info(VNX::Shortcuts::PlaybackReset).isCustom);
}

TEST(ShortcutManager, RebindUpdatesAllWidgetConsumersAndDropsDestroyedActions)
{
    VNX::ShortcutManager& manager = VNX::ShortcutManager::instance();
    ShortcutRestore restore(manager, VNX::Shortcuts::SaveProject);
    const qsizetype initialActionCount =
        manager.info(VNX::Shortcuts::SaveProject).actions.size();
    const QKeySequence replacement(QStringLiteral("Ctrl+Alt+Shift+F12"));

    {
        QWidget firstWidget;
        QWidget secondWidget;
        int firstTriggerCount = 0;
        int secondTriggerCount = 0;
        QAction* firstAction = manager.bindWidgetAction(
            VNX::Shortcuts::SaveProject, &firstWidget, [&firstTriggerCount]() {
                ++firstTriggerCount;
            });
        QAction* secondAction = manager.bindWidgetAction(
            VNX::Shortcuts::SaveProject, &secondWidget, [&secondTriggerCount]() {
                ++secondTriggerCount;
            });

        ASSERT_NE(firstAction, nullptr);
        ASSERT_NE(secondAction, nullptr);
        EXPECT_EQ(manager.info(VNX::Shortcuts::SaveProject).actions.size(),
                  initialActionCount + 2);

        manager.setShortcut(VNX::Shortcuts::SaveProject, replacement);
        EXPECT_EQ(firstAction->shortcut(), replacement);
        EXPECT_EQ(secondAction->shortcut(), replacement);

        firstAction->trigger();
        secondAction->trigger();
        EXPECT_EQ(firstTriggerCount, 1);
        EXPECT_EQ(secondTriggerCount, 1);
    }

    EXPECT_EQ(manager.info(VNX::Shortcuts::SaveProject).actions.size(),
              initialActionCount);
}
