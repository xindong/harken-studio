#include "Core/ContextInfoService.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/ThemeManager.h"
#include "Core/WorkspaceDockFactory.h"
#include "Core/WorkspaceWindowManager.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/Information/InformationPanel.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/PreviewPanel/PreviewPanel.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/TransitionEditor/TransitionEditorPanel.h"

#include <DockAreaTabBar.h>
#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockWidgetTab.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QImage>
#include <QLabel>
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

    template<typename Panel>
    void checkPanelBinding(const QString& oldButtonName)
    {
        auto* panel = new Panel();
        panel->setPinned(true);
        auto* dock = addDock(panel);
        auto* button = pinButton(dock);
        ASSERT_NE(button, nullptr);
        EXPECT_TRUE(button->isChecked());
        panel->setPinned(false);
        EXPECT_FALSE(button->isChecked());
        button->click();
        EXPECT_TRUE(panel->isPinned());
        EXPECT_EQ(panel->template findChild<QToolButton*>(oldButtonName), nullptr);
    }

    ads::CDockManager::ConfigFlags previousFlags_;
    std::unique_ptr<QMainWindow> window_;
    ads::CDockManager* manager_ = nullptr;
    int nextId_ = 0;
};

TEST_F(WorkspaceDockPinTest, ActiveTabOwnsTheButtonAndItsIndependentState)
{
    auto* first = new VNX::InspectorPanel();
    auto* firstDock = addDock(first);
    auto* second = new VNX::InspectorPanel();
    second->setPinned(true);
    auto* secondDock = addDock(second, firstDock->dockAreaWidget());
    auto* title = secondDock->dockAreaWidget()->titleBar();
    auto* button = pinButton(secondDock);
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(button->isChecked());
    EXPECT_EQ(title->findChildren<QToolButton*>(QStringLiteral("dockPinButton")).size(), 1);
    EXPECT_EQ(secondDock->tabWidget()->findChild<QToolButton*>(QStringLiteral("dockPinButton")), nullptr);
    EXPECT_EQ(button->iconSize(), QSize(20, 20));
    EXPECT_GE(button->contentsRect().width(), 20);
    EXPECT_GE(button->contentsRect().height(), 20);
    auto* close = title->button(ads::TitleBarButtonClose);
    EXPECT_EQ(title->indexOf(button) + 1, title->indexOf(close));
    EXPECT_LT(button->geometry().right(), close->geometry().left());
    EXPECT_EQ(button->geometry().center().y(), close->geometry().center().y());

    firstDock->setAsCurrentTab();
    drainEvents();
    button = pinButton(firstDock);
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(button->isChecked());
    const auto outline = button->icon().pixmap(button->iconSize(), button->devicePixelRatioF());
    ASSERT_FALSE(outline.isNull());
    EXPECT_EQ(outline.deviceIndependentSize(), QSizeF(20, 20));
    const QString unpinnedTip = button->toolTip();
    int changes = 0;
    QObject::connect(first, &VNX::InspectorPanel::pinToggled, first,
                     [&changes](int, bool) { ++changes; });
    button->click();
    EXPECT_TRUE(first->isPinned());
    EXPECT_TRUE(second->isPinned());
    EXPECT_EQ(changes, 1);
    const auto filled = button->icon().pixmap(button->iconSize(), button->devicePixelRatioF());
    EXPECT_EQ(filled.deviceIndependentSize(), QSizeF(20, 20));
    EXPECT_NE(outline.toImage(), filled.toImage());
    EXPECT_NE(button->toolTip(), unpinnedTip);

    second->setPinned(false);
    EXPECT_TRUE(button->isChecked());
    secondDock->setAsCurrentTab();
    drainEvents();
    ASSERT_NE(pinButton(secondDock), nullptr);
    EXPECT_FALSE(pinButton(secondDock)->isChecked());
    EXPECT_TRUE(first->isPinned());
}

TEST_F(WorkspaceDockPinTest, TabsWithoutPinSupportHideTheButton)
{
    auto* panel = new VNX::InspectorPanel();
    auto* dock = addDock(panel);
    auto* plain = addDock(new QLabel(QStringLiteral("Plain")), dock->dockAreaWidget());
    EXPECT_EQ(pinButton(plain), nullptr);
    panel->setPinned(true);
    dock->setAsCurrentTab();
    drainEvents();
    ASSERT_NE(pinButton(dock), nullptr);
    EXPECT_TRUE(pinButton(dock)->isChecked());
    dock->toggleView(false);
    drainEvents();
    EXPECT_EQ(plain->dockAreaWidget()->currentDockWidget(), plain);
    EXPECT_EQ(pinButton(plain), nullptr);
    dock->toggleView(true);
    dock->setAsCurrentTab();
    drainEvents();
    ASSERT_NE(pinButton(dock), nullptr);
    EXPECT_TRUE(pinButton(dock)->isChecked());
}

TEST_F(WorkspaceDockPinTest, FloatingMergingAndRestoringKeepTheCorrectAction)
{
    auto* first = new VNX::InspectorPanel();
    first->setPinned(true);
    auto* firstDock = addDock(first);
    auto* second = new VNX::GridEditorPanel();
    auto* secondDock = addDock(second, firstDock->dockAreaWidget());
    const QByteArray layout = manager_->saveState();

    manager_->addDockWidgetFloating(firstDock);
    drainEvents();
    ASSERT_NE(firstDock->dockAreaWidget(), secondDock->dockAreaWidget());
    ASSERT_NE(pinButton(firstDock), nullptr);
    ASSERT_NE(pinButton(secondDock), nullptr);
    EXPECT_TRUE(pinButton(firstDock)->isChecked());
    EXPECT_FALSE(pinButton(secondDock)->isChecked());
    pinButton(firstDock)->click();
    EXPECT_FALSE(first->isPinned());
    EXPECT_FALSE(second->isPinned());

    manager_->addDockWidget(ads::CenterDockWidgetArea, firstDock, secondDock->dockAreaWidget());
    first->setPinned(true);
    drainEvents();
    ASSERT_NE(pinButton(firstDock), nullptr);
    EXPECT_TRUE(pinButton(firstDock)->isChecked());
    ASSERT_TRUE(manager_->restoreState(layout));
    drainEvents();
    EXPECT_EQ(firstDock->dockAreaWidget(), secondDock->dockAreaWidget());
    EXPECT_EQ(secondDock->dockAreaWidget()->currentDockWidget(), secondDock);
    ASSERT_NE(pinButton(secondDock), nullptr);
    EXPECT_FALSE(pinButton(secondDock)->isChecked());
    firstDock->setAsCurrentTab();
    drainEvents();
    ASSERT_NE(pinButton(firstDock), nullptr);
    EXPECT_TRUE(pinButton(firstDock)->isChecked());
    pinButton(firstDock)->click();
    EXPECT_FALSE(first->isPinned());
}

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
    EXPECT_FALSE(pinButton(firstDock)->isChecked());
    pinButton(firstDock)->click();
    EXPECT_TRUE(first->isPinned());
}

TEST_F(WorkspaceDockPinTest, AllEditorBindingsUseTheExistingPanelState)
{
    checkPanelBinding<VNX::InspectorPanel>(QStringLiteral("inspectorPinButton"));
    checkPanelBinding<PreviewPanel>(QStringLiteral("dockPinButton"));
    checkPanelBinding<VNX::GridEditorPanel>(QStringLiteral("gridPin"));
    checkPanelBinding<VNX::TransitionEditorPanel>(QStringLiteral("transitionPin"));
    checkPanelBinding<VNX::TimelineEditorPanel>(QStringLiteral("timelinePin"));
    checkPanelBinding<VNX::ExternalEditorPanel>(QStringLiteral("dockPinButton"));
    checkPanelBinding<VNX::SourceEditorPanel>(QStringLiteral("dockPinButton"));
}

TEST_F(WorkspaceDockPinTest, LazyInformationPanelImmediatelyExposesItsLockState)
{
    VNX::WorkspaceWindowManager windows(VNX::defaultWorkspaceWindowCatalog(),
                                        VNX::QtAdsWindowAdapter(manager_, {}));
    windows.registerSingletonFactory(QStringLiteral("Information"),
        {[]() -> QWidget* { return new VNX::InformationPanel(); }, {}, {}});
    auto* dock = windows.ensureSingleton(QStringLiteral("Information"));
    ASSERT_NE(dock, nullptr);
    EXPECT_EQ(dock->widget(), nullptr);
    dock->setAsCurrentTab();
    auto& service = VNX::ContextInfoService::instance();
    service.setLocked(true);
    ASSERT_NE(windows.materializeSingletonContent(QStringLiteral("Information")), nullptr);
    drainEvents();
    auto* button = pinButton(dock);
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(button->isChecked());
    EXPECT_EQ(dock->widget()->findChild<QToolButton*>(QStringLiteral("informationLockButton")), nullptr);
    const QString lockedTip = button->toolTip();
    button->click();
    EXPECT_FALSE(service.isLocked());
    EXPECT_NE(button->toolTip(), lockedTip);
    service.setLocked(true);
    EXPECT_TRUE(button->isChecked());
}

}
