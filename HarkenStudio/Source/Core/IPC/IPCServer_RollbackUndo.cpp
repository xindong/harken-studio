
#include "IPCServerInternal.h"
#include "../Actions/MixerActions.h"

#include "ItemTypeCatalog.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"

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
        const auto* item = pm.findBusItem(busID);
        const auto* bc   = item ? item->comp<VBus>() : nullptr;
        const auto* mix  = item ? item->comp<VMixNode>() : nullptr;
        if (!bc || !mix || !item)
        {
            if (error) *error = QString("Bus ID %1 not found").arg(busID);
            return false;
        }

        const bool hasAnyProperty =
            args.contains("volume") || args.contains("pan") || args.contains("mute") || args.contains("solo");
        if (!hasAnyProperty)
        {
            if (error) *error = "set_bus_properties requires at least one of volume/pan/mute/solo.";
            return false;
        }

        const auto& overlay = pm.auditionOverlay();
        QJsonObject rollbackArgs{
            {"bus_id", static_cast<qint64>(busID)},
            {"volume", mix->volume},
            {"pan", bc->pan},
            {"mute", overlay.isMuted(item->runtimeId())},
            {"solo", overlay.isSoloed(item->runtimeId())}
        };
        *rollbackStep = QJsonObject{
            {"kind", "command"},
            {"cmd", "set_bus_properties"},
            {"args", rollbackArgs}
        };

        QJsonObject before{
            {"volume", mix->volume},
            {"pan", bc->pan},
            {"mute", overlay.isMuted(item->runtimeId())},
            {"solo", overlay.isSoloed(item->runtimeId())}
        };
        QJsonObject after = before;
        if (args.contains("volume")) after["volume"] = args.value("volume");
        if (args.contains("pan")) after["pan"] = args.value("pan");
        if (args.contains("mute")) after["mute"] = args.value("mute");
        if (args.contains("solo")) after["solo"] = args.value("solo");

        QJsonArray changed;
        if (args.contains("volume")) changed.append("volume");
        if (args.contains("pan")) changed.append("pan");
        if (args.contains("mute")) changed.append("mute");
        if (args.contains("solo")) changed.append("solo");

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
            if (send.targetBusID == targetBusID)
            {
                sendExists = true;
                oldSend = send;
                break;
            }
        }

        if (cmd == QLatin1String("set_send"))
        {
            const float newLevel = static_cast<float>(args.value("level").toDouble(1.0));

            if (sendExists)
            {
                *rollbackStep = QJsonObject{
                    {"kind", "command"},
                    {"cmd", "set_send"},
                    {"args", QJsonObject{
                        {"src_bus", srcSelector},
                        {"target_bus_id", static_cast<qint64>(targetBusID)},
                        {"level", oldSend.sendLevel}
                    }}
                };
            }
            else
            {
                *rollbackStep = QJsonObject{
                    {"kind", "command"},
                    {"cmd", "remove_send"},
                    {"args", QJsonObject{
                        {"src_bus", srcSelector},
                        {"target_bus_id", static_cast<qint64>(targetBusID)}
                    }}
                };
            }

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
            *rollbackStep = QJsonObject{
                {"kind", "command"},
                {"cmd", "set_send"},
                {"args", QJsonObject{
                        {"src_bus", srcSelector},
                    {"target_bus_id", static_cast<qint64>(targetBusID)},
                    {"level", oldSend.sendLevel}
                }}
            };

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
        *rollbackStep = QJsonObject{
            {"kind", "command"},
            {"cmd", "set_send_level"},
            {"args", QJsonObject{
                        {"src_bus", srcSelector},
                {"target_bus_id", static_cast<qint64>(targetBusID)},
                {"level", oldSend.sendLevel}
            }}
        };

        (*previewEntry)["src_bus"] = srcName;
        (*previewEntry)["src_bus_id"] = static_cast<qint64>(srcID);
        (*previewEntry)["target_bus_id"] = static_cast<qint64>(targetBusID);
        (*previewEntry)["before"] = QJsonObject{{"level", oldSend.sendLevel}};
        (*previewEntry)["after"] = QJsonObject{{"level", newLevel}};
        return true;
    }

    if (cmd == QLatin1String("create_bus"))
    {
        const QString name = args.value("name").toString().trimmed();
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
        const double value = args.value("value").toDouble(0.0);

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

        QJsonObject rollbackArgs{
            {"container_id", static_cast<qint64>(itemShortID(container))},
            {"property", propertyName}
        };
        QJsonObject before;
        QJsonObject after;

        bool isCommonSpatialProp = false;
        auto* cSpatial = container->comp<VSpatial>();
        if (propertyName == QLatin1String("spatializationMix") && cSpatial)
        {
            rollbackArgs = QJsonObject{
                {"authoring_handle", static_cast<qint64>(container->objectID)},
                {"property", "spatializationMix"},
                {"override_operation", cSpatial->spatializationMixPresent
                    ? "set_override" : "clear_override"},
                {"value", cSpatial->spatializationMix}};
            before["value"] = cSpatial->spatializationMix;
            before["present"] = cSpatial->spatializationMixPresent;
            after["value"] = static_cast<float>(value);
            after["present"] = true;
            isCommonSpatialProp = true;
        }
        else if (propertyName == QLatin1String("is3DEnabled") && cSpatial)
        {
            rollbackArgs["value"] = cSpatial->is3DEnabled ? 1.0 : 0.0;
            before["value"] = cSpatial->is3DEnabled;
            after["value"] = (value != 0.0);
            isCommonSpatialProp = true;
        }

        if (!isCommonSpatialProp)
        {
            if (error) *error = "Container type is not supported for transactional property updates.";
            return false;
        }

        *rollbackStep = QJsonObject{
            {"kind", "command"},
            {"cmd", propertyName == QLatin1String("spatializationMix")
                ? "set_item_property" : "set_container_property"},
            {"args", rollbackArgs}
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
