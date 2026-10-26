#include "Core/ShortcutManager.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>
#include <gtest/gtest.h>

TEST(ShortcutManager, MenuDoesNotDispatchToDestroyedTarget)
{
    auto& manager = VNX::ShortcutManager::instance();
    const QString id = VNX::Shortcuts::Copy;
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
