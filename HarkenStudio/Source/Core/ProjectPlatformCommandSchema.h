#pragma once

#include "ProjectPlatform.h"

namespace harken::studio {

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
    const QJsonObject ids{{"type", "array"}, {"uniqueItems", true}, {"items", projectPlatformIdSchema()}};
    if (command == "list_project_platforms" || command == "get_pack_build_status"
        || command == "get_last_pack_build_records" || command == "cancel_pack_build") return object({});
    if (command == "create_project_platform") return object({
        {"name", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 120}}},
        {"base_platform", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}}}, {"name", "base_platform"});
    if (command == "rename_project_platform")
        return object({{"platform_id", projectPlatformIdSchema()}, {"name", text}}, {"platform_id", "name"});
    if (command == "set_project_platform_base")
        return object({{"platform_id", projectPlatformIdSchema()}, {"base_platform", QJsonObject{
            {"type", "string"}, {"enum", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}}},
            {"platform_id", "base_platform"});
    if (command == "delete_project_platform" || command == "rescan_media_sources")
        return object({{"platform_id", projectPlatformIdSchema()}}, {"platform_id"});
    if (command == "get_media_status")
        return object({{"platform_id", projectPlatformIdSchema()}, {"source_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"uniqueItems", false}, {"items", source}}}},
                      {"platform_id", "source_ids"});
    if (command == "export_pack") {
        auto schema = object({
            {"platform_id", projectPlatformIdSchema()},
            {"platform_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"uniqueItems", true},
                {"items", projectPlatformIdSchema()},
                {"description", "Project platform UUIDs, mutually exclusive with platform_id. Omit both to use the export selection."}}},
            {"output_path", text}, {"action_name", text}, {"language", text},
            {"all", QJsonObject{{"type", "boolean"}, {"default", false},
                {"description", "Export every Action for the requested platforms, including unchecked packs. Cannot be true with action_name."}}},
            {"force", QJsonObject{{"type", "boolean"}, {"default", false},
                {"description", "Force re-export even when pack inputs are unchanged."}}},
            {"self_clean", QJsonObject{{"type", "boolean"}, {"default", false},
                {"description", "Remove outputs outside this export after all requested platforms succeed. Cannot be true with action_name."}}}});
        schema.insert("allOf", QJsonArray{
            QJsonObject{{"not", QJsonObject{{"required", QJsonArray{"platform_id", "platform_ids"}}}}},
            QJsonObject{{"not", QJsonObject{{"required", QJsonArray{"action_name", "all"}},
                {"properties", QJsonObject{{"action_name", QJsonObject{{"minLength", 1}}}, {"all", QJsonObject{{"const", true}}}}}}}},
            QJsonObject{{"not", QJsonObject{{"required", QJsonArray{"action_name", "self_clean"}},
                {"properties", QJsonObject{{"action_name", QJsonObject{{"minLength", 1}}}, {"self_clean", QJsonObject{{"const", true}}}}}}}}});
        return schema;
    }
    if (command == "get_action_pack_plan")
        return object({{"actionStableId", QJsonObject{{"type", "string"}, {"format", "uuid"},
            {"description", "Action runtime UUID returned by get_item_info or list_actions."}}},
                       {"platform_ids", ids}, {"languages", QJsonObject{{"type", "array"}, {"uniqueItems", true}, {"items", text}}}}, {"actionStableId"});
    if (command == "apply_project_platforms") {
        const auto fields = QJsonObject{{"id", projectPlatformIdSchema()}, {"name", text},
            {"basePlatform", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"Windows", "macOS", "Linux", "Android", "iOS"}}}}};
        const auto rows = QJsonObject{{"type", "array"}, {"minItems", 1}, {"items", object(fields, {"name", "basePlatform"})}};
        return object({{"platforms", rows}, {"expected_platforms", rows}}, {"platforms", "expected_platforms"});
    }
    return {};
}

}
