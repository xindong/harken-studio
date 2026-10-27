#include "ContainerBehaviorSemantics.h"

#include "Components/VSelection.h"

#include <harken_studio_build/World.h>

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX::ContainerBehaviorSemantics {

namespace {

bool shuffleMode(const VSelection& selection)
{
    return selection.mode == SelectionMode::Random
        && (selection.isQueue() || selection.behavior.discipline == RandomDiscipline::Shuffle);
}

uint64_t nextEntropy(uint64_t& state)
{
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

int weightedIndex(const VSelection& selection, const QList<uint32_t>& candidates,
                  uint64_t& entropy, PlatformScope scope)
{
    uint64_t total = 0;
    for (uint32_t occurrence : candidates)
        total += static_cast<uint64_t>(
            effectiveChildWeight(selection, occurrence, scope));
    if (total == 0) return 0;

    uint64_t pick = nextEntropy(entropy) % total;
    for (int index = 0; index < candidates.size(); ++index) {
        const uint64_t weight = static_cast<uint64_t>(
            effectiveChildWeight(selection, candidates[index], scope));
        if (pick < weight) return index;
        pick -= weight;
    }
    return candidates.size() - 1;
}

void trimHistory(std::vector<uint32_t>& history, int count)
{
    if (count <= 0) {
        history.clear();
        return;
    }
    if (history.size() > static_cast<size_t>(count))
        history.erase(history.begin(), history.end() - count);
}

QList<uint32_t> eligibleDrawChildren(
    const VSelection& selection,
    const QList<uint32_t>& occurrenceOrdinals,
    const std::vector<uint32_t>& history, PlatformScope scope)
{
    QList<uint32_t> positive;
    QList<uint32_t> eligible;
    for (uint32_t occurrence : occurrenceOrdinals) {
        if (effectiveChildWeight(selection, occurrence, scope) <= 0)
            continue;
        positive.push_back(occurrence);
        if (std::find(history.begin(), history.end(), occurrence) == history.end())
            eligible.push_back(occurrence);
    }
    return eligible.isEmpty() ? positive : eligible;
}

int maxAvoidRepeatFor(const VSelection& selection,
                      const QList<uint32_t>& occurrenceOrdinals, PlatformScope scope)
{
    const int eligibleCount = selection.mode == SelectionMode::Random && !selection.isQueue()
            && selection.behavior.discipline == RandomDiscipline::Draw
        ? static_cast<int>(std::count_if(
            occurrenceOrdinals.begin(), occurrenceOrdinals.end(),
            [&](uint32_t ordinal) { return effectiveChildWeight(selection, ordinal, scope) > 0; }))
        : static_cast<int>(occurrenceOrdinals.size());
    constexpr int limit = static_cast<int>(
        harkenstudiobuild::kHardSelectionAvoidCapacity);
    return std::clamp(eligibleCount - 1, 0, limit);
}

std::vector<uint32_t> makeShuffleOrder(
    const VSelection& selection,
    const QList<uint32_t>& occurrenceOrdinals,
    uint64_t& entropy,
    const std::vector<uint32_t>& history, PlatformScope scope)
{
    QList<uint32_t> pool = occurrenceOrdinals;
    std::vector<uint32_t> result;
    result.reserve(static_cast<size_t>(pool.size()));

    while (!pool.isEmpty()) {
        QList<uint32_t> candidates = pool;
        if (result.empty() && !history.empty()) {
            const uint32_t previous = history.back();
            candidates.erase(std::remove(candidates.begin(), candidates.end(), previous),
                             candidates.end());
            if (candidates.isEmpty()) candidates = pool;
        }
        const int candidateIndex = weightedIndex(selection, candidates, entropy, scope);
        const uint32_t selected = candidates[candidateIndex];
        result.push_back(selected);
        pool.removeOne(selected);
    }
    return result;
}

}

Visibility visibilityFor(const VSelection& selection)
{
    Visibility result;
    switch (selection.mode) {
    case SelectionMode::Random:
        result.discipline = !selection.isQueue();
        result.childWeights = true;
        result.avoidRepeat = true;
        result.memoryScope = !selection.isQueue();
        result.resumeFromLast = selection.isQueue();
        break;
    case SelectionMode::Sequence:
        result.memoryScope = !selection.isQueue();
        result.resumeFromLast = selection.isQueue();
        break;
    case SelectionMode::Match:
    case SelectionMode::Blend:
        break;
    }
    return result;
}

QString childWeightLabel(const VSelection& selection)
{
    return shuffleMode(selection) ? QStringLiteral("Order") : QStringLiteral("Frequency");
}

int effectiveChildWeight(const VSelection& selection,
                         uint32_t stableOccurrenceOrdinal, PlatformScope scope)
{
    const auto* row = selection.selections.table(scope).find(stableOccurrenceOrdinal);
    return std::clamp(row ? row->weight : kDefaultChildWeight,
                      minimumChildWeight(selection),
                      kMaxChildWeight);
}

int minimumChildWeight(const VSelection& selection)
{
    return !(selection.mode == SelectionMode::Random && selection.isQueue())
            && selection.behavior.discipline == RandomDiscipline::Draw
        ? kMinDrawChildWeight : kMinOrderChildWeight;
}

int maxAvoidRepeat(const VSelection& selection,
                   const QList<uint32_t>& occurrenceOrdinals, PlatformScope scope)
{
    return maxAvoidRepeatFor(selection, occurrenceOrdinals, scope);
}

int effectiveAvoidRepeat(const VSelection& selection, PlatformScope scope)
{
    QList<uint32_t> ordinals;
    for (const auto& row : selection.selections.table(scope).occurrenceRows())
        ordinals.append(row.stableOccurrenceOrdinal);
    return std::clamp(selection.behavior.avoidRepeat, 0, maxAvoidRepeat(selection, ordinals, scope));
}

bool validateSelections(const VSelection& selection, QString* error)
{
    if (selection.mode != SelectionMode::Random || selection.isQueue()
        || selection.behavior.discipline != RandomDiscipline::Draw)
        return true;
    for (const auto* table : selection.selections.tables()) {
        const auto rows = table->occurrenceRows();
        if (!rows.empty() && std::none_of(rows.begin(), rows.end(),
                [](const auto& row) { return row.weight > 0; })) {
            if (error) *error = QStringLiteral("Selection table requires a positive draw weight");
            return false;
        }
    }
    return true;
}

QJsonObject toJson(const SelectionBehaviorDef& behavior)
{
    QJsonObject json;
    if (behavior.discipline != RandomDiscipline::Draw)
        json[QStringLiteral("discipline")] = QStringLiteral("shuffle");
    if (behavior.avoidRepeat != 0)
        json[QStringLiteral("avoidRepeat")] = behavior.avoidRepeat;
    if (behavior.memoryScope != SelectionMemoryScope::PerEmitter)
        json[QStringLiteral("memoryScope")] = QStringLiteral("global");
    if (behavior.resumeFromLast)
        json[QStringLiteral("resumeFromLast")] = true;
    return json;
}

std::optional<SelectionBehaviorDef> fromJson(const QJsonObject& json, QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<SelectionBehaviorDef> {
        if (error) *error = message;
        return std::nullopt;
    };

    SelectionBehaviorDef result;
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != QStringLiteral("discipline") && it.key() != QStringLiteral("avoidRepeat")
            && it.key() != QStringLiteral("memoryScope") && it.key() != QStringLiteral("resumeFromLast"))
            return fail(QStringLiteral("Unknown selection behavior field: %1").arg(it.key()));
    }
    if (json.contains(QStringLiteral("discipline"))) {
        if (!json.value(QStringLiteral("discipline")).isString())
            return fail(QStringLiteral("behavior.discipline must be a string"));
        const QString value = json.value(QStringLiteral("discipline")).toString();
        if (value == QStringLiteral("draw")) result.discipline = RandomDiscipline::Draw;
        else if (value == QStringLiteral("shuffle")) result.discipline = RandomDiscipline::Shuffle;
        else return fail(QStringLiteral("behavior.discipline is invalid"));
    }
    if (json.contains(QStringLiteral("avoidRepeat"))) {
        const QJsonValue value = json.value(QStringLiteral("avoidRepeat"));
        if (!value.isDouble() || !std::isfinite(value.toDouble())
            || value.toDouble() != std::trunc(value.toDouble())
            || value.toDouble() > std::numeric_limits<int>::max())
            return fail(QStringLiteral("behavior.avoidRepeat must be an integer"));
        result.avoidRepeat = value.toInt();
        if (result.avoidRepeat < 0)
            return fail(QStringLiteral("behavior.avoidRepeat must be non-negative"));
    }
    if (json.contains(QStringLiteral("memoryScope"))) {
        if (!json.value(QStringLiteral("memoryScope")).isString())
            return fail(QStringLiteral("behavior.memoryScope must be a string"));
        const QString value = json.value(QStringLiteral("memoryScope")).toString();
        if (value == QStringLiteral("perEmitter"))
            result.memoryScope = SelectionMemoryScope::PerEmitter;
        else if (value == QStringLiteral("global"))
            result.memoryScope = SelectionMemoryScope::Global;
        else
            return fail(QStringLiteral("behavior.memoryScope is invalid"));
    }
    if (json.contains(QStringLiteral("resumeFromLast"))) {
        if (!json.value(QStringLiteral("resumeFromLast")).isBool())
            return fail(QStringLiteral("behavior.resumeFromLast must be a boolean"));
        result.resumeFromLast = json.value(QStringLiteral("resumeFromLast")).toBool();
    }
    return result;
}

StepResult advance(const VSelection& selection,
                   const QList<uint32_t>& occurrenceOrdinals,
                   uint64_t entropy,
                   Memory& memory, PlatformScope scope)
{
    StepResult result;
    if (occurrenceOrdinals.isEmpty()
        || selection.mode == SelectionMode::Match || selection.mode == SelectionMode::Blend)
        return result;

    const int avoid = std::clamp(selection.behavior.avoidRepeat, 0,
                                 maxAvoidRepeat(selection, occurrenceOrdinals, scope));
    if (selection.mode == SelectionMode::Sequence) {
        if (memory.cursor < 0 || memory.cursor >= occurrenceOrdinals.size())
            memory.cursor = 0;
        result.child = occurrenceOrdinals[memory.cursor];
        ++memory.cursor;
        if (memory.cursor >= occurrenceOrdinals.size()) {
            memory.cursor = 0;
            result.roundCompleted = true;
        }
        return result;
    }

    if (!shuffleMode(selection)) {
        trimHistory(memory.history, avoid);
        const QList<uint32_t> candidates = eligibleDrawChildren(
            selection, occurrenceOrdinals, memory.history, scope);
        if (candidates.isEmpty())
            return result;
        result.child = candidates[weightedIndex(selection, candidates, entropy, scope)];
        memory.history.push_back(*result.child);
        trimHistory(memory.history, avoid);
        return result;
    }

    const bool invalidOrder = memory.cursor < 0
        || memory.cursor >= static_cast<int>(memory.order.size())
        || memory.order.size() != static_cast<size_t>(occurrenceOrdinals.size())
        || std::any_of(memory.order.begin(), memory.order.end(),
                       [&](uint32_t occurrence) {
                           return !occurrenceOrdinals.contains(occurrence);
                       });
    if (invalidOrder) {
        memory.order = makeShuffleOrder(
            selection, occurrenceOrdinals, entropy, memory.history, scope);
        memory.cursor = 0;
    }
    result.child = memory.order[static_cast<size_t>(memory.cursor++)];
    if (memory.cursor >= static_cast<int>(memory.order.size())) {
        result.roundCompleted = true;
        memory.history = {*result.child};
        trimHistory(memory.history, std::max(avoid, 1));
        memory.order.clear();
        memory.cursor = 0;
    }
    return result;
}

}
