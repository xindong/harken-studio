#include "CommandSchemas.h"
#include "Components/VConversion.h"
#include "EffectChainSemantics.h"
#include "ItemPropertySchema.h"
#include "ProjectPlatformCommandSchema.h"
#include "VariableSemantics.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <limits>

namespace harken::studio {
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
        {"custom", object({{"type", choice({EffectChainSemantics::typeName(EffectType::Limiter)})},
                            {"parameters", described(tuple(EffectChainSemantics::parameterCount(EffectType::Limiter)),
                                "Limiter ceiling in dB followed by release in milliseconds.")}}, {"type", "parameters"})}}, {"guid"});
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
        put({"save_project_as"}, described(object({{"path", text()}}, {"path"}),
            "Save to the current .hsp file or to a .hsp file in a directory containing no project, then continue editing there."));
        put({"get_preview_context"}, object(), true);
        put({"set_preview_context"}, requireAny(object({
            {"platform_id", projectPlatformIdSchema()},
            {"all_platforms", described(boolean(), "Select the Studio toolbar's All edit scope while keeping the primary audition platform.")},
            {"language", described(text(), "Project language returned by list_project_languages.")},
            {"media_mode", choice({"Original", "Converted"})}}), {"platform_id", "all_platforms", "language", "media_mode"}));
        put({"remote_disconnect", "stop_remote_capture"}, object());
        put({"start_remote_capture"}, object({{"output_path", text()}}));
        put({"get_remote_status", "get_remote_catalog"}, object(), true);
        put({"remote_discover"}, object({{"discovery_port", withDefault(integer(1, 65535), 37840)}}));
        put({"remote_connect"}, object({{"address", text()}, {"port", integer(1, 65535)}}, {"address", "port"}));
        put({"poll_remote_events"}, object({{"cursor", withDefault(integer(0, 9007199254740991.0), 0)},
                                            {"max", withDefault(integer(1, 1000), 100)}}), true);
        put({"get_command_schema"}, object({{"command", text()}}, {"command"}), true);
        put({"open_project", "load_pack", "unload_pack"}, object({{"path", text()}}, {"path"}));
        put({"get_pack_info", "get_folder_info", "list_folders", "get_modulator_binding"}, object({{"path", text()}}, {"path"}), true);
        put({"list_items"}, object({{"parent", text(false)}, {"parent_id", uint32(false)}}), true);
        put({"get_item_info", "get_container_info", "get_signal_chain", "get_referencing_actions"}, itemSelector(), true);
        put({"get_inspector_schema"}, object({{"schema_id", choice({"Sound", "Container", "Track", "Segment", "Layer",
            "Bus", "Action", "Variable", "Attenuation", "Effect"})}}, {"schema_id"}), true);
        put({"get_asset_info"}, object({{"source_id", uint32()}}, {"source_id"}), true);
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

        for (const auto* name : {"list_project_platforms", "get_action_pack_plan",
                                "get_pack_build_status", "get_last_pack_build_records", "get_media_status"})
            put({name}, projectPlatformCommandSchema(QLatin1String(name)), true);
        for (const auto* name : {"create_project_platform", "rename_project_platform", "delete_project_platform",
                                "set_project_platform_base", "apply_project_platforms",
                                "export_pack", "cancel_pack_build",
                                "rescan_media_sources"})
            put({name}, projectPlatformCommandSchema(QLatin1String(name)));

        const auto conversionSelector = described(text(), "Conversion preset GUID or unambiguous project path or name.");
        const auto nullableConversion = QJsonObject{{"anyOf", QJsonArray{conversionSelector, QJsonObject{{"type", "null"}}}}};
        put({"get_conversion_fields"}, object({{"conversion", conversionSelector}}, {"conversion"}), true);
        put({"create_conversion"}, object({{"name", text()}, {"folder", described(text(),
            "Existing directory in Conversions, relative to that bucket or prefixed with Conversions/.")}}, {"name"}));
        const QJsonObject nonnegativeInt64{{"type", "integer"}, {"minimum", 0}, {"maximum", std::numeric_limits<qint64>::max()}};
        const auto tier = object({{"max_bytes", QJsonObject{{"anyOf", QJsonArray{nonnegativeInt64, QJsonObject{{"type", "null"}}}}}},
            {"sample_rate", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{0, 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000}}}}},
            {"max_bytes", "sample_rate"});
        const auto quality = integer(0, kMaxVorbisQuality);
        const auto ruleSchema = [&](bool vorbis) {
            auto properties = tier.value("properties").toObject();
            properties.insert("format", choice({vorbis ? "Vorbis" : "PCM"}));
            QJsonArray required{"max_bytes", "sample_rate", "format"};
            if (vorbis) {
                properties.insert("quality", quality);
                required.append("quality");
            }
            return object(properties, required);
        };
        const QJsonObject rule{{"oneOf", QJsonArray{ruleSchema(false), ruleSchema(true)}}};
        auto conversionFields = object({{"conversion", conversionSelector},
            {"rules", described(array(rule, 1), "Complete size-ordered encoding rules. The last max_bytes is null. Vorbis requires an integer quality from 0 to 10.")},
            {"format", choice({"PCM", "Vorbis"})}, {"quality", quality}, {"sample_rate_tiers", array(tier, 1)}}, {"conversion"});
        conversionFields.insert("oneOf", QJsonArray{QJsonObject{{"required", QJsonArray{"rules"}}},
            alternatives({"format", "quality", "sample_rate_tiers"})});
        put({"set_conversion_fields"}, conversionFields);
        put({"set_conversion_rule_range"}, requireAny(object({{"conversion", conversionSelector},
            {"rule_index", described(integer(), "Zero-based index in the preset's ordered rules.")},
            {"min_bytes", described(nonnegativeInt64, "Exclusive lower boundary in original file bytes, shared with the preceding rule. Omit for the first rule.")},
            {"max_bytes", described(nonnegativeInt64, "Inclusive upper boundary in original file bytes, shared with the following rule. Omit for the final rule.")}},
            {"conversion", "rule_index"}), {"min_bytes", "max_bytes"}));
        put({"set_object_conversion"}, object({{"object", text()}, {"inherit", boolean()},
            {"settings", described(nullableConversion, "Local preset reference, or null for no conversion. Omit to retain local history; unavailable when inherit is true.")},
            {"platform_id", described(projectPlatformIdSchema(), "Platform override to edit. Omit to edit Shared.")}},
            {"object", "inherit"}));
        put({"set_default_conversion"}, object({{"platform_id", projectPlatformIdSchema()}, {"conversion", nullableConversion}},
            {"platform_id", "conversion"}));
        put({"get_effective_conversion"}, object({{"object", text()}, {"platform_id", projectPlatformIdSchema()}}, {"object"}), true);
        const auto conversionQuery = object({{"conversion", conversionSelector}, {"platform_id", projectPlatformIdSchema()}}, {"conversion"});
        put({"get_conversion_results"}, conversionQuery, true);
        put({"convert"}, conversionQuery);

        auto platformRows = projectPlatformCommandSchema("apply_project_platforms").value("properties").toObject().value("platforms").toObject();
        auto platformRow = platformRows.value("items").toObject();
        platformRow.insert("required", QJsonArray{"id", "name", "basePlatform"});
        platformRows.insert("items", platformRow);
        auto createProject = object({{"name", text()}, {"location", text()},
            {"sample_rate", withDefault(integer(1), 48000)},
            {"channel_config", withDefault(choice({"mono", "stereo", "5_1", "7_1", "atmos_7_1_4"}), "stereo")},
            {"platforms", platformRows}, {"template_id", text()},
            {"template_data", described({{"type", "object"}, {"minProperties", 1}}, "Optional inline template for template_id. With only template_id, use an installed template from list_project_templates.")}}, {"name", "location"});
        createProject.insert("oneOf", QJsonArray{
            QJsonObject{{"required", QJsonArray{"template_id"}}},
            QJsonObject{{"not", alternatives({"template_id", "template_data"})}}});
        put({"create_project"}, createProject);
        put({"list_project_templates"}, object(), true);
        const auto itemFolder = described(text(), "Existing project bucket or directory, e.g. Layers/Ambience.");
        auto createItem = object({{"parent", text(false)}, {"folder", itemFolder}, {"name", text()}, {"type", described(text(),
            "Sound, Container, Track, Segment, Layer, Action, Variable, Attenuation, Effect or Conversion. Use create_bus for a Bus.")},
            {"kind", choice({"Step", "Smooth"})}}, {"name", "type"});
        createItem.insert("not", QJsonObject{{"required", QJsonArray{"folder", "parent"}}});
        put({"create_item"}, createItem);
        const auto itemList = described(array(text(), 1, true), "Project item GUIDs or unambiguous paths or names.");
        put({"duplicate_items"}, object({{"items", itemList}}, {"items"}));
        put({"create_parent"}, object({{"items", itemList}, {"type", choice({"Container", "Track", "Segment", "Layer", "Bus"})},
            {"name", text()}}, {"items", "type", "name"}));
        put({"create_action"}, object({{"name", text()}, {"parent", text(false)}}, {"name"}));
        const auto clipMove = object({{"source_track", text()}, {"destination_track", text()},
            {"clip_guid", uuid()}, {"start", described(nonnegativeInt64, "Destination position in the Segment's time domain (nanoseconds or musical ticks).")}},
            {"source_track", "destination_track", "clip_guid", "start"});
        put({"move_track_clips"}, object({{"moves", array(clipMove, 1)}}, {"moves"}));
        put({"rename_item"}, itemSelector({{"new_name", text()}}, {"new_name"}, true));
        put({"delete_item"}, itemSelector());
        auto clipboardItems = object({{"path", text()}, {"object_id", uint32()}, {"items", itemList}});
        clipboardItems.insert("oneOf", QJsonArray{alternatives({"path", "object_id"}),
            QJsonObject{{"required", QJsonArray{"items"}}}});
        put({"copy_item", "cut_item"}, clipboardItems);
        auto moveItem = itemSelector({{"new_parent", text()}, {"new_parent_id", uint32()}, {"folder", itemFolder}});
        moveItem.insert("oneOf", QJsonArray{alternatives({"new_parent", "new_parent_id"}),
            QJsonObject{{"required", QJsonArray{"folder"}}}});
        put({"move_item"}, moveItem);
        QJsonObject pasteFields{{"target_path", text()}, {"target_object_id", uint32()},
            {"source_path", text()}, {"source_object_id", uint32()}};
        auto propertyTargets = pasteFields;
        propertyTargets.insert("targets", itemList);
        auto propertiesPaste = object(propertyTargets);
        propertiesPaste.insert("oneOf", QJsonArray{alternatives({"target_path", "target_object_id"}),
            QJsonObject{{"required", QJsonArray{"targets"}}}});
        put({"paste_properties"}, described(propertiesPaste,
            "Paste properties to objects of the same type. Omit the source to use the session clipboard."));
        pasteFields.insert("op", choice({"copy", "cut"}));
        pasteFields.insert("target_bucket", described(text(), "Project bucket root, such as Variables or Layers. Accepts copied objects."));
        auto pasteItem = object(pasteFields);
        pasteItem.insert("oneOf", QJsonArray{alternatives({"target_path", "target_object_id"}),
            QJsonObject{{"required", QJsonArray{"target_bucket"}}}});
        put({"paste_item"}, pasteItem);
        put({"create_folder", "delete_folder"}, object({{"path", text()}}, {"path"}));
        put({"rename_folder"}, object({{"path", text()}, {"new_name", text()}}, {"path", "new_name"}));
        put({"move_folder"}, object({{"path", text()}, {"new_parent", text()}}, {"path", "new_parent"}));
        auto effectChain = array(effect());
        effectChain.insert("maxItems", static_cast<int>(EffectChainLedger::kCapacity));
        auto itemProperty = setItemPropertySchema();
        auto itemPropertyValues = itemProperty.value("propertyValues").toObject();
        itemPropertyValues.insert("kind", described(integer(0, 1), "Variable kind. 0 selects Step and 1 selects Smooth."));
        itemPropertyValues.insert("defaultValue", number());
        itemPropertyValues.insert("rangeMin", nonnegative());
        itemPropertyValues.insert("rangeMax", nonnegative());
        auto range = tuple(2);
        range.insert("items", nonnegative());
        itemPropertyValues.insert("range", range);
        itemPropertyValues.insert("labels", array(object({{"id", uuid()}, {"name", text(false)},
            {"anchor", integer(0, VariableSemantics::kStepExactIntBound)}}, {"anchor", "name"})));
        itemPropertyValues.insert("effects", described(effectChain,
            "Complete Bus FX chain in processing order. Preserve slot guid and bypass when replacing or converting an effect. Use replace_field_group_effects for playable definitions."));
        itemProperty.insert("propertyValues", itemPropertyValues);
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
        const auto sendType = withDefault(choice({"Send", "Sidechain"}), "Send");
        const auto duckTarget = withDefault(boolean(), false);
        const auto sendRoute = [&](QJsonObject fields, const QJsonArray& required) {
            fields.insert("type", sendType);
            fields.insert("duckTarget", duckTarget);
            auto schema = object(fields, required);
            schema.insert("anyOf", QJsonArray{
                QJsonObject{{"properties", QJsonObject{{"duckTarget", QJsonObject{{"const", false}}}}}},
                QJsonObject{{"required", QJsonArray{"type"}},
                    {"properties", QJsonObject{{"type", QJsonObject{{"const", "Sidechain"}}}}}}
            });
            return schema;
        };
        const auto send = sendRoute({{"src_bus", text()}, {"target_bus_id", uint32()},
            {"level", described(withDefault(nonnegative(), 1), "Linear gain: 0 is silence (-96 dB), 1 is unity, 0.5 is approximately -6.02 dB.")}}, {"src_bus", "target_bus_id"});
        put({"set_send", "set_send_level"}, send);
        put({"remove_send"}, sendRoute({{"src_bus", text()}, {"target_bus_id", uint32()}}, {"src_bus", "target_bus_id"}));

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

        const auto scopeProperties = QJsonObject{{"definition_guid", uuid()}, {"group", choice({"output", "send", "spatialRating", "effect", "conversion"})},
            {"scope", choice({"Shared"})}, {"platform_ids", array(projectPlatformIdSchema(), 1, true)},
            {"expected_before", described({{"type", "object"}}, "Complete previous fieldGroups state for optimistic concurrency; validated by the field-group domain parser.")}};
        for (const auto* name : {"set_field_group_mode", "set_field_group_field", "clear_field_group_field_override",
                                "replace_field_group_sends", "replace_field_group_effects", "rebuild_field_group_local"}) {
            auto fields = scopeProperties;
            QJsonArray required{"definition_guid", "group"};
            const QString command = QLatin1String(name);
            if (command == "set_field_group_mode") { fields.insert("mode", choice({"Local", "Inherit", "UseShared"})); required.append("mode"); }
            if (command == "set_field_group_field" || command == "clear_field_group_field_override") { fields.insert("field", text()); required.append("field"); }
            if (command == "set_field_group_field") { fields.insert("value", described({}, "Typed value: GUID for outputBusRef/spatial.attenuationRef/settingsRef, boolean for spatial.is3DEnabled, number for scalar fields; null resets outputBusRef to Master or clears settingsRef.")); required.append("value"); }
            if (command == "replace_field_group_sends") {
                fields.insert("sends", array(sendRoute({{"targetBusID", uuid()}, {"sendLevel", number()}},
                                                      {"targetBusID", "sendLevel"})));
                required.append("sends");
            }
            if (command == "replace_field_group_effects") {
                fields.insert("effects", effectChain);
                required.append("effects");
            }
            auto schema = object(fields, required);
            if (command == "set_field_group_field") {
                schema.insert("allOf", QJsonArray{QJsonObject{{"oneOf", QJsonArray{
                    QJsonObject{{"properties", QJsonObject{{"field", choice({"outputBusRef", "settingsRef"})}, {"value", QJsonObject{{"anyOf", QJsonArray{uuid(), QJsonObject{{"type", "null"}}}}}}}}},
                    QJsonObject{{"properties", QJsonObject{{"field", choice({"spatial.attenuationRef"})}, {"value", uuid()}}}},
                    QJsonObject{{"properties", QJsonObject{{"field", choice({"spatial.is3DEnabled"})}, {"value", boolean()}}}},
                    QJsonObject{{"properties", QJsonObject{{"field", text()}, {"value", number()}}},
                        {"not", QJsonObject{{"properties", QJsonObject{{"field", choice({"outputBusRef", "settingsRef", "spatial.attenuationRef", "spatial.is3DEnabled"})}}}}}}
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
            {"device_mode", described(withDefault(choice({"real", "null", "null_realtime"}), "real"), "real uses an audio device; null runs the actual runtime silently without audio hardware and advances audio only when explicitly pumped; null_realtime also runs silently but advances audio in real time while output is started.")}}));
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
        const auto importPaths = described(array(text(), 1), "Audio files or folders to scan recursively for supported audio files.");
        auto importFiles = object({{"files", importPaths}, {"target_container_id", described(uint32(),
            "Target authoring handle: container, sound or output bus. Omit to import at the project root.")},
            {"language", described(text(), "Project language group of the target Sound. Defaults to the preview language.")},
            {"entry_id", described(uuid(), "Target Sound source row. Its group determines the language and resolves ambiguous missing files.")}}, {"files"});
        importFiles.insert("anyOf", QJsonArray{QJsonObject{{"required", QJsonArray{"target_container_id"}}},
            QJsonObject{{"not", alternatives({"language", "entry_id"})}}});
        put({"import_audio_files"}, importFiles);
        const auto resourceFolder = described(text(false), "Folder relative to the project audio sources root. Omit or use an empty string for the root.");
        put({"list_audio_resources"}, described(object({{"folder", resourceFolder}}),
            "List folders and original audio files directly inside a Resources folder."), true);
        put({"create_audio_folder"}, described(object({{"folder", resourceFolder}, {"name", text()}}, {"name"}),
            "Create an empty Resources folder. Returns the actual relative path."));
        put({"rename_audio_resource"}, described(object({{"path", text()}, {"name", text()}}, {"path", "name"}),
            "Rename an original audio file or folder, preserving references. Include the existing extension for files."));
        put({"move_audio_resources"}, described(object({{"paths", array(text(), 1, true)}, {"folder", resourceFolder}}, {"paths"}),
            "Move relative Resources paths into an existing Resources folder, preserving source identities."));
        put({"delete_audio_folder"}, described(object({{"path", text()}}, {"path"}), "Delete an empty Resources folder."));
        put({"restore_audio_resource"}, described(object({{"source_id", uint32()}, {"missing_id", uint32()}}, {"source_id", "missing_id"}),
            "Restore a missing audio resource by renaming an existing resource and redirecting the missing resource's references."));
        put({"relink_audio_resource"}, described(object({{"source_id", uint32()}, {"file", text()}}, {"source_id", "file"}),
            "Replace an original Resources file with the chosen audio file, keeping its source ID, path and references. Supports undo and discard."));
        put({"import_audio_resources"}, described(object({{"files", importPaths}, {"folder", resourceFolder},
            {"replace", withDefault(boolean(), false)}}, {"files"}),
            "Import original audio files into Resources without creating Sounds. Set replace to overwrite different contents at matching paths."));
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
            {"remote_discover", "Broadcast discovery for running games with the development runtime; poll get_remote_status for discovered targets."},
            {"remote_connect", "Connect to a development runtime at address and port; poll get_remote_status until connected or failed."},
            {"remote_disconnect", "Disconnect from the current remote runtime."},
            {"get_remote_status", "Return the remote connection state, discovered targets and event capture state."},
            {"get_remote_catalog", "Return the connected runtime's Action, Variable and Bus IDs, with names resolved from the open project."},
            {"poll_remote_events", "Return captured remote events from cursor onward, the next cursor, and how many events were missed or dropped."},
            {"start_remote_capture", "Start capturing events from the connected remote runtime; with output_path, received records are also written to a session file that only this Studio version opens."},
            {"stop_remote_capture", "Stop capturing remote events; captured events remain pollable."},
            {"post_action", "Execute an authored Action and return its playback handle."},
            {"seek_action", "Request a seek in milliseconds. Poll query_seek_action with seek_operation_id to observe completion, then release_seek_action to release its record."},
            {"query_seek_action", "Read the state and results of a seek operation without changing it."},
            {"release_seek_action", "Release a completed seek operation record."},
            {"batch_replace_media", "Replace imported sources by source ID. All input rows must be valid; per-source I/O failures return an error with partial results."},
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
