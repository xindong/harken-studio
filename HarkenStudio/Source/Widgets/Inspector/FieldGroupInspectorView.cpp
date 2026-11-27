#include "FieldGroupInspectorView.h"

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectTypes.h"
#include "Core/FieldGroupJson.h"
#include "Core/FieldGroupProjectAdapter.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace harken::studio {
namespace {

using Property = harkenplayableproperties::PlayablePropertyIdV1;

QVariant payloadField(const FieldGroupPayload& payload, Property field)
{
    switch (field) {
    case Property::OutputBusRef:
        return fromFieldGroupGuid(std::get<OutputFieldGroup>(payload).outputBusRef);
    case Property::OutputBusVolume:
        return std::get<OutputFieldGroup>(payload).outputBusVolume;
    case Property::Sends: {
        QVariantList rows;
        for (const auto& row : std::get<SendFieldGroup>(payload).sends) {
            QVariantMap entry{
                {QStringLiteral("targetBusID"), fromFieldGroupGuid(row.targetBusID)},
                {QStringLiteral("sendLevel"), row.sendLevel},
                {QStringLiteral("type"), sendTypeName(row.type)}, {QStringLiteral("duckTarget"), row.duckTarget}};
            rows.append(entry);
        }
        return rows;
    }
    case Property::SignalGraph:
        return std::get<EffectFieldGroup>(payload).effects.toJson().toVariantList();
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
    if (group == FieldGroupId::Conversion) {
        if (fieldName != QLatin1String("settingsRef")) return result;
        result.uniform = true;
        for (const auto& target : perScope) {
            const QVariant value = fromFieldGroupGuid(
                std::get<ConversionFieldGroup>(target.resolved.payload).settingsRef);
            if (result.perScope.isEmpty())
                result.value = value;
            else if (result.value != value)
                result.uniform = false;
            result.perScope.append({target.scope, value});
        }
        return result;
    }
    const auto* descriptor = PlayablePropertyAuthoringV1::fieldByName(fieldName);
    if (!descriptor && fieldName != QLatin1String("effects")) return result;
    const auto property = fieldName == QLatin1String("effects")
        ? Property::SignalGraph : static_cast<Property>(descriptor->id);
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

std::optional<EffectChainLedger> FieldGroupInspectorView::effectChain() const
{
    if (!valid || group != FieldGroupId::Effect) return {};
    if (editScopes.shared && fieldsEditable)
        return expectedBefore.effects.shared.localHistory
            ? expectedBefore.effects.shared.localHistory->effects : EffectChainLedger{};
    const auto value = field(QStringLiteral("effects"));
    if (!value.uniform) return {};
    return EffectChainLedger::fromJson(QJsonArray::fromVariantList(value.value.toList()));
}

static QString formatSourceTooltip(const ProjectData& project, const FieldGroupInspectorView& view,
                                   const QString& fieldName,
                                   const bridge::CanonicalRuntimeProjectSourceIndexV1& index)
{
    if (!view.valid) return view.diagnostics.join(QLatin1Char('\n'));
    const auto* field = PlayablePropertyAuthoringV1::fieldByName(fieldName);
    QStringList lines;
    for (const auto& target : view.perScope) {
        const auto& source = target.resolved.source;
        QString provider;
        if (source.isDefault) {
            provider = QStringLiteral("Default");
        } else {
            const auto item = index.byGuid.find(source.definition);
            const auto name = item == index.byGuid.end()
                ? fromFieldGroupGuid(source.definition).toString(QUuid::WithoutBraces) : item->second->name;
            auto scope = source.scope;
            if (scope && item != index.byGuid.end() && field
                && !(FieldGroupOverrideMask(item->second->fieldGroups, *scope) & FieldGroupFieldMask(
                    static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field->id))))
                scope.reset();
            provider = QStringLiteral("%1 / %2").arg(name, scopeName(project, scope));
        }
        lines.append(QStringLiteral("%1: %2").arg(scopeName(project, target.scope), provider));
    }
    return lines.join(QLatin1Char('\n'));
}

QString FieldGroupInspectorView::sourceTooltip(const ProjectData& project, const QString& fieldName) const
{
    if (!valid) return diagnostics.join(QLatin1Char('\n'));
    return formatSourceTooltip(project, *this, fieldName, *bridge::canonicalRuntimeProjectSourceIndex(project));
}

QString FieldGroupInspectorRead::sourceTooltip(const FieldGroupInspectorView& view, const QString& fieldName) const
{
    return formatSourceTooltip(project_, view, fieldName, *source_);
}

FieldGroupInspectorView buildFieldGroupInspectorView(
    const ProjectData& project, const QUuid& definition,
    FieldGroupId group, const PlatformContext& context)
{
    return FieldGroupInspectorRead(project).read(definition, group, context);
}

FieldGroupInspectorRead::FieldGroupInspectorRead(const ProjectData& project)
    : project_(project)
    , source_(bridge::canonicalRuntimeProjectSourceIndex(project))
    , view_(buildFieldGroupProjectView(project, *source_))
    , resolver_(view_.definitions, project.busses.empty() || !project.busses.front().rootItem
        ? project_schema::AuthoringGuid{} : toFieldGroupGuid(project.busses.front().rootItem->guid))
{}

FieldGroupInspectorView FieldGroupInspectorRead::read(
    const QUuid& definition, FieldGroupId group, const PlatformContext& context)
{
    const auto& project = project_;
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
    const auto& view = view_;
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
    result.canInherit = !harken::studio::project_schema::IsNull(authored->parentGuid);
    for (auto* item = source_->byQtGuid.at(definition); item; item = source_->parentDefinition.at(item))
        result.ancestry.append(item->guid);
    const auto* master = project.busses.empty() ? nullptr : project.busses.front().rootItem.get();
    if (!master || master->type != ProjectItem::Type::Bus || master->guid.isNull()) {
        result.diagnostics.append(QStringLiteral("Project Master Bus must have a valid persistent GUID"));
        return result;
    }
    std::vector<FieldGroupScope> scopes;
    if (result.editScopes.shared) {
        for (const auto& platform : project.platforms)
            scopes.push_back(toFieldGroupGuid(platform.id));
    }
    else
        for (const auto platform : result.editScopes.platforms)
            scopes.push_back(platform);
    for (const auto scope : scopes) {
        const auto [diagnostics, inserted] = scopeDiagnostics_.try_emplace(scope);
        if (inserted) {
            const auto snapshot = buildFieldGroupEffectiveSnapshot(project, scope, *source_);
            for (const auto& diagnostic : snapshot.diagnostics)
                diagnostics->second.append(QStringLiteral("%1: %2").arg(scopeName(project, scope), diagnostic.message));
        }
        if (!diagnostics->second.empty()) {
            result.diagnostics.append(diagnostics->second);
            result.perScope.clear();
            return result;
        }
        const auto resolved = resolver_.Resolve(guid, scope, group);
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
