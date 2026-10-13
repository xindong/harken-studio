#pragma once

#include "WorkspaceWindowTypes.h"

#include <DockManager.h>
#include <DockWidget.h>

#include <functional>

class QAction;

namespace VNX {

class QtAdsWindowAdapter
{
public:
    using RevealDockFn = std::function<void(ads::CDockWidget*)>;

    QtAdsWindowAdapter() = default;
    QtAdsWindowAdapter(ads::CDockManager* dockManager, RevealDockFn revealDock);

    void setDockManager(ads::CDockManager* dockManager);
    void setRevealDockFn(RevealDockFn revealDock);

    bool showDock(ads::CDockWidget* dock) const;
    bool hideDock(ads::CDockWidget* dock) const;
    bool focusDock(ads::CDockWidget* dock) const;
    bool setDockVisible(ads::CDockWidget* dock, bool visible) const;
    bool addDock(ads::CDockWidget* dock, const WorkspaceWindowDescriptor& descriptor) const;

    bool isDockVisible(ads::CDockWidget* dock) const;
    bool isDockAvailable(ads::CDockWidget* dock) const;
    QAction* toggleAction(ads::CDockWidget* dock) const;

private:
    ads::CDockManager* dockManager_ = nullptr;
    RevealDockFn revealDock_;
};

}
