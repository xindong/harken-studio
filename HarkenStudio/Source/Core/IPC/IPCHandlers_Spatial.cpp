#include "IPCServerInternal.h"
#include "AttenuationStore.h"
#include "Components/VSpatial.h"

namespace VNX {

void IPCServer::registerSpatialHandlers()
{

    commandHandlers_["get_binaural_status"] = [this](const QJsonObject&) {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        auto status = engine_->getBinauralStatus();
        return successResponse(QJsonObject{
            {"hrtf_loaded", status.hrtfLoaded},
            {"measurements", static_cast<int>(status.measurements)},
            {"ir_length", static_cast<int>(status.irLength)},
            {"sample_rate", status.sampleRate}
        });
    };

    commandHandlers_["get_attenuation_distance_scaling"] = [this](const QJsonObject& args) {
        auto* project = ProjectManager::instance().currentProject();
        if (!project) return errorResponse("NO_PROJECT", "No project open");

        QString path = args["path"].toString();
        if (path.isEmpty()) return errorResponse("INVALID_PARAM", "path required");

        auto* item = findItemByPathInProject(project, path);
        if (!item) return errorResponse("NOT_FOUND", "Item not found: " + path);

        auto* spatialComp = item->comp<VSpatial>();
        if (!spatialComp) return errorResponse("NO_COMPONENT", "Item has no VSpatial");

        QJsonObject result;
        result["path"] = path;
        result["attenuation_distance_scaling"] = spatialComp->attenuationDistanceScaling;
        return successResponse(result);
    };
}

}
