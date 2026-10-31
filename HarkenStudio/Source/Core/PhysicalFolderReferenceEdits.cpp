#include "PhysicalFolderReferenceEdits.h"


#include <QJsonArray>

#include <functional>

namespace VNX {
namespace {

bool deleted(const QJsonValue& value, const QSet<QUuid>& guids)
{
    const QUuid guid = QUuid::fromString(value.isObject()
        ? value.toObject().value(QStringLiteral("id")).toString() : value.toString());
    return !guid.isNull() && guids.contains(guid);
}

QJsonArray filtered(const QJsonArray& array, const std::function<bool(const QJsonValue&)>& remove)
{
    QJsonArray result;
    for (const auto& value : array)
        if (!remove(value)) result.append(value);
    return result;
}

void editObject(QJsonObject& object, const QString& key,
                const std::function<void(QJsonObject&)>& edit)
{
    if (!object.value(key).isObject()) return;
    auto child = object.value(key).toObject();
    edit(child);
    object.insert(key, child);
}

void editArray(QJsonObject& object, const QString& key,
               const std::function<void(QJsonObject&)>& edit)
{
    if (!object.value(key).isArray()) return;
    QJsonArray array;
    for (const auto& value : object.value(key).toArray()) {
        auto child = value.toObject();
        edit(child);
        array.append(child);
    }
    object.insert(key, array);
}

void cleanActions(QJsonObject& object, const QString& key, const QSet<QUuid>& guids)
{
    editArray(object, key, [&](QJsonObject& step) {
        if (deleted(step.value(QStringLiteral("targetRef")), guids))
            step.remove(QStringLiteral("targetRef"));
        editObject(step, QStringLiteral("quantize"), [&](QJsonObject& quantize) {
            if (deleted(quantize.value(QStringLiteral("referenceRef")), guids))
                quantize.remove(QStringLiteral("referenceRef"));
        });
    });
}

void cleanPlayable(QJsonObject& properties, const QSet<QUuid>& guids)
{
    if (deleted(properties.value(QStringLiteral("spatial.attenuationRef")), guids))
        properties.insert(QStringLiteral("spatial.attenuationRef"), QJsonValue::Null);
    editArray(properties, QStringLiteral("platformOverrides"), [&](QJsonObject& row) {
        if (deleted(row.value(QStringLiteral("spatial.attenuationRef")), guids))
            row.insert(QStringLiteral("spatial.attenuationRef"), QJsonValue::Null);
    });
}

void cleanSelectionTable(QJsonObject& table, const QSet<QUuid>& guids)
{
    const auto axes = table.value(QStringLiteral("matchAxes")).toArray();
    QList<int> removedAxes;
    for (int i = 0; i < axes.size(); ++i)
        if (deleted(axes.at(i), guids)) removedAxes.append(i);
    if (!removedAxes.isEmpty())
        table.insert(QStringLiteral("matchAxes"), filtered(axes, [&](const auto& row) {
            return deleted(row, guids);
        }));
    editArray(table, QStringLiteral("blendAxes"), [&](QJsonObject& axis) {
        if (deleted(axis.value(QStringLiteral("variable")), guids))
            axis.insert(QStringLiteral("variable"), QJsonValue::Null);
    });
    auto entries = filtered(table.value(QStringLiteral("entries")).toArray(), [&](const auto& row) {
        return deleted(row.toObject().value(QStringLiteral("childGuid")), guids);
    });
    for (int i = 0; i < entries.size(); ++i) {
        auto entry = entries.at(i).toObject();
        if (!removedAxes.isEmpty()) {
            editArray(entry, QStringLiteral("conditions"), [&](QJsonObject& condition) {
                auto cells = condition.value(QStringLiteral("cells")).toArray();
                for (auto it = removedAxes.crbegin(); it != removedAxes.crend(); ++it)
                    if (*it < cells.size()) cells.removeAt(*it);
                condition.insert(QStringLiteral("cells"), cells);
            });
        }
        entries.replace(i, entry);
    }
    if (table.contains(QStringLiteral("entries")))
        table.insert(QStringLiteral("entries"), entries);
}

QJsonValue rowIdentity(const QJsonValue& value)
{
    if (!value.isObject()) return QJsonValue::Undefined;
    const auto object = value.toObject();
    for (const auto* key : {"guid", "clipGuid", "stableId", "stableMemberOrdinal", "stableRuleOrdinal", "blendGuid"}) {
        const QLatin1String field(key);
        if (object.contains(field)) return QJsonArray{QString(field), object.value(field)};
    }
    return QJsonValue::Undefined;
}

bool sameRow(const QJsonValue& left, const QJsonValue& right)
{
    const auto identity = rowIdentity(left);
    return left == right || (!identity.isUndefined() && identity == rowIdentity(right));
}

bool matchesUnchangedFields(const QJsonValue& current, const QJsonValue& before,
                            const QJsonValue& after)
{
    if (before == after) return current == before;
    if (!before.isObject() || !after.isObject()) return true;
    if (!current.isObject()) return false;
    const auto original = before.toObject();
    const auto replacement = after.toObject();
    const auto candidate = current.toObject();
    QSet<QString> keys;
    for (auto it = original.begin(); it != original.end(); ++it) keys.insert(it.key());
    for (auto it = replacement.begin(); it != replacement.end(); ++it) keys.insert(it.key());
    for (const auto& key : keys)
        if (!matchesUnchangedFields(candidate.value(key), original.value(key), replacement.value(key))) return false;
    return true;
}

int matchingRow(const QJsonArray& array, const QJsonValue& before, const QJsonValue& after)
{
    const auto identity = rowIdentity(before);
    int match = -1;
    for (int i = 0; i < array.size(); ++i) {
        const bool matches = !identity.isUndefined()
            ? rowIdentity(array.at(i)) == identity
            : before.isObject() && after.isObject()
                ? matchesUnchangedFields(array.at(i), before, after)
                : array.at(i) == before || array.at(i) == after;
        if (!matches) continue;
        if (match >= 0) return -1;
        match = i;
    }
    return match;
}

bool mergeChange(QJsonValue& current, const QJsonValue& expected, const QJsonValue& replacement)
{
    if (expected == replacement || current == replacement) return true;
    if (current == expected) {
        current = replacement;
        return true;
    }
    if (expected.isObject() && replacement.isObject() && current.isObject()) {
        auto result = current.toObject();
        const auto before = expected.toObject();
        const auto after = replacement.toObject();
        QSet<QString> keys;
        for (auto it = before.begin(); it != before.end(); ++it) keys.insert(it.key());
        for (auto it = after.begin(); it != after.end(); ++it) keys.insert(it.key());
        for (const auto& key : keys) {
            auto value = result.value(key);
            if (!mergeChange(value, before.value(key), after.value(key))) return false;
            if (value.isUndefined()) result.remove(key);
            else result.insert(key, value);
        }
        current = result;
        return true;
    }
    if (!expected.isArray() || !replacement.isArray() || !current.isArray()) return false;
    auto result = current.toArray();
    const auto before = expected.toArray();
    const auto after = replacement.toArray();
    if (before.size() == after.size()) {
        QSet<int> matched;
        for (int i = 0; i < before.size(); ++i) {
            if (before.at(i) == after.at(i)) continue;
            const int index = matchingRow(result, before.at(i), after.at(i));
            if (index < 0 || matched.contains(index)) return false;
            auto row = result.at(index);
            if (!mergeChange(row, before.at(i), after.at(i))) return false;
            result.replace(index, row);
            matched.insert(index);
        }
    } else {
        const bool removing = before.size() > after.size();
        const auto& larger = removing ? before : after;
        const auto& smaller = removing ? after : before;
        int smallIndex = 0;
        int previous = -1;
        for (int i = 0; i < larger.size(); ++i) {
            const bool kept = smallIndex < smaller.size()
                && sameRow(larger.at(i), smaller.at(smallIndex));
            if (kept) {
                const auto oldRow = removing ? larger.at(i) : smaller.at(smallIndex);
                const auto newRow = removing ? smaller.at(smallIndex) : larger.at(i);
                const int index = matchingRow(result, oldRow, newRow);
                if (index < 0) return false;
                auto row = result.at(index);
                if (!mergeChange(row, oldRow, newRow)) return false;
                result.replace(index, row);
                previous = index;
                ++smallIndex;
            } else if (removing) {
                const int index = matchingRow(result, larger.at(i), larger.at(i));
                if (index < 0 || result.at(index) != larger.at(i)) return false;
                result.removeAt(index);
                if (previous >= index) --previous;
            } else {
                if (matchingRow(result, larger.at(i), larger.at(i)) >= 0) return false;
                int next = result.size();
                if (smallIndex < smaller.size()) {
                    next = matchingRow(result, smaller.at(smallIndex), smaller.at(smallIndex));
                    if (next < 0 || next <= previous) return false;
                }
                const int index = previous < 0 ? next : previous + 1;
                result.insert(index, larger.at(i));
                previous = index;
            }
        }
        if (smallIndex != smaller.size()) return false;
    }
    current = result;
    return true;
}

}

QJsonObject PhysicalFolderReferenceEdits::removeReferences(QJsonObject content,
                                                          const QSet<QUuid>& guids)
{
    cleanActions(content, QStringLiteral("actions"), guids);
    editObject(content, QStringLiteral("playableProperties"), [&](auto& properties) { cleanPlayable(properties, guids); });
    editObject(content, QStringLiteral("fieldGroups"), [&](QJsonObject& groups) {
        const auto cleanGroups = [&](QJsonObject& row) {
            editObject(row, QStringLiteral("spatialRating"), [&](QJsonObject& spatial) {
                editObject(spatial, QStringLiteral("values"), [&](QJsonObject& values) {
                    if (deleted(values.value(QStringLiteral("spatialAttenuationRef")), guids))
                        values.insert(QStringLiteral("spatialAttenuationRef"), QUuid{}.toString(QUuid::WithoutBraces));
                });
            });
        };
        const bool root = content.value(QStringLiteral("parentRef")).toObject()
            .value(QStringLiteral("kind")).toString() == QStringLiteral("root");
        const bool sharedLocal = !groups.value(QStringLiteral("shared")).toObject()
            .value(QStringLiteral("spatialRating")).toObject().value(QStringLiteral("inherit")).toBool(true);
        bool sharedActive = root || sharedLocal;
        editObject(groups, QStringLiteral("platforms"), [&](QJsonObject& platforms) {
            for (const auto& key : platforms.keys()) {
                const auto state = platforms.value(key).toObject().value(QStringLiteral("spatialRating")).toObject();
                const bool active = root || (state.contains(QStringLiteral("inherit"))
                    ? !state.value(QStringLiteral("inherit")).toBool() : sharedLocal);
                if (!active) continue;
                sharedActive = true;
                editObject(platforms, key, cleanGroups);
            }
        });
        if (sharedActive) editObject(groups, QStringLiteral("shared"), cleanGroups);
    });
    editObject(content, QStringLiteral("components"), [&](QJsonObject& components) {
        editObject(components, QStringLiteral("action"), [&](QJsonObject& action) {
            cleanActions(action, QStringLiteral("steps"), guids);
        });
        editObject(components, QStringLiteral("mixnode"), [&](QJsonObject& mix) {
            if (mix.contains(QStringLiteral("bindings")))
                mix.insert(QStringLiteral("bindings"), filtered(mix.value(QStringLiteral("bindings")).toArray(), [&](const auto& row) {
                    return deleted(row.toObject().value(QStringLiteral("variable")), guids);
                }));
        });
        editObject(components, QStringLiteral("tracktimeline"), [&](QJsonObject& track) {
            if (!track.contains(QStringLiteral("clips"))) return;
            track.insert(QStringLiteral("clips"), filtered(track.value(QStringLiteral("clips")).toArray(), [&](const auto& row) {
                return deleted(row.toObject().value(QStringLiteral("targetGuid")), guids);
            }));
            editArray(track, QStringLiteral("clips"), [&](QJsonObject& clip) {
                editObject(clip, QStringLiteral("playableProperties"), [&](auto& properties) { cleanPlayable(properties, guids); });
            });
        });
        editObject(components, QStringLiteral("selection"), [&](QJsonObject& selection) {
            editObject(selection, QStringLiteral("selections"), [&](QJsonObject& selections) {
                editObject(selections, QStringLiteral("shared"), [&](auto& table) { cleanSelectionTable(table, guids); });
                editObject(selections, QStringLiteral("platforms"), [&](QJsonObject& platforms) {
                    for (const auto& key : platforms.keys())
                        editObject(platforms, key, [&](auto& table) { cleanSelectionTable(table, guids); });
                });
            });
            editObject(selection, QStringLiteral("transitions"), [&](QJsonObject& transitions) {
                transitions.insert(QStringLiteral("rules"), filtered(transitions.value(QStringLiteral("rules")).toArray(), [&](const auto& row) {
                    const auto rule = row.toObject();
                    return deleted(rule.value(QStringLiteral("source")), guids)
                        || deleted(rule.value(QStringLiteral("target")), guids);
                }));
                editArray(transitions, QStringLiteral("rules"), [&](QJsonObject& rule) {
                    if (deleted(rule.value(QStringLiteral("bridge")), guids))
                        rule.insert(QStringLiteral("bridge"), QJsonValue::Null);
                });
            });
        });
    });
    editArray(content, QStringLiteral("children"), [&](auto& child) { child = removeReferences(child, guids); });
    if (content.contains(QStringLiteral("actionPackTargeting")))
        content.insert(QStringLiteral("actionPackTargeting"), filtered(content.value(QStringLiteral("actionPackTargeting")).toArray(), [&](const auto& row) {
            return deleted(row.toObject().value(QStringLiteral("stableId")), guids);
        }));
    return content;
}

bool PhysicalFolderReferenceEdits::mergeReferences(QJsonObject& current,
    const QJsonObject& expected, const QJsonObject& replacement)
{
    QJsonValue merged = current;
    if (!mergeChange(merged, expected, replacement)) return false;
    current = merged.toObject();
    return true;
}

}
