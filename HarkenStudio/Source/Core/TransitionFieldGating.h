#pragma once

#include "TransitionSemantics.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <utility>

namespace VNX {

class PlacementContext;
struct ProjectItem;

enum class FieldAvail : uint8_t { Available, Uncertain, Unavailable };

struct TransitionOptionEffect
{
    FieldAvail effect = FieldAvail::Available;
    QString note;

    bool effective() const { return effect == FieldAvail::Available; }
};

struct TransitionFieldGating
{
    TransitionOptionEffect immediate;
    TransitionOptionEffect naturalEnd;
    TransitionOptionEffect nextBeat;
    TransitionOptionEffect nextMeasure;
    TransitionOptionEffect exitMarker;
    TransitionOptionEffect initialPosition;
    TransitionOptionEffect entryMarker;
    TransitionOptionEffect sourceTail;
    TransitionOptionEffect targetLeadIn;

    const TransitionOptionEffect& exit(TransitionExitTiming timing) const;
    const TransitionOptionEffect& entry(TransitionEntryMode mode) const;
};

struct TransitionLedgerGating
{
    FieldAvail addRule = FieldAvail::Unavailable;
    QString reasonAddRule;
    FieldAvail removeRule = FieldAvail::Unavailable;
    QString reasonRemoveRule;
};

namespace TransitionSemantics {

TransitionFieldGating gatingFor(const ProjectItem& container,
                                const TransitionRuleDef& rule,
                                const PlacementContext* placement,
                                const TimelineItemResolver& resolve);

using TransitionEndpoints = std::pair<std::optional<QUuid>, std::optional<QUuid>>;

std::optional<TransitionEndpoints> firstUnusedCombination(
    const ProjectItem& container, const TransitionTableDef& table,
    const TimelineItemResolver& resolve = {});

TransitionLedgerGating ledgerGatingFor(const ProjectItem& container,
                                       const TransitionTableDef& table,
                                       const TransitionRuleDef* selectedRule,
                                       const TimelineItemResolver& resolve = {});

QString defaultRuleEndpointReason();

}

}
