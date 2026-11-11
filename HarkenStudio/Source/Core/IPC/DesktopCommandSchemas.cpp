#include "DesktopCommandSchemas.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <climits>

namespace VNX {
namespace {

QJsonObject text(const QString& description = {})
{
    QJsonObject schema{{"type", "string"}, {"minLength", 1}};
    if (!description.isEmpty()) schema["description"] = description;
    return schema;
}

QJsonObject integer(double minimum = 0, double maximum = INT_MAX)
{
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject flag(bool defaultValue)
{
    return {{"type", "boolean"}, {"default", defaultValue}};
}

QJsonObject choice(const QJsonArray& values)
{
    return {{"type", "string"}, {"enum", values}};
}

QJsonObject object(QJsonObject properties = {}, QJsonArray required = {})
{
    return {{"type", "object"}, {"properties", properties},
        {"required", required}, {"additionalProperties", false}};
}

QJsonObject needs(const char* field)
{
    return {{"required", QJsonArray{field}}};
}

QJsonObject target(QJsonObject properties = {}, QJsonArray required = {}, bool optional = false)
{
    properties["name"] = text("Exact unique widget objectName. Use widget_id when names repeat.");
    properties["widget_id"] = text("Opaque widget identity returned by discovery, valid only while that widget exists in this Studio session.");
    properties["scope_widget_id"] = text("Restrict name lookup to this discovered widget and its descendants.");
    auto schema = object(properties, required);
    if (optional)
        schema["not"] = QJsonObject{{"required", QJsonArray{"name", "widget_id"}}};
    else
        schema["oneOf"] = QJsonArray{needs("name"), needs("widget_id")};
    return schema;
}

const QMap<QString, QJsonObject>& schemas()
{
    static const QMap<QString, QJsonObject> values = [] {
        QMap<QString, QJsonObject> result;
        for (const auto* name : {"get_blueprint", "get_blueprint_layers", "list_open_dialogs",
            "get_focused_widget", "get_last_interacted_widget", "list_ui_windows", "get_theme",
            "get_remote_connection_status", "list_named_layouts", "get_selected_bus",
            "get_current_layout", "get_layout_state", "list_inspector_panels", "list_panels",
            "get_canvas_flow", "open_remote_connect"}) result[name] = object();
        result["get_ui_operation"] = object({{"operation_id", text(
            "ID returned by a queued UI command. Retains the latest 256 completed operations per session. Success means the UI operation returned, not downstream business completion.")}}, {"operation_id"});
        result["get_ui_tree"] = target({{"depth", integer(0, 10)}}, {}, true);
        result["find_widget"] = object({{"name", text()}, {"text", text()}, {"type", text()},
            {"scope_widget_id", text()}, {"visible_only", flag(true)}, {"limit", integer(1, 1000)}});
        result["find_widget"].insert("anyOf", QJsonArray{needs("name"), needs("text"), needs("type")});
        result["grab_widget"] = target({{"max_width", integer(1, 16384)}});
        result["get_widget_value"] = target();
        result["get_widget_children"] = target({{"named_only", flag(false)}, {"depth", integer(0, 5)}});
        result["get_table_data"] = target({{"max_rows", integer(0, 5000)}});
        result["get_list_items"] = target({{"max_items", integer(0, 5000)}});
        result["get_tree_data"] = target({{"max_depth", integer(1, 50)}});
        result["click_widget"] = target({{"button", choice({"left", "right", "middle"})},
            {"double_click", flag(false)}, {"x", integer()}, {"y", integer()}});
        result["key_press"] = target({{"key", integer(1)}, {"modifiers", integer()}}, {"key"}, true);
        result["set_widget_value"] = target({{"value", QJsonObject{
            {"type", QJsonArray{"string", "number", "boolean"}},
            {"description", "Value appropriate for the discovered widget type and range; booleans for checkable controls, strings for text controls, numeric values for sliders/spinners/list rows."}}}}, {"value"});
        result["select_table_row"] = target({{"row", integer()}, {"match_text", text()},
            {"match_column", QJsonObject{{"anyOf", QJsonArray{integer(), text()}}}},
            {"match_contains", flag(false)}});
        result["select_table_row"].insert("allOf", QJsonArray{QJsonObject{
            {"oneOf", QJsonArray{needs("row"), needs("match_text")}}}});
        result["open_context_menu"] = target({{"x", integer()}, {"y", integer()}});
        const QJsonObject bothCoordinates{{"required", QJsonArray{"x", "y"}}};
        const QJsonObject anyCoordinate{{"anyOf", QJsonArray{needs("x"), needs("y")}}};
        const QJsonObject noCoordinates{{"not", anyCoordinate}};
        const QJsonObject coordinatePair{{"anyOf", QJsonArray{bothCoordinates, noCoordinates}}};
        result["open_context_menu"].insert("allOf", QJsonArray{coordinatePair});
        result["click_widget"].insert("allOf", QJsonArray{coordinatePair});
        result["trigger_menu_action"] = target({{"action_text", text()}}, {"action_text"}, true);
        result["answer_dialog"] = object({{"response", text("Enabled button text, or accept/ok/reject/cancel where supported by the active dialog.")},
            {"text", QJsonObject{{"type", "string"}}}, {"path", text()}});
        result["dismiss_dialog"] = object({{"id", text()}, {"result", integer(0, 1)}}, {"id"});
        result["flush_ui_events"] = object({{"max_iterations", integer(1, 50)}});
        result["set_blueprint_layer"] = object({{"layer", choice({"children", "parents", "bus"})},
            {"visible", flag(true)}}, {"layer"});
        result["set_theme"] = object({{"theme", text()}}, {"theme"});
        result["get_panel_rect"] = object({{"name", text("Unique name from list_panels.")}}, {"name"});
        result["get_inspector_context"] = object({{"panel_id", integer()}});
        result["create_inspector_panel"] = object({{"path", text()}});
        result["create_editor"] = object({{"type", text("Creatable workspace panel type or label.")}}, {"type"});
        result["set_inspector_pinned"] = object({{"panel_id", integer()}, {"pinned", flag(true)}}, {"panel_id"});
        result["close_inspector_panel"] = object({{"panel_id", integer()}}, {"panel_id"});
        result["get_recent_logs"] = object({{"count", integer(1, 10000)}, {"level", text()}});
        result["select_bus"] = object({{"bus_id", integer(1, 4294967295.0)}}, {"bus_id"});
        for (const auto* name : {"select_item", "select_project_item", "select_folder"})
            result[name] = object({{"path", text()}}, {"path"});
        result["switch_layout"] = object({{"layout", text("Built-in layout ID from get_layout_state.")}}, {"layout"});
        result["reset_layout"] = object({{"layout", text("Omit to reset the current layout.")}});
        result["toggle_panel"] = object({{"name", text("Unique name from list_panels.")}, {"visible", QJsonObject{{"type", "boolean"}}}}, {"name"});
        for (const auto* name : {"save_named_layout", "load_named_layout", "delete_named_layout"})
            result[name] = object({{"name", text()}}, {"name"});
        result["open_curve_editor"] = object({{"name", text()},
            {"guid", QJsonObject{{"type", "string"}, {"format", "uuid"}}},
            {"dimension", choice({"volume", "pitch"})}});
        result["open_curve_editor"].insert("oneOf", QJsonArray{needs("name"), needs("guid")});
        for (const auto* name : {"connect_nodes", "disconnect_nodes"})
            result[name] = object({{"output_node", integer(1, 4294967295.0)}, {"input_node", integer(1, 4294967295.0)},
                {"output_node_kind", choice({"item", "bus"})}, {"input_node_kind", choice({"item", "bus"})},
                {"edge_type", text("Connection kind supported by the active Blueprint source.")}}, {"output_node", "input_node"});
        result["set_node_override"] = object({{"node_id", integer(1, 4294967295.0)},
            {"override", QJsonObject{{"type", "boolean"}}}}, {"node_id", "override"});
        result["set_canvas_flow"] = object({{"direction", choice({"LR", "RL", "TB", "BT"})}}, {"direction"});
        result["batch"] = object({{"commands", QJsonObject{{"type", "array"}, {"minItems", 1},
            {"items", object({{"cmd", text()}, {"args", QJsonObject{{"type", "object"}}}}, {"cmd"})}}},
            {"stop_on_error", flag(true)}}, {"commands"});
        return result;
    }();
    return values;
}

}

QJsonObject desktopCommandInputSchema(const QString& name)
{
    return schemas().value(name);
}

bool desktopCommandIsReadOnly(const QString& name)
{
    static const QSet<QString> names{"get_blueprint", "get_blueprint_layers", "list_open_dialogs",
        "get_focused_widget", "get_last_interacted_widget", "list_ui_windows", "get_theme",
        "get_remote_connection_status", "list_named_layouts", "get_selected_bus", "get_current_layout",
        "get_layout_state", "list_inspector_panels", "list_panels", "get_canvas_flow", "get_ui_operation",
        "get_ui_tree", "find_widget", "grab_widget", "get_widget_value", "get_widget_children",
        "get_table_data", "get_list_items", "get_tree_data", "get_panel_rect", "get_inspector_context",
        "get_recent_logs"};
    return names.contains(name);
}

}
