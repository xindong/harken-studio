#include "../ActionManager.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "../ProjectManager.h"
#include "../FieldGroupEditing.h"
#include "../FieldGroupJson.h"
#include "../FieldGroupProjectAdapter.h"

#include <QJsonDocument>

namespace VNX {
namespace {

using Property = harkenplayableproperties::PlayablePropertyIdV1;
using Guid = harkenprojectschema::AuthoringGuid;

bool parseGuid(const QJsonValue& value, Guid& guid)
{
    if (!value.isString()) return false;
    const auto text = value.toString();
    const QUuid parsed(text);
    if (text.size() != 36 || parsed.toString(QUuid::WithoutBraces).compare(text, Qt::CaseInsensitive) != 0)
        return false;
    guid = toFieldGroupGuid(parsed);
    return true;
}

ActionResult invalid(const QString& message)
{
    return {false, {}, QStringLiteral("INVALID_PARAM"), message};
}

ActionResult executeGroupEdit(ProjectManager* manager, const QString& action,
                              const QJsonObject& args)
{
    if (!manager || !manager->currentProject())
        return {false, {}, QStringLiteral("NO_PROJECT"), QStringLiteral("No project open")};
    auto& project = *manager->currentProject();
    FieldGroupEditRequest request;
    Guid definition;
    if (!parseGuid(args.value(QStringLiteral("definition_guid")), definition)
        || harkenprojectschema::IsNull(definition))
        return invalid(QStringLiteral("definition_guid must identify an object definition"));
    request.definition = fromFieldGroupGuid(definition);
    const auto groupName = args.value(QStringLiteral("group")).toString();
    if (groupName == QStringLiteral("mainOutput")) request.group = FieldGroupId::MainOutput;
    else if (groupName == QStringLiteral("send")) request.group = FieldGroupId::Send;
    else if (groupName == QStringLiteral("spatialRating")) request.group = FieldGroupId::SpatialRating;
    else return invalid(QStringLiteral("Unknown field group"));
    if (args.value(QStringLiteral("scope")) == QJsonValue(QStringLiteral("Shared"))) {
        if (args.contains(QStringLiteral("platform_ids")))
            return invalid(QStringLiteral("Shared and platform scopes are mutually exclusive"));
        request.scopes.shared = true;
    } else {
        if (args.contains(QStringLiteral("scope")) || !args.value(QStringLiteral("platform_ids")).isArray())
            return invalid(QStringLiteral("Specify Shared scope or an explicit platform list"));
        for (const auto& entry : args.value(QStringLiteral("platform_ids")).toArray()) {
            const auto platform = ProjectPlatformSemantics::parseId(entry.toString());
            if (!entry.isString() || !platform
                || !ProjectPlatformSemantics::find(project.platforms, *platform))
                return invalid(QStringLiteral("Unknown project platform ID"));
            request.scopes.platforms.push_back(toFieldGroupGuid(*platform));
        }
    }
    if (args.contains(QStringLiteral("occurrence_target"))
        || args.contains(QStringLiteral("clip_guid")))
        return invalid(QStringLiteral("Field groups only edit definitions"));
    if (args.contains(QStringLiteral("expected_before"))) {
        if (!args.value(QStringLiteral("expected_before")).isObject())
            return invalid(QStringLiteral("expected_before must contain field groups"));
        DefinitionFieldGroups expected;
        std::string error;
        const auto text = QJsonDocument(args.value(QStringLiteral("expected_before")).toObject()).toJson(QJsonDocument::Compact);
        if (!ReadFieldGroupsJson(text.toStdString(), expected, error))
            return invalid(QString::fromStdString(error));
        request.expectedBefore = std::move(expected);
    }
    if (action == QStringLiteral("set_field_group_mode")) {
        const auto mode = args.value(QStringLiteral("mode")).toString();
        if (mode == QStringLiteral("Local")) request.operation = SetFieldGroupMode{FieldGroupMode::Local};
        else if (mode == QStringLiteral("Inherit")) request.operation = SetFieldGroupMode{FieldGroupMode::Inherit};
        else if (mode == QStringLiteral("UseShared")) request.operation = SetFieldGroupMode{FieldGroupMode::UseShared};
        else return invalid(QStringLiteral("Unknown field group mode"));
    } else if (action == QStringLiteral("rebuild_field_group_local")) {
        request.operation = RebuildLocalFieldGroup{};
    } else if (action == QStringLiteral("replace_field_group_sends")) {
        if (!args.value(QStringLiteral("sends")).isArray())
            return invalid(QStringLiteral("sends must be a complete array"));
        SendFieldGroup sends;
        for (const auto& entry : args.value(QStringLiteral("sends")).toArray()) {
            const auto row = entry.toObject();
            SendFieldGroupRow send;
            if (!entry.isObject() || row.size() != 2
                || !parseGuid(row.value(QStringLiteral("targetBusID")), send.targetBusID)
                || !row.value(QStringLiteral("sendLevel")).isDouble())
                return invalid(QStringLiteral("Invalid send row"));
            send.sendLevel = static_cast<float>(row.value(QStringLiteral("sendLevel")).toDouble());
            sends.sends.push_back(send);
        }
        request.operation = ReplaceFieldGroupSends{std::move(sends)};
    } else {
        const auto* field = PlayablePropertyAuthoringV1::fieldByJsonKey(args.value(QStringLiteral("field")).toString());
        if (!field) return invalid(QStringLiteral("Unknown group field"));
        SetFieldGroupField edit{static_cast<Property>(field->id), 0.0f};
        if (action == QStringLiteral("clear_field_group_field_override")) {
            request.operation = ClearFieldGroupFieldOverride{edit.property};
        } else {
            const auto value = args.value(QStringLiteral("value"));
            if (edit.property == Property::MainOutputBusRef || edit.property == Property::SpatialAttenuationRef) {
                Guid ref{};
                if (!(value.isNull() && edit.property == Property::MainOutputBusRef) && !parseGuid(value, ref))
                    return invalid(QStringLiteral("Group reference must be a GUID"));
                edit.value = ref;
            } else if (edit.property == Property::SpatialIs3DEnabled) {
                if (!value.isBool()) return invalid(QStringLiteral("Group field must be a boolean"));
                edit.value = value.toBool();
            } else {
                if (!value.isDouble()) return invalid(QStringLiteral("Group field must be a number"));
                edit.value = static_cast<float>(value.toDouble());
            }
            request.operation = edit;
        }
    }
    const auto prepared = prepareFieldGroupEdit(project, request);
    if (!prepared.ok()) {
        QJsonArray diagnostics;
        for (const auto& error : prepared.diagnostics) {
            QJsonObject detail{
                {"definition_guid", error.definition.toString(QUuid::WithoutBraces)},
                {"message", error.message}, {"field", error.field},
                {"scope", error.scope ? ProjectPlatformSemantics::idString(fromFieldGroupGuid(*error.scope))
                                       : QStringLiteral("Shared")}};
            if (error.group)
                detail.insert(QStringLiteral("group"), QString::fromLatin1(FieldGroupName(*error.group).data()));
            if (error.row) detail.insert(QStringLiteral("row"), static_cast<qint64>(*error.row));
            if (!harkenprojectschema::IsNull(error.relatedGuid))
                detail.insert(QStringLiteral("reference_guid"), fromFieldGroupGuid(error.relatedGuid).toString(QUuid::WithoutBraces));
            diagnostics.append(detail);
        }
        return {false, {{"diagnostics", diagnostics}}, QStringLiteral("FIELD_GROUP_REJECTED"),
                prepared.diagnostics.front().message};
    }
    ActionResult result;
    result.success = true;
    result.data.insert(QStringLiteral("changed"), prepared.changed);
    if (!prepared.changed) return result;
    const auto source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    auto* item = source->byQtGuid.at(request.definition);
    if (!manager->restoreFieldGroupsInternal(item, *prepared.after,
                                               prepared.before, ActionManager::instance().executeOriginator()))
        return {false, {}, QStringLiteral("FIELD_GROUP_REJECTED"), QStringLiteral("Field group state changed before commit")};
    const auto projectId = project.projectId;
    const auto restore = [manager, projectId, guid = request.definition](
                             const DefinitionFieldGroups& from, const DefinitionFieldGroups& to) {
        auto* current = manager->currentProject();
        if (!current || current->projectId != projectId) return;
        const auto source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*current);
        const auto found = source->byQtGuid.find(guid);
        if (found != source->byQtGuid.end())
            manager->restoreFieldGroupsInternal(found->second, to, from);
    };
    result.undoText = QStringLiteral("Edit Field Group");
    result.undoFn = [restore, before = *prepared.before, after = *prepared.after] { restore(after, before); };
    result.redoFn = [restore, before = *prepared.before, after = *prepared.after] { restore(before, after); };
    return result;
}

}

void registerFieldGroupActions(ActionManager& manager, ProjectManager* projectManager)
{
    for (const auto* name : {"set_field_group_mode", "set_field_group_field",
                             "replace_field_group_sends", "rebuild_field_group_local",
                             "clear_field_group_field_override"}) {
        ActionDef action;
        action.id = QLatin1String(name);
        action.displayName = action.id;
        action.category = QStringLiteral("project");
        action.paramSchema = {{"definition_guid", "string"}, {"group", "string"},
                              {"scope", "string?"}, {"platform_ids", "array?"},
                              {"expected_before", "object?"}};
        if (action.id == QStringLiteral("set_field_group_mode"))
            action.paramSchema.insert(QStringLiteral("mode"), QStringLiteral("string"));
        else if (action.id == QStringLiteral("clear_field_group_field_override"))
            action.paramSchema.insert(QStringLiteral("field"), QStringLiteral("string"));
        else if (action.id == QStringLiteral("set_field_group_field")) {
            action.paramSchema.insert(QStringLiteral("field"), QStringLiteral("string"));
            action.paramSchema.insert(QStringLiteral("value"), QStringLiteral("any"));
        } else if (action.id == QStringLiteral("replace_field_group_sends"))
            action.paramSchema.insert(QStringLiteral("sends"), QStringLiteral("array"));
        action.handler = [projectManager, id = action.id](const QJsonObject& args) {
            return executeGroupEdit(projectManager, id, args);
        };
        manager.registerAction(action);
    }
}

}
