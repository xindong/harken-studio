#include "AICommandCatalog.h"
#include "../ItemPropertySchema.h"
#include "../ProjectPlatformCommandSchema.h"

#include <QJsonArray>

#include <algorithm>

namespace VNX {
namespace {

QJsonObject emptyObjectSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject listItemsSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("parent"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("parent_id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}}}
        }},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject itemInfoSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("path"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}},
            {QStringLiteral("object_id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}}}
        }},
        {QStringLiteral("anyOf"), QJsonArray{
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}},
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("object_id")}}}
        }},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject folderPathSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("path"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("minLength"), 1}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject voiceIdSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("voice_id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("voice_id")}},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject busIdSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("bus_id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("bus_id")}},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject packMetricsSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("pack_name"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}}
        }},
        {QStringLiteral("additionalProperties"), false}
    };
}

QJsonObject actionNameSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("name"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}},
        {QStringLiteral("additionalProperties"), false}
    };
}

AICommandSpec spec(const char* cmd, AICommandSpec::Availability availability,
                   AICommandSpec::Effect effect, AICommandSpec::Confirmation confirmation,
                   AICommandSpec::ChangeSupport changeSupport, AICommandSpec::Source source,
                   int toolPriority,
                   const QJsonObject& schema = QJsonObject(),
                   const QString& description = QString())
{
    return AICommandSpec{QString::fromLatin1(cmd), 1, effect, availability, confirmation,
                          changeSupport, source, toolPriority, schema, description};
}

const QList<AICommandSpec>& catalog()
{
    using Availability = AICommandSpec::Availability;
    using Confirmation = AICommandSpec::Confirmation;
    using Effect = AICommandSpec::Effect;
    using ChangeSupport = AICommandSpec::ChangeSupport;
    using Source = AICommandSpec::Source;
    static const QList<AICommandSpec> entries = {
        spec("list_project_platforms", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::ActionManager, 45, projectPlatformCommandSchema(QStringLiteral("list_project_platforms")), QStringLiteral("List project platform instances and their UUIDs, names, base types and stored conversion profiles, plus effective_profiles keyed by UUID.")),
        spec("create_project_platform", Availability::Enabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 46, projectPlatformCommandSchema(QStringLiteral("create_project_platform")), QStringLiteral("Create an independent project platform from one of the five base types. Multiple instances may share a base type.")),
        spec("rename_project_platform", Availability::Enabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 47, projectPlatformCommandSchema(QStringLiteral("rename_project_platform")), QStringLiteral("Rename a project platform by UUID, preserving its identity and authored references.")),
        spec("delete_project_platform", Availability::Enabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 48, projectPlatformCommandSchema(QStringLiteral("delete_project_platform")), QStringLiteral("Delete a project platform by UUID and remove its overrides, source selections and build filters. Undo restores them. The last platform cannot be deleted.")),
        spec("set_project_platform_profile", Availability::Enabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 49, projectPlatformCommandSchema(QStringLiteral("set_project_platform_profile")), QStringLiteral("Set an instance conversion profile or reset it with null. Use effective_profiles from list_project_platforms as the starting configuration.")),
        spec("set_project_platform_base", Availability::Enabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 50, projectPlatformCommandSchema(QStringLiteral("set_project_platform_base")), QStringLiteral("Change an instance's base type and replace its conversion profile with the new base defaults, preserving its UUID, name and authored references. Selecting its current base type has no effect.")),
        spec("get_platform_profile", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 51, projectPlatformCommandSchema(QStringLiteral("get_platform_profile")), QStringLiteral("Return the specified project platform and its complete, nonoverlapping source size ranges and sample rate settings.")),
        spec("get_action_pack_plan", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 52, projectPlatformCommandSchema(QStringLiteral("get_action_pack_plan")), QStringLiteral("Preview Action builds for explicit project platform UUIDs and languages; omitted dimensions use project targets.")),
        spec("export_pack", Availability::Enabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::CommandHandler, 53, projectPlatformCommandSchema(QStringLiteral("export_pack")), QStringLiteral("Build project packs for platform_id, or the export selection if omitted. Poll get_pack_build_status for progress and the final result.")),
        spec("get_pack_build_status", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 54, projectPlatformCommandSchema(QStringLiteral("get_pack_build_status")), QStringLiteral("Return pack build progress and the final result, including platform UUIDs, names and base types.")),
        spec("get_last_pack_build_records", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 55, projectPlatformCommandSchema(QStringLiteral("get_last_pack_build_records")), QStringLiteral("Return historical build records with captured platform identity, name and actual output paths.")),
        spec("cancel_pack_build", Availability::Enabled, Effect::EditorState, Confirmation::Always, ChangeSupport::None, Source::CommandHandler, 56, projectPlatformCommandSchema(QStringLiteral("cancel_pack_build")), QStringLiteral("Cancel the current pack build.")),
        spec("transcode_asset", Availability::Enabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::CommandHandler, 57, projectPlatformCommandSchema(QStringLiteral("transcode_asset")), QStringLiteral("Convert a source for an explicit project platform UUID. Poll get_transcode_status using the same platform_id and source_id.")),
        spec("get_transcode_status", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 58, projectPlatformCommandSchema(QStringLiteral("get_transcode_status")), QStringLiteral("Return progress and the final result for a requested platform_id and source_id. Changed projects, platforms or sources invalidate the result.")),
        spec("get_media_status", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 59, projectPlatformCommandSchema(QStringLiteral("get_media_status")), QStringLiteral("Return media status for one project platform UUID.")),
        spec("apply_project_platforms", Availability::InternalOnly, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0, projectPlatformCommandSchema(QStringLiteral("apply_project_platforms"))),
        spec("ping", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 10, emptyObjectSchema(), QStringLiteral("Check whether the editor is reachable.")),
        spec("get_version", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 20, emptyObjectSchema(), QStringLiteral("Return the editor version.")),
        spec("get_status", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 30, emptyObjectSchema(), QStringLiteral("Return the current editor status.")),
        spec("get_app_state", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 40, emptyObjectSchema(), QStringLiteral("Return the current application state.")),
        spec("get_project_info", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 50, emptyObjectSchema(), QStringLiteral("Return information about the open project.")),
        spec("list_packs", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 60, emptyObjectSchema(), QStringLiteral("List packs in the current project.")),
        spec("list_items", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 70, listItemsSchema(), QStringLiteral("List items under an optional parent.")),
        spec("get_item_info", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 80, itemInfoSchema(), QStringLiteral("Return item information; provide either path or object_id.")),
        spec("get_folder_info", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::ActionManager, 81, folderPathSchema(), QStringLiteral("Return physical directory information by bucket-relative path, for example Layers/Ambience. Directories have no GUID.")),
        spec("list_folders", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::ActionManager, 82, folderPathSchema(), QStringLiteral("List physical child directories at a bucket or directory path, for example Layers or Layers/Ambience.")),
        spec("get_project_state", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 90, emptyObjectSchema(), QStringLiteral("Return the current project state.")),
        spec("get_loaded_packs", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 100, emptyObjectSchema(), QStringLiteral("List currently loaded packs.")),
        spec("get_engine_snapshot", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 140, emptyObjectSchema(), QStringLiteral("Return a combined engine diagnostic snapshot.")),
        spec("get_active_voices", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 150, emptyObjectSchema(), QStringLiteral("List currently active voices with per-voice diagnostics.")),
        spec("get_playing_actions", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 160, emptyObjectSchema(), QStringLiteral("List actions that are currently playing.")),
        spec("get_voice_contribution", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0, voiceIdSchema(), QStringLiteral("Voice contribution breakdown is unavailable on the current runtime bridge.")),
        spec("get_voice_path_latency", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0, voiceIdSchema(), QStringLiteral("Voice path latency is unavailable on the current runtime bridge.")),
        spec("get_mixer_levels", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 190, emptyObjectSchema(), QStringLiteral("Return current output levels for all buses.")),
        spec("list_buses", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 200, emptyObjectSchema(), QStringLiteral("List buses in the current project.")),
        spec("get_bus_hierarchy", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 210, emptyObjectSchema(), QStringLiteral("Return the bus hierarchy tree.")),
        spec("get_bus_properties", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 220, busIdSchema(), QStringLiteral("Return properties of a bus by id.")),
        spec("get_performance", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 230, emptyObjectSchema(), QStringLiteral("Return engine performance counters.")),
        spec("get_pack_metrics", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0, packMetricsSchema(), QStringLiteral("Pack load metrics are unavailable on the current runtime bridge.")),
        spec("get_pack_dependencies", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 280, emptyObjectSchema(), QStringLiteral("Return the pack dependency tree.")),
        spec("validate_multipack_collisions", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 290, emptyObjectSchema(), QStringLiteral("Report action name collisions across loaded packs.")),
        spec("list_actions", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 300, emptyObjectSchema(), QStringLiteral("List actions in the current project.")),
        spec("get_action_definition", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 310, actionNameSchema(), QStringLiteral("Return the definition of an action by name.")),
        spec("get_3d_scene", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_current_layout", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 110, emptyObjectSchema(), QStringLiteral("Return the current editor layout.")),
        spec("get_current_view", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 120, emptyObjectSchema(), QStringLiteral("Return the current editor view.")),
        spec("get_inspector_context", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_ui_tree", Availability::TestOnly, Effect::EditorState, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_focused_widget", Availability::TestOnly, Effect::EditorState, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_last_interacted_widget", Availability::TestOnly, Effect::EditorState, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_action_playback_limit", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::ActionManager, 0),
        spec("get_canvas_flow", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::ActionManager, 0),
        spec("get_exec_errors", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 260, emptyObjectSchema(), QStringLiteral("List recent engine execution errors without clearing them.")),
        spec("get_job_stats", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_object_audio_status", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("get_memory_budget_status", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0, emptyObjectSchema(), QStringLiteral("Memory budget status is unavailable on the current runtime bridge.")),
        spec("get_cpu_budget_state", Availability::Enabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 240, emptyObjectSchema(), QStringLiteral("Return the CPU budget state.")),
        spec("get_render_plan", Availability::Disabled, Effect::Query, Confirmation::None, ChangeSupport::None, Source::CommandHandler, 0),
        spec("create_project", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("open_project", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::CommandHandler, 0),
        spec("save_project", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("close_project", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("create_pack", Availability::Retired, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("rename_pack", Availability::Retired, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("create_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("create_folder", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("rename_folder", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("move_folder", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("delete_folder", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("rename_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_item_property", Availability::Enabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 130, setItemPropertySchema(), QStringLiteral("Set an approved scalar or canonical authoring ledger. platform_ids are project UUIDs from list_project_platforms; Sound sources contains independent plain and localized sets, each with entries, shared active, and platforms keyed by project UUID with active overrides.")),
        spec("move_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("copy_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("cut_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("paste_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("paste_properties", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("create_action", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("add_action_step", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("update_action_step", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("reorder_action_step", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_action_properties", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_action_playback_limit", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("add_child_to_container", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("remove_child_from_container", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("reorder_container_children", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_container_property", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("add_node", Availability::Retired, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("connect_nodes", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("disconnect_nodes", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_node_position", Availability::Retired, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_node_override", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_canvas_flow", Availability::Disabled, Effect::EditorState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_listener", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_action_position", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_master_volume", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_listener_output_bus", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_spatial_fallback_policy", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_pack_routing_priority", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_cpu_budget_config", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("set_memory_budget_limit", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("switch_layout", Availability::Disabled, Effect::EditorState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("switch_view", Availability::Disabled, Effect::EditorState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("toggle_panel", Availability::Disabled, Effect::EditorState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("save_named_layout", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("load_named_layout", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("delete_pack", Availability::Retired, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("delete_item", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("remove_action_step", Availability::Disabled, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("remove_node", Availability::Retired, Effect::ProjectState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("delete_named_layout", Availability::Disabled, Effect::External, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("shutdown", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("stop_all", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0),
        spec("unload_pack", Availability::Disabled, Effect::RuntimeState, Confirmation::Always, ChangeSupport::None, Source::ActionManager, 0)
    };
    return entries;
}

}

const QList<AICommandSpec>& AICommandCatalog::all()
{
    return catalog();
}

const AICommandSpec* AICommandCatalog::find(const QString& cmd)
{
    for (const auto& entry : catalog())
    {
        if (entry.cmd == cmd)
            return &entry;
    }
    return nullptr;
}

QList<AICommandSpec> AICommandCatalog::enabled()
{
    return selectTools();
}

QList<AICommandSpec> AICommandCatalog::selectTools(const QString& permission)
{
    return selectTools(parsePermissionMode(permission));
}

QList<AICommandSpec> AICommandCatalog::selectTools(AIPermissionMode permission)
{
    QList<AICommandSpec> selected;
    for (const auto& entry : catalog())
    {
        if (entry.availability != AICommandSpec::Availability::Enabled
            || (!allowsWrites(permission) && entry.effect != AICommandSpec::Effect::Query))
            continue;
        selected.append(entry);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
        if (left.toolPriority != right.toolPriority)
            return left.toolPriority < right.toolPriority;
        return left.cmd < right.cmd;
    });
    return selected;
}

}
