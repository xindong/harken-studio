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

QVariant payloadField(const FieldGroupPayload& payload, const QString& field)
{
    QVariant result;
    std::visit([&](const auto& group) {
        VisitFieldGroupFields(group, [&](auto, const char* name, const auto& value) {
            if (field != QLatin1String(name)) return;
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, harken::studio::project_schema::AuthoringGuid>)
                result = fromFieldGroupGuid(value);
            else if constexpr (std::is_same_v<Value, EffectChainLedger>)
                result = value.toJson().toVariantList();
            else if constexpr (std::is_same_v<Value, std::vector<SendFieldGroupRow>>
                               || std::is_same_v<Value, std::vector<SidechainFieldGroupRow>>)
                result = fieldGroupRouteRowsToJson(value).toVariantList();
            else
                result = value;
        });
    }, payload);
    return result;
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

const FieldGroupInspectorScopeView* FieldGroupInspectorView::displayed() const
{
    const auto current = std::find_if(perScope.begin(), perScope.end(), [this](const auto& target) {
        return target.scope && fromFieldGroupGuid(*target.scope) == context.primary;
    });
    if (current != perScope.end()) return &*current;
    return perScope.empty() ? nullptr : &perScope.front();
}

FieldGroupInspectorFieldView FieldGroupInspectorView::field(const QString& fieldName) const
{
    FieldGroupInspectorFieldView result;
    if (!valid || perScope.empty() || FieldGroupForField(fieldName.toStdString()) != group) return result;
    result.valid = true;
    result.perScope.reserve(static_cast<qsizetype>(perScope.size()));
    for (const auto& target : perScope) {
        const auto value = payloadField(target.resolved.payload, fieldName);
        if (&target == displayed()) result.value = value;
        result.perScope.append({target.scope, value});
    }
    if (shared) result.shared = payloadField(shared->payload, fieldName);
    return result;
}

QJsonObject FieldGroupInspectorView::actionArgs(bool sharedScope) const
{
    if (!valid) return {};
    std::string json;
    std::string error;
    if (!WriteFieldGroupsJson(expectedBefore, json, error)) return {};
    QJsonObject result{
        {QStringLiteral("definition_guid"), definition.toString(QUuid::WithoutBraces)},
        {QStringLiteral("group"), QString::fromLatin1(FieldGroupName(group).data())},
        {QStringLiteral("expected_before"), QJsonDocument::fromJson(QByteArray::fromStdString(json)).object()}};
    if (sharedScope || editScopes.shared) {
        result.insert(QStringLiteral("scope"), QStringLiteral("Shared"));
    } else {
        QJsonArray platforms;
        for (const auto platform : editScopes.platforms)
            platforms.append(ProjectPlatformSemantics::idString(fromFieldGroupGuid(platform)));
        result.insert(QStringLiteral("platform_ids"), platforms);
    }
    return result;
}

std::optional<EffectChainLedger> FieldGroupInspectorView::effectChain(bool shared) const
{
    if (!valid || group != FieldGroupId::Effect) return {};
    const auto value = field(QStringLiteral("effects"));
    return EffectChainLedger::fromJson(QJsonArray::fromVariantList((shared ? value.shared : value.value).toList()));
}

QString FieldGroupInspectorView::sourceTooltip(const ProjectData& project, const QString& fieldName) const
{
    if (!valid) return diagnostics.join(QLatin1Char('\n'));
    const auto index = harken::studio::bridge::canonicalRuntimeProjectSourceIndex(project);
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
    result.canInherit = !harken::studio::project_schema::IsNull(authored->parentGuid);
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
    if (IsFieldGroupRowList(group)) {
        const auto resolved = resolver.Resolve(guid, std::nullopt, group);
        if (!resolved.ok()) {
            result.diagnostics.append(QStringLiteral("The field group cannot be resolved"));
            result.perScope.clear();
            return result;
        }
        result.shared = *resolved.value;
    }
    result.effectiveInherit = result.displayed()->resolved.effectiveInherit;
    result.fieldsEditable = !result.canInherit || !result.effectiveInherit;
    result.valid = true;
    return result;
}

}
