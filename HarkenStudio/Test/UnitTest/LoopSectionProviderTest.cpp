#include "Components/VAudio.h"
#include "ProjectTypes.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"
#include "Widgets/Inspector/PieceInspectorSchema.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"

#include <gtest/gtest.h>

namespace {

const VNX::PropertyDescriptor* findLoop(const VNX::PropertySchema& schema)
{
    for (const auto& descriptor : schema)
        if (descriptor.field == QStringLiteral("loopCount")) return &descriptor;
    return nullptr;
}

}

TEST(LoopSectionProviderTest, LoopIsPlaybackScalarAcrossContentTypes)
{
    VNX::registerBuiltinInspectorCustomizations();
    const auto schemas = {
        &VNX::soundInspectorSchema(),
        &VNX::containerInspectorSchema(),
        &VNX::trackInspectorSchema(),
        &VNX::pieceInspectorSchema(),
    };
    for (const auto* schema : schemas) {
        const auto* loop = findLoop(*schema);
        ASSERT_NE(loop, nullptr);
        EXPECT_EQ(loop->section, VNX::PropertySection::Playback);
        EXPECT_EQ(loop->type, VNX::PropType::Int);
        EXPECT_EQ(loop->label, QStringLiteral("Loop"));
    }
}

TEST(LoopSectionProviderTest, LegacyLoopSectionIsNotRegistered)
{
    VNX::registerBuiltinInspectorCustomizations();
    auto& registry = VNX::InspectorCustomizationRegistry::instance();
    for (VNX::ProjectItem::Type type : {VNX::ProjectItem::Type::Sound,
                                        VNX::ProjectItem::Type::Container,
                                        VNX::ProjectItem::Type::Track,
                                        VNX::ProjectItem::Type::Piece})
        EXPECT_EQ(registry.sectionProvider(type, QStringLiteral("loop")), nullptr);
}
