#include "BindingContract.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "harken_playable_properties.g.h"

#include <harken/param/ParamTargets.h>
#include <harken/param/ParamTargetTable.h>
#include <harken/param/PropertyPublisher.h>

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QToolButton>
#include <QUuid>

#include <memory>

using harkenplayableproperties::PlayablePropertyCombineRoleV1;
using harkenplayableproperties::PlayablePropertyIdV1;
using harkenplayableproperties::PlayablePropertyPhaseRoleV1;
using harkenplayableproperties::PlayablePropertyWireV1;
using harkenplayableproperties::kPlayablePropertyRegistryV1;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

const VNX::PropertyDescriptor* findField(const VNX::PropertySchema& schema,
                                         QLatin1String field)
{
    for (const auto& descriptor : schema)
        if (descriptor.field == field) return &descriptor;
    return nullptr;
}

VNX::ProjectItem makeSound()
{
    VNX::ProjectItem item(QStringLiteral("definition"), VNX::ProjectItem::Type::Sound);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    return item;
}

}

TEST(BindingContract, PlayableSlotsDeriveExpectedRoles)
{
    using VNX::BindingBindRole;
    using VNX::BindingCombine;
    using VNX::BindingHostClass;
    using VNX::BindingOutputUnit;
    using VNX::ProjectItemType;

    for (const auto& meta : kPlayablePropertyRegistryV1) {
        const uint8_t id = static_cast<uint8_t>(meta.id);
        const auto contract = VNX::BindingContractSemantics::derive(id, ProjectItemType::Sound);
        EXPECT_EQ(contract.hostClass, BindingHostClass::Voice) << meta.name;
        EXPECT_FALSE(contract.bindPath.has_value()) << meta.name;

        const bool firstAudibleF32 =
            meta.phaseRole == PlayablePropertyPhaseRoleV1::FirstAudible
            && meta.wire == PlayablePropertyWireV1::F32;
        if (!firstAudibleF32) {
            EXPECT_EQ(contract.bindRole, BindingBindRole::None) << meta.name;
            continue;
        }

        switch (meta.id) {
        case PlayablePropertyIdV1::Volume:
            EXPECT_EQ(contract.bindRole, BindingBindRole::LiveOffset);
            EXPECT_EQ(contract.outputUnit, BindingOutputUnit::Decibel);
            EXPECT_EQ(contract.combine, BindingCombine::Add);
            EXPECT_FLOAT_EQ(contract.defaultOutputRange.min, -96.0f);
            EXPECT_FLOAT_EQ(contract.defaultOutputRange.max, 24.0f);
            EXPECT_FLOAT_EQ(contract.legalOutputRange.min, -96.0f);
            EXPECT_FLOAT_EQ(contract.legalOutputRange.max, 24.0f);
            EXPECT_FALSE(VNX::BindingContractSemantics::previewAudible(contract));
            EXPECT_FALSE(contract.runtimeUnimplemented);
            break;
        case PlayablePropertyIdV1::Pitch:
            EXPECT_EQ(contract.bindRole, BindingBindRole::LiveOffset);
            EXPECT_EQ(contract.outputUnit, BindingOutputUnit::Cent);
            EXPECT_EQ(contract.combine, BindingCombine::AddClamp);
            EXPECT_FLOAT_EQ(contract.defaultOutputRange.min, 0.0f);
            EXPECT_FLOAT_EQ(contract.defaultOutputRange.max, 1200.0f);
            EXPECT_FLOAT_EQ(contract.legalOutputRange.min, -7200.0f);
            EXPECT_FLOAT_EQ(contract.legalOutputRange.max, 7200.0f);
            EXPECT_FALSE(VNX::BindingContractSemantics::previewAudible(contract));
            EXPECT_FALSE(contract.runtimeUnimplemented);
            break;
        case PlayablePropertyIdV1::MakeUpGain:
        case PlayablePropertyIdV1::OutputBusVolume:
            EXPECT_EQ(contract.bindRole, BindingBindRole::None);
            EXPECT_FALSE(contract.runtimeUnimplemented);
            break;
        case PlayablePropertyIdV1::SpatializationMix:
            EXPECT_EQ(contract.bindRole, BindingBindRole::None);
            EXPECT_FALSE(contract.runtimeUnimplemented);
            break;
        case PlayablePropertyIdV1::SpatialSpread:
            EXPECT_EQ(contract.bindRole, BindingBindRole::None);
            EXPECT_FALSE(contract.runtimeUnimplemented);
            break;
        case PlayablePropertyIdV1::SpatialPanning:
        case PlayablePropertyIdV1::SpatialAttenuationDistanceScaling:
            EXPECT_EQ(contract.bindRole, BindingBindRole::None);
            EXPECT_TRUE(contract.runtimeUnimplemented);
            break;
        default:
            ADD_FAILURE() << "unhandled firstAudible f32 slot " << meta.name;
            break;
        }
    }
}

TEST(BindingContract, CombineRoleSwitchCoversOpenFamilies)
{
    using VNX::BindingBindRole;
    using VNX::BindingCombine;
    using VNX::ProjectItemType;

    const auto volume = VNX::BindingContractSemantics::derive(
        static_cast<uint8_t>(PlayablePropertyIdV1::Volume), ProjectItemType::Sound);
    EXPECT_EQ(volume.bindRole, BindingBindRole::LiveOffset);
    EXPECT_EQ(volume.combine, BindingCombine::Add);

    const auto pitch = VNX::BindingContractSemantics::derive(
        static_cast<uint8_t>(PlayablePropertyIdV1::Pitch), ProjectItemType::Sound);
    EXPECT_EQ(pitch.bindRole, BindingBindRole::LiveOffset);
    EXPECT_EQ(pitch.combine, BindingCombine::AddClamp);

    const auto loop = VNX::BindingContractSemantics::derive(
        static_cast<uint8_t>(PlayablePropertyIdV1::LoopCount), ProjectItemType::Sound);
    EXPECT_EQ(loop.bindRole, BindingBindRole::None);

    const auto streaming = VNX::BindingContractSemantics::derive(
        static_cast<uint8_t>(PlayablePropertyIdV1::StreamingMode), ProjectItemType::Sound);
    EXPECT_EQ(streaming.bindRole, BindingBindRole::None);

    const auto busRef = VNX::BindingContractSemantics::derive(
        static_cast<uint8_t>(PlayablePropertyIdV1::MainOutputBusRef), ProjectItemType::Sound);
    EXPECT_EQ(busRef.bindRole, BindingBindRole::None);
}

TEST(BindingContract, NearestExplicitSlotsAreNotBindable)
{
    for (const auto& meta : kPlayablePropertyRegistryV1) {
        if (meta.combineRole != PlayablePropertyCombineRoleV1::NearestExplicit)
            continue;
        const auto contract = VNX::BindingContractSemantics::derive(
            static_cast<uint8_t>(meta.id), VNX::ProjectItemType::Sound);
        EXPECT_FALSE(VNX::BindingContractSemantics::isBindable(contract)) << meta.name;
    }
}

TEST(BindingContract, BindableContractsHaveEngineTargets)
{
    using VNX::ProjectItemType;

    for (const auto hostType : {ProjectItemType::Sound, ProjectItemType::Bus}) {
        for (const auto& meta : kPlayablePropertyRegistryV1) {
            const uint8_t slotId = static_cast<uint8_t>(meta.id);
            const auto contract = VNX::BindingContractSemantics::derive(slotId, hostType);
            if (!VNX::BindingContractSemantics::isBindable(contract)
                || contract.runtimeUnimplemented)
                continue;
            const auto address = VNX::BindingContractSemantics::addressOf(slotId, hostType);
            EXPECT_TRUE(VNX::BindingContractSemantics::engineTargetId(address).has_value())
                << meta.name << " host=" << static_cast<int>(hostType);
        }
    }
}

TEST(BindingContract, HostClassSplitsVoiceVolumeFromBusVolume)
{
    using VNX::BindingHostClass;
    using VNX::ProjectItemType;

    const uint8_t volume = static_cast<uint8_t>(PlayablePropertyIdV1::Volume);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(volume, ProjectItemType::Sound).hostClass,
              BindingHostClass::Voice);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(volume, ProjectItemType::Bus).hostClass,
              BindingHostClass::Bus);
    EXPECT_EQ(VNX::BindingContractSemantics::hostClassOf(ProjectItemType::Layer),
              BindingHostClass::Voice);

    const auto voiceVolume = VNX::BindingContractSemantics::addressOf(volume, ProjectItemType::Sound);
    const auto busVolume = VNX::BindingContractSemantics::addressOf(volume, ProjectItemType::Bus);
    EXPECT_NE(voiceVolume, busVolume);
    EXPECT_EQ(VNX::BindingContractSemantics::engineTargetScopeKind(voiceVolume.hostClass),
              harken::kTargetScopeVoice);
    EXPECT_EQ(VNX::BindingContractSemantics::engineTargetScopeKind(busVolume.hostClass),
              harken::kTargetScopeBus);
    EXPECT_EQ(VNX::BindingContractSemantics::engineTargetId(voiceVolume),
              harken::kTargetVoiceVolume);
    EXPECT_EQ(VNX::BindingContractSemantics::engineTargetId(busVolume),
              harken::kBusTargetBusVolume);
    EXPECT_FALSE(VNX::BindingContractSemantics::previewAudible(
        VNX::BindingContractSemantics::derive(volume, ProjectItemType::Sound)));
    EXPECT_TRUE(VNX::BindingContractSemantics::previewAudible(
        VNX::BindingContractSemantics::derive(volume, ProjectItemType::Bus)));

    VNX::ProjectItem sound(QStringLiteral("Voice"), VNX::ProjectItem::Type::Sound);
    sound.attachDefaultComponents();
    VNX::ProjectItem bus(QStringLiteral("Master"), VNX::ProjectItem::Type::Bus);
    bus.attachDefaultComponents();
    EXPECT_EQ(VNX::BindingContractSemantics::resolveHostComponent(&sound, BindingHostClass::Voice),
              sound.comp<VNX::VMixNode>());
    EXPECT_EQ(VNX::BindingContractSemantics::resolveHostComponent(&bus, BindingHostClass::Bus),
              bus.comp<VNX::VMixNode>());
}

TEST(BindingContract, DefaultTransitionMsIsTwentyForVolumeAndPitch)
{
    using VNX::ProjectItemType;
    const uint8_t volume = static_cast<uint8_t>(PlayablePropertyIdV1::Volume);
    const uint8_t pitch = static_cast<uint8_t>(PlayablePropertyIdV1::Pitch);
    const uint8_t makeUp = static_cast<uint8_t>(PlayablePropertyIdV1::MakeUpGain);

    EXPECT_EQ(VNX::BindingContractSemantics::derive(volume, ProjectItemType::Sound)
                  .defaultTransitionMs,
              20u);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(volume, ProjectItemType::Bus)
                  .defaultTransitionMs,
              20u);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(pitch, ProjectItemType::Sound)
                  .defaultTransitionMs,
              20u);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(makeUp, ProjectItemType::Sound)
                  .defaultTransitionMs,
              0u);
    const uint8_t outputVolume =
        static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(outputVolume, ProjectItemType::Bus)
                  .defaultTransitionMs,
              20u);
    EXPECT_EQ(VNX::BindingContractSemantics::derive(outputVolume, ProjectItemType::Sound)
                  .defaultTransitionMs,
              0u);
    EXPECT_TRUE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeVoice, harken::kTargetVoiceVolume));
    EXPECT_TRUE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeVoice, harken::kTargetVoicePitch));
    EXPECT_TRUE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeBus, harken::kBusTargetBusVolume));
    EXPECT_TRUE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeBus, harken::kBusTargetBusOutputVolume));
    EXPECT_FALSE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeVoice, harken::kTargetVoicePan));
}

TEST(BindingContract, ContradictionFieldsStayCompleteWithoutInspectorRows)
{
    using VNX::BindingBindRole;
    using VNX::PlayablePropertyAuthoringV1;
    using VNX::ProjectItemType;

    EXPECT_FALSE(PlayablePropertyAuthoringV1::isInspectorVisible(
        static_cast<uint8_t>(PlayablePropertyIdV1::MakeUpGain)));

    const auto rows = VNX::contentObjectInspectorRows(VNX::ProjectItem::Type::Sound);
    EXPECT_EQ(findField(rows, QLatin1String("makeUpGain")), nullptr);
    EXPECT_NE(findField(rows, QLatin1String("spatializationMix")), nullptr);
    EXPECT_EQ(findField(rows, QLatin1String("spatialAttenuationCurveType")), nullptr);

    const auto makeUp = VNX::BindingContractSemantics::derive(
        static_cast<uint8_t>(PlayablePropertyIdV1::MakeUpGain), ProjectItemType::Sound);
    EXPECT_EQ(makeUp.bindRole, BindingBindRole::None);
    EXPECT_FALSE(makeUp.runtimeUnimplemented);

    const auto* mix = findField(VNX::VSpatial::schema(), QLatin1String("spatializationMix"));
    ASSERT_NE(mix, nullptr);
    const auto mixContract = VNX::BindingContractSemantics::derive(*mix, ProjectItemType::Sound);
    EXPECT_EQ(mixContract.bindRole, BindingBindRole::None);
    EXPECT_FALSE(mixContract.runtimeUnimplemented);

}

TEST(BindingContract, RetiredAttenuationCurveTypeHasNoAuthoringSlot)
{
    using VNX::PlayablePropertyAuthoringV1;
    EXPECT_EQ(PlayablePropertyAuthoringV1::fieldById(13), nullptr);
    EXPECT_EQ(PlayablePropertyAuthoringV1::fieldByName(
                  QStringLiteral("spatialAttenuationCurveType")), nullptr);
    EXPECT_EQ(findField(VNX::VSpatial::schema(),
                        QLatin1String("attenuationCurveType")), nullptr);
    VNX::ProjectItem sound = makeSound();
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, 13, 3, std::nullopt));
}

TEST(BindingContract, HangPointFollowsSlotIdNotFieldName)
{
    using VNX::ProjectItemType;

    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    const auto* volume = findField(VNX::VMixNode::schema(), QLatin1String("volume"));
    ASSERT_NE(volume, nullptr);
    VNX::PropertyDescriptor renamed = *volume;
    renamed.field = QStringLiteral("loudness");
    renamed.jsonKey = QStringLiteral("volume");
    EXPECT_TRUE(VNX::BindingContractSemantics::isBindable(
        VNX::BindingContractSemantics::derive(renamed, ProjectItemType::Sound)));

    VNX::PropertyEditorEngine engine;
    engine.setSchema({renamed}, VNX::ComponentType::MixNode);
    engine.setItemType(VNX::ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_volume")),
              nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_loudness")),
              nullptr);

    const auto* makeUp = findField(VNX::VMixNode::schema(), QLatin1String("makeUpGain"));
    ASSERT_NE(makeUp, nullptr);
    VNX::PropertyEditorEngine gated;
    gated.setSchema({*makeUp}, VNX::ComponentType::MixNode);
    gated.setItemType(VNX::ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> gatedPage(gated.buildPage());
    EXPECT_EQ(gatedPage->findChild<QToolButton*>(QStringLiteral("bindingPoint_makeUpGain")),
              nullptr);
}

TEST(BindingContract, VolumeAndPitchRemainTheOpenHangPoints)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::ProjectItem item(QStringLiteral("Bound"), VNX::ProjectItem::Type::Sound);
    item.attachDefaultComponents();
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(VNX::ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(VNX::ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(&item);

    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_volume")), nullptr);
    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_pitch")), nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_makeUpGain")), nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_spatialSpread")), nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_outputBusVolume")), nullptr);
}

TEST(BindingContract, BusOutputVolumeIsBindableLiveOffsetWithEngineTarget)
{
    using VNX::BindingBindRole;
    using VNX::BindingCombine;
    using VNX::BindingHostClass;
    using VNX::BindingOutputUnit;
    using VNX::ProjectItemType;

    const uint8_t outputVolume =
        static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume);
    const auto voice = VNX::BindingContractSemantics::derive(
        outputVolume, ProjectItemType::Sound);
    EXPECT_FALSE(VNX::BindingContractSemantics::isBindable(voice));
    EXPECT_EQ(voice.bindRole, BindingBindRole::None);
    EXPECT_EQ(voice.hostClass, BindingHostClass::Voice);

    const auto bus = VNX::BindingContractSemantics::derive(
        outputVolume, ProjectItemType::Bus);
    EXPECT_TRUE(VNX::BindingContractSemantics::isBindable(bus));
    EXPECT_EQ(bus.hostClass, BindingHostClass::Bus);
    EXPECT_EQ(bus.bindRole, BindingBindRole::LiveOffset);
    EXPECT_EQ(bus.outputUnit, BindingOutputUnit::Decibel);
    EXPECT_EQ(bus.combine, BindingCombine::Add);
    EXPECT_EQ(bus.defaultTransitionMs, 20u);
    EXPECT_FALSE(bus.runtimeUnimplemented);
    const auto address =
        VNX::BindingContractSemantics::addressOf(outputVolume, ProjectItemType::Bus);
    const auto target = VNX::BindingContractSemantics::engineTargetId(address);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, harken::kBusTargetBusOutputVolume);
    EXPECT_EQ(VNX::BindingContractSemantics::engineTargetScopeKind(address.hostClass),
              harken::kTargetScopeBus);
}

TEST(BindingContract, BusInspectorEnablesExistingOutputVolumeHangPoint)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::ProjectItem bus(QStringLiteral("BoundBus"), VNX::ProjectItem::Type::Bus);
    bus.attachDefaultComponents();
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::busInspectorSchema(), VNX::ComponentType::Bus);
    engine.setItemType(VNX::ProjectItem::Type::Bus);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(&bus);

    auto* point = page->findChild<QToolButton*>(
        QStringLiteral("bindingPoint_outputBusVolume"));
    ASSERT_NE(point, nullptr);
    EXPECT_TRUE(point->isEnabled());
}

TEST(BindingContract, DescriptorHangPointFollowsFirstAudibleDbSum)
{
    using VNX::BindingBindRole;
    using VNX::BindingCombine;
    using VNX::BindingOutputUnit;
    using VNX::ProjectItemType;

    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    const auto* volume = findField(VNX::VMixNode::schema(), QLatin1String("volume"));
    ASSERT_NE(volume, nullptr);
    VNX::PropertyDescriptor live = *volume;
    live.field = QStringLiteral("futureGain");
    live.jsonKey = QStringLiteral("volume");
    const auto contract = VNX::BindingContractSemantics::derive(live, ProjectItemType::Sound);
    EXPECT_TRUE(VNX::BindingContractSemantics::isBindable(contract));
    EXPECT_EQ(contract.bindRole, BindingBindRole::LiveOffset);
    EXPECT_EQ(contract.outputUnit, BindingOutputUnit::Decibel);
    EXPECT_EQ(contract.combine, BindingCombine::Add);
    EXPECT_FLOAT_EQ(contract.defaultOutputRange.min, -96.0f);
    EXPECT_FLOAT_EQ(contract.defaultOutputRange.max, 24.0f);
    EXPECT_FALSE(contract.runtimeUnimplemented);

    VNX::PropertyEditorEngine engine;
    engine.setSchema({live}, VNX::ComponentType::MixNode);
    engine.setItemType(VNX::ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_volume")),
              nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_futureGain")),
              nullptr);
}

TEST(BindingContract, TransitionCapabilityUsesRuntimeScopedMetadata)
{
    for (const auto& meta : harken::kTargetMetaTable) {
        EXPECT_EQ(VNX::BindingContractSemantics::supportsTransition(meta.scopeKind, meta.targetId),
                  meta.supportsTransition);
    }
    EXPECT_FALSE(VNX::BindingContractSemantics::supportsTransition(99, 0));
    EXPECT_TRUE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeBus, harken::kBusTargetBusOutputVolume));
    EXPECT_FALSE(VNX::BindingContractSemantics::supportsTransition(
        harken::kTargetScopeBus, harken::kBusTargetCount));
    EXPECT_FALSE(VNX::BindingContractSemantics::supportsTransition(harken::kTargetScopeVoice, UINT32_MAX));
}
