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

QJsonValue cellJson(const std::optional<DiscreteGridCell>& cell)
{
    if (!cell) return QJsonValue::Null;
    if (!cell->labelId.isNull())
        return QJsonObject{{QStringLiteral("labelId"), cell->labelId.toString(QUuid::WithoutBraces)}};
    return cell->legacyValue;
}

QJsonArray cellsJson(const std::vector<std::optional<DiscreteGridCell>>& cells)
{
    QJsonArray result;
    for (const auto& cell : cells) result.append(cellJson(cell));
    return result;
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

QJsonObject shapeJson(const QVector<CurvePointDef>& points, const QVector<CurveHandlePairDef>& handles)
{
    QJsonArray pointValues;
    QJsonArray handleValues;
    for (const auto& point : points) pointValues.append(QJsonArray{point.x, point.value});
    for (const auto& handle : handles)
        handleValues.append(QJsonArray{handle.h1x, handle.h1y, handle.h2x, handle.h2y});
    return {{QStringLiteral("points"), pointValues}, {QStringLiteral("handles"), handleValues}};
}

std::optional<SelectionCurveShape> parseShape(const QUuid& rowGuid, const QUuid& variable, const QUuid& target,
                                              const QJsonValue& points, const QJsonValue& handles)
{
    const auto mixing = MixingDef::fromJson(QJsonArray{QJsonObject{
        {QStringLiteral("guid"), rowGuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("variable"), variable.isNull() ? QJsonValue(QJsonValue::Null)
            : QJsonValue(variable.toString(QUuid::WithoutBraces))},
        {QStringLiteral("children"), QJsonArray{QJsonObject{
            {QStringLiteral("target"), target.toString(QUuid::WithoutBraces)},
            {QStringLiteral("points"), points}, {QStringLiteral("handles"), handles}}}}}});
    if (!mixing) return std::nullopt;
    const auto& child = mixing->rows.front().children.front();
    return SelectionCurveShape{child.points, child.handles};
}

QJsonObject curveJson(const SelectionCurve& curve)
{
    auto result = shapeJson(curve.points, curve.handles);
    result.insert(QStringLiteral("blendGuid"), curve.rowGuid.toString(QUuid::WithoutBraces));
    result.insert(QStringLiteral("order"), static_cast<qint64>(curve.order));
    if (curve.platforms.empty()) return result;
    QJsonObject platforms;
    for (const auto& [platform, shape] : curve.platforms)
        platforms.insert(platformKey(platform), shapeJson(shape.points, shape.handles));
    result.insert(QStringLiteral("platforms"), platforms);
    return result;
}

int axisIndex(const std::vector<QUuid>& axes, const QUuid& axis)
{
    const auto found = std::find(axes.begin(), axes.end(), axis);
    return found == axes.end() ? -1 : static_cast<int>(std::distance(axes.begin(), found));
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

std::vector<SelectionEntry> SelectionTable::occurrenceRows(PlatformScope scope) const
{
    std::vector<std::pair<uint32_t, SelectionEntry>> ordered;
    for (const auto& entry : entries)
        for (const auto& occurrence : entry.occurrences) {
            const auto platform = scope ? occurrence.platformWeights.find(*scope) : occurrence.platformWeights.end();
            ordered.push_back({occurrence.order, {entry.childGuid, occurrence.stableOccurrenceOrdinal,
                platform == occurrence.platformWeights.end() ? occurrence.weight : platform->second}});
        }
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
    std::map<uint32_t, std::map<QUuid, int>> platformWeights;
    for (const auto& entry : entries)
        for (const auto& occurrence : entry.occurrences)
            if (!occurrence.platformWeights.empty())
                platformWeights.emplace(occurrence.stableOccurrenceOrdinal, occurrence.platformWeights);
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
        const auto platform = platformWeights.find(row.stableOccurrenceOrdinal);
        entry->occurrences.push_back({row.stableOccurrenceOrdinal, static_cast<uint32_t>(index), row.weight,
            platform == platformWeights.end() ? std::map<QUuid, int>{} : platform->second});
        next.occurrenceOrdinalHighWater = std::max(next.occurrenceOrdinalHighWater,
            static_cast<uint64_t>(row.stableOccurrenceOrdinal) + 1);
    }
    next.removeMembers([](const auto& entry) { return entry.occurrences.empty(); });
    *this = std::move(next);
    return true;
}

bool SelectionTable::editOccurrenceRows(const std::vector<SelectionEntry>& edited, PlatformScope displayed,
                                        PlatformScope target)
{
    const auto shown = occurrenceRows(displayed);
    std::vector<SelectionEntry> rows;
    std::vector<std::pair<uint32_t, int>> changed;
    for (auto row : edited) {
        const auto* authored = find(row.stableOccurrenceOrdinal);
        const auto before = std::find_if(shown.begin(), shown.end(), [&](const SelectionEntry& entry) {
            return entry.stableOccurrenceOrdinal == row.stableOccurrenceOrdinal;
        });
        if (authored && before != shown.end() && row.weight != before->weight) {
            changed.push_back({row.stableOccurrenceOrdinal, row.weight});
            if (target) row.weight = authored->weight;
        } else if (authored && before != shown.end()) {
            row.weight = authored->weight;
        }
        rows.push_back(row);
    }
    SelectionTable next = *this;
    if (!next.setOccurrenceRows(rows)) return false;
    for (const auto& [ordinal, weight] : changed) {
        auto* occurrence = next.find(ordinal);
        if (target) occurrence->platformWeights[*target] = weight;
        else occurrence->platformWeights.clear();
    }
    *this = std::move(next);
    return true;
}

void SelectionTable::removeMembers(const std::function<bool(const SelectionMember&)>& predicate)
{
    std::erase_if(entries, predicate);
    for (auto& row : matchRows)
        if (!row.target.isNull() && !member(row.target)) row.target = QUuid{};
}

DiscreteGridDef SelectionTable::matchGrid(PlatformScope scope) const
{
    DiscreteGridDef result;
    result.axes = matchAxes;
    for (const auto& row : matchRows) {
        auto cells = row.cells;
        const auto platform = scope ? row.platformCells.find(*scope) : row.platformCells.end();
        if (platform != row.platformCells.end())
            for (const auto& [axis, cell] : platform->second)
                if (const int index = axisIndex(matchAxes, axis); index >= 0) cells[static_cast<size_t>(index)] = cell;
        result.rows.push_back({row.target, cells, row.stableRuleOrdinal});
    }
    return result;
}

bool SelectionTable::setMatchGrid(const DiscreteGridDef& grid)
{
    std::map<uint32_t, std::map<QUuid, std::map<QUuid, std::optional<DiscreteGridCell>>>> platformCells;
    for (const auto& row : matchRows)
        if (!row.platformCells.empty()) platformCells.emplace(row.stableRuleOrdinal, row.platformCells);
    SelectionTable next = *this;
    std::vector<QUuid> axes;
    if (!parseAxes(axisJson(grid.axes), axes)) return false;
    next.matchAxes = std::move(axes);
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
        SelectionMatchRow rule{ordinal, row.target, row.cells, {}};
        if (const auto saved = platformCells.find(ordinal); saved != platformCells.end())
            for (const auto& [platform, cells] : saved->second)
                for (const auto& [axis, cell] : cells)
                    if (axisIndex(next.matchAxes, axis) >= 0) rule.platformCells[platform][axis] = cell;
        next.matchRows.push_back(std::move(rule));
    }
    next.removeMembers([&](const SelectionMember& member) {
        return std::none_of(next.matchRows.begin(), next.matchRows.end(),
            [&](const SelectionMatchRow& row) { return row.target == member.childGuid; });
    });
    *this = std::move(next);
    return true;
}

bool SelectionTable::editMatchGrid(const DiscreteGridDef& edited, PlatformScope displayed, PlatformScope target)
{
    struct Change { uint32_t ordinal; QUuid axis; std::optional<DiscreteGridCell> cell; };
    const auto shown = matchGrid(displayed);
    auto shared = edited;
    std::vector<Change> changes;
    for (auto& row : shared.rows) {
        if (!row.stableRuleOrdinal) continue;
        const auto authored = std::find_if(matchRows.begin(), matchRows.end(),
            [&](const SelectionMatchRow& rule) { return rule.stableRuleOrdinal == *row.stableRuleOrdinal; });
        const auto before = std::find_if(shown.rows.begin(), shown.rows.end(),
            [&](const auto& rule) { return rule.stableRuleOrdinal == row.stableRuleOrdinal; });
        if (authored == matchRows.end() || before == shown.rows.end()) continue;
        for (size_t index = 0; index < shared.axes.size(); ++index) {
            const int shownIndex = axisIndex(shown.axes, shared.axes[index]);
            const int authoredIndex = axisIndex(matchAxes, shared.axes[index]);
            if (shownIndex < 0 || authoredIndex < 0) continue;
            const auto& previous = authored->cells[static_cast<size_t>(authoredIndex)];
            if (row.cells[index] == before->cells[static_cast<size_t>(shownIndex)]) {
                row.cells[index] = previous;
                continue;
            }
            changes.push_back({*row.stableRuleOrdinal, shared.axes[index], row.cells[index]});
            if (target) row.cells[index] = previous;
        }
    }
    SelectionTable next = *this;
    if (!next.setMatchGrid(shared)) return false;
    for (const auto& change : changes) {
        auto& rule = *std::find_if(next.matchRows.begin(), next.matchRows.end(),
            [&](const SelectionMatchRow& row) { return row.stableRuleOrdinal == change.ordinal; });
        if (target) {
            rule.platformCells[*target][change.axis] = change.cell;
            continue;
        }
        for (auto& [platform, cells] : rule.platformCells) cells.erase(change.axis);
        std::erase_if(rule.platformCells, [](const auto& entry) { return entry.second.empty(); });
    }
    *this = std::move(next);
    return true;
}

void SelectionTable::remapMatchAxes(const QHash<QUuid, QUuid>& guids)
{
    for (auto& axis : matchAxes) axis = guids.value(axis, axis);
    for (auto& row : matchRows)
        for (auto& [platform, cells] : row.platformCells) {
            std::map<QUuid, std::optional<DiscreteGridCell>> mapped;
            for (const auto& [axis, cell] : cells) mapped.emplace(guids.value(axis, axis), cell);
            cells = std::move(mapped);
        }
}

MixingDef SelectionTable::mixing(PlatformScope scope) const
{
    MixingDef result;
    for (const auto& mixingRow : mixingRows) {
        MixingRowDef row{mixingRow.variable, {}, mixingRow.guid};
        std::vector<std::pair<uint32_t, MixingChildDef>> ordered;
        for (const auto& entry : entries)
            for (const auto& curve : entry.curves) {
                if (curve.rowGuid != mixingRow.guid) continue;
                const auto platform = scope ? curve.platforms.find(*scope) : curve.platforms.end();
                ordered.push_back({curve.order, platform == curve.platforms.end()
                    ? MixingChildDef{entry.childGuid, curve.points, curve.handles}
                    : MixingChildDef{entry.childGuid, platform->second.points, platform->second.handles}});
            }
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
    std::map<std::pair<QUuid, QUuid>, std::map<QUuid, SelectionCurveShape>> platforms;
    for (const auto& entry : entries)
        for (const auto& curve : entry.curves)
            if (!curve.platforms.empty()) platforms.emplace(std::pair{curve.rowGuid, entry.childGuid}, curve.platforms);
    SelectionTable next = *this;
    next.mixingRows.clear();
    for (auto& entry : next.entries) entry.curves.clear();
    for (const auto& row : mixing.rows) {
        next.mixingRows.push_back({row.guid, row.variable});
        for (size_t index = 0; index < row.children.size(); ++index) {
            const auto& child = row.children[index];
            auto* entry = ensureMember(next, child.target);
            if (!entry) return false;
            const auto saved = platforms.find({row.guid, child.target});
            entry->curves.push_back({row.guid, child.points, child.handles, static_cast<uint32_t>(index),
                saved == platforms.end() ? std::map<QUuid, SelectionCurveShape>{} : saved->second});
        }
    }
    next.removeMembers([](const SelectionMember& member) { return member.curves.empty(); });
    *this = std::move(next);
    return true;
}

bool SelectionTable::editMixing(const MixingDef& edited, PlatformScope displayed, PlatformScope target)
{
    struct Change { QUuid row; QUuid child; SelectionCurveShape shape; };
    const auto shown = mixing(displayed);
    auto shared = edited;
    std::vector<Change> changes;
    for (auto& row : shared.rows) {
        const int shownRow = shown.rowIndex(row.guid);
        if (shownRow < 0) continue;
        const auto& shownChildren = shown.rows[static_cast<size_t>(shownRow)].children;
        for (auto& child : row.children) {
            const auto before = std::find_if(shownChildren.begin(), shownChildren.end(),
                [&](const MixingChildDef& candidate) { return candidate.target == child.target; });
            const auto* entry = member(child.target);
            const auto authored = entry ? std::find_if(entry->curves.begin(), entry->curves.end(),
                [&](const SelectionCurve& curve) { return curve.rowGuid == row.guid; }) : std::vector<SelectionCurve>::const_iterator{};
            if (before == shownChildren.end() || !entry || authored == entry->curves.end()) continue;
            if (child.points == before->points && child.handles == before->handles) {
                child.points = authored->points;
                child.handles = authored->handles;
                continue;
            }
            changes.push_back({row.guid, child.target, {child.points, child.handles}});
            if (!target) continue;
            child.points = authored->points;
            child.handles = authored->handles;
        }
    }
    SelectionTable next = *this;
    if (!next.setMixing(shared)) return false;
    for (const auto& change : changes) {
        auto& curves = next.member(change.child)->curves;
        auto& curve = *std::find_if(curves.begin(), curves.end(),
            [&](const SelectionCurve& candidate) { return candidate.rowGuid == change.row; });
        if (target) curve.platforms[*target] = change.shape;
        else curve.platforms.clear();
    }
    *this = std::move(next);
    return true;
}

QSet<QUuid> SelectionTable::platformIds() const
{
    QSet<QUuid> result;
    for (const auto& entry : entries) {
        for (const auto& occurrence : entry.occurrences)
            for (const auto& [platform, weight] : occurrence.platformWeights) result.insert(platform);
        for (const auto& curve : entry.curves)
            for (const auto& [platform, shape] : curve.platforms) result.insert(platform);
    }
    for (const auto& row : matchRows)
        for (const auto& [platform, cells] : row.platformCells) result.insert(platform);
    return result;
}

void SelectionTable::removePlatform(const QUuid& platform)
{
    for (auto& entry : entries) {
        for (auto& occurrence : entry.occurrences) occurrence.platformWeights.erase(platform);
        for (auto& curve : entry.curves) curve.platforms.erase(platform);
    }
    for (auto& row : matchRows) row.platformCells.erase(platform);
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
        QJsonArray curves;
        for (const auto& occurrence : entry.occurrences) {
            QJsonObject row{
                {QStringLiteral("stableOccurrenceOrdinal"), static_cast<qint64>(occurrence.stableOccurrenceOrdinal)},
                {QStringLiteral("order"), static_cast<qint64>(occurrence.order)},
                {QStringLiteral("weight"), occurrence.weight}};
            if (!occurrence.platformWeights.empty()) {
                QJsonObject platforms;
                for (const auto& [platform, weight] : occurrence.platformWeights)
                    platforms.insert(platformKey(platform), weight);
                row.insert(QStringLiteral("platforms"), platforms);
            }
            occurrences.append(row);
        }
        for (const auto& curve : entry.curves) curves.append(curveJson(curve));
        values.append(QJsonObject{
            {QStringLiteral("childGuid"), entry.childGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("stableMemberOrdinal"), static_cast<qint64>(entry.stableMemberOrdinal)},
            {QStringLiteral("occurrences"), occurrences}, {QStringLiteral("curves"), curves}});
    }
    QJsonArray rows;
    for (const auto& row : matchRows) {
        QJsonObject rule{
            {QStringLiteral("stableRuleOrdinal"), static_cast<qint64>(row.stableRuleOrdinal)},
            {QStringLiteral("target"), row.target.isNull() ? QJsonValue(QJsonValue::Null)
                : QJsonValue(row.target.toString(QUuid::WithoutBraces))},
            {QStringLiteral("cells"), cellsJson(row.cells)}};
        if (!row.platformCells.empty()) {
            QJsonObject platforms;
            for (const auto& [platform, cells] : row.platformCells) {
                QJsonObject values;
                for (const auto& [axis, cell] : cells) values.insert(axis.toString(QUuid::WithoutBraces), cellJson(cell));
                platforms.insert(platformKey(platform), values);
            }
            rule.insert(QStringLiteral("platforms"), platforms);
        }
        rows.append(rule);
    }
    return {{QStringLiteral("occurrenceOrdinalHighWater"), static_cast<qint64>(occurrenceOrdinalHighWater)},
            {QStringLiteral("ruleOrdinalHighWater"), static_cast<qint64>(ruleOrdinalHighWater)},
            {QStringLiteral("entries"), values}, {QStringLiteral("matchAxes"), axisJson(matchAxes)},
            {QStringLiteral("matchRows"), rows}, {QStringLiteral("blendAxes"), mixingRowsJson(mixingRows)}};
}

std::optional<SelectionTable> SelectionTable::fromJson(const QJsonObject& json, QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<SelectionTable> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (json.size() != 6 || !json.value(QStringLiteral("entries")).isArray()
        || !json.value(QStringLiteral("matchRows")).isArray()
        || !unsignedInteger(json.value(QStringLiteral("occurrenceOrdinalHighWater")), 4294967296.0)
        || !unsignedInteger(json.value(QStringLiteral("ruleOrdinalHighWater")), 4294967296.0))
        return fail(QStringLiteral("Selection table must contain entries, matchRows, axes and identity allocation ranges"));
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
            const auto platforms = row.value(QStringLiteral("platforms"));
            if (!source.isObject() || row.size() != (platforms.isUndefined() ? 3 : 4)
                || !unsignedInteger(row.value(QStringLiteral("stableOccurrenceOrdinal")))
                || !unsignedInteger(row.value(QStringLiteral("order")))
                || !unsignedInteger(row.value(QStringLiteral("weight")), 100.0)
                || (!platforms.isUndefined() && (!platforms.isObject() || platforms.toObject().isEmpty())))
                return fail(QStringLiteral("Selection occurrence is invalid"));
            SelectionOccurrence occurrence{
                static_cast<uint32_t>(row.value(QStringLiteral("stableOccurrenceOrdinal")).toDouble()),
                static_cast<uint32_t>(row.value(QStringLiteral("order")).toDouble()),
                row.value(QStringLiteral("weight")).toInt(), {}};
            const auto weights = platforms.toObject();
            for (auto it = weights.begin(); it != weights.end(); ++it) {
                QUuid platform;
                if (!parsePlatformKey(it.key(), platform) || !unsignedInteger(it.value(), 100.0))
                    return fail(QStringLiteral("Selection platform weight is invalid"));
                occurrence.platformWeights.emplace(platform, it.value().toInt());
            }
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
            const auto platforms = curve.value(QStringLiteral("platforms"));
            if (!source.isObject() || curve.size() != (platforms.isUndefined() ? 4 : 5)
                || !unsignedInteger(curve.value(QStringLiteral("order")))
                || (!platforms.isUndefined() && (!platforms.isObject() || platforms.toObject().isEmpty())))
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
            SelectionCurve parsedCurve{mixingRow->guid, child.points, child.handles, order, {}};
            const auto shapes = platforms.toObject();
            for (auto it = shapes.begin(); it != shapes.end(); ++it) {
                QUuid platform;
                const auto values = it.value().toObject();
                const auto shape = parsePlatformKey(it.key(), platform) && it.value().isObject() && values.size() == 2
                    ? parseShape(mixingRow->guid, variable, entry.childGuid, values.value(QStringLiteral("points")),
                                 values.value(QStringLiteral("handles")))
                    : std::nullopt;
                if (!shape) return fail(QStringLiteral("Selection platform curve is invalid"));
                parsedCurve.platforms.emplace(platform, *shape);
            }
            entry.curves.push_back(std::move(parsedCurve));
        }
        parsed.entries.push_back(std::move(entry));
    }
    for (const auto& source : json.value(QStringLiteral("matchRows")).toArray()) {
        const auto object = source.toObject();
        const auto target = object.value(QStringLiteral("target"));
        const auto cells = object.value(QStringLiteral("cells"));
        const auto platforms = object.value(QStringLiteral("platforms"));
        if (!source.isObject() || object.size() != (platforms.isUndefined() ? 3 : 4)
            || !unsignedInteger(object.value(QStringLiteral("stableRuleOrdinal")))
            || !cells.isArray()
            || cells.toArray().size() != static_cast<qsizetype>(parsed.matchAxes.size())
            || (!platforms.isUndefined() && (!platforms.isObject() || platforms.toObject().isEmpty())))
            return fail(QStringLiteral("Selection match row is invalid"));
        SelectionMatchRow row{
            static_cast<uint32_t>(object.value(QStringLiteral("stableRuleOrdinal")).toDouble()),
            QUuid(target.toString()), {}, {}};
        const auto platformValues = platforms.toObject();
        for (auto it = platformValues.begin(); it != platformValues.end(); ++it) {
            QUuid platform;
            const auto values = it.value().toObject();
            if (!parsePlatformKey(it.key(), platform) || !it.value().isObject() || values.isEmpty())
                return fail(QStringLiteral("Selection platform conditions are invalid"));
            for (auto cell = values.begin(); cell != values.end(); ++cell) {
                const QUuid axis(cell.key());
                std::optional<DiscreteGridCell> parsedCell;
                if (axisIndex(parsed.matchAxes, axis) < 0 || !parseCell(cell.value(), parsedCell))
                    return fail(QStringLiteral("Selection platform condition is invalid"));
                row.platformCells[platform][axis] = parsedCell;
            }
        }
        if (!target.isNull() && (!target.isString() || !targets.contains(row.target)))
            return fail(QStringLiteral("Selection match row target must be null or a Selection member"));
        if (row.stableRuleOrdinal >= parsed.ruleOrdinalHighWater || ruleIds.contains(row.stableRuleOrdinal))
            return fail(QStringLiteral("Selection match row ordinal is outside its allocation range or reused"));
        ruleIds.insert(row.stableRuleOrdinal);
        for (const auto& cell : cells.toArray()) {
            std::optional<DiscreteGridCell> parsedCell;
            if (!parseCell(cell, parsedCell)) return fail(QStringLiteral("Selection match row cell is invalid"));
            row.cells.push_back(parsedCell);
        }
        parsed.matchRows.push_back(std::move(row));
    }
    return parsed;
}

const SelectionTable& SelectionData::table(SelectionMode mode) const
{
    switch (mode) {
    case SelectionMode::Sequence: return sequence;
    case SelectionMode::Match: return match;
    case SelectionMode::Blend: return blend;
    case SelectionMode::Random: break;
    }
    return random;
}

SelectionTable& SelectionData::editTable(SelectionMode mode)
{
    return const_cast<SelectionTable&>(std::as_const(*this).table(mode));
}

QList<const SelectionTable*> SelectionData::tables() const
{
    return {&random, &sequence, &match, &blend};
}

QList<SelectionTable*> SelectionData::tables()
{
    return {&random, &sequence, &match, &blend};
}

QSet<QUuid> SelectionData::platformIds() const
{
    QSet<QUuid> result;
    for (const auto* table : tables()) result.unite(table->platformIds());
    return result;
}

QJsonObject SelectionData::toJson() const
{
    return {{QStringLiteral("random"), random.toJson()}, {QStringLiteral("sequence"), sequence.toJson()},
            {QStringLiteral("match"), match.toJson()}, {QStringLiteral("blend"), blend.toJson()}};
}

std::optional<SelectionData> SelectionData::fromJson(const QJsonObject& json, QString* error)
{
    SelectionData parsed;
    const std::pair<const char*, SelectionTable*> modes[] = {
        {"random", &parsed.random}, {"sequence", &parsed.sequence}, {"match", &parsed.match}, {"blend", &parsed.blend}};
    if (json.size() != 4) {
        if (error) *error = QStringLiteral("Selections must contain random, sequence, match and blend");
        return std::nullopt;
    }
    for (const auto& [key, table] : modes) {
        const auto value = json.value(QLatin1String(key));
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("Selections must contain random, sequence, match and blend");
            return std::nullopt;
        }
        auto parsedTable = SelectionTable::fromJson(value.toObject(), error);
        if (!parsedTable) return std::nullopt;
        *table = std::move(*parsedTable);
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
    const auto& table = selections.table(mode);
    if (mode == SelectionMode::Random || mode == SelectionMode::Sequence)
        return table.occurrenceRows(scope);
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
