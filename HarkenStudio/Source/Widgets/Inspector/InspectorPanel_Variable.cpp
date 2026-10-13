#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"

namespace VNX {

void InspectorPanelPrivate::setupVariablePage()
{
    if (!variableEngine_) {
        variableEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(variableEngine_);
        wireAuthoringCommit(variableEngine_);
    }

    variableCustomization_.tabbedLayout = true;

    variableEngine_->setSchema(variableInspectorSchema(), ComponentType::Variable);
    variableEngine_->setItemType(ProjectItem::Type::Variable);
    variableEngine_->setCustomization(&variableCustomization_);

    stack_->addWidget(variableEngine_->buildPage());
}

}
