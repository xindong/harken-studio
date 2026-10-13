#include "QtAdsWindowAdapter.h"

#include <DockAreaWidget.h>

#include <QAction>
#include <QWidget>

#include <utility>

namespace VNX {

QtAdsWindowAdapter::QtAdsWindowAdapter(ads::CDockManager* dockManager, RevealDockFn revealDock)
    : dockManager_(dockManager)
    , revealDock_(std::move(revealDock))
{
}

void QtAdsWindowAdapter::setDockManager(ads::CDockManager* dockManager)
{
    dockManager_ = dockManager;
}

void QtAdsWindowAdapter::setRevealDockFn(RevealDockFn revealDock)
{
    revealDock_ = std::move(revealDock);
}

bool QtAdsWindowAdapter::showDock(ads::CDockWidget* dock) const
{
    if (!dock || !isDockAvailable(dock))
        return false;

    if (revealDock_)
        revealDock_(dock);
    else
        dock->toggleView(true);

    return focusDock(dock);
}

bool QtAdsWindowAdapter::hideDock(ads::CDockWidget* dock) const
{
    if (!dock)
        return false;
    dock->toggleView(false);
    return true;
}

bool QtAdsWindowAdapter::focusDock(ads::CDockWidget* dock) const
{
    if (!dock || dock->isClosed())
        return false;

    if (auto* area = dock->dockAreaWidget())
        area->setCurrentDockWidget(dock);
    dock->setAsCurrentTab();
    dock->raise();
    if (QWidget* content = dock->widget())
        content->setFocus(Qt::OtherFocusReason);
    return true;
}

bool QtAdsWindowAdapter::setDockVisible(ads::CDockWidget* dock, bool visible) const
{
    return visible ? showDock(dock) : hideDock(dock);
}

bool QtAdsWindowAdapter::addDock(ads::CDockWidget* dock,
                                 const WorkspaceWindowDescriptor& descriptor) const
{
    if (!dock || !dockManager_)
        return false;

    auto toAdsArea = [](DefaultDockArea area) {
        switch (area)
        {
        case DefaultDockArea::Top: return ads::TopDockWidgetArea;
        case DefaultDockArea::Left: return ads::LeftDockWidgetArea;
        case DefaultDockArea::Right: return ads::RightDockWidgetArea;
        case DefaultDockArea::Bottom: return ads::BottomDockWidgetArea;
        case DefaultDockArea::Center: return ads::CenterDockWidgetArea;
        case DefaultDockArea::Floating: return ads::NoDockWidgetArea;
        }
        return ads::NoDockWidgetArea;
    };

    if (descriptor.placement.area == DefaultDockArea::Floating)
    {
        dockManager_->addDockWidgetFloating(dock, descriptor.placement.initiallyVisible);
        if (!descriptor.placement.initiallyVisible)
            dock->toggleView(false);
    }
    else
    {
        ads::CDockAreaWidget* anchorArea = nullptr;
        if (!descriptor.placement.anchorDockObjectName.isEmpty())
            if (auto* anchorDock = dockManager_->findDockWidget(descriptor.placement.anchorDockObjectName))
                anchorArea = anchorDock->dockAreaWidget();

        dockManager_->addDockWidget(toAdsArea(descriptor.placement.area), dock, anchorArea);
        if (!descriptor.placement.initiallyVisible)
            dock->toggleView(false);
    }

    return true;
}

bool QtAdsWindowAdapter::isDockVisible(ads::CDockWidget* dock) const
{
    return dock && !dock->isClosed();
}

bool QtAdsWindowAdapter::isDockAvailable(ads::CDockWidget* dock) const
{
    if (!dock)
        return false;
    if (QAction* action = dock->toggleViewAction())
        return action->isEnabled();
    return dockManager_ != nullptr;
}

QAction* QtAdsWindowAdapter::toggleAction(ads::CDockWidget* dock) const
{
    return dock ? dock->toggleViewAction() : nullptr;
}

}
