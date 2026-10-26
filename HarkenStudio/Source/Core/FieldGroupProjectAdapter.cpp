#include "FieldGroupProjectAdapter.h"

#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "ProjectTypes.h"

#include <map>
#include <QtEndian>

namespace VNX {
namespace {

harkenprojectschema::ContentKind definitionKind(ProjectItem::Type type)
{
    using Kind = harkenprojectschema::ContentKind;
    switch (type) {
    case ProjectItem::Type::Layer: return Kind::Layer;
    case ProjectItem::Type::Sound: return Kind::Sound;
    case ProjectItem::Type::Container: return Kind::Container;
    case ProjectItem::Type::Track: return Kind::Track;
    case ProjectItem::Type::Piece: return Kind::Piece;
    default: return static_cast<Kind>(UINT32_MAX);
    }
}

}

FieldGroupDefinitionView fieldGroupDefinitionView(const ProjectItem& item)
{
    return {toFieldGroupGuid(item.guid), definitionKind(item.type),
            item.parent ? toFieldGroupGuid(item.parent->guid) : harkenprojectschema::AuthoringGuid{},
            &item.fieldGroups};
}

harkenprojectschema::AuthoringGuid toFieldGroupGuid(const QUuid& guid)
{
    const auto bytes = guid.toRfc4122();
    return {qFromBigEndian<quint64>(bytes.constData()),
            qFromBigEndian<quint64>(bytes.constData() + 8)};
}

QUuid fromFieldGroupGuid(harkenprojectschema::AuthoringGuid guid)
{
    QByteArray bytes(16, Qt::Uninitialized);
    qToBigEndian<quint64>(guid.high, bytes.data());
    qToBigEndian<quint64>(guid.low, bytes.data() + 8);
    return QUuid::fromRfc4122(bytes);
}

FieldGroupScope toFieldGroupScope(PlatformScope scope)
{
    return scope ? FieldGroupScope{toFieldGroupGuid(*scope)} : std::nullopt;
}

FieldGroupProjectView buildFieldGroupProjectView(const ProjectData& project)
{
    return buildFieldGroupProjectView(project, [](const ProjectItem& item) {
        return &item.fieldGroups;
    });
}

namespace {

FieldGroupProjectView buildProjectView(
    const ProjectData& project,
    const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1& source,
    const std::function<const DefinitionFieldGroups*(
        const ProjectItem&)>& groupsForDefinition)
{
    using namespace harkenprojectschema;
    using namespace HarkenEditor::Bridge;
    FieldGroupProjectView result;
    result.diagnostics = source.structureDiagnostics;
    const auto fail = [&](ProjectInputDiagnosticCode code, const QString& message,
                          const ProjectItem* item, const QString& field) {
        result.diagnostics.push_back(
            {code, message, item ? item->guid : QUuid{}, {}, field});
    };
    if (!source.valid || !source.duplicateGuids.empty()) {
        if (result.diagnostics.empty())
            fail(ProjectInputDiagnosticCode::DuplicateIdentity,
                 QStringLiteral("Project source identity is ambiguous"),
                 nullptr, QStringLiteral("guid"));
        return result;
    }
    std::map<const ProjectItem*, AuthoringGuid> identities;
    for (const auto& [guid, item] : source.byGuid)
        identities.emplace(item, guid);
    for (const auto& [guid, item] : source.byGuid) {
        auto kind = FieldGroupReferenceKind::Other;
        if (item->type == ProjectItem::Type::Bus)
            kind = FieldGroupReferenceKind::Bus;
        else if (item->type == ProjectItem::Type::Attenuation)
            kind = FieldGroupReferenceKind::Attenuation;
        result.referenceTargets.push_back({guid, kind});
    }
    for (ProjectItem* item : source.definitions) {
        const auto identity = identities.find(item);
        if (identity == identities.end()) {
            fail(ProjectInputDiagnosticCode::MissingIdentity,
                 QStringLiteral("Field group definition has no authoring GUID"),
                 item, QStringLiteral("guid"));
            continue;
        }
        const ProjectItem* parent = source.parentDefinition.at(item);
        AuthoringGuid parentGuid{};
        if (parent) {
            const auto parentIdentity = identities.find(parent);
            if (parentIdentity == identities.end()) {
                fail(ProjectInputDiagnosticCode::MissingIdentity,
                     QStringLiteral("Field group parent has no authoring GUID"),
                     item, QStringLiteral("parent"));
                continue;
            }
            parentGuid = parentIdentity->second;
        }
        if (item->parentRef.hasGuid()
            && (item->parentRef.kind != ParentRef::Kind::Item
                || !parent || item->parentRef.guid != parent->guid)) {
            fail(ProjectInputDiagnosticCode::MissingReference,
                 QStringLiteral("Field group parent reference is not attached to its ownership parent"),
                 item, QStringLiteral("parentRef"));
            continue;
        }
        const auto* groups = groupsForDefinition ? groupsForDefinition(*item) : nullptr;
        if (!groups) {
            fail(ProjectInputDiagnosticCode::MissingReference,
                 QStringLiteral("Field group definition state is missing"),
                 item, QStringLiteral("fieldGroups"));
            continue;
        }
        const auto validatePlatforms = [&](const auto& authored) {
            for (const auto& [platform, state] : authored.platforms) {
                if (!ProjectPlatformSemantics::find(project.platforms, fromFieldGroupGuid(platform)))
                    fail(ProjectInputDiagnosticCode::MissingReference,
                         QStringLiteral("Field group platform ID does not belong to the project"),
                         item, QStringLiteral("fieldGroups.platforms"));
            }
        };
        validatePlatforms(groups->mainOutput);
        validatePlatforms(groups->send);
        validatePlatforms(groups->spatialRating);
        auto definition = fieldGroupDefinitionView(*item);
        definition.guid = identity->second;
        definition.parentGuid = parentGuid;
        definition.groups = groups;
        result.definitions.push_back(definition);
    }
    if (!result.ok()) {
        result.definitions.clear();
        result.referenceTargets.clear();
    }
    return result;
}

}

FieldGroupProjectView buildFieldGroupProjectView(
    const ProjectData& project,
    const std::function<const DefinitionFieldGroups*(
        const ProjectItem&)>& groupsForDefinition)
{
    const auto source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    return buildProjectView(project, *source, groupsForDefinition);
}

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope)
{
    return buildFieldGroupEffectiveSnapshot(project, scope, [](const ProjectItem& item) {
        return &item.fieldGroups;
    });
}

FieldGroupEffectiveSnapshot buildFieldGroupEffectiveSnapshot(
    const ProjectData& project, FieldGroupScope scope,
    const std::function<const DefinitionFieldGroups*(const ProjectItem&)>& groupsForDefinition)
{
    using namespace harkenprojectschema;
    using namespace HarkenEditor::Bridge;
    FieldGroupEffectiveSnapshot result;
    result.scope = scope;
    const auto source = canonicalRuntimeProjectSourceIndex(project);
    const auto view = buildProjectView(project, *source, groupsForDefinition);
    result.diagnostics = view.diagnostics;
    if (!view.ok()) return result;
    const auto fail = [&](const QString& message, AuthoringGuid definition = {},
                          const QString& field = QStringLiteral("fieldGroups")) {
        result.diagnostics.push_back({ProjectInputDiagnosticCode::FrontEndRejected,
            message, fromFieldGroupGuid(definition), {}, field});
    };
    if (!IsFieldGroupScopeValid(scope)
        || (scope && !ProjectPlatformSemantics::find(project.platforms, fromFieldGroupGuid(*scope)))) {
        fail(QStringLiteral("Field group platform is invalid"));
        return result;
    }
    const auto* master = project.busses.empty() ? nullptr : project.busses.front().rootItem.get();
    if (!master || master->type != ProjectItem::Type::Bus || master->guid.isNull()
        || !source->byGuid.contains(toFieldGroupGuid(master->guid))) {
        fail(QStringLiteral("Project Master Bus must have a valid persistent GUID"));
        return result;
    }
    const auto append = [&](const std::vector<FieldGroupDiagnostic>& diagnostics,
                            AuthoringGuid definition) {
        for (const auto& diagnostic : diagnostics) {
            fail(QStringLiteral("Field group resolution failed (%1)")
                     .arg(static_cast<unsigned>(diagnostic.code)),
                 IsNull(diagnostic.definition) ? definition : diagnostic.definition,
                 QString::fromUtf8(diagnostic.field.data(),
                                   static_cast<qsizetype>(diagnostic.field.size())));
        }
    };
    const FieldGroupResolver resolver(view.definitions, toFieldGroupGuid(master->guid));
    append(resolver.diagnostics(), {});
    if (!result.ok()) return result;
    const FieldGroupReferenceCatalog references(view.referenceTargets);
    for (const auto& definition : view.definitions) {
        const auto output = resolver.Resolve(definition.guid, scope, FieldGroupId::MainOutput);
        const auto send = resolver.Resolve(definition.guid, scope, FieldGroupId::Send);
        const auto spatial = resolver.Resolve(definition.guid, scope, FieldGroupId::SpatialRating);
        for (const auto* resolved : {&output, &send, &spatial}) {
            append(resolved->diagnostics, definition.guid);
            if (resolved->value)
                append(references.Validate(resolved->value->group,
                    resolved->value->payload), definition.guid);
        }
        if (!output.ok() || !send.ok() || !spatial.ok()) continue;
        EffectiveDefinitionFieldGroups groups{
            std::get<MainOutputFieldGroup>(output.value->payload),
            std::get<SendFieldGroup>(send.value->payload),
            std::get<SpatialRatingFieldGroup>(spatial.value->payload),
            output.value->source, send.value->source, spatial.value->source};
        for (const auto& row : groups.send.sends) {
            if (row.targetBusID == groups.mainOutput.mainOutputBusRef)
                fail(QStringLiteral("A Send target cannot also be the effective Main Output"),
                     definition.guid, QStringLiteral("targetBusID"));
        }
        result.definitions.emplace(definition.guid, std::move(groups));
    }
    if (!result.ok()) {
        result.definitions.clear();
        return result;
    }
    result.objects.insert(source->byGuid.begin(), source->byGuid.end());
    return result;
}

}
