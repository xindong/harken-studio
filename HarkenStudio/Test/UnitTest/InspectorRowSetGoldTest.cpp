#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/PieceInspectorSchema.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <vector>

using VNX::ProjectItem;

namespace {

const std::vector<std::pair<ProjectItem::Type, const VNX::PropertySchema*>>& nineTypeSchemas()
{
    static const std::vector<std::pair<ProjectItem::Type, const VNX::PropertySchema*>> rows = {
        {ProjectItem::Type::Sound, &VNX::soundInspectorSchema()},
        {ProjectItem::Type::Container, &VNX::containerInspectorSchema()},
        {ProjectItem::Type::Track, &VNX::trackInspectorSchema()},
        {ProjectItem::Type::Piece, &VNX::pieceInspectorSchema()},
        {ProjectItem::Type::Layer, &VNX::layerInspectorSchema()},
        {ProjectItem::Type::Bus, &VNX::busInspectorSchema()},
        {ProjectItem::Type::Action, &VNX::actionInspectorSchema()},
        {ProjectItem::Type::Variable, &VNX::variableInspectorSchema()},
        {ProjectItem::Type::Attenuation, &VNX::attenuationInspectorSchema()},
    };
    return rows;
}

}

TEST(InspectorRowSetGold, EveryRootFieldHasOneRegisteredSectionAndAUniqueIdentity)
{
    for (const auto& row : nineTypeSchemas()) {
        SCOPED_TRACE(VNX::ItemTypeCatalog::canonicalName(row.first).toStdString());
        EXPECT_NO_THROW(VNX::validateInspectorSchema(*row.second));
    }

    auto invalid = VNX::soundInspectorSchema();
    invalid.front().section = VNX::PropertySection::None;
    EXPECT_THROW(VNX::validateInspectorSchema(invalid), std::invalid_argument);

    invalid = VNX::soundInspectorSchema();
    invalid.push_back(invalid.front());
    EXPECT_THROW(VNX::validateInspectorSchema(invalid), std::invalid_argument);
}
