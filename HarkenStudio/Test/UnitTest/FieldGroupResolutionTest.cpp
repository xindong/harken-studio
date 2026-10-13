#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "FieldGroups.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace VNX {
namespace {

using namespace harkenprojectschema;

using Group = FieldGroupId;
using Mode = FieldGroupMode;
using Code = FieldGroupDiagnosticCode;
using Platform = harkenprojectschema::AuthoringGuid;

constexpr AuthoringGuid G(std::uint64_t value)
{
    return {0, value};
}

constexpr auto kMaster = G(100);
constexpr std::array kGroups{Group::MainOutput, Group::Send, Group::SpatialRating};
constexpr std::array kKinds{
    ContentKind::Layer, ContentKind::Sound, ContentKind::Container,
    ContentKind::Track, ContentKind::Piece};

template <typename Payload>
GroupState<Payload> Local(Payload payload)
{
    return {Mode::Local, std::move(payload)};
}

DefinitionFieldGroups LocalGroups(std::uint64_t bus, float volume)
{
    DefinitionFieldGroups groups;
    groups.mainOutput.shared = Local(MainOutputFieldGroup{G(bus), volume});
    groups.send.shared = Local(SendFieldGroup{{{G(bus + 1), volume}}});
    groups.spatialRating.shared = Local(
        SpatialRatingFieldGroup{0.5f, true, G(bus + 2), 2.0f});
    return groups;
}

bool HasCode(const std::vector<FieldGroupDiagnostic>& diagnostics, Code code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void ExpectSource(const ResolvedFieldGroup& resolved, AuthoringGuid definition,
                  FieldGroupScope scope = {})
{
    EXPECT_FALSE(resolved.source.isDefault);
    EXPECT_EQ(resolved.source.definition, definition);
    EXPECT_EQ(resolved.source.scope, scope);
}

TEST(FieldGroupResolution, EveryContentKindUsesRootCurrentValuesWithTheRetainedInheritanceFlag)
{
    const auto groups = MakeRootFieldGroups(kMaster);
    std::vector<FieldGroupDefinitionView> definitions;
    for (std::size_t i = 0; i < kKinds.size(); ++i)
        definitions.push_back({G(i + 1), kKinds[i], {}, &groups});
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    for (const auto& definition : definitions) {
        for (const auto group : kGroups) {
            for (const auto scope : {FieldGroupScope{}, FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Windows}}) {
                const auto result = resolver.Resolve(definition.guid, scope, group);
                ASSERT_TRUE(result.ok());
                EXPECT_EQ(result.value->group, group);
                EXPECT_FALSE(result.value->source.isDefault);
                EXPECT_EQ(result.value->source.definition, definition.guid);
                EXPECT_FALSE(result.value->source.scope);
                EXPECT_TRUE(result.value->effectiveInherit);
                EXPECT_EQ(result.value->requestedMode, scope ? Mode::UseShared : Mode::Inherit);
                switch (group) {
                case Group::MainOutput:
                    EXPECT_EQ(std::get<MainOutputFieldGroup>(result.value->payload),
                              (MainOutputFieldGroup{kMaster, 0.0f}));
                    break;
                case Group::Send:
                    EXPECT_TRUE(std::get<SendFieldGroup>(result.value->payload).sends.empty());
                    break;
                case Group::SpatialRating:
                    EXPECT_EQ(std::get<SpatialRatingFieldGroup>(result.value->payload),
                              SpatialRatingFieldGroup{});
                    break;
                }
            }
        }
    }
}

TEST(FieldGroupResolution, RootsRequireCompleteOwnValuesAfterApplyingFieldOverrides)
{
    std::vector<FieldGroupScope> scopes{{}};
    for (const auto platform : HarkenTest::fieldGroupPlatforms) scopes.push_back(platform);
    for (const auto kind : kKinds) {
        for (const auto group : kGroups) {
            for (const auto scope : scopes) {
                SCOPED_TRACE(::testing::Message() << static_cast<int>(kind) << ":"
                    << static_cast<int>(group) << ":"
                    << (scope ? scope->low : -1));
                auto root = MakeRootFieldGroups(kMaster);
                const auto inherit = [scope](auto& authored) {
                    if (scope)
                        authored.platforms[*scope].mode = Mode::Inherit;
                    else
                        authored.shared.localHistory.reset();
                };
                switch (group) {
                case Group::MainOutput: inherit(root.mainOutput); break;
                case Group::Send: inherit(root.send); break;
                case Group::SpatialRating: inherit(root.spatialRating); break;
                }
                const std::array definitions{
                    FieldGroupDefinitionView{G(1), kind, {}, &root}};
                const FieldGroupResolver resolver(definitions, kMaster);
                if (scope) {
                    ASSERT_TRUE(resolver.ok());
                    const auto resolved = resolver.Resolve(G(1), scope, group);
                    ASSERT_TRUE(resolved.ok());
                    EXPECT_TRUE(resolved.value->effectiveInherit);
                    continue;
                }
                EXPECT_FALSE(resolver.ok());
                ASSERT_EQ(resolver.diagnostics().size(), 1u);
                const auto& diagnostic = resolver.diagnostics().front();
                EXPECT_EQ(diagnostic.code, Code::MissingLocalHistory);
                EXPECT_EQ(diagnostic.definition, G(1));
                EXPECT_EQ(diagnostic.group, group);
                EXPECT_EQ(diagnostic.scope, scope);
                EXPECT_FALSE(resolver.Resolve(G(1), {}, Group::Send).ok());
            }
        }
    }
}

TEST(FieldGroupResolution, OwnershipRulesUseAllFiveKinds)
{
    constexpr bool allowed[5][5] = {
        {true, true, true, true, true},
        {false, false, false, false, false},
        {false, true, true, true, true},
        {false, true, true, false, true},
        {false, false, false, true, false}
    };
    const auto parentGroups = LocalGroups(101, -12.0f);
    const DefinitionFieldGroups childGroups;
    for (std::size_t parent = 0; parent < kKinds.size(); ++parent) {
        for (std::size_t child = 0; child < kKinds.size(); ++child) {
            SCOPED_TRACE(::testing::Message() << parent << ":" << child);
            const std::array definitions{
                FieldGroupDefinitionView{G(1), kKinds[parent], {}, &parentGroups},
                FieldGroupDefinitionView{G(2), kKinds[child], G(1), &childGroups}};
            const FieldGroupResolver resolver(definitions, kMaster);
            EXPECT_EQ(resolver.ok(), allowed[parent][child]);
            if (!allowed[parent][child]) {
                EXPECT_TRUE(HasCode(resolver.diagnostics(), Code::IllegalParentKind));
                EXPECT_FALSE(resolver.Resolve(G(1), {}, Group::MainOutput).ok());
                continue;
            }
            for (const auto group : kGroups) {
                const auto resolved = resolver.Resolve(G(2), {}, group);
                ASSERT_TRUE(resolved.ok());
                ExpectSource(*resolved.value, G(1));
            }
        }
    }
}

TEST(FieldGroupResolution, RealContainerPieceAndTrackAncestorsProvideAtomicGroups)
{
    std::array authored{
        LocalGroups(101, -1.0f), LocalGroups(201, -2.0f), LocalGroups(301, -3.0f),
        LocalGroups(401, -4.0f), DefinitionFieldGroups{}};
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &authored[0]},
        FieldGroupDefinitionView{G(2), ContentKind::Container, G(1), &authored[1]},
        FieldGroupDefinitionView{G(3), ContentKind::Piece, G(2), &authored[2]},
        FieldGroupDefinitionView{G(4), ContentKind::Track, G(3), &authored[3]},
        FieldGroupDefinitionView{G(5), ContentKind::Sound, G(4), &authored[4]}};
    for (std::size_t remaining = 4; remaining != 0; --remaining) {
        const auto sourceIndex = remaining - 1;
        const auto snapshot = authored;
        const FieldGroupResolver resolver(definitions, kMaster);
        ASSERT_TRUE(resolver.ok());
        const auto output = resolver.Resolve(G(5), {}, Group::MainOutput);
        const auto send = resolver.Resolve(G(5), {}, Group::Send);
        const auto spatial = resolver.Resolve(G(5), {}, Group::SpatialRating);
        ASSERT_TRUE(output.ok());
        ASSERT_TRUE(send.ok());
        ASSERT_TRUE(spatial.ok());
        ExpectSource(*output.value, G(remaining));
        ExpectSource(*send.value, G(remaining));
        ExpectSource(*spatial.value, G(remaining));
        EXPECT_EQ(std::get<MainOutputFieldGroup>(output.value->payload),
                  *authored[sourceIndex].mainOutput.shared.localHistory);
        EXPECT_EQ(std::get<SendFieldGroup>(send.value->payload),
                  *authored[sourceIndex].send.shared.localHistory);
        EXPECT_EQ(std::get<SpatialRatingFieldGroup>(spatial.value->payload),
                  *authored[sourceIndex].spatialRating.shared.localHistory);
        EXPECT_EQ(authored, snapshot);
        authored[sourceIndex].mainOutput.shared.mode = Mode::Inherit;
        authored[sourceIndex].send.shared.mode = Mode::Inherit;
        authored[sourceIndex].spatialRating.shared.mode = Mode::Inherit;
    }
}

TEST(FieldGroupResolution, GroupsCanChooseDifferentAncestorsWithoutCombiningMembers)
{
    auto layer = LocalGroups(101, -12.0f);
    auto container = LocalGroups(201, -24.0f);
    auto track = LocalGroups(301, -36.0f);
    container.mainOutput.shared.mode = Mode::Inherit;
    container.spatialRating.shared.mode = Mode::Inherit;
    track.mainOutput.shared.mode = Mode::Inherit;
    track.send.shared.mode = Mode::Inherit;
    track.spatialRating.shared.localHistory = SpatialRatingFieldGroup{};
    const DefinitionFieldGroups sound;
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &layer},
        FieldGroupDefinitionView{G(2), ContentKind::Container, G(1), &container},
        FieldGroupDefinitionView{G(3), ContentKind::Track, G(2), &track},
        FieldGroupDefinitionView{G(4), ContentKind::Sound, G(3), &sound}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    const auto output = resolver.Resolve(G(4), {}, Group::MainOutput);
    const auto send = resolver.Resolve(G(4), {}, Group::Send);
    const auto spatial = resolver.Resolve(G(4), {}, Group::SpatialRating);
    ASSERT_TRUE(output.ok());
    ASSERT_TRUE(send.ok());
    ASSERT_TRUE(spatial.ok());
    ExpectSource(*output.value, G(1));
    ExpectSource(*send.value, G(2));
    ExpectSource(*spatial.value, G(3));
    EXPECT_EQ(std::get<MainOutputFieldGroup>(output.value->payload),
              *layer.mainOutput.shared.localHistory);
    EXPECT_EQ(std::get<SendFieldGroup>(send.value->payload),
              *container.send.shared.localHistory);
    EXPECT_EQ(std::get<SpatialRatingFieldGroup>(spatial.value->payload),
              SpatialRatingFieldGroup{});
}

TEST(FieldGroupResolution, LocalValuesReplaceParentsIncludingAnEmptySendList)
{
    const auto parent = LocalGroups(101, -24.0f);
    DefinitionFieldGroups child;
    child.mainOutput.shared = Local(MainOutputFieldGroup{G(201), 0.0f});
    child.send.shared = Local(SendFieldGroup{});
    child.spatialRating.shared = Local(SpatialRatingFieldGroup{});
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &parent},
        FieldGroupDefinitionView{G(2), ContentKind::Sound, G(1), &child}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    const auto output = resolver.Resolve(G(2), {}, Group::MainOutput);
    const auto send = resolver.Resolve(G(2), {}, Group::Send);
    const auto spatial = resolver.Resolve(G(2), {}, Group::SpatialRating);
    ASSERT_TRUE(output.ok());
    ASSERT_TRUE(send.ok());
    ASSERT_TRUE(spatial.ok());
    for (const auto* result : {&output, &send, &spatial}) {
        ExpectSource(*result->value, G(2));
        EXPECT_EQ(result->value->requestedMode, Mode::Local);
        EXPECT_FALSE(result->value->effectiveInherit);
    }
    EXPECT_EQ(std::get<MainOutputFieldGroup>(output.value->payload),
              (MainOutputFieldGroup{G(201), 0.0f}));
    EXPECT_TRUE(std::get<SendFieldGroup>(send.value->payload).sends.empty());
    EXPECT_EQ(std::get<SpatialRatingFieldGroup>(spatial.value->payload),
              SpatialRatingFieldGroup{});
}

TEST(FieldGroupResolution, PlatformModesAndSharedModesFormTheCompleteMatrix)
{
    const std::array<std::optional<Mode>, 4> platformModes{
        std::nullopt, Mode::Inherit, Mode::Local, Mode::UseShared};
    for (const auto platform : HarkenTest::fieldGroupPlatforms) {
        for (const auto sharedMode : {Mode::Inherit, Mode::Local}) {
            for (const auto platformMode : platformModes) {
                SCOPED_TRACE(::testing::Message()
                             << platform.low << ":"
                             << static_cast<int>(sharedMode) << ":"
                             << (platformMode ? static_cast<int>(*platformMode) : -1));
                auto parent = MakeRootFieldGroups(kMaster);
                parent.mainOutput.shared = Local(MainOutputFieldGroup{G(101), -1.0f});
                parent.mainOutput.platforms[platform] = Local(
                    MainOutputFieldGroup{G(102), -2.0f});
                DefinitionFieldGroups child;
                child.mainOutput.shared = {sharedMode, MainOutputFieldGroup{G(201), -3.0f}};
                if (platformMode)
                    child.mainOutput.platforms[platform] = {
                        *platformMode, MainOutputFieldGroup{G(202), -4.0f}};
                const std::array definitions{
                    FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &parent},
                    FieldGroupDefinitionView{G(2), ContentKind::Sound, G(1), &child}};
                const auto snapshot = child;
                const FieldGroupResolver resolver(definitions, kMaster);
                ASSERT_TRUE(resolver.ok());
                const auto shared = resolver.Resolve(G(2), {}, Group::MainOutput);
                ASSERT_TRUE(shared.ok());
                EXPECT_EQ(std::get<MainOutputFieldGroup>(shared.value->payload).mainOutputBusRef,
                          G(sharedMode == Mode::Local ? 201 : 101));
                EXPECT_EQ(shared.value->effectiveInherit, sharedMode == Mode::Inherit);
                const auto resolved = resolver.Resolve(G(2), platform, Group::MainOutput);
                ASSERT_TRUE(resolved.ok());
                const auto requested = platformMode.value_or(Mode::UseShared);
                const bool useShared = requested == Mode::UseShared;
                const bool ownLocal = requested == Mode::Local ||
                                      (useShared && sharedMode == Mode::Local);
                const auto bus = ownLocal ? (platformMode ? 202 : 201) : 102;
                EXPECT_EQ(std::get<MainOutputFieldGroup>(resolved.value->payload).mainOutputBusRef,
                          G(bus));
                ExpectSource(*resolved.value, G(ownLocal ? 2 : 1),
                             ownLocal && !platformMode ? FieldGroupScope{} : platform);
                EXPECT_EQ(resolved.value->requestedMode, requested);
                EXPECT_EQ(resolved.value->effectiveInherit, !ownLocal);
                EXPECT_EQ(child, snapshot);
            }
        }
    }
}

TEST(FieldGroupResolution, UseSharedInheritContinuesOnTheRequestedPlatform)
{
    auto root = LocalGroups(101, -1.0f);
    auto middle = LocalGroups(201, -2.0f);
    auto leaf = LocalGroups(301, -3.0f);
    root.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = Local(MainOutputFieldGroup{G(401), -4.0f});
    middle.mainOutput.shared.mode = Mode::Inherit;
    middle.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        Mode::UseShared, MainOutputFieldGroup{G(501), -5.0f}};
    leaf.mainOutput.shared.mode = Mode::Inherit;
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &root},
        FieldGroupDefinitionView{G(2), ContentKind::Container, G(1), &middle},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(2), &leaf}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    const auto windows = resolver.Resolve(G(3), HarkenTest::FieldGroupPlatformIds::Windows, Group::MainOutput);
    const auto shared = resolver.Resolve(G(3), {}, Group::MainOutput);
    ASSERT_TRUE(windows.ok());
    ASSERT_TRUE(shared.ok());
    ExpectSource(*windows.value, G(1), HarkenTest::FieldGroupPlatformIds::Windows);
    ExpectSource(*shared.value, G(1));
    EXPECT_EQ(std::get<MainOutputFieldGroup>(windows.value->payload).mainOutputBusRef, G(401));
    EXPECT_EQ(std::get<MainOutputFieldGroup>(shared.value->payload).mainOutputBusRef, G(101));
}

TEST(FieldGroupResolution, PlatformStatesAreIndependentForEveryGroup)
{
    auto parent = LocalGroups(101, -1.0f);
    auto child = LocalGroups(201, -2.0f);
    parent.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = Local(SendFieldGroup{{{G(301), -3.0f}}});
    child.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = Local(MainOutputFieldGroup{G(401), -4.0f});
    child.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {Mode::Inherit, SendFieldGroup{}};
    child.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        Mode::UseShared, SpatialRatingFieldGroup{}};
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &parent},
        FieldGroupDefinitionView{G(2), ContentKind::Sound, G(1), &child}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    const auto output = resolver.Resolve(G(2), HarkenTest::FieldGroupPlatformIds::Windows, Group::MainOutput);
    const auto send = resolver.Resolve(G(2), HarkenTest::FieldGroupPlatformIds::Windows, Group::Send);
    const auto spatial = resolver.Resolve(G(2), HarkenTest::FieldGroupPlatformIds::Windows, Group::SpatialRating);
    ASSERT_TRUE(output.ok());
    ASSERT_TRUE(send.ok());
    ASSERT_TRUE(spatial.ok());
    ExpectSource(*output.value, G(2), HarkenTest::FieldGroupPlatformIds::Windows);
    ExpectSource(*send.value, G(1), HarkenTest::FieldGroupPlatformIds::Windows);
    ExpectSource(*spatial.value, G(2), HarkenTest::FieldGroupPlatformIds::Windows);
    EXPECT_EQ(std::get<MainOutputFieldGroup>(output.value->payload).mainOutputBusRef, G(401));
    EXPECT_EQ(std::get<SendFieldGroup>(send.value->payload),
              (SendFieldGroup{{{G(301), -3.0f}}}));
    EXPECT_EQ(std::get<SpatialRatingFieldGroup>(spatial.value->payload),
              SpatialRatingFieldGroup{});
    EXPECT_TRUE(send.value->effectiveInherit);
    EXPECT_FALSE(spatial.value->effectiveInherit);
    EXPECT_EQ(spatial.value->requestedMode, Mode::UseShared);
}

TEST(FieldGroupResolution, UnrelatedDefinitionsCannotSupplyAnInheritanceGroup)
{
    const auto parent = LocalGroups(101, -1.0f);
    const auto unrelatedTrack = LocalGroups(201, -2.0f);
    const auto unrelatedPiece = LocalGroups(301, -3.0f);
    const DefinitionFieldGroups sound;
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &parent},
        FieldGroupDefinitionView{G(2), ContentKind::Sound, G(1), &sound},
        FieldGroupDefinitionView{G(3), ContentKind::Piece, {}, &unrelatedPiece},
        FieldGroupDefinitionView{G(4), ContentKind::Track, G(3), &unrelatedTrack}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    for (const auto group : kGroups) {
        const auto result = resolver.Resolve(G(2), {}, group);
        ASSERT_TRUE(result.ok());
        ExpectSource(*result.value, G(1));
    }
}

TEST(FieldGroupResolution, DormantReferencesAndSendConflictsAreCheckedOnlyWhenActivated)
{
    const auto parent = LocalGroups(101, -1.0f);
    auto child = LocalGroups(201, -2.0f);
    child.mainOutput.shared.mode = Mode::Inherit;
    child.send.shared.mode = Mode::Inherit;
    child.send.shared.localHistory = SendFieldGroup{{{G(102), -2.0f}, {G(102), -3.0f}}};
    child.spatialRating.shared.mode = Mode::Inherit;
    child.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        Mode::UseShared, MainOutputFieldGroup{G(301), -3.0f}};
    const std::array targets{
        FieldGroupReferenceTarget{G(101), FieldGroupReferenceKind::Bus},
        FieldGroupReferenceTarget{G(102), FieldGroupReferenceKind::Bus},
        FieldGroupReferenceTarget{G(103), FieldGroupReferenceKind::Attenuation}};
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &parent},
        FieldGroupDefinitionView{G(2), ContentKind::Sound, G(1), &child}};
    const auto snapshot = child;
    ASSERT_TRUE(ValidateDefinitionFieldGroups(child).empty());
    {
        const FieldGroupResolver resolver(definitions, kMaster);
        ASSERT_TRUE(resolver.ok());
        for (const auto group : kGroups) {
            const auto result = resolver.Resolve(G(2), HarkenTest::FieldGroupPlatformIds::Windows, group);
            ASSERT_TRUE(result.ok());
            EXPECT_TRUE(ValidateActiveFieldGroup(group, result.value->payload, targets).empty());
        }
        EXPECT_EQ(child, snapshot);
    }
    child.mainOutput.shared.mode = Mode::Local;
    child.send.shared.mode = Mode::Local;
    child.spatialRating.shared.mode = Mode::Local;
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    for (const auto group : {Group::MainOutput, Group::SpatialRating}) {
        const auto result = resolver.Resolve(G(2), {}, group);
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(group, result.value->payload, targets),
                            Code::MissingReference));
    }
    const auto send = resolver.Resolve(G(2), {}, Group::Send);
    ASSERT_TRUE(send.ok());
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::Send, send.value->payload, targets),
                        Code::DuplicateSendTarget));
    EXPECT_EQ(child.mainOutput.shared.localHistory, snapshot.mainOutput.shared.localHistory);
    EXPECT_EQ(child.send.shared.localHistory, snapshot.send.shared.localHistory);
    EXPECT_EQ(child.spatialRating.shared.localHistory, snapshot.spatialRating.shared.localHistory);
}

TEST(FieldGroupResolution, DormantHistoryStillRequiresValidValuesAndRequiredReferences)
{
    DefinitionFieldGroups groups;
    groups.mainOutput.shared.localHistory = MainOutputFieldGroup{};
    groups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        Mode::UseShared, SendFieldGroup{{{G(101), std::numeric_limits<float>::infinity()}}}};
    groups.spatialRating.shared.localHistory = SpatialRatingFieldGroup{};
    groups.spatialRating.shared.localHistory->spatialAttenuationDistanceScaling = -1.0f;
    const auto diagnostics = ValidateDefinitionFieldGroups(groups, G(1));
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_EQ(diagnostics[0].code, Code::EmptyRequiredReference);
    EXPECT_EQ(diagnostics[0].field, "mainOutputBusRef");
    EXPECT_EQ(diagnostics[1].scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_EQ(diagnostics[1].row, 0u);
    EXPECT_EQ(diagnostics[1].field, "sendLevel");
    EXPECT_EQ(diagnostics[2].field, "spatialAttenuationDistanceScaling");
    for (const auto& diagnostic : diagnostics) EXPECT_EQ(diagnostic.definition, G(1));
}

TEST(FieldGroupResolution, PayloadsEnforceFiniteDomainRangesWithoutClamping)
{
    for (const auto value : {-160.0f, -97.0f, 25.0f, 48.0f})
        EXPECT_TRUE(ValidateFieldGroupPayload(
            Group::MainOutput, MainOutputFieldGroup{G(101), value}).empty());
    for (const auto value : {-96.0f, 24.0f}) {
        EXPECT_TRUE(ValidateFieldGroupPayload(
            Group::MainOutput, MainOutputFieldGroup{G(101), value}).empty());
        EXPECT_TRUE(ValidateFieldGroupPayload(
            Group::Send, SendFieldGroup{{{G(101), value}}}).empty());
    }
    for (const auto value : {-97.0f, 25.0f, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(
            Group::Send, SendFieldGroup{{{G(101), value}}}), Code::InvalidValue));
    }
    for (const auto value : {std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()})
        EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(
            Group::MainOutput, MainOutputFieldGroup{G(101), value}), Code::InvalidValue));
    EXPECT_TRUE(ValidateFieldGroupPayload(Group::SpatialRating,
        SpatialRatingFieldGroup{-1.0f, false, {}, 0.01f}).empty());
    EXPECT_TRUE(ValidateFieldGroupPayload(Group::SpatialRating,
        SpatialRatingFieldGroup{1.0f, true, {}, 100.0f}).empty());
    const std::array invalidSpatial{
        SpatialRatingFieldGroup{-1.01f, false, {}, 1.0f},
        SpatialRatingFieldGroup{1.01f, false, {}, 1.0f},
        SpatialRatingFieldGroup{0.0f, false, {}, 0.009f},
        SpatialRatingFieldGroup{0.0f, false, {}, 100.01f},
        SpatialRatingFieldGroup{std::numeric_limits<float>::quiet_NaN(), false, {}, 1.0f},
        SpatialRatingFieldGroup{0.0f, false, {}, std::numeric_limits<float>::infinity()}};
    for (const auto& payload : invalidSpatial)
        EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(Group::SpatialRating, payload),
                            Code::InvalidValue));
    EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(Group::Send, SendFieldGroup{{{{}, 0.0f}}}),
                        Code::EmptyRequiredReference));
    EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(Group::MainOutput, SendFieldGroup{}),
                        Code::PayloadGroupMismatch));
    EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(static_cast<Group>(255), SendFieldGroup{}),
                        Code::UnknownGroup));
}

TEST(FieldGroupResolution, StateShapeRejectsInvalidModesPlatformsAndMissingLocalPayloads)
{
    DefinitionFieldGroups groups;
    groups.mainOutput.shared.mode = Mode::UseShared;
    groups.send.shared.mode = Mode::Local;
    groups.spatialRating.shared.mode = static_cast<Mode>(255);
    groups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Universal].mode = Mode::Inherit;
    groups.send.platforms[harkenprojectschema::AuthoringGuid{}].mode = Mode::Local;
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::iOS].mode = Mode::Local;
    const auto diagnostics = ValidateDefinitionFieldGroups(groups);
    EXPECT_TRUE(HasCode(diagnostics, Code::SharedUseShared));
    EXPECT_TRUE(HasCode(diagnostics, Code::MissingLocalHistory));
    EXPECT_TRUE(HasCode(diagnostics, Code::InvalidMode));
    EXPECT_EQ(std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item) {
        return item.code == Code::InvalidPlatform;
    }), 2);
    EXPECT_EQ(std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item) {
        return item.code == Code::MissingLocalHistory;
    }), 3);
    for (const auto group : kGroups) {
        DefinitionFieldGroups missing;
        switch (group) {
        case Group::MainOutput: missing.mainOutput.shared.mode = Mode::Local; break;
        case Group::Send: missing.send.shared.mode = Mode::Local; break;
        case Group::SpatialRating: missing.spatialRating.shared.mode = Mode::Local; break;
        }
        EXPECT_TRUE(HasCode(ValidateDefinitionFieldGroups(missing), Code::MissingLocalHistory));
    }
}

TEST(FieldGroupResolution, CompleteTreeValidationRejectsMissingParentsAndDuplicateOwnership)
{
    const auto groups = MakeRootFieldGroups(kMaster);
    const std::array missingParent{
        FieldGroupDefinitionView{G(1), ContentKind::Sound, G(99), &groups},
        FieldGroupDefinitionView{G(2), ContentKind::Layer, {}, &groups}};
    const FieldGroupResolver missing(missingParent, kMaster);
    EXPECT_FALSE(missing.ok());
    EXPECT_TRUE(HasCode(missing.diagnostics(), Code::MissingParent));
    EXPECT_FALSE(missing.Resolve(G(2), {}, Group::MainOutput).value);
    const std::array duplicateOwnership{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &groups},
        FieldGroupDefinitionView{G(2), ContentKind::Layer, {}, &groups},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(1), &groups},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(2), &groups}};
    const FieldGroupResolver duplicate(duplicateOwnership, kMaster);
    EXPECT_FALSE(duplicate.ok());
    EXPECT_TRUE(HasCode(duplicate.diagnostics(), Code::DuplicateDefinitionGuid));
    EXPECT_FALSE(duplicate.Resolve(G(3), {}, Group::MainOutput).value);
}

TEST(FieldGroupResolution, CyclesInvalidateTheWholeContextEvenWhenQueriedElsewhere)
{
    const auto groups = LocalGroups(101, -1.0f);
    const std::array cyclic{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, G(2), &groups},
        FieldGroupDefinitionView{G(2), ContentKind::Layer, G(3), &groups},
        FieldGroupDefinitionView{G(3), ContentKind::Layer, G(1), &groups},
        FieldGroupDefinitionView{G(4), ContentKind::Sound, {}, &groups}};
    const FieldGroupResolver cycle(cyclic, kMaster);
    EXPECT_FALSE(cycle.ok());
    EXPECT_TRUE(HasCode(cycle.diagnostics(), Code::ParentCycle));
    EXPECT_FALSE(cycle.Resolve(G(4), {}, Group::MainOutput).value);
    const std::array self{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, G(1), &groups}};
    const FieldGroupResolver selfCycle(self, kMaster);
    EXPECT_TRUE(HasCode(selfCycle.diagnostics(), Code::ParentCycle));
    EXPECT_FALSE(selfCycle.Resolve(G(1), {}, Group::MainOutput).value);
}

TEST(FieldGroupResolution, UnknownKindsAndMissingAuthoringStatesCannotEnterTheResolver)
{
    const auto groups = MakeRootFieldGroups(kMaster);
    const std::array invalid{
        FieldGroupDefinitionView{{}, ContentKind::Sound, {}, &groups},
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &groups},
        FieldGroupDefinitionView{G(2), static_cast<ContentKind>(5), G(1), &groups},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(1), nullptr}};
    const FieldGroupResolver resolver(invalid, {});
    EXPECT_FALSE(resolver.ok());
    EXPECT_TRUE(HasCode(resolver.diagnostics(), Code::InvalidMasterBus));
    EXPECT_TRUE(HasCode(resolver.diagnostics(), Code::InvalidDefinitionGuid));
    EXPECT_TRUE(HasCode(resolver.diagnostics(), Code::InvalidContentKind));
    EXPECT_TRUE(HasCode(resolver.diagnostics(), Code::MissingGroupState));
    EXPECT_FALSE(resolver.Resolve(G(1), {}, Group::MainOutput).value);
}

TEST(FieldGroupResolution, InvalidQueriesReturnErrorsInsteadOfDefaults)
{
    const auto groups = MakeRootFieldGroups(kMaster);
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Sound, {}, &groups}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Universal, harkenprojectschema::AuthoringGuid{}}) {
        const auto result = resolver.Resolve(G(1), platform, Group::MainOutput);
        EXPECT_FALSE(result.value);
        EXPECT_TRUE(HasCode(result.diagnostics, Code::InvalidPlatform));
    }
    const auto unknownDefinition = resolver.Resolve(G(99), {}, Group::MainOutput);
    EXPECT_FALSE(unknownDefinition.value);
    EXPECT_TRUE(HasCode(unknownDefinition.diagnostics, Code::UnknownDefinition));
    const auto unknownGroup = resolver.Resolve(G(1), {}, static_cast<Group>(255));
    EXPECT_FALSE(unknownGroup.value);
    EXPECT_TRUE(HasCode(unknownGroup.diagnostics, Code::UnknownGroup));
}

TEST(FieldGroupResolution, BorrowedAuthoringStateIsValidatedAgainBeforeItIsRead)
{
    auto parent = LocalGroups(101, -1.0f);
    DefinitionFieldGroups child;
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &parent},
        FieldGroupDefinitionView{G(2), ContentKind::Sound, G(1), &child}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    parent.mainOutput.shared.localHistory.reset();
    const auto missing = resolver.Resolve(G(2), {}, Group::MainOutput);
    EXPECT_FALSE(missing.value);
    EXPECT_TRUE(HasCode(missing.diagnostics, Code::MissingLocalHistory));
    parent.mainOutput.shared = Local(MainOutputFieldGroup{G(201), -2.0f});
    const auto changed = resolver.Resolve(G(2), {}, Group::MainOutput);
    ASSERT_TRUE(changed.ok());
    EXPECT_EQ(std::get<MainOutputFieldGroup>(changed.value->payload),
              (MainOutputFieldGroup{G(201), -2.0f}));
    child.mainOutput.shared.mode = static_cast<Mode>(255);
    const auto invalidMode = resolver.Resolve(G(2), {}, Group::MainOutput);
    EXPECT_FALSE(invalidMode.value);
    EXPECT_TRUE(HasCode(invalidMode.diagnostics, Code::InvalidMode));
    child.mainOutput.shared.mode = Mode::Inherit;
    parent.mainOutput.shared.localHistory->outputBusVolume = std::numeric_limits<float>::infinity();
    const auto invalidRange = resolver.Resolve(G(2), {}, Group::MainOutput);
    EXPECT_FALSE(invalidRange.value);
    EXPECT_TRUE(HasCode(invalidRange.diagnostics, Code::InvalidValue));
    parent.mainOutput.shared.localHistory->outputBusVolume = -2.0f;
    parent.mainOutput.shared.mode = Mode::Inherit;
    const auto rootCurrent = resolver.Resolve(G(2), {}, Group::MainOutput);
    ASSERT_TRUE(rootCurrent.ok());
    EXPECT_EQ(std::get<MainOutputFieldGroup>(rootCurrent.value->payload),
              (MainOutputFieldGroup{G(201), -2.0f}));
    parent.mainOutput.shared.localHistory.reset();
    const auto missingRootCurrent = resolver.Resolve(G(2), {}, Group::MainOutput);
    EXPECT_FALSE(missingRootCurrent.value);
    EXPECT_TRUE(HasCode(missingRootCurrent.diagnostics, Code::MissingLocalHistory));
}

TEST(FieldGroupResolution, ActiveReferencesRequireTheCorrectTargetKinds)
{
    const std::array targets{
        FieldGroupReferenceTarget{G(101), FieldGroupReferenceKind::Bus},
        FieldGroupReferenceTarget{G(102), FieldGroupReferenceKind::Attenuation},
        FieldGroupReferenceTarget{G(103), FieldGroupReferenceKind::Other}};
    EXPECT_TRUE(ValidateActiveFieldGroup(Group::MainOutput,
        MainOutputFieldGroup{G(101), 0.0f}, targets).empty());
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::MainOutput,
        MainOutputFieldGroup{G(102), 0.0f}, targets), Code::WrongReferenceKind));
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::Send,
        SendFieldGroup{{{G(103), 0.0f}}}, targets), Code::WrongReferenceKind));
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::MainOutput,
        MainOutputFieldGroup{G(999), 0.0f}, targets), Code::MissingReference));
    auto spatial = SpatialRatingFieldGroup{};
    EXPECT_TRUE(ValidateActiveFieldGroup(Group::SpatialRating, spatial, {}).empty());
    spatial.spatialAttenuationRef = G(102);
    EXPECT_TRUE(ValidateActiveFieldGroup(Group::SpatialRating, spatial, targets).empty());
    spatial.spatialAttenuationRef = G(101);
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::SpatialRating, spatial, targets),
                        Code::WrongReferenceKind));
    EXPECT_TRUE(ValidateActiveFieldGroup(Group::Send, SendFieldGroup{}, {}).empty());
}

TEST(FieldGroupResolution, InvalidReferenceCatalogsCannotProduceAValidActiveGroup)
{
    const auto payload = MainOutputFieldGroup{G(101), 0.0f};
    const std::array duplicate{
        FieldGroupReferenceTarget{G(101), FieldGroupReferenceKind::Bus},
        FieldGroupReferenceTarget{G(101), FieldGroupReferenceKind::Bus}};
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::MainOutput, payload, duplicate),
                        Code::InvalidReferenceCatalog));
    const std::array invalid{
        FieldGroupReferenceTarget{{}, FieldGroupReferenceKind::Bus},
        FieldGroupReferenceTarget{G(101), static_cast<FieldGroupReferenceKind>(255)}};
    EXPECT_TRUE(HasCode(ValidateActiveFieldGroup(Group::MainOutput, payload, invalid),
                        Code::InvalidReferenceCatalog));
}

TEST(FieldGroupResolution, FieldOwnershipContainsOnlyTheThreeApprovedGroups)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    struct OwnedField {
        std::string_view name;
        Property property;
        Group group;
    };
    constexpr std::array fields{
        OwnedField{"mainOutputBusRef", Property::MainOutputBusRef, Group::MainOutput},
        OwnedField{"outputBusVolume", Property::OutputBusVolume, Group::MainOutput},
        OwnedField{"sends", Property::Sends, Group::Send},
        OwnedField{"spatialPanning", Property::SpatialPanning, Group::SpatialRating},
        OwnedField{"spatialIs3DEnabled", Property::SpatialIs3DEnabled, Group::SpatialRating},
        OwnedField{"spatialAttenuationDistanceScaling", Property::SpatialAttenuationDistanceScaling,
                   Group::SpatialRating},
        OwnedField{"spatialAttenuationRef", Property::SpatialAttenuationRef, Group::SpatialRating}};
    std::uint64_t bits = 0;
    for (const auto& field : fields) {
        EXPECT_EQ(FieldGroupForField(field.name), field.group);
        EXPECT_EQ(FieldGroupForProperty(field.property), field.group);
        const auto bit = UINT64_C(1) << (static_cast<unsigned>(field.property) - 1);
        EXPECT_NE(FieldGroupPropertyMask(field.group) & bit, 0u);
        bits |= bit;
    }
    EXPECT_EQ(bits, kFieldGroupPropertyMask);
    EXPECT_EQ(FieldGroupPropertyMask(Group::MainOutput) |
                  FieldGroupPropertyMask(Group::Send) |
                  FieldGroupPropertyMask(Group::SpatialRating),
              kFieldGroupPropertyMask);
    for (const auto field : {"volume", "pitch", "makeUpGain", "streamingMode", "signalGraph",
                             "volumeBinding", "pitchBinding", "bindings"})
        EXPECT_FALSE(FieldGroupForField(field));
    for (const auto property : {Property::Volume, Property::Pitch, Property::MakeUpGain,
                                Property::LoopCount, Property::StreamingMode, Property::SignalGraph})
        EXPECT_FALSE(FieldGroupForProperty(property));
    EXPECT_EQ(FieldGroupPropertyMask(static_cast<Group>(255)), 0u);
    EXPECT_TRUE(FieldGroupName(static_cast<Group>(255)).empty());
}

}
}
