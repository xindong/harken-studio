#include "IPCServerInternal.h"

namespace VNX {

void IPCServer::registerContainerHandlers()
{
    commandHandlers_["get_container_info"] = [this](const QJsonObject& args) { return handleGetContainerInfo(args); };
}

QJsonObject IPCServer::handleGetContainerInfo(const QJsonObject& args)
{
    if (!projectManager_ || !projectManager_->isProjectOpen())
    {
        return errorResponse("NO_PROJECT_OPEN", "No project is currently open");
    }

    QString containerPath = args["path"].toString();
    uint32_t containerID = static_cast<uint32_t>(args["object_id"].toInteger(0));

    if (containerPath.isEmpty() && containerID == 0)
    {
        return errorResponse("INVALID_ARGS", "path or object_id is required");
    }

    ProjectData* project = projectManager_->currentProject();
    if (!project) return errorResponse("NO_PROJECT_OPEN", "Project closed during command");
    if (!containerPath.isEmpty() && containerID != 0)
    {
        ProjectItem* byPath = findItemByPathInProject(project, containerPath);
        ProjectItem* byID = findItemByObjectIDInProject(project, containerID);
        if (byPath && byID && byPath != byID)
            return errorResponse("INVALID_ARGS", "path and object_id refer to different items");
    }
    ProjectItem* container = resolveItemByPathOrObjectID(project, containerPath, containerID);

    if (!container)
    {
        return errorResponse("CONTAINER_NOT_FOUND",
                             containerID != 0
                                 ? QString("Container with object_id '%1' not found").arg(containerID)
                                 : QString("Container '%1' not found").arg(containerPath));
    }

    QJsonArray children;
    for (const auto& child : container->children)
    {
        QJsonObject childObj;
        childObj["name"] = child->name;
        childObj["type"] = itemTypeToString(child->type);
        childObj["object_id"] = static_cast<qint64>(itemShortID(child.get()));
        childObj["authoring_handle"] = static_cast<qint64>(child->objectID);
        childObj["source_id"] = static_cast<qint64>(child->sourceID);
        childObj["path"] = buildItemFullPath(project, child.get());
        children.append(childObj);
    }

    QJsonObject result;
    result["name"] = container->name;
    result["type"] = itemTypeToString(container->type);
    result["object_id"] = static_cast<qint64>(itemShortID(container));
    result["authoring_handle"] = static_cast<qint64>(container->objectID);
    result["path"] = buildItemFullPath(project, container);
    result["asset_path"] = container->type == ProjectItem::Type::Sound ? container->sourcePath
        : container->parent ? QString{} : project->itemFolderPath(container);
    result["children"] = children;
    result["child_count"] = static_cast<int>(container->children.size());

    return successResponse(result);
}

}
