#pragma once

#include "ProjectPlatform.h"
#include "MixingSemantics.h"
#include "GridSemantics.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

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

struct SelectionCondition
{
    uint32_t stableRuleOrdinal = 0;
    uint32_t priority = 0;
    std::vector<std::optional<DiscreteGridCell>> cells;

    bool operator==(const SelectionCondition&) const = default;
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
    std::vector<SelectionCondition> conditions;
    std::vector<SelectionCurve> curves;
};

struct SelectionTable
{
    uint64_t occurrenceOrdinalHighWater = 0;
    uint64_t ruleOrdinalHighWater = 0;
    std::vector<SelectionMember> entries;
    std::vector<QUuid> matchAxes;
    std::vector<SelectionMixingRow> mixingRows;

    const SelectionOccurrence* find(uint32_t ordinal) const;
    SelectionOccurrence* find(uint32_t ordinal);
    const SelectionMember* member(const QUuid& childGuid) const;
    SelectionMember* member(const QUuid& childGuid);
    const SelectionMember* memberForOrdinal(uint32_t ordinal) const;
    const SelectionMember* memberForOccurrence(uint32_t ordinal) const;
    std::vector<SelectionEntry> occurrenceRows() const;
    std::vector<SelectionEntry> memberRows() const;
    bool setOccurrenceRows(const std::vector<SelectionEntry>& rows);
    DiscreteGridDef matchGrid() const;
    bool setMatchGrid(const DiscreteGridDef& grid);
    MixingDef mixing() const;
    bool setMixing(const MixingDef& mixing);
    QJsonObject toJson() const;
    static std::optional<SelectionTable> fromJson(const QJsonObject& json,
                                                  QString* error = nullptr);
    bool operator==(const SelectionTable& other) const;
};

struct SelectionData
{
    SelectionTable shared;
    QMap<QString, SelectionTable> platforms;

    const SelectionTable& table(PlatformScope scope = std::nullopt) const;
    SelectionTable& editTable(PlatformScope scope = std::nullopt);
    QList<const SelectionTable*> tables() const;
    QList<SelectionTable*> tables();
    QJsonObject toJson() const;
    static std::optional<SelectionData> fromJson(const QJsonObject& json,
                                                 QString* error = nullptr);
    bool operator==(const SelectionData&) const = default;
};

}
