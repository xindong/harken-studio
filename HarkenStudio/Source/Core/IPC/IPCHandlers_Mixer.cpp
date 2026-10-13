#include "IPCServerInternal.h"
#include "../RuntimeU64Json.h"
#include "../Actions/ActionSelectors.h"

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

void IPCServer::registerMixerHandlers()
{
    commandHandlers_["get_mixer_levels"] = [this](const QJsonObject& args) { return handleGetMixerLevels(args); };

    commandHandlers_["get_bus_pitch"] = [this](const QJsonObject& args) {
        auto& pm = ProjectManager::instance();
        if (!pm.isProjectOpen())
            return errorResponse("NO_PROJECT", "No project open");
        const QString busName = args.value(QStringLiteral("bus_name"))
                                    .toString().trimmed();
        auto* item = pm.findBusItemByName(busName);
        const auto* mix = item ? item->comp<VMixNode>() : nullptr;
        if (!item || !item->comp<VBus>() || !mix)
            return errorResponse(
                "BUS_NOT_FOUND", "Bus not found: " + busName);
        return successResponse(QJsonObject{
            {"bus_id", objectIdString(item->runtimeId())},
            {"bus", busName},
            {"cents", mix->pitch}
        });
    };

    commandHandlers_["get_bus_volume_db"] = [this](const QJsonObject& args) {
        auto& pm = ProjectManager::instance();
        if (!pm.isProjectOpen()) return errorResponse("NO_PROJECT", "No project open");
        if (!engine_) return errorResponse("NO_ENGINE", "Engine not initialized");
        QString busName = args["bus_name"].toString().trimmed();
        auto* item = pm.findBusItemByName(busName);
        if (!item) return errorResponse("BUS_NOT_FOUND", "Bus not found: " + busName);
        const auto* bc = item->comp<VBus>();
        if (!bc) return errorResponse("BUS_NOT_FOUND", "Bus not found: " + busName);
        float db = engine_->getBusVolumeDbById(item->runtimeId());
        return successResponse(QJsonObject{
            {"bus_id", objectIdString(item->runtimeId())},
            {"bus", busName}, {"bus_volume_db", db}
        });
    };

    commandHandlers_["get_send_filter"] = [this](const QJsonObject& args) {
        auto& pm = ProjectManager::instance();
        if (!pm.isProjectOpen())
            return errorResponse("NO_PROJECT", "No project open");
        const QString srcBusName = args.value("src_bus").toString().trimmed();
        if (srcBusName.isEmpty())
            return errorResponse("INVALID_ARGS", "src_bus is required");
        if (!args.contains("slot"))
            return errorResponse("INVALID_ARGS", "slot (0-3) is required");

        const int32_t slot = args.value("slot").toInt(-1);
        if (slot < 0 || slot > 3)
            return errorResponse("INVALID_ARGS", "slot must be 0-3");

        const auto selected = resolveProjectSelector(
            pm.currentProject(), srcBusName, ProjectItem::Type::Bus);
        if (!selected.ok)
            return errorResponse(selected.errorCode == QLatin1String("PATH_NOT_FOUND")
                ? QStringLiteral("BUS_NOT_FOUND") : selected.errorCode,
                selected.errorMessage);
        const auto* item = selected.item;
        return errorResponse("PROPERTY_NOT_SUPPORTED",
            QString("Send Filter on Bus '%1' (%2), slot %3 is not supported")
                .arg(item->name, item->guid.toString(QUuid::WithoutBraces))
                .arg(slot));
    };


    commandHandlers_["get_output_bus_routing"] = [this](const QJsonObject& args) {
        if (!projectManager_ || !projectManager_->isProjectOpen())
            return errorResponse("NO_PROJECT_OPEN", "No project is currently open");

        const QString path = args.value("path").toString().trimmed();
        if (path.isEmpty())
            return errorResponse("INVALID_ARGS", "path is required (sound object path)");

        ProjectData* project = projectManager_->currentProject();
        ProjectItem* item = findItemByPathInProject(project, path);
        if (!item)
            return errorResponse("ITEM_NOT_FOUND", QString("Item '%1' not found").arg(path));

        auto* mcomp = item->comp<VMixNode>();
        if (!mcomp)
            return errorResponse("NO_AUDIO_COMPONENT", "Item has no VMixNode");

        const uint32_t outputBusObjectId = mcomp->outputBusId;
        const auto* outputBus = projectManager_->findBusItem(outputBusObjectId);
        const harkenstudio::ObjectId outputBusRef = outputBus
            ? outputBus->runtimeId() : harkenstudio::ObjectId{};
        QJsonObject result;
        result["path"] = path;
        result["output_bus_id"] = static_cast<qint64>(outputBusObjectId);
        result["output_bus_ref"] =
            objectIdString(outputBusRef);
        result["volume_db"] = mcomp->outputBusVolume;
        return successResponse(result);
    };

    commandHandlers_["get_bus_properties"] = [](const QJsonObject& args) {
        QJsonObject result;
        auto& pm = ProjectManager::instance();
        if (!pm.isProjectOpen()) {
            result["status"] = "error";
            result["result"] = QJsonObject{{"code", "NO_PROJECT"}, {"message", "No project open"}};
            return result;
        }

        const QJsonValue busValue = args.value(QStringLiteral("bus_id"));
        harkenstudio::ObjectId busId{};
        ProjectItem* item = nullptr;
        if (parseObjectIdJson(busValue, &busId) && !harkenstudio::IsNull(busId)) {
            item = pm.findItemByRuntimeStableId(busId);
        } else if (busValue.isDouble()) {
            const qint64 objectId = busValue.toInteger(-1);
            if (objectId > 0 && objectId <= UINT32_MAX)
                item = pm.findBusItem(static_cast<uint32_t>(objectId));
        }
        if (!item) {
            result["status"] = "error";
            result["result"] = QJsonObject{
                {"code", "INVALID_ARGS"},
                {"message", "bus_id must identify an existing Bus"}};
            return result;
        }
        const auto* bc   = item ? item->comp<VBus>() : nullptr;
        const auto* mix  = item ? item->comp<VMixNode>() : nullptr;
        if (bc && mix) {
            QJsonObject props;
            props["id"] = static_cast<qint64>(item->objectID);
            props["bus_id"] =
                objectIdString(item->runtimeId());
            props["name"] = item->name;
            const ProjectItem* parentBus = item->parent
                    && item->parent->type == ProjectItem::Type::Bus
                ? item->parent : item;
            props["parent_bus_id"] = static_cast<qint64>(parentBus->objectID);
            props["parent_bus_ref"] = objectIdString(
                parentBus == item ? harkenstudio::ObjectId{} : parentBus->runtimeId());
            props["has_receives"] = pm.busHasIncomingSends(item->objectID);
            props["volume"] = mix->volume;
            props["pan"] = bc->pan;
            props["mute"] = pm.auditionOverlay().isMuted(item->runtimeId());
            props["solo"] = pm.auditionOverlay().isSoloed(item->runtimeId());

            result["status"] = "ok";
            result["result"] = props;
            return result;
        }

        result["status"] = "error";
        result["result"] = QJsonObject{{"code", "BUS_NOT_FOUND"}, {"message", QString("Bus ID %1 not found").arg(objectIdString(busId))}};
        return result;
    };

    commandHandlers_["list_buses"] = [](const QJsonObject& args) {
        Q_UNUSED(args);
        QJsonObject result;
        auto& pm = ProjectManager::instance();
        if (!pm.isProjectOpen()) {
            result["status"] = "error";
            result["result"] = QJsonObject{{"code", "NO_PROJECT"}, {"message", "No project open"}};
            return result;
        }

        QJsonArray busArray;
        const auto items = pm.collectBusItems();
        for (auto* item : items) {
            if (!item) continue;
            const auto* mix = item->comp<VMixNode>();
            if (!item->comp<VBus>() || !mix) continue;
            QJsonObject busObj;
            busObj["id"] = static_cast<qint64>(item->objectID);
            busObj["bus_id"] =
                objectIdString(item->runtimeId());
            busObj["name"] = item->name;
            const ProjectItem* parentBus = item->parent
                    && item->parent->type == ProjectItem::Type::Bus
                ? item->parent : item;
            busObj["parent_bus_id"] = static_cast<qint64>(parentBus->objectID);
            busObj["parent_bus_ref"] = objectIdString(
                parentBus == item ? harkenstudio::ObjectId{} : parentBus->runtimeId());
            busObj["has_receives"] = pm.busHasIncomingSends(item->objectID);
            busObj["volume"] = mix->volume;
            busObj["mute"] = pm.auditionOverlay().isMuted(item->runtimeId());
            busObj["solo"] = pm.auditionOverlay().isSoloed(item->runtimeId());
            busArray.append(busObj);
        }

        result["status"] = "ok";
        result["result"] = QJsonObject{{"buses", busArray}, {"count", busArray.size()}};
        return result;
    };


    commandHandlers_["get_bus_hierarchy"] = [](const QJsonObject& args) {
        Q_UNUSED(args);
        QJsonObject result;
        auto* project = ProjectManager::instance().currentProject();
        if (!project) {
            result["status"] = "error";
            result["result"] = QJsonObject{{"code", "NO_PROJECT"}, {"message", "No project open"}};
            return result;
        }

        std::function<QJsonObject(ProjectItem*)> buildTree = [&](ProjectItem* item) -> QJsonObject {
            QJsonObject busObj;
            if (!item) return busObj;
            const auto* bc = item->comp<VBus>();
            const auto* mix = item->comp<VMixNode>();
            if (!bc || !mix) return busObj;
            busObj["id"] = static_cast<qint64>(item->objectID);
            busObj["bus_id"] =
                objectIdString(item->runtimeId());
            busObj["name"] = item->name;
            const ProjectItem* parentBus = item->parent
                    && item->parent->type == ProjectItem::Type::Bus
                ? item->parent : item;
            busObj["parent_bus_id"] = static_cast<qint64>(parentBus->objectID);
            busObj["parent_bus_ref"] = objectIdString(
                parentBus == item ? harkenstudio::ObjectId{} : parentBus->runtimeId());
            busObj["has_receives"] = ProjectManager::instance().busHasIncomingSends(item->objectID);
            busObj["volume"] = mix->volume;
            busObj["pan"] = bc->pan;
            busObj["mute"] = ProjectManager::instance().auditionOverlay().isMuted(item->runtimeId());
            busObj["solo"] = ProjectManager::instance().auditionOverlay().isSoloed(item->runtimeId());
            busObj["color"] = static_cast<qint64>(item->color);

            QJsonArray children;
            for (const auto& child : item->children) {
                if (!child) continue;
                if (child->type != ProjectItem::Type::Bus) continue;
                children.append(buildTree(child.get()));
            }
            busObj["children"] = children;
            return busObj;
        };

        QJsonObject tree = buildTree(ProjectManager::instance().getMasterBusItem());

        result["status"] = "ok";
        result["result"] = QJsonObject{{"hierarchy", tree}};
        return result;
    };

    commandHandlers_["get_selected_bus"] = [](const QJsonObject& args) {
        Q_UNUSED(args);
        QJsonObject result;
        auto* project = ProjectManager::instance().currentProject();
        if (!project) {
            result["status"] = "error";
            result["result"] = QJsonObject{{"code", "NO_PROJECT"}, {"message", "No project open"}};
            return result;
        }

        result["status"] = "ok";
        result["result"] = QJsonObject{{"message", "Use get_bus_hierarchy for full tree, or list_buses for flat list"}};
        return result;
    };

}


QJsonObject IPCServer::handleGetMixerLevels(const QJsonObject& args)
{
    Q_UNUSED(args);

    if (!engine_ || !engine_->isInitialized() || engine_->isSimulationMode())
        return errorResponse("NO_ENGINE", "Engine not initialized");

    auto linearToDb = [](float linear) -> double {
        if (linear <= 0.0f) return -96.0;
        double db = 20.0 * std::log10(static_cast<double>(linear));
        return (std::max)(db, -96.0);
    };

    QJsonArray buses;
    const auto busItems = projectManager_ ? projectManager_->collectBusItems()
                                          : std::vector<ProjectItem*>{};
    if (busItems.empty() || !busItems.front())
        return errorResponse("UNAVAILABLE", "Mixer levels unavailable");
    const auto* master = busItems.front();

    float peakL = 0.0f, peakR = 0.0f;
    const auto masterStatus = engine_->getBusMeterPeakById(
        master->runtimeId(), peakL, peakR);
    if (masterStatus != Bridge::IpcQueryStatus::Ok)
        return errorResponse("UNAVAILABLE", "Mixer levels unavailable");
    const double masterDb = linearToDb((std::max)(peakL, peakR));
    buses.append(QJsonObject{{"name", master->name}, {"level_db", masterDb},
                             {"peak_l", linearToDb(peakL)}, {"peak_r", linearToDb(peakR)}});

    for (size_t i = 1; i < busItems.size(); ++i) {
        auto* item = busItems[i];
        if (!item) continue;
        float bPeakL = 0.0f, bPeakR = 0.0f;
        if (engine_->getBusMeterPeakById(item->runtimeId(),
                                         bPeakL, bPeakR)
            != Bridge::IpcQueryStatus::Ok)
            continue;
        const double bDb = linearToDb((std::max)(bPeakL, bPeakR));
        buses.append(QJsonObject{{"name", item->name}, {"level_db", bDb},
                                 {"peak_l", linearToDb(bPeakL)}, {"peak_r", linearToDb(bPeakR)}});
    }

    QJsonObject result;
    result["buses"] = buses;
    return successResponse(result);
}

}
