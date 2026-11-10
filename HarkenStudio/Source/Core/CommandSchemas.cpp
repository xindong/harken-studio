#include "CommandSchemas.h"
#include "ItemPropertySchema.h"
#include "ProjectPlatformCommandSchema.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>

namespace VNX {
namespace {

QJsonObject object(const QJsonObject& properties = {}, const QJsonArray& required = {})
{
    return {{"type", "object"}, {"properties", properties}, {"required", required},
            {"additionalProperties", false}};
}

QJsonObject text(bool nonempty = true)
{
    return {{"type", "string"}, {"minLength", nonempty ? 1 : 0}};
}

QJsonObject integer(double minimum = 0, double maximum = 2147483647.0)
{
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject number()
{
    return {{"type", "number"}, {"minimum", -3.4028234663852886e38}, {"maximum", 3.4028234663852886e38}};
}

QJsonObject nonnegative()
{
    auto schema = number();
    schema.insert("minimum", 0);
    return schema;
}

QJsonObject boolean()
{
    return {{"type", "boolean"}};
}

QJsonObject choice(const QJsonArray& values)
{
    return {{"type", "string"}, {"enum", values}};
}

QJsonObject array(const QJsonObject& item, int minimum = 0, bool unique = false)
{
    return {{"type", "array"}, {"items", item}, {"minItems", minimum}, {"uniqueItems", unique}};
}

QJsonObject tuple(int size)
{
    auto schema = array(number(), size);
    schema.insert("maxItems", size);
    return schema;
}

QJsonObject withDefault(QJsonObject schema, const QJsonValue& value)
{
    schema.insert("default", value);
    return schema;
}

QJsonObject described(QJsonObject schema, const char* description)
{
    schema.insert("description", description);
    return schema;
}

QJsonObject alternatives(const QJsonArray& names)
{
    QJsonArray options;
    for (const auto& name : names) options.append(QJsonObject{{"required", QJsonArray{name}}});
    return {{"anyOf", options}};
}

QJsonObject requireAny(QJsonObject schema, const QJsonArray& names)
{
    auto all = schema.value("allOf").toArray();
    all.append(alternatives(names));
    schema.insert("allOf", all);
    return schema;
}

QJsonObject uuid()
{
    return {{"type", "string"}, {"format", "uuid"}};
}

QJsonObject uint32(bool positive = true)
{
    return integer(positive ? 1 : 0, 4294967295.0);
}

QJsonObject uint64()
{
    return {{"anyOf", QJsonArray{
        integer(1, 9007199254740991.0),
        QJsonObject{{"type", "string"}, {"format", "uint64"}, {"pattern", "^[1-9][0-9]*$"}}}},
        {"description", "Positive uint64. Use a decimal string above 9007199254740991 to preserve every digit."}};
}

QJsonObject itemSelector(const QJsonObject& extra = {}, const QJsonArray& required = {}, bool allowGuid = false)
{
    QJsonObject properties = extra;
    properties.insert("path", text());
    properties.insert("object_id", uint32());
    QJsonArray selectors{"path", "object_id"};
    if (allowGuid) {
        properties.insert("guid", uuid());
        selectors.append("guid");
    }
    return requireAny(object(properties, required), selectors);
}

QJsonObject bindingAddress()
{
    return object({{"hostClass", choice({"voice", "bus", "send", "effect"})}, {"slotId", integer(1, 16)},
                   {"path", uint32(false)}, {"targetFamily", choice({"playable", "filter"})}},
                  {"hostClass", "slotId"});
}

QJsonObject binding()
{
    auto points = array(tuple(2), 2);
    points.insert("maxItems", 8);
    auto handles = array(tuple(4));
    handles.insert("maxItems", 7);
    auto curve = object({{"points", points}, {"handles", handles}}, {"points", "handles"});
    curve.insert("additionalProperties", true);
    auto schema = object({{"guid", uuid()}, {"address", bindingAddress()}, {"variable", uuid()},
                   {"range", tuple(2)}, {"transitionMs", integer(0, 60000)},
                   {"curve", curve}},
                  {"address", "variable", "range", "curve"});
    schema.insert("additionalProperties", true);
    return schema;
}

QJsonObject effect()
{
    auto schema = object({{"guid", uuid()}, {"mode", choice({"custom", "preset"})},
        {"effectGuid", uuid()}, {"bypass", boolean()},
        {"custom", object({{"type", choice({"feedbackDelay", "threeBandTone", "gain", "limiter", "compressor"})},
                            {"parameters", array(number(), 1)}}, {"type", "parameters"})}}, {"guid"});
    schema.insert("oneOf", QJsonArray{
        QJsonObject{{"required", QJsonArray{"mode", "custom"}}, {"properties", QJsonObject{{"mode", QJsonObject{{"const", "custom"}}}}},
                    {"not", QJsonObject{{"required", QJsonArray{"effectGuid"}}}}},
        QJsonObject{{"required", QJsonArray{"effectGuid"}}, {"properties", QJsonObject{{"mode", QJsonObject{{"const", "preset"}}}}},
                    {"not", QJsonObject{{"required", QJsonArray{"custom"}}}}}});
    return schema;
}

struct Contract {
    QJsonObject schema;
    bool readOnly = false;
};

const QMap<QString, Contract>& contracts()
{
    static const QMap<QString, Contract> value = [] {
        QMap<QString, Contract> result;
        const auto put = [&](std::initializer_list<const char*> names, const QJsonObject& schema, bool readOnly = false) {
            for (const auto* name : names) result.insert(QLatin1String(name), {schema, readOnly});
        };

        put({"ping", "get_version", "get_status", "get_app_state", "get_project_info", "get_project_state",
             "get_3d_scene", "get_buffer_config", "get_loaded_packs", "get_playing_actions", "get_active_voices",
             "get_engine_snapshot", "get_bus_topology_plan_count", "get_bus_info", "get_performance",
             "get_session_recap", "get_job_stats", "get_metering_snapshot", "get_mixer_levels", "get_bus_hierarchy",
             "get_pack_dependencies", "get_unused_assets", "get_available_languages", "get_pack_export_settings",
             "get_format_version", "get_file_watch_status", "can_undo", "can_redo", "audition_get_state",
             "list_commands", "list_buses", "list_assets", "list_attenuations", "list_actions",
             "list_packs", "list_project_languages", "search_status", "validate_pack", "validate_pack_build_readiness"},
            object(), true);
        put({"save_project", "close_project", "shutdown_engine", "start_output", "stop_output", "undo", "redo"}, object());
        put({"get_command_schema"}, object({{"command", text()}}, {"command"}), true);
        put({"open_project", "load_pack", "unload_pack"}, object({{"path", text()}}, {"path"}));
        put({"get_pack_info", "get_folder_info", "list_folders", "get_modulator_binding"}, object({{"path", text()}}, {"path"}), true);
        put({"list_items"}, object({{"parent", text(false)}, {"parent_id", uint32(false)}}), true);
        put({"get_item_info", "get_container_info", "get_signal_chain", "get_referencing_actions"}, itemSelector(), true);
        put({"get_inspector_schema"}, object({{"schema_id", choice({"Sound", "Container", "Track", "Segment", "Layer",
            "Bus", "Action", "Variable", "Attenuation", "Effect"})}}, {"schema_id"}), true);
        put({"get_asset_info", "get_conversion_settings"}, object({{"source_id", uint32()}}, {"source_id"}), true);
        put({"get_bus_properties"}, object({{"bus_id", QJsonObject{{"anyOf", QJsonArray{uuid(), uint32()}}}}}, {"bus_id"}), true);
        put({"get_bus_pitch", "get_bus_volume_db", "get_output_bus_gain"}, object({{"bus_name", text()}}, {"bus_name"}), true);
        put({"get_action_definition"}, object({{"name", text()}}, {"name"}), true);
        put({"get_listener_info"}, object({{"listener_id", withDefault(uint32(false), 1)}}), true);
        put({"compare_files"}, object({{"reference_path", text()}, {"candidate_path", text()}}, {"reference_path", "candidate_path"}), true);
        const auto domain = choice({"Music", "Stop", "Lifecycle", "Dispatch", "Gate"});
        put({"get_exec_errors"}, object({{"domain", domain}, {"max_count", withDefault(integer(), 50)}}), true);
        put({"clear_exec_errors"}, object({{"domain", domain}}));
        put({"get_attenuation_distance_scaling"}, object({{"path", text()}, {"platform_id", projectPlatformIdSchema()}}, {"path"}), true);
        put({"search_fulltext"}, object({{"query", text()}, {"max_results", withDefault(integer(1), 100)}}, {"query"}), true);
        put({"run_search_benchmark"}, object({{"object_count", withDefault(integer(1, 500000), 1000)}}));
        put({"query"}, object({{"vql", text()}, {"return", array(text(), 0, true)}}, {"vql"}), true);
        const auto operation = choice({"packBuild", "liveSync", "actionRegistration", "projectInputCompile", "compiledPreview", "auditionPlay", "delete"});
        auto capability = object({{"rootStableId", uuid()}, {"actionStableId", uuid()}, {"operation", operation},
            {"variant", object({{"platform", projectPlatformIdSchema()}, {"language", text(false)}})}});
        capability.insert("not", QJsonObject{{"required", QJsonArray{"rootStableId", "actionStableId"}}});
        put({"get_runtime_capability_matrix"}, capability, true);

        for (const auto* name : {"list_project_platforms", "get_platform_profile", "get_action_pack_plan",
                                "get_pack_build_status", "get_last_pack_build_records", "get_transcode_status", "get_media_status"})
            put({name}, projectPlatformCommandSchema(QLatin1String(name)), true);
        for (const auto* name : {"create_project_platform", "rename_project_platform", "delete_project_platform",
                                "set_project_platform_base", "set_project_platform_profile", "apply_project_platforms",
                                "export_pack", "cancel_pack_build", "transcode_asset", "batch_convert_media",
                                "cancel_transcode", "rescan_media_sources"})
            put({name}, projectPlatformCommandSchema(QLatin1String(name)));

        auto platformRows = projectPlatformCommandSchema("apply_project_platforms").value("properties").toObject().value("platforms").toObject();
        auto platformRow = platformRows.value("items").toObject();
        platformRow.insert("required", QJsonArray{"id", "name", "basePlatform", "profile"});
        platformRows.insert("items", platformRow);
        auto createProject = object({{"name", text()}, {"location", text()},
            {"sample_rate", withDefault(integer(1), 48000)},
            {"channel_config", withDefault(choice({"mono", "stereo", "5_1", "7_1", "atmos_7_1_4"}), "stereo")},
            {"platforms", platformRows}, {"template_id", text()},
            {"template_data", described({{"type", "object"}, {"minProperties", 1}}, "Full project template data. Supply template_id together with template_data.")}}, {"name", "location"});
        createProject.insert("oneOf", QJsonArray{
            QJsonObject{{"required", QJsonArray{"template_id", "template_data"}}},
            QJsonObject{{"not", alternatives({"template_id", "template_data"})}}});
        put({"create_project"}, createProject);
        put({"create_item"}, object({{"parent", text(false)}, {"name", text()}, {"type", text()},
            {"kind", choice({"Step", "Smooth"})}}, {"name", "type"}));
        put({"create_action"}, object({{"name", text()}, {"parent", text(false)}}, {"name"}));
        put({"rename_item"}, itemSelector({{"new_name", text()}}, {"new_name"}, true));
        put({"delete_item", "copy_item", "cut_item"}, itemSelector());
        put({"move_item"}, requireAny(itemSelector({{"new_parent", text()}, {"new_parent_id", uint32()}}), {"new_parent", "new_parent_id"}));
        auto paste = requireAny(object({{"target_path", text()}, {"target_object_id", uint32()},
            {"source_path", text()}, {"source_object_id", uint32()}, {"op", choice({"copy", "cut"})}}), {"target_path", "target_object_id"});
        put({"paste_item"}, paste);
        auto propertiesPaste = paste.value("properties").toObject();
        propertiesPaste.remove("op");
        paste.insert("properties", propertiesPaste);
        put({"paste_properties"}, requireAny(paste, {"source_path", "source_object_id"}));
        put({"create_folder", "delete_folder"}, object({{"path", text()}}, {"path"}));
        put({"rename_folder"}, object({{"path", text()}, {"new_name", text()}}, {"path", "new_name"}));
        put({"move_folder"}, object({{"path", text()}, {"new_parent", text()}}, {"path", "new_parent"}));
        auto itemProperty = setItemPropertySchema();
        itemProperty.insert("allOf", QJsonArray{QJsonObject{{"anyOf", QJsonArray{
            QJsonObject{{"required", QJsonArray{"value"}}},
            QJsonObject{{"required", QJsonArray{"override_operation"}}, {"properties", QJsonObject{
                {"override_operation", choice({"clear_override", "clear_platform_override"})}}}}}}}});
        put({"set_item_property"}, itemProperty);
        put({"set_pack_export_current_platform"}, object({{"platform_id", projectPlatformIdSchema()}}, {"platform_id"}));
        put({"set_pack_export_selected_platforms"}, object({{"platform_ids", array(projectPlatformIdSchema(), 0, true)}}, {"platform_ids"}));
        put({"set_project_languages"}, object({{"languages", array(text(), 0, true)}}, {"languages"}));
        put({"set_pack_build_output_dir"}, object({{"path", text(false)}}, {"path"}));
        put({"set_build_project_data_export", "set_build_pack_metadata_export"}, object({{"enabled", boolean()}}, {"enabled"}));
        put({"set_build_waveform_settings"}, object({{"enabled", boolean()}, {"path", text(false)}}, {"enabled", "path"}));
        put({"set_action_pack_targeting"}, object({{"stableId", uuid()}, {"allPlatforms", boolean()},
            {"platform_ids", array(projectPlatformIdSchema(), 0, true)}, {"language", text(false)}}, {"stableId"}));
        put({"add_project_language", "remove_project_language", "set_default_language"}, object({{"name", text()}}, {"name"}));

        put({"create_bus"}, object({{"name", withDefault(text(), "New Bus")}, {"parent_bus_id", uint32()}}));
        put({"delete_bus"}, object({{"bus_id", uint32()}}, {"bus_id"}));
        put({"move_bus"}, object({{"bus_id", uint32()}, {"new_parent_bus_id", uint32()}}, {"bus_id", "new_parent_bus_id"}));
        put({"rename_bus"}, object({{"bus_id", uint32()}, {"new_name", text()}}, {"bus_id", "new_name"}));
        for (const auto* flag : {"mute", "solo"})
            result.insert(QStringLiteral("set_bus_") + QLatin1String(flag), {object({{"bus_name", text()}, {QLatin1String(flag), boolean()}}, {"bus_name", QLatin1String(flag)}), false});
        put({"set_bus_pitch"}, object({{"bus_name", text()}, {"cents", number()}}, {"bus_name", "cents"}));
        put({"set_bus_volume_db", "set_output_bus_gain"}, object({{"bus_name", text()}, {"db", number()}}, {"bus_name", "db"}));
        put({"set_bus_properties"}, requireAny(object({{"bus_id", uint32()}, {"volume", number()}, {"pitch", number()},
            {"pan", number()}, {"mute", boolean()}, {"solo", boolean()}, {"outputBusVolume", number()}}, {"bus_id"}),
            {"volume", "pitch", "pan", "mute", "solo", "outputBusVolume"}));
        const auto send = object({{"src_bus", text()}, {"target_bus_id", uint32()},
            {"level", described(withDefault(nonnegative(), 1), "Linear gain: 0 is silence (-96 dB), 1 is unity, 0.5 is approximately -6.02 dB.")}}, {"src_bus", "target_bus_id"});
        put({"set_send", "set_send_level"}, send);
        put({"remove_send"}, object({{"src_bus", text()}, {"target_bus_id", uint32()}}, {"src_bus", "target_bus_id"}));

        const auto container = [&](QJsonObject properties, const QJsonArray& required = {}) {
            properties.insert("container_path", text());
            properties.insert("container_id", uint32());
            return requireAny(object(properties, required), {"container_path", "container_id"});
        };
        put({"add_child_to_container", "remove_child_from_container"}, requireAny(container({{"child_path", text()}, {"child_id", uint32()}}), {"child_path", "child_id"}));
        auto reorder = requireAny(container({{"child_order", array(text(), 1, true)}, {"child_order_ids", array(uint32(), 1, true)}}), {"child_order", "child_order_ids"});
        reorder.insert("not", QJsonObject{{"required", QJsonArray{"child_order", "child_order_ids"}}});
        put({"reorder_container_children"}, reorder);
        auto containerProperty = container({{"property", choice({"volume", "pitch"})},
            {"value", number()}}, {"property", "value"});
        put({"set_container_property"}, containerProperty);

        const auto scopeProperties = QJsonObject{{"definition_guid", uuid()}, {"group", choice({"output", "send", "spatialRating", "effect"})},
            {"scope", choice({"Shared"})}, {"platform_ids", array(projectPlatformIdSchema(), 1, true)},
            {"expected_before", described({{"type", "object"}}, "Complete previous fieldGroups state for optimistic concurrency; validated by the field-group domain parser.")}};
        for (const auto* name : {"set_field_group_mode", "set_field_group_field", "clear_field_group_field_override",
                                "replace_field_group_sends", "replace_field_group_effects", "rebuild_field_group_local"}) {
            auto fields = scopeProperties;
            QJsonArray required{"definition_guid", "group"};
            const QString command = QLatin1String(name);
            if (command == "set_field_group_mode") { fields.insert("mode", choice({"Local", "Inherit", "UseShared"})); required.append("mode"); }
            if (command == "set_field_group_field" || command == "clear_field_group_field_override") { fields.insert("field", text()); required.append("field"); }
            if (command == "set_field_group_field") { fields.insert("value", described({}, "Typed value: GUID for outputBusRef/spatial.attenuationRef, boolean for spatial.is3DEnabled, number for scalar fields; null resets outputBusRef to Master.")); required.append("value"); }
            if (command == "replace_field_group_sends") {
                fields.insert("sends", array(object({{"targetBusID", uuid()}, {"sendLevel", number()},
                    {"mode", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{0, 1, 2}}}},
                    {"modeVolumes", object({{"0", number()}, {"1", number()}, {"2", number()}})}}, {"targetBusID", "sendLevel"})));
                required.append("sends");
            }
            if (command == "replace_field_group_effects") {
                fields.insert("effects", array(effect()));
                required.append("effects");
            }
            auto schema = object(fields, required);
            if (command == "set_field_group_field") {
                schema.insert("allOf", QJsonArray{QJsonObject{{"oneOf", QJsonArray{
                    QJsonObject{{"properties", QJsonObject{{"field", choice({"outputBusRef"})}, {"value", QJsonObject{{"anyOf", QJsonArray{uuid(), QJsonObject{{"type", "null"}}}}}}}}},
                    QJsonObject{{"properties", QJsonObject{{"field", choice({"spatial.attenuationRef"})}, {"value", uuid()}}}},
                    QJsonObject{{"properties", QJsonObject{{"field", choice({"spatial.is3DEnabled"})}, {"value", boolean()}}}},
                    QJsonObject{{"properties", QJsonObject{{"field", text()}, {"value", number()}}},
                        {"not", QJsonObject{{"properties", QJsonObject{{"field", choice({"outputBusRef", "spatial.attenuationRef", "spatial.is3DEnabled"})}}}}}}
                }}}});
            }
            schema.insert("oneOf", QJsonArray{QJsonObject{{"required", QJsonArray{"scope"}}}, QJsonObject{{"required", QJsonArray{"platform_ids"}}}});
            put({name}, schema);
        }

        for (const auto* name : {"set_binding", "remove_binding", "clear_bindings", "reorder_bindings", "set_modulator_binding"}) {
            QJsonObject fields{{"undo_text", text()}};
            QJsonArray required;
            const QString command = QLatin1String(name);
            if (command == "set_binding") { fields.insert("binding", binding()); required.append("binding"); }
            if (command == "remove_binding") { fields.insert("runtime_stable_id", uuid()); required.append("runtime_stable_id"); }
            if (command == "clear_bindings" || command == "reorder_bindings") { fields.insert("address", bindingAddress()); required.append("address"); }
            if (command == "reorder_bindings") { fields.insert("from", integer()); fields.insert("to", integer()); required.append("from"); required.append("to"); }
            if (command == "set_modulator_binding") { fields.insert("bindings", array(binding())); required.append("bindings"); }
            put({name}, itemSelector(fields, required, true));
        }

        put({"init_engine"}, object({{"sample_rate", described(withDefault(integer(), 0), "Sample rate in Hz. Zero selects the engine/device default.")},
            {"buffer_size", described(withDefault(integer(), 0), "Buffer size in frames. Zero selects the engine/device default.")}, {"max_voices", withDefault(integer(1), 64)},
            {"device_mode", described(withDefault(choice({"real", "null"}), "real"), "real uses an audio device; null runs the actual runtime silently without audio hardware.")}}));
        put({"set_buffer_size"}, object({{"block_size", integer(1)}}, {"block_size"}));
        put({"post_action"}, object({{"name", text()}}, {"name"}));
        put({"post_action_3d"}, object({{"name", text()}, {"x", withDefault(number(), 0)}, {"y", withDefault(number(), 0)}, {"z", withDefault(number(), 0)}}, {"name"}));
        put({"stop_action"}, object({{"handle", integer(1, 9007199254740991.0)}, {"fade_ms", withDefault(nonnegative(), 0)}}, {"handle"}));
        put({"stop_all"}, object({{"fade_ms", withDefault(nonnegative(), 0)}}));
        put({"set_action_position"}, object({{"handle", integer(1, 9007199254740991.0)}, {"x", number()}, {"y", number()}, {"z", number()}}, {"handle", "x", "y", "z"}));
        put({"set_master_volume"}, object({{"volume", nonnegative()}, {"fade_ms", withDefault(integer(), 0)}}, {"volume"}));
        put({"set_listener"}, object({{"pos", tuple(3)}, {"forward", tuple(3)}, {"up", tuple(3)}}, {"pos"}));
        put({"set_listener_position"}, object({{"listener_id", uint32(false)}, {"pos", tuple(3)}, {"forward", tuple(3)}, {"up", tuple(3)}}, {"listener_id", "pos"}));
        put({"seek_action"}, object({{"action_handle", uint64()}, {"name", text()},
            {"position", described(number(), "Milliseconds. Absolute position when is_relative is false; a signed offset from the current position when true.")},
            {"is_relative", withDefault(boolean(), false)}, {"snap_to_marker", QJsonObject{{"type", "boolean"}, {"const", false}}}}, {"action_handle", "position"}));
        put({"query_seek_action"}, object({{"seek_operation_id", uint64()}}, {"seek_operation_id"}), true);
        put({"release_seek_action"}, object({{"seek_operation_id", uint64()}}, {"seek_operation_id"}));
        put({"audition_play"}, object({{"object_path", text()}, {"game_object_id", withDefault(integer(0, 9007199254740991.0), 0)}}, {"object_path"}));
        put({"audition_stop"}, object({{"auditionID", described(withDefault(uint32(false), 0), "Audition ID. Zero or omission stops all auditions.")}, {"fade_ms", withDefault(nonnegative(), 0)}}));
        put({"audition_close"}, object({{"auditionID", uint32()}}, {"auditionID"}));
        put({"audition_reset"}, object({{"authoring_handle", uint32()}}, {"authoring_handle"}));
        put({"audition_set_position"}, object({{"auditionID", uint32()}, {"samplePosition", integer(0, 9007199254740991.0)}}, {"auditionID", "samplePosition"}));
        put({"audition_get_seek_status"}, described(object({{"audition_seek_request_id", text()}}, {"audition_seek_request_id"}),
            "Query a pending audition seek or one of the latest 128 completed requests."), true);

        auto importAudio = object({{"path", text()}, {"pack", text()}, {"target_parent_path", text()},
            {"target_parent_id", uint32()}, {"as_music_source", withDefault(boolean(), false)}}, {"path"});
        importAudio.insert("anyOf", QJsonArray{QJsonObject{{"required", QJsonArray{"pack"}}},
            QJsonObject{{"not", alternatives({"target_parent_path", "target_parent_id"})}}});
        put({"import_audio"}, importAudio);
        put({"import_audio_files"}, object({{"files", array(text(), 1)}, {"target_container_id", described(uint32(),
            "Target authoring handle: container, sound or output bus. Omit to import at the project root.")}}, {"files"}));
        put({"set_conversion_settings"}, requireAny(object({{"source_id", uint32()},
            {"codec", choice({"PCM_Float", "Vorbis", "ADPCM"})},
            {"quality", QJsonObject{{"type", "number"}, {"minimum", -0.1}, {"maximum", 1}}},
            {"resample", boolean()}, {"target_sample_rate", uint32()}, {"as_override", withDefault(boolean(), false)}}, {"source_id"}),
            {"codec", "quality", "resample", "target_sample_rate", "as_override"}));
        put({"cleanup_cache"}, object({{"max_age_days", withDefault(integer(), 90)}, {"max_size_bytes", withDefault(integer(0, 9007199254740991.0), 0)}}));
        put({"regenerate_waveform_cache"}, object({{"path", described(text(), "Imported audio path. Omit to rebuild every waveform in the open project.")}}));
        const auto replacements = QJsonObject{{"type", "object"}, {"minProperties", 1},
            {"propertyNames", QJsonObject{{"pattern", "^[1-9][0-9]*$"}}}, {"additionalProperties", text()}};
        auto replace = object({{"replacements", replacements}, {"items", array(object({{"source_id", uint32()}, {"path", text()}}, {"source_id", "path"}), 1)}});
        replace.insert("oneOf", QJsonArray{QJsonObject{{"required", QJsonArray{"replacements"}}}, QJsonObject{{"required", QJsonArray{"items"}}}});
        put({"batch_replace_media"}, replace);

        const auto attenuationSelector = [&](QJsonObject fields, const QJsonArray& required = {}) {
            fields.insert("name", text());
            fields.insert("guid", uuid());
            return requireAny(object(fields, required), {"name", "guid"});
        };
        const auto dimension = choice({"volume", "pitch"});
        put({"create_attenuation"}, object({{"name", text()}, {"guid", uuid()}, {"max_distance", nonnegative()},
            {"definition", described({{"type", "object"}}, "Attenuation snapshot with dimensions, validated before creating the item.")}}, {"name"}));
        put({"get_attenuation"}, attenuationSelector({}), true);
        put({"delete_attenuation"}, requireAny(object({{"guid", uuid()}, {"path", text()}, {"name", text()}}), {"guid", "path", "name"}));
        put({"rename_attenuation"}, requireAny(object({{"guid", uuid()}, {"path", text()}, {"old_name", text()}, {"new_name", text()}}, {"new_name"}), {"guid", "path", "old_name"}));
        const auto point = QJsonObject{{"anyOf", QJsonArray{tuple(2), object({{"distance", nonnegative()}, {"value", number()}}, {"distance", "value"})}}};
        auto attenuationPoints = array(point, 2);
        attenuationPoints.insert("maxItems", 8);
        auto attenuationHandles = array(tuple(4));
        attenuationHandles.insert("maxItems", 7);
        put({"set_attenuation_curve"}, attenuationSelector({{"curve", dimension}, {"points", attenuationPoints},
            {"handles", attenuationHandles}, {"max_distance", nonnegative()}, {"create_if_missing", withDefault(boolean(), false)}, {"undo_text", text()}}, {"curve", "points"}));
        put({"add_attenuation_dimension", "remove_attenuation_dimension"}, attenuationSelector({{"dimension", dimension}}, {"dimension"}));

        auto requests = array(object({{"cmd", text()}, {"args", QJsonObject{{"type", "object"}}}}, {"cmd"}), 1);
        requests.insert("maxItems", 64);
        put({"preview_actions"}, object({{"actions", requests}, {"label", text(false)}, {"source", text()}}, {"actions"}));
        put({"confirm_actions"}, object({{"tx_id", text()}, {"confirm", withDefault(boolean(), true)}}, {"tx_id"}));
        put({"apply_actions", "rollback_actions"}, object({{"tx_id", text()}}, {"tx_id"}));
        put({"get_action_tx"}, object({{"tx_id", text()}}, {"tx_id"}), true);

        const QMap<QString, QString> descriptions{
            {"create_project", "Create a project directory and its .hsp file. Existing directories are rejected. Full templates require both template_id and template_data."},
            {"open_project", "Open an existing .hsp project. The CLI waits for loading to finish before returning."},
            {"save_project", "Save all pending authoring changes in the open project."},
            {"close_project", "Close the current project. Save pending changes first; Studio may request a dialog response for unsaved edits."},
            {"init_engine", "Initialize the audio runtime with a real or null audio device. Use start_output to begin processing. Repeated calls without arguments reuse the runtime; changing settings requires shutdown_engine first."},
            {"shutdown_engine", "Stop and release the audio runtime."},
            {"start_output", "Start audio output on the initialized runtime."},
            {"stop_output", "Stop audio output while retaining the initialized runtime."},
            {"post_action", "Execute an authored Action and return its playback handle."},
            {"seek_action", "Request a seek in milliseconds. Poll query_seek_action with seek_operation_id to observe completion, then release_seek_action to release its record."},
            {"query_seek_action", "Read the state and results of a seek operation without changing it."},
            {"release_seek_action", "Release a completed seek operation record."},
            {"batch_replace_media", "Replace imported sources by source ID. All input rows must be valid; per-source I/O failures return an error with partial results."},
            {"set_conversion_settings", "Update an imported source's conversion settings. Supported codecs are PCM_Float, ADPCM and Vorbis; as_override applies a file override."},
            {"regenerate_waveform_cache", "Regenerate one source's waveform cache, or rebuild all project waveforms when path is omitted."},
            {"set_container_property", "Set a supported container scalar property using value. Output, Send and Spatial Rating definitions use field-group commands."},
            {"set_bus_volume_db", "Set a bus's authored volume in decibels."},
            {"set_bus_pitch", "Set a bus's authored pitch in cents."},
            {"set_master_volume", "Set the initialized runtime's master linear gain; 1 is unity and 0 is silence."},
            {"set_modulator_binding", "Replace the object's complete typed parameter-binding ledger using bindings."},
            {"preview_actions", "Validate supported edits and create a transaction preview. Confirm the returned tx_id before applying."},
            {"apply_actions", "Apply a previously confirmed transaction; failures include rollback results."}
        };
        for (auto it = descriptions.begin(); it != descriptions.end(); ++it)
            result[it.key()].schema.insert("description", it.value());

        for (const auto* name : {"set_bus_properties", "create_bus", "set_send", "remove_send", "set_send_level",
                                "add_child_to_container", "remove_child_from_container", "set_container_property", "reorder_container_children"}) {
            auto& schema = result[QLatin1String(name)].schema;
            auto properties = schema.value("properties").toObject();
            properties.insert("preview", withDefault(boolean(), false));
            schema.insert("properties", properties);
        }
        return result;
    }();
    return value;
}

}

QJsonObject commandInputSchema(const QString& name)
{
    return contracts().value(name).schema;
}

bool commandIsReadOnly(const QString& name)
{
    return contracts().value(name).readOnly;
}

}
