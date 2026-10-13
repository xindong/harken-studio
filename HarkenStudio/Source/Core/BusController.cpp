
#include "BusController.h"

#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "AudioEngine.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"

#include "../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../Widgets/ProjectExplorer/ProjectExplorer.h"
#include "../Widgets/Inspector/InspectorPanel.h"

#include <DockWidget.h>

#include <cmath>
#include <algorithm>

namespace VNX {

BusController::BusController(MainWindow* host)
    : QObject(host)
    , host_(host)
{
}

void BusController::setViews(const Views& v)
{
    blueprintCanvas_     = v.blueprintCanvas;
    projectExplorer_     = v.projectExplorer;
    blueprintCanvasDock_ = v.blueprintCanvasDock;
}

void BusController::clearSelection()
{
    selectedBusId_ = kInvalidBusSelection;
}


void BusController::scheduleBusRefresh()
{
    if (busRefreshPending_) return;
    busRefreshPending_ = true;
    QMetaObject::invokeMethod(this, [this]() {
        if (selectedBusId_ != kInvalidBusSelection) {
            auto& pm = ProjectManager::instance();
            const bool exists = pm.findVBus(selectedBusId_) != nullptr;
            if (!exists) {
                selectedBusId_ = kInvalidBusSelection;
            }
        }
        busRefreshPending_ = false;
    }, Qt::QueuedConnection);
}

void BusController::handleBusParameterChanged(uint32_t busID, const QString& parameterName,
                                              const QObject* origin)
{
    auto& pm = ProjectManager::instance();
    const auto* busItem = pm.findBusItem(busID);
    const auto* bc = busItem ? busItem->comp<VBus>() : nullptr;
    if (bc == nullptr) return;

    if (parameterName == "pan") {
        AudioEngine::instance().setBusPanById(pm.busRuntimeHandleForId(busID), bc->pan);
    }

    if (selectedBusId_ == busID) {
        if (auto* ip = host_->activeInspectorPanel()) {
            if (!(origin && ip->ownsEditorOrigin(origin))) {
                ip->showBusInspector(busID);
                ip->refreshBusInspector();
            }
        }
    }

}

void BusController::loadBusSignalChain(uint32_t busID)
{
    if (!blueprintCanvas_) return;

    auto& pm = ProjectManager::instance();
    auto* busItem = pm.findBusItem(busID);
    if (busItem == nullptr) return;

    blueprintCanvas_->focusItem(busItem);
}

void BusController::focusBusInBlueprint(uint32_t busID)
{
    if (!blueprintCanvas_) return;
    if (host_ && blueprintCanvasDock_)
        host_->revealDock(blueprintCanvasDock_);
    blueprintCanvas_->focusItem(ProjectManager::instance().findBusItem(busID));
}

void BusController::selectBus(uint32_t busID)
{
    if (selectionSyncGuard_) return;

    auto& pm = ProjectManager::instance();
    const auto* bc = pm.findVBus(busID);
    if (!bc) {
        selectedBusId_ = kInvalidBusSelection;
        if (auto* ip = host_->activeInspectorPanel()) ip->showBusInspector(UINT32_MAX);
        return;
    }

    selectedBusId_ = busID;
    selectionSyncGuard_ = true;

    if (auto* ip = host_->activeInspectorPanel())
        ip->showBusInspector(busID);
    if (projectExplorer_) projectExplorer_->selectBusInTree(busID);

    selectionSyncGuard_ = false;
}

}
