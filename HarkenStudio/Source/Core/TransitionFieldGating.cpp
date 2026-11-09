#include "TransitionFieldGating.h"

#include "AuthoringPolicy.h"
#include "Components/VAudio.h"
#include "PlacementContext.h"
#include "ProjectTypes.h"
#include "Timeline/TimelineSemantics.h"

#include <QCoreApplication>

#include <algorithm>
#include <vector>

namespace VNX {

const TransitionOptionEffect& TransitionFieldGating::exit(TransitionExitTiming timing) const
{
    switch (timing) {
        case TransitionExitTiming::Immediate:   return immediate;
        case TransitionExitTiming::NaturalEnd:  return naturalEnd;
        case TransitionExitTiming::NextBeat:    return nextBeat;
        case TransitionExitTiming::NextMeasure: return nextMeasure;
        case TransitionExitTiming::ExitMarker:  return exitMarker;
    }
    return immediate;
}

const TransitionOptionEffect& TransitionFieldGating::entry(TransitionEntryMode mode) const
{
    return mode == TransitionEntryMode::EntryMarker ? entryMarker : initialPosition;
}

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

FieldAvail mergeUncertain(bool any, bool all)
{
    if (all) return FieldAvail::Available;
    if (any) return FieldAvail::Uncertain;
    return FieldAvail::Unavailable;
}

struct SegmentTraits
{
    bool clock = false;
    bool exitMarker = false;
    bool entryMarker = false;
};

SegmentTraits traitsOf(const ProjectItem* segment)
{
    SegmentTraits traits;
    if (!segment) return traits;
    traits.clock = TimelineSemantics::hasMusicalClock(*segment);
    traits.exitMarker = TimelineSemantics::hasMarkerKind(*segment, MarkerKind::Exit);
    traits.entryMarker = TimelineSemantics::hasMarkerKind(*segment, MarkerKind::Entry);
    return traits;
}

bool loopsForever(const ProjectItem& item)
{
    const auto* audio = item.comp<VAudio>();
    return audio && audio->loopCount == 0;
}

struct EndpointTraits
{
    bool resolved = true;
    FieldAvail clock = FieldAvail::Unavailable;
    FieldAvail exitMarker = FieldAvail::Unavailable;
    FieldAvail entryMarker = FieldAvail::Unavailable;
    FieldAvail finiteEnd = FieldAvail::Available;
};

struct ChildScan
{
    bool anyChild = false;
    bool anyClock = false, allClock = true;
    bool anyExit = false, allExit = true;
    bool anyEntry = false, allEntry = true;
    bool anyFinite = false, allFinite = true;
};

ChildScan scanChildren(const ProjectItem& container, const TimelineItemResolver& resolve)
{
    ChildScan scan;
    for (const auto& child : container.children) {
        if (!child) continue;
        scan.anyChild = true;
        const bool finite = !loopsForever(*child);
        scan.anyFinite = scan.anyFinite || finite;
        scan.allFinite = scan.allFinite && finite;
        const TransitionWatchResult watch = resolveWatchReference(child.get(), container, nullptr, resolve);
        const SegmentTraits traits = watch.direction == TransitionWatchDirection::Down
            ? traitsOf(watch.referenceSegment) : SegmentTraits{};
        scan.anyClock = scan.anyClock || traits.clock;
        scan.allClock = scan.allClock && traits.clock;
        scan.anyExit = scan.anyExit || traits.exitMarker;
        scan.allExit = scan.allExit && traits.exitMarker;
        scan.anyEntry = scan.anyEntry || traits.entryMarker;
        scan.allEntry = scan.allEntry && traits.entryMarker;
    }
    if (!scan.anyChild)
        scan.allClock = scan.allExit = scan.allEntry = scan.allFinite = false;
    return scan;
}

EndpointTraits sourceTraits(const ProjectItem& container, const std::optional<QUuid>& source,
                            const PlacementContext* placement, const TimelineItemResolver& resolve)
{
    EndpointTraits traits;
    if (source) {
        const ProjectItem* item = resolve ? resolve(*source) : nullptr;
        if (!item) {
            traits.resolved = false;
            return traits;
        }
        const SegmentTraits segment = traitsOf(
            resolveWatchReference(item, container, placement, resolve).referenceSegment);
        traits.clock = lift(segment.clock);
        traits.exitMarker = lift(segment.exitMarker);
        traits.finiteEnd = lift(!loopsForever(*item));
        return traits;
    }
    const SegmentTraits upward = traitsOf(
        resolveWatchReference(nullptr, container, placement, resolve).referenceSegment);
    const ChildScan scan = scanChildren(container, resolve);
    traits.clock = upward.clock ? FieldAvail::Available : mergeUncertain(scan.anyClock, scan.allClock);
    traits.exitMarker = upward.exitMarker ? FieldAvail::Available : mergeUncertain(scan.anyExit, scan.allExit);
    traits.finiteEnd = mergeUncertain(scan.anyFinite, scan.allFinite);
    return traits;
}

EndpointTraits targetTraits(const ProjectItem& container, const std::optional<QUuid>& target,
                            const TimelineItemResolver& resolve)
{
    EndpointTraits traits;
    if (target) {
        const ProjectItem* item = resolve ? resolve(*target) : nullptr;
        if (!item) {
            traits.resolved = false;
            return traits;
        }
        const TransitionWatchResult watch = resolveWatchReference(item, container, nullptr, resolve);
        traits.entryMarker = lift(watch.direction == TransitionWatchDirection::Down
                                  && traitsOf(watch.referenceSegment).entryMarker);
        return traits;
    }
    const ChildScan scan = scanChildren(container, resolve);
    traits.entryMarker = mergeUncertain(scan.anyEntry, scan.allEntry);
    return traits;
}

TransitionOptionEffect option(FieldAvail effect, const QString& unavailable, const QString& uncertain)
{
    if (effect == FieldAvail::Unavailable) return {effect, unavailable};
    if (effect == FieldAvail::Uncertain) return {effect, uncertain};
    return {};
}

}

TransitionFieldGating gatingFor(const ProjectItem& container,
                                const TransitionRuleDef& rule,
                                const PlacementContext* placement,
                                const TimelineItemResolver& resolve)
{
    const EndpointTraits source = sourceTraits(container, rule.source, placement, resolve);
    const EndpointTraits target = targetTraits(container, rule.target, resolve);
    TransitionFieldGating gating;

    gating.naturalEnd = option(source.finiteEnd,
        trText("The source loops forever and never reaches its end, so this transition never happens."),
        trText("Depends on the playing child: for a child that loops forever, this transition never happens."));
    const TransitionOptionEffect musical = source.resolved
        ? option(source.clock,
                 trText("No musical clock in reach of the source; acts as Immediate."),
                 trText("Depends on the playing child: without a musical clock it acts as Immediate."))
        : TransitionOptionEffect{FieldAvail::Unavailable,
                                 trText("The source reference is missing; acts as Immediate.")};
    gating.nextBeat = musical;
    gating.nextMeasure = musical;
    gating.exitMarker = source.resolved
        ? option(source.exitMarker,
                 trText("The source has no exit marker; acts as Natural End."),
                 trText("Depends on the playing child: without an exit marker it acts as Natural End."))
        : TransitionOptionEffect{FieldAvail::Unavailable,
                                 trText("The source reference is missing; acts as Natural End.")};
    gating.entryMarker = target.resolved
        ? option(target.entryMarker,
                 trText("The target has no entry marker; acts as Initial Position."),
                 trText("Depends on the chosen child: without an entry marker it acts as Initial Position."))
        : TransitionOptionEffect{FieldAvail::Unavailable,
                                 trText("The target reference is missing; acts as Initial Position.")};

    if (rule.exitTiming != TransitionExitTiming::ExitMarker)
        gating.sourceTail = {FieldAvail::Unavailable,
            trText("Source Tail only plays with the Exit Marker exit; here it counts as 0 seconds.")};
    else
        gating.sourceTail = option(gating.exitMarker.effect,
            trText("The source has no exit marker, so its tail counts as 0 seconds."),
            trText("Depends on the playing child: without an exit marker the tail counts as 0 seconds."));
    if (rule.entryMode != TransitionEntryMode::EntryMarker)
        gating.targetLeadIn = {FieldAvail::Unavailable,
            trText("Target Lead-In only plays with the Entry Marker entry; here it counts as 0 seconds.")};
    else
        gating.targetLeadIn = option(gating.entryMarker.effect,
            trText("The target has no entry marker, so its lead-in counts as 0 seconds."),
            trText("Depends on the chosen child: without an entry marker the lead-in counts as 0 seconds."));
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
