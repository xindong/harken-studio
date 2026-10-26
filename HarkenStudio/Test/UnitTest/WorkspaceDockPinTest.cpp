#include "Core/ContextInfoService.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/ThemeManager.h"
#include "Core/WorkspaceDockFactory.h"
#include "Widgets/Inspector/InspectorPanel.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QMainWindow>
#include <QPointer>
#include <QToolButton>

#include <gtest/gtest.h>

#include <memory>

namespace {

void drainEvents()
{
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QToolButton* pinButton(ads::CDockWidget* dock)
{
    return dock->dockAreaWidget()->titleBar()->findChild<QToolButton*>(
        QStringLiteral("dockPinButton"), Qt::FindDirectChildrenOnly);
}

class WorkspaceDockPinTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        previousFlags_ = ads::CDockManager::configFlags();
        ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
        ads::CDockManager::setConfigFlag(ads::CDockManager::AlwaysShowTabs, true);
        ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
        VNX::ContextInfoService::instance().setLocked(false);
        auto& theme = VNX::ThemeManager::instance();
        VNX::QtAdsThemeAdapter::applyIcons(theme.assetResolver(), theme.snapshot());
        window_ = std::make_unique<QMainWindow>();
        window_->resize(1000, 700);
        manager_ = new ads::CDockManager(window_.get());
        manager_->setStyleSheet(VNX::QtAdsThemeAdapter(theme.snapshot()).styleSheet());
        window_->show();
    }

    void TearDown() override
    {
        window_.reset();
        drainEvents();
        VNX::ContextInfoService::instance().setFreezeRoot(nullptr);
        VNX::ContextInfoService::instance().setActive(false);
        VNX::ContextInfoService::instance().setLocked(false);
        ads::CDockManager::setConfigFlags(previousFlags_);
    }

    ads::CDockWidget* addDock(QWidget* content, ads::CDockAreaWidget* area = nullptr)
    {
        const QString name = QStringLiteral("PinTestDock_%1").arg(++nextId_);
        auto* dock = VNX::WorkspaceDockFactory::createDock(manager_, {
            name, name, content, false, true, false,
            area ? ads::CenterDockWidgetArea : ads::LeftDockWidgetArea, area
        });
        drainEvents();
        return dock;
    }

    ads::CDockManager::ConfigFlags previousFlags_;
    std::unique_ptr<QMainWindow> window_;
    ads::CDockManager* manager_ = nullptr;
    int nextId_ = 0;
};

TEST_F(WorkspaceDockPinTest, DeletingTheActiveTabDisconnectsItsAction)
{
    auto* first = new VNX::InspectorPanel();
    auto* firstDock = addDock(first);
    auto* second = new VNX::InspectorPanel();
    second->setPinned(true);
    auto* secondDock = addDock(second, firstDock->dockAreaWidget());
    QPointer<QAction> oldAction = pinButton(secondDock)->defaultAction();
    secondDock->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);
    secondDock->closeDockWidget();
    drainEvents();
    EXPECT_TRUE(oldAction.isNull());
    ASSERT_NE(pinButton(firstDock), nullptr);
    const bool beforeClick = first->isPinned();
    pinButton(firstDock)->click();
    EXPECT_NE(first->isPinned(), beforeClick);
}

}
