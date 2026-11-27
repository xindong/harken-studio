#include "PlatformLocalState.h"

#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "FieldGroupProjectAdapter.h"
#include "FilterAuthoring.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "SoundSourceSemantics.h"
#include "VariableConsumerSemanticsPrivate.h"

#include <QJsonArray>
#include <QJsonObject>

namespace harken::studio::PlatformLocalState {
namespace {

using Kind = PlatformLocalAddress::Kind;

template <typename Groups, typename Visitor>
void visitGroup(Groups& groups, FieldGroupId group, Visitor&& visit)
{
    switch (group) {
    case FieldGroupId::Output: visit(groups.output); break;
    case FieldGroupId::Send: visit(groups.send); break;
    case FieldGroupId::Sidechain: visit(groups.sidechain); break;
    case FieldGroupId::SpatialRating: visit(groups.spatialRating); break;
    case FieldGroupId::Effect: visit(groups.effects); break;
    case FieldGroupId::Conversion: visit(groups.conversion); break;
    }
}

std::uint64_t fieldMask(const PlatformLocalAddress& address)
{
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(address.field);
    return property && address.group != FieldGroupId::Conversion
        ? FieldGroupFieldMask(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(property->id))
        : FieldGroupPropertyMask(address.group);
}

QVariant payloadValue(const FieldGroupPayload& payload, const QString& field)
{
    QVariant result;
    std::visit([&](const auto& group) {
        VisitFieldGroupFields(group, [&](auto, const char* name, const auto& value) {
            if (field != QLatin1String(name)) return;
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, harken::studio::project_schema::AuthoringGuid>)
                result = fromFieldGroupGuid(value);
            else if constexpr (std::is_constructible_v<QVariant, Value>)
                result = value;
        });
    }, payload);
    return result;
}

std::optional<ResolvedFieldGroup> resolveGroup(const ProjectData& project, const ProjectItem& item,
                                               FieldGroupId group, PlatformScope platform)
{
    const auto view = buildFieldGroupProjectView(project);
    if (!view.ok() || project.busses.empty()) return {};
    const FieldGroupResolver resolver(view.definitions,
        toFieldGroupGuid(project.busses.front().rootItem->guid));
    return resolver.Resolve(toFieldGroupGuid(item.guid), toFieldGroupScope(platform), group).value;
}

const ProjectItem* valueSource(const ProjectData& project, const ProjectItem& item,
                               const PlatformLocalAddress& address)
{
    if (!address.canInherit || item.type == ProjectItemType::Bus
        || (address.kind != Kind::GroupField && address.kind != Kind::RowCell)) return &item;
    bool inherited = false;
    visitGroup(item.fieldGroups, address.group, [&](const auto& group) {
        inherited = group.shared.mode != FieldGroupMode::Local;
    });
    if (!inherited) return &item;
    const auto resolved = resolveGroup(project, item, address.group, std::nullopt);
    return resolved ? VariableConsumerSemantics::Internal::findItem(&project,
        fromFieldGroupGuid(resolved->source.definition)) : nullptr;
}

PlatformLocalAddress sourceAddress(const ProjectItem& item, const ProjectItem& source, PlatformLocalAddress address)
{
    if (&source != &item) address.canInherit = false;
    return address;
}

const SoundSourceSet* sourceSet(const VSound& sound, const QString& language)
{
    if (!sound.distinguishLanguage) return &sound.sources.plain;
    const auto found = sound.sources.localized.constFind(language);
    return found == sound.sources.localized.cend() ? nullptr : &*found;
}

QJsonObject itemArgs(const ProjectItem& item, const QString& property)
{
    return {{QStringLiteral("authoring_handle"), static_cast<qint64>(item.objectID)},
            {QStringLiteral("property"), property}};
}

QJsonObject overrideArgs(const ProjectItem& item, const QString& property,
                         const ProjectPlatformId& platform, const QString& operation)
{
    auto args = itemArgs(item, property);
    args.insert(QStringLiteral("override_operation"), operation);
    args.insert(QStringLiteral("platform_ids"), QJsonArray{ProjectPlatformSemantics::idString(platform)});
    return args;
}

QJsonObject groupArgs(const ProjectItem& item, FieldGroupId group, const ProjectPlatformId& platform)
{
    return {{QStringLiteral("definition_guid"), item.guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("group"), QString::fromLatin1(FieldGroupName(group).data())},
            {QStringLiteral("platform_ids"), QJsonArray{ProjectPlatformSemantics::idString(platform)}}};
}

QString groupFieldKey(const PlatformLocalAddress& address)
{
    if (address.group == FieldGroupId::Conversion)
        return address.field;
    return QString::fromLatin1(PlayablePropertyAuthoringV1::fieldByName(address.field)->jsonKey);
}

ActionResult execute(const QString& action, const QJsonObject& args, const QObject* originator)
{
    return ActionManager::instance().execute(action, args, ActionSource::GUI, originator);
}

ActionResult commitSources(const ProjectItem& item, const SoundSources& sources, const QObject* originator)
{
    auto args = itemArgs(item, QStringLiteral("sources"));
    args.insert(QStringLiteral("value"), SoundSourceSemantics::toJson(sources));
    return execute(QStringLiteral("set_item_property"), args, originator);
}

ActionResult unavailable()
{
    return {false, {}, QStringLiteral("INVALID_ARGS"), QStringLiteral("Platform value is unavailable")};
}

template <typename Payload>
const Payload* ownedRows(const AuthoredGroup<Payload>& group, bool canInherit)
{
    return group.shared.localHistory && (!canInherit || group.shared.mode == FieldGroupMode::Local)
        ? &*group.shared.localHistory : nullptr;
}

std::optional<QJsonArray> rowList(const ProjectItem& item, const PlatformLocalAddress& address)
{
    const bool effects = address.field == QLatin1String("effects");
    const bool sidechains = address.field == QLatin1String("sidechains");
    if (item.type == ProjectItemType::Bus) {
        const auto* mix = item.comp<VMixNode>();
        if (!mix) return {};
        if (effects) return mix->effects.toJson();
        return VMixNode::routesToVariant(sidechains ? mix->sidechains : mix->sends, sidechains).toJsonArray();
    }
    if (effects) {
        const auto* payload = ownedRows(item.fieldGroups.effects, address.canInherit);
        return payload ? std::optional(payload->effects.toJson()) : std::nullopt;
    }
    if (sidechains) {
        const auto* payload = ownedRows(item.fieldGroups.sidechain, address.canInherit);
        return payload ? std::optional(fieldGroupRouteRowsToJson(payload->sidechains)) : std::nullopt;
    }
    const auto* payload = ownedRows(item.fieldGroups.send, address.canInherit);
    return payload ? std::optional(fieldGroupRouteRowsToJson(payload->sends)) : std::nullopt;
}

qsizetype rowIndex(const QJsonArray& rows, const PlatformLocalAddress& address)
{
    const auto key = address.field == QLatin1String("effects") ? QStringLiteral("guid") : QStringLiteral("id");
    for (qsizetype index = 0; index < rows.size(); ++index)
        if (QUuid(rows[index].toObject().value(key).toString()) == address.row) return index;
    return -1;
}

QJsonValue rowCellValue(const QJsonObject& row, const QString& cell, const ProjectPlatformId& platform)
{
    const auto values = row.value(QStringLiteral("platforms")).toObject()
        .value(ProjectPlatformSemantics::idString(platform)).toObject();
    if (values.contains(cell)) return values.value(cell);
    if (cell == QLatin1String("bypass")) return row.value(cell).toBool();
    return row.value(cell);
}

ActionResult commitRows(const ProjectItem& item, const PlatformLocalAddress& address, const QJsonArray& rows,
                        const QObject* originator)
{
    if (item.type == ProjectItemType::Bus) {
        auto args = itemArgs(item, address.field);
        args.insert(QStringLiteral("value"), rows);
        return execute(QStringLiteral("set_item_property"), args, originator);
    }
    const auto action = address.field == QLatin1String("effects") ? QStringLiteral("replace_field_group_effects")
        : address.field == QLatin1String("sidechains") ? QStringLiteral("replace_field_group_sidechains")
        : QStringLiteral("replace_field_group_sends");
    return execute(action, {{QStringLiteral("definition_guid"), item.guid.toString(QUuid::WithoutBraces)},
                            {QStringLiteral("group"), QString::fromLatin1(FieldGroupName(address.group).data())},
                            {QStringLiteral("scope"), QStringLiteral("Shared")},
                            {address.field, rows}}, originator);
}

ActionResult editRowCell(const ProjectItem& item, const PlatformLocalAddress& address,
                         const ProjectPlatformId& platform, bool local, const QObject* originator)
{
    auto rows = rowList(item, address);
    const auto index = rows ? rowIndex(*rows, address) : -1;
    if (index < 0) return unavailable();
    auto row = (*rows)[index].toObject();
    auto platforms = row.value(QStringLiteral("platforms")).toObject();
    const auto key = ProjectPlatformSemantics::idString(platform);
    auto values = platforms.value(key).toObject();
    if (local) values.insert(address.cell, rowCellValue(row, address.cell, platform));
    else values.remove(address.cell);
    if (values.isEmpty()) platforms.remove(key);
    else platforms.insert(key, values);
    if (platforms.isEmpty()) row.remove(QStringLiteral("platforms"));
    else row.insert(QStringLiteral("platforms"), platforms);
    (*rows)[index] = row;
    return commitRows(item, address, *rows, originator);
}

template <typename Data>
auto findRule(Data& data, uint32_t ordinal) -> decltype(&data.match.matchRows.front())
{
    const auto found = std::find_if(data.match.matchRows.begin(), data.match.matchRows.end(),
        [&](const auto& row) { return row.stableRuleOrdinal == ordinal; });
    return found == data.match.matchRows.end() ? nullptr : &*found;
}

template <typename Data>
auto findCurve(Data& data, const PlatformLocalAddress& address) -> decltype(&data.blend.entries.front().curves.front())
{
    auto* member = data.blend.member(address.column);
    if (!member) return nullptr;
    const auto found = std::find_if(member->curves.begin(), member->curves.end(),
        [&](const auto& curve) { return curve.rowGuid == address.row; });
    return found == member->curves.end() ? nullptr : &*found;
}

std::optional<size_t> conditionAxis(const SelectionTable& table, const QUuid& axis)
{
    const auto found = std::find(table.matchAxes.begin(), table.matchAxes.end(), axis);
    return found == table.matchAxes.end() ? std::nullopt
                                          : std::optional(static_cast<size_t>(found - table.matchAxes.begin()));
}

QVariant conditionValue(const std::optional<DiscreteGridCell>& cell)
{
    if (!cell) return QVariant::fromValue(nullptr);
    return cell->labelId.isNull() ? QVariant(cell->legacyValue) : QVariant(cell->labelId);
}

bool editSelectionCell(SelectionData& data, const PlatformLocalAddress& address, const ProjectPlatformId& platform,
                       bool local)
{
    switch (address.kind) {
    case Kind::SelectionWeight: {
        auto* occurrence = data.random.find(address.ordinal);
        if (!occurrence) return false;
        if (local) occurrence->platformWeights.try_emplace(platform, occurrence->weight);
        else occurrence->platformWeights.erase(platform);
        return true;
    }
    case Kind::SelectionCondition: {
        auto* rule = findRule(data, address.ordinal);
        const auto axis = conditionAxis(data.match, address.column);
        if (!rule || !axis) return false;
        if (local) {
            rule->platformCells[platform].try_emplace(address.column, rule->cells[*axis]);
            return true;
        }
        const auto cells = rule->platformCells.find(platform);
        if (cells == rule->platformCells.end()) return true;
        cells->second.erase(address.column);
        if (cells->second.empty()) rule->platformCells.erase(cells);
        return true;
    }
    case Kind::SelectionCurve: {
        auto* curve = findCurve(data, address);
        if (!curve) return false;
        if (local) curve->platforms.try_emplace(platform, SelectionCurveShape{curve->points, curve->handles});
        else curve->platforms.erase(platform);
        return true;
    }
    default: return false;
    }
}

ActionResult changeSelectionCell(const ProjectItem& item, const PlatformLocalAddress& address,
                                 const ProjectPlatformId& platform, bool local, const QObject* originator)
{
    const auto* selection = item.comp<VSelection>();
    if (!selection) return unavailable();
    auto data = selection->selections;
    if (!editSelectionCell(data, address, platform, local)) return unavailable();
    auto args = itemArgs(item, QStringLiteral("selections"));
    args.insert(QStringLiteral("value"), data.toJson());
    return execute(QStringLiteral("set_item_property"), args, originator);
}

}

QSet<ProjectPlatformId> localPlatforms(const ProjectData& project, const ProjectItem& owner,
                                       const PlatformLocalAddress& requested)
{
    QSet<ProjectPlatformId> result;
    const auto* source = valueSource(project, owner, requested);
    if (!source) return result;
    const auto& item = *source;
    const auto address = sourceAddress(owner, item, requested);
    switch (address.kind) {
    case Kind::Property: {
        const auto* property = PlayablePropertyAuthoringV1::fieldByName(address.field);
        if (!property) break;
        for (const auto& row : item.platformOverrides.rows)
            if (row.properties.presence.Has(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(property->id)))
                result.insert(row.platformId);
        break;
    }
    case Kind::Filter:
        for (const auto& row : item.platformOverrides.rows) {
            const auto state = FilterAuthoring::readProperty(item, address.field, row.platformId, true);
            if (state && state->present) result.insert(row.platformId);
        }
        break;
    case Kind::GroupField:
        visitGroup(item.fieldGroups, address.group, [&](const auto& group) {
            for (const auto& [platform, state] : group.platforms)
                if (state.localHistory && (state.localFields & fieldMask(address)) != 0
                    && (!address.canInherit || group.shared.mode == FieldGroupMode::Local))
                    result.insert(fromFieldGroupGuid(platform));
        });
        break;
    case Kind::SourceActive:
        if (const auto* sound = item.comp<VSound>())
            if (const auto* set = sourceSet(*sound, address.field))
                for (auto it = set->platforms.cbegin(); it != set->platforms.cend(); ++it)
                    result.insert(QUuid(it.key()));
        break;
    case Kind::RowCell: {
        const auto rows = rowList(item, address);
        const auto index = rows ? rowIndex(*rows, address) : -1;
        if (index < 0) break;
        const auto platforms = (*rows)[index].toObject().value(QStringLiteral("platforms")).toObject();
        for (auto it = platforms.begin(); it != platforms.end(); ++it)
            if (it.value().toObject().contains(address.cell)) result.insert(QUuid(it.key()));
        break;
    }
    case Kind::SelectionWeight:
        if (const auto* selection = item.comp<VSelection>())
            if (const auto* occurrence = selection->selections.random.find(address.ordinal))
                for (const auto& [platform, weight] : occurrence->platformWeights) result.insert(platform);
        break;
    case Kind::SelectionCondition:
        if (const auto* selection = item.comp<VSelection>())
            if (const auto* rule = findRule(selection->selections, address.ordinal))
                for (const auto& [platform, cells] : rule->platformCells)
                    if (cells.contains(address.column)) result.insert(platform);
        break;
    case Kind::SelectionCurve:
        if (const auto* selection = item.comp<VSelection>())
            if (const auto* curve = findCurve(selection->selections, address))
                for (const auto& [platform, shape] : curve->platforms) result.insert(platform);
        break;
    }
    return result;
}

bool hasScalarValue(const PlatformLocalAddress& address)
{
    switch (address.kind) {
    case Kind::Property:
    case Kind::GroupField:
    case Kind::SourceActive:
    case Kind::RowCell:
    case Kind::SelectionWeight:
    case Kind::SelectionCondition: return true;
    case Kind::Filter:
    case Kind::SelectionCurve: return false;
    }
    return false;
}

QVariant effectiveValue(const ProjectData& project, ProjectItem& item,
                        const PlatformLocalAddress& address, const ProjectPlatformId& platform)
{
    switch (address.kind) {
    case Kind::Property: {
        const auto* property = PlayablePropertyAuthoringV1::fieldByName(address.field);
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
        if (!property || !locator) return {};
        const auto state = PlayablePropertyAuthoringV1::readProperty(item, *locator, property->id, platform);
        return state ? state->value.toVariant() : QVariant{};
    }
    case Kind::Filter: {
        const auto state = FilterAuthoring::readProperty(item, address.field, platform);
        return state ? state->value.toVariant() : QVariant{};
    }
    case Kind::GroupField: {
        const auto resolved = resolveGroup(project, item, address.group, platform);
        return resolved ? payloadValue(resolved->payload, address.field) : QVariant{};
    }
    case Kind::SourceActive: {
        const auto* sound = item.comp<VSound>();
        if (!sound) return {};
        const auto resolved = SoundSourceSemantics::resolveSourceSelection(
            *sound, ProjectPlatformSemantics::idString(platform), address.field);
        return resolved.source ? QVariant(resolved.source->name) : QVariant{};
    }
    case Kind::RowCell: {
        const auto* source = valueSource(project, item, address);
        if (!source) return {};
        const auto rows = rowList(*source, sourceAddress(item, *source, address));
        const auto index = rows ? rowIndex(*rows, address) : -1;
        return index < 0 ? QVariant{} : rowCellValue((*rows)[index].toObject(), address.cell, platform).toVariant();
    }
    case Kind::SelectionWeight: {
        const auto* selection = item.comp<VSelection>();
        const auto* occurrence = selection ? selection->selections.random.find(address.ordinal) : nullptr;
        if (!occurrence) return {};
        const auto local = occurrence->platformWeights.find(platform);
        return local == occurrence->platformWeights.end() ? occurrence->weight : local->second;
    }
    case Kind::SelectionCondition: {
        const auto* selection = item.comp<VSelection>();
        const auto* rule = selection ? findRule(selection->selections, address.ordinal) : nullptr;
        const auto axis = selection ? conditionAxis(selection->selections.match, address.column) : std::nullopt;
        if (!rule || !axis) return {};
        const auto cells = rule->platformCells.find(platform);
        if (cells != rule->platformCells.end())
            if (const auto local = cells->second.find(address.column); local != cells->second.end())
                return conditionValue(local->second);
        return conditionValue(rule->cells[*axis]);
    }
    case Kind::SelectionCurve: return {};
    }
    return {};
}

ActionResult setLocal(const ProjectData& project, ProjectItem& item,
                      const PlatformLocalAddress& address, const ProjectPlatformId& platform,
                      const QObject* originator)
{
    const auto value = effectiveValue(project, item, address, platform);
    switch (address.kind) {
    case Kind::Property:
    case Kind::Filter: {
        if (!value.isValid()) return unavailable();
        auto args = overrideArgs(item, address.field, platform, QStringLiteral("set_override"));
        args.insert(QStringLiteral("value"), QJsonValue::fromVariant(value));
        return execute(QStringLiteral("set_item_property"), args, originator);
    }
    case Kind::GroupField: {
        if (!value.isValid()) return unavailable();
        auto args = groupArgs(item, address.group, platform);
        args.insert(QStringLiteral("field"), groupFieldKey(address));
        args.insert(QStringLiteral("value"), value.metaType().id() == QMetaType::QUuid
            ? QJsonValue(value.toUuid().toString(QUuid::WithoutBraces)) : QJsonValue::fromVariant(value));
        return execute(QStringLiteral("set_field_group_field"), args, originator);
    }
    case Kind::SourceActive: {
        const auto* sound = item.comp<VSound>();
        const auto* set = sound ? sourceSet(*sound, address.field) : nullptr;
        if (!set || set->active.isNull()) return unavailable();
        auto sources = sound->sources;
        auto& target = sound->distinguishLanguage ? sources.localized[address.field] : sources.plain;
        SoundSourceSemantics::assignActive(target, set->active, ProjectPlatformSemantics::idString(platform));
        return commitSources(item, sources, originator);
    }
    case Kind::RowCell:
        return editRowCell(item, address, platform, true, originator);
    case Kind::SelectionWeight:
    case Kind::SelectionCondition:
    case Kind::SelectionCurve:
        return changeSelectionCell(item, address, platform, true, originator);
    }
    return unavailable();
}

ActionResult clearLocal(ProjectItem& item, const PlatformLocalAddress& address,
                        const ProjectPlatformId& platform, const QObject* originator)
{
    switch (address.kind) {
    case Kind::Property:
    case Kind::Filter:
        return execute(QStringLiteral("set_item_property"),
            overrideArgs(item, address.field, platform, QStringLiteral("clear_platform_override")), originator);
    case Kind::GroupField: {
        auto args = groupArgs(item, address.group, platform);
        args.insert(QStringLiteral("field"), groupFieldKey(address));
        return execute(QStringLiteral("clear_field_group_field_override"), args, originator);
    }
    case Kind::SourceActive: {
        const auto* sound = item.comp<VSound>();
        if (!sound) return unavailable();
        auto sources = sound->sources;
        auto& target = sound->distinguishLanguage ? sources.localized[address.field] : sources.plain;
        target.platforms.remove(ProjectPlatformSemantics::idString(platform));
        return commitSources(item, sources, originator);
    }
    case Kind::RowCell:
        return editRowCell(item, address, platform, false, originator);
    case Kind::SelectionWeight:
    case Kind::SelectionCondition:
    case Kind::SelectionCurve:
        return changeSelectionCell(item, address, platform, false, originator);
    }
    return unavailable();
}

}
