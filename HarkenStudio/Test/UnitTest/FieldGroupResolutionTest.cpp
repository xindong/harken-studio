#include "FieldGroups.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>

namespace VNX {
namespace {

using namespace harkenprojectschema;

using Group = FieldGroupId;
using Mode = FieldGroupMode;
using Code = FieldGroupDiagnosticCode;

constexpr AuthoringGuid G(std::uint64_t value)
{
    return {0, value};
}

constexpr auto kMaster = G(100);
template <typename Payload>
GroupState<Payload> Local(Payload payload)
{
    return {Mode::Local, std::move(payload)};
}

DefinitionFieldGroups LocalGroups(std::uint64_t bus, float volume)
{
    DefinitionFieldGroups groups;
    groups.output.shared = Local(OutputFieldGroup{G(bus), volume});
    groups.send.shared = Local(SendFieldGroup{{{G(bus + 1), volume}}});
    groups.spatialRating.shared = Local(
        SpatialRatingFieldGroup{0.5f, true, G(bus + 2), 2.0f});
    groups.effects.shared = Local(EffectFieldGroup{});
    return groups;
}

bool HasCode(const std::vector<FieldGroupDiagnostic>& diagnostics, Code code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
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
    const std::array duplicateOwnership{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &groups},
        FieldGroupDefinitionView{G(2), ContentKind::Layer, {}, &groups},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(1), &groups},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(2), &groups}};
    const FieldGroupResolver duplicate(duplicateOwnership, kMaster);
    EXPECT_FALSE(duplicate.ok());
    EXPECT_TRUE(HasCode(duplicate.diagnostics(), Code::DuplicateDefinitionGuid));
    EXPECT_FALSE(duplicate.Resolve(G(3), {}, Group::Output).value);
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
    const std::array self{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, G(1), &groups}};
    const FieldGroupResolver selfCycle(self, kMaster);
    EXPECT_TRUE(HasCode(selfCycle.diagnostics(), Code::ParentCycle));
    EXPECT_FALSE(selfCycle.Resolve(G(1), {}, Group::Output).value);
}

TEST(FieldGroupResolution, EveryGroupSelectsOneOwnershipSourceAndRespectsPlatformMode)
{
    const auto root = MakeRootFieldGroups(kMaster);
    auto parent = LocalGroups(101, -3.0f);
    DefinitionFieldGroups child;
    const auto platform = G(200);
    parent.output.platforms[platform] = {Mode::Inherit, {}};
    parent.send.platforms[platform] = {Mode::Inherit, {}};
    parent.spatialRating.platforms[platform] = {Mode::Inherit, {}};
    parent.effects.platforms[platform] = {Mode::Inherit, {}};
    const std::array definitions{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, {}, &root},
        FieldGroupDefinitionView{G(2), ContentKind::Container, G(1), &parent},
        FieldGroupDefinitionView{G(3), ContentKind::Sound, G(2), &child}};
    const FieldGroupResolver resolver(definitions, kMaster);
    ASSERT_TRUE(resolver.ok());
    for (const auto group : {Group::Output, Group::Send, Group::SpatialRating, Group::Effect}) {
        SCOPED_TRACE(FieldGroupName(group));
        const auto shared = resolver.Resolve(G(3), {}, group);
        const auto overridden = resolver.Resolve(G(3), platform, group);
        ASSERT_TRUE(shared.ok());
        ASSERT_TRUE(overridden.ok());
        EXPECT_EQ(shared.value->source.definition, G(2));
        EXPECT_EQ(overridden.value->source.definition, G(1));
        EXPECT_TRUE(shared.value->effectiveInherit);
        EXPECT_TRUE(overridden.value->effectiveInherit);
        EXPECT_EQ(shared.value->payload, resolver.Resolve(G(2), {}, group).value->payload);
        EXPECT_EQ(overridden.value->payload, resolver.Resolve(G(1), platform, group).value->payload);
    }
}

TEST(FieldGroupResolution, RejectedEffectReplacementPreservesStateAndParsedChainsCannotBypassCapacity)
{
    std::vector<EffectEntry> rows(EffectChainLedger::kCapacity);
    for (auto& row : rows) {
        row.guid = QUuid::createUuid();
        row.mode = EffectMode::Custom;
    }
    EffectChainLedger chain;
    ASSERT_TRUE(chain.replace(rows));
    const auto saved = chain.toJson();
    auto extra = rows.back();
    extra.guid = QUuid::createUuid();
    rows.push_back(extra);
    EXPECT_FALSE(chain.replace(rows));
    EXPECT_EQ(chain.toJson(), saved);
    auto oversized = saved;
    oversized.append(extra.toJson());
    const EffectFieldGroup parsed{EffectChainLedger::fromJson(oversized)};
    EXPECT_TRUE(HasCode(ValidateFieldGroupPayload(Group::Effect, parsed), Code::InvalidValue));
    EXPECT_EQ(parsed.effects.toJson(), oversized);
}

}
}
