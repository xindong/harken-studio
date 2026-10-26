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
    const std::array self{
        FieldGroupDefinitionView{G(1), ContentKind::Layer, G(1), &groups}};
    const FieldGroupResolver selfCycle(self, kMaster);
    EXPECT_TRUE(HasCode(selfCycle.diagnostics(), Code::ParentCycle));
    EXPECT_FALSE(selfCycle.Resolve(G(1), {}, Group::MainOutput).value);
}

}
}
