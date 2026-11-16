
#include "../CommandSchemas.h"
#include "../IPC/DesktopCommandSchemas.h"
#include "../ActionManager.h"
#include "../AudioEngine.h"
#include "AdvancedActionResult.h"

#include <QJsonObject>
#include "../ProjectManager.h"
#include "../ProjectTypes.h"
#include "../AuditionManager.h"
#include "../Components/VSelection.h"

#include <cmath>
#include <limits>

namespace VNX {


static ActionResult auditionNotAvailable()
{
    return {false, {}, "NOT_INIT", "AuditionManager not available"};
}

static bool parsePositiveUint32(const QJsonValue& value, uint32_t& parsed)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0.0
        || number > static_cast<double>(std::numeric_limits<uint32_t>::max())
        || std::trunc(number) != number) {
        return false;
    }
    parsed = static_cast<uint32_t>(number);
    return true;
}

static bool parseNonNegativeExactInt64(const QJsonValue& value,
                                       int64_t& parsed)
{
    constexpr double kMaximumExactJsonInteger = 9007199254740991.0;
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0
        || number > kMaximumExactJsonInteger || std::trunc(number) != number) {
        return false;
    }
    parsed = static_cast<int64_t>(number);
    return true;
}


void registerAuditionActions(ActionManager& mgr,
                              HarkenEditor::AuditionManager* auditionManager,
                              AudioEngine* engine)
{
    using namespace HarkenEditor;

    {
        ActionDef def;
        def.id = "audition_get_state";
        def.displayName = "Get Audition State";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject&) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            QJsonArray auditions;
            for (const auto& info : auditionManager->getActiveAuditions()) {
                QJsonObject entry = auditionManager->previewIpcSnapshot(info.playingId);
                entry.insert(QStringLiteral("auditionID"), static_cast<int>(info.playingId));
                entry.insert(QStringLiteral("objectName"), info.objectName);
                entry.insert(QStringLiteral("gameObjectId"), static_cast<double>(info.gameObjId));
                auditions.append(entry);
            }
            return {true, QJsonObject{{"auditions", auditions},
                {"activeCount", auditionManager->activeAuditionCount()}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_get_position";
        def.displayName = "Get Audition Position";
        def.category = "audition";
        def.paramSchema = QJsonObject{{"auditionID", "uint32"}};
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t id = 0;
            if (!parsePositiveUint32(args["auditionID"], id))
                return {false, {}, "INVALID_PARAM", "auditionID must be a positive uint32 integer"};
            const auto position = auditionManager->getPositionSamples(id);
            return {true, QJsonObject{
                {"samplePosition", static_cast<double>(position.samplePosition)},
                {"timeSeconds", position.timeSeconds},
                {"beat", static_cast<int>(position.beat)},
                {"bar", static_cast<int>(position.bar)},
                {"beatFraction", static_cast<double>(position.beatFraction)},
                {"isMusic", position.isMusic}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_get_speed";
        def.displayName = "Get Audition Speed";
        def.category = "audition";
        def.paramSchema = QJsonObject{{"auditionID", "uint32"}};
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t id = 0;
            if (!parsePositiveUint32(args["auditionID"], id))
                return {false, {}, "INVALID_PARAM", "auditionID must be a positive uint32 integer"};
            return {true, QJsonObject{{"auditionID", static_cast<int>(id)},
                {"speed", static_cast<double>(auditionManager->getPlaybackSpeed(id))}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_play";
        def.displayName = "Audition Play";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            QString objectPath = args["object_path"].toString();
            if (objectPath.isEmpty())
                return {false, {}, "INVALID_PARAM", "object_path required"};
            uint64_t gameObjId = static_cast<uint64_t>(args["game_object_id"].toDouble(0));
            uint32_t id = auditionManager->play(objectPath, gameObjId);
            QJsonObject data = auditionManager->previewIpcSnapshot(id);
            data.insert(QStringLiteral("auditionID"), static_cast<int>(id));
            data.insert(QStringLiteral("object_path"), objectPath);
            const bool pending = data.value(QStringLiteral("pending")).toBool();
            if (pending)
                data.insert(QStringLiteral("state"), QStringLiteral("queued"));
            else if (id != 0)
                data.insert(QStringLiteral("state"), QStringLiteral("playing"));
            else
                data.insert(QStringLiteral("state"), QStringLiteral("idle"));
            if (id == 0)
                return {false, data, "PLAY_FAILED",
                        "Failed to start playback for: " + objectPath};
            return {true, data};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_stop";
        def.displayName = "Audition Stop";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t tid = static_cast<uint32_t>(args["auditionID"].toInteger(0));
            float fadeMs = static_cast<float>(args["fade_ms"].toDouble(0));
            if (tid == 0) {
                auditionManager->stopAll(fadeMs);
            } else {
                auditionManager->stop(tid, fadeMs);
            }
            QJsonObject data = auditionManager->stopIpcSnapshot(tid);
            data.insert("auditionID", static_cast<qint64>(tid));
            data.insert("action", tid == 0 ? "stop_all" : "stop");
            data.insert("fade_ms", static_cast<double>(fadeMs));
            if (data.value("failed_count").toInt() != 0)
                return {false, data, "STOP_AUDITION_FAILED",
                        "The audition could not be stopped. It remains available for another stop request."};
            return {true, data};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_set_position";
        def.displayName = "Audition Set Position";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager, engine](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t tid = 0;
            if (!parsePositiveUint32(args["auditionID"], tid)) {
                return {false,
                        QJsonObject{{"state", "rejected"}},
                        "INVALID_PARAM",
                        "auditionID must be a positive uint32 integer"};
            }
            int64_t samplePos = 0;
            if (!parseNonNegativeExactInt64(args["samplePosition"], samplePos)) {
                return {false,
                        QJsonObject{{"auditionID", static_cast<int>(tid)},
                                    {"state", "rejected"}},
                        "INVALID_PARAM",
                        "samplePosition must be a non-negative exact integer"};
            }
            const auto result = auditionManager->setPositionSamples(tid, samplePos);
            const QString state = result.result
                    == HarkenEditor::Bridge::kEngineOk
                ? QStringLiteral("accepted")
                : QStringLiteral("rejected");
            auto response = AdvancedActionDetail::fromEngineResult(
                engine && engine->isInitialized(), result.result,
                QJsonObject{{"auditionID", static_cast<int>(tid)},
                            {"samplePosition", static_cast<double>(samplePos)},
                            {"audition_seek_request_id",
                             QString::number(result.requestId)},
                            {"state", state}},
                QStringLiteral("Set audition position"));
            if (response.success) {
                response.data.insert("async", true);
                response.data.insert("operation_id", QString::number(result.requestId));
                response.data.insert("operation", QJsonObject{
                    {"status_command", "audition_get_seek_status"},
                    {"status_args", QJsonObject{{"audition_seek_request_id", QString::number(result.requestId)}}}});
            }
            return response;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_get_seek_status";
        def.displayName = "Get Audition Seek Status";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            const QString id = args.value("audition_seek_request_id").toString();
            bool valid = false;
            const auto requestId = id.toULongLong(&valid);
            if (!valid || requestId == 0 || QString::number(requestId) != id)
                return {false, {}, "INVALID_PARAM", "audition_seek_request_id must be a positive decimal request ID"};
            const auto status = auditionManager->seekStatus(requestId);
            if (!status)
                return {false, {}, "NOT_FOUND", "Audition seek request is unknown or its completed result has expired"};
            QJsonObject progress{{"audition_seek_request_id", id}, {"in_progress", status->inProgress}};
            if (!status->inProgress) {
                QJsonObject completed{{"status", status->applied ? "ok" : "error"},
                    {"result", QJsonObject{
                        {"auditionID", static_cast<qint64>(status->playingId)},
                        {"audition_seek_request_id", id},
                        {"samplePosition", static_cast<qint64>(status->samplePosition)},
                        {"engine_result", static_cast<int32_t>(status->result)},
                        {"state", status->applied ? "applied" : "failed"}}}};
                if (!status->applied) {
                    completed.insert("code", "SEEK_FAILED");
                    completed.insert("message", "The audition seek was not applied.");
                }
                progress.insert("result", completed);
            }
            return {true, progress};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_set_speed";
        def.displayName = "Audition Set Speed";
        def.category = "audition";
        def.paramSchema = QJsonObject{{"auditionID", "uint32"}, {"speed", "float"}};
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t tid = static_cast<uint32_t>(args["auditionID"].toInteger(0));
            float speed = static_cast<float>(args["speed"].toDouble(1.0));
            if (tid == 0)
                return {false, {}, "INVALID_PARAM", "auditionID required"};
            auditionManager->setPlaybackSpeed(tid, speed);
            return {true, QJsonObject{{"auditionID", static_cast<int>(tid)}, {"speed", static_cast<double>(speed)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_close";
        def.displayName = "Audition Close";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t tid = static_cast<uint32_t>(args["auditionID"].toInteger(0));
            if (tid == 0)
                return {false, {}, "INVALID_PARAM", "auditionID required"};
            auditionManager->closeAudition(tid);
            return {true, QJsonObject{{"auditionID", static_cast<int>(tid)}, {"action", "closed"}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_reset";
        def.displayName = "Audition Reset";
        def.category = "audition";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = [auditionManager, engine](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            if (!engine) return {false, {}, "ENGINE_NOT_AVAILABLE", "AudioEngine is not available"};
            const qint64 handle = args[QStringLiteral("authoring_handle")].toInteger(0);
            if (handle <= 0)
                return {false, {}, "INVALID_PARAM", "authoring_handle required"};
            ProjectItem* item = ProjectManager::instance().findItemByObjectID(
                static_cast<uint32_t>(handle));
            if (!item || item->type != ProjectItem::Type::Container)
                return {false, {}, "INVALID_PARAM", "selected item is not a Container"};
            const auto* selection = item->comp<VSelection>();
            if (!selection || selection->mode == SelectionMode::Match || selection->mode == SelectionMode::Blend)
                return {false, {}, "INVALID_PARAM", "Container has no selection memory"};
            if (harkenstudio::IsNull(item->runtimeId()))
                return {false, {}, "NOT_READY", "Container has no runtime identity"};
            const int32_t status = auditionManager->resetPreviewContainer(
                item->runtimeId());
            if (HarkenEditor::AuditionManager::previewResetSucceeded(status))
                return {true, QJsonObject{{"status", status}}};
            if (HarkenEditor::AuditionManager::previewResetBackpressured(status))
                return {false, {}, "BACKPRESSURE", "Preview reset was backpressured"};
            return {false, {}, "RESET_FAILED", "Preview reset refused"};
        };
        mgr.registerAction(def);
    }
}

}
