
#include "../ActionManager.h"
#include "../BindingEditing.h"
#include "../CurveSemantics.h"
#include "../ProjectManager.h"
#include "../Components/VMixNode.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

#include <cmath>

namespace VNX {

ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);
ProjectItem* resolveItemByPathOrObjectID(ProjectData* project,
                                         const QString& path,
                                         uint32_t objectID);
uint32_t itemShortID(const ProjectItem* item);

namespace {

bool finiteNumber(const QJsonValue& value)
{
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool validCurveTupleArray(const QJsonValue& value, int tupleSize, int maxSize)
{
    if (!value.isArray()) return false;
    const QJsonArray tuples = value.toArray();
    if (tuples.size() > maxSize) return false;
    for (const QJsonValue& tupleValue : tuples) {
        if (!tupleValue.isArray()) return false;
        const QJsonArray tuple = tupleValue.toArray();
        if (tuple.size() < tupleSize) return false;
        for (int i = 0; i < tupleSize; ++i)
            if (!finiteNumber(tuple.at(i))) return false;
    }
    return true;
}

ProjectItemType hostTypeOf(BindingHostClass hostClass)
{
    return hostClass == BindingHostClass::Bus
        ? ProjectItemType::Bus : ProjectItemType::Sound;
}

bool isBindableAddress(const BindingAddress& address)
{
    return BindingContractSemantics::isBindable(
        BindingContractSemantics::derive(address.slotId, hostTypeOf(address.hostClass)));
}

bool validBindingJson(ProjectManager* projectManager, const QJsonValue& value)
{
    if (!value.isObject()) return false;
    const QJsonObject binding = value.toObject();
    const auto address = BindingSemantics::parseAddress(binding);
    if (!address || !isBindableAddress(*address)) return false;
    if (!binding.value(QStringLiteral("variable")).isString()) return false;
    const QUuid variableId = QUuid::fromString(
        binding.value(QStringLiteral("variable")).toString());
    const ProjectItem* variable = projectManager->findItemByGuid(variableId);
    if (variableId.isNull() || !variable
        || variable->type != ProjectItem::Type::Variable) return false;
    const QJsonArray range = binding.value(QStringLiteral("range")).toArray();
    if (range.size() != 2 || !finiteNumber(range.at(0)) || !finiteNumber(range.at(1)))
        return false;
    const QJsonValue curveValue = binding.value(QStringLiteral("curve"));
    if (!curveValue.isObject()) return false;
    const QJsonObject curve = curveValue.toObject();
    const QJsonValue points = curve.value(QStringLiteral("points"));
    if (!validCurveTupleArray(points, 2, kCurveMaxPoints)
        || points.toArray().size() < 2) return false;
    const QJsonValue handles = curve.value(QStringLiteral("handles"));
    return validCurveTupleArray(handles, 4, kCurveMaxPoints - 1)
        && handles.toArray().size() == points.toArray().size() - 1;
}

ProjectItem* resolveBindingHost(const QJsonObject& args)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project) return nullptr;
    const QUuid guid = QUuid::fromString(args.value(QStringLiteral("guid")).toString());
    if (!guid.isNull())
        return ProjectManager::instance().findItemByGuid(guid);
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const uint32_t objectId = static_cast<uint32_t>(
        args.value(QStringLiteral("object_id")).toInteger(
            static_cast<qint64>(args.value(QStringLiteral("object_id")).toDouble(0))));
    if (!path.isEmpty() || objectId != 0)
        return resolveItemByPathOrObjectID(project, path, objectId);
    return nullptr;
}

QJsonObject hostData(const ProjectItem* item)
{
    return QJsonObject{
        {QStringLiteral("guid"), item->guid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("name"), item->name},
        {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)}
    };
}

ActionResult snapshotResult(ProjectItem* item, const BindingLedger& oldLedger,
                            const QString& undoText)
{
    const QJsonArray oldValue = oldLedger.toJson();
    const QJsonArray newValue = item->comp<VMixNode>()->bindings.toJson();
    ActionResult result;
    result.success = true;
    result.data = hostData(item);
    result.data[QStringLiteral("count")] = newValue.size();
    if (oldValue == newValue)
        return result;
    const QUuid guid = item->guid;
    auto apply = [guid](const QJsonArray& value) {
        ProjectItem* target = ProjectManager::instance().findItemByGuid(guid);
        BindingEditing::apply(target, BindingLedger::fromJson(value));
    };
    result.undoFn = [apply, oldValue]() { apply(oldValue); };
    result.redoFn = [apply, newValue]() { apply(newValue); };
    result.undoText = undoText;
    result.mergeId = 0;
    return result;
}

QString undoTextOf(const QJsonObject& args, const QString& fallback)
{
    const QString text = args.value(QStringLiteral("undo_text")).toString();
    return text.isEmpty() ? fallback : text;
}

ActionResult missingHost()
{
    return {false, {}, "NOT_FOUND", "Binding host not found"};
}

ActionResult replaceLedger(ProjectItem* item, const QJsonArray& input, const QString& undoText)
{
    auto* mix = BindingEditing::ledgerHost(item, BindingEditing::hostClassOf(item));
    if (!mix) return {false, {}, "NO_COMPONENT", "No MixNode on binding host"};
    auto* projectManager = &ProjectManager::instance();
    for (const QJsonValue& binding : input)
        if (!validBindingJson(projectManager, binding))
            return {false, {}, "INVALID_ARGS", "Invalid binding ledger"};
    const BindingLedger oldLedger = mix->bindings;
    BindingLedger replacement = BindingLedger::fromJson(input);
    if (!BindingEditing::apply(
            item, replacement, ActionManager::instance().executeOriginator()))
        return {false, {}, "INVALID_ARGS", "Invalid binding ledger"};
    return snapshotResult(item, oldLedger, undoText);
}

}

void registerBindingActions(ActionManager& mgr)
{
    {
        ActionDef def;
        def.id = "set_binding";
        def.displayName = "Set Binding";
        def.category = "project";
        def.paramSchema = QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("string")},
            {QStringLiteral("path"), QStringLiteral("string")},
            {QStringLiteral("object_id"), QStringLiteral("uint32")},
            {QStringLiteral("binding"), QStringLiteral("object")}
        };
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            ProjectItem* item = resolveBindingHost(args);
            if (!item) return missingHost();
            auto* mix = BindingEditing::ledgerHost(item, BindingEditing::hostClassOf(item));
            if (!mix) return {false, {}, "NO_COMPONENT", "No MixNode on binding host"};
            ParameterBindingDef binding = ParameterBindingDef::fromJson(
                args.value(QStringLiteral("binding")));
            if (!binding.typed || !isBindableAddress(binding.address))
                return {false, {}, "INVALID_ARGS", "Invalid binding"};
            const BindingLedger oldLedger = mix->bindings;
            if (!BindingEditing::upsert(
                    item, std::move(binding),
                    ActionManager::instance().executeOriginator()))
                return {false, {}, "INVALID_ARGS", "Invalid binding"};
            return snapshotResult(item, oldLedger, undoTextOf(args, QStringLiteral("Set Binding")));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "remove_binding";
        def.displayName = "Remove Binding";
        def.category = "project";
        def.paramSchema = QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("string")},
            {QStringLiteral("path"), QStringLiteral("string")},
            {QStringLiteral("object_id"), QStringLiteral("uint32")},
            {QStringLiteral("runtime_stable_id"), QStringLiteral("string")}
        };
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            ProjectItem* item = resolveBindingHost(args);
            if (!item) return missingHost();
            auto* mix = BindingEditing::ledgerHost(item, BindingEditing::hostClassOf(item));
            if (!mix) return {false, {}, "NO_COMPONENT", "No MixNode on binding host"};
            harkenstudio::ObjectId runtimeStableId{};
            if (!parseObjectIdJson(args.value(QStringLiteral("runtime_stable_id")), &runtimeStableId) ||
                harkenstudio::IsNull(runtimeStableId))
                return {false, {}, "INVALID_ARGS", "runtime_stable_id is required"};
            const BindingLedger oldLedger = mix->bindings;
            if (!BindingEditing::remove(
                    item, runtimeStableId,
                    ActionManager::instance().executeOriginator()))
                return {false, {}, "NOT_FOUND", "Binding not found"};
            return snapshotResult(item, oldLedger, undoTextOf(args, QStringLiteral("Remove Binding")));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "clear_bindings";
        def.displayName = "Clear Bindings";
        def.category = "project";
        def.paramSchema = QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("string")},
            {QStringLiteral("path"), QStringLiteral("string")},
            {QStringLiteral("object_id"), QStringLiteral("uint32")},
            {QStringLiteral("address"), QStringLiteral("object")}
        };
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            ProjectItem* item = resolveBindingHost(args);
            if (!item) return missingHost();
            const auto address = BindingSemantics::parseAddress(args);
            if (!address)
                return {false, {}, "INVALID_ARGS", "address is required"};
            auto* mix = BindingEditing::ledgerHost(item, *address);
            if (!mix) return {false, {}, "NO_COMPONENT", "No MixNode on binding host"};
            const BindingLedger oldLedger = mix->bindings;
            if (!BindingEditing::removeAll(
                    item, *address,
                    ActionManager::instance().executeOriginator()))
                return {false, {}, "NOT_FOUND", "No bindings at address"};
            return snapshotResult(item, oldLedger, undoTextOf(args, QStringLiteral("Clear Bindings")));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "reorder_bindings";
        def.displayName = "Reorder Bindings";
        def.category = "project";
        def.paramSchema = QJsonObject{
            {QStringLiteral("guid"), QStringLiteral("string")},
            {QStringLiteral("path"), QStringLiteral("string")},
            {QStringLiteral("object_id"), QStringLiteral("uint32")},
            {QStringLiteral("address"), QStringLiteral("object")},
            {QStringLiteral("from"), QStringLiteral("int")},
            {QStringLiteral("to"), QStringLiteral("int")}
        };
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            ProjectItem* item = resolveBindingHost(args);
            if (!item) return missingHost();
            const auto address = BindingSemantics::parseAddress(args);
            if (!address)
                return {false, {}, "INVALID_ARGS", "address is required"};
            auto* mix = BindingEditing::ledgerHost(item, *address);
            if (!mix) return {false, {}, "NO_COMPONENT", "No MixNode on binding host"};
            const int fromLocal = args.value(QStringLiteral("from")).toInt(-1);
            const int toLocal = args.value(QStringLiteral("to")).toInt(-1);
            const BindingLedger oldLedger = mix->bindings;
            if (!BindingEditing::reorder(
                    item, *address, fromLocal, toLocal,
                    ActionManager::instance().executeOriginator()))
                return {false, {}, "INVALID_ARGS", "Invalid binding reorder"};
            return snapshotResult(item, oldLedger, undoTextOf(args, QStringLiteral("Reorder Bindings")));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_modulator_binding";
        def.displayName = "Set Modulator Binding";
        def.category = "advanced";
        def.paramSchema = QJsonObject{
            {QStringLiteral("path"), QStringLiteral("string")},
            {QStringLiteral("modulators"), QStringLiteral("array")},
            {QStringLiteral("bindings"), QStringLiteral("array")}
        };
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            ProjectItem* item = resolveBindingHost(args);
            if (!item) return missingHost();
            const QJsonArray input = args.contains(QStringLiteral("bindings"))
                ? args.value(QStringLiteral("bindings")).toArray()
                : args.value(QStringLiteral("modulators")).toArray();
            return replaceLedger(
                item, input, undoTextOf(args, QStringLiteral("Set Modulator Binding")));
        };
        mgr.registerAction(def);
    }
}

}
