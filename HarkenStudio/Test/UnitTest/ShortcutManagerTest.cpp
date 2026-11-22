#include "Core/ShortcutManager.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QWidget>
#include <gtest/gtest.h>

TEST(ShortcutManager, DefaultAliasesFollowRemappingClearingAndReset)
{
    auto& manager = harken::studio::ShortcutManager::instance();
    const auto id = harken::studio::Shortcuts::ZoomIn;
    const auto original = manager.info(id);
    const auto restore = qScopeGuard([&] {
        if (original.isCustom) manager.setShortcut(id, original.currentShortcut);
        else manager.resetToDefault(id);
    });
    QAction action;
    manager.bindAction(id, &action);
    manager.resetToDefault(id);
    const QList<QKeySequence> defaults{QKeySequence(QStringLiteral("Ctrl+=")),
                                      QKeySequence(QStringLiteral("Ctrl++"))};
    EXPECT_EQ(action.shortcuts(), defaults);
    manager.setShortcut(id, QKeySequence(QStringLiteral("Ctrl+Alt+=")));
    EXPECT_EQ(action.shortcuts(), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Alt+="))});
    manager.setShortcut(id, {});
    EXPECT_TRUE(action.shortcuts().isEmpty());
    manager.resetToDefault(id);
    EXPECT_EQ(action.shortcuts(), defaults);
}

TEST(ShortcutManager, DefaultAliasesParticipateInAllConflicts)
{
    auto& manager = harken::studio::ShortcutManager::instance();
    const QStringList ids{harken::studio::Shortcuts::ZoomIn, harken::studio::Shortcuts::AudioSettings, harken::studio::Shortcuts::Preferences};
    QList<harken::studio::ShortcutInfo> originals;
    for (const auto& id : ids) originals.append(manager.info(id));
    const auto restore = qScopeGuard([&] {
        for (const auto& info : originals) {
            if (info.isCustom) manager.setShortcut(info.id, info.currentShortcut);
            else manager.resetToDefault(info.id);
        }
    });
    manager.resetToDefault(harken::studio::Shortcuts::ZoomIn);
    EXPECT_EQ(manager.conflictingShortcut(harken::studio::Shortcuts::AudioSettings,
        QKeySequence(QStringLiteral("Ctrl++"))), harken::studio::Shortcuts::ZoomIn);
    manager.setShortcut(harken::studio::Shortcuts::ZoomIn, {});
    manager.setShortcut(harken::studio::Shortcuts::AudioSettings, QKeySequence(QStringLiteral("Ctrl+=")));
    manager.setShortcut(harken::studio::Shortcuts::Preferences, QKeySequence(QStringLiteral("Ctrl++")));
    const auto conflicts = manager.conflictingShortcuts(harken::studio::Shortcuts::ZoomIn,
        QKeySequence(QStringLiteral("Ctrl+=")));
    EXPECT_EQ(conflicts.size(), 2);
    EXPECT_TRUE(conflicts.contains(harken::studio::Shortcuts::AudioSettings));
    EXPECT_TRUE(conflicts.contains(harken::studio::Shortcuts::Preferences));
    EXPECT_FALSE(manager.hasConflict(harken::studio::Shortcuts::ZoomIn, {}));
}

TEST(ShortcutManager, MenuDoesNotDispatchToDestroyedTarget)
{
    auto& manager = harken::studio::ShortcutManager::instance();
    const QString id = harken::studio::Shortcuts::Copy;
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* second = new QWidget(&window);
    second->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(second);
    second->setMinimumSize(100, 50);
    int secondCount = 0;
    manager.bindWidgetAction(id, second, [&]() { ++secondCount; });
    QMenu menu(&window);
    QAction* menuAction = manager.createMenuAction(id, &menu);
    window.show();
    window.activateWindow();
    second->setFocus();
    QApplication::processEvents();
    ASSERT_EQ(QApplication::focusWidget(), second);
    menu.aboutToShow();
    ASSERT_TRUE(menuAction->isEnabled());

    delete second;
    menuAction->trigger();
    EXPECT_EQ(secondCount, 0);
}
