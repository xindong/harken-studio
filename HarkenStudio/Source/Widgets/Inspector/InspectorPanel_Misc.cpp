#include "InspectorPanel.h"
#include "InspectorPanel_p.h"
#include "Core/AppSettings.h"
#include "Core/ProjectTypes.h"
#include "Core/ActionManager.h"
#include "Core/UndoCommands.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PlatformProfile.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/AudioEngine.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QHeaderView>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QInputDialog>
#include <QMessageBox>
#include <cmath>

#include "Core/Components/VAttenuation.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VSound.h"

#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/PropertySchemaJson.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"
#include "Widgets/Inspector/EffectInspectorSchema.h"
#include "Core/ComponentCatalog.h"
#include "Core/ItemTypeCatalog.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace VNX {

void InspectorPanelPrivate::setupActionPage()
{
    if (!actionEngine_) {
        actionEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(actionEngine_);
        wireAuthoringCommit(actionEngine_);
    }
    actionCustomization_.tabbedLayout = true;

    actionEngine_->setSchema(actionInspectorSchema(), ComponentType::Action);
    actionEngine_->setItemType(ProjectItem::Type::Action);
    actionEngine_->setCustomization(&actionCustomization_);

    QWidget* page = actionEngine_->buildPage();
    stack_->addWidget(page);
}


void InspectorPanelPrivate::setupAttenuationPage()
{
    if (!attenuationEngine_) {
        attenuationEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(attenuationEngine_);
        wireAuthoringCommit(attenuationEngine_);
    }
    attenuationCustomization_.tabbedLayout = true;

    attenuationEngine_->setSchema(attenuationInspectorSchema(), ComponentType::Attenuation);
    attenuationEngine_->setItemType(ProjectItem::Type::Attenuation);
    attenuationEngine_->setCustomization(&attenuationCustomization_);

    QWidget* page = attenuationEngine_->buildPage();
    stack_->addWidget(page);
}


void InspectorPanelPrivate::setupEffectPage()
{
    if (!effectEngine_) {
        effectEngine_ = new PropertyEditorEngine(q_ptr);
        q_ptr->wireEngineLaunch(effectEngine_);
        wireAuthoringCommit(effectEngine_);
    }
    effectCustomization_.tabbedLayout = true;

    effectEngine_->setSchema(effectInspectorSchema(), ComponentType::Effect);
    effectEngine_->setItemType(ProjectItem::Type::Effect);
    effectEngine_->setCustomization(&effectCustomization_);

    QWidget* page = effectEngine_->buildPage();
    stack_->addWidget(page);
}

void InspectorPanel::selectCurveDimension(AttenuationDimension dimension)
{
    if (d->stack_->currentIndex() != InspectorPanelPrivate::PageAttenuation)
        return;
    const auto* comp = d->currentAttenuationItem_
        ? d->currentAttenuationItem_->comp<VAttenuation>()
        : nullptr;
    if (!comp || !comp->dimension(dimension))
        return;
    requestCurveSelection(CurveSelection::attenuation(
        comp->hostItem()->guid, comp->hostItem()->name, dimension));
}

void InspectorPanel::requestCurveSelection(const CurveSelection& selection)
{
    emit curveSelectionRequested(selection);
}

void InspectorPanel::requestExternalEditor(const ExternalEditorSelection& selection, bool addBinding)
{
    emit externalEditorRequested(selection, addBinding);
}

QJsonObject InspectorPanel::getInspectorSchema(const QString& schemaId) const
{
    return inspectorSchemaToJson(schemaId);
}

}
