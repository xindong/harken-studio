#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/InspectorCustomization.h"

namespace VNX {

void InspectorPanelPrivate::setupLayerPage()
{
    if (!layerEngine_) {
        layerEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(layerEngine_);
        wireAuthoringCommit(layerEngine_);
    }

    layerCustomization_.tabbedLayout = true;

    layerEngine_->setSchema(layerInspectorSchema(), ComponentType::MixNode);
    layerEngine_->setItemType(ProjectItem::Type::Layer);
    layerEngine_->setCustomization(&layerCustomization_);

    QWidget* page = layerEngine_->buildPage();
    stack_->addWidget(page);
}

}
