#include "TransitionFieldGating.h"

#include "AuthoringPolicy.h"
#include "PlacementContext.h"
#include "ProjectTypes.h"
#include "Timeline/TimelineSemantics.h"

#include <QCoreApplication>

#include <algorithm>
#include <vector>

namespace VNX {
namespace TransitionSemantics {
namespace {

QString trText(const char* source)
{
    return QCoreApplication::translate("TransitionFieldGating", source);
}

FieldAvail lift(bool present)
{
    return present ? FieldAvail::Available : FieldAvail::Unavailable;
}

struct PieceTraits
{
    bool clock = false;
    bool anyMarker = false;
    bool exitMarker = false;
};

PieceTraits traitsOf(const ProjectItem* piece)
{
    PieceTraits traits;
    if (!piece) return traits;
    traits.clock = TimelineSemantics::hasMusicalClock(*piece);
    traits.anyMarker = TimelineSemantics::hasAnyMarker(*piece);
    traits.exitMarker = TimelineSemantics::hasMarkerKind(*piece, MarkerKind::Exit);
    return traits;
}

FieldAvail mergeUncertain(bool any, bool all)
{
    if (all) return FieldAvail::Available;
    if (any) return FieldAvail::Uncertain;
    return FieldAvail::Unavailable;
}

struct ChildWatchScan
{
    bool anyClock = false;
    bool allClock = true;
    bool anyMarker = false;
    bool allMarker = true;
    bool anyExitMarker = false;
    bool allExitMarker = true;
    bool anyChild = false;
    bool anyReference = false;
};

ChildWatchScan scanChildren(const ProjectItem& container,
                            const TimelineItemResolver& resolve)
{
    ChildWatchScan scan;
    for (const auto& child : container.children) {
        if (!child) continue;
        scan.anyChild = true;
        const TransitionWatchResult watch = resolveWatchReference(
            child.get(), container, nullptr, resolve);
        if (watch.direction != TransitionWatchDirection::Down) {
            scan.allClock = false;
            scan.allMarker = false;
            scan.allExitMarker = false;
            continue;
        }
        scan.anyReference = scan.anyReference || watch.found();
        const PieceTraits traits = traitsOf(watch.referencePiece);
        scan.anyClock = scan.anyClock || traits.clock;
        scan.allClock = scan.allClock && traits.clock;
        scan.anyMarker = scan.anyMarker || traits.anyMarker;
        scan.allMarker = scan.allMarker && traits.anyMarker;
        scan.anyExitMarker = scan.anyExitMarker || traits.exitMarker;
        scan.allExitMarker = scan.allExitMarker && traits.exitMarker;
    }
    if (!scan.anyChild) {
        scan.allClock = false;
        scan.allMarker = false;
        scan.allExitMarker = false;
    }
    return scan;
}

FieldAvail combineUpThenChildren(bool upwardHas, bool anyChild, bool allChild)
{
    if (upwardHas) return FieldAvail::Available;
    return mergeUncertain(anyChild, allChild);
}


QString noMarkerReason()
{
    return trText(
        "No marker in reach - no markers resolve from this source.");
}

QString exitMarkerNote()
{
    return trText(
        "The reference has no exit marker; this exit resolves to the tail, same as Natural End.");
}


QString uncertainMarkerReason()
{
    return trText(
        "Markers depend on which child is the incumbent - some reach markers, some do not.");
}

QString uncertainExitMarkerReason()
{
    return trText(
        "Exit marker depends on which child is the incumbent; when missing, this exit resolves to the tail.");
}

QString noWatchReferenceNote()
{
    return trText(
        "No watch reference; marker timing cannot resolve from this source.");
}

QString barrenReferenceNote()
{
    return trText(
        "The watch reference has no markers. Immediate and natural-end exits remain available.");
}

QString unresolvedSourceReason()
{
    return trText(
        "The source reference is unresolved, so nothing can be reached from it.");
}

QString unresolvedSourceNote()
{
    return trText(
        "This rule points at a deleted source; repoint it before its exit timing "
        "can resolve.");
}

QString idleBridgeFadeReason()
{
    return trText("No bridge - this fade slot is idle.");
}

QString currentExitTimingUnavailableNote()
{
    return trText(
        "The current exit timing is unavailable in this context. Choose a supported exit timing.");
}

FieldAvail exitTimingAvail(const ExitTimingAvail& exit, TransitionExitTiming timing)
{
    switch (timing) {
        case TransitionExitTiming::Immediate:  return exit.immediate;
        case TransitionExitTiming::NaturalEnd: return exit.naturalEnd;
        case TransitionExitTiming::NextBeat:   return exit.nextBeat;
        case TransitionExitTiming::NextBar:    return exit.nextBar;
        case TransitionExitTiming::NextMarker:    return exit.nextMarker;
        case TransitionExitTiming::ExitMarker:    return exit.exitMarker;
    }
    return FieldAvail::Available;
}

}

TransitionWatchAvailability watchAvailabilityFor(
    const ProjectItem& container,
    const std::optional<QUuid>& source,
    const PlacementContext* placement,
    const TimelineItemResolver& resolve)
{
    TransitionWatchAvailability availability;

    if (source) {
        const ProjectItem* sourceItem = resolve ? resolve(*source) : nullptr;
        if (!sourceItem) {
            availability.sourceResolved = false;
            return availability;
        }
        const TransitionWatchResult watch = resolveWatchReference(
            sourceItem, container, placement, resolve);
        const PieceTraits traits = traitsOf(watch.referencePiece);
        availability.referencePresent = watch.found();
        availability.clock = lift(traits.clock);
        availability.anyMarker = lift(traits.anyMarker);
        availability.exitMarker = lift(traits.exitMarker);
        return availability;
    }

    const TransitionWatchResult upward = resolveWatchReference(
        nullptr, container, placement, resolve);
    const PieceTraits upTraits = traitsOf(upward.referencePiece);
    const ChildWatchScan scan = scanChildren(container, resolve);
    availability.referencePresent = upward.found() || scan.anyReference;
    availability.clock = combineUpThenChildren(
        upTraits.clock, scan.anyClock, scan.allClock);
    availability.anyMarker = combineUpThenChildren(
        upTraits.anyMarker, scan.anyMarker, scan.allMarker);
    availability.exitMarker = combineUpThenChildren(
        upTraits.exitMarker, scan.anyExitMarker, scan.allExitMarker);
    return availability;
}

TransitionFieldGating gatingFor(const ProjectItem& container,
                                const TransitionRuleDef& rule,
                                const TransitionWatchAvailability& watch)
{
    TransitionFieldGating gating;
    gating.sourceEndpointEditable = !rule.isDefault();
    gating.targetEndpointEditable = !rule.isDefault();
    gating.bridgeFadesLive = rule.bridge.has_value();
    gating.samePositionReadoutVisible =
        rule.entryMode == TransitionEntryMode::SamePosition;

    gating.exit.immediate = FieldAvail::Available;
    gating.exit.naturalEnd = FieldAvail::Available;
    gating.exit.nextBeat = AuthoringPolicy::allowsTransitionExitTiming(TransitionExitTiming::NextBeat)
        ? watch.clock : FieldAvail::Unavailable;
    gating.exit.nextBar = AuthoringPolicy::allowsTransitionExitTiming(TransitionExitTiming::NextBar)
        ? watch.clock : FieldAvail::Unavailable;
    gating.exit.nextMarker = watch.anyMarker;
    gating.exit.exitMarker = FieldAvail::Available;

    gating.exit.reasonNextBeat = AuthoringPolicy::musicalTimeRestriction();
    gating.exit.reasonNextBar = AuthoringPolicy::musicalTimeRestriction();
    const QString markerUnavailableReason =
        watch.sourceResolved ? noMarkerReason() : unresolvedSourceReason();

    if (watch.anyMarker == FieldAvail::Unavailable) {
        gating.exit.reasonNextMarker = markerUnavailableReason;
    } else if (watch.anyMarker == FieldAvail::Uncertain) {
        gating.exit.reasonNextMarker = uncertainMarkerReason();
    }

    if (watch.exitMarker == FieldAvail::Unavailable) {
        gating.exit.reasonExitMarker = exitMarkerNote();
    } else if (watch.exitMarker == FieldAvail::Uncertain) {
        gating.exit.reasonExitMarker = uncertainExitMarkerReason();
    }

    if (!gating.bridgeFadesLive)
        gating.reasonBridgeFades = idleBridgeFadeReason();

    if (rule.entryMode == TransitionEntryMode::SamePosition) {
        gating.bridge = FieldAvail::Unavailable;
        gating.reasonBridge = trText(
            "Same Position is selected - a bridge removes the position correspondence between source and target.");
    }

    if (rule.bridge) {
        gating.samePosition = FieldAvail::Unavailable;
        gating.reasonSamePosition = trText(
            "A bridge is set - the target would have to align to the bridge, which shares no timeline with the source.");
    }

    QString primaryNote;
    if (!AuthoringPolicy::allowsTransitionExitTiming(rule.exitTiming)) {
        primaryNote = AuthoringPolicy::musicalTimeRestriction();
        gating.exit.noteIsWarning = true;
    } else if (exitTimingAvail(gating.exit, rule.exitTiming) == FieldAvail::Unavailable) {
        primaryNote = currentExitTimingUnavailableNote();
        gating.exit.noteIsWarning = true;
    } else if (!watch.sourceResolved) {
        primaryNote = unresolvedSourceNote();
        gating.exit.noteIsWarning = true;
    } else if (!watch.referencePresent) {
        primaryNote = noWatchReferenceNote();
    } else if (watch.anyMarker == FieldAvail::Unavailable) {
        primaryNote = barrenReferenceNote();
    }

    QString secondaryNote;
    if (watch.anyMarker == FieldAvail::Uncertain)
        secondaryNote = gating.exit.reasonNextMarker;
    else if (!gating.exit.reasonExitMarker.isEmpty())
        secondaryNote = gating.exit.reasonExitMarker;

    if (primaryNote.isEmpty())
        gating.exit.note = secondaryNote;
    else if (secondaryNote.isEmpty())
        gating.exit.note = primaryNote;
    else
        gating.exit.note = primaryNote + QStringLiteral(" ") + secondaryNote;

    (void)container;
    return gating;
}

QString defaultRuleEndpointReason()
{
    return trText(
        "Default rule - its endpoints are fixed at Any and cannot be changed.");
}

std::optional<TransitionEndpoints> firstUnusedCombination(
    const ProjectItem& container, const TransitionTableDef& table,
    const TimelineItemResolver& resolve)
{
    std::vector<const ProjectItem*> children;
    children.reserve(container.children.size());
    for (const auto& child : container.children) {
        if (child && AuthoringPolicy::validateNewSubtree(*child, nullptr, resolve))
            children.push_back(child.get());
    }
    if (children.empty())
        return std::nullopt;

    const auto occupied = [&table](const TransitionEndpoints& combo) {
        return std::any_of(table.rules.begin(), table.rules.end(),
            [&combo](const TransitionRuleDef& rule) {
                return rule.source == combo.first && rule.target == combo.second;
            });
    };

    for (const ProjectItem* source : children) {
        for (const ProjectItem* target : children) {
            if (source == target)
                continue;
            const TransitionEndpoints combo{source->guid, target->guid};
            if (!occupied(combo))
                return combo;
        }
    }
    for (const ProjectItem* source : children) {
        const TransitionEndpoints combo{source->guid, std::nullopt};
        if (!occupied(combo))
            return combo;
    }
    for (const ProjectItem* target : children) {
        const TransitionEndpoints combo{std::nullopt, target->guid};
        if (!occupied(combo))
            return combo;
    }
    return std::nullopt;
}

TransitionLedgerGating ledgerGatingFor(const ProjectItem& container,
                                       const TransitionTableDef& table,
                                       const TransitionRuleDef* selectedRule,
                                       const TimelineItemResolver& resolve)
{
    TransitionLedgerGating ledger;

    const bool hasChildren = std::any_of(
        container.children.begin(), container.children.end(),
        [&resolve](const std::shared_ptr<ProjectItem>& child) {
            return child && AuthoringPolicy::validateNewSubtree(*child, nullptr, resolve);
        });
    if (!hasChildren) {
        ledger.reasonAddRule = container.children.empty()
            ? trText("This Container has no children to reference.")
            : trText("This Container has no supported children to reference.");
    } else if (!firstUnusedCombination(container, table, resolve)) {
        ledger.reasonAddRule =
            trText("Every source and target combination already has a rule.");
    } else {
        ledger.addRule = FieldAvail::Available;
    }

    if (!selectedRule)
        ledger.reasonRemoveRule = trText("Select an override rule to remove it.");
    else if (selectedRule->isDefault())
        ledger.reasonRemoveRule = trText("The default rule cannot be removed.");
    else
        ledger.removeRule = FieldAvail::Available;

    return ledger;
}

}

}
