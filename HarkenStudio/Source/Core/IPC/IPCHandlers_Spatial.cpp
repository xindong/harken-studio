#include "IPCServerInternal.h"
#include "FieldGroupProjectAdapter.h"

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

        FieldGroupScope scope;
        if (args.contains("platform_id")) {
            const auto platform = ProjectPlatformSemantics::parseId(args.value("platform_id").toString());
            if (!platform || !ProjectPlatformSemantics::find(project->platforms, *platform))
                return errorResponse("INVALID_PARAM", "platform_id must identify a project platform");
            scope = toFieldGroupGuid(*platform);
        }
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*project, scope);
        if (!snapshot.ok())
            return errorResponse("INVALID_FIELD_GROUPS", snapshot.diagnostics.front().message);
        const auto definition = snapshot.definitions.find(toFieldGroupGuid(item->guid));
        if (definition == snapshot.definitions.end())
            return errorResponse("NO_COMPONENT", "Item has no spatial field group");
        QJsonObject result{{"path", path},
            {"attenuation_distance_scaling", definition->second.spatialRating.spatialAttenuationDistanceScaling}};
        if (scope)
            result.insert("platform_id", args.value("platform_id"));
        else
            result.insert("scope", "Shared");
        return successResponse(result);
    };
}

}
