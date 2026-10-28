
#include "ProjectTypes.h"
#include "ActionStepEditing.h"
#include "ItemTypeCatalog.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSound.h"
#include "Components/VSpatial.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Components/VAction.h"
#include "Components/VBus.h"
#include "Components/VAttenuation.h"
#include "Components/VTrack.h"
#include "Components/VSegment.h"

namespace VNX {

void ProjectItem::attachDefaultComponents()
{
    using T = ProjectItem::Type;

    if (ItemTypeCatalog::hasMixingCapability(type))
    {
        auto* mc = components.getOrCreate<VMixNode>();
        if (mc) mc->setHostItem(this);
    }
    if (ItemTypeCatalog::isContentObject(type))
    {
        auto* ac = components.getOrCreate<VAudio>();
        if (ac) ac->setHostItem(this);
    }
    if (ItemTypeCatalog::hasSpatialCapability(type))
    {
        auto* sc = components.getOrCreate<VSpatial>();
        if (sc) sc->setHostItem(this);
    }

    switch (type)
    {
    case T::Sound:
    {
        auto* sound = components.getOrCreate<VSound>();
        if (sound) sound->setHostItem(this);
        break;
    }

    case T::Container:
        components.getOrCreate<VSelection>();
        break;

    case T::Track:
    {
        auto* timeline = components.getOrCreate<VTrack>();
        if (timeline) timeline->setHostItem(this);
        break;
    }

    case T::Segment:
    {
        auto* timeline = components.getOrCreate<VSegment>();
        if (timeline) timeline->setHostItem(this);
        break;
    }

    case T::Bus:
    {
        auto* bc = components.getOrCreate<VBus>();
        if (bc) bc->setHostItem(this);
        break;
    }

    case T::Action:
        components.getOrCreate<VAction>();
        break;

    case T::Variable:
        components.getOrCreate<VVariable>();
        break;

    case T::Attenuation:
        compOrCreate<VAttenuation>();
        break;

    default:
        break;
    }
}


void VAction::toJsonExtra(QJsonObject& j) const
{
    if (!steps.empty())
        j["steps"] = ActionStepEditing::stepsToJson(steps);
}

void VAction::fromJsonExtra(const QJsonObject& j)
{
    steps = ActionStepEditing::stepsFromJson(j.value(QStringLiteral("steps")).toArray());
}

}
