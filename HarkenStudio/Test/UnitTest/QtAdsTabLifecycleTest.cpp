#include <DockAreaTabBar.h>
#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>

#include <QApplication>
#include <QEvent>
#include <QMainWindow>
#include <QPointer>
#include <QScrollBar>

#include <gtest/gtest.h>

#include <memory>

namespace {

void drainEvents()
{
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

class QtAdsTabLifecycleTest : public testing::Test
{
protected:
    void SetUp() override
    {
        previousFlags_ = ads::CDockManager::configFlags();
        ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
        window_ = std::make_unique<QMainWindow>();
        window_->resize(500, 300);
        manager_ = new ads::CDockManager(window_.get());
    }

    void TearDown() override
    {
        window_.reset();
        drainEvents();
        ads::CDockManager::setConfigFlags(previousFlags_);
    }

    ads::CDockWidget* addDock(const QString& name, ads::CDockAreaWidget* area = nullptr)
    {
        auto* dock = new ads::CDockWidget(name);
        dock->setWidget(new QWidget());
        manager_->addDockWidget(
            area ? ads::CenterDockWidgetArea : ads::LeftDockWidgetArea, dock, area);
        return dock;
    }

    ads::CDockManager::ConfigFlags previousFlags_;
    std::unique_ptr<QMainWindow> window_;
    ads::CDockManager* manager_ = nullptr;
};

}

TEST_F(QtAdsTabLifecycleTest, RemovedTabCanBeDestroyedBeforeDeferredScroll)
{
    auto* removed = addDock(QStringLiteral("Removed"));
    auto* area = removed->dockAreaWidget();
    auto* remaining = addDock(QStringLiteral("Remaining"), area);
    auto* tabBar = area->titleBar()->tabBar();
    area->setCurrentDockWidget(removed);
    QPointer<ads::CDockWidgetTab> removedTab = removed->tabWidget();

    manager_->removeDockWidget(removed);
    delete removed;

    ASSERT_TRUE(removedTab.isNull());
    ASSERT_EQ(tabBar->currentTab(), remaining->tabWidget());
    drainEvents();
    EXPECT_EQ(tabBar->currentTab(), remaining->tabWidget());
}

TEST_F(QtAdsTabLifecycleTest, LastTabCanBeDestroyedBeforeDeferredScroll)
{
    auto* removed = addDock(QStringLiteral("Removed"));
    QPointer<ads::CDockAreaTabBar> tabBar = removed->dockAreaWidget()->titleBar()->tabBar();
    QPointer<ads::CDockWidgetTab> removedTab = removed->tabWidget();

    manager_->removeDockWidget(removed);
    delete removed;

    ASSERT_TRUE(removedTab.isNull());
    ASSERT_FALSE(tabBar.isNull());
    EXPECT_EQ(tabBar->count(), 0);
    EXPECT_EQ(tabBar->currentTab(), nullptr);
    drainEvents();
    EXPECT_TRUE(tabBar.isNull());
}

TEST_F(QtAdsTabLifecycleTest, WindowCanBeDestroyedWithPendingTabScroll)
{
    auto* dock = addDock(QStringLiteral("Pending"));
    QPointer<ads::CDockAreaTabBar> tabBar = dock->dockAreaWidget()->titleBar()->tabBar();
    QPointer<ads::CDockWidgetTab> tab = dock->tabWidget();

    window_.reset();

    EXPECT_TRUE(tabBar.isNull());
    EXPECT_TRUE(tab.isNull());
    drainEvents();
}

TEST_F(QtAdsTabLifecycleTest, CurrentTabRemainsVisibleAfterRapidChanges)
{
    auto* first = addDock(QStringLiteral("First"));
    auto* area = first->dockAreaWidget();
    first->tabWidget()->setFixedWidth(140);
    auto* last = first;
    for (int index = 0; index < 8; ++index)
    {
        last = addDock(QStringLiteral("Tab%1").arg(index), area);
        last->tabWidget()->setFixedWidth(140);
    }
    window_->show();
    area->setCurrentDockWidget(first);
    drainEvents();
    auto* tabBar = area->titleBar()->tabBar();
    ASSERT_GT(tabBar->horizontalScrollBar()->maximum(), 0);

    area->setCurrentDockWidget(last);
    area->setCurrentDockWidget(first);
    area->setCurrentDockWidget(last);
    drainEvents();

    EXPECT_EQ(tabBar->currentTab(), last->tabWidget());
    const int left = last->tabWidget()->mapTo(tabBar->viewport(), QPoint()).x();
    EXPECT_GE(left, 0);
    EXPECT_LE(left + last->tabWidget()->width(), tabBar->viewport()->width());
}
