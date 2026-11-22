#pragma once

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"

#include "FieldGroups.h"
#include "ProjectPlatform.h"

#include <functional>
#include <map>
#include <vector>

namespace harken::studio::bridge {
struct CanonicalRuntimeProjectSourceIndexV1;
}

namespace harken::studio {

struct ProjectItem;

FieldGroupDefinitionView fieldGroupDefinitionView(const ProjectItem& item);

harken::studio::project_schema::AuthoringGuid toFieldGroupGuid(const QUuid& guid);
QUuid fromFieldGroupGuid(harken::studio::project_schema::AuthoringGuid guid);
FieldGroupScope toFieldGroupScope(PlatformScope scope);

struct FieldGroupProjectView {
    std::vector<FieldGroupDefinitionView> definitions;
    std::vector<FieldGroupReferenceTarget> referenceTargets;
    std::vector<harken::studio::bridge::ProjectInputDiagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

FieldGroupProjectView buildFieldGroupProjectView(const ProjectData& project);

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const std::function<const DefinitionFieldGroups*(
        const ProjectItem&)>& groupsForDefinition);

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const harken::studio::bridge::CanonicalRuntimeProjectSourceIndexV1& source);

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const harken::studio::bridge::CanonicalRuntimeProjectSourceIndexV1& source,
    const std::function<const DefinitionFieldGroups*(
        const ProjectItem&)>& groupsForDefinition);

struct EffectiveDefinitionFieldGroups {
    OutputFieldGroup output;
    SendFieldGroup send;
    SpatialRatingFieldGroup spatialRating;
    FieldGroupSource outputSource;
    FieldGroupSource sendSource;
    FieldGroupSource spatialRatingSource;
    EffectFieldGroup effects;
    FieldGroupSource effectsSource;
};

struct FieldGroupEffectiveSnapshot {
    FieldGroupScope scope;
    std::map<harken::studio::project_schema::AuthoringGuid, EffectiveDefinitionFieldGroups> definitions;
    std::map<harken::studio::project_schema::AuthoringGuid, const ProjectItem*> objects;
    std::vector<harken::studio::bridge::ProjectInputDiagnostic> diagnostics;

    bool ok() const noexcept { return diagnostics.empty(); }
};

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope);

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const std::function<const DefinitionFieldGroups*(const ProjectItem&)>& groupsForDefinition);

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const harken::studio::bridge::CanonicalRuntimeProjectSourceIndexV1& source);

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const harken::studio::bridge::CanonicalRuntimeProjectSourceIndexV1& source,
    const std::function<const DefinitionFieldGroups*(const ProjectItem&)>& groupsForDefinition);

}
