#include "VSelection.h"

#include "../ContainerBehaviorSemantics.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace VNX {

namespace {

bool unsignedInteger(const QJsonValue& value, double maximum = 4294967295.0)
{
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number)
        && number >= 0.0 && number <= maximum && number == std::trunc(number);
}

SelectionMember* ensureMember(SelectionTable& table, const QUuid& target)
{
    if (auto* existing = table.member(target)) return existing;
    if (target.isNull() || table.occurrenceOrdinalHighWater > std::numeric_limits<uint32_t>::max())
        return nullptr;
    uint64_t nextOrder = 0;
    for (const auto& entry : table.entries)
        for (const auto& occurrence : entry.occurrences)
            nextOrder = std::max(nextOrder, static_cast<uint64_t>(occurrence.order) + 1);
    if (nextOrder > std::numeric_limits<uint32_t>::max()) return nullptr;
    const auto ordinal = static_cast<uint32_t>(table.occurrenceOrdinalHighWater++);
    const auto order = static_cast<uint32_t>(nextOrder);
    table.entries.push_back({target, ordinal, {{ordinal, order, 50}}, {}, {}});
    return &table.entries.back();
}

QJsonArray axisJson(const std::vector<QUuid>& axes)
{
    QJsonArray result;
    for (const auto& axis : axes)
        result.append(axis.isNull() ? QJsonValue(QJsonValue::Null)
                                    : QJsonValue(axis.toString(QUuid::WithoutBraces)));
    return result;
}

bool parseAxes(const QJsonValue& source, std::vector<QUuid>& axes)
{
    if (!source.isArray()) return false;
    QSet<QUuid> seen;
    for (const auto& value : source.toArray()) {
        const QUuid id(value.toString());
        if (id.isNull() || seen.contains(id)) return false;
        seen.insert(id);
        axes.push_back(id);
    }
    return true;
}

QJsonArray mixingRowsJson(const std::vector<SelectionMixingRow>& rows)
{
    QJsonArray result;
    for (const auto& row : rows)
        result.append(QJsonObject{
            {QStringLiteral("guid"), row.guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("variable"), row.variable.isNull() ? QJsonValue(QJsonValue::Null)
                : QJsonValue(row.variable.toString(QUuid::WithoutBraces))}});
    return result;
}

bool parseMixingRows(const QJsonValue& source, std::vector<SelectionMixingRow>& rows)
{
    if (!source.isArray()) return false;
    QSet<QUuid> guids;
    QSet<QUuid> variables;
    for (const auto& value : source.toArray()) {
        SelectionMixingRow row;
        QJsonValue variable = value;
        if (value.isObject()) {
            const auto object = value.toObject();
            if (object.size() != 2) return false;
            row.guid = QUuid(object.value(QStringLiteral("guid")).toString());
            variable = object.value(QStringLiteral("variable"));
        }
        row.variable = QUuid(variable.toString());
        if (!variable.isNull() && (!variable.isString() || row.variable.isNull())) return false;
        if (!value.isObject()) row.guid = MixingSemantics::legacyRowGuid(row.variable);
        if (row.guid.isNull() || guids.contains(row.guid)
            || (!row.variable.isNull() && variables.contains(row.variable))) return false;
        guids.insert(row.guid);
        variables.insert(row.variable);
        rows.push_back(row);
    }
    return true;
}

QJsonArray cellsJson(const std::vector<std::optional<DiscreteGridCell>>& cells)
{
    QJsonArray result;
    for (const auto& cell : cells) {
        if (!cell) result.append(QJsonValue::Null);
        else if (!cell->labelId.isNull()) result.append(QJsonObject{
            {QStringLiteral("labelId"), cell->labelId.toString(QUuid::WithoutBraces)}});
        else result.append(cell->legacyValue);
    }
    return result;
}

QJsonObject curveJson(const SelectionCurve& curve)
{
    QJsonArray points;
    QJsonArray handles;
    for (const auto& point : curve.points) points.append(QJsonArray{point.x, point.value});
    for (const auto& handle : curve.handles)
        handles.append(QJsonArray{handle.h1x, handle.h1y, handle.h2x, handle.h2y});
    return {{QStringLiteral("blendGuid"), curve.rowGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("points"), points}, {QStringLiteral("handles"), handles},
            {QStringLiteral("order"), static_cast<qint64>(curve.order)}};
}

}

const SelectionOccurrence* SelectionTable::find(uint32_t ordinal) const
{
    for (const auto& entry : entries)
        for (const auto& occurrence : entry.occurrences)
            if (occurrence.stableOccurrenceOrdinal == ordinal) return &occurrence;
    return nullptr;
}

SelectionOccurrence* SelectionTable::find(uint32_t ordinal)
{
    return const_cast<SelectionOccurrence*>(std::as_const(*this).find(ordinal));
}

const SelectionMember* SelectionTable::member(const QUuid& childGuid) const
{
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return entry.childGuid == childGuid; });
    return found == entries.end() ? nullptr : &*found;
}

SelectionMember* SelectionTable::member(const QUuid& childGuid)
{
    return const_cast<SelectionMember*>(std::as_const(*this).member(childGuid));
}

const SelectionMember* SelectionTable::memberForOrdinal(uint32_t ordinal) const
{
    for (const auto& entry : entries)
        if (entry.stableMemberOrdinal == ordinal) return &entry;
    return nullptr;
}

const SelectionMember* SelectionTable::memberForOccurrence(uint32_t ordinal) const
{
    for (const auto& entry : entries) {
        for (const auto& occurrence : entry.occurrences)
            if (occurrence.stableOccurrenceOrdinal == ordinal) return &entry;
    }
    return nullptr;
}

std::vector<SelectionEntry> SelectionTable::occurrenceRows() const
{
    std::vector<std::pair<uint32_t, SelectionEntry>> ordered;
    for (const auto& entry : entries)
        for (const auto& occurrence : entry.occurrences)
            ordered.push_back({occurrence.order,
                {entry.childGuid, occurrence.stableOccurrenceOrdinal, occurrence.weight}});
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<SelectionEntry> result;
    result.reserve(ordered.size());
    for (const auto& value : ordered) result.push_back(value.second);
    return result;
}

std::vector<SelectionEntry> SelectionTable::memberRows() const
{
    std::vector<SelectionEntry> result;
    QSet<QUuid> seen;
    for (const auto& occurrence : occurrenceRows()) {
        if (seen.contains(occurrence.childGuid)) continue;
        seen.insert(occurrence.childGuid);
        const auto* entry = member(occurrence.childGuid);
        result.push_back({entry->childGuid, entry->stableMemberOrdinal, 50});
    }
    return result;
}

bool SelectionTable::setOccurrenceRows(const std::vector<SelectionEntry>& rows)
{
    SelectionTable next = *this;
    for (auto& entry : next.entries) {
        const bool retained = std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
            return row.childGuid == entry.childGuid;
        });
        if (retained) continue;
        const SelectionEntry* replacement = nullptr;
        for (const auto& occurrence : entry.occurrences) {
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                return row.stableOccurrenceOrdinal == occurrence.stableOccurrenceOrdinal;
            });
            if (found == rows.end()) continue;
            if (replacement && replacement->childGuid != found->childGuid) return false;
            replacement = &*found;
        }
        if (!replacement) continue;
        if (auto* target = next.member(replacement->childGuid)) {
            for (const auto& curve : entry.curves) {
                const auto found = std::find_if(target->curves.begin(), target->curves.end(),
                    [&](const auto& value) { return value.rowGuid == curve.rowGuid; });
                if (found == target->curves.end()) target->curves.push_back(curve);
                else {
                    auto before = curveJson(*found);
                    auto after = curveJson(curve);
                    before.remove(QStringLiteral("order"));
                    after.remove(QStringLiteral("order"));
                    if (before != after) return false;
                }
            }
            target->conditions.insert(target->conditions.end(), entry.conditions.begin(), entry.conditions.end());
            entry.conditions.clear();
            entry.curves.clear();
            continue;
        }
        entry.childGuid = replacement->childGuid;
    }
    for (auto& entry : next.entries) entry.occurrences.clear();
    QSet<uint32_t> ordinals;
    for (size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        if (row.childGuid.isNull() || row.weight < 0 || row.weight > 100
            || ordinals.contains(row.stableOccurrenceOrdinal)) return false;
        ordinals.insert(row.stableOccurrenceOrdinal);
        auto* entry = next.member(row.childGuid);
        if (!entry) {
            uint32_t memberOrdinal = row.stableOccurrenceOrdinal;
            if (next.memberForOrdinal(memberOrdinal)) {
                next.occurrenceOrdinalHighWater = std::max(next.occurrenceOrdinalHighWater,
                    static_cast<uint64_t>(row.stableOccurrenceOrdinal) + 1);
                if (next.occurrenceOrdinalHighWater > std::numeric_limits<uint32_t>::max()) return false;
                memberOrdinal = static_cast<uint32_t>(next.occurrenceOrdinalHighWater++);
            }
            next.entries.push_back({row.childGuid, memberOrdinal, {}, {}, {}});
            entry = &next.entries.back();
        }
        entry->occurrences.push_back({row.stableOccurrenceOrdinal, static_cast<uint32_t>(index), row.weight});
        next.occurrenceOrdinalHighWater = std::max(next.occurrenceOrdinalHighWater,
            static_cast<uint64_t>(row.stableOccurrenceOrdinal) + 1);
    }
    std::erase_if(next.entries, [](const auto& entry) { return entry.occurrences.empty(); });
    *this = std::move(next);
    return true;
}

DiscreteGridDef SelectionTable::matchGrid() const
{
    DiscreteGridDef result;
    result.axes = matchAxes;
    std::vector<std::pair<uint32_t, DiscreteGridDef::Row>> ordered;
    for (const auto& entry : entries)
        for (const auto& condition : entry.conditions)
            ordered.push_back({condition.priority,
                {entry.childGuid, condition.cells, condition.stableRuleOrdinal}});
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& value : ordered) result.rows.push_back(std::move(value.second));
    return result;
}

bool SelectionTable::setMatchGrid(const DiscreteGridDef& grid)
{
    SelectionTable next = *this;
    std::vector<QUuid> axes;
    if (!parseAxes(axisJson(grid.axes), axes)) return false;
    next.matchAxes = std::move(axes);
    for (auto& entry : next.entries) entry.conditions.clear();
    QSet<uint32_t> ordinals;
    for (size_t index = 0; index < grid.rows.size(); ++index) {
        const auto& row = grid.rows[index];
        if (row.cells.size() != grid.axes.size()) return false;
        auto* entry = ensureMember(next, row.target);
        if (!entry) return false;
        uint32_t ordinal;
        if (row.stableRuleOrdinal) ordinal = *row.stableRuleOrdinal;
        else {
            if (next.ruleOrdinalHighWater > std::numeric_limits<uint32_t>::max()) return false;
            ordinal = static_cast<uint32_t>(next.ruleOrdinalHighWater++);
        }
        if (ordinals.contains(ordinal)) return false;
        ordinals.insert(ordinal);
        next.ruleOrdinalHighWater = std::max(next.ruleOrdinalHighWater, static_cast<uint64_t>(ordinal) + 1);
        entry->conditions.push_back({ordinal, static_cast<uint32_t>(index), row.cells});
    }
    *this = std::move(next);
    return true;
}

MixingDef SelectionTable::mixing() const
{
    MixingDef result;
    for (const auto& mixingRow : mixingRows) {
        MixingRowDef row{mixingRow.variable, {}, mixingRow.guid};
        std::vector<std::pair<uint32_t, MixingChildDef>> ordered;
        for (const auto& entry : entries)
            for (const auto& curve : entry.curves)
                if (curve.rowGuid == mixingRow.guid)
                    ordered.push_back({curve.order, {entry.childGuid, curve.points, curve.handles}});
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& value : ordered) row.children.push_back(std::move(value.second));
        result.rows.push_back(std::move(row));
    }
    return result;
}

bool SelectionTable::setMixing(const MixingDef& mixing)
{
    if (!MixingDef::fromJson(mixing.toJson())) return false;
    SelectionTable next = *this;
    next.mixingRows.clear();
    for (auto& entry : next.entries) entry.curves.clear();
    for (const auto& row : mixing.rows) {
        next.mixingRows.push_back({row.guid, row.variable});
        for (size_t index = 0; index < row.children.size(); ++index) {
            const auto& child = row.children[index];
            auto* entry = ensureMember(next, child.target);
            if (!entry) return false;
            entry->curves.push_back({row.guid, child.points, child.handles, static_cast<uint32_t>(index)});
        }
    }
    *this = std::move(next);
    return true;
}

bool SelectionTable::operator==(const SelectionTable& other) const
{
    return toJson() == other.toJson();
}

QJsonObject SelectionTable::toJson() const
{
    QJsonArray values;
    for (const auto& entry : entries) {
        QJsonArray occurrences;
        QJsonArray conditions;
        QJsonArray curves;
        for (const auto& occurrence : entry.occurrences)
            occurrences.append(QJsonObject{
                {QStringLiteral("stableOccurrenceOrdinal"), static_cast<qint64>(occurrence.stableOccurrenceOrdinal)},
                {QStringLiteral("order"), static_cast<qint64>(occurrence.order)},
                {QStringLiteral("weight"), occurrence.weight}});
        for (const auto& condition : entry.conditions)
            conditions.append(QJsonObject{
                {QStringLiteral("stableRuleOrdinal"), static_cast<qint64>(condition.stableRuleOrdinal)},
                {QStringLiteral("priority"), static_cast<qint64>(condition.priority)},
                {QStringLiteral("cells"), cellsJson(condition.cells)}});
        for (const auto& curve : entry.curves) curves.append(curveJson(curve));
        values.append(QJsonObject{
            {QStringLiteral("childGuid"), entry.childGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("stableMemberOrdinal"), static_cast<qint64>(entry.stableMemberOrdinal)},
            {QStringLiteral("occurrences"), occurrences}, {QStringLiteral("conditions"), conditions},
            {QStringLiteral("curves"), curves}});
    }
    return {{QStringLiteral("occurrenceOrdinalHighWater"), static_cast<qint64>(occurrenceOrdinalHighWater)},
            {QStringLiteral("ruleOrdinalHighWater"), static_cast<qint64>(ruleOrdinalHighWater)},
            {QStringLiteral("entries"), values}, {QStringLiteral("matchAxes"), axisJson(matchAxes)},
            {QStringLiteral("blendAxes"), mixingRowsJson(mixingRows)}};
}

std::optional<SelectionTable> SelectionTable::fromJson(const QJsonObject& json, QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<SelectionTable> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (json.size() != 5 || !json.value(QStringLiteral("entries")).isArray()
        || !unsignedInteger(json.value(QStringLiteral("occurrenceOrdinalHighWater")), 4294967296.0)
        || !unsignedInteger(json.value(QStringLiteral("ruleOrdinalHighWater")), 4294967296.0))
        return fail(QStringLiteral("Selection table must contain entries, axes and identity allocation ranges"));
    SelectionTable parsed;
    parsed.occurrenceOrdinalHighWater = static_cast<uint64_t>(
        json.value(QStringLiteral("occurrenceOrdinalHighWater")).toDouble());
    parsed.ruleOrdinalHighWater = static_cast<uint64_t>(json.value(QStringLiteral("ruleOrdinalHighWater")).toDouble());
    if (!parseAxes(json.value(QStringLiteral("matchAxes")), parsed.matchAxes)
        || !parseMixingRows(json.value(QStringLiteral("blendAxes")), parsed.mixingRows))
        return fail(QStringLiteral("Selection axes must have unique identities and Variable references"));
    QSet<QUuid> targets;
    QSet<uint32_t> memberIds;
    QSet<uint32_t> occurrenceIds;
    QSet<uint32_t> occurrenceOrders;
    QSet<uint32_t> ruleIds;
    QSet<uint32_t> priorities;
    QHash<QUuid, QSet<uint32_t>> curveOrders;
    for (const auto& value : json.value(QStringLiteral("entries")).toArray()) {
        const auto object = value.toObject();
        SelectionMember entry;
        entry.childGuid = QUuid(object.value(QStringLiteral("childGuid")).toString());
        if (!value.isObject() || object.size() != 5 || entry.childGuid.isNull() || targets.contains(entry.childGuid)
            || !unsignedInteger(object.value(QStringLiteral("stableMemberOrdinal")))
            || !object.value(QStringLiteral("occurrences")).isArray()
            || object.value(QStringLiteral("occurrences")).toArray().isEmpty()
            || !object.value(QStringLiteral("conditions")).isArray()
            || !object.value(QStringLiteral("curves")).isArray())
            return fail(QStringLiteral("Selection member identity or attributes are invalid"));
        targets.insert(entry.childGuid);
        entry.stableMemberOrdinal = static_cast<uint32_t>(object.value(QStringLiteral("stableMemberOrdinal")).toDouble());
        if (entry.stableMemberOrdinal >= parsed.occurrenceOrdinalHighWater
            || memberIds.contains(entry.stableMemberOrdinal))
            return fail(QStringLiteral("Selection member ordinal is outside its allocation range or reused"));
        memberIds.insert(entry.stableMemberOrdinal);
        for (const auto& source : object.value(QStringLiteral("occurrences")).toArray()) {
            const auto row = source.toObject();
            if (!source.isObject() || row.size() != 3
                || !unsignedInteger(row.value(QStringLiteral("stableOccurrenceOrdinal")))
                || !unsignedInteger(row.value(QStringLiteral("order")))
                || !unsignedInteger(row.value(QStringLiteral("weight")), 100.0))
                return fail(QStringLiteral("Selection occurrence is invalid"));
            SelectionOccurrence occurrence{
                static_cast<uint32_t>(row.value(QStringLiteral("stableOccurrenceOrdinal")).toDouble()),
                static_cast<uint32_t>(row.value(QStringLiteral("order")).toDouble()),
                row.value(QStringLiteral("weight")).toInt()};
            if (occurrence.stableOccurrenceOrdinal >= parsed.occurrenceOrdinalHighWater
                || occurrenceIds.contains(occurrence.stableOccurrenceOrdinal)
                || occurrenceOrders.contains(occurrence.order))
                return fail(QStringLiteral("Selection occurrence identity or order is reused"));
            occurrenceIds.insert(occurrence.stableOccurrenceOrdinal);
            occurrenceOrders.insert(occurrence.order);
            entry.occurrences.push_back(occurrence);
        }
        for (const auto& source : object.value(QStringLiteral("conditions")).toArray()) {
            const auto row = source.toObject();
            if (!source.isObject() || row.size() != 3
                || !unsignedInteger(row.value(QStringLiteral("stableRuleOrdinal")))
                || !unsignedInteger(row.value(QStringLiteral("priority")))
                || !row.value(QStringLiteral("cells")).isArray()
                || row.value(QStringLiteral("cells")).toArray().size() != static_cast<qsizetype>(parsed.matchAxes.size()))
                return fail(QStringLiteral("Selection condition is invalid"));
            SelectionCondition condition{
                static_cast<uint32_t>(row.value(QStringLiteral("stableRuleOrdinal")).toDouble()),
                static_cast<uint32_t>(row.value(QStringLiteral("priority")).toDouble()), {}};
            if (condition.stableRuleOrdinal >= parsed.ruleOrdinalHighWater || ruleIds.contains(condition.stableRuleOrdinal)
                || priorities.contains(condition.priority))
                return fail(QStringLiteral("Selection condition identity or priority is reused"));
            ruleIds.insert(condition.stableRuleOrdinal);
            priorities.insert(condition.priority);
            for (const auto& cell : row.value(QStringLiteral("cells")).toArray()) {
                if (cell.isNull()) condition.cells.push_back(std::nullopt);
                else if (cell.isObject() && cell.toObject().size() == 1
                    && !QUuid(cell.toObject().value(QStringLiteral("labelId")).toString()).isNull())
                    condition.cells.push_back(DiscreteGridCell(QUuid(cell.toObject().value(QStringLiteral("labelId")).toString())));
                else if (unsignedInteger(cell, 16777216.0)) condition.cells.push_back(DiscreteGridCell(cell.toInt()));
                else return fail(QStringLiteral("Selection condition cell is invalid"));
            }
            entry.conditions.push_back(std::move(condition));
        }
        QSet<QUuid> curveRows;
        for (const auto& source : object.value(QStringLiteral("curves")).toArray()) {
            const auto curve = source.toObject();
            if (!source.isObject() || curve.size() != 4
                || !unsignedInteger(curve.value(QStringLiteral("order"))))
                return fail(QStringLiteral("Selection curve is invalid"));
            const SelectionMixingRow* mixingRow = nullptr;
            if (curve.contains(QStringLiteral("blendGuid"))) {
                const QUuid guid(curve.value(QStringLiteral("blendGuid")).toString());
                for (const auto& candidate : parsed.mixingRows)
                    if (candidate.guid == guid) mixingRow = &candidate;
            } else {
                const auto reference = curve.value(QStringLiteral("variable"));
                const QUuid variable(reference.toString());
                if (!reference.isNull() && (!reference.isString() || variable.isNull()))
                    return fail(QStringLiteral("Selection curve Variable is invalid"));
                for (const auto& candidate : parsed.mixingRows) {
                    if (candidate.variable != variable) continue;
                    if (mixingRow) return fail(QStringLiteral("Selection curve Variable is ambiguous"));
                    mixingRow = &candidate;
                }
            }
            if (!mixingRow || curveRows.contains(mixingRow->guid))
                return fail(QStringLiteral("Selection curve row is missing or duplicated"));
            const auto& variable = mixingRow->variable;
            const auto order = static_cast<uint32_t>(curve.value(QStringLiteral("order")).toDouble());
            if (curveOrders[mixingRow->guid].contains(order)) return fail(QStringLiteral("Selection curve order is reused"));
            curveOrders[mixingRow->guid].insert(order);
            const QJsonObject childJson{
                {QStringLiteral("target"), entry.childGuid.toString(QUuid::WithoutBraces)},
                {QStringLiteral("points"), curve.value(QStringLiteral("points"))},
                {QStringLiteral("handles"), curve.value(QStringLiteral("handles"))}};
            const auto mixing = MixingDef::fromJson(QJsonArray{QJsonObject{
                {QStringLiteral("guid"), mixingRow->guid.toString(QUuid::WithoutBraces)},
                {QStringLiteral("variable"), variable.isNull() ? QJsonValue(QJsonValue::Null)
                    : QJsonValue(variable.toString(QUuid::WithoutBraces))},
                {QStringLiteral("children"), QJsonArray{childJson}}}});
            if (!mixing) return fail(QStringLiteral("Selection curve is invalid"));
            curveRows.insert(mixingRow->guid);
            const auto& child = mixing->rows.front().children.front();
            entry.curves.push_back({mixingRow->guid, child.points, child.handles, order});
        }
        parsed.entries.push_back(std::move(entry));
    }
    return parsed;
}

const SelectionTable& SelectionData::table(PlatformScope scope) const
{
    if (scope) {
        const auto found = platforms.constFind(ProjectPlatformSemantics::idString(*scope));
        if (found != platforms.constEnd()) return *found;
    }
    return shared;
}

SelectionTable& SelectionData::editTable(PlatformScope scope)
{
    if (!scope) return shared;
    const QString key = ProjectPlatformSemantics::idString(*scope);
    auto found = platforms.find(key);
    if (found == platforms.end()) found = platforms.insert(key, shared);
    return *found;
}

QList<const SelectionTable*> SelectionData::tables() const
{
    QList<const SelectionTable*> result{&shared};
    for (const auto& table : platforms) result.append(&table);
    return result;
}

QList<SelectionTable*> SelectionData::tables()
{
    QList<SelectionTable*> result{&shared};
    for (auto& table : platforms) result.append(&table);
    return result;
}

QJsonObject SelectionData::toJson() const
{
    QJsonObject overrides;
    for (auto it = platforms.constBegin(); it != platforms.constEnd(); ++it)
        overrides.insert(it.key(), it->toJson());
    return {{QStringLiteral("shared"), shared.toJson()},
            {QStringLiteral("platforms"), overrides}};
}

std::optional<SelectionData> SelectionData::fromJson(const QJsonObject& json, QString* error)
{
    if (json.size() != 2 || !json.value(QStringLiteral("shared")).isObject()
        || !json.value(QStringLiteral("platforms")).isObject()) {
        if (error) *error = QStringLiteral("Selections must contain shared and platforms");
        return std::nullopt;
    }
    SelectionData parsed;
    auto shared = SelectionTable::fromJson(json.value(QStringLiteral("shared")).toObject(), error);
    if (!shared) return std::nullopt;
    parsed.shared = std::move(*shared);
    const auto platforms = json.value(QStringLiteral("platforms")).toObject();
    for (auto it = platforms.begin(); it != platforms.end(); ++it) {
        const QUuid platform(it.key());
        if (platform.isNull() || ProjectPlatformSemantics::idString(platform) != it.key()
            || !it->isObject()) {
            if (error) *error = QStringLiteral("Selection platform is invalid");
            return std::nullopt;
        }
        auto table = SelectionTable::fromJson(it->toObject(), error);
        if (!table) return std::nullopt;
        parsed.platforms.insert(it.key(), std::move(*table));
    }
    return parsed;
}

bool VSelection::fromJson(const QJsonObject& json)
{
    const QSet<QString> fields{QStringLiteral("mode"), QStringLiteral("playType"),
        QStringLiteral("matchResponse"), QStringLiteral("blendResponse"), QStringLiteral("selections"),
        QStringLiteral("behavior"), QStringLiteral("spliceByMarker"), QStringLiteral("transitions")};
    for (auto it = json.begin(); it != json.end(); ++it)
        if (!fields.contains(it.key())) return false;
    VSelection parsed;
    for (const auto& field : {QStringLiteral("mode"), QStringLiteral("playType")}) {
        if (json.contains(field) && !parsed.setField(field, json.value(field).toVariant()))
            return false;
    }
    for (const auto& field : {QStringLiteral("matchResponse"), QStringLiteral("blendResponse")}) {
        if (!json.contains(field)) continue;
        if (!unsignedInteger(json.value(field), 1.0)) return false;
        auto& response = field == QLatin1String("matchResponse") ? parsed.matchResponse : parsed.blendResponse;
        response = static_cast<VariableResponse>(json.value(field).toInt());
    }
    parsed.spliceByMarker = json.value(QStringLiteral("spliceByMarker")).toBool(false);
    if (json.contains(QStringLiteral("behavior"))) {
        if (!json.value(QStringLiteral("behavior")).isObject()) return false;
        const auto behavior = ContainerBehaviorSemantics::fromJson(
            json.value(QStringLiteral("behavior")).toObject());
        if (!behavior) return false;
        parsed.behavior = *behavior;
    }
    if (json.contains(QStringLiteral("selections"))) {
        if (!json.value(QStringLiteral("selections")).isObject()) return false;
        auto selections = SelectionData::fromJson(json.value(QStringLiteral("selections")).toObject());
        if (!selections) return false;
        parsed.selections = std::move(*selections);
    }
    if (json.contains(QStringLiteral("transitions"))) {
        if (!json.value(QStringLiteral("transitions")).isObject()) return false;
        const auto table = TransitionTableDef::fromJson(json.value(QStringLiteral("transitions")).toObject());
        if (!table) return false;
        parsed.transitions = *table;
    }
    mode = parsed.mode;
    playType = parsed.playType;
    matchResponse = parsed.matchResponse;
    blendResponse = parsed.blendResponse;
    selections = std::move(parsed.selections);
    behavior = parsed.behavior;
    spliceByMarker = parsed.spliceByMarker;
    transitions = std::move(parsed.transitions);
    return true;
}

void VSelection::toJsonExtra(QJsonObject& json) const
{
    json[QStringLiteral("selections")] = selections.toJson();
    json[QStringLiteral("matchResponse")] = static_cast<int>(matchResponse);
    json[QStringLiteral("blendResponse")] = static_cast<int>(blendResponse);
    const auto behaviorJson = ContainerBehaviorSemantics::toJson(behavior);
    if (!behaviorJson.isEmpty()) json[QStringLiteral("behavior")] = behaviorJson;
    if (spliceByMarker) json[QStringLiteral("spliceByMarker")] = true;
    if (transitions != canonicalTransitionTable()) json[QStringLiteral("transitions")] = transitions.toJson();
}

void VSelection::fromJsonExtra(const QJsonObject& json)
{
    spliceByMarker = json.value(QStringLiteral("spliceByMarker")).toBool(false);
}

std::vector<SelectionEntry> VSelection::activeRows(PlatformScope scope) const
{
    const auto& table = selections.table(scope);
    if (mode == SelectionMode::Random || mode == SelectionMode::Sequence)
        return table.occurrenceRows();
    auto result = table.memberRows();
    std::erase_if(result, [&](const auto& row) {
        const auto* entry = table.member(row.childGuid);
        return mode == SelectionMode::Match ? entry->conditions.empty() : entry->curves.empty();
    });
    return result;
}

}
