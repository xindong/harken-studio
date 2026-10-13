#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/ProjectTypes.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/PieceInspectorSchema.h"

namespace VNX {

void InspectorPanelPrivate::setupPiecePage()
{
    if (!pieceEngine_) {
        pieceEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(pieceEngine_);
        wireAuthoringCommit(pieceEngine_);
    }

    pieceCustomization_.tabbedLayout = true;

    pieceEngine_->setSchema(pieceInspectorSchema(), ComponentType::Audio);
    pieceEngine_->setItemType(ProjectItem::Type::Piece);
    pieceEngine_->setCustomization(&pieceCustomization_);

    stack_->addWidget(pieceEngine_->buildPage());
}

}
