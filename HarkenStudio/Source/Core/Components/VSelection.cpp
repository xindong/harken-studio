#include "VSelection.h"

#include "../ContainerBehaviorSemantics.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace harken::studio {

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
    table.entries.push_back({target, ordinal, {{ordinal, order, 50}}, {}});
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

QJsonValue cellJson(const DiscreteGridCell& cell)
{
    if (!cell.labelId.isNull())
        return QJsonObject{{QStringLiteral("labelId"), cell.labelId.toString(QUuid::WithoutBraces)}};
    return cell.legacyValue;
}

bool parseCell(const QJsonValue& value, std::optional<DiscreteGridCell>& cell)
{
    if (value.isNull()) cell = std::nullopt;
    else if (value.isObject() && value.toObject().size() == 1
        && !QUuid(value.toObject().value(QStringLiteral("labelId")).toString()).isNull())
        cell = DiscreteGridCell(QUuid(value.toObject().value(QStringLiteral("labelId")).toString()));
    else if (unsignedInteger(value, 16777216.0)) cell = DiscreteGridCell(value.toInt());
    else return false;
    return true;
}

QString platformKey(const QUuid& platform)
{
    return platform.toString(QUuid::WithoutBraces);
}

bool parsePlatformKey(const QString& key, QUuid& platform)
{
    platform = QUuid(key);
    return !platform.isNull() && platformKey(platform) == key;
}

QJsonObject curveJson(const SelectionCurve& curve)
{
    QJsonArray points;
    QJsonArray handles;
    for (const auto& point : curve.points) points.append(QJsonArray{point.x, point.value});
    for (const auto& handle : curve.handles)
        handles.append(QJsonArray{handle.h1x, handle.h1y, handle.h2x, handle.h2y});
    return {{QStringLiteral("points"), points}, {QStringLiteral("handles"), handles},
            {QStringLiteral("blendGuid"), curve.rowGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("order"), static_cast<qint64>(curve.order)}};
}

template <typename Tables>
auto modeTablesOf(Tables& data, SelectionMode mode) -> decltype(&data.random)
{
    switch (mode) {
    case SelectionMode::Sequence: return &data.sequence;
    case SelectionMode::Match: return &data.match;
    case SelectionMode::Blend: return &data.blend;
    case SelectionMode::Random: break;
    }
    return &data.random;
}

constexpr std::pair<const char*, SelectionMode> kModes[] = {
    {"random", SelectionMode::Random}, {"sequence", SelectionMode::Sequence},
    {"match", SelectionMode::Match}, {"blend", SelectionMode::Blend}};

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
        for (auto& matchRow : next.matchRows)
            if (matchRow.target == entry.childGuid) matchRow.target = replacement->childGuid;
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
            next.entries.push_back({row.childGuid, memberOrdinal, {}, {}});
            entry = &next.entries.back();
        }
        entry->occurrences.push_back({row.stableOccurrenceOrdinal, static_cast<uint32_t>(index), row.weight});
        next.occurrenceOrdinalHighWater = std::max(next.occurrenceOrdinalHighWater,
            static_cast<uint64_t>(row.stableOccurrenceOrdinal) + 1);
    }
    next.removeMembers([](const auto& entry) { return entry.occurrences.empty(); });
    *this = std::move(next);
    return true;
}

void SelectionTable::removeMembers(const std::function<bool(const SelectionMember&)>& predicate)
{
    std::erase_if(entries, predicate);
    for (auto& row : matchRows)
        if (!row.target.isNull() && !member(row.target)) row.target = QUuid{};
}

DiscreteGridDef SelectionTable::matchGrid(const std::vector<QUuid>& axes) const
{
    DiscreteGridDef result;
    result.axes = axes;
    for (const auto& row : matchRows) {
        std::vector<std::optional<DiscreteGridCell>> cells;
        for (const auto& axis : axes) {
            const auto cell = row.cells.find(axis);
            cells.push_back(cell == row.cells.end() ? std::nullopt : std::optional(cell->second));
        }
        result.rows.push_back({row.target, std::move(cells), row.stableRuleOrdinal});
    }
    return result;
}

bool SelectionTable::setMatchGrid(const DiscreteGridDef& grid)
{
    SelectionTable next = *this;
    std::vector<QUuid> axes;
    if (!parseAxes(axisJson(grid.axes), axes)) return false;
    next.matchRows.clear();
    QSet<uint32_t> ordinals;
    for (const auto& row : grid.rows) {
        if (row.cells.size() != grid.axes.size()) return false;
        if (!row.target.isNull() && !ensureMember(next, row.target)) return false;
        uint32_t ordinal;
        if (row.stableRuleOrdinal) ordinal = *row.stableRuleOrdinal;
        else {
            if (next.ruleOrdinalHighWater > std::numeric_limits<uint32_t>::max()) return false;
            ordinal = static_cast<uint32_t>(next.ruleOrdinalHighWater++);
        }
        if (ordinals.contains(ordinal)) return false;
        ordinals.insert(ordinal);
        next.ruleOrdinalHighWater = std::max(next.ruleOrdinalHighWater, static_cast<uint64_t>(ordinal) + 1);
        SelectionMatchRow rule{ordinal, row.target, {}};
        for (size_t index = 0; index < axes.size(); ++index)
            if (row.cells[index]) rule.cells.emplace(axes[index], *row.cells[index]);
        next.matchRows.push_back(std::move(rule));
    }
    next.removeMembers([&](const SelectionMember& member) {
        return std::none_of(next.matchRows.begin(), next.matchRows.end(),
            [&](const SelectionMatchRow& row) { return row.target == member.childGuid; });
    });
    *this = std::move(next);
    return true;
}

MixingDef SelectionTable::mixing(const std::vector<SelectionMixingRow>& axes) const
{
    MixingDef result;
    for (const auto& axis : axes) {
        MixingRowDef row{axis.variable, {}, axis.guid};
        std::vector<std::pair<uint32_t, MixingChildDef>> ordered;
        for (const auto& entry : entries)
            for (const auto& curve : entry.curves)
                if (curve.rowGuid == axis.guid)
                    ordered.push_back({curve.order, MixingChildDef{entry.childGuid, curve.points, curve.handles}});
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
    for (auto& entry : next.entries) entry.curves.clear();
    for (const auto& row : mixing.rows)
        for (size_t index = 0; index < row.children.size(); ++index) {
            const auto& child = row.children[index];
            auto* entry = ensureMember(next, child.target);
            if (!entry) return false;
            entry->curves.push_back({row.guid, child.points, child.handles, static_cast<uint32_t>(index)});
        }
    next.removeMembers([](const SelectionMember& member) { return member.curves.empty(); });
    *this = std::move(next);
    return true;
}

bool SelectionTable::operator==(const SelectionTable& other) const
{
    return occurrenceOrdinalHighWater == other.occurrenceOrdinalHighWater
        && ruleOrdinalHighWater == other.ruleOrdinalHighWater && toJson() == other.toJson();
}

QJsonObject SelectionTable::toJson() const
{
    QJsonArray values;
    for (const auto& entry : entries) {
        QJsonArray occurrences;
        QJsonArray curves;
        for (const auto& occurrence : entry.occurrences)
            occurrences.append(QJsonObject{
                {QStringLiteral("stableOccurrenceOrdinal"), static_cast<qint64>(occurrence.stableOccurrenceOrdinal)},
                {QStringLiteral("order"), static_cast<qint64>(occurrence.order)},
                {QStringLiteral("weight"), occurrence.weight}});
        for (const auto& curve : entry.curves) curves.append(curveJson(curve));
        values.append(QJsonObject{
            {QStringLiteral("childGuid"), entry.childGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("stableMemberOrdinal"), static_cast<qint64>(entry.stableMemberOrdinal)},
            {QStringLiteral("occurrences"), occurrences}, {QStringLiteral("curves"), curves}});
    }
    QJsonArray rows;
    for (const auto& row : matchRows) {
        QJsonObject cells;
        for (const auto& [axis, cell] : row.cells) cells.insert(axis.toString(QUuid::WithoutBraces), cellJson(cell));
        rows.append(QJsonObject{
            {QStringLiteral("stableRuleOrdinal"), static_cast<qint64>(row.stableRuleOrdinal)},
            {QStringLiteral("target"), row.target.isNull() ? QJsonValue(QJsonValue::Null)
                : QJsonValue(row.target.toString(QUuid::WithoutBraces))},
            {QStringLiteral("cells"), cells}});
    }
    return {{QStringLiteral("entries"), values}, {QStringLiteral("matchRows"), rows}};
}

std::optional<SelectionTable> SelectionTable::fromJson(const QJsonObject& json, uint64_t occurrenceOrdinalHighWater,
                                                       uint64_t ruleOrdinalHighWater,
                                                       const std::vector<QUuid>& matchAxes,
                                                       const std::vector<SelectionMixingRow>& blendAxes,
                                                       QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<SelectionTable> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (json.size() != 2 || !json.value(QStringLiteral("entries")).isArray()
        || !json.value(QStringLiteral("matchRows")).isArray())
        return fail(QStringLiteral("Selection table must contain entries and matchRows"));
    SelectionTable parsed;
    parsed.occurrenceOrdinalHighWater = occurrenceOrdinalHighWater;
    parsed.ruleOrdinalHighWater = ruleOrdinalHighWater;
    QSet<QUuid> targets;
    QSet<uint32_t> memberIds;
    QSet<uint32_t> occurrenceIds;
    QSet<uint32_t> occurrenceOrders;
    QSet<uint32_t> ruleIds;
    QHash<QUuid, QSet<uint32_t>> curveOrders;
    for (const auto& value : json.value(QStringLiteral("entries")).toArray()) {
        const auto object = value.toObject();
        SelectionMember entry;
        entry.childGuid = QUuid(object.value(QStringLiteral("childGuid")).toString());
        if (!value.isObject() || object.size() != 4 || entry.childGuid.isNull() || targets.contains(entry.childGuid)
            || !unsignedInteger(object.value(QStringLiteral("stableMemberOrdinal")))
            || !object.value(QStringLiteral("occurrences")).isArray()
            || object.value(QStringLiteral("occurrences")).toArray().isEmpty()
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
        QSet<QUuid> curveRows;
        for (const auto& source : object.value(QStringLiteral("curves")).toArray()) {
            const auto curve = source.toObject();
            if (!source.isObject() || curve.size() != 4 || !unsignedInteger(curve.value(QStringLiteral("order"))))
                return fail(QStringLiteral("Selection curve is invalid"));
            const SelectionMixingRow* mixingRow = nullptr;
            if (curve.contains(QStringLiteral("blendGuid"))) {
                const QUuid guid(curve.value(QStringLiteral("blendGuid")).toString());
                for (const auto& candidate : blendAxes)
                    if (candidate.guid == guid) mixingRow = &candidate;
            } else {
                const auto reference = curve.value(QStringLiteral("variable"));
                const QUuid variable(reference.toString());
                if (!reference.isNull() && (!reference.isString() || variable.isNull()))
                    return fail(QStringLiteral("Selection curve Variable is invalid"));
                for (const auto& candidate : blendAxes) {
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
    for (const auto& source : json.value(QStringLiteral("matchRows")).toArray()) {
        const auto object = source.toObject();
        const auto target = object.value(QStringLiteral("target"));
        const auto cells = object.value(QStringLiteral("cells"));
        if (!source.isObject() || object.size() != 3
            || !unsignedInteger(object.value(QStringLiteral("stableRuleOrdinal"))) || !cells.isObject())
            return fail(QStringLiteral("Selection match row is invalid"));
        SelectionMatchRow row{
            static_cast<uint32_t>(object.value(QStringLiteral("stableRuleOrdinal")).toDouble()),
            QUuid(target.toString()), {}};
        if (!target.isNull() && (!target.isString() || !targets.contains(row.target)))
            return fail(QStringLiteral("Selection match row target must be null or a Selection member"));
        if (row.stableRuleOrdinal >= parsed.ruleOrdinalHighWater || ruleIds.contains(row.stableRuleOrdinal))
            return fail(QStringLiteral("Selection match row ordinal is outside its allocation range or reused"));
        ruleIds.insert(row.stableRuleOrdinal);
        const auto values = cells.toObject();
        for (auto cell = values.begin(); cell != values.end(); ++cell) {
            const QUuid axis(cell.key());
            std::optional<DiscreteGridCell> parsedCell;
            if (std::find(matchAxes.begin(), matchAxes.end(), axis) == matchAxes.end()
                || axis.toString(QUuid::WithoutBraces) != cell.key() || !parseCell(cell.value(), parsedCell))
                return fail(QStringLiteral("Selection match row cell is invalid"));
            if (parsedCell) row.cells.emplace(axis, *parsedCell);
        }
        parsed.matchRows.push_back(std::move(row));
    }
    return parsed;
}

const SelectionTable& SelectionModeTables::table(PlatformScope scope) const
{
    if (scope)
        if (const auto found = platforms.find(*scope); found != platforms.end()) return found->second;
    return shared;
}

QList<const SelectionTable*> SelectionModeTables::tables() const
{
    QList<const SelectionTable*> result{&shared};
    for (const auto& [platform, table] : platforms) result.append(&table);
    return result;
}

QList<SelectionTable*> SelectionModeTables::tables()
{
    QList<SelectionTable*> result{&shared};
    for (auto& [platform, table] : platforms) result.append(&table);
    return result;
}

const SelectionModeTables& SelectionData::modeTables(SelectionMode mode) const
{
    return *modeTablesOf(*this, mode);
}

SelectionModeTables& SelectionData::editModeTables(SelectionMode mode)
{
    return *modeTablesOf(*this, mode);
}

const SelectionTable& SelectionData::table(SelectionMode mode, PlatformScope scope) const
{
    return modeTables(mode).table(scope);
}

QList<const SelectionTable*> SelectionData::tables() const
{
    QList<const SelectionTable*> result;
    for (const auto& [key, mode] : kModes) result.append(modeTables(mode).tables());
    return result;
}

QList<SelectionTable*> SelectionData::tables()
{
    QList<SelectionTable*> result;
    for (const auto& [key, mode] : kModes) result.append(editModeTables(mode).tables());
    return result;
}

DiscreteGridDef SelectionData::matchGrid(PlatformScope scope) const
{
    return match.table(scope).matchGrid(matchAxes);
}

MixingDef SelectionData::mixing(PlatformScope scope) const
{
    return blend.table(scope).mixing(blendAxes);
}

bool SelectionData::canEdit(SelectionMode mode, PlatformScope displayed, PlatformScope target) const
{
    if (target) return true;
    return !displayed || !modeTables(mode).platforms.contains(*displayed);
}

bool SelectionData::editTable(SelectionMode mode, PlatformScope target,
                              const std::function<bool(SelectionTable&)>& edit)
{
    auto& tables = editModeTables(mode);
    uint64_t occurrences = 0;
    uint64_t rules = 0;
    for (const auto* table : tables.tables()) {
        occurrences = std::max(occurrences, table->occurrenceOrdinalHighWater);
        rules = std::max(rules, table->ruleOrdinalHighWater);
    }
    SelectionTable next = tables.table(target);
    next.occurrenceOrdinalHighWater = occurrences;
    next.ruleOrdinalHighWater = rules;
    if (!edit(next)) return false;
    const bool unchangedFork = target && !tables.platforms.contains(*target)
        && next.toJson() == tables.shared.toJson();
    if (!unchangedFork) (target ? tables.platforms[*target] : tables.shared) = next;
    for (auto* table : tables.tables()) {
        table->occurrenceOrdinalHighWater = next.occurrenceOrdinalHighWater;
        table->ruleOrdinalHighWater = next.ruleOrdinalHighWater;
    }
    return true;
}

bool SelectionData::setOccurrenceRows(SelectionMode mode, const std::vector<SelectionEntry>& rows,
                                      PlatformScope target)
{
    auto next = *this;
    if (!next.editTable(mode, target, [&](SelectionTable& table) { return table.setOccurrenceRows(rows); }))
        return false;
    *this = std::move(next);
    return true;
}

bool SelectionData::setMatchGrid(const DiscreteGridDef& grid, PlatformScope target)
{
    auto next = *this;
    next.matchAxes.clear();
    if (!parseAxes(axisJson(grid.axes), next.matchAxes)) return false;
    for (auto* table : next.tables())
        for (auto& row : table->matchRows)
            std::erase_if(row.cells, [&](const auto& cell) {
                return std::find(next.matchAxes.begin(), next.matchAxes.end(), cell.first) == next.matchAxes.end();
            });
    if (!next.editTable(SelectionMode::Match, target, [&](SelectionTable& table) { return table.setMatchGrid(grid); }))
        return false;
    *this = std::move(next);
    return true;
}

bool SelectionData::setMixing(const MixingDef& mixing, PlatformScope target)
{
    if (!MixingDef::fromJson(mixing.toJson())) return false;
    auto next = *this;
    next.blendAxes.clear();
    for (const auto& row : mixing.rows) next.blendAxes.push_back({row.guid, row.variable});
    const auto kept = [&](const QUuid& guid) {
        return std::any_of(next.blendAxes.begin(), next.blendAxes.end(),
            [&](const SelectionMixingRow& axis) { return axis.guid == guid; });
    };
    for (auto* table : next.tables())
        for (auto& entry : table->entries)
            std::erase_if(entry.curves, [&](const SelectionCurve& curve) { return !kept(curve.rowGuid); });
    for (auto* table : next.blend.tables())
        table->removeMembers([](const SelectionMember& member) { return member.curves.empty(); });
    if (!next.editTable(SelectionMode::Blend, target, [&](SelectionTable& table) { return table.setMixing(mixing); }))
        return false;
    *this = std::move(next);
    return true;
}

bool SelectionData::setLocal(SelectionMode mode, const QUuid& platform, bool local)
{
    auto& tables = editModeTables(mode);
    if (platform.isNull()) return false;
    if (!local) return tables.platforms.erase(platform) > 0;
    return tables.platforms.try_emplace(platform, tables.shared).second;
}

void SelectionData::removeMembers(const std::function<bool(const SelectionMember&)>& predicate)
{
    for (auto* table : tables()) table->removeMembers(predicate);
}

void SelectionData::remapVariables(const QHash<QUuid, QUuid>& guids)
{
    for (auto& axis : matchAxes) axis = guids.value(axis, axis);
    for (auto& axis : blendAxes) axis.variable = guids.value(axis.variable, axis.variable);
    for (auto* table : tables())
        for (auto& row : table->matchRows) {
            std::map<QUuid, DiscreteGridCell> mapped;
            for (const auto& [axis, cell] : row.cells) mapped.emplace(guids.value(axis, axis), cell);
            row.cells = std::move(mapped);
        }
}

void SelectionData::replaceMatchAxis(size_t index, const QUuid& variable)
{
    const auto previous = matchAxes[index];
    matchAxes[index] = variable;
    for (auto* table : tables())
        for (auto& row : table->matchRows)
            if (const auto cell = row.cells.find(previous); cell != row.cells.end()) {
                const auto value = cell->second;
                row.cells.erase(cell);
                row.cells.insert_or_assign(variable, value);
            }
}

QSet<QUuid> SelectionData::platformIds() const
{
    QSet<QUuid> result;
    for (const auto& [key, mode] : kModes)
        for (const auto& [platform, table] : modeTables(mode).platforms) result.insert(platform);
    return result;
}

void SelectionData::removePlatform(const QUuid& platform)
{
    for (const auto& [key, mode] : kModes) editModeTables(mode).platforms.erase(platform);
}

QJsonObject SelectionData::toJson() const
{
    QJsonObject result{{QStringLiteral("matchAxes"), axisJson(matchAxes)},
                       {QStringLiteral("blendAxes"), mixingRowsJson(blendAxes)}};
    for (const auto& [key, mode] : kModes) {
        const auto& tables = modeTables(mode);
        QJsonObject value{
            {QStringLiteral("occurrenceOrdinalHighWater"), static_cast<qint64>(tables.shared.occurrenceOrdinalHighWater)},
            {QStringLiteral("ruleOrdinalHighWater"), static_cast<qint64>(tables.shared.ruleOrdinalHighWater)},
            {QStringLiteral("shared"), tables.shared.toJson()}};
        if (!tables.platforms.empty()) {
            QJsonObject platforms;
            for (const auto& [platform, table] : tables.platforms) platforms.insert(platformKey(platform), table.toJson());
            value.insert(QStringLiteral("platforms"), platforms);
        }
        result.insert(QLatin1String(key), value);
    }
    return result;
}

std::optional<SelectionData> SelectionData::fromJson(const QJsonObject& json, QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<SelectionData> {
        if (error) *error = message;
        return std::nullopt;
    };
    SelectionData parsed;
    if (json.size() != 6 || !parseAxes(json.value(QStringLiteral("matchAxes")), parsed.matchAxes)
        || !parseMixingRows(json.value(QStringLiteral("blendAxes")), parsed.blendAxes))
        return fail(QStringLiteral("Selections must contain matchAxes, blendAxes, random, sequence, match and blend"));
    for (const auto& [key, mode] : kModes) {
        const auto value = json.value(QLatin1String(key)).toObject();
        const auto platforms = value.value(QStringLiteral("platforms"));
        if (!json.value(QLatin1String(key)).isObject() || value.size() != (platforms.isUndefined() ? 3 : 4)
            || !value.value(QStringLiteral("shared")).isObject()
            || !unsignedInteger(value.value(QStringLiteral("occurrenceOrdinalHighWater")), 4294967296.0)
            || !unsignedInteger(value.value(QStringLiteral("ruleOrdinalHighWater")), 4294967296.0)
            || (!platforms.isUndefined() && (!platforms.isObject() || platforms.toObject().isEmpty())))
            return fail(QStringLiteral("Selection mode %1 must contain shared, platforms and identity allocation ranges")
                .arg(QLatin1String(key)));
        const auto occurrences = static_cast<uint64_t>(value.value(QStringLiteral("occurrenceOrdinalHighWater")).toDouble());
        const auto rules = static_cast<uint64_t>(value.value(QStringLiteral("ruleOrdinalHighWater")).toDouble());
        auto& tables = parsed.editModeTables(mode);
        auto shared = SelectionTable::fromJson(value.value(QStringLiteral("shared")).toObject(), occurrences, rules,
                                               parsed.matchAxes, parsed.blendAxes, error);
        if (!shared) return std::nullopt;
        tables.shared = std::move(*shared);
        const auto scopes = platforms.toObject();
        for (auto it = scopes.begin(); it != scopes.end(); ++it) {
            QUuid platform;
            if (!parsePlatformKey(it.key(), platform) || !it.value().isObject())
                return fail(QStringLiteral("Selection platform table is invalid"));
            auto table = SelectionTable::fromJson(it.value().toObject(), occurrences, rules,
                                                  parsed.matchAxes, parsed.blendAxes, error);
            if (!table) return std::nullopt;
            tables.platforms.emplace(platform, std::move(*table));
        }
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
    const auto& table = selections.table(mode, scope);
    if (mode == SelectionMode::Random || mode == SelectionMode::Sequence)
        return table.occurrenceRows();
    auto result = table.memberRows();
    std::erase_if(result, [&](const auto& row) {
        const auto* entry = table.member(row.childGuid);
        if (mode != SelectionMode::Match) return entry->curves.empty();
        return std::none_of(table.matchRows.begin(), table.matchRows.end(),
            [&](const auto& matchRow) { return matchRow.target == row.childGuid; });
    });
    return result;
}

}
