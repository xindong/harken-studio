#pragma once

#include "FieldGroups.h"

#include <QString>
#include <QUuid>

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace harken::studio {

struct ProjectData;
struct ProjectItem;

struct FieldGroupEditScopes {
    bool shared = false;
    std::vector<harken::studio::project_schema::AuthoringGuid> platforms;
};

struct SetFieldGroupMode {
    FieldGroupMode mode = FieldGroupMode::Inherit;
};

struct RebuildLocalFieldGroup {};

using FieldGroupFieldValue = std::variant<
    float, bool, harken::studio::project_schema::AuthoringGuid>;

struct SetFieldGroupField {
    harkenplayableproperties::PlayablePropertyIdV1 property =
        static_cast<harkenplayableproperties::PlayablePropertyIdV1>(0);
    FieldGroupFieldValue value;
};

struct ReplaceFieldGroupSends {
    SendFieldGroup value;
};

struct ReplaceFieldGroupSidechains {
    SidechainFieldGroup value;
};

struct ReplaceFieldGroupEffects {
    EffectFieldGroup value;
};

struct SetConversionFieldGroup {
    ConversionFieldGroup value;
};

struct ClearFieldGroupFieldOverride {
    harkenplayableproperties::PlayablePropertyIdV1 property;
};

struct ClearConversionFieldGroupOverride {};

using FieldGroupEditOperation = std::variant<
    SetFieldGroupMode, RebuildLocalFieldGroup,
    SetFieldGroupField, ReplaceFieldGroupSends, ReplaceFieldGroupSidechains, ReplaceFieldGroupEffects,
    SetConversionFieldGroup, ClearFieldGroupFieldOverride, ClearConversionFieldGroupOverride>;

struct FieldGroupEditRequest {
    QUuid definition;
    FieldGroupId group = FieldGroupId::Output;
    FieldGroupEditScopes scopes;
    FieldGroupEditOperation operation;
    std::optional<DefinitionFieldGroups> expectedBefore;
};

enum class FieldGroupEditDiagnosticCode {
    InvalidProject,
    InvalidMasterBus,
    UnknownDefinition,
    UnsupportedDefinition,
    InvalidScopes,
    InvalidOperation,
    ReadOnly,
    StaleState,
    InvalidGroups,
    InvalidReference,
    OutputSendConflict,
    MixedSendLists
};

struct FieldGroupEditDiagnostic {
    FieldGroupEditDiagnosticCode code = FieldGroupEditDiagnosticCode::InvalidProject;
    QString message;
    QUuid definition;
    std::optional<FieldGroupId> group;
    FieldGroupScope scope;
    QString field;
    std::optional<std::size_t> row;
    harken::studio::project_schema::AuthoringGuid relatedGuid{};
    std::optional<FieldGroupDiagnosticCode> groupCode;
};

struct FieldGroupEditResult {
    std::optional<DefinitionFieldGroups> before;
    std::optional<DefinitionFieldGroups> after;
    bool changed = false;
    std::vector<FieldGroupEditDiagnostic> diagnostics;

    bool ok() const noexcept { return after.has_value() && diagnostics.empty(); }
};

struct FieldGroupDefinitionSnapshot {
    QUuid definition;
    DefinitionFieldGroups groups;
};

struct FieldGroupParentSnapshot {
    QUuid definition;
    QUuid parent;
};

struct FieldGroupCopySnapshot {
    const ProjectItem* root;
    QUuid parent;
    const DefinitionFieldGroups* rootGroups = nullptr;
};

struct FieldGroupProjectValidation {
    std::vector<FieldGroupEditDiagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

FieldGroupEditResult prepareFieldGroupEdit(
    ProjectData& project, const FieldGroupEditRequest& request);

std::optional<DefinitionFieldGroups> previewFieldGroupNumber(
    const ProjectData& project, const ProjectItem& item, const FieldGroupEditScopes& scopes,
    harkenplayableproperties::PlayablePropertyIdV1 property, float value,
    std::span<const FieldGroupDefinitionView> ancestry);

FieldGroupEditResult prepareFieldGroupRestore(
    ProjectData& project, const QUuid& definition,
    const DefinitionFieldGroups& replacement,
    const std::optional<DefinitionFieldGroups>& expectedBefore = {});

FieldGroupProjectValidation validateFieldGroupProject(
    ProjectData& project,
    std::span<const FieldGroupDefinitionSnapshot> replacements = {},
    std::span<const FieldGroupParentSnapshot> parents = {},
    std::span<const FieldGroupCopySnapshot> copies = {});

}
