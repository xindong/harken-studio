#include "FieldGroupInspectorView.h"

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectTypes.h"
#include "Core/FieldGroupJson.h"
#include "Core/FieldGroupProjectAdapter.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace VNX {
namespace {

using Property = harkenplayableproperties::PlayablePropertyIdV1;

QVariant payloadField(const FieldGroupPayload& payload, Property field)
{
    switch (field) {
    case Property::MainOutputBusRef:
        return fromFieldGroupGuid(std::get<MainOutputFieldGroup>(payload).mainOutputBusRef);
    case Property::OutputBusVolume:
        return std::get<MainOutputFieldGroup>(payload).outputBusVolume;
    case Property::Sends: {
        QVariantList rows;
        for (const auto& row : std::get<SendFieldGroup>(payload).sends)
            rows.append(QVariantMap{
                {QStringLiteral("targetBusID"), fromFieldGroupGuid(row.targetBusID)},
                {QStringLiteral("sendLevel"), row.sendLevel}});
        return rows;
    }
    case Property::SpatialPanning:
        return std::get<SpatialRatingFieldGroup>(payload).spatialPanning;
    case Property::SpatialIs3DEnabled:
        return std::get<SpatialRatingFieldGroup>(payload).spatialIs3DEnabled;
    case Property::SpatialAttenuationRef:
        return fromFieldGroupGuid(std::get<SpatialRatingFieldGroup>(payload).spatialAttenuationRef);
    case Property::SpatialAttenuationDistanceScaling:
        return std::get<SpatialRatingFieldGroup>(payload).spatialAttenuationDistanceScaling;
    default:
        return {};
    }
}

QString scopeName(const ProjectData& project, FieldGroupScope scope)
{
    if (!scope) return QStringLiteral("All Platforms");
    const auto* platform = ProjectPlatformSemantics::find(project.platforms, fromFieldGroupGuid(*scope));
    return platform ? platform->name : ProjectPlatformSemantics::idString(fromFieldGroupGuid(*scope));
}

bool resolveEditScopes(const ProjectData& project, const PlatformContext& context,
                       FieldGroupEditScopes& scopes)
{
    if (context.all || context.selected.isEmpty()) {
        scopes.shared = true;
        return true;
    }
    if (context.selected.size() != 1 || context.selected.front() != context.primary
        || context.primary.isNull()
        || !selectedIsSubsetOfTargetPlatforms(context.selected, ProjectPlatformSemantics::ids(project.platforms)))
        return false;
    scopes.platforms.push_back(toFieldGroupGuid(context.primary));
    return true;
}

}

FieldGroupInspectorFieldView FieldGroupInspectorView::field(const QString& fieldName) const
{
    FieldGroupInspectorFieldView result;
    if (!valid || perScope.empty()) return result;
    const auto* descriptor = PlayablePropertyAuthoringV1::fieldByName(fieldName);
    if (!descriptor) return result;
    const auto property = static_cast<Property>(descriptor->id);
    if (FieldGroupForProperty(property) != group) return result;
    result.uniform = true;
    result.perScope.reserve(static_cast<qsizetype>(perScope.size()));
    for (const auto& target : perScope) {
        const auto value = payloadField(target.resolved.payload, property);
        if (result.perScope.isEmpty())
            result.value = value;
        else if (result.value != value)
            result.uniform = false;
        result.perScope.append({target.scope, value});
    }
    return result;
}

QJsonObject FieldGroupInspectorView::actionArgs() const
{
    if (!valid) return {};
    std::string json;
    std::string error;
    if (!WriteFieldGroupsJson(expectedBefore, json, error)) return {};
    QJsonObject result{
        {QStringLiteral("definition_guid"), definition.toString(QUuid::WithoutBraces)},
        {QStringLiteral("group"), QString::fromLatin1(FieldGroupName(group).data())},
        {QStringLiteral("expected_before"), QJsonDocument::fromJson(QByteArray::fromStdString(json)).object()}};
    if (editScopes.shared) {
        result.insert(QStringLiteral("scope"), QStringLiteral("Shared"));
    } else {
        QJsonArray platforms;
        for (const auto platform : editScopes.platforms)
            platforms.append(ProjectPlatformSemantics::idString(fromFieldGroupGuid(platform)));
        result.insert(QStringLiteral("platform_ids"), platforms);
    }
    return result;
}

QString FieldGroupInspectorView::sourceTooltip(const ProjectData& project, const QString& fieldName) const
{
    if (!valid) return diagnostics.join(QLatin1Char('\n'));
    const auto index = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    QStringList lines;
    for (const auto& target : perScope) {
        const auto& source = target.resolved.source;
        QString provider;
        if (source.isDefault) {
            provider = QStringLiteral("Default");
        } else {
            const auto item = index->byGuid.find(source.definition);
            const auto name = item == index->byGuid.end()
                ? fromFieldGroupGuid(source.definition).toString(QUuid::WithoutBraces)
                : item->second->name;
            auto scope = source.scope;
            if (scope && item != index->byGuid.end()) {
                if (const auto* field = PlayablePropertyAuthoringV1::fieldByName(fieldName))
                    if ((FieldGroupOverrideMask(item->second->fieldGroups, *scope)
                        & (FieldGroupFieldMask(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field->id)))) == 0) scope.reset();
            }
            provider = QStringLiteral("%1 / %2").arg(name, scopeName(project, scope));
        }
        lines.append(QStringLiteral("%1: %2").arg(scopeName(project, target.scope), provider));
    }
    return lines.join(QLatin1Char('\n'));
}

FieldGroupInspectorView buildFieldGroupInspectorView(
    const ProjectData& project, const QUuid& definition,
    FieldGroupId group, const PlatformContext& context)
{
    FieldGroupInspectorView result;
    result.definition = definition;
    result.group = group;
    result.context = context;
    if (definition.isNull() || FieldGroupName(group).empty()) {
        result.diagnostics.append(QStringLiteral("Select a valid definition and field group"));
        return result;
    }
    if (!resolveEditScopes(project, context, result.editScopes)) {
        result.diagnostics.append(QStringLiteral("Select a valid project target platform"));
        return result;
    }
    const auto view = buildFieldGroupProjectView(project);
    for (const auto& diagnostic : view.diagnostics)
        result.diagnostics.append(diagnostic.message);
    if (!view.ok()) return result;
    const auto guid = toFieldGroupGuid(definition);
    const auto authored = std::find_if(view.definitions.begin(), view.definitions.end(),
        [&](const FieldGroupDefinitionView& candidate) { return candidate.guid == guid; });
    if (authored == view.definitions.end()) {
        result.diagnostics.append(QStringLiteral("This object is not a field group definition"));
        return result;
    }
    result.expectedBefore = *authored->groups;
    result.canInherit = !harkenprojectschema::IsNull(authored->parentGuid);
    const auto* master = project.busses.empty() ? nullptr : project.busses.front().rootItem.get();
    if (!master || master->type != ProjectItem::Type::Bus || master->guid.isNull()) {
        result.diagnostics.append(QStringLiteral("Project Master Bus must have a valid persistent GUID"));
        return result;
    }
    const FieldGroupResolver resolver(view.definitions, toFieldGroupGuid(master->guid));
    std::vector<FieldGroupScope> scopes;
    if (result.editScopes.shared) {
        for (const auto& platform : project.platforms)
            scopes.push_back(toFieldGroupGuid(platform.id));
    }
    else
        for (const auto platform : result.editScopes.platforms)
            scopes.push_back(platform);
    for (const auto scope : scopes) {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(project, scope);
        for (const auto& diagnostic : snapshot.diagnostics)
            result.diagnostics.append(QStringLiteral("%1: %2").arg(scopeName(project, scope), diagnostic.message));
        if (!snapshot.ok()) {
            result.perScope.clear();
            return result;
        }
        const auto resolved = resolver.Resolve(guid, scope, group);
        if (!resolved.ok()) {
            result.diagnostics.append(QStringLiteral("The field group cannot be resolved"));
            result.perScope.clear();
            return result;
        }
        result.perScope.push_back({scope, *resolved.value});
    }
    if (result.perScope.empty()) {
        result.diagnostics.append(QStringLiteral("The project has no target platforms"));
        return result;
    }
    const auto& first = result.perScope.front().resolved;
    result.effectiveInherit = first.effectiveInherit;
    result.fieldsEditable = !result.canInherit || !result.effectiveInherit;
    result.allExplicitLocal = true;
    result.allUseShared = true;
    result.sendsUniform = true;
    for (const auto& target : result.perScope) {
        const auto& resolved = target.resolved;
        result.inheritanceMixed |= resolved.effectiveInherit != first.effectiveInherit;
        result.fieldsEditable &= !result.canInherit || !resolved.effectiveInherit;
        result.modesMixed |= resolved.requestedMode != first.requestedMode;
        result.allExplicitLocal &= resolved.requestedMode == FieldGroupMode::Local;
        result.anyUseShared |= resolved.requestedMode == FieldGroupMode::UseShared;
        result.allUseShared &= resolved.requestedMode == FieldGroupMode::UseShared;
        if (group == FieldGroupId::Send)
            result.sendsUniform &= resolved.payload == first.payload;
    }
    result.valid = true;
    return result;
}

}
