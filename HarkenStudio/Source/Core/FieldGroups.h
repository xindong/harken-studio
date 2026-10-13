#pragma once

#include "harken_project_schema/Model.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace VNX {

using FieldGroupScope = std::optional<harkenprojectschema::AuthoringGuid>;

enum class FieldGroupId : std::uint8_t {
    MainOutput,
    Send,
    SpatialRating
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

struct MainOutputFieldGroup {
    static constexpr auto groupId = FieldGroupId::MainOutput;
    harkenprojectschema::AuthoringGuid mainOutputBusRef{};
    float outputBusVolume = 0.0f;

    bool operator==(const MainOutputFieldGroup&) const = default;
};

struct SendFieldGroupRow {
    harkenprojectschema::AuthoringGuid targetBusID{};
    float sendLevel = 0.0f;

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
    harkenprojectschema::AuthoringGuid spatialAttenuationRef{};
    float spatialAttenuationDistanceScaling = 1.0f;

    bool operator==(const SpatialRatingFieldGroup&) const = default;
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
    std::map<harkenprojectschema::AuthoringGuid, GroupState<Payload>> platforms;

    bool operator==(const AuthoredGroup&) const = default;
};

template <typename Payload, typename Visitor>
void VisitFieldGroupFields(Payload& payload, Visitor&& visit)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    using Type = std::remove_cv_t<Payload>;
    if constexpr (std::is_same_v<Type, MainOutputFieldGroup>) {
        visit(Property::MainOutputBusRef, "mainOutputBusRef", payload.mainOutputBusRef);
        visit(Property::OutputBusVolume, "outputBusVolume", payload.outputBusVolume);
    } else if constexpr (std::is_same_v<Type, SendFieldGroup>) {
        visit(Property::Sends, "sends", payload.sends);
    } else if constexpr (std::is_same_v<Type, SpatialRatingFieldGroup>) {
        visit(Property::SpatialPanning, "spatialPanning", payload.spatialPanning);
        visit(Property::SpatialIs3DEnabled, "spatialIs3DEnabled", payload.spatialIs3DEnabled);
        visit(Property::SpatialAttenuationRef, "spatialAttenuationRef", payload.spatialAttenuationRef);
        visit(Property::SpatialAttenuationDistanceScaling, "spatialAttenuationDistanceScaling", payload.spatialAttenuationDistanceScaling);
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
    AuthoredGroup<MainOutputFieldGroup> mainOutput;
    AuthoredGroup<SendFieldGroup> send;
    AuthoredGroup<SpatialRatingFieldGroup> spatialRating;

    bool operator==(const DefinitionFieldGroups&) const = default;
};

inline std::uint64_t FieldGroupOverrideMask(const DefinitionFieldGroups& groups,
                                             harkenprojectschema::AuthoringGuid platform)
{
    const auto mask = [platform](const auto& group) {
        const auto state = group.platforms.find(platform);
        return state != group.platforms.end() && state->second.localHistory
            ? state->second.localFields : UINT64_C(0);
    };
    return mask(groups.mainOutput) | mask(groups.send) | mask(groups.spatialRating);
}

using FieldGroupPayload = std::variant<
    MainOutputFieldGroup, SendFieldGroup, SpatialRatingFieldGroup>;

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
    harkenprojectschema::AuthoringGuid definition{};
    std::optional<FieldGroupId> group;
    FieldGroupScope scope;
    std::string_view field;
    std::optional<std::size_t> row;
    harkenprojectschema::AuthoringGuid relatedGuid{};
};

struct FieldGroupDefinitionView {
    harkenprojectschema::AuthoringGuid guid{};
    harkenprojectschema::ContentKind kind = harkenprojectschema::ContentKind::Sound;
    harkenprojectschema::AuthoringGuid parentGuid{};
    const DefinitionFieldGroups* groups = nullptr;
};

struct FieldGroupSource {
    harkenprojectschema::AuthoringGuid definition{};
    FieldGroupScope scope;
    bool isDefault = true;
};

struct ResolvedFieldGroup {
    FieldGroupId group = FieldGroupId::MainOutput;
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
    Other
};

struct FieldGroupReferenceTarget {
    harkenprojectschema::AuthoringGuid guid{};
    FieldGroupReferenceKind kind = FieldGroupReferenceKind::Other;
};

bool IsFieldGroupScopeValid(FieldGroupScope scope) noexcept;
DefinitionFieldGroups MakeRootFieldGroups(
    harkenprojectschema::AuthoringGuid masterBusGuid);
std::string_view FieldGroupName(FieldGroupId group) noexcept;
std::optional<FieldGroupId> FieldGroupForField(std::string_view field) noexcept;
std::optional<FieldGroupId> FieldGroupForProperty(
    harkenplayableproperties::PlayablePropertyIdV1 property) noexcept;
std::uint64_t FieldGroupPropertyMask(FieldGroupId group) noexcept;
inline constexpr std::uint64_t kFieldGroupPropertyMask = UINT64_C(0x2de0);

std::vector<FieldGroupDiagnostic> ValidateFieldGroupPayload(
    FieldGroupId group, const FieldGroupPayload& payload);
std::vector<FieldGroupDiagnostic> ValidateDefinitionFieldGroups(
    const DefinitionFieldGroups& groups, harkenprojectschema::AuthoringGuid definition = {});
std::vector<FieldGroupDiagnostic> ValidateActiveFieldGroup(
    FieldGroupId group, const FieldGroupPayload& payload,
    std::span<const FieldGroupReferenceTarget> targets);

class FieldGroupResolver {
public:
    FieldGroupResolver(std::span<const FieldGroupDefinitionView> definitions,
                         harkenprojectschema::AuthoringGuid masterBusGuid);

    bool ok() const noexcept { return diagnostics_.empty(); }
    const std::vector<FieldGroupDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    FieldGroupResolutionResult Resolve(harkenprojectschema::AuthoringGuid definition,
                                         FieldGroupScope scope,
                                         FieldGroupId group) const;

private:
    std::map<harkenprojectschema::AuthoringGuid, FieldGroupDefinitionView> definitions_;
    std::vector<FieldGroupDiagnostic> diagnostics_;
};

}
