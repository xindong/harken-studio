#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"

namespace VNX {

void InspectorPanelPrivate::setupTrackPage()
{
    if (!trackEngine_) {
        trackEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(trackEngine_);
        wireAuthoringCommit(trackEngine_);
    }

    trackCustomization_.tabbedLayout = true;

    trackEngine_->setSchema(trackInspectorSchema(), ComponentType::Audio);
    trackEngine_->setItemType(ProjectItem::Type::Track);
    trackEngine_->setCustomization(&trackCustomization_);

    stack_->addWidget(trackEngine_->buildPage());
}

}
