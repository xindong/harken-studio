#include "VariableRangeEditing.h"

#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "VariableConsumerSemantics.h"
#include "VariableConsumerSemanticsPrivate.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace VNX::VariableRangeEditing {
namespace {

float remapX(float x, float oldMin, float oldMax, float newMin, float newMax)
{
    const double position = (static_cast<double>(x) - oldMin)
        / (static_cast<double>(oldMax) - oldMin);
    return static_cast<float>(newMin + position * (static_cast<double>(newMax) - newMin));
}

bool validCurve(const QVector<CurvePointDef>& points,
                const QVector<CurveHandlePairDef>& handles)
{
    if (points.size() < 2 || points.size() > kCurveMaxPoints
        || handles.size() != points.size() - 1) return false;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].value)
            || (i > 0 && !(points[i].x > points[i - 1].x))) return false;
    }
    for (qsizetype i = 0; i < handles.size(); ++i) {
        const auto& h = handles[i];
        if (!std::isfinite(h.h1x) || !std::isfinite(h.h2x)
            || !std::isfinite(h.h1y) || !std::isfinite(h.h2y)
            || h.h1x < points[i].x || h.h1x > points[i + 1].x
            || h.h2x < points[i].x || h.h2x > points[i + 1].x) return false;
    }
    return true;
}

template<typename Map>
void remapCurve(QVector<CurvePointDef>& points, QVector<CurveHandlePairDef>& handles,
                const Map& map)
{
    for (auto& point : points) point.x = map(point.x);
    for (auto& handle : handles) {
        handle.h1x = map(handle.h1x);
        handle.h2x = map(handle.h2x);
    }
}

bool fixBinding(ParameterBindingDef& binding, const VVariable& current, const VVariable& proposed)
{
    if (!(current.rangeMin < current.rangeMax)) {
        const float value = binding.curve.front().value;
        binding.curve = {{0.0f, value}, {1.0f, value}};
        binding.handles = {{0.0f, value, 1.0f, value}};
        return true;
    }
    const double span = static_cast<double>(proposed.rangeMax) - proposed.rangeMin;
    remapCurve(binding.curve, binding.handles, [&](float x) {
        return static_cast<float>((current.rangeMin
            + static_cast<double>(x) * (static_cast<double>(current.rangeMax) - current.rangeMin)
            - proposed.rangeMin) / span);
    });
    if (!validCurve(binding.curve, binding.handles)) return false;
    const float outsideValue = binding.curve.front().x >= 1.0f
        ? binding.curve.front().value : binding.curve.back().value;
    const auto hostType = binding.address.hostClass == BindingHostClass::Bus
        ? ProjectItemType::Bus : ProjectItemType::Sound;
    const auto contract = BindingContractSemantics::derive(binding.address.slotId, hostType);
    curveClipX(0.0f, 1.0f, binding.curve, binding.handles,
        contract.outputUnit == BindingOutputUnit::Decibel
            ? CurveAxisScale::MirroredAmplitudeDb : CurveAxisScale::LinearValue);
    if (binding.curve.isEmpty()) {
        binding.curve = {{0.0f, outsideValue}, {1.0f, outsideValue}};
        binding.handles = {{0.0f, outsideValue, 1.0f, outsideValue}};
        return true;
    }
    if (binding.curve.front().x > 0.0f) {
        const auto endpoint = binding.curve.front();
        binding.curve.prepend({0.0f, endpoint.value});
        binding.handles.prepend({0.0f, endpoint.value, endpoint.x, endpoint.value});
    }
    if (binding.curve.back().x < 1.0f) {
        const auto endpoint = binding.curve.back();
        binding.curve.append({1.0f, endpoint.value});
        binding.handles.append({endpoint.x, endpoint.value, 1.0f, endpoint.value});
    }
    return validCurve(binding.curve, binding.handles);
}

}

VariableRangeEditPlan prepare(const ProjectData& project, const ProjectItem& variable,
                             const VVariable& proposed, VariableRangePolicy policy,
                             const std::vector<VariableConsumerRef>& consumers)
{
    VariableRangeEditPlan plan;
    const auto& current = *variable.comp<VVariable>();
    const bool stretch = policy == VariableRangePolicy::Stretch;
    const bool canStretch = current.rangeMin < current.rangeMax;
    if (stretch && !canStretch) {
        plan.error = QObject::tr("The previous range has no width. Use Fix to change this range.");
        return plan;
    }
    const auto map = [&](float x) {
        return remapX(x, current.rangeMin, current.rangeMax, proposed.rangeMin, proposed.rangeMax);
    };
    const auto valueFor = [&](float value) {
        if (stretch) value = map(value);
        return VariableSemantics::normalizeDeclarationForKind(proposed.kind,
            proposed.rangeMin, proposed.rangeMax, value).defaultValue;
    };
    const auto curveError = [&](const ProjectItem& host, qsizetype points) {
        if (!plan.error.isEmpty()) return;
        plan.error = points > kCurveMaxPoints
            ? QObject::tr("Fix would exceed the %1-point curve limit in '%2'. Simplify the curve or use Stretch.")
                .arg(kCurveMaxPoints).arg(host.name)
            : QObject::tr("The range cannot represent the existing curve points in '%1'.")
                .arg(host.name);
    };
    plan.defaultValue = valueFor(current.defaultValue);

    QSet<QUuid> seenActions;
    QHash<QUuid, const ProjectItem*> hosts;
    for (const auto& consumer : consumers) {
        if (consumer.kind == VariableConsumerKind::DiscreteGridAxis) continue;
        if (!hosts.contains(consumer.hostGuid))
            hosts.insert(consumer.hostGuid,
                VariableConsumerSemantics::Internal::findItem(&project, consumer.hostGuid));
        const auto* host = hosts.value(consumer.hostGuid);
        if (!host) continue;
        if (consumer.kind == VariableConsumerKind::Binding) {
            const auto* mix = host->comp<VMixNode>();
            const auto* source = mix ? mix->bindings.find(consumer.bindingStableId) : nullptr;
            if (!source || !source->typed) continue;
            auto updated = *source;
            if (!validCurve(updated.curve, updated.handles)) {
                curveError(*host, updated.curve.size());
                continue;
            }
            if (!stretch && !fixBinding(updated, current, proposed)) {
                curveError(*host, updated.curve.size());
                continue;
            }
            auto displayPoints = BindingSemantics::storedToDisplay(updated.curve,
                BindingSemantics::displayDomain(proposed.rangeMin, proposed.rangeMax));
            auto displayHandles = BindingSemantics::storedToDisplay(updated.handles,
                BindingSemantics::displayDomain(proposed.rangeMin, proposed.rangeMax));
            if (!validCurve(displayPoints, displayHandles)) {
                curveError(*host, updated.curve.size());
                continue;
            }
            if (updated.toJson() == source->toJson()) continue;
            if (!plan.after.bindings.contains(host->guid)) {
                plan.before.bindings.insert(host->guid, mix->bindings);
                plan.after.bindings.insert(host->guid, mix->bindings);
            }
            *plan.after.bindings[host->guid].find(consumer.bindingStableId) = std::move(updated);
        } else if (consumer.kind == VariableConsumerKind::ContinuousGridAxis) {
            const auto* selection = host->comp<VSelection>();
            if (!plan.after.selections.contains(host->guid)) {
                plan.before.selections.insert(host->guid, selection->selections);
                plan.after.selections.insert(host->guid, selection->selections);
            }
            auto& table = plan.after.selections[host->guid].editTable(consumer.platform);
            const auto rowGuid = table.mixingRows[consumer.axisIndex].guid;
            for (auto& member : table.entries) {
                for (auto& curve : member.curves) {
                    if (curve.rowGuid != rowGuid) continue;
                    if (!validCurve(curve.points, curve.handles)) {
                        curveError(*host, curve.points.size());
                        continue;
                    }
                    if (stretch) {
                        remapCurve(curve.points, curve.handles, map);
                    } else {
                        curveClipX(proposed.rangeMin, proposed.rangeMax, curve.points, curve.handles);
                        if (curve.points.isEmpty()) {
                            curve.points = {{proposed.rangeMin, 0.0f}, {proposed.rangeMax, 0.0f}};
                            curve.handles = {{proposed.rangeMin, 0.0f, proposed.rangeMax, 0.0f}};
                        }
                    }
                    if (!validCurve(curve.points, curve.handles)) curveError(*host, curve.points.size());
                }
            }
        } else if (consumer.kind == VariableConsumerKind::ActionSetVariable
                   && !seenActions.contains(host->guid)) {
            seenActions.insert(host->guid);
            const auto* action = host->comp<VAction>();
            auto steps = action->steps;
            bool changed = false;
            for (auto& step : steps) {
                if (step.type != VActionStep::Type::SetVariable || step.targetObjectID != variable.objectID)
                    continue;
                const float updated = valueFor(step.value);
                changed |= updated != step.value;
                step.value = updated;
            }
            if (changed) {
                plan.before.actions.insert(host->guid, action->steps);
                plan.after.actions.insert(host->guid, std::move(steps));
            }
        }
    }
    for (auto it = plan.after.selections.begin(); it != plan.after.selections.end();) {
        if (it.value() == plan.before.selections.value(it.key())) {
            plan.before.selections.remove(it.key());
            it = plan.after.selections.erase(it);
        } else ++it;
    }
    return plan;
}

}
