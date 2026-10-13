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

struct TransitionWatchAvailability
{
    FieldAvail clock = FieldAvail::Unavailable;
    FieldAvail anyMarker = FieldAvail::Unavailable;
    FieldAvail exitMarker = FieldAvail::Unavailable;
    bool referencePresent = false;
    bool sourceResolved = true;
};

struct ExitTimingAvail
{
    FieldAvail immediate = FieldAvail::Available;
    FieldAvail naturalEnd = FieldAvail::Available;
    FieldAvail nextBeat = FieldAvail::Unavailable;
    FieldAvail nextBar = FieldAvail::Unavailable;
    FieldAvail nextMarker = FieldAvail::Unavailable;
    FieldAvail exitMarker = FieldAvail::Available;
    QString reasonNextBeat;
    QString reasonNextBar;
    QString reasonNextMarker;
    QString reasonExitMarker;
    QString note;
    bool noteIsWarning = false;
};

struct TransitionFieldGating
{
    bool sourceEndpointEditable = true;
    bool targetEndpointEditable = true;
    ExitTimingAvail exit;
    FieldAvail bridge = FieldAvail::Available;
    QString reasonBridge;
    FieldAvail samePosition = FieldAvail::Available;
    QString reasonSamePosition;
    bool bridgeFadesLive = false;
    QString reasonBridgeFades;
    bool samePositionReadoutVisible = false;
};

struct TransitionLedgerGating
{
    FieldAvail addRule = FieldAvail::Unavailable;
    QString reasonAddRule;
    FieldAvail removeRule = FieldAvail::Unavailable;
    QString reasonRemoveRule;
};

namespace TransitionSemantics {

TransitionWatchAvailability watchAvailabilityFor(
    const ProjectItem& container,
    const std::optional<QUuid>& source,
    const PlacementContext* placement,
    const TimelineItemResolver& resolve);

TransitionFieldGating gatingFor(const ProjectItem& container,
                                const TransitionRuleDef& rule,
                                const TransitionWatchAvailability& watch);

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
