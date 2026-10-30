
#include "BlueprintConnectCommand.h"

#include "../../Core/Components/VAudio.h"
#include "../../Core/Components/VMixNode.h"
#include "../../Core/Components/VBus.h"
#include "../../Core/PlayablePropertyAuthoringV1.h"
#include "../../Core/ProjectManager.h"
#include "../../Core/ProjectTypes.h"

#include <QObject>
#include <QtEndian>

#include <algorithm>

namespace VNX {

namespace {

ProjectItem* findItem(uint32_t objectID)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || objectID == 0) return nullptr;

    std::function<ProjectItem*(ProjectItem*)> walk = [&](ProjectItem* node) -> ProjectItem* {
        if (!node) return nullptr;
        if (node->objectID == objectID) return node;
        for (auto const& child : node->children) {
            if (auto* hit = walk(child.get())) return hit;
        }
        return nullptr;
    };
    for (const auto* binding : project->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        if (auto* hit = walk(binding->rootItem.get())) return hit;
    }
    return nullptr;
}

void emitChanged(ProjectItem* item, const QString& propName)
{
    if (!item) return;
    emit ProjectManager::instance().itemPropertyChanged(item, propName);
}

harkenprojectschema::AuthoringGuid authoringGuid(const QUuid& guid)
{
    const auto bytes = guid.toRfc4122();
    return {qFromBigEndian<quint64>(bytes.constData()),
            qFromBigEndian<quint64>(bytes.constData() + 8)};
}

FieldGroupEditResult prepareRouteEdit(ProjectItem& item, uint32_t targetBusId)
{
    FieldGroupEditRequest request;
    request.definition = item.guid;
    request.group = FieldGroupId::MainOutput;
    request.scopes.shared = true;
    request.expectedBefore = item.fieldGroups;
    harkenprojectschema::AuthoringGuid target{};
    if (targetBusId != 0) {
        const auto* bus = ProjectManager::instance().findBusItem(targetBusId);
        if (!bus) {
            FieldGroupEditResult result;
            result.diagnostics.push_back({FieldGroupEditDiagnosticCode::InvalidReference,
                                          QStringLiteral("Target Bus does not exist")});
            return result;
        }
        target = authoringGuid(bus->guid);
    }
    request.operation = SetFieldGroupField{
        harkenplayableproperties::PlayablePropertyIdV1::MainOutputBusRef, target};
    return prepareFieldGroupEdit(*ProjectManager::instance().currentProject(), request);
}

FieldGroupEditResult prepareSendEdit(ProjectItem& item, uint32_t targetBusId, bool connect)
{
    const auto* bus = ProjectManager::instance().findBusItem(targetBusId);
    if (!bus) {
        FieldGroupEditResult result;
        result.diagnostics.push_back({FieldGroupEditDiagnosticCode::InvalidReference,
                                      QStringLiteral("Target Bus does not exist")});
        return result;
    }
    auto sends = item.fieldGroups.send.shared.localHistory.value_or(SendFieldGroup{});
    const auto target = authoringGuid(bus->guid);
    const auto existing = std::find_if(sends.sends.begin(), sends.sends.end(),
        [&](const auto& row) { return row.targetBusID == target; });
    if (connect && existing == sends.sends.end())
        sends.sends.push_back({target, 0.0f});
    else if (!connect && existing != sends.sends.end())
        sends.sends.erase(existing);
    FieldGroupEditRequest request;
    request.definition = item.guid;
    request.group = FieldGroupId::Send;
    request.scopes.shared = true;
    request.expectedBefore = item.fieldGroups;
    request.operation = ReplaceFieldGroupSends{std::move(sends)};
    return prepareFieldGroupEdit(*ProjectManager::instance().currentProject(), request);
}

QString groupEditError(const FieldGroupEditResult& edit)
{
    if (edit.ok()) return edit.changed ? QString{} : QStringLiteral("duplicate");
    for (const auto& diagnostic : edit.diagnostics)
        if (diagnostic.code == FieldGroupEditDiagnosticCode::ReadOnly)
            return QStringLiteral("field_group_read_only");
    return QStringLiteral("output_rejected");
}

bool restoreGroupEdit(ProjectItem* item, const QUuid& definition,
                      const FieldGroupEditResult& edit, bool forward)
{
    if (!item || item->guid != definition || !edit.ok() || !edit.before) return false;
    return ProjectManager::instance().restoreFieldGroupsInternal(
        item, forward ? *edit.after : *edit.before,
        forward ? edit.before : edit.after);
}

}

BlueprintConnectCommand::BlueprintConnectCommand(uint32_t fromItemId,
                                                 uint32_t toId,
                                                 Effective effectiveType,
                                                 QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Blueprint Connect"), parent)
    , fromItemId_(fromItemId)
    , toId_(toId)
    , effective_(effectiveType)
{
    auto* item = findItem(fromItemId_);
    if (!item) return;

    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        groupDefinition_ = item->guid;
        groupEdit_ = effective_ == Effective::Containment
            ? FieldGroupEditResult{} : prepareRouteEdit(*item, toId_);
        if (!groupEdit_->ok() || !groupEdit_->changed) setObsolete(true);
        return;
    }

    const auto* mc = item->comp<VMixNode>();
    oldOutputBusId_  = mc ? mc->outputBusId : 0;
    const auto* ac = item->comp<VAudio>();
    oldOverrideBits_ = ac ? ac->overrideBits : 0;
    oldOutputBusOverride_ = item->outputBusOverride;
    oldParentRaw_    = item->parent;
}

void BlueprintConnectCommand::redo()
{
    auto* item = findItem(fromItemId_);
    if (groupEdit_) {
        if (!restoreGroupEdit(item, groupDefinition_, *groupEdit_, true)) setObsolete(true);
        return;
    }
    if (!item) return;

    if (firstRedo_) {
        firstRedo_ = false;
    }

    switch (effective_) {
        case Effective::Containment:
            item->compOrCreate<VMixNode>()->outputBusId = 0;
            item->setOutputBusOverride(false);
            emitChanged(item, QStringLiteral("outputBus"));
            break;

        case Effective::BusRoute:
            item->compOrCreate<VMixNode>()->outputBusId = toId_;
            item->setOutputBusOverride(false);
            emitChanged(item, QStringLiteral("outputBus"));
            break;

        case Effective::Override:
            item->compOrCreate<VMixNode>()->outputBusId = toId_;
            item->setOutputBusOverride(true);
            emitChanged(item, QStringLiteral("outputBus"));
            break;
    }
}

void BlueprintConnectCommand::undo()
{
    auto* item = findItem(fromItemId_);
    if (groupEdit_) {
        if (!restoreGroupEdit(item, groupDefinition_, *groupEdit_, false)) setObsolete(true);
        return;
    }
    if (!item) return;

    item->compOrCreate<VMixNode>()->outputBusId = oldOutputBusId_;
    if (auto* ac = item->comp<VAudio>()) ac->overrideBits = oldOverrideBits_;
    item->outputBusOverride = oldOutputBusOverride_;
    emitChanged(item, QStringLiteral("outputBus"));
}

QString BlueprintConnectCommand::error() const
{
    return groupEdit_ ? groupEditError(*groupEdit_) : QString{};
}

BlueprintDisconnectCommand::BlueprintDisconnectCommand(uint32_t fromItemId,
                                                       uint32_t toId,
                                                       Kind kind,
                                                       QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Blueprint Disconnect"), parent)
    , fromItemId_(fromItemId)
    , toId_(toId)
    , kind_(kind)
{
    auto* item = findItem(fromItemId_);
    if (!item) return;

    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        groupDefinition_ = item->guid;
        groupEdit_ = kind_ == Kind::Containment
            ? FieldGroupEditResult{} : prepareRouteEdit(*item, 0);
        if (groupEdit_->ok()) {
            const auto* bus = ProjectManager::instance().findBusItem(toId_);
            const auto& output = groupEdit_->before->mainOutput.shared.localHistory;
            if (!bus || !output || output->mainOutputBusRef != authoringGuid(bus->guid)) {
                groupEdit_->after.reset();
                groupEdit_->diagnostics.push_back({FieldGroupEditDiagnosticCode::StaleState,
                    QStringLiteral("Main Output no longer targets this Bus")});
            }
        }
        if (!groupEdit_->ok() || !groupEdit_->changed) setObsolete(true);
        return;
    }

    oldParentRaw_    = item->parent;
    const auto* mc = item->comp<VMixNode>();
    oldOutputBusId_  = mc ? mc->outputBusId : 0;
    const auto* ac = item->comp<VAudio>();
    oldOverrideBits_ = ac ? ac->overrideBits : 0;
    oldOutputBusOverride_ = item->outputBusOverride;
}

void BlueprintDisconnectCommand::redo()
{
    auto* item = findItem(fromItemId_);
    if (groupEdit_) {
        if (!restoreGroupEdit(item, groupDefinition_, *groupEdit_, true)) setObsolete(true);
        return;
    }
    if (!item) return;

    if (firstRedo_) {
        firstRedo_ = false;
    }

    switch (kind_) {
        case Kind::Containment:
            break;
        case Kind::BusRoute:
        case Kind::Override:
            item->compOrCreate<VMixNode>()->outputBusId = 0;
            item->setOutputBusOverride(false);
            emitChanged(item, QStringLiteral("outputBus"));
            break;
    }
}

void BlueprintDisconnectCommand::undo()
{
    auto* item = findItem(fromItemId_);
    if (groupEdit_) {
        if (!restoreGroupEdit(item, groupDefinition_, *groupEdit_, false)) setObsolete(true);
        return;
    }
    if (!item) return;

    item->compOrCreate<VMixNode>()->outputBusId = oldOutputBusId_;
    if (auto* ac = item->comp<VAudio>()) ac->overrideBits = oldOverrideBits_;
    item->outputBusOverride = oldOutputBusOverride_;
    emitChanged(item, QStringLiteral("outputBus"));
}

QString BlueprintDisconnectCommand::error() const
{
    return groupEdit_ ? groupEditError(*groupEdit_) : QString{};
}

BlueprintOverrideToggleCommand::BlueprintOverrideToggleCommand(uint32_t itemId,
                                                               bool newOverride,
                                                               uint32_t autoRouteToBusId,
                                                               QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Blueprint Override Toggle"), parent)
    , itemId_(itemId)
    , newOverride_(newOverride)
    , autoRouteToBusId_(autoRouteToBusId)
{
    auto* item = findItem(itemId_);
    if (!item) return;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        setObsolete(true);
        return;
    }
    const auto* mc = item->comp<VMixNode>();
    oldOutputBusId_  = mc ? mc->outputBusId : 0;
    const auto* ac = item->comp<VAudio>();
    oldOverrideBits_ = ac ? ac->overrideBits : 0;
    oldOutputBusOverride_ = item->outputBusOverride;
}

void BlueprintOverrideToggleCommand::redo()
{
    auto* item = findItem(itemId_);
    if (!item) return;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        setObsolete(true);
        return;
    }

    if (firstRedo_) firstRedo_ = false;

    item->setOutputBusOverride(newOverride_);
    const auto* mc = item->comp<VMixNode>();
    if (newOverride_ && (mc ? mc->outputBusId : 0) == 0 && autoRouteToBusId_ != 0) {
        item->compOrCreate<VMixNode>()->outputBusId = autoRouteToBusId_;
    }
    emitChanged(item, QStringLiteral("outputBus"));
}

void BlueprintOverrideToggleCommand::undo()
{
    auto* item = findItem(itemId_);
    if (!item) return;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        setObsolete(true);
        return;
    }

    item->compOrCreate<VMixNode>()->outputBusId = oldOutputBusId_;
    if (auto* ac = item->comp<VAudio>()) ac->overrideBits = oldOverrideBits_;
    item->outputBusOverride = oldOutputBusOverride_;
    emitChanged(item, QStringLiteral("outputBus"));
}

BlueprintSendConnectCommand::BlueprintSendConnectCommand(uint32_t fromBusId,
                                                         uint32_t targetBusId,
                                                         QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Blueprint Send Connect"), parent)
    , fromBusId_(fromBusId)
    , targetBusId_(targetBusId)
{
    auto* item = findItem(fromBusId_);
    if (item && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        groupDefinition_ = item->guid;
        groupEdit_ = prepareSendEdit(*item, targetBusId_, true);
        if (!groupEdit_->ok() || !groupEdit_->changed) setObsolete(true);
    }
}

void BlueprintSendConnectCommand::redo()
{
    if (groupEdit_) {
        if (!restoreGroupEdit(findItem(fromBusId_), groupDefinition_, *groupEdit_, true))
            setObsolete(true);
        return;
    }
    if (firstRedo_) firstRedo_ = false;
    applied_ = ProjectManager::instance().upsertSendInternal(
        fromBusId_, targetBusId_, 1.0f);
}

void BlueprintSendConnectCommand::undo()
{
    if (groupEdit_) {
        if (!restoreGroupEdit(findItem(fromBusId_), groupDefinition_, *groupEdit_, false))
            setObsolete(true);
        return;
    }
    if (!applied_) return;
    ProjectManager::instance().removeSendInternal(fromBusId_, targetBusId_);
}

BlueprintSendDisconnectCommand::BlueprintSendDisconnectCommand(uint32_t fromBusId,
                                                               uint32_t targetBusId,
                                                               QUndoCommand* parent)
    : QUndoCommand(QObject::tr("Blueprint Send Disconnect"), parent)
    , fromBusId_(fromBusId)
    , targetBusId_(targetBusId)
{
    auto* item = findItem(fromBusId_);
    if (item && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        groupDefinition_ = item->guid;
        groupEdit_ = prepareSendEdit(*item, targetBusId_, false);
        if (!groupEdit_->ok() || !groupEdit_->changed) setObsolete(true);
        return;
    }
    ProjectItem* fromItem = ProjectManager::instance().findBusItem(fromBusId_);
    if (const auto* mix = fromItem ? fromItem->comp<VMixNode>() : nullptr) {
        for (auto const& s : mix->sends) {
            if (s.targetBusID == targetBusId_) {
                hadSend_   = true;
                sendLevel_ = s.sendLevel;
                break;
            }
        }
    }
}

void BlueprintSendDisconnectCommand::redo()
{
    if (groupEdit_) {
        if (!restoreGroupEdit(findItem(fromBusId_), groupDefinition_, *groupEdit_, true))
            setObsolete(true);
        return;
    }
    if (firstRedo_) firstRedo_ = false;
    ProjectManager::instance().removeSendInternal(fromBusId_, targetBusId_);
}

void BlueprintSendDisconnectCommand::undo()
{
    if (groupEdit_) {
        if (!restoreGroupEdit(findItem(fromBusId_), groupDefinition_, *groupEdit_, false))
            setObsolete(true);
        return;
    }
    if (!hadSend_) return;
    ProjectManager::instance().upsertSendInternal(
        fromBusId_, targetBusId_, sendLevel_);
}

}
