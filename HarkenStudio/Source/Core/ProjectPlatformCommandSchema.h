#pragma once

#include "ProjectPlatform.h"

namespace VNX {

inline QJsonObject projectPlatformIdSchema()
{
    return {{"type", "string"}, {"pattern", "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"},
            {"description", "Project platform UUID returned by list_project_platforms; names and base types are not IDs."}};
}

inline QJsonObject projectPlatformCommandSchema(const QString& command)
{
    const auto object = [](const QJsonObject& properties, const QJsonArray& required = {}) {
        return QJsonObject{{"type", "object"}, {"properties", properties},
                           {"required", required}, {"additionalProperties", false}};
    };
    const QJsonObject text{{"type", "string"}};
    const QJsonObject source{{"type", "integer"}, {"minimum", 1}, {"maximum", double(UINT32_MAX)}};
    const QJsonObject ids{{"type", "array"}, {"items", projectPlatformIdSchema()}};
    QJsonObject configProperties;
    const auto defaults = ConversionParameters{}.toJson();
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        configProperties.insert(it.key(), QJsonObject{{"type", it.value().isBool() ? "boolean" : "number"}});
    for (const auto& key : {"format", "vorbisEncodeMode", "targetSampleRate", "vorbisAverageBitrate", "vorbisMinBitrate", "vorbisMaxBitrate"})
        configProperties.insert(key, QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", double(UINT32_MAX)}});
    configProperties.insert("format", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 5}});
    configProperties.insert("vorbisEncodeMode", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 1}});
    configProperties.insert("targetSampleRate", source);
    configProperties.insert("vorbisQuality", QJsonObject{{"type", "number"}, {"minimum", -0.1}, {"maximum", 1}});
    const auto config = object(configProperties, QJsonArray::fromStringList(defaults.keys()));
    const QJsonObject bound{{"type", "integer"}, {"minimum", 0}, {"maximum", qint64(INT64_MAX)}};
    const QJsonObject upper{{"anyOf", QJsonArray{bound, QJsonObject{{"type", "null"}}}}};
    const auto profile = object({{"performResample", QJsonObject{{"type", "boolean"}}}, {"sizePolicies", QJsonObject{
        {"type", "array"}, {"minItems", 1},
        {"description", "Unique increasing inclusive byte limits. Each row starts after the preceding limit; the last maxBytes is null (Infinite)."},
        {"items", object({{"maxBytes", upper}, {"config", config}}, {"maxBytes", "config"})}}}},
        {"performResample", "sizePolicies"});
    const QJsonObject nullableProfile{{"anyOf", QJsonArray{profile, QJsonObject{{"type", "null"}}}}};
    if (command == "list_project_platforms" || command == "get_pack_build_status"
        || command == "get_last_pack_build_records" || command == "cancel_pack_build") return object({});
    if (command == "create_project_platform") return object({
        {"name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 120}}},
        {"base_platform", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}},
        {"profile", nullableProfile}}, {"name", "base_platform"});
    if (command == "rename_project_platform")
        return object({{"platform_id", projectPlatformIdSchema()}, {"name", text}}, {"platform_id", "name"});
    if (command == "set_project_platform_base")
        return object({{"platform_id", projectPlatformIdSchema()}, {"base_platform", QJsonObject{
            {"type", "string"}, {"enum", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}}},
            {"platform_id", "base_platform"});
    if (command == "set_project_platform_profile")
        return object({{"platform_id", projectPlatformIdSchema()}, {"profile", nullableProfile}}, {"platform_id", "profile"});
    if (command == "delete_project_platform" || command == "get_platform_profile"
        || command == "rescan_media_sources")
        return object({{"platform_id", projectPlatformIdSchema()}}, {"platform_id"});
    if (command == "transcode_asset" || command == "get_transcode_status")
        return object({{"platform_id", projectPlatformIdSchema()}, {"source_id", source}}, {"platform_id", "source_id"});
    if (command == "batch_convert_media" || command == "get_media_status")
        return object({{"platform_id", projectPlatformIdSchema()}, {"source_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"items", source}}}},
                      {"platform_id", "source_ids"});
    if (command == "export_pack")
        return object({{"platform_id", projectPlatformIdSchema()}, {"output_path", text}, {"action_name", text}, {"language", text}});
    if (command == "get_action_pack_plan")
        return object({{"actionStableId", QJsonObject{{"type", "integer"}, {"minimum", 1}}},
                       {"platform_ids", ids}, {"languages", QJsonObject{{"type", "array"}, {"items", text}}}}, {"actionStableId"});
    if (command == "apply_project_platforms") {
        const auto fields = QJsonObject{{"id", projectPlatformIdSchema()}, {"name", text},
            {"basePlatform", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}},
            {"profile", profile}};
        const auto rows = QJsonObject{{"type", "array"}, {"minItems", 1}, {"items", object(fields, {"name", "basePlatform", "profile"})}};
        return object({{"platforms", rows}, {"expected_platforms", rows}}, {"platforms", "expected_platforms"});
    }
    return {};
}

}
