
#include "VariableRehearsal.h"

#include <cmath>

namespace VNX {

VariableRehearsal::VariableRehearsal(QObject* parent)
    : QObject(parent)
{
}

std::optional<float> VariableRehearsal::overrideFor(const QUuid& variableId) const
{
    const auto it = overrides_.constFind(variableId);
    if (it == overrides_.constEnd())
        return std::nullopt;
    return *it;
}

bool VariableRehearsal::setOverride(const QUuid& variableId, float value)
{
    if (variableId.isNull() || !std::isfinite(value))
        return false;
    overrides_.insert(variableId, value);
    emit rehearsalChanged(variableId);
    return true;
}

VariableScope VariableRehearsal::scopeFor(const QUuid& variableId) const
{
    return scopes_.value(variableId, VariableScope::Emitter);
}

void VariableRehearsal::setScope(const QUuid& variableId, VariableScope scope)
{
    if (variableId.isNull() ||
        (scope != VariableScope::Global && scope != VariableScope::Emitter) ||
        scopeFor(variableId) == scope)
        return;
    scopes_.insert(variableId, scope);
    emit scopeChanged(variableId);
    if (overrides_.contains(variableId))
        emit rehearsalChanged(variableId);
}

void VariableRehearsal::clearAll()
{
    const QList<QUuid> touched = overrides_.keys();
    const QList<QUuid> scoped = scopes_.keys();
    overrides_.clear();
    scopes_.clear();
    for (const QUuid& id : scoped)
        emit scopeChanged(id);
    for (const QUuid& id : touched)
        emit rehearsalChanged(id);
}

}
