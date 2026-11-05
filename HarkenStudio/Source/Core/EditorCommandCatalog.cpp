#include "EditorCommandCatalog.h"
#include "ActionManager.h"
#include "AI/AICommandCatalog.h"
#include "IPC/IPCServer.h"

#include <QJsonArray>
#include <QSet>

namespace VNX {
namespace {

QJsonObject inputSchema(const QJsonObject& parameters)
{
    if (parameters.value(QStringLiteral("type")) == QStringLiteral("object"))
        return parameters;
    QJsonObject properties;
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        QJsonObject field;
        if (it.value().isObject()) {
            field = it.value().toObject();
        } else {
            const QString type = it.value().toString();
            if (type == QStringLiteral("string") || type == QStringLiteral("object")
                || type == QStringLiteral("array") || type == QStringLiteral("number")
                || type == QStringLiteral("boolean") || type == QStringLiteral("integer"))
                field.insert(QStringLiteral("type"), type);
            else if (type == QStringLiteral("bool"))
                field.insert(QStringLiteral("type"), QStringLiteral("boolean"));
            else if (type == QStringLiteral("float") || type == QStringLiteral("double"))
                field.insert(QStringLiteral("type"), QStringLiteral("number"));
            else if (type.startsWith(QStringLiteral("int")) || type.startsWith(QStringLiteral("uint"))) {
                field.insert(QStringLiteral("type"), QStringLiteral("integer"));
                if (type.startsWith(QStringLiteral("uint")))
                    field.insert(QStringLiteral("minimum"), 0);
            }
            field.insert(QStringLiteral("description"), type);
        }
        properties.insert(it.key(), field);
    }
    return {{QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"), properties}};
}

}

bool commandSupportedByHost(CommandHost host, const QString& name, bool requiresDesktop)
{
    if (host == CommandHost::Desktop) return true;
    if (requiresDesktop) return false;
#ifndef HARKEN_HAS_LMDB
    if (name == QStringLiteral("search_fulltext") || name == QStringLiteral("search_status")
        || name == QStringLiteral("run_search_benchmark"))
        return false;
#endif
    static const QSet<QString> unavailable{
        "batch", "ai_event", "ai_response", "observe", "unobserve",
        "create_pack", "rename_pack", "delete_pack",
        "set_platform_profile", "set_target_platforms", "set_voice_language", "set_preferred_format",
        "get_object_audio_status", "get_cpu_budget_state", "get_render_plan",
        "audition_get_position", "audition_get_speed", "audition_set_speed",
        "bind_attenuation_to_object", "unbind_attenuation_from_object", "set_attenuation_distance_scaling",
        "list_named_layouts", "get_selected_bus", "set_send_filter", "get_send_filter",
        "link_capture_start", "link_capture_stop", "link_get_status", "link_mute_bus",
        "link_pause_engine", "link_post_action", "link_resume_engine", "link_set_property",
        "link_solo_bus", "link_stop_all",
        "set_clock_pll", "get_clock_sync_status", "list_codecs", "get_codec_info",
        "get_codec_capabilities", "get_stream_status", "get_stream_position",
        "get_stream_device_info", "get_stream_device_count", "get_stream_throughput",
        "get_mmap_status", "get_device_fairness", "get_memory_budget_status",
        "get_listener_output_bus", "get_device_capabilities", "get_active_format",
        "get_hotplug_events", "get_watched_parameters", "get_live_parameters",
        "watch_parameter", "unwatch_parameter", "get_monitor_filter", "set_monitor_filter",
        "get_voice_contribution", "get_voice_path_latency", "get_pack_metrics",
        "get_voice_metadata_chain", "get_voice_node_virtual_states", "set_spatial_fallback_policy",
        "get_pack_load_history", "get_action_routing", "resolve_action_packs",
        "validate_multipack_collisions", "list_modulators", "list_modulator_chains",
        "get_modulator_presets", "save_modulator_user_preset", "delete_modulator_user_preset",
        "apply_modulator_preset", "set_game_object_volume", "set_game_object_mute",
        "set_game_object_position", "set_game_object_orientation", "set_metering_interval",
        "set_cpu_budget_config", "set_memory_budget_limit", "set_runtime_language",
        "set_pack_routing_priority",
        "add_listener", "remove_listener", "set_game_object_listeners",
        "set_listener_spatialization", "set_listener_output_bus", "set_multi_position",
        "clear_multi_position", "set_automation_path", "start_automation", "stop_automation",
        "spatial_init", "spatial_load_hrtf", "spatial_unload_hrtf", "spatial_set_hrtf_enabled",
        "spatial_set_image_source", "spatial_remove_image_source", "spatial_clear_image_sources",
        "spatial_set_diffraction_order", "spatial_set_transmission_operation",
        "spatial_set_smoothing", "spatial_set_load_balancing", "spatial_set_game_object_radius",
        "get_binaural_status"
    };
    return !unavailable.contains(name);
}

QJsonObject describeEditorCommand(const QString& name, const IPCServer* dispatcher)
{
    const auto* action = ActionManager::instance().findAction(name);
    const auto* spec = AICommandCatalog::find(name);
    const bool handler = dispatcher && dispatcher->hasCommandHandler(name);
    QJsonObject result{
        {QStringLiteral("command"), name},
        {QStringLiteral("registered"), action != nullptr || handler},
        {QStringLiteral("readOnly"), dispatcher && dispatcher->isReadOnlyCommand(name)},
        {QStringLiteral("requiresDesktop"), dispatcher && dispatcher->commandRequiresDesktop(name)}
    };
    QJsonObject parameters;
    if (spec && (!spec->argumentSchema.isEmpty() || !action || action->paramSchema.isEmpty())) {
        parameters = spec->argumentSchema;
        result.insert(QStringLiteral("source"), QStringLiteral("catalog"));
        result.insert(QStringLiteral("description"), spec->description);
        result.insert(QStringLiteral("contractVersion"), spec->contractVersion);
    } else if (action) {
        parameters = action->paramSchema;
        result.insert(QStringLiteral("source"), QStringLiteral("action"));
        result.insert(QStringLiteral("description"), action->displayName);
    } else {
        result.insert(QStringLiteral("source"), handler ? QStringLiteral("handler") : QStringLiteral("unknown"));
    }
    if (action) {
        result.insert(QStringLiteral("displayName"), action->displayName);
        result.insert(QStringLiteral("category"), action->category);
        result.insert(QStringLiteral("undoable"), action->undoable);
    }
    result.insert(QStringLiteral("parameters"), parameters);
    result.insert(QStringLiteral("inputSchema"), inputSchema(parameters));
    const bool declared = parameters.value(QStringLiteral("type")) == QStringLiteral("object");
    result.insert(QStringLiteral("schemaSource"), declared ? QStringLiteral("declared")
        : parameters.isEmpty() ? QStringLiteral("undocumented") : QStringLiteral("legacy"));
    if (!declared)
        result.insert(QStringLiteral("note"), QStringLiteral(
            "Legacy parameter descriptions may omit requirements and constraints. "
            "An undocumented schema does not mean the command takes no arguments. "
            "The editor validates execution and returns errors for invalid input."));
    return result;
}

}
