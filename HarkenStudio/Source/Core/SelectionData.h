#pragma once

#include "ProjectPlatform.h"
#include "MixingSemantics.h"
#include "GridSemantics.h"

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
    std::map<QUuid, int> platformWeights;

    bool operator==(const SelectionOccurrence&) const = default;
};

struct SelectionMatchRow
{
    uint32_t stableRuleOrdinal = 0;
    QUuid target;
    std::vector<std::optional<DiscreteGridCell>> cells;
    std::map<QUuid, std::map<QUuid, std::optional<DiscreteGridCell>>> platformCells;

    bool operator==(const SelectionMatchRow&) const = default;
};

struct SelectionCurveShape
{
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;

    bool operator==(const SelectionCurveShape&) const = default;
};

struct SelectionCurve
{
    QUuid rowGuid;
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;
    uint32_t order = 0;
    std::map<QUuid, SelectionCurveShape> platforms;
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
    std::vector<QUuid> matchAxes;
    std::vector<SelectionMatchRow> matchRows;
    std::vector<SelectionMixingRow> mixingRows;

    const SelectionOccurrence* find(uint32_t ordinal) const;
    SelectionOccurrence* find(uint32_t ordinal);
    const SelectionMember* member(const QUuid& childGuid) const;
    SelectionMember* member(const QUuid& childGuid);
    const SelectionMember* memberForOrdinal(uint32_t ordinal) const;
    const SelectionMember* memberForOccurrence(uint32_t ordinal) const;
    std::vector<SelectionEntry> occurrenceRows(PlatformScope scope = std::nullopt) const;
    std::vector<SelectionEntry> memberRows() const;
    bool setOccurrenceRows(const std::vector<SelectionEntry>& rows);
    bool editOccurrenceRows(const std::vector<SelectionEntry>& edited, PlatformScope displayed,
                            PlatformScope target);
    void removeMembers(const std::function<bool(const SelectionMember&)>& predicate);
    DiscreteGridDef matchGrid(PlatformScope scope = std::nullopt) const;
    bool setMatchGrid(const DiscreteGridDef& grid);
    bool editMatchGrid(const DiscreteGridDef& edited, PlatformScope displayed, PlatformScope target);
    void remapMatchAxes(const QHash<QUuid, QUuid>& guids);
    MixingDef mixing(PlatformScope scope = std::nullopt) const;
    bool setMixing(const MixingDef& mixing);
    bool editMixing(const MixingDef& edited, PlatformScope displayed, PlatformScope target);
    QSet<QUuid> platformIds() const;
    void removePlatform(const QUuid& platform);
    QJsonObject toJson() const;
    static std::optional<SelectionTable> fromJson(const QJsonObject& json,
                                                  QString* error = nullptr);
    bool operator==(const SelectionTable& other) const;
};

struct SelectionData
{
    SelectionTable random;
    SelectionTable sequence;
    SelectionTable match;
    SelectionTable blend;

    const SelectionTable& table(SelectionMode mode) const;
    SelectionTable& editTable(SelectionMode mode);
    QList<const SelectionTable*> tables() const;
    QList<SelectionTable*> tables();
    QSet<QUuid> platformIds() const;
    QJsonObject toJson() const;
    static std::optional<SelectionData> fromJson(const QJsonObject& json,
                                                 QString* error = nullptr);
    bool operator==(const SelectionData&) const = default;
};

}
