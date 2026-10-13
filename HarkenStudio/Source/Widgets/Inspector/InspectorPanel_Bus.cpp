#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"
#include "Core/ProjectManager.h"

#include "Core/Components/VAudio.h"
#include "Core/Components/VBus.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/BusInspectorSchema.h"

namespace VNX {

namespace {

void applyBusCustomization(InspectorCustomization& c)
{
    c.tabbedLayout = true;
}

}

void InspectorPanelPrivate::setupBusPage()
{
    if (!busEngine_) {
        busEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(busEngine_);
        wireAuthoringCommit(busEngine_);
    }
    applyBusCustomization(busCustomization_);
    busEngine_->setSchema(busInspectorSchema(), ComponentType::Bus);
    busEngine_->setItemType(ProjectItem::Type::Bus);
    busEngine_->setCustomization(&busCustomization_);

    stack_->addWidget(busEngine_->buildPage());
}


void InspectorPanel::showBusInspector(uint32_t busID)
{
    placementContext_.reset();
    if (busID == UINT32_MAX) { showEmpty(); return; }
    auto* item = ProjectManager::instance().findBusItem(busID);
    if (!item)               { showEmpty(); return; }
    d->clearItemBindings();
    d->currentBusItem_ = item;

    d->ensurePageBuilt(InspectorPanelPrivate::PageBus);
    d->updateContext(ItemTypeCatalog::displayName(ProjectItem::Type::Bus), item->name);

    d->busEngine_->bindItem(item);
    d->stack_->setCurrentIndex(InspectorPanelPrivate::PageBus);
}


void InspectorPanel::refreshBusInspector()
{
    if (!d->currentBusItem_) return;
    if (d->busEngine_) d->busEngine_->bindItem(d->currentBusItem_);
}


}
