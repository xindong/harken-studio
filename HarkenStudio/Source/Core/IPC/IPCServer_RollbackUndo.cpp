
#include "IPCServerInternal.h"
#include "../Actions/MixerActions.h"
#include "../BusRouteGraph.h"

#include "ItemTypeCatalog.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <QJsonArray>

namespace VNX {



VBus* IPCServer::resolveBusByNameForTx(const QString& rawName) const
{
    const QString busName = rawName.trimmed();
    if (busName.isEmpty()) return nullptr;

    auto& pm = ProjectManager::instance();
    if (auto* item = pm.findBusItemByName(busName)) {
        if (auto* bc = item->comp<VBus>())
            return bc;
    }
    return nullptr;
}


bool IPCServer::prepareRollbackStepForAction(const QString& cmd,
                                             const QJsonObject& args,
                                             QJsonObject* rollbackStep,
                                             QJsonObject* previewEntry,
                                             QString* error)
{
    if (!rollbackStep || !previewEntry)
    {
        if (error) *error = "Internal error: rollbackStep/previewEntry is null.";
        return false;
    }

    *rollbackStep = QJsonObject{};
    *previewEntry = QJsonObject{};
    (*previewEntry)["cmd"] = cmd;

    if (cmd == QLatin1String("set_bus_properties"))
    {
        const QString unsupportedKey = unsupportedBusPropertiesKey(args);
        if (!unsupportedKey.isEmpty()) {
            if (error) *error = QString("Bus property '%1' is not supported by set_bus_properties")
                .arg(unsupportedKey);
            return false;
        }
        if (!args.contains("bus_id"))
        {
            if (error) *error = "set_bus_properties requires bus_id.";
            return false;
        }

        const uint32_t busID = static_cast<uint32_t>(args.value("bus_id").toInteger());
        auto& pm = ProjectManager::instance();
        auto* item = pm.findBusItem(busID);
        const auto* bc   = item ? item->comp<VBus>() : nullptr;
        const auto* mix  = item ? item->comp<VMixNode>() : nullptr;
        if (!bc || !mix || !item)
        {
            if (error) *error = QString("Bus ID %1 not found").arg(busID);
            return false;
        }

        const auto& overlay = pm.auditionOverlay();
        QJsonObject before{
            {"volume", mix->volume}, {"pitch", mix->pitch},
            {"outputBusVolume", mix->outputBusVolume}, {"pan", bc->pan},
            {"mute", overlay.isMuted(item->runtimeId())},
            {"solo", overlay.isSoloed(item->runtimeId())}
        };
        QJsonObject after = before;
        QJsonObject playable;
        QJsonArray changed;
        *rollbackStep = QJsonObject{
            {"kind", "restore_playable_properties"},
            {"item_guid", item->guid.toString(QUuid::WithoutBraces)}
        };
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
        for (const auto* name : {"volume", "pitch", "outputBusVolume", "pan", "mute", "solo"}) {
            const QString key = QLatin1String(name);
            if (!args.contains(key)) continue;
            changed.append(key);
            after.insert(key, args.value(key));
            if (key == "pan" || key == "mute" || key == "solo") {
                rollbackStep->insert(key, before.value(key));
                continue;
            }
            const auto* field = PlayablePropertyAuthoringV1::fieldByName(key);
            const auto state = locator && field
                ? PlayablePropertyAuthoringV1::readProperty(*item, *locator, field->id, std::nullopt)
                : std::nullopt;
            if (!state) {
                if (error) *error = "Unable to capture authored bus property: " + key;
                return false;
            }
            playable.insert(key, QJsonArray{QJsonObject{{"present", state->present}, {"value", state->value}}});
        }
        if (changed.isEmpty()) {
            if (error) *error = "set_bus_properties requires at least one supported property.";
            return false;
        }
        rollbackStep->insert("playable", playable);

        (*previewEntry)["bus_id"] = static_cast<qint64>(busID);
        (*previewEntry)["bus_name"] = item->name;
        (*previewEntry)["changed_fields"] = changed;
        (*previewEntry)["before"] = before;
        (*previewEntry)["after"] = after;
        return true;
    }

    if (cmd == QLatin1String("set_send") ||
        cmd == QLatin1String("remove_send") ||
        cmd == QLatin1String("set_send_level"))
    {
        const QString srcBusName = args.value("src_bus").toString().trimmed();
        if (srcBusName.isEmpty())
        {
            if (error) *error = QString("%1 requires src_bus.").arg(cmd);
            return false;
        }
        if (!args.contains("target_bus_id"))
        {
            if (error) *error = QString("%1 requires target_bus_id.").arg(cmd);
            return false;
        }

        auto& pm = ProjectManager::instance();
        auto* srcItem = findItemByPathInProject(pm.currentProject(), srcBusName);
        const auto* srcMix = srcItem ? srcItem->comp<VMixNode>() : nullptr;
        if (!srcItem || !srcMix)
        {
            if (error) *error = QString("Send source '%1' not found").arg(srcBusName);
            return false;
        }
        const QString srcName = srcItem->name;
        const QString srcSelector = srcItem->guid.toString(QUuid::WithoutBraces);
        const uint32_t srcID  = srcItem->objectID;

        const uint32_t targetBusID = static_cast<uint32_t>(args.value("target_bus_id").toInteger());
        const auto* targetBC = pm.findVBus(targetBusID);
        if (!targetBC)
        {
            if (error) *error = QString("Send target bus %1 not found").arg(targetBusID);
            return false;
        }

        bool sendExists = false;
        Send oldSend;
        const auto& srcSends = srcMix->sends;
        for (const auto& send : srcSends)
        {
            if (send.targetBusID == targetBusID && send.mode == SendMode::Mix)
            {
                sendExists = true;
                oldSend = send;
                break;
            }
        }

        const auto before = captureBusSendState(*srcItem);
        if (!before) {
            if (error) *error = "Unable to read Send configuration.";
            return false;
        }
        *rollbackStep = QJsonObject{
            {"kind", "restore_sends"},
            {"bus_guid", srcSelector},
            {"present", before->value.present},
            {"value", before->value.value}
        };

        if (cmd == QLatin1String("set_send"))
        {
            const float newLevel = static_cast<float>(args.value("level").toDouble(1.0));

            (*previewEntry)["src_bus"] = srcName;
            (*previewEntry)["src_bus_id"] = static_cast<qint64>(srcID);
            (*previewEntry)["target_bus_id"] = static_cast<qint64>(targetBusID);
            (*previewEntry)["before_exists"] = sendExists;
            if (sendExists)
            {
                (*previewEntry)["before"] = QJsonObject{
                    {"level", oldSend.sendLevel}
                };
            }
            (*previewEntry)["after"] = QJsonObject{
                {"level", newLevel}
            };
            return true;
        }

        if (!sendExists)
        {
            if (error) *error = QString("Send %1 -> %2 not found").arg(srcName).arg(targetBusID);
            return false;
        }

        if (cmd == QLatin1String("remove_send"))
        {
            (*previewEntry)["src_bus"] = srcName;
            (*previewEntry)["src_bus_id"] = static_cast<qint64>(srcID);
            (*previewEntry)["target_bus_id"] = static_cast<qint64>(targetBusID);
            (*previewEntry)["before"] = QJsonObject{
                {"level", oldSend.sendLevel}
            };
            (*previewEntry)["after"] = QJsonObject{{"removed", true}};
            return true;
        }

        const float newLevel = static_cast<float>(args.value("level").toDouble(1.0));

        (*previewEntry)["src_bus"] = srcName;
        (*previewEntry)["src_bus_id"] = static_cast<qint64>(srcID);
        (*previewEntry)["target_bus_id"] = static_cast<qint64>(targetBusID);
        (*previewEntry)["before"] = QJsonObject{{"level", oldSend.sendLevel}};
        (*previewEntry)["after"] = QJsonObject{{"level", newLevel}};
        return true;
    }

    if (cmd == QLatin1String("create_bus"))
    {
        const QString name = args.value("name").toString("New Bus").trimmed();
        if (name.isEmpty())
        {
            if (error) *error = "create_bus requires name.";
            return false;
        }

        if (args.contains("parent_bus_id"))
        {
            const uint32_t parentBusID = static_cast<uint32_t>(args.value("parent_bus_id").toInteger());
            auto* parentItem = ProjectManager::instance().findBusItem(parentBusID);
            if (!parentItem)
            {
                if (error) *error = QString("Parent bus ID %1 not found").arg(parentBusID);
                return false;
            }
            (*previewEntry)["parent_bus_id"] = static_cast<qint64>(parentBusID);
            (*previewEntry)["parent_bus_name"] = parentItem->name;
        }

        *rollbackStep = QJsonObject{
            {"kind", "deferred_create_bus"}
        };
        (*previewEntry)["name"] = name;
        (*previewEntry)["after"] = QJsonObject{{"create_bus", true}};
        return true;
    }

    if (cmd == QLatin1String("add_child_to_container") ||
        cmd == QLatin1String("remove_child_from_container"))
    {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project)
        {
            if (error) *error = "No project open.";
            return false;
        }

        const QString containerPath = args.value("container_path").toString().trimmed();
        const uint32_t containerID = static_cast<uint32_t>(args.value("container_id").toInteger(0));
        const QString childPath = args.value("child_path").toString().trimmed();
        const uint32_t childID = static_cast<uint32_t>(args.value("child_id").toInteger(0));

        if ((containerPath.isEmpty() && containerID == 0) ||
            (childPath.isEmpty() && childID == 0))
        {
            if (error) *error = QString("%1 requires container and child identifiers.").arg(cmd);
            return false;
        }

        ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
        ProjectItem* child = resolveItemByPathOrObjectID(project, childPath, childID);
        if (!container || !child)
        {
            if (error) *error = "Container or child not found.";
            return false;
        }
        if (container->type == ProjectItem::Type::Bus)
        {
            if (error) *error = "Use bus commands (move_bus / create_bus) for bus topology, not container commands";
            return false;
        }
        if (!ItemTypeCatalog::canBeChildOf(child->type, container->type))
        {
            if (error) *error = QString("'%1' cannot contain '%2'")
                                    .arg(buildItemFullPath(project, container),
                                         buildItemFullPath(project, child));
            return false;
        }
        if (cmd == QLatin1String("remove_child_from_container") && child->parent != container)
        {
            if (error) *error = "Child is not currently under the specified container.";
            return false;
        }
        if (!child->parent)
        {
            if (error) *error = "Child has no parent to restore.";
            return false;
        }

        ProjectItem* oldParent = child->parent;

        *rollbackStep = QJsonObject{
            {"kind", "restore_item_parent"},
            {"child_id", static_cast<qint64>(child->objectID)},
            {"old_parent_id", static_cast<qint64>(oldParent->objectID)}
        };
        const auto position = std::find_if(oldParent->children.begin(), oldParent->children.end(),
            [child](const auto& entry) { return entry.get() == child; });
        (*rollbackStep)["old_child_index"] = static_cast<qint64>(
            std::distance(oldParent->children.begin(), position));
        if (const auto* selection = oldParent->comp<VSelection>())
            (*rollbackStep)["old_selection"] = selection->toJson();
        if (const auto* timeline = oldParent->comp<VTrack>())
            (*rollbackStep)["old_clips"] = timeline->clips.toJsonObject();
        if (cmd == QLatin1String("add_child_to_container") && container != oldParent)
            if (const auto* selection = container->comp<VSelection>())
                (*rollbackStep)["destination_selection"] = selection->toJson();
        (*previewEntry)["container_id"] = static_cast<qint64>(itemShortID(container));
        (*previewEntry)["container_authoring_handle"] = static_cast<qint64>(container->objectID);
        (*previewEntry)["container_path"] = buildItemFullPath(project, container);
        (*previewEntry)["child_id"] = static_cast<qint64>(itemShortID(child));
        (*previewEntry)["child_authoring_handle"] = static_cast<qint64>(child->objectID);
        (*previewEntry)["child_path"] = buildItemFullPath(project, child);
        (*previewEntry)["before_parent_id"] = static_cast<qint64>(itemShortID(oldParent));
        (*previewEntry)["before_parent_authoring_handle"] = static_cast<qint64>(oldParent->objectID);
        (*previewEntry)["before_parent_path"] = buildItemFullPath(project, oldParent);

        if (cmd == QLatin1String("add_child_to_container"))
        {
            (*previewEntry)["after_parent_id"] = static_cast<qint64>(itemShortID(container));
            (*previewEntry)["after_parent_authoring_handle"] = static_cast<qint64>(container->objectID);
            (*previewEntry)["after_parent_path"] = buildItemFullPath(project, container);
        }
        else
        {
            ProjectItem* fallbackParent = container->parent;
            if (!fallbackParent)
            {
                if (error) *error = "Container has no parent to detach child into.";
                return false;
            }
            (*previewEntry)["after_parent_id"] = static_cast<qint64>(itemShortID(fallbackParent));
            (*previewEntry)["after_parent_authoring_handle"] = static_cast<qint64>(fallbackParent->objectID);
            (*previewEntry)["after_parent_path"] = buildItemFullPath(project, fallbackParent);
        }
        return true;
    }

    if (cmd == QLatin1String("set_container_property"))
    {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project)
        {
            if (error) *error = "No project open.";
            return false;
        }

        const QString containerPath = args.value("container_path").toString().trimmed();
        const uint32_t containerID = static_cast<uint32_t>(args.value("container_id").toInteger(0));
        const QString propertyName = args.value("property").toString().trimmed();

        if ((containerPath.isEmpty() && containerID == 0) || propertyName.isEmpty())
        {
            if (error) *error = "set_container_property requires container and property.";
            return false;
        }

        ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
        if (!container || container->type == ProjectItem::Type::Bus
            || !ItemTypeCatalog::canAcceptAnyChild(container->type))
        {
            if (error) *error = "Container not found or invalid.";
            return false;
        }

        if (propertyName != QLatin1String("volume") && propertyName != QLatin1String("pitch")) {
            if (error) *error = "Use field group commands to edit Output, Send, or Spatial Rating";
            return false;
        }
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*container);
        const auto state = locator && field
            ? PlayablePropertyAuthoringV1::readProperty(*container, *locator, field->id, std::nullopt)
            : std::nullopt;
        if (!state) {
            if (error) *error = "Unable to capture authored container property: " + propertyName;
            return false;
        }
        QJsonArray snapshots{QJsonObject{{"present", state->present}, {"value", state->value}}};
        for (const auto& platform : project->platforms) {
            const auto overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
                *container, *locator, field->id, platform.id);
            if (!overlay) {
                if (error) *error = "Unable to capture platform container property: " + propertyName;
                return false;
            }
            snapshots.append(QJsonObject{{"platform_id", platform.id.toString(QUuid::WithoutBraces)},
                {"present", overlay->present}, {"value", overlay->value}});
        }
        const QJsonObject before{{"value", state->value}, {"present", state->present}};
        const QJsonObject after{{"value", args.value("value")}, {"present", true}};
        *rollbackStep = QJsonObject{
            {"kind", "restore_playable_properties"},
            {"item_guid", container->guid.toString(QUuid::WithoutBraces)},
            {"playable", QJsonObject{{propertyName, snapshots}}}
        };

        (*previewEntry)["container_id"] = static_cast<qint64>(itemShortID(container));
        (*previewEntry)["container_authoring_handle"] = static_cast<qint64>(container->objectID);
        (*previewEntry)["container_path"] = buildItemFullPath(project, container);
        (*previewEntry)["property"] = propertyName;
        (*previewEntry)["before"] = before;
        (*previewEntry)["after"] = after;
        return true;
    }

    if (cmd == QLatin1String("reorder_container_children"))
    {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project)
        {
            if (error) *error = "No project open.";
            return false;
        }

        const QString containerPath = args.value("container_path").toString().trimmed();
        const uint32_t containerID = static_cast<uint32_t>(args.value("container_id").toInteger(0));
        const QJsonArray orderNames = args.value("child_order").toArray();
        const QJsonArray orderIDs = args.value("child_order_ids").toArray();

        if ((containerPath.isEmpty() && containerID == 0) || (orderNames.isEmpty() && orderIDs.isEmpty()))
        {
            if (error) *error = "reorder_container_children requires container and order list.";
            return false;
        }

        ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
        if (!container || container->type == ProjectItem::Type::Bus
            || !ItemTypeCatalog::canAcceptAnyChild(container->type))
        {
            if (error) *error = "Container not found or invalid.";
            return false;
        }

        QJsonArray oldOrderIDs;
        QJsonArray oldOrderGuids;
        for (const auto& child : container->children)
        {
            oldOrderIDs.append(static_cast<qint64>(itemShortID(child.get())));
            oldOrderGuids.append(child->guid.toString(QUuid::WithoutBraces));
        }

        QJsonArray newOrderIDs;
        QSet<QUuid> selected;
        if (!orderIDs.isEmpty())
        {
            for (const QJsonValue& val : orderIDs)
            {
                const uint32_t childObjectID = static_cast<uint32_t>(val.toInteger(0));
                if (std::count_if(container->children.begin(), container->children.end(),
                        [childObjectID](const auto& child) { return itemShortID(child.get()) == childObjectID; }) > 1) {
                    if (error) *error = "Ambiguous child name hash; use GUIDs in child_order.";
                    return false;
                }
                auto it = std::find_if(container->children.begin(), container->children.end(),
                    [childObjectID](const std::shared_ptr<ProjectItem>& child) { return itemShortID(child.get()) == childObjectID; });
                if (it == container->children.end())
                {
                    if (error) *error = QString("Child object_id %1 not found in container").arg(childObjectID);
                    return false;
                }
                if (selected.contains((*it)->guid))
                {
                    if (error) *error = QString("Duplicate child object_id %1 in child_order_ids").arg(childObjectID);
                    return false;
                }
                selected.insert((*it)->guid);
                newOrderIDs.append(static_cast<qint64>(childObjectID));
            }
        }
        else
        {
            for (const QJsonValue& val : orderNames)
            {
                const QString childRef = val.toString();
                auto matches = project->findItemsBySelector(childRef);
                std::erase_if(matches, [container](const auto* child) { return child->parent != container; });
                if (matches.size() > 1) {
                    if (error) *error = "Ambiguous child name or path; use GUIDs in child_order.";
                    return false;
                }
                auto it = std::find_if(container->children.begin(), container->children.end(),
                    [&matches](const std::shared_ptr<ProjectItem>& child) {
                        return matches.size() == 1 && child.get() == matches.front();
                    });
                if (it == container->children.end())
                {
                    if (error) *error = QString("Child '%1' not found in container").arg(childRef);
                    return false;
                }
                const uint32_t childShortID = itemShortID(it->get());
                if (selected.contains((*it)->guid))
                {
                    if (error) *error = QString("Duplicate child '%1' in child_order").arg(childRef);
                    return false;
                }
                selected.insert((*it)->guid);
                newOrderIDs.append(static_cast<qint64>(childShortID));
            }
        }

        for (const auto& child : container->children)
        {
            const uint32_t childShortID = itemShortID(child.get());
            if (!selected.contains(child->guid))
            {
                newOrderIDs.append(static_cast<qint64>(childShortID));
            }
        }

        *rollbackStep = QJsonObject{
            {"kind", "command"},
            {"cmd", "reorder_container_children"},
            {"args", QJsonObject{
                {"container_path", container->guid.toString(QUuid::WithoutBraces)},
                {"child_order", oldOrderGuids}
            }}
        };

        (*previewEntry)["container_id"] = static_cast<qint64>(itemShortID(container));
        (*previewEntry)["container_authoring_handle"] = static_cast<qint64>(container->objectID);
        (*previewEntry)["container_path"] = buildItemFullPath(project, container);
        (*previewEntry)["before_order_ids"] = oldOrderIDs;
        (*previewEntry)["after_order_ids"] = newOrderIDs;
        return true;
    }

    if (error) *error = QString("Command '%1' is not supported in transactional preview").arg(cmd);
    return false;
}


bool IPCServer::executeRollbackStep(const QJsonObject& step, QString* error)
{
    const QString kind = step.value("kind").toString();

    if (kind == QLatin1String("restore_playable_properties")) {
        auto& manager = ProjectManager::instance();
        auto* item = manager.findItemByGuid(QUuid(step.value("item_guid").toString()));
        const auto locator = item ? PlayablePropertyAuthoringV1::definitionLocator(*item) : std::nullopt;
        if (!item || !locator) {
            if (error) *error = "Unable to find the item for property rollback.";
            return false;
        }
        const auto playable = step.value("playable").toObject();
        for (auto it = playable.begin(); it != playable.end(); ++it) {
            const auto* field = PlayablePropertyAuthoringV1::fieldByName(it.key());
            for (const auto& snapshot : it.value().toArray()) {
                const auto value = snapshot.toObject();
                const PlayablePropertyStateV1 state{value.value("present").toBool(), value.value("value")};
                const PlatformScope scope = value.contains("platform_id")
                    ? PlatformScope{QUuid(value.value("platform_id").toString())} : std::nullopt;
                if (!field || !PlayablePropertyAuthoringV1::restoreProperty(
                        *item, *locator, field->id, state, scope)) {
                    if (error) *error = "Unable to restore authored property: " + it.key();
                    return false;
                }
            }
            item->markDirty();
            if (item->type == ProjectItem::Type::Bus) {
                manager.pushBusPropertyLive(item, it.key());
                emit manager.busChanged(item->objectID, it.key());
            } else {
                ProjectManager::syncPlayablePropertyToRuntime(item, field->id);
                emit manager.itemPropertyChanged(item, it.key());
            }
        }
        if ((step.contains("pan") && !manager.setBusPanInternal(item->objectID, step.value("pan").toDouble()))
            || (step.contains("mute") && !manager.setBusMuteInternal(item->objectID, step.value("mute").toBool()))
            || (step.contains("solo") && !manager.setBusSoloInternal(item->objectID, step.value("solo").toBool()))) {
            if (error) *error = "Unable to restore bus pan or audition state.";
            return false;
        }
        return true;
    }

    if (kind == QLatin1String("restore_sends")) {
        auto& manager = ProjectManager::instance();
        const QUuid guid(step.value("bus_guid").toString());
        auto* item = manager.findItemByGuid(guid);
        const BusSendState state{guid, {}, {
            step.value("present").toBool(), step.value("value")}};
        if (!item || !restoreBusSendState(*item, state)) {
            if (error) *error = "Unable to restore Send configuration.";
            return false;
        }
        item->markDirty();
        ProjectManager::syncPlayablePropertyToRuntime(item, static_cast<uint8_t>(
            harkenplayableproperties::PlayablePropertyIdV1::Sends));
        emit manager.busChanged(item->objectID, QStringLiteral("send"));
        return true;
    }

    if (kind == QLatin1String("command"))
    {
        const QString cmd = step.value("cmd").toString();
        QJsonObject args = step.value("args").toObject();
        args.remove("preview");
        const QJsonObject response = handleCommand(cmd, args);
        if (response.value("status").toString() != QLatin1String("ok"))
        {
            if (error)
            {
                *error = QString("Rollback command '%1' failed: [%2] %3")
                             .arg(cmd, response.value("code").toString(), response.value("message").toString());
            }
            return false;
        }
        return true;
    }

    if (kind == QLatin1String("restore_item_parent"))
    {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project)
        {
            if (error) *error = "No project open.";
            return false;
        }

        const uint32_t childID = static_cast<uint32_t>(step.value("child_id").toInteger());
        const uint32_t oldParentID = static_cast<uint32_t>(step.value("old_parent_id").toInteger());

        ProjectItem* child = findItemByAuthoringHandleInProject(project, childID);
        ProjectItem* oldParent = findItemByAuthoringHandleInProject(project, oldParentID);
        if (!child || !oldParent)
        {
            if (error) *error = "Rollback restore_item_parent failed: child or old parent not found.";
            return false;
        }

        ProjectItem* currentParent = child->parent;
        if (currentParent != oldParent)
        {
            if (!ProjectManager::instance().moveItemInternal(
                    child, oldParent, step.value("old_child_index").toInt(-1), std::nullopt,
                    nullptr, nullptr, nullptr, true, nullptr, false)) {
                if (error) *error = "Rollback restore_item_parent failed: moveItemInternal returned false.";
                return false;
            }
            if (auto* selection = oldParent->comp<VSelection>()) {
                if (!selection->fromJson(step.value("old_selection").toObject())) return false;
                for (const auto* property : {"selections", "behavior", "transitions"})
                    emit ProjectManager::instance().itemPropertyChanged(oldParent, QLatin1String(property));
            }
            if (auto* timeline = oldParent->comp<VTrack>()) {
                auto clips = ClipLedger::fromJson(step.value("old_clips").toObject(), *oldParent,
                    [](const QUuid& guid) { return ProjectManager::instance().findItemByGuid(guid); }, error);
                if (!clips) return false;
                clips->occurrenceOrdinalHighWater = std::max(
                    clips->occurrenceOrdinalHighWater, timeline->clips.occurrenceOrdinalHighWater);
                timeline->clips = std::move(*clips);
                emit ProjectManager::instance().itemPropertyChanged(oldParent, QStringLiteral("clips"));
            }
            if (currentParent && currentParent != oldParent)
            {
                if (step.contains("destination_selection")) {
                    auto* selection = currentParent->comp<VSelection>();
                    if (!selection || !selection->fromJson(step.value("destination_selection").toObject()))
                        return false;
                    emit ProjectManager::instance().itemPropertyChanged(currentParent, QStringLiteral("selections"));
                }
                ProjectManager::resyncItemToRuntime(currentParent);
            }
            emit ProjectManager::instance().itemMoved(child, currentParent, oldParent);
        }

        ProjectManager::resyncItemToRuntime(oldParent);
        return true;
    }

    if (kind == QLatin1String("deferred_create_bus"))
    {
        return true;
    }

    if (error) *error = QString("Unknown rollback step kind '%1'").arg(kind);
    return false;
}

}
