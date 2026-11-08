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

struct SegmentTraits
{
    bool clock = false;
    bool anyMarker = false;
    bool exitMarker = false;
};

SegmentTraits traitsOf(const ProjectItem* segment)
{
    SegmentTraits traits;
    if (!segment) return traits;
    traits.clock = TimelineSemantics::hasMusicalClock(*segment);
    traits.anyMarker = TimelineSemantics::hasAnyMarker(*segment);
    traits.exitMarker = TimelineSemantics::hasMarkerKind(*segment, MarkerKind::Exit);
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
        const SegmentTraits traits = traitsOf(watch.referenceSegment);
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

QString noClockReason()
{
    return trText("No musical clock in reach - choose a source with access to a musical clock.");
}

QString uncertainClockReason()
{
    return trText("Musical timing depends on which child is the incumbent - some reach a musical clock, some do not.");
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
        const SegmentTraits traits = traitsOf(watch.referenceSegment);
        availability.clock = lift(traits.clock);
        availability.anyMarker = lift(traits.anyMarker);
        availability.exitMarker = lift(traits.exitMarker);
        return availability;
    }

    const TransitionWatchResult upward = resolveWatchReference(
        nullptr, container, placement, resolve);
    const SegmentTraits upTraits = traitsOf(upward.referenceSegment);
    const ChildWatchScan scan = scanChildren(container, resolve);
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

    gating.exit.immediate = FieldAvail::Available;
    gating.exit.naturalEnd = FieldAvail::Available;
    gating.exit.nextBeat = watch.clock;
    gating.exit.nextBar = watch.clock;
    gating.exit.nextMarker = watch.anyMarker;
    gating.exit.exitMarker = FieldAvail::Available;

    if (watch.clock == FieldAvail::Unavailable) {
        gating.exit.reasonNextBeat = watch.sourceResolved ? noClockReason() : unresolvedSourceReason();
        gating.exit.reasonNextBar = gating.exit.reasonNextBeat;
    } else if (watch.clock == FieldAvail::Uncertain) {
        gating.exit.reasonNextBeat = uncertainClockReason();
        gating.exit.reasonNextBar = gating.exit.reasonNextBeat;
    }
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

    if (exitTimingAvail(gating.exit, rule.exitTiming) == FieldAvail::Unavailable)
        gating.exit.warning = currentExitTimingUnavailableNote();
    else if (!watch.sourceResolved)
        gating.exit.warning = unresolvedSourceNote();

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
