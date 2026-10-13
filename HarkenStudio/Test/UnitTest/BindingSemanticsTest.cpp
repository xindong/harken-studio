#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingSemantics.h"
#include "Components/VMixNode.h"
#include "harken_playable_properties.g.h"
#include <harken/param/Curve.h>
#include <harken/param/ParamTargetTable.h>

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace {

QUuid guid(int value)
{
    return QUuid(QStringLiteral("{%1-0000-0000-0000-000000000000}")
        .arg(value, 8, 16, QLatin1Char('0')));
}

}

TEST(BindingSemantics, SmoothAndStepUseContinuousAndIntegerReads)
{
    const VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(1));

    EXPECT_NEAR(VNX::BindingSemantics::contribution(
                    binding, VNX::VariableKind::Smooth, 2.25f, 0.0f, 10.0f),
                -2.213926f, 0.0001f);
    EXPECT_NEAR(VNX::BindingSemantics::contribution(
                    binding, VNX::VariableKind::Step, 2.49f, 0.0f, 10.0f),
                -1.938166f, 0.0001f);
    EXPECT_NEAR(VNX::BindingSemantics::contribution(
                    binding, VNX::VariableKind::Step, 2.5f, 0.0f, 10.0f),
                -3.097980f, 0.0001f);
}

TEST(BindingSemantics, VolumeDefaultsUseDescendingAmplitudeForVoiceAndBus)
{
    for (auto host : {VNX::ProjectItemType::Sound, VNX::ProjectItemType::Bus}) {
        const auto binding = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(host), guid(1));
        EXPECT_FLOAT_EQ(binding.outputMin, -96.0f);
        EXPECT_FLOAT_EQ(binding.outputMax, 24.0f);
        EXPECT_FLOAT_EQ(binding.curve.front().value, 0.0f);
        EXPECT_FLOAT_EQ(binding.curve.back().value, -96.0f);
        EXPECT_NEAR(VNX::BindingSemantics::contribution(binding,
            VNX::VariableKind::Smooth, 0.5f, 0.0f, 1.0f), -6.0205f, 0.0001f);
    }
}

TEST(BindingSemantics, PitchDefaultRangeIsCent)
{
    const VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), guid(2));
    EXPECT_EQ(VNX::BindingSemantics::targetUnit(binding.address), QStringLiteral("cent"));
    EXPECT_FLOAT_EQ(binding.outputMin, 0.0f);
    EXPECT_FLOAT_EQ(binding.outputMax, 1200.0f);
}

TEST(BindingSemantics, DecibelPreviewMatchesRuntimeAndMirroredCanvas)
{
    auto outputAddress = VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus);
    outputAddress.slotId = static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    const VNX::BindingAddress addresses[] = {
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Sound),
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus), outputAddress};
    for (const auto& address : addresses) {
        auto binding = VNX::BindingSemantics::defaultBinding(address, guid(2));
        binding.curve = {{0.0f, 6.0f}, {1.0f, -6.0f}};
        binding.handles = {{0.0f, 6.0f, 1.0f, -6.0f}};
        harken::Curve runtime;
        const harken::CurvePoint points[] = {{0.0f, 6.0f}, {1.0f, -6.0f}};
        const harken::CurveHandlePair handles[] = {{0.0f, 6.0f, 1.0f, -6.0f}};
        ASSERT_TRUE(runtime.SetPoints(points, handles, 2));
        const auto target = VNX::BindingContractSemantics::engineTargetId(address);
        ASSERT_TRUE(target.has_value());
        const auto domain = harken::BindingCurveDomain(
            VNX::BindingContractSemantics::engineTargetScopeKind(address.hostClass), *target);
        for (int i = 0; i <= 20; ++i) {
            const float x = i / 20.0f;
            const float preview = VNX::BindingSemantics::contribution(
                binding, VNX::VariableKind::Smooth, x, 0.0f, 1.0f);
            EXPECT_FLOAT_EQ(preview, runtime.Eval(x, domain));
            EXPECT_FLOAT_EQ(preview, VNX::curveBezierValueAt(binding.curve, binding.handles, x,
                VNX::CurveAxisScale::MirroredAmplitudeDb));
        }
        EXPECT_NEAR(VNX::BindingSemantics::contribution(
            binding, VNX::VariableKind::Smooth, 0.5f, 0.0f, 1.0f), 0.0f, 1e-6f);
    }
}

TEST(BindingSemantics, DomainRemapsPreserveNormalizedShape)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), guid(2));
    binding.curve.insert(1, {0.25f, 300.0f});
    binding.handles.clear();
    VNX::curveNormalize(0.0f, 1200.0f, binding.curve, binding.handles);

    VNX::BindingSemantics::remapOutputDomain(binding, -600.0f, 600.0f);
    EXPECT_FLOAT_EQ(binding.curve[1].value, -300.0f);
    EXPECT_FLOAT_EQ(binding.outputMin, -600.0f);
    EXPECT_FLOAT_EQ(binding.outputMax, 600.0f);
}

TEST(BindingSemantics, TypedLedgerRoundtripsCurveAndExtensions)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(3));
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(3));
    binding.extensions[QStringLiteral("future")] = 42;
    binding.curveExtensions[QStringLiteral("interpolation")] = QStringLiteral("future");
    VNX::BindingLedger ledger;
    ASSERT_TRUE(ledger.upsert(binding));

    const QJsonArray saved = ledger.toJson();
    VNX::BindingLedger loaded = VNX::BindingLedger::fromJson(saved);
    ASSERT_NE(loaded.findFirst(VNX::BindingSemantics::volumeAddress()), nullptr);
    EXPECT_EQ(loaded.toJson(), saved);
    EXPECT_EQ(loaded.toJson().first().toObject()[QStringLiteral("future")].toInt(), 42);
    EXPECT_EQ(loaded.toJson().first().toObject()[QStringLiteral("curve")]
                  .toObject()[QStringLiteral("interpolation")].toString(),
              QStringLiteral("future"));
    EXPECT_TRUE(saved.first().toObject().contains(QStringLiteral("address")));
    EXPECT_FALSE(saved.first().toObject().contains(QStringLiteral("target")));
}

TEST(BindingSemantics, MixNodePersistsTypedBindingsAsOptionalV6Data)
{
    VNX::VMixNode mix;
    auto pitch = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), guid(7));
    pitch.guid = VNX::authoringUuid(harkentest::StudioObjectId(7));
    ASSERT_TRUE(mix.bindings.upsert(std::move(pitch)));
    const QJsonObject saved = mix.toJson();
    ASSERT_TRUE(saved.contains(QStringLiteral("bindings")));

    VNX::VMixNode loaded;
    loaded.fromJson(saved);
    ASSERT_NE(loaded.bindings.findFirst(VNX::BindingSemantics::pitchAddress()), nullptr);
    EXPECT_EQ(loaded.toJson(), saved);

    VNX::VMixNode empty;
    EXPECT_FALSE(empty.toJson().contains(QStringLiteral("bindings")));
}

TEST(BindingSemantics, OpaqueLegacyEntriesRoundtripWithoutInventingDefaults)
{
    const QJsonArray legacy{
        7,
        QStringLiteral("legacy"),
        QJsonValue::Null,
        QJsonObject{{QStringLiteral("id"), 5},
                    {QStringLiteral("subtype"), 2}}};
    const VNX::BindingLedger ledger = VNX::BindingLedger::fromJson(legacy);
    EXPECT_EQ(ledger.toJson(), legacy);
    EXPECT_EQ(ledger.findFirst(VNX::BindingSemantics::volumeAddress()), nullptr);
}

TEST(BindingSemantics, LedgerAllowsDuplicateTypedAddresses)
{
    VNX::BindingLedger ledger;
    auto pitch = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), guid(4));
    pitch.guid = VNX::authoringUuid(harkentest::StudioObjectId(4));
    ASSERT_TRUE(ledger.upsert(std::move(pitch)));
    auto first = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(5));
    auto second = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(6));
    first.guid = VNX::authoringUuid(harkentest::StudioObjectId(5));
    second.guid = VNX::authoringUuid(harkentest::StudioObjectId(6));
    std::vector<VNX::ParameterBindingDef> duplicate{std::move(first), std::move(second)};
    EXPECT_TRUE(ledger.replace(std::move(duplicate)));
    EXPECT_EQ(ledger.findAll(VNX::BindingSemantics::volumeAddress()).size(), 2u);
    EXPECT_EQ(ledger.findFirst(VNX::BindingSemantics::pitchAddress()), nullptr);
}

TEST(BindingSemantics, ReorderMovesOnlyMatchingAddress)
{
    VNX::BindingLedger ledger;
    auto first = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(10));
    auto second = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(11));
    auto pitch = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), guid(12));
    first.guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    second.guid = VNX::authoringUuid(harkentest::StudioObjectId(2));
    pitch.guid = VNX::authoringUuid(harkentest::StudioObjectId(3));
    ASSERT_TRUE(ledger.upsert(first));
    ASSERT_TRUE(ledger.upsert(second));
    ASSERT_TRUE(ledger.upsert(pitch));
    ASSERT_TRUE(ledger.reorder(VNX::BindingSemantics::volumeAddress(), 0, 1));
    const auto volume = ledger.findAll(VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(volume.size(), 2u);
    EXPECT_EQ(volume[0]->runtimeId(), harkentest::StudioObjectId(2u));
    EXPECT_EQ(volume[1]->runtimeId(), harkentest::StudioObjectId(1u));
    EXPECT_EQ(ledger.findFirst(VNX::BindingSemantics::pitchAddress())->runtimeId(), harkentest::StudioObjectId(3u));

    const QJsonArray saved = ledger.toJson();
    const VNX::BindingLedger loaded = VNX::BindingLedger::fromJson(saved);
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded.entries()[0].runtimeId(), harkentest::StudioObjectId(2u));
    EXPECT_EQ(loaded.entries()[1].runtimeId(), harkentest::StudioObjectId(1u));
    EXPECT_EQ(loaded.entries()[2].runtimeId(), harkentest::StudioObjectId(3u));
    EXPECT_EQ(loaded.toJson(), saved);
}

TEST(BindingSemantics, VoiceAndBusVolumeDoNotCollide)
{
    auto voice = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Sound), guid(8));
    auto bus = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus), guid(9));
    voice.guid = VNX::authoringUuid(harkentest::StudioObjectId(8));
    bus.guid = VNX::authoringUuid(harkentest::StudioObjectId(9));
    EXPECT_NE(voice.address, bus.address);
    EXPECT_NE(VNX::BindingSemantics::addressKey(voice.address),
              VNX::BindingSemantics::addressKey(bus.address));

    VNX::BindingLedger voiceLedger;
    ASSERT_TRUE(voiceLedger.upsert(voice));
    EXPECT_FALSE(voiceLedger.upsert(bus));
    EXPECT_EQ(voiceLedger.findAll(VNX::BindingSemantics::volumeAddress(
                  VNX::ProjectItemType::Sound)).size(), 1u);
    EXPECT_EQ(voiceLedger.findAll(VNX::BindingSemantics::volumeAddress(
                  VNX::ProjectItemType::Bus)).size(), 0u);

    VNX::BindingLedger busLedger;
    ASSERT_TRUE(busLedger.upsert(bus));
    EXPECT_FALSE(busLedger.upsert(voice));
    EXPECT_EQ(busLedger.findAll(VNX::BindingSemantics::volumeAddress(
                  VNX::ProjectItemType::Bus)).size(), 1u);
    EXPECT_EQ(busLedger.findAll(VNX::BindingSemantics::volumeAddress(
                  VNX::ProjectItemType::Sound)).size(), 0u);
}

TEST(BindingSemantics, UpsertRejectsGatedNoneAddress)
{
    VNX::BindingLedger ledger;
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(13));
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(13));
    binding.address.slotId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);
    EXPECT_FALSE(ledger.upsert(std::move(binding)));
    EXPECT_EQ(ledger.size(), 0u);
}

TEST(BindingSemantics, ReplaceAcceptsGatedNoneButRejectsMixedHosts)
{
    auto gated = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(14));
    gated.guid = VNX::authoringUuid(harkentest::StudioObjectId(14));
    gated.address.slotId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);
    VNX::BindingLedger ledger;
    EXPECT_TRUE(ledger.replace({gated}));

    auto voice = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Sound), guid(15));
    voice.guid = VNX::authoringUuid(harkentest::StudioObjectId(15));
    auto bus = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus), guid(16));
    bus.guid = VNX::authoringUuid(harkentest::StudioObjectId(16));
    EXPECT_FALSE(ledger.replace({voice, bus}));
}

TEST(BindingSemantics, ReplaceCombineTakesLastContribution)
{
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::combineContributions(
        VNX::BindingCombine::Add, -3.0f, -2.0f), -5.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::combineContributions(
        VNX::BindingCombine::Replace, -3.0f, -2.0f), -2.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::combineWithBase(
        VNX::BindingCombine::Add, 1.0f, -4.0f), -3.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::combineWithBase(
        VNX::BindingCombine::Replace, 1.0f, -4.0f), -4.0f);
}

TEST(BindingSemantics, DisplayDomainMapsStoredUnitInterval)
{
    const auto domain = VNX::BindingSemantics::displayDomain(-12.0f, 12.0f);
    EXPECT_FALSE(domain.degenerate);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::storedToDisplayX(0.0f, domain), -12.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::storedToDisplayX(1.0f, domain), 12.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::storedToDisplayX(0.25f, domain), -6.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::displayToStoredX(-6.0f, domain), 0.25f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::displayToStoredX(12.0f, domain), 1.0f);
}

TEST(BindingSemantics, TypedBindingWithZeroIdIsRejected)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(13));
    EXPECT_EQ(binding.runtimeId(), harkentest::StudioObjectId(0u));
    VNX::BindingLedger ledger;
    EXPECT_FALSE(ledger.upsert(binding));
    EXPECT_TRUE(ledger.entries().empty());
}

TEST(BindingSemantics, StoredToDisplayRoundtripsCurveAndHandles)
{
    const auto domain = VNX::BindingSemantics::displayDomain(-12.0f, 12.0f);
    const QVector<VNX::CurvePointDef> stored{{0.0f, -10.0f}, {0.25f, -5.0f}, {1.0f, 0.0f}};
    const QVector<VNX::CurveHandlePairDef> handles{{0.1f, -8.0f, 0.2f, -6.0f}};
    const auto displayed = VNX::BindingSemantics::storedToDisplay(stored, domain);
    ASSERT_EQ(displayed.size(), 3);
    EXPECT_FLOAT_EQ(displayed[1].x, -6.0f);
    EXPECT_FLOAT_EQ(displayed[1].value, -5.0f);
    const auto roundtrip = VNX::BindingSemantics::displayToStored(displayed, domain);
    ASSERT_EQ(roundtrip.size(), 3);
    EXPECT_NEAR(roundtrip[1].x, 0.25f, 0.000001f);
    const auto displayedHandles = VNX::BindingSemantics::storedToDisplay(handles, domain);
    ASSERT_EQ(displayedHandles.size(), 1);
    EXPECT_NEAR(displayedHandles[0].h1x, -9.6f, 0.0001f);
    EXPECT_NEAR(VNX::BindingSemantics::displayToStored(displayedHandles, domain)[0].h1x,
                0.1f, 0.000001f);
}

TEST(BindingSemantics, DegenerateDomainPinsDisplayAndStoredOrigin)
{
    const auto domain = VNX::BindingSemantics::displayDomain(5.0f, 5.0f);
    EXPECT_TRUE(domain.degenerate);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::storedToDisplayX(0.0f, domain), 5.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::storedToDisplayX(1.0f, domain), 5.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::displayToStoredX(5.0f, domain), 0.0f);
    EXPECT_FLOAT_EQ(VNX::BindingSemantics::displayToStoredX(9.0f, domain), 0.0f);
}

TEST(BindingSemantics, AxisTickLabelPrefersNamedAnchorAndLeavesSparseGapsNumeric)
{
    const std::vector<VNX::VariableLabel> labels = {
        {0, QStringLiteral("Calm")},
        {3, QStringLiteral("Storm")},
    };
    EXPECT_EQ(VNX::BindingSemantics::axisTickLabel(0.2f, labels), QStringLiteral("Calm"));
    EXPECT_EQ(VNX::BindingSemantics::axisTickLabel(2.6f, labels), QStringLiteral("Storm"));
    EXPECT_EQ(VNX::BindingSemantics::axisTickLabel(1.0f, labels), QStringLiteral("1"));
    EXPECT_EQ(VNX::BindingSemantics::axisTickLabel(1.5f, labels), QStringLiteral("1.5"));
}

TEST(BindingSemantics, MissingTransitionMsDefaultsToTwenty)
{
    auto makeMissing = [](const VNX::BindingAddress& address, int id) {
        VNX::ParameterBindingDef binding =
            VNX::BindingSemantics::defaultBinding(address, guid(id));
        binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(static_cast<uint64_t>(id)));
        EXPECT_EQ(binding.transitionMs, 20u);
        QJsonObject obj = binding.toJson().toObject();
        obj.remove(QStringLiteral("transitionMs"));
        VNX::ParameterBindingDef loaded = VNX::ParameterBindingDef::fromJson(obj);
        EXPECT_TRUE(loaded.typed);
        EXPECT_EQ(loaded.transitionMs, 20u);
    };
    makeMissing(VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Sound), 21);
    makeMissing(VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus), 22);
    makeMissing(VNX::BindingSemantics::pitchAddress(), 23);
}

TEST(BindingSemantics, ExplicitZeroTransitionMsIsKept)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(24));
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(24));
    binding.transitionMs = 0;
    const QJsonValue saved = binding.toJson();
    EXPECT_EQ(saved.toObject().value(QStringLiteral("transitionMs")).toInt(), 0);
    const VNX::ParameterBindingDef loaded = VNX::ParameterBindingDef::fromJson(saved);
    EXPECT_TRUE(loaded.typed);
    EXPECT_EQ(loaded.transitionMs, 0u);
}

TEST(BindingSemantics, TransitionMsRoundtripsTwenty)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), guid(25));
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(25));
    const QJsonValue saved = binding.toJson();
    EXPECT_EQ(saved.toObject().value(QStringLiteral("transitionMs")).toInt(), 20);
    const VNX::ParameterBindingDef loaded = VNX::ParameterBindingDef::fromJson(saved);
    EXPECT_TRUE(loaded.typed);
    EXPECT_EQ(loaded.transitionMs, 20u);
    EXPECT_EQ(loaded.toJson(), saved);
}

TEST(BindingSemantics, IllegalTransitionMsStaysOpaque)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), guid(26));
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(26));
    const QJsonObject base = binding.toJson().toObject();
    const QJsonValue illegals[] = {
        QJsonValue(-1),
        QJsonValue(20.5),
        QJsonValue(QStringLiteral("20")),
        QJsonValue(true),
        QJsonValue::Null,
        QJsonValue(60001),
    };
    for (const QJsonValue& illegal : illegals) {
        QJsonObject obj = base;
        obj.insert(QStringLiteral("transitionMs"), illegal);
        const QJsonValue original(obj);
        const VNX::ParameterBindingDef loaded =
            VNX::ParameterBindingDef::fromJson(original);
        EXPECT_FALSE(loaded.typed);
        EXPECT_EQ(loaded.opaqueValue, original);
        EXPECT_EQ(loaded.toJson(), original);
    }
}
