#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"

namespace VNX {

void InspectorPanelPrivate::setupSoundPageEngine()
{
    if (!soundEngine_) {
        soundEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(soundEngine_);
        wireAuthoringCommit(soundEngine_);
    }


    soundCustomization_.tabbedLayout = true;

    soundEngine_->setSchema(soundInspectorSchema(), ComponentType::Audio);
    soundEngine_->setItemType(ProjectItem::Type::Sound);
    soundEngine_->setCustomization(&soundCustomization_);

    QWidget* page = soundEngine_->buildPage();
    stack_->addWidget(page);
}

}
