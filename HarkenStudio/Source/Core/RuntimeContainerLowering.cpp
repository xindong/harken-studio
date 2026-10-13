#include "Core/Components/VSelection.h"
#include "RuntimeContainerLowering.h"

#include "Components/VVariable.h"
#include "ContainerBehaviorSemantics.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlaybackReferenceValidation.h"

#include <algorithm>
#include <functional>
#include <map>

namespace VNX::RuntimeContainerLowering {
namespace {

std::map<QUuid, const ProjectItem*> indexProject(const ProjectData& project)
{
    std::map<QUuid, const ProjectItem*> index;
    std::function<void(const ProjectItem*)> visit = [&](const ProjectItem* item) {
        if (!item) return;
        index.emplace(item->guid, item);
        for (const auto& child : item->children) visit(child.get());
    };
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem) visit(binding->rootItem.get());
    return index;
}

RuntimeContainerLoweringResult refuse(const QString& diagnostic)
{
    RuntimeContainerLoweringResult result;
    result.diagnostic = diagnostic;
    return result;
}

}

RuntimeContainerLoweringResult lower(const ProjectItem& container,
                                     const ProjectData& project, PlatformScope scope)
{
    if (container.type != ProjectItem::Type::Container
        || harkenstudio::IsNull(container.runtimeId()))
        return refuse(QStringLiteral("Runtime Container requires a stable Container object"));
    const auto* selection = container.comp<VSelection>();
    if (!selection)
        return refuse(QStringLiteral("Container selection component is missing"));
    if ((selection->isQueue() || (selection->mode == SelectionMode::Match
                                 && selection->variableResponse() == VariableResponse::Live))
        && !selection->transitions.rules.empty()
        && selection->transitions != canonicalTransitionTable())
        return refuse(QStringLiteral("Transition tables have no runtime Container contract"));
    if (selection->mode == SelectionMode::Blend)
        return refuse(QStringLiteral("Continuous grid has no runtime Container contract"));
    if (selection->isQueue())
        return refuse(QStringLiteral("Queue memory has no runtime Container contract"));
    if (selection->mode == SelectionMode::Random
        && selection->behavior.discipline == RandomDiscipline::Shuffle)
        return refuse(QStringLiteral("Shuffle memory has no runtime Container contract"));

    RuntimeContainerLoweringResult result;
    result.plan.containerId = container.runtimeId();
    result.plan.weightedRandom = selection->mode == SelectionMode::Random
        || selection->mode == SelectionMode::Match;
    result.plan.avoidRepeat = result.plan.weightedRandom
        ? static_cast<uint32_t>(ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection, scope)) : 0;

    struct ResolvedOccurrence
    {
        uint32_t stableOccurrenceOrdinal = 0;
        QUuid childGuid;
        const ProjectItem* child = nullptr;
    };
    const auto projectIndex = indexProject(project);
    std::vector<ResolvedOccurrence> occurrences;
    const auto activeRows = selection->activeRows(scope);
    occurrences.reserve(activeRows.size());
    for (const auto& occurrence : activeRows) {
        const auto child = projectIndex.find(occurrence.childGuid);
        if (child == projectIndex.end() || !child->second
            || harkenstudio::IsNull(child->second->runtimeId()))
            return refuse(QStringLiteral("Container child has no stable runtime identity"));
        if (child->second->type != ProjectItem::Type::Sound
            && child->second->type != ProjectItem::Type::Container)
            return refuse(QStringLiteral("Flat runtime Container accepts Sound or Container children only"));
        occurrences.push_back({occurrence.stableOccurrenceOrdinal,
                               occurrence.childGuid, child->second});
    }
    if (occurrences.empty())
        return refuse(QStringLiteral("Runtime Container requires at least one child"));

    if (selection->mode == SelectionMode::Match) {
        const auto grid = selection->matchGrid(scope);
        if (selection->variableResponse() != VariableResponse::AtStart)
            return refuse(QStringLiteral("Live Match is unsupported by flat runtime Container lowering"));
        if (grid.axes.size() != 1 || grid.rows.empty())
            return refuse(QStringLiteral("Runtime Container requires exactly one discrete axis"));
        const auto variable = projectIndex.find(grid.axes.front());
        if (variable == projectIndex.end()
            || variable->second->type != ProjectItem::Type::Variable
            || harkenstudio::IsNull(variable->second->runtimeId()))
            return refuse(QStringLiteral("Runtime Container axis Variable is missing"));
        result.plan.variableId = variable->second->runtimeId();
        std::map<uint32_t, QUuid> matches;
        for (const auto& row : grid.rows) {
            if (!validateDirectPlaybackTarget(container, row.target,
                    findDirectPlaybackChild(container, row.target)))
                return refuse(QStringLiteral("Runtime Container grid row must target a direct child"));
            const auto child = std::find_if(
                    occurrences.begin(), occurrences.end(),
                    [&](const ResolvedOccurrence& occurrence) {
                        return occurrence.childGuid == row.target;
                    });
            if (child == occurrences.end())
                return refuse(QStringLiteral("Runtime Container grid row must target a direct child"));
            const auto* declaration = variable->second->comp<VVariable>();
            const auto value = row.cells.size() == 1 && row.cells.front()
                ? GridSemantics::discreteCellValue(*row.cells.front(), declaration) : std::nullopt;
            if (!value || !declaration || *value < 0
                || *value < declaration->rangeMin || *value > declaration->rangeMax)
                return refuse(QStringLiteral("Runtime Container rows require one non-negative exact value"));
            const uint32_t match = static_cast<uint32_t>(*value);
            if (!matches.emplace(match, row.target).second)
                return refuse(QStringLiteral("Runtime Container match values must be unique"));
            result.plan.children.push_back({child->child->runtimeId(),
                static_cast<uint32_t>(ContainerBehaviorSemantics::effectiveChildWeight(
                    *selection, child->stableOccurrenceOrdinal, scope)), match,
                child->stableOccurrenceOrdinal});
        }
        if (result.plan.children.size() != occurrences.size())
            return refuse(QStringLiteral("Runtime Container grid must map every direct child exactly once"));
        return result;
    }

    for (const ResolvedOccurrence& occurrence : occurrences)
        result.plan.children.push_back({occurrence.child->runtimeId(),
            static_cast<uint32_t>(ContainerBehaviorSemantics::effectiveChildWeight(
                *selection, occurrence.stableOccurrenceOrdinal, scope)), 0,
            occurrence.stableOccurrenceOrdinal});
    return result;
}

}
