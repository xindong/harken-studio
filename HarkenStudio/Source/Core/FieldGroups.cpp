#include "FieldGroups.h"

#include <cmath>
#include <set>
#include <utility>

namespace VNX {
namespace {

using harkenprojectschema::AuthoringGuid;
using harkenprojectschema::ContentKind;

using DiagnosticCode = FieldGroupDiagnosticCode;
using Diagnostics = std::vector<FieldGroupDiagnostic>;
using PropertyId = harkenplayableproperties::PlayablePropertyIdV1;

void AddDiagnostic(Diagnostics& diagnostics, DiagnosticCode code,
                   std::optional<FieldGroupId> group = {},
                   std::string_view field = {},
                   std::optional<std::size_t> row = {},
                   AuthoringGuid relatedGuid = {})
{
    diagnostics.push_back({code, {}, group, {}, field, row, relatedGuid});
}

bool IsContentKindValid(ContentKind kind)
{
    switch (kind) {
    case ContentKind::Layer:
    case ContentKind::Sound:
    case ContentKind::Container:
    case ContentKind::Track:
    case ContentKind::Piece:
        return true;
    }
    return false;
}

bool IsModeValid(FieldGroupMode mode)
{
    switch (mode) {
    case FieldGroupMode::Inherit:
    case FieldGroupMode::Local:
    case FieldGroupMode::UseShared:
        return true;
    }
    return false;
}

bool InRange(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

template <typename Payload>
void ValidateState(const GroupState<Payload>& state, FieldGroupId group,
                   FieldGroupScope scope, AuthoringGuid definition,
                   Diagnostics& diagnostics)
{
    const auto first = diagnostics.size();
    if (!IsModeValid(state.mode)) {
        AddDiagnostic(diagnostics, DiagnosticCode::InvalidMode, group, "mode");
    } else if (!scope && state.mode == FieldGroupMode::UseShared) {
        AddDiagnostic(diagnostics, DiagnosticCode::SharedUseShared, group, "mode");
    } else if (!scope && state.mode == FieldGroupMode::Local && !state.localHistory) {
        AddDiagnostic(diagnostics, DiagnosticCode::MissingLocalHistory,
                      group, "localHistory");
    }
    if (state.localHistory) {
        if (state.localFields == 0 || (state.localFields & ~FieldGroupPropertyMask(group)) != 0
            || (!scope && state.localFields != FieldGroupPropertyMask(group)))
            AddDiagnostic(diagnostics, DiagnosticCode::InvalidValue, group, "values");
        auto payloadDiagnostics = ValidateFieldGroupPayload(group, *state.localHistory);
        for (const auto& diagnostic : payloadDiagnostics) {
            bool present = false;
            VisitFieldGroupFields(*state.localHistory, [&](auto property, const char* field, const auto&) {
                if (diagnostic.field == field || group == FieldGroupId::Send)
                    present = (state.localFields & (FieldGroupFieldMask(property))) != 0;
            });
            if (present) diagnostics.push_back(diagnostic);
        }
    }
    for (auto i = first; i < diagnostics.size(); ++i) {
        diagnostics[i].definition = definition;
        diagnostics[i].scope = scope;
    }
}

template <typename Payload>
void ValidateAuthoredGroup(const AuthoredGroup<Payload>& authored,
                          FieldGroupId group, AuthoringGuid definition,
                          Diagnostics& diagnostics)
{
    ValidateState(authored.shared, group, {}, definition, diagnostics);
    for (const auto& [platform, state] : authored.platforms) {
        if (harkenstudio::IsNull(platform)) {
            diagnostics.push_back({DiagnosticCode::InvalidPlatform, definition,
                                   group, platform, "platforms", {}, {}});
        }
        ValidateState(state, group, platform, definition, diagnostics);
        const auto local = ResolveFieldGroupLocal(authored, platform);
        if (local.mode == FieldGroupMode::Local
            && (!local.localHistory || local.localFields != FieldGroupPropertyMask(group)))
            diagnostics.push_back({DiagnosticCode::MissingLocalHistory, definition,
                                   group, platform, "values", {}, {}});
    }
}

template <typename Payload>
void ValidateRootGroup(const AuthoredGroup<Payload>& authored,
                       FieldGroupId group, AuthoringGuid definition,
                       Diagnostics& diagnostics)
{
    const auto validate = [&](FieldGroupScope scope) {
        const auto state = ResolveFieldGroupLocal(authored, scope);
        if (!state.localHistory || state.localFields != FieldGroupPropertyMask(group))
            diagnostics.push_back({DiagnosticCode::MissingLocalHistory, definition,
                                   group, scope, "localHistory", {}, {}});
    };
    validate({});
    for (const auto& [platform, state] : authored.platforms)
        validate(platform);
}

template <typename Payload>
FieldGroupResolutionResult ResolveGroup(
    const std::map<AuthoringGuid, FieldGroupDefinitionView>& definitions,
    const FieldGroupDefinitionView& requestedDefinition,
    FieldGroupScope scope, FieldGroupId group,
    const AuthoredGroup<Payload> DefinitionFieldGroups::* member)
{
    ResolvedFieldGroup result;
    result.group = group;
    const auto* definition = &requestedDefinition;
    while (true) {
        const auto& authored = definition->groups->*member;
        Diagnostics diagnostics;
        ValidateAuthoredGroup(authored, group, definition->guid, diagnostics);
        if (harkenstudio::IsNull(definition->parentGuid))
            ValidateRootGroup(authored, group, definition->guid, diagnostics);
        if (!diagnostics.empty()) return {{}, std::move(diagnostics)};
        const auto selected = ResolveFieldGroupLocal(authored, scope);
        const auto platform = scope ? authored.platforms.find(*scope) : authored.platforms.end();
        if (definition == &requestedDefinition) {
            result.requestedMode = !scope ? authored.shared.mode
                : platform == authored.platforms.end() ? FieldGroupMode::UseShared : platform->second.mode;
            result.effectiveInherit = selected.mode == FieldGroupMode::Inherit;
        }
        if (selected.mode == FieldGroupMode::Local
            || harkenstudio::IsNull(definition->parentGuid)) {
            if (!selected.localHistory || selected.localFields != FieldGroupPropertyMask(group)) {
                diagnostics.push_back({DiagnosticCode::MissingLocalHistory, definition->guid,
                                       group, scope, "values", {}, {}});
                return {{}, std::move(diagnostics)};
            }
            result.payload = *selected.localHistory;
            result.source = {definition->guid, platform != authored.platforms.end()
                && platform->second.localHistory ? scope : FieldGroupScope{}, false};
            return {std::move(result), {}};
        }
        definition = &definitions.at(definition->parentGuid);
    }
}

}

bool IsFieldGroupScopeValid(FieldGroupScope scope) noexcept
{
    return !scope || !harkenprojectschema::IsNull(*scope);
}

DefinitionFieldGroups MakeRootFieldGroups(AuthoringGuid masterBusGuid)
{
    DefinitionFieldGroups groups;
    groups.mainOutput.shared = {
        FieldGroupMode::Inherit, MainOutputFieldGroup{masterBusGuid, 0.0f}};
    groups.send.shared = {FieldGroupMode::Inherit, SendFieldGroup{}};
    groups.spatialRating.shared = {
        FieldGroupMode::Inherit, SpatialRatingFieldGroup{}};
    return groups;
}

std::string_view FieldGroupName(FieldGroupId group) noexcept
{
    switch (group) {
    case FieldGroupId::MainOutput: return "mainOutput";
    case FieldGroupId::Send: return "send";
    case FieldGroupId::SpatialRating: return "spatialRating";
    }
    return {};
}

std::optional<FieldGroupId> FieldGroupForField(std::string_view field) noexcept
{
    if (field == "mainOutputBusRef" || field == "outputBusVolume")
        return FieldGroupId::MainOutput;
    if (field == "sends") return FieldGroupId::Send;
    if (field == "spatialPanning" || field == "spatialIs3DEnabled" ||
        field == "spatialAttenuationRef" ||
        field == "spatialAttenuationDistanceScaling")
        return FieldGroupId::SpatialRating;
    return {};
}

std::optional<FieldGroupId> FieldGroupForProperty(PropertyId property) noexcept
{
    switch (property) {
    case PropertyId::MainOutputBusRef:
    case PropertyId::OutputBusVolume:
        return FieldGroupId::MainOutput;
    case PropertyId::Sends:
        return FieldGroupId::Send;
    case PropertyId::SpatialPanning:
    case PropertyId::SpatialIs3DEnabled:
    case PropertyId::SpatialAttenuationDistanceScaling:
    case PropertyId::SpatialAttenuationRef:
        return FieldGroupId::SpatialRating;
    default:
        return {};
    }
}

std::uint64_t FieldGroupPropertyMask(FieldGroupId group) noexcept
{
    switch (group) {
    case FieldGroupId::MainOutput: return UINT64_C(0x60);
    case FieldGroupId::Send: return UINT64_C(0x80);
    case FieldGroupId::SpatialRating: return UINT64_C(0x2d00);
    }
    return 0;
}

std::vector<FieldGroupDiagnostic> ValidateFieldGroupPayload(
    FieldGroupId group, const FieldGroupPayload& payload)
{
    Diagnostics diagnostics;
    switch (group) {
    case FieldGroupId::MainOutput: {
        const auto* mainOutput = std::get_if<MainOutputFieldGroup>(&payload);
        if (!mainOutput) {
            AddDiagnostic(diagnostics, DiagnosticCode::PayloadGroupMismatch, group);
            break;
        }
        if (harkenstudio::IsNull(mainOutput->mainOutputBusRef))
            AddDiagnostic(diagnostics, DiagnosticCode::EmptyRequiredReference,
                          group, "mainOutputBusRef");
        if (!std::isfinite(mainOutput->outputBusVolume))
            AddDiagnostic(diagnostics, DiagnosticCode::InvalidValue,
                          group, "outputBusVolume");
        break;
    }
    case FieldGroupId::Send: {
        const auto* send = std::get_if<SendFieldGroup>(&payload);
        if (!send) {
            AddDiagnostic(diagnostics, DiagnosticCode::PayloadGroupMismatch, group);
            break;
        }
        for (std::size_t i = 0; i < send->sends.size(); ++i) {
            const auto& row = send->sends[i];
            if (harkenstudio::IsNull(row.targetBusID))
                AddDiagnostic(diagnostics, DiagnosticCode::EmptyRequiredReference,
                              group, "targetBusID", i);
            if (!InRange(row.sendLevel, -96.0f, 24.0f))
                AddDiagnostic(diagnostics, DiagnosticCode::InvalidValue,
                              group, "sendLevel", i);
        }
        break;
    }
    case FieldGroupId::SpatialRating: {
        const auto* spatial = std::get_if<SpatialRatingFieldGroup>(&payload);
        if (!spatial) {
            AddDiagnostic(diagnostics, DiagnosticCode::PayloadGroupMismatch, group);
            break;
        }
        if (!InRange(spatial->spatialPanning, -1.0f, 1.0f))
            AddDiagnostic(diagnostics, DiagnosticCode::InvalidValue,
                          group, "spatialPanning");
        if (!InRange(spatial->spatialAttenuationDistanceScaling, 0.01f, 100.0f))
            AddDiagnostic(diagnostics, DiagnosticCode::InvalidValue,
                          group, "spatialAttenuationDistanceScaling");
        break;
    }
    default:
        AddDiagnostic(diagnostics, DiagnosticCode::UnknownGroup, group);
        break;
    }
    return diagnostics;
}

std::vector<FieldGroupDiagnostic> ValidateDefinitionFieldGroups(
    const DefinitionFieldGroups& groups, AuthoringGuid definition)
{
    Diagnostics diagnostics;
    ValidateAuthoredGroup(groups.mainOutput, FieldGroupId::MainOutput,
                          definition, diagnostics);
    ValidateAuthoredGroup(groups.send, FieldGroupId::Send, definition, diagnostics);
    ValidateAuthoredGroup(groups.spatialRating, FieldGroupId::SpatialRating,
                          definition, diagnostics);
    return diagnostics;
}

std::vector<FieldGroupDiagnostic> ValidateActiveFieldGroup(
    FieldGroupId group, const FieldGroupPayload& payload,
    std::span<const FieldGroupReferenceTarget> targets)
{
    return FieldGroupReferenceCatalog(targets).Validate(group, payload);
}

FieldGroupReferenceCatalog::FieldGroupReferenceCatalog(
    std::span<const FieldGroupReferenceTarget> targets)
{
    for (const auto& target : targets) {
        const bool validKind = target.kind == FieldGroupReferenceKind::Bus ||
                               target.kind == FieldGroupReferenceKind::Attenuation ||
                               target.kind == FieldGroupReferenceKind::Other;
        if (harkenstudio::IsNull(target.guid) || !validKind ||
            !targets_.emplace(target.guid, target.kind).second)
            invalidTargets_.push_back(target.guid);
    }
}

std::vector<FieldGroupDiagnostic> FieldGroupReferenceCatalog::Validate(
    FieldGroupId group, const FieldGroupPayload& payload) const
{
    auto diagnostics = ValidateFieldGroupPayload(group, payload);
    if (!diagnostics.empty()) return diagnostics;
    for (const auto guid : invalidTargets_)
        AddDiagnostic(diagnostics, DiagnosticCode::InvalidReferenceCatalog,
                      group, {}, {}, guid);
    if (!diagnostics.empty()) return diagnostics;
    const auto validateReference = [&](AuthoringGuid guid,
                                       FieldGroupReferenceKind expectedKind,
                                       std::string_view field,
                                       std::optional<std::size_t> row = {}) {
        const auto target = targets_.find(guid);
        if (target == targets_.end()) {
            AddDiagnostic(diagnostics, DiagnosticCode::MissingReference,
                          group, field, row, guid);
        } else if (target->second != expectedKind) {
            AddDiagnostic(diagnostics, DiagnosticCode::WrongReferenceKind,
                          group, field, row, guid);
        }
    };
    switch (group) {
    case FieldGroupId::MainOutput:
        validateReference(std::get<MainOutputFieldGroup>(payload).mainOutputBusRef,
                          FieldGroupReferenceKind::Bus, "mainOutputBusRef");
        break;
    case FieldGroupId::Send: {
        std::set<AuthoringGuid> seen;
        const auto& sends = std::get<SendFieldGroup>(payload).sends;
        for (std::size_t i = 0; i < sends.size(); ++i) {
            const auto guid = sends[i].targetBusID;
            if (!seen.insert(guid).second)
                AddDiagnostic(diagnostics, DiagnosticCode::DuplicateSendTarget,
                              group, "targetBusID", i, guid);
            validateReference(guid, FieldGroupReferenceKind::Bus, "targetBusID", i);
        }
        break;
    }
    case FieldGroupId::SpatialRating: {
        const auto guid = std::get<SpatialRatingFieldGroup>(payload).spatialAttenuationRef;
        if (!harkenstudio::IsNull(guid))
            validateReference(guid, FieldGroupReferenceKind::Attenuation,
                              "spatialAttenuationRef");
        break;
    }
    }
    return diagnostics;
}

FieldGroupResolver::FieldGroupResolver(
    std::span<const FieldGroupDefinitionView> definitions,
    AuthoringGuid masterBusGuid)
{
    if (harkenstudio::IsNull(masterBusGuid))
        AddDiagnostic(diagnostics_, DiagnosticCode::InvalidMasterBus);
    for (const auto& definition : definitions) {
        const auto first = diagnostics_.size();
        if (harkenstudio::IsNull(definition.guid))
            AddDiagnostic(diagnostics_, DiagnosticCode::InvalidDefinitionGuid);
        if (!IsContentKindValid(definition.kind))
            AddDiagnostic(diagnostics_, DiagnosticCode::InvalidContentKind);
        if (!definition.groups) {
            AddDiagnostic(diagnostics_, DiagnosticCode::MissingGroupState);
        } else {
            auto stateDiagnostics = ValidateDefinitionFieldGroups(*definition.groups);
            diagnostics_.insert(diagnostics_.end(), stateDiagnostics.begin(),
                                stateDiagnostics.end());
            if (harkenstudio::IsNull(definition.parentGuid)) {
                ValidateRootGroup(definition.groups->mainOutput,
                    FieldGroupId::MainOutput, definition.guid, diagnostics_);
                ValidateRootGroup(definition.groups->send,
                    FieldGroupId::Send, definition.guid, diagnostics_);
                ValidateRootGroup(definition.groups->spatialRating,
                    FieldGroupId::SpatialRating, definition.guid, diagnostics_);
            }
        }
        if (!harkenstudio::IsNull(definition.guid) &&
            !definitions_.emplace(definition.guid, definition).second)
            AddDiagnostic(diagnostics_, DiagnosticCode::DuplicateDefinitionGuid);
        for (auto i = first; i < diagnostics_.size(); ++i)
            diagnostics_[i].definition = definition.guid;
    }
    for (const auto& [guid, definition] : definitions_) {
        if (harkenstudio::IsNull(definition.parentGuid)) continue;
        const auto parent = definitions_.find(definition.parentGuid);
        if (parent == definitions_.end()) {
            diagnostics_.push_back({DiagnosticCode::MissingParent, guid, {}, {},
                                    "parentGuid", {}, definition.parentGuid});
        } else if (IsContentKindValid(parent->second.kind) &&
                   IsContentKindValid(definition.kind) &&
                   !MayOwn(parent->second.kind, definition.kind)) {
            diagnostics_.push_back({DiagnosticCode::IllegalParentKind, guid, {}, {},
                                    "parentGuid", {}, definition.parentGuid});
        }
    }
    std::map<AuthoringGuid, std::uint8_t> colors;
    for (const auto& [guid, definition] : definitions_) {
        if (colors[guid] != 0) continue;
        std::vector<AuthoringGuid> path;
        auto current = guid;
        while (!harkenstudio::IsNull(current) && definitions_.contains(current)) {
            auto& color = colors[current];
            if (color == 2) break;
            if (color == 1) {
                diagnostics_.push_back({DiagnosticCode::ParentCycle, current, {}, {},
                                        "parentGuid", {}, current});
                break;
            }
            color = 1;
            path.push_back(current);
            current = definitions_.at(current).parentGuid;
        }
        for (const auto visited : path) colors[visited] = 2;
    }
}

FieldGroupResolutionResult FieldGroupResolver::Resolve(
    AuthoringGuid definition, FieldGroupScope scope, FieldGroupId group) const
{
    if (!ok()) return {{}, diagnostics_};
    FieldGroupResolutionResult result;
    if (!IsFieldGroupScopeValid(scope))
        result.diagnostics.push_back({DiagnosticCode::InvalidPlatform, definition,
                                      group, scope, {}, {}, {}});
    if (FieldGroupName(group).empty())
        result.diagnostics.push_back({DiagnosticCode::UnknownGroup, definition,
                                      group, scope, {}, {}, {}});
    const auto found = definitions_.find(definition);
    if (found == definitions_.end())
        result.diagnostics.push_back({DiagnosticCode::UnknownDefinition, definition,
                                      group, scope, {}, {}, {}});
    if (!result.diagnostics.empty()) return result;
    switch (group) {
    case FieldGroupId::MainOutput:
        return ResolveGroup(definitions_, found->second, scope, group,
                            &DefinitionFieldGroups::mainOutput);
    case FieldGroupId::Send:
        return ResolveGroup(definitions_, found->second, scope, group,
                            &DefinitionFieldGroups::send);
    case FieldGroupId::SpatialRating:
        return ResolveGroup(definitions_, found->second, scope, group,
                            &DefinitionFieldGroups::spatialRating);
    }
    return result;
}

}
