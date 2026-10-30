
#include "../ActionManager.h"
#include "../AuthoringPolicy.h"
#include "../ContainerBehaviorSemantics.h"
#include "../ItemTypeCatalog.h"
#include "../PlayablePropertyAuthoringV1.h"
#include "../FieldGroups.h"
#include "../ProjectManager.h"
#include "../UndoCommands.h"
#include "../Components/VMixNode.h"
#include "../Components/VSelection.h"
#include "../Components/VSpatial.h"
#include "ActionSelectors.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace VNX {

ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);
ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
uint32_t itemShortID(const ProjectItem* item);
ProjectItem* resolveItemByPathOrObjectID(ProjectData* project,
                                         const QString& path,
                                         uint32_t objectID);
QString buildItemFullPath(ProjectData* project, ProjectItem* item);
QString itemTypeToString(ProjectItem::Type type);
ActionResult handlePlayableItemProperty(ProjectManager*, ProjectItem*, const QString&,
                                       const QJsonObject&, const QObject*);

void registerContainerActions(ActionManager& mgr,
                              ProjectManager* projectManager)
{

    {
        ActionDef def;
        def.id = "add_child_to_container";
        def.displayName = "Add Child to Container";
        def.category = "container";
        def.paramSchema = QJsonObject{{"container_path", "string"}, {"container_id", "uint32"}, {"child_path", "string"}, {"child_id", "uint32"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString containerPath = args["container_path"].toString();
            if (!containerPath.isEmpty()) {
                const auto sel = resolveProjectSelector(projectManager->currentProject(), containerPath);
                if (!sel.ok) return {false, {{"candidates", sel.candidates}}, sel.errorCode, sel.errorMessage};
                containerPath = sel.item->guid.toString(QUuid::WithoutBraces);
            }
            uint32_t containerID = static_cast<uint32_t>(args["container_id"].toInteger(0));
            QString childPath = args["child_path"].toString();
            if (!childPath.isEmpty()) {
                const auto sel = resolveProjectSelector(projectManager->currentProject(), childPath);
                if (!sel.ok) return {false, {{"candidates", sel.candidates}}, sel.errorCode, sel.errorMessage};
                childPath = sel.item->guid.toString(QUuid::WithoutBraces);
            }
            uint32_t childID = static_cast<uint32_t>(args["child_id"].toInteger(0));

            if ((containerPath.isEmpty() && containerID == 0) ||
                (childPath.isEmpty() && childID == 0))
                return {false, {}, "INVALID_ARGS",
                        "container_path/container_id and child_path/child_id are required"};

            ProjectData* project = projectManager->currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};
            if (!containerPath.isEmpty() && containerID != 0) {
                ProjectItem* byPath = findItemByPathInProject(project, containerPath);
                ProjectItem* byID = findItemByObjectIDInProject(project, containerID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS",
                            "container_path and container_id refer to different items"};
            }
            if (!childPath.isEmpty() && childID != 0) {
                ProjectItem* byPath = findItemByPathInProject(project, childPath);
                ProjectItem* byID = findItemByObjectIDInProject(project, childID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS",
                            "child_path and child_id refer to different items"};
            }

            ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
            ProjectItem* child = resolveItemByPathOrObjectID(project, childPath, childID);

            if (!container)
                return {false, {},
                        "CONTAINER_NOT_FOUND",
                        containerID != 0
                            ? QString("Container with object_id '%1' not found").arg(containerID)
                            : QString("Container '%1' not found").arg(containerPath)};
            if (!child)
                return {false, {},
                        "CHILD_NOT_FOUND",
                        childID != 0
                            ? QString("Child with object_id '%1' not found").arg(childID)
                            : QString("Child '%1' not found").arg(childPath)};
            if (container->type == ProjectItem::Type::Bus)
                return {false, {}, "USE_BUS_API",
                        "Use bus commands (move_bus / create_bus) for bus topology, not container commands"};
            if (!ItemTypeCatalog::canBeChildOf(child->type, container->type))
                return {false, {}, "NOT_A_CONTAINER",
                        QString("'%1' cannot contain '%2'")
                            .arg(buildItemFullPath(project, container),
                                 buildItemFullPath(project, child))};

            if (!projectManager->moveItem(child, container))
                return {false, {}, "MOVE_ITEM_FAILED",
                        QString("Failed to move '%1' under container '%2'")
                            .arg(buildItemFullPath(project, child),
                                 buildItemFullPath(project, container))};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success", true},
                {"container_id", static_cast<qint64>(itemShortID(container))},
                {"container_authoring_handle", static_cast<qint64>(container->objectID)},
                {"child_id", static_cast<qint64>(itemShortID(child))},
                {"child_authoring_handle", static_cast<qint64>(child->objectID)},
                {"container_path", buildItemFullPath(project, container)},
                {"child_path", buildItemFullPath(project, child)}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "remove_child_from_container";
        def.displayName = "Remove Child from Container";
        def.category = "container";
        def.paramSchema = QJsonObject{{"container_path", "string"}, {"container_id", "uint32"}, {"child_path", "string"}, {"child_id", "uint32"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString containerPath = args["container_path"].toString();
            if (!containerPath.isEmpty()) {
                const auto sel = resolveProjectSelector(projectManager->currentProject(), containerPath);
                if (!sel.ok) return {false, {{"candidates", sel.candidates}}, sel.errorCode, sel.errorMessage};
                containerPath = sel.item->guid.toString(QUuid::WithoutBraces);
            }
            uint32_t containerID = static_cast<uint32_t>(args["container_id"].toInteger(0));
            QString childPath = args["child_path"].toString();
            if (!childPath.isEmpty()) {
                const auto sel = resolveProjectSelector(projectManager->currentProject(), childPath);
                if (!sel.ok) return {false, {{"candidates", sel.candidates}}, sel.errorCode, sel.errorMessage};
                childPath = sel.item->guid.toString(QUuid::WithoutBraces);
            }
            uint32_t childID = static_cast<uint32_t>(args["child_id"].toInteger(0));

            if ((containerPath.isEmpty() && containerID == 0) ||
                (childPath.isEmpty() && childID == 0))
                return {false, {}, "INVALID_ARGS",
                        "container_path/container_id and child_path/child_id are required"};

            ProjectData* project = projectManager->currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};
            if (!containerPath.isEmpty() && containerID != 0) {
                ProjectItem* byPath = findItemByPathInProject(project, containerPath);
                ProjectItem* byID = findItemByObjectIDInProject(project, containerID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS",
                            "container_path and container_id refer to different items"};
            }
            if (!childPath.isEmpty() && childID != 0) {
                ProjectItem* byPath = findItemByPathInProject(project, childPath);
                ProjectItem* byID = findItemByObjectIDInProject(project, childID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS",
                            "child_path and child_id refer to different items"};
            }

            ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
            ProjectItem* child = resolveItemByPathOrObjectID(project, childPath, childID);

            if (!container)
                return {false, {},
                        "CONTAINER_NOT_FOUND",
                        containerID != 0
                            ? QString("Container with object_id '%1' not found").arg(containerID)
                            : QString("Container '%1' not found").arg(containerPath)};
            if (!child)
                return {false, {},
                        "CHILD_NOT_FOUND",
                        childID != 0
                            ? QString("Child with object_id '%1' not found").arg(childID)
                            : QString("Child '%1' not found").arg(childPath)};
            if (container->type == ProjectItem::Type::Bus)
                return {false, {}, "USE_BUS_API",
                        "Use bus commands (move_bus / create_bus) for bus topology, not container commands"};
            if (!ItemTypeCatalog::canBeChildOf(child->type, container->type))
                return {false, {}, "NOT_A_CONTAINER",
                        QString("'%1' cannot contain '%2'")
                            .arg(buildItemFullPath(project, container),
                                 buildItemFullPath(project, child))};

            auto& children = container->children;
            auto it = std::find_if(children.begin(), children.end(),
                [child](const std::shared_ptr<ProjectItem>& c) { return c.get() == child; });
            if (it == children.end())
                return {false, {}, "CHILD_NOT_IN_CONTAINER",
                        QString("'%1' is not a child of '%2'")
                            .arg(buildItemFullPath(project, child),
                                 buildItemFullPath(project, container))};

            ProjectItem* fallbackParent = container->parent;
            if (!fallbackParent)
                return {false, {}, "INVALID_CONTAINER_STATE",
                        QString("Container '%1' has no parent to receive detached child")
                            .arg(buildItemFullPath(project, container))};

            if (!projectManager->moveItem(child, fallbackParent))
                return {false, {}, "MOVE_ITEM_FAILED",
                        QString("Failed to detach '%1' from '%2'")
                            .arg(buildItemFullPath(project, child),
                                 buildItemFullPath(project, container))};

            ProjectManager::instance().requestResync(container);
            ProjectManager::instance().requestResync(fallbackParent);

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success", true},
                {"container_id", static_cast<qint64>(itemShortID(container))},
                {"container_authoring_handle", static_cast<qint64>(container->objectID)},
                {"child_id", static_cast<qint64>(itemShortID(child))},
                {"child_authoring_handle", static_cast<qint64>(child->objectID)},
                {"container_path", buildItemFullPath(project, container)},
                {"removed_child", buildItemFullPath(project, child)}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "reorder_container_children";
        def.displayName = "Reorder Container Children";
        def.category = "container";
        def.paramSchema = QJsonObject{{"container_path", "string"}, {"container_id", "uint32"}, {"child_order", "array"}, {"child_order_ids", "array"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString containerPath = args["container_path"].toString();
            if (!containerPath.isEmpty()) {
                const auto sel = resolveProjectSelector(projectManager->currentProject(), containerPath);
                if (!sel.ok) return {false, {{"candidates", sel.candidates}}, sel.errorCode, sel.errorMessage};
                containerPath = sel.item->guid.toString(QUuid::WithoutBraces);
            }
            uint32_t containerID = static_cast<uint32_t>(args["container_id"].toInteger(0));
            QJsonArray orderArray = args["child_order"].toArray();
            QJsonArray orderIDs = args["child_order_ids"].toArray();

            if ((containerPath.isEmpty() && containerID == 0) ||
                (orderArray.isEmpty() && orderIDs.isEmpty()))
                return {false, {}, "INVALID_ARGS",
                        "container_path/container_id and child_order/child_order_ids are required"};

            ProjectData* project = projectManager->currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};
            if (!containerPath.isEmpty() && containerID != 0) {
                ProjectItem* byPath = findItemByPathInProject(project, containerPath);
                ProjectItem* byID = findItemByObjectIDInProject(project, containerID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS",
                            "container_path and container_id refer to different items"};
            }

            ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
            if (!container)
                return {false, {},
                        "CONTAINER_NOT_FOUND",
                        containerID != 0
                            ? QString("Container with object_id '%1' not found").arg(containerID)
                            : QString("Container '%1' not found").arg(containerPath)};

            const std::vector<std::shared_ptr<ProjectItem>> oldOrder = container->children;

            std::vector<std::shared_ptr<ProjectItem>> newOrder;
            if (!orderIDs.isEmpty()) {
                for (const auto& val : orderIDs) {
                    const uint32_t childObjectID = static_cast<uint32_t>(val.toInteger(0));
                    if (std::count_if(container->children.begin(), container->children.end(),
                            [childObjectID](const auto& child) { return itemShortID(child.get()) == childObjectID; }) > 1)
                        return {false, {}, "AMBIGUOUS_SELECTOR", "Multiple children match this name hash; use GUIDs in child_order."};
                    auto cit = std::find_if(container->children.begin(), container->children.end(),
                        [childObjectID](const std::shared_ptr<ProjectItem>& c) {
                            return itemShortID(c.get()) == childObjectID;
                        });
                    if (cit != container->children.end()) {
                        newOrder.push_back(*cit);
                    } else {
                        return {false, {}, "CHILD_NOT_FOUND",
                                QString("Child with object_id '%1' not found in container")
                                    .arg(childObjectID)};
                    }
                }
            } else {
                for (const auto& val : orderArray) {
                    const QString childRef = val.toString();
                    auto matches = project->findItemsBySelector(childRef);
                    std::erase_if(matches, [container](const auto* child) { return child->parent != container; });
                    if (matches.size() > 1)
                        return {false, {}, "AMBIGUOUS_SELECTOR", "Multiple children match this name or path; use GUIDs in child_order."};
                    auto cit = std::find_if(container->children.begin(), container->children.end(),
                        [&matches](const std::shared_ptr<ProjectItem>& c) {
                            return matches.size() == 1 && c.get() == matches.front();
                        });
                    if (cit != container->children.end()) {
                        newOrder.push_back(*cit);
                    } else {
                        return {false, {}, "CHILD_NOT_FOUND",
                                QString("Child '%1' not found in container").arg(childRef)};
                    }
                }
            }

            for (auto& ch : container->children) {
                auto fit = std::find(newOrder.begin(), newOrder.end(), ch);
                if (fit == newOrder.end())
                    newOrder.push_back(ch);
            }

            if (!projectManager->reorderItemChildrenInternal(container, newOrder))
                return {false, {}, "REORDER_FAILED",
                        QString("Failed to reorder children for '%1'").arg(containerPath)};

            UndoRedoManager::instance().push(
                new SequenceReorderCommand(container, oldOrder, newOrder));

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success", true},
                {"container_id", static_cast<qint64>(itemShortID(container))},
                {"container_authoring_handle", static_cast<qint64>(container->objectID)},
                {"container_path", buildItemFullPath(project, container)},
                {"new_child_count", static_cast<int>(container->children.size())}
            };
            return r;
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_container_property";
        def.displayName = "Set Container Property";
        def.category = "container";
        def.paramSchema = QJsonObject{{"container_path", "string"}, {"container_id", "uint32"}, {"property", "string"}, {"value", "float"}};
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            QString containerPath = args["container_path"].toString();
            if (!containerPath.isEmpty()) {
                const auto sel = resolveProjectSelector(projectManager->currentProject(), containerPath);
                if (!sel.ok) return {false, {{"candidates", sel.candidates}}, sel.errorCode, sel.errorMessage};
                containerPath = sel.item->guid.toString(QUuid::WithoutBraces);
            }
            uint32_t containerID = static_cast<uint32_t>(args["container_id"].toInteger(0));
            QString propertyName = args["property"].toString();
            double value = args["value"].toDouble(0.0);

            if ((containerPath.isEmpty() && containerID == 0) || propertyName.isEmpty())
                return {false, {}, "INVALID_ARGS",
                        "container_path/container_id and property are required"};

            ProjectData* project = projectManager->currentProject();
            if (!project) return {false, {}, "NO_PROJECT_OPEN", "Project closed during command"};
            if (!containerPath.isEmpty() && containerID != 0) {
                ProjectItem* byPath = findItemByPathInProject(project, containerPath);
                ProjectItem* byID = findItemByObjectIDInProject(project, containerID);
                if (byPath && byID && byPath != byID)
                    return {false, {}, "INVALID_ARGS",
                            "container_path and container_id refer to different items"};
            }

            ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);
            if (!container)
                return {false, {},
                        "CONTAINER_NOT_FOUND",
                        containerID != 0
                            ? QString("Container with object_id '%1' not found").arg(containerID)
                            : QString("Container '%1' not found").arg(containerPath)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(container->type)) {
                const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
                if ((field && FieldGroupForProperty(
                        static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field->id)))
                    || propertyName == QStringLiteral("outputBusId")
                    || propertyName == QStringLiteral("outputBus")
                    || propertyName == QStringLiteral("output_bus"))
                    return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                            "Use field group commands to edit Main Output, Send, or Spatial Rating"};
            }

            QString stringValue = args["string_value"].toString();
            QJsonObject objectValue = args["object_value"].toObject();

            if (propertyName == QStringLiteral("spatializationMix")) {
                auto result = handlePlayableItemProperty(projectManager, container, propertyName,
                    args, ActionManager::instance().executeOriginator());
                if (!result.success) return result;
                result.data = QJsonObject{
                    {"success", true},
                    {"container_id", static_cast<qint64>(itemShortID(container))},
                    {"container_authoring_handle", static_cast<qint64>(container->objectID)},
                    {"container_path", buildItemFullPath(project, container)},
                    {"property", propertyName}, {"string_value", stringValue},
                    {"object_value", objectValue}, {"value", value}};
                return result;
            }

            QVariant undoOldValue;
            QVariant undoNewValue;
            bool undoTracked = false;

            bool handledCommon = false;
            auto* contSpatial = container->comp<VSpatial>();
            auto* contMix = container->compOrCreate<VMixNode>();
            static const QSet<QString> kSpatialBackedProps = {
                QStringLiteral("spatializationMix"),
                QStringLiteral("attenuationDistanceScaling"),
                QStringLiteral("is3DEnabled")};
            if (!contSpatial && kSpatialBackedProps.contains(propertyName))
                return {false, {}, "PROPERTY_NOT_SUPPORTED",
                        QString("Property '%1' is not supported on '%2'")
                            .arg(propertyName, container->name)};
            if (propertyName == "volume") {
                undoOldValue = contMix->volume; undoNewValue = static_cast<float>(value);
                contMix->volume = static_cast<float>(value);
                handledCommon = true; undoTracked = true;
            } else if (propertyName == "pitch") {
                undoOldValue = contMix->pitch; undoNewValue = static_cast<float>(value);
                contMix->pitch = static_cast<float>(value);
                handledCommon = true; undoTracked = true;
            } else if (propertyName == "attenuationDistanceScaling") {
                float ads = qBound(0.01f, static_cast<float>(value), 100.0f);
                undoOldValue = contSpatial->attenuationDistanceScaling; undoNewValue = ads;
                contSpatial->attenuationDistanceScaling = ads;
                handledCommon = true; undoTracked = true;
            } else if (propertyName == "is3DEnabled") {
                undoOldValue = contSpatial->is3DEnabled; undoNewValue = (value != 0.0);
                contSpatial->is3DEnabled = (value != 0.0);
                handledCommon = true; undoTracked = true;
            } else if (propertyName == "makeUpGain") {
                undoOldValue = contMix->makeUpGain; undoNewValue = static_cast<float>(value);
                if (projectManager->writeAudioPropertySilent(container, propertyName, static_cast<float>(value)))
                    handledCommon = true;
                undoTracked = true;
            } else if (propertyName == "outputBus") {
                uint32_t newBusId = static_cast<uint32_t>(value);
                undoOldValue = contMix->outputBusId; undoNewValue = newBusId;
                if (projectManager->writeAudioPropertySilent(container, propertyName, newBusId))
                    handledCommon = true;
                undoTracked = true;
            }

            if (!handledCommon)
                return {false, {}, "NOT_A_CONTAINER",
                        QString("'%1' is not a supported container type").arg(containerPath)};

            ProjectManager::resyncItemToRuntime(container);

            {
                if (propertyName == "pitch") {
                    projectManager->propagateItemPitchLive(container);
                } else if (propertyName == "volume") {
                    projectManager->propagateItemVolumeLive(container);
                }
            }

            container->markDirty();
            emit projectManager->itemPropertyChanged(container, propertyName);

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"success", true},
                {"container_id", static_cast<qint64>(itemShortID(container))},
                {"container_authoring_handle", static_cast<qint64>(container->objectID)},
                {"container_path", buildItemFullPath(project, container)},
                {"property", propertyName},
                {"string_value", stringValue},
                {"object_value", objectValue},
                {"value", value}
            };

            if (undoTracked) {
                auto applyFn = [propertyName](ProjectItem* item, const QVariant& val) {
                    auto* spatial = item->comp<VSpatial>();
                    auto* mixc = item->comp<VMixNode>();
                    if (propertyName == "volume" && mixc) mixc->volume = val.toFloat();
                    else if (propertyName == "pitch" && mixc) mixc->pitch = val.toFloat();
                    else if (propertyName == "attenuationDistanceScaling" && spatial) spatial->attenuationDistanceScaling = val.toFloat();
                    else if (propertyName == "is3DEnabled" && spatial) spatial->is3DEnabled = val.toBool();
                    else if (propertyName == "makeUpGain" || propertyName == "outputBus")
                        ProjectManager::instance().writeAudioPropertySilent(item, propertyName, val);
                    ProjectManager::resyncItemToRuntime(item);
                    item->markDirty();
                    emit ProjectManager::instance().itemPropertyChanged(item, propertyName);
                };
                uint32_t containerObjID = container->objectID;
                r.undoFn = [applyFn, containerObjID, undoOldValue]() {
                    auto* proj = ProjectManager::instance().currentProject();
                    if (!proj) return;
                    auto* ci = findItemByObjectIDInProject(proj, containerObjID);
                    if (!ci) return;
                    applyFn(ci, undoOldValue);
                };
                r.redoFn = [applyFn, containerObjID, undoNewValue]() {
                    auto* proj = ProjectManager::instance().currentProject();
                    if (!proj) return;
                    auto* ci = findItemByObjectIDInProject(proj, containerObjID);
                    if (!ci) return;
                    applyFn(ci, undoNewValue);
                };
                r.undoText = QString("Set %1").arg(propertyName);
                r.authoringWriteSet = QSet<QUuid>{container->guid};
            }

            return r;
        };
        mgr.registerAction(def);
    }
}

}
