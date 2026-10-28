#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/SegmentInspectorSchema.h"

namespace VNX {

void InspectorPanelPrivate::setupSegmentPage()
{
    if (!segmentEngine_) {
        segmentEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(segmentEngine_);
        wireAuthoringCommit(segmentEngine_);
    }

    segmentCustomization_.tabbedLayout = true;

    segmentEngine_->setSchema(segmentInspectorSchema(), ComponentType::Audio);
    segmentEngine_->setItemType(ProjectItem::Type::Segment);
    segmentEngine_->setCustomization(&segmentCustomization_);

    stack_->addWidget(segmentEngine_->buildPage());
}

}
