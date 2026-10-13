
#include "TransitionRehearsal.h"

#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectTypes.h"
#include "VariableRehearsal.h"
#include "VariableSemantics.h"

namespace VNX {

TransitionRehearsal::TransitionRehearsal(QObject* parent)
    : QObject(parent)
{
}

std::optional<QUuid> TransitionRehearsal::previousWinner(const QUuid& containerId) const
{
    const auto it = states_.constFind(containerId);
    if (it == states_.constEnd()) return std::nullopt;
    return it->previousWinner;
}

std::optional<QUuid> TransitionRehearsal::currentWinner(const QUuid& containerId) const
{
    const auto it = states_.constFind(containerId);
    if (it == states_.constEnd()) return std::nullopt;
    return it->currentWinner;
}

void TransitionRehearsal::refreshWinner(const QUuid& containerId, std::optional<QUuid> winner)
{
    if (containerId.isNull()) return;
    const auto existing = states_.constFind(containerId);
    if (existing != states_.constEnd() && existing->currentWinner == winner)
        return;
    WinnerState& state = states_[containerId];
    state.previousWinner = state.currentWinner;
    state.currentWinner = std::move(winner);
    emit rehearsalChanged(containerId);
}

TransitionSemantics::HandoverResult TransitionRehearsal::handoverFor(
    const QUuid& containerId, const TransitionTableDef& table) const
{
    return TransitionSemantics::deriveHandover(
        table, previousWinner(containerId), currentWinner(containerId));
}

void TransitionRehearsal::clear(const QUuid& containerId)
{
    if (states_.remove(containerId) > 0)
        emit rehearsalChanged(containerId);
}

void TransitionRehearsal::clearAll()
{
    const QList<QUuid> touched = states_.keys();
    states_.clear();
    for (const QUuid& id : touched)
        emit rehearsalChanged(id);
}

std::optional<QUuid> TransitionRehearsal::winnerForContainer(
    const ProjectItem& container,
    const VariableRehearsal& variableRehearsal,
    const TimelineItemResolver& resolveVariable, PlatformScope scope)
{
    const auto* selection = container.comp<VSelection>();
    if (!selection)
        return std::nullopt;
    if (selection->mode == SelectionMode::Match) {
        if (selection->variableResponse() != VariableResponse::Live)
            return std::nullopt;
        return variableControlWinner(
            selection->matchGrid(scope), variableRehearsal, resolveVariable);
    }
    if (selection->isQueue()) {
        return queueIllustrativeWinner(container, scope);
    }
    return std::nullopt;
}

std::optional<QUuid> TransitionRehearsal::queueIllustrativeWinner(const ProjectItem& container, PlatformScope scope)
{
    const auto* selection = container.comp<VSelection>();
    if (!selection) return std::nullopt;
    const auto rows = selection->activeRows(scope);
    return rows.empty() ? std::nullopt : std::optional(rows.front().childGuid);
}

std::optional<QUuid> TransitionRehearsal::variableControlWinner(
    const DiscreteGridDef& grid,
    const VariableRehearsal& variableRehearsal,
    const TimelineItemResolver& resolveVariable)
{
    std::vector<float> axisValues;
    axisValues.reserve(grid.axes.size());
    for (const QUuid& axisId : grid.axes) {
        const ProjectItem* item = resolveVariable ? resolveVariable(axisId) : nullptr;
        const auto* variable = item ? item->comp<VVariable>() : nullptr;
        axisValues.push_back(variable
            ? VariableSemantics::currentValue(
                  variableRehearsal.overrideFor(axisId), variable->kind,
                  variable->rangeMin, variable->rangeMax, variable->defaultValue)
            : 0.0f);
    }

    const GridSemantics::DiscreteGridResult result =
        GridSemantics::evalDiscreteGrid(grid, axisValues, [&](const QUuid& id) {
            const auto* item = resolveVariable ? resolveVariable(id) : nullptr;
            return item ? item->comp<VVariable>() : nullptr;
        });
    if (result.winner < 0 || result.winner >= static_cast<int>(grid.rows.size()))
        return std::nullopt;

    const QUuid& target = grid.rows[static_cast<size_t>(result.winner)].target;
    if (target.isNull()) return std::nullopt;
    return target;
}

}
