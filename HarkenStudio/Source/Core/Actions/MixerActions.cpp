
#include "../ActionManager.h"
#include "../AudioEngine.h"
#include "../PlayablePropertyAuthoringV1.h"
#include "../ProjectManager.h"
#include "../NameValidator.h"
#include "../UndoCommands.h"
#include "../../MainWindow.h"
#include "../Components/VAudio.h"
#include "../Components/VMixNode.h"
#include "../Components/VBus.h"
#include "ActionSelectors.h"
#include "MixerActions.h"

#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <functional>
#include <optional>

namespace VNX {

QString unsupportedBusPropertiesKey(const QJsonObject& args)
{
    static const QSet<QString> supported{
        QStringLiteral("bus_id"), QStringLiteral("preview"), QStringLiteral("volume"),
        QStringLiteral("pitch"), QStringLiteral("pan"),
        QStringLiteral("makeUpGain"), QStringLiteral("makeup_gain"),
        QStringLiteral("mute"), QStringLiteral("solo"),
        QStringLiteral("outputBusVolume")};
    for (auto it = args.constBegin(); it != args.constEnd(); ++it)
        if (!supported.contains(it.key())) return it.key();
    return {};
}

ActionResult applyBusPropertiesArgs(const QJsonObject& args, const QObject* originator)
{
    auto& pm = ProjectManager::instance();
    if (!pm.currentProject())
        return {false, {}, "NO_PROJECT", "No project open"};
    const QString unsupportedKey = unsupportedBusPropertiesKey(args);
    if (!unsupportedKey.isEmpty())
        return {false, {}, "PROPERTY_NOT_SUPPORTED",
                QString("Bus property '%1' is not supported by set_bus_properties")
                    .arg(unsupportedKey)};

    uint32_t busId = static_cast<uint32_t>(args["bus_id"].toInt());
    auto* busItem = pm.findBusItem(busId);
    auto* bus = busItem ? busItem->comp<VBus>() : nullptr;
    auto* mix = busItem ? busItem->comp<VMixNode>() : nullptr;
    if (!bus || !mix)
        return {false, {}, "BUS_NOT_FOUND", QString("Bus ID %1 not found").arg(busId)};

    const bool hasVolume = args.contains("volume");
    const bool hasPitch  = args.contains("pitch");
    const bool hasPan    = args.contains("pan");
    const bool hasMakeUpGain = args.contains("makeUpGain") || args.contains("makeup_gain");
    const bool hasMute   = args.contains("mute");
    const bool hasSolo   = args.contains("solo");
    const bool hasObv    = args.contains("outputBusVolume");
    if (!hasVolume && !hasPitch && !hasPan && !hasMakeUpGain
        && !hasMute && !hasSolo && !hasObv)
        return {false, {}, "INVALID_ARGS",
                "At least one of volume/pitch/pan/makeUpGain/mute/solo/outputBusVolume is required"};

    const float oldPan = bus->pan;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*busItem);
    auto snapshot = [&](const QString& propertyName)
        -> std::optional<PlayablePropertyStateV1> {
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
        if (!locator || !field) return std::nullopt;
        return PlayablePropertyAuthoringV1::readProperty(
            *busItem, *locator, field->id, std::nullopt);
    };
    const auto oldVolume = hasVolume ? snapshot(QStringLiteral("volume")) : std::nullopt;
    const auto oldPitch = hasPitch ? snapshot(QStringLiteral("pitch")) : std::nullopt;
    const auto oldMakeUpGain = hasMakeUpGain
        ? snapshot(QStringLiteral("makeUpGain")) : std::nullopt;
    const auto oldObv = hasObv
        ? snapshot(QStringLiteral("outputBusVolume")) : std::nullopt;

    const float newVolume = hasVolume ? static_cast<float>(args["volume"].toDouble()) : 0.0f;
    const float newPitch = hasPitch ? static_cast<float>(args["pitch"].toDouble()) : 0.0f;
    const float newPan = hasPan ? static_cast<float>(args["pan"].toDouble()) : oldPan;
    const QJsonValue makeUpGainValue = args.contains("makeUpGain")
        ? args["makeUpGain"] : args["makeup_gain"];
    const float newMakeUpGain = hasMakeUpGain
        ? static_cast<float>(makeUpGainValue.toDouble()) : 0.0f;
    const float newObv = hasObv ? static_cast<float>(args["outputBusVolume"].toDouble()) : 0.0f;

    if (hasVolume) pm.setBusVolumeInternal(busId, newVolume, originator);
    if (hasPitch) pm.setBusPitchInternal(busId, newPitch, originator);
    if (hasPan) pm.setBusPanInternal(busId, newPan, originator);
    if (hasMakeUpGain)
        pm.setBusMakeUpGainInternal(busId, newMakeUpGain, originator);
    if (hasMute) pm.setBusMuteInternal(busId, args["mute"].toBool(), originator);
    if (hasSolo) pm.setBusSoloInternal(busId, args["solo"].toBool(), originator);
    if (hasObv) pm.setBusOutputBusVolumeInternal(busId, newObv, originator);

    ActionResult r;
    r.success = true;
    r.data = QJsonObject{
        {"bus_id", static_cast<qint64>(busId)},
        {"message", "Properties updated"}
    };

    const bool hasUndoableProp = hasVolume || hasPitch || hasPan
        || hasMakeUpGain || hasObv;
    if (!hasUndoableProp)
        return r;

    int changedCount = (hasVolume ? 1 : 0) + (hasPitch ? 1 : 0)
        + (hasPan ? 1 : 0) + (hasMakeUpGain ? 1 : 0) + (hasObv ? 1 : 0);
    QString propKey;
    if (changedCount == 1) {
        if (hasVolume) propKey = "volume";
        else if (hasPitch) propKey = "pitch";
        else if (hasPan) propKey = "pan";
        else if (hasMakeUpGain) propKey = "makeUpGain";
        else if (hasObv) propKey = "outputBusVolume";
    }

    const QUuid busGuid = busItem->guid;
    auto restorePlayable = [busGuid](const QString& propertyName,
                                     const std::optional<PlayablePropertyStateV1>& state) {
        ProjectItem* item = ProjectManager::instance().findItemByGuid(busGuid);
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(propertyName);
        const auto locator = item
            ? PlayablePropertyAuthoringV1::definitionLocator(*item) : std::nullopt;
        if (!item || !field || !locator || !state) return false;
        return PlayablePropertyAuthoringV1::restoreProperty(
            *item, *locator, field->id, *state, std::nullopt);
    };

    auto undoFn = [busId, busGuid,
                   hasVolume, hasPitch, hasPan, hasMakeUpGain, hasObv,
                   oldVolume, oldPitch, oldPan, oldMakeUpGain, oldObv,
                   restorePlayable]() {
        auto& pmm = ProjectManager::instance();
        auto live = [&](const QString& propertyName) {
            if (ProjectItem* item = pmm.findItemByGuid(busGuid)) {
                pmm.pushBusPropertyLive(item, propertyName);
                emit pmm.busChanged(item->objectID, propertyName);
            }
        };
        if (hasVolume) {
            if (!restorePlayable(QStringLiteral("volume"), oldVolume))
                pmm.setBusVolumeInternal(busId, 0.0f);
            else
                live(QStringLiteral("volume"));
        }
        if (hasPitch) {
            if (!restorePlayable(QStringLiteral("pitch"), oldPitch))
                pmm.setBusPitchInternal(busId, 0.0f);
            else
                live(QStringLiteral("pitch"));
        }
        if (hasPan) pmm.setBusPanInternal(busId, oldPan);
        if (hasMakeUpGain) {
            if (!restorePlayable(QStringLiteral("makeUpGain"), oldMakeUpGain))
                pmm.setBusMakeUpGainInternal(busId, 0.0f);
            else
                live(QStringLiteral("makeUpGain"));
        }
        if (hasObv) {
            if (!restorePlayable(QStringLiteral("outputBusVolume"), oldObv))
                pmm.setBusOutputBusVolumeInternal(busId, 0.0f);
            else
                live(QStringLiteral("outputBusVolume"));
        }
    };
    auto redoFn = [busId,
                   hasVolume, hasPitch, hasPan, hasMakeUpGain, hasObv,
                   newVolume, newPitch, newPan, newMakeUpGain, newObv]() {
        auto& pmm = ProjectManager::instance();
        if (hasVolume) pmm.setBusVolumeInternal(busId, newVolume);
        if (hasPitch) pmm.setBusPitchInternal(busId, newPitch);
        if (hasPan) pmm.setBusPanInternal(busId, newPan);
        if (hasMakeUpGain)
            pmm.setBusMakeUpGainInternal(busId, newMakeUpGain);
        if (hasObv) pmm.setBusOutputBusVolumeInternal(busId, newObv);
    };

    r.undoFn = std::move(undoFn);
    r.redoFn = std::move(redoFn);
    r.undoText = changedCount == 1
        ? QObject::tr("Set Bus %1").arg(propKey)
        : QObject::tr("Set Bus Properties");
    r.authoringWriteSet = QSet<QUuid>{busGuid};
    return r;
}

void registerMixerActions(ActionManager& mgr,
                          AudioEngine* engine,
                          ProjectManager* projectManager,
                          MainWindow* mainWindow)
{

    {
        ActionDef def;
        def.id = "set_mixer_volume";
        def.displayName = "Set Mixer Volume";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus", "string"}, {"volume_db", "float"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(), args["bus"].toString(), ProjectItem::Type::Bus);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            float volumeDb = args["volume_db"].toDouble(0.0);
            const uint32_t busId = selected.item->objectID;

            ProjectManager::instance().setBusVolumeInternal(busId, volumeDb);

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"bus", selected.name}, {"bus_guid", selected.item->guid.toString(QUuid::WithoutBraces)},
                                 {"volume_db", volumeDb}, {"success", true}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_mixer_mute";
        def.displayName = "Set Mixer Mute";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus", "string"}, {"mute", "bool"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(), args["bus"].toString(), ProjectItem::Type::Bus);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            bool mute = args["mute"].toBool();
            const uint32_t busId = selected.item->objectID;

            ProjectManager::instance().setBusMuteInternal(busId, mute);

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"bus", selected.name}, {"bus_guid", selected.item->guid.toString(QUuid::WithoutBraces)},
                                 {"mute", mute}, {"success", true}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_mixer_solo";
        def.displayName = "Set Mixer Solo";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus", "string"}, {"solo", "bool"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(), args["bus"].toString(), ProjectItem::Type::Bus);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            bool solo = args["solo"].toBool();
            const uint32_t busId = selected.item->objectID;

            ProjectManager::instance().setBusSoloInternal(busId, solo);

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"bus", selected.name}, {"bus_guid", selected.item->guid.toString(QUuid::WithoutBraces)},
                                 {"solo", solo}, {"success", true}};
            return r;
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "create_bus";
        def.displayName = "Create Bus";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"name", "string"}, {"parent_bus_id", "uint32"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            auto& pm = ProjectManager::instance();
            if (!pm.currentProject())
                return {false, {}, "NO_PROJECT", "No project open"};

            QString name = args["name"].toString("New Bus");
            uint32_t parentBusId = static_cast<uint32_t>(args["parent_bus_id"].toInt(
                static_cast<int>(VNX::kMasterBusID)));

            ProjectItem* parent = pm.findBusItem(parentBusId);
            if (!parent)
                return {false, {}, "CREATE_FAILED",
                        QString("Parent bus ID %1 not found").arg(parentBusId)};

            // Surface the specific validation reason (e.g. ASCII-only) in the
            // result so an AI caller can correct the name; createItem otherwise
            // reports it only via errorOccurred, which never reaches the caller.
            NameValidator::Result vr;
            if (!NameValidator::validateItemName(name, ProjectItem::Type::Bus, &vr))
                return {false, {}, "INVALID_NAME", NameValidator::resultMessage(vr)};

            ProjectItem* busItem = pm.createItem(parent, name, ProjectItem::Type::Bus);
            if (!busItem)
                return {false, {}, "CREATE_FAILED",
                        QString("Failed to create bus '%1'").arg(name)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"bus_id", static_cast<qint64>(busItem->objectID)},
                {"name", busItem->name},
                {"parent_bus_id", static_cast<qint64>(parentBusId)}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "delete_bus";
        def.displayName = "Delete Bus";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_id", "uint32"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            auto& pm = ProjectManager::instance();
            if (!pm.currentProject())
                return {false, {}, "NO_PROJECT", "No project open"};

            uint32_t busId = static_cast<uint32_t>(args["bus_id"].toInt());
            ProjectItem* busItem = pm.findBusItem(busId);
            if (!busItem)
                return {false, {}, "DELETE_FAILED", QString("Bus ID %1 not found").arg(busId)};

            if (!pm.deleteItem(busItem))
                return {false, {}, "DELETE_FAILED", QString("Failed to delete bus ID %1").arg(busId)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"bus_id", static_cast<qint64>(busId)}, {"message", "Bus deleted"}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "move_bus";
        def.displayName = "Move Bus";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_id", "uint32"}, {"new_parent_bus_id", "uint32"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            auto& pm = ProjectManager::instance();
            if (!pm.currentProject())
                return {false, {}, "NO_PROJECT", "No project open"};

            uint32_t busId = static_cast<uint32_t>(args["bus_id"].toInt());
            uint32_t newParentId = static_cast<uint32_t>(args["new_parent_bus_id"].toInt());

            auto* busItem = pm.findBusItem(busId);
            if (!busItem) return {false, {}, "BUS_NOT_FOUND", QString("Bus ID %1 not found").arg(busId)};
            auto* newParent = pm.findBusItem(newParentId);
            if (!newParent) return {false, {}, "BUS_NOT_FOUND", QString("Parent bus ID %1 not found").arg(newParentId)};

            if (!pm.moveItem(busItem, newParent))
                return {false, {}, "MOVE_FAILED", QString("Failed to move bus ID %1").arg(busId)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"bus_id", static_cast<qint64>(busId)},
                {"new_parent_bus_id", static_cast<qint64>(newParentId)}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "rename_bus";
        def.displayName = "Rename Bus";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_id", "uint32"}, {"new_name", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            auto& pm = ProjectManager::instance();
            if (!pm.currentProject())
                return {false, {}, "NO_PROJECT", "No project open"};

            uint32_t busId = static_cast<uint32_t>(args["bus_id"].toInt());
            QString newName = args["new_name"].toString();

            auto* busItem = pm.findBusItem(busId);
            if (!busItem) return {false, {}, "BUS_NOT_FOUND", QString("Bus ID %1 not found").arg(busId)};

            // Surface the specific validation reason (e.g. ASCII-only) so an AI
            // caller can correct the name instead of only seeing a generic
            // rename failure.
            NameValidator::Result vr;
            if (!NameValidator::validateItemName(newName, ProjectItem::Type::Bus, &vr))
                return {false, {}, "INVALID_NAME", NameValidator::resultMessage(vr)};

            if (!pm.renameItem(busItem, newName))
                return {false, {}, "RENAME_FAILED",
                        QString("Failed to rename bus ID %1 to '%2'").arg(busId).arg(newName)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"bus_id", static_cast<qint64>(busId)}, {"new_name", busItem->name}};
            return r;
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_send";
        def.displayName = "Set Send";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"src_bus", "string"}, {"target_bus_id", "uint32"}, {"level", "float"}};
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(), args.value("src_bus").toString());
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            const QString srcBusName = selected.name;
            if (!args.contains("target_bus_id"))
                return {false, {}, "INVALID_ARGS", "target_bus_id is required"};

            auto* srcBusItem = selected.item;
            if (!srcBusItem->comp<VMixNode>())
                return {false, {}, "BUS_NOT_FOUND", QString("Send source '%1' not found").arg(srcBusName)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(srcBusItem->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Send"};

            const uint32_t targetBusId = static_cast<uint32_t>(args.value("target_bus_id").toInteger());
            auto* targetBus = ProjectManager::instance().findVBus(targetBusId);
            if (!targetBus)
                return {false, {}, "BUS_NOT_FOUND", QString("Send target bus %1 not found").arg(targetBusId)};
            if (srcBusItem->objectID == targetBusId)
                return {false, {}, "INVALID_ARGS", "src_bus and target_bus_id cannot be the same bus"};

            const float level = static_cast<float>(args.value("level").toDouble(1.0));
            const uint32_t srcBusId = srcBusItem->objectID;

            bool existingSend = false;
            if (const auto* srcMix = srcBusItem->comp<VMixNode>())
                for (const auto& send : srcMix->sends)
                    if (send.targetBusID == targetBusId) { existingSend = true; break; }
            if (!existingSend && !ProjectManager::instance().sendTargetSelectable(srcBusId, targetBusId))
                return {false, {}, "SEND_REJECTED",
                        QString("Send %1 -> %2 rejected: invalid target, send loop, or main output conflict")
                            .arg(srcBusName).arg(targetBusId)};

            if (!ProjectManager::instance().upsertSendInternal(srcBusId, targetBusId, level))
                return {false, {}, "SEND_CYCLE",
                        QString("Send %1 -> %2 rejected: it would close a send loop")
                            .arg(srcBusName).arg(targetBusId)};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"src_bus", srcBusName},
                {"src_bus_guid", srcBusItem->guid.toString(QUuid::WithoutBraces)},
                {"src_bus_id", static_cast<qint64>(srcBusId)},
                {"target_bus_id", static_cast<qint64>(targetBusId)},
                {"level", level}
            };
            r.undoFn = [srcBusId, targetBusId]() {
                ProjectManager::instance().removeSendInternal(srcBusId, targetBusId);
            };
            r.redoFn = [srcBusId, targetBusId, level]() {
                ProjectManager::instance().upsertSendInternal(srcBusId, targetBusId, level);
            };
            r.undoText = QObject::tr("Add Send");
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "remove_send";
        def.displayName = "Remove Send";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"src_bus", "string"}, {"target_bus_id", "uint32"}};
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(), args.value("src_bus").toString());
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            const QString srcBusName = selected.name;
            if (!args.contains("target_bus_id"))
                return {false, {}, "INVALID_ARGS", "target_bus_id is required"};

            auto* srcBusItem = selected.item;
            auto* srcMix = srcBusItem->comp<VMixNode>();
            if (!srcMix)
                return {false, {}, "BUS_NOT_FOUND", QString("Send source '%1' not found").arg(srcBusName)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(srcBusItem->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Send"};

            const uint32_t targetBusId = static_cast<uint32_t>(args.value("target_bus_id").toInteger());

            float oldLevel = 1.0f;
            bool foundSend = false;
            for (const auto& send : srcMix->sends) {
                if (send.targetBusID == targetBusId) {
                    foundSend = true;
                    oldLevel = send.sendLevel;
                    break;
                }
            }
            if (!foundSend)
                return {false, {}, "SEND_NOT_FOUND",
                        QString("Send %1 -> %2 not found").arg(srcBusName).arg(targetBusId)};

            const uint32_t srcBusId = srcBusItem->objectID;
            ProjectManager::instance().removeSendInternal(srcBusId, targetBusId);

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"src_bus", srcBusName},
                {"src_bus_guid", srcBusItem->guid.toString(QUuid::WithoutBraces)},
                {"src_bus_id", static_cast<qint64>(srcBusId)},
                {"target_bus_id", static_cast<qint64>(targetBusId)},
                {"removed", true}
            };
            r.undoFn = [srcBusId, targetBusId, oldLevel]() {
                ProjectManager::instance().upsertSendInternal(srcBusId, targetBusId, oldLevel);
            };
            r.redoFn = [srcBusId, targetBusId]() {
                ProjectManager::instance().removeSendInternal(srcBusId, targetBusId);
            };
            r.undoText = QObject::tr("Remove Send");
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_send_level";
        def.displayName = "Set Send Level";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"src_bus", "string"}, {"target_bus_id", "uint32"}, {"level", "float"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(), args.value("src_bus").toString());
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            const QString srcBusName = selected.name;
            if (!args.contains("target_bus_id"))
                return {false, {}, "INVALID_ARGS", "target_bus_id is required"};

            auto* srcBusItem = selected.item;
            auto* srcMix = srcBusItem->comp<VMixNode>();
            if (!srcMix)
                return {false, {}, "BUS_NOT_FOUND", QString("Send source '%1' not found").arg(srcBusName)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(srcBusItem->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Send"};

            const uint32_t targetBusId = static_cast<uint32_t>(args.value("target_bus_id").toInteger());
            const float level = static_cast<float>(args.value("level").toDouble(1.0));

            float oldLevel = 1.0f;
            bool foundSend = false;
            for (const auto& send : srcMix->sends) {
                if (send.targetBusID == targetBusId) {
                    foundSend = true;
                    oldLevel = send.sendLevel;
                    break;
                }
            }
            if (!foundSend)
                return {false, {}, "SEND_NOT_FOUND",
                        QString("Send %1 -> %2 not found").arg(srcBusName).arg(targetBusId)};

            const uint32_t srcBusId = srcBusItem->objectID;
            ProjectManager::instance().upsertSendInternal(srcBusId, targetBusId, level);

            UndoRedoManager::instance().push(
                new BusPropertyChangeCommand(srcBusId,
                    QString("sendLevel_%1").arg(targetBusId),
                    static_cast<double>(oldLevel), static_cast<double>(level),
                    [targetBusId](uint32_t bID, const QVariant& val) {
                        float lvl = val.toFloat();
                        ProjectManager::instance().upsertSendInternal(bID, targetBusId, lvl);
                    },
                    QObject::tr("Set Send Level")));

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"src_bus", srcBusName},
                {"src_bus_guid", srcBusItem->guid.toString(QUuid::WithoutBraces)},
                {"src_bus_id", static_cast<qint64>(srcBusId)},
                {"target_bus_id", static_cast<qint64>(targetBusId)},
                {"level", level}
            };
            return r;
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_bus_properties";
        def.displayName = "Set Bus Properties";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_id", "uint32"}, {"volume", "float"}, {"pitch", "float"}, {"pan", "float"}, {"mute", "bool"}, {"solo", "bool"}, {"outputBusVolume", "float"}};
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            return applyBusPropertiesArgs(args, ActionManager::instance().executeOriginator());
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_bus_pitch";
        def.displayName = "Set Bus Pitch";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_name", "string"}, {"cents", "float"}};
        def.undoable = true;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            auto& pm = ProjectManager::instance();
            if (!pm.currentProject())
                return {false, {}, "NO_PROJECT", "No project open"};
            const QJsonValue centsValue = args.value("cents");
            const float cents = static_cast<float>(centsValue.toDouble());
            if (!centsValue.isDouble() || !std::isfinite(cents))
                return {false, {}, "INVALID_ARGS",
                        "cents is required and must be a finite number"};
            const auto selected = resolveProjectSelector(
                pm.currentProject(), args["bus_name"].toString(), ProjectItem::Type::Bus);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* busItem = selected.item;
            QJsonObject busArgs;
            busArgs.insert("bus_id", static_cast<qint64>(busItem->objectID));
            busArgs.insert("pitch", cents);
            ActionResult r = applyBusPropertiesArgs(
                busArgs, ActionManager::instance().executeOriginator());
            if (r.success)
                r.data = QJsonObject{{"bus", busItem->name}, {"bus_guid", busItem->guid.toString(QUuid::WithoutBraces)},
                                     {"cents", cents}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_bus_volume_db";
        def.displayName = "Set Bus Volume (dB)";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_name", "string"}, {"db", "float"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            auto& pm = ProjectManager::instance();
            if (!pm.currentProject())
                return {false, {}, "NO_PROJECT", "No project open"};
            const auto selected = resolveProjectSelector(
                pm.currentProject(), args["bus_name"].toString(), ProjectItem::Type::Bus);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* busItem = selected.item;
            float db = static_cast<float>(args["db"].toDouble());
            pm.setBusVolumeInternal(busItem->objectID, db);
            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"bus", busItem->name}, {"bus_guid", busItem->guid.toString(QUuid::WithoutBraces)},
                                 {"bus_volume_db", db}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_send_filter";
        def.displayName = "Set Send Filter";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"src_bus", "string"}, {"slot", "int"}, {"lpf_hz", "float"}, {"hpf_hz", "float"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            const QString srcBus = args.value("src_bus").toString().trimmed();
            if (srcBus.isEmpty())
                return {false, {}, "INVALID_ARGS", "src_bus is required"};
            const auto selected = resolveProjectSelector(
                ProjectManager::instance().currentProject(),
                srcBus, ProjectItem::Type::Bus);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            if (!args.contains("slot"))
                return {false, {}, "INVALID_ARGS", "slot (0-3) is required"};

            const int32_t slot = args.value("slot").toInt(-1);
            if (slot < 0 || slot > 3)
                return {false, {}, "INVALID_ARGS", "slot must be 0-3"};

            for (const auto& field : {"lpf_hz", "hpf_hz"}) {
                const QJsonValue value = args.value(QLatin1String(field));
                if (!value.isUndefined() && (!value.isDouble()
                    || !std::isfinite(value.toDouble()) || value.toDouble() <= 0.0))
                    return {false, {}, "INVALID_ARGS",
                        QString("%1 must be a positive finite number").arg(QLatin1String(field))};
            }
            return {false, {}, "PROPERTY_NOT_SUPPORTED",
                    QString("Send Filter on Bus '%1' (%2), slot %3 is not supported")
                        .arg(selected.name,
                             selected.item->guid.toString(QUuid::WithoutBraces))
                        .arg(slot)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "select_bus";
        def.displayName = "Select Bus";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"bus_id", "uint32"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!args.contains("bus_id"))
                return {false, {}, "INVALID_ARGS", "bus_id required"};
            uint32_t busId = static_cast<uint32_t>(args["bus_id"].toInteger());

            auto& pm = ProjectManager::instance();
            if (pm.currentProject()) {
                if (pm.findVBus(busId) == nullptr)
                    return {false, {}, "NOT_FOUND", QString("Bus ID %1 not found").arg(busId)};
            }

            if (mainWindow) {
                QMetaObject::invokeMethod(mainWindow, [mainWindow, busId]() {
                    mainWindow->selectBus(busId, true);
                }, Qt::QueuedConnection);
            }

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"bus_id", static_cast<qint64>(busId)},
                {"selected", true}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_output_bus_routing";
        def.displayName = "Set Main Output Routing";
        def.category = "mixer";
        def.paramSchema = QJsonObject{{"path", "string"}, {"volume_db", "float"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            if (!projectManager || !projectManager->isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};

            const QString path = args.value("path").toString();
            if (path.isEmpty())
                return {false, {}, "INVALID_ARGS", "path is required (sound object path)"};

            ProjectData* project = projectManager->currentProject();
            const auto selected = resolveProjectSelector(project, path);
            if (!selected.ok) return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            ProjectItem* item = selected.item;
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Main Output"};

            auto* mcomp = item->comp<VMixNode>();
            if (!mcomp)
                return {false, {}, "NO_AUDIO_COMPONENT", "Item has no VMixNode"};

            bool changed = false;
            if (args.contains("volume_db")) {
                if (!projectManager->writeBusPropertySilent(
                        item, QStringLiteral("outputBusVolume"),
                        args.value("volume_db").toDouble()))
                    return {false, {}, "SET_PROPERTY_FAILED",
                            "Failed to write output bus volume"};
                changed = true;
            }

            if (changed) {
                item->markDirty();
                ProjectManager::resyncItemToRuntime(item);
            }

            QJsonObject result;
            result["path"] = path;
            result["volume_db"] = mcomp->outputBusVolume;
            ActionResult r;
            r.success = true;
            r.data = result;
            return r;
        };
        mgr.registerAction(def);
    }
}


}
