#pragma once

#include "ProjectPlatform.h"
#include "MixingSemantics.h"
#include "GridSemantics.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QUuid>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace harken::studio {

enum class SelectionMode : uint8_t
{
    Random = 0,
    Sequence = 2,
    Match = 4,
    Blend = 5
};

struct SelectionEntry
{
    QUuid childGuid;
    uint32_t stableOccurrenceOrdinal = 0;
    int weight = 50;

    bool operator==(const SelectionEntry&) const = default;
};

struct SelectionOccurrence
{
    uint32_t stableOccurrenceOrdinal = 0;
    uint32_t order = 0;
    int weight = 50;

    bool operator==(const SelectionOccurrence&) const = default;
};

struct SelectionMatchRow
{
    uint32_t stableRuleOrdinal = 0;
    QUuid target;
    std::map<QUuid, DiscreteGridCell> cells;

    bool operator==(const SelectionMatchRow&) const = default;
};

struct SelectionCurve
{
    QUuid rowGuid;
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;
    uint32_t order = 0;
};

struct SelectionMixingRow
{
    QUuid guid;
    QUuid variable;

    bool operator==(const SelectionMixingRow&) const = default;
};

struct SelectionMember
{
    QUuid childGuid;
    uint32_t stableMemberOrdinal = 0;
    std::vector<SelectionOccurrence> occurrences;
    std::vector<SelectionCurve> curves;
};

struct SelectionTable
{
    uint64_t occurrenceOrdinalHighWater = 0;
    uint64_t ruleOrdinalHighWater = 0;
    std::vector<SelectionMember> entries;
    std::vector<SelectionMatchRow> matchRows;

    const SelectionOccurrence* find(uint32_t ordinal) const;
    SelectionOccurrence* find(uint32_t ordinal);
    const SelectionMember* member(const QUuid& childGuid) const;
    SelectionMember* member(const QUuid& childGuid);
    const SelectionMember* memberForOrdinal(uint32_t ordinal) const;
    const SelectionMember* memberForOccurrence(uint32_t ordinal) const;
    std::vector<SelectionEntry> occurrenceRows() const;
    std::vector<SelectionEntry> memberRows() const;
    bool setOccurrenceRows(const std::vector<SelectionEntry>& rows);
    void removeMembers(const std::function<bool(const SelectionMember&)>& predicate);
    DiscreteGridDef matchGrid(const std::vector<QUuid>& axes) const;
    bool setMatchGrid(const DiscreteGridDef& grid);
    MixingDef mixing(const std::vector<SelectionMixingRow>& axes) const;
    bool setMixing(const MixingDef& mixing);
    QJsonObject toJson() const;
    static std::optional<SelectionTable> fromJson(const QJsonObject& json, uint64_t occurrenceOrdinalHighWater,
                                                  uint64_t ruleOrdinalHighWater, const std::vector<QUuid>& matchAxes,
                                                  const std::vector<SelectionMixingRow>& blendAxes,
                                                  QString* error = nullptr);
    bool operator==(const SelectionTable& other) const;
};

struct SelectionModeTables
{
    SelectionTable shared;
    std::map<QUuid, SelectionTable> platforms;

    const SelectionTable& table(PlatformScope scope = std::nullopt) const;
    QList<const SelectionTable*> tables() const;
    QList<SelectionTable*> tables();
    bool operator==(const SelectionModeTables&) const = default;
};

struct SelectionData
{
    std::vector<QUuid> matchAxes;
    std::vector<SelectionMixingRow> blendAxes;
    SelectionModeTables random;
    SelectionModeTables sequence;
    SelectionModeTables match;
    SelectionModeTables blend;

    const SelectionModeTables& modeTables(SelectionMode mode) const;
    SelectionModeTables& editModeTables(SelectionMode mode);
    const SelectionTable& table(SelectionMode mode, PlatformScope scope = std::nullopt) const;
    QList<const SelectionTable*> tables() const;
    QList<SelectionTable*> tables();
    DiscreteGridDef matchGrid(PlatformScope scope = std::nullopt) const;
    MixingDef mixing(PlatformScope scope = std::nullopt) const;
    bool canEdit(SelectionMode mode, PlatformScope displayed, PlatformScope target) const;
    bool editTable(SelectionMode mode, PlatformScope target, const std::function<bool(SelectionTable&)>& edit);
    bool setOccurrenceRows(SelectionMode mode, const std::vector<SelectionEntry>& rows,
                           PlatformScope target = std::nullopt);
    bool setMatchGrid(const DiscreteGridDef& grid, PlatformScope target = std::nullopt);
    bool setMixing(const MixingDef& mixing, PlatformScope target = std::nullopt);
    bool setLocal(SelectionMode mode, const QUuid& platform, bool local);
    void removeMembers(const std::function<bool(const SelectionMember&)>& predicate);
    void remapVariables(const QHash<QUuid, QUuid>& guids);
    void replaceMatchAxis(size_t index, const QUuid& variable);
    QSet<QUuid> platformIds() const;
    void removePlatform(const QUuid& platform);
    QJsonObject toJson() const;
    static std::optional<SelectionData> fromJson(const QJsonObject& json,
                                                 QString* error = nullptr);
    bool operator==(const SelectionData&) const = default;
};

}
