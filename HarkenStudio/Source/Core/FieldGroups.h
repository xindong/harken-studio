#pragma once

#include "harken_project_schema/Model.h"
#include "EffectChainSemantics.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace harken::studio {

using FieldGroupScope = std::optional<harken::studio::project_schema::AuthoringGuid>;

enum class FieldGroupId : std::uint8_t {
    Output,
    Send,
    SpatialRating,
    Effect,
    Conversion
};

enum class FieldGroupMode : std::uint8_t {
    Inherit,
    Local,
    UseShared
};

std::uint64_t FieldGroupPropertyMask(FieldGroupId group) noexcept;

constexpr std::uint64_t FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1 property)
{
    harkenplayableproperties::ChassisPresenceV1 fields;
    fields.Set(property);
    return fields.bits;
}

struct OutputFieldGroup {
    static constexpr auto groupId = FieldGroupId::Output;
    harken::studio::project_schema::AuthoringGuid outputBusRef{};
    float outputBusVolume = 0.0f;

    bool operator==(const OutputFieldGroup&) const = default;
};

struct SendFieldGroupRow {
    harken::studio::project_schema::AuthoringGuid targetBusID{};
    float sendLevel = 0.0f;
    harken::studio::project_schema::SendType type = harken::studio::project_schema::SendType::Send;
    bool duckTarget = false;

    bool operator==(const SendFieldGroupRow&) const = default;
};

struct SendFieldGroup {
    static constexpr auto groupId = FieldGroupId::Send;
    std::vector<SendFieldGroupRow> sends;

    bool operator==(const SendFieldGroup&) const = default;
};

struct SpatialRatingFieldGroup {
    static constexpr auto groupId = FieldGroupId::SpatialRating;
    float spatialPanning = 0.0f;
    bool spatialIs3DEnabled = false;
    harken::studio::project_schema::AuthoringGuid spatialAttenuationRef{};
    float spatialAttenuationDistanceScaling = 1.0f;

    bool operator==(const SpatialRatingFieldGroup&) const = default;
};

struct EffectFieldGroup {
    static constexpr auto groupId = FieldGroupId::Effect;
    EffectChainLedger effects;

    bool operator==(const EffectFieldGroup&) const = default;
};

enum class ConversionFieldId : std::uint8_t {
    Settings
};

constexpr std::uint64_t FieldGroupFieldMask(ConversionFieldId)
{
    return UINT64_C(1);
}

struct ConversionFieldGroup {
    static constexpr auto groupId = FieldGroupId::Conversion;
    harken::studio::project_schema::AuthoringGuid settingsRef{};

    bool operator==(const ConversionFieldGroup&) const = default;
};

template <typename Payload>
struct GroupState {
    FieldGroupMode mode = FieldGroupMode::Inherit;
    std::optional<Payload> localHistory;
    std::uint64_t localFields = FieldGroupPropertyMask(Payload::groupId);

    bool operator==(const GroupState&) const = default;
};

template <typename Payload>
struct AuthoredGroup {
    GroupState<Payload> shared;
    std::map<harken::studio::project_schema::AuthoringGuid, GroupState<Payload>> platforms;

    bool operator==(const AuthoredGroup&) const = default;
};

template <typename Payload, typename Visitor>
void VisitFieldGroupFields(Payload& payload, Visitor&& visit)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    using Type = std::remove_cv_t<Payload>;
    if constexpr (std::is_same_v<Type, OutputFieldGroup>) {
        visit(Property::OutputBusRef, "outputBusRef", payload.outputBusRef);
        visit(Property::OutputBusVolume, "outputBusVolume", payload.outputBusVolume);
    } else if constexpr (std::is_same_v<Type, SendFieldGroup>) {
        visit(Property::Sends, "sends", payload.sends);
    } else if constexpr (std::is_same_v<Type, SpatialRatingFieldGroup>) {
        visit(Property::SpatialPanning, "spatialPanning", payload.spatialPanning);
        visit(Property::SpatialIs3DEnabled, "spatialIs3DEnabled", payload.spatialIs3DEnabled);
        visit(Property::SpatialAttenuationRef, "spatialAttenuationRef", payload.spatialAttenuationRef);
        visit(Property::SpatialAttenuationDistanceScaling, "spatialAttenuationDistanceScaling", payload.spatialAttenuationDistanceScaling);
    } else if constexpr (std::is_same_v<Type, EffectFieldGroup>) {
        visit(Property::SignalGraph, "effects", payload.effects);
    } else if constexpr (std::is_same_v<Type, ConversionFieldGroup>) {
        visit(ConversionFieldId::Settings, "settingsRef", payload.settingsRef);
    }
}

template <typename Payload>
void CopyFieldGroupFields(Payload& target, const Payload& source, std::uint64_t fields)
{
    VisitFieldGroupFields(target, [&](auto property, const char*, auto& value) {
        const auto bit = FieldGroupFieldMask(property);
        if ((fields & bit) == 0) return;
        VisitFieldGroupFields(source, [&](auto sourceProperty, const char*, const auto& sourceValue) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, std::remove_cvref_t<decltype(sourceValue)>>)
                if (property == sourceProperty) value = sourceValue;
        });
    });
}

template <typename Payload>
std::uint64_t FieldGroupDifferenceMask(const Payload& left, const Payload& right)
{
    std::uint64_t fields = 0;
    VisitFieldGroupFields(left, [&](auto property, const char*, const auto& value) {
        VisitFieldGroupFields(right, [&](auto otherProperty, const char*, const auto& other) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, std::remove_cvref_t<decltype(other)>>)
                if (property == otherProperty && value != other)
                    fields |= FieldGroupFieldMask(property);
        });
    });
    return fields;
}

template <typename Payload>
GroupState<Payload> ResolveFieldGroupLocal(const AuthoredGroup<Payload>& authored,
                                            FieldGroupScope scope)
{
    auto result = authored.shared;
    if (!scope) return result;
    const auto found = authored.platforms.find(*scope);
    if (found == authored.platforms.end()) return result;
    const auto& overlay = found->second;
    if (overlay.mode != FieldGroupMode::UseShared) result.mode = overlay.mode;
    if (!overlay.localHistory) return result;
    if (!result.localHistory) {
        result.localHistory.emplace();
        result.localFields = 0;
    }
    CopyFieldGroupFields(*result.localHistory, *overlay.localHistory, overlay.localFields);
    result.localFields |= overlay.localFields;
    return result;
}

struct DefinitionFieldGroups {
    AuthoredGroup<OutputFieldGroup> output;
    AuthoredGroup<SendFieldGroup> send;
    AuthoredGroup<SpatialRatingFieldGroup> spatialRating;
    AuthoredGroup<EffectFieldGroup> effects;
    AuthoredGroup<ConversionFieldGroup> conversion;

    bool operator==(const DefinitionFieldGroups&) const = default;
};

template <typename Groups, typename Visitor>
void VisitAuthoredEffectChains(Groups& groups, Visitor&& visit)
{
    if (groups.effects.shared.localHistory)
        visit(groups.effects.shared.localHistory->effects, FieldGroupScope{});
    for (auto& [platform, state] : groups.effects.platforms)
        if (state.localHistory)
            visit(state.localHistory->effects, FieldGroupScope{platform});
}

inline std::uint64_t FieldGroupOverrideMask(const DefinitionFieldGroups& groups,
                                             harken::studio::project_schema::AuthoringGuid platform)
{
    const auto mask = [platform](const auto& group) {
        const auto state = group.platforms.find(platform);
        return state != group.platforms.end() && state->second.localHistory
            ? state->second.localFields : UINT64_C(0);
    };
    return mask(groups.output) | mask(groups.send) | mask(groups.spatialRating) | mask(groups.effects);
}

using FieldGroupPayload = std::variant<
    OutputFieldGroup, SendFieldGroup, SpatialRatingFieldGroup, EffectFieldGroup,
    ConversionFieldGroup>;

enum class FieldGroupDiagnosticCode : std::uint8_t {
    InvalidDefinitionGuid,
    DuplicateDefinitionGuid,
    InvalidContentKind,
    MissingGroupState,
    MissingParent,
    IllegalParentKind,
    ParentCycle,
    InvalidPlatform,
    InvalidMode,
    SharedUseShared,
    MissingLocalHistory,
    PayloadGroupMismatch,
    InvalidValue,
    EmptyRequiredReference,
    DuplicateSendTarget,
    InvalidMasterBus,
    UnknownDefinition,
    UnknownGroup,
    MissingReference,
    WrongReferenceKind,
    InvalidReferenceCatalog
};

struct FieldGroupDiagnostic {
    FieldGroupDiagnosticCode code = FieldGroupDiagnosticCode::InvalidValue;
    harken::studio::project_schema::AuthoringGuid definition{};
    std::optional<FieldGroupId> group;
    FieldGroupScope scope;
    std::string_view field;
    std::optional<std::size_t> row;
    harken::studio::project_schema::AuthoringGuid relatedGuid{};
};

struct FieldGroupDefinitionView {
    harken::studio::project_schema::AuthoringGuid guid{};
    harken::studio::project_schema::ContentKind kind = harken::studio::project_schema::ContentKind::Sound;
    harken::studio::project_schema::AuthoringGuid parentGuid{};
    const DefinitionFieldGroups* groups = nullptr;
};

struct FieldGroupSource {
    harken::studio::project_schema::AuthoringGuid definition{};
    FieldGroupScope scope;
    bool isDefault = true;
};

struct ResolvedFieldGroup {
    FieldGroupId group = FieldGroupId::Output;
    FieldGroupPayload payload;
    FieldGroupSource source;
    FieldGroupMode requestedMode = FieldGroupMode::Inherit;
    bool effectiveInherit = true;
};

struct FieldGroupResolutionResult {
    std::optional<ResolvedFieldGroup> value;
    std::vector<FieldGroupDiagnostic> diagnostics;

    bool ok() const noexcept { return value.has_value() && diagnostics.empty(); }
};

enum class FieldGroupReferenceKind : std::uint8_t {
    Bus,
    Attenuation,
    Effect,
    Conversion,
    Other
};

struct FieldGroupReferenceTarget {
    harken::studio::project_schema::AuthoringGuid guid{};
    FieldGroupReferenceKind kind = FieldGroupReferenceKind::Other;
};

class FieldGroupReferenceCatalog {
public:
    explicit FieldGroupReferenceCatalog(std::span<const FieldGroupReferenceTarget> targets);

    std::vector<FieldGroupDiagnostic> Validate(
        FieldGroupId group, const FieldGroupPayload& payload) const;

private:
    std::map<harken::studio::project_schema::AuthoringGuid, FieldGroupReferenceKind> targets_;
    std::vector<harken::studio::project_schema::AuthoringGuid> invalidTargets_;
};

bool IsFieldGroupScopeValid(FieldGroupScope scope) noexcept;
DefinitionFieldGroups MakeRootFieldGroups(
    harken::studio::project_schema::AuthoringGuid masterBusGuid);
std::string_view FieldGroupName(FieldGroupId group) noexcept;
std::optional<FieldGroupId> FieldGroupForField(std::string_view field) noexcept;
std::optional<FieldGroupId> FieldGroupForProperty(
    harkenplayableproperties::PlayablePropertyIdV1 property) noexcept;
std::uint64_t FieldGroupPropertyMask(FieldGroupId group) noexcept;
inline constexpr std::uint64_t kFieldGroupPropertyMask = UINT64_C(0x2de0)
    | FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::SignalGraph);

std::vector<FieldGroupDiagnostic> ValidateFieldGroupPayload(
    FieldGroupId group, const FieldGroupPayload& payload);
std::vector<FieldGroupDiagnostic> ValidateDefinitionFieldGroups(
    const DefinitionFieldGroups& groups, harken::studio::project_schema::AuthoringGuid definition = {});
std::vector<FieldGroupDiagnostic> ValidateActiveFieldGroup(
    FieldGroupId group, const FieldGroupPayload& payload,
    std::span<const FieldGroupReferenceTarget> targets);

class FieldGroupResolver {
public:
    FieldGroupResolver(std::span<const FieldGroupDefinitionView> definitions,
                         harken::studio::project_schema::AuthoringGuid masterBusGuid);

    bool ok() const noexcept { return diagnostics_.empty(); }
    const std::vector<FieldGroupDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    FieldGroupResolutionResult Resolve(harken::studio::project_schema::AuthoringGuid definition,
                                         FieldGroupScope scope,
                                         FieldGroupId group) const;

private:
    std::map<harken::studio::project_schema::AuthoringGuid, FieldGroupDefinitionView> definitions_;
    std::vector<FieldGroupDiagnostic> diagnostics_;
};

}
