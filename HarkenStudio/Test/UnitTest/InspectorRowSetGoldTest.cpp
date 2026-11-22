#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"
#include "Widgets/Inspector/EffectInspectorSchema.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/SegmentInspectorSchema.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <vector>

using harken::studio::ProjectItem;

namespace {

const std::vector<std::pair<ProjectItem::Type, const harken::studio::PropertySchema*>>& itemTypeSchemas()
{
    static const std::vector<std::pair<ProjectItem::Type, const harken::studio::PropertySchema*>> rows = {
        {ProjectItem::Type::Sound, &harken::studio::soundInspectorSchema()},
        {ProjectItem::Type::Container, &harken::studio::containerInspectorSchema()},
        {ProjectItem::Type::Track, &harken::studio::trackInspectorSchema()},
        {ProjectItem::Type::Segment, &harken::studio::segmentInspectorSchema()},
        {ProjectItem::Type::Layer, &harken::studio::layerInspectorSchema()},
        {ProjectItem::Type::Bus, &harken::studio::busInspectorSchema()},
        {ProjectItem::Type::Action, &harken::studio::actionInspectorSchema()},
        {ProjectItem::Type::Variable, &harken::studio::variableInspectorSchema()},
        {ProjectItem::Type::Attenuation, &harken::studio::attenuationInspectorSchema()},
        {ProjectItem::Type::Effect, &harken::studio::effectInspectorSchema()},
    };
    return rows;
}

}

TEST(InspectorRowSetGold, EveryRootFieldHasOneRegisteredSectionAndAUniqueIdentity)
{
    for (const auto& row : itemTypeSchemas()) {
        SCOPED_TRACE(harken::studio::ItemTypeCatalog::canonicalName(row.first).toStdString());
        EXPECT_NO_THROW(harken::studio::validateInspectorSchema(*row.second));
    }

    auto invalid = harken::studio::soundInspectorSchema();
    invalid.front().section = harken::studio::PropertySection::None;
    EXPECT_THROW(harken::studio::validateInspectorSchema(invalid), std::invalid_argument);

    invalid = harken::studio::soundInspectorSchema();
    invalid.push_back(invalid.front());
    EXPECT_THROW(harken::studio::validateInspectorSchema(invalid), std::invalid_argument);
}
