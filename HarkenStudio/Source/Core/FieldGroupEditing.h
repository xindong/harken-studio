#pragma once

#include "FieldGroups.h"

#include <QString>
#include <QUuid>

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace VNX {

struct ProjectData;
struct ProjectItem;

struct FieldGroupEditScopes {
    bool shared = false;
    std::vector<harkenprojectschema::AuthoringGuid> platforms;
};

struct SetFieldGroupMode {
    FieldGroupMode mode = FieldGroupMode::Inherit;
};

struct RebuildLocalFieldGroup {};

using FieldGroupFieldValue = std::variant<
    float, bool, harkenprojectschema::AuthoringGuid>;

struct SetFieldGroupField {
    harkenplayableproperties::PlayablePropertyIdV1 property =
        static_cast<harkenplayableproperties::PlayablePropertyIdV1>(0);
    FieldGroupFieldValue value;
};

struct ReplaceFieldGroupSends {
    SendFieldGroup value;
};

struct ClearFieldGroupFieldOverride {
    harkenplayableproperties::PlayablePropertyIdV1 property;
};

using FieldGroupEditOperation = std::variant<
    SetFieldGroupMode, RebuildLocalFieldGroup,
    SetFieldGroupField, ReplaceFieldGroupSends, ClearFieldGroupFieldOverride>;

struct FieldGroupEditRequest {
    QUuid definition;
    FieldGroupId group = FieldGroupId::MainOutput;
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
    MainOutputSendConflict,
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
    harkenprojectschema::AuthoringGuid relatedGuid{};
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
