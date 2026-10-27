#pragma once

#include "CanonicalRuntimeProjectAdapter.h"

#include "FieldGroups.h"
#include "ProjectPlatform.h"

#include <functional>
#include <map>
#include <vector>

namespace HarkenEditor::Bridge {
struct CanonicalRuntimeProjectSourceIndexV1;
}

namespace VNX {

struct ProjectItem;

FieldGroupDefinitionView fieldGroupDefinitionView(const ProjectItem& item);

harkenprojectschema::AuthoringGuid toFieldGroupGuid(const QUuid& guid);
QUuid fromFieldGroupGuid(harkenprojectschema::AuthoringGuid guid);
FieldGroupScope toFieldGroupScope(PlatformScope scope);

struct FieldGroupProjectView {
    std::vector<FieldGroupDefinitionView> definitions;
    std::vector<FieldGroupReferenceTarget> referenceTargets;
    std::vector<HarkenEditor::Bridge::ProjectInputDiagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

FieldGroupProjectView buildFieldGroupProjectView(const ProjectData& project);

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const std::function<const DefinitionFieldGroups*(
        const ProjectItem&)>& groupsForDefinition);

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1& source);

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1& source,
    const std::function<const DefinitionFieldGroups*(
        const ProjectItem&)>& groupsForDefinition);

struct EffectiveDefinitionFieldGroups {
    MainOutputFieldGroup mainOutput;
    SendFieldGroup send;
    SpatialRatingFieldGroup spatialRating;
    FieldGroupSource mainOutputSource;
    FieldGroupSource sendSource;
    FieldGroupSource spatialRatingSource;
};

struct FieldGroupEffectiveSnapshot {
    FieldGroupScope scope;
    std::map<harkenprojectschema::AuthoringGuid, EffectiveDefinitionFieldGroups> definitions;
    std::map<harkenprojectschema::AuthoringGuid, const ProjectItem*> objects;
    std::vector<HarkenEditor::Bridge::ProjectInputDiagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope);

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const std::function<const DefinitionFieldGroups*(const ProjectItem&)>& groupsForDefinition);

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1& source);

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1& source,
    const std::function<const DefinitionFieldGroups*(const ProjectItem&)>& groupsForDefinition);

}
