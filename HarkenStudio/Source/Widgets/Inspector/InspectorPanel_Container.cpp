#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"

namespace VNX {

void InspectorPanelPrivate::setupContainerPage()
{
    if (!containerEngine_) {
        containerEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(containerEngine_);
        wireAuthoringCommit(containerEngine_);
    }

    containerCustomization_.tabbedLayout = true;

    containerEngine_->setSchema(containerInspectorSchema(), ComponentType::Audio);
    containerEngine_->setItemType(ProjectItem::Type::Container);
    containerEngine_->setCustomization(&containerCustomization_);

    stack_->addWidget(containerEngine_->buildPage());
}

}
