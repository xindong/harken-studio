
#include <harken_studio_build/BindingTransition.h>
#include "BindingRuntimeLowering.h"

#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "ProjectTypes.h"

#include <harken_studio_build/BindingTargets.h>

#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>

#include <cmath>
#include <functional>
#include <set>
#include <unordered_set>

namespace VNX {
namespace BindingRuntimeLoweringSemantics {
namespace {

void visitItem(const ProjectItem* item, const std::function<void(const ProjectItem&)>& fn)
{
    if (!item) return;
    fn(*item);
    for (const auto& child : item->children)
        visitItem(child.get(), fn);
}

void visitProject(const ProjectData& project, const std::function<void(const ProjectItem&)>& fn)
{
    if (project.root)
        visitItem(project.root.get(), fn);
    for (const ProjectFileBinding* binding : project.allFileBindings()) {
        if (binding && binding->rootItem)
            visitItem(binding->rootItem.get(), fn);
    }
}

bool bindingShapedUntypedHasIllegalTransitionMs(const ParameterBindingDef& source)
{
    if (source.typed || !source.opaqueValue.isObject())
        return false;
    const QJsonObject obj = source.opaqueValue.toObject();
    if (!BindingSemantics::parseAddress(obj)
        || QUuid::fromString(obj.value(QStringLiteral("variable")).toString()).isNull())
        return false;
    if (!obj.contains(QStringLiteral("transitionMs")))
        return false;
    const QJsonValue raw = obj.value(QStringLiteral("transitionMs"));
    if (!raw.isDouble())
        return true;
    const double ms = raw.toDouble();
    return !std::isfinite(ms) || std::trunc(ms) != ms || ms < 0.0 || ms > harkenstudiobuild::kMaxBindingTransitionMs;
}

const ProjectItem* findByGuid(const ProjectData& project, const QUuid& guid)
{
    const ProjectItem* found = nullptr;
    if (guid.isNull()) return nullptr;
    visitProject(project, [&](const ProjectItem& item) {
        if (!found && item.guid == guid)
            found = &item;
    });
    return found;
}

}

bool isContentHost(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Layer
        || type == ProjectItem::Type::Sound
        || type == ProjectItem::Type::Container
        || type == ProjectItem::Type::Track
        || type == ProjectItem::Type::Segment;
}

void collectAncestorBindings(const ProjectItem& leaf,
                             BindingHostClass hostClass,
                             std::vector<const ParameterBindingDef*>* out)
{
    if (!out) return;
    std::set<harkenstudio::ObjectId> seen;
    for (const ProjectItem* ancestor = &leaf; ancestor; ancestor = ancestor->parent) {
        if (!isContentHost(ancestor->type))
            continue;
        const auto* mix = ancestor->comp<VMixNode>();
        if (!mix) continue;
        for (const ParameterBindingDef& source : mix->bindings.entries()) {
            if (!source.typed || harkenstudio::IsNull(source.runtimeId())
                || source.address.hostClass != hostClass)
                continue;
            if (!seen.insert(source.runtimeId()).second)
                continue;
            out->push_back(&source);
        }
    }
}

QVector<CurvePointDef> storedCurveToEngineX(
    const QVector<CurvePointDef>& points, float rangeMin, float rangeMax)
{
    return BindingSemantics::storedToDisplay(
        points, BindingSemantics::displayDomain(rangeMin, rangeMax));
}

QVector<CurveHandlePairDef> storedHandlesToEngineX(
    const QVector<CurveHandlePairDef>& handles, float rangeMin, float rangeMax)
{
    return BindingSemantics::storedToDisplay(
        handles, BindingSemantics::displayDomain(rangeMin, rangeMax));
}

const VVariable* variableOf(const ProjectData& project, const QUuid& variableId)
{
    const ProjectItem* item = findByGuid(project, variableId);
    return item && item->type == ProjectItem::Type::Variable
        ? item->comp<VVariable>() : nullptr;
}

harkenstudio::ObjectId attachmentObjectId(const ProjectItem& host)
{
    return host.runtimeId();
}

BindingLowerStatus appendLoweredBinding(const ProjectData& project,
                                        const ProjectItem& host,
                                        const ParameterBindingDef& source,
                                        BindingRuntimeLowering* output)
{
    if (!output)
        return BindingLowerStatus::Fail;
    if (!source.typed) {
        if (bindingShapedUntypedHasIllegalTransitionMs(source))
            return BindingLowerStatus::Fail;
        return BindingLowerStatus::Skip;
    }
    const auto targetId = BindingContractSemantics::engineTargetId(source.address);
    if (!harkenstudiobuild::IsValidBindingTransitionMs(source.transitionMs)
        || (source.transitionMs != 0
            && targetId
            && !BindingContractSemantics::supportsTransition(source.address)))
        return BindingLowerStatus::Fail;
    if (harkenstudio::IsNull(source.runtimeId())
        || source.variable.isNull() || source.curve.size() < 2
        || !targetId)
        return BindingLowerStatus::Skip;

    const ProjectItem* variableItem = findByGuid(project, source.variable);
    const auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
    if (!variableItem || variableItem->type != ProjectItem::Type::Variable
        || harkenstudio::IsNull(variableItem->runtimeId()) || !variable)
        return BindingLowerStatus::Skip;
    if (harkenstudio::IsNull(host.runtimeId()))
        return BindingLowerStatus::Skip;

    BindingRuntimeProgram program;
    program.bindingId = source.runtimeId();
    program.variableId = variableItem->runtimeId();
    program.targetScopeKind =
        BindingContractSemantics::engineTargetScopeKind(source.address.hostClass);
    program.targetId = *targetId;
    program.transitionMs = source.transitionMs;
    program.points = storedCurveToEngineX(source.curve, variable->rangeMin, variable->rangeMax);
    program.handles = storedHandlesToEngineX(source.handles, variable->rangeMin, variable->rangeMax);

    BindingRuntimeAttachment attachment;
    attachment.bindingId = source.runtimeId();
    attachment.scopeKind = program.targetScopeKind;
    attachment.objectId = host.runtimeId();

    output->programs.push_back(std::move(program));
    output->attachments.push_back(attachment);
    return BindingLowerStatus::Ok;
}

BindingRuntimeLowering lowerProject(const ProjectData& project)
{
    BindingRuntimeLowering lowering;
    std::unordered_set<harkenstudio::ObjectId> seenPrograms;
    std::set<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> seenAttachments;
    const auto apply = [&](const ProjectItem& item, const ParameterBindingDef& source) {
        if (lowering.failed)
            return;
        const BindingLowerStatus status =
            appendLoweredBinding(project, item, source, &lowering);
        if (status == BindingLowerStatus::Fail) {
            lowering.failed = true;
            lowering.error = QStringLiteral(
                "Binding transitionMs is illegal or unsupported");
            lowering.programs.clear();
            lowering.attachments.clear();
            return;
        }
        if (status != BindingLowerStatus::Ok)
            return;
        if (!seenPrograms.insert(source.runtimeId()).second)
            lowering.programs.pop_back();
        if (!seenAttachments.insert({source.runtimeId(), item.runtimeId()}).second)
            lowering.attachments.pop_back();
    };

    visitProject(project, [&](const ProjectItem& item) {
        if (lowering.failed)
            return;
        const BindingHostClass hostClass =
            BindingContractSemantics::hostClassOf(item.type);
        if (hostClass == BindingHostClass::Bus) {
            const auto* mix = item.comp<VMixNode>();
            if (!mix) return;
            for (const ParameterBindingDef& source : mix->bindings.entries()) {
                if (source.address.hostClass != BindingHostClass::Bus)
                    continue;
                apply(item, source);
            }
            return;
        }

        if (!isContentHost(item.type))
            return;
        const auto* mix = item.comp<VMixNode>();
        if (!mix) return;
        for (const ParameterBindingDef& source : mix->bindings.entries()) {
            if (source.address.hostClass != BindingHostClass::Voice)
                continue;
            apply(item, source);
        }
    });
    return lowering;
}

bool sameAttachment(const BindingRuntimeAttachment& a,
                    const BindingRuntimeAttachment& b)
{
    return a.bindingId == b.bindingId
        && a.scopeKind == b.scopeKind
        && a.objectId == b.objectId;
}

PreviewBindingSyncState reconcilePreviewSync(
    const BindingRuntimeLowering& desired,
    const PreviewBindingSyncState& previous,
    const std::vector<harkenstudio::ObjectId>& registeredOk,
    const std::vector<BindingRuntimeAttachment>& attachedOk)
{
    auto hasId = [](const std::vector<harkenstudio::ObjectId>& ids, harkenstudio::ObjectId id) {
        for (harkenstudio::ObjectId candidate : ids) {
            if (candidate == id)
                return true;
        }
        return false;
    };
    auto hasAttachment = [](const std::vector<BindingRuntimeAttachment>& items,
                            const BindingRuntimeAttachment& item) {
        for (const BindingRuntimeAttachment& candidate : items) {
            if (sameAttachment(candidate, item))
                return true;
        }
        return false;
    };

    PreviewBindingSyncState next;
    next.ids.reserve(desired.programs.size());
    for (const BindingRuntimeProgram& program : desired.programs) {
        if (hasId(registeredOk, program.bindingId)
            || hasId(previous.ids, program.bindingId))
            next.ids.push_back(program.bindingId);
    }
    next.attachments.reserve(desired.attachments.size());
    for (const BindingRuntimeAttachment& attachment : desired.attachments) {
        if (hasAttachment(attachedOk, attachment)
            || hasAttachment(previous.attachments, attachment))
            next.attachments.push_back(attachment);
    }
    return next;
}

}
}
