
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
        def.id = "audition_play";
        def.displayName = "Audition Play";
        def.category = "audition";
        def.paramSchema = QJsonObject{{"object_path", "string"}, {"game_object_id", "uint64"}};
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
        def.paramSchema = QJsonObject{{"auditionID", "uint32"}, {"fade_ms", "float"}};
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t tid = static_cast<uint32_t>(args["auditionID"].toInt(0));
            float fadeMs = static_cast<float>(args["fade_ms"].toDouble(0));
            if (tid == 0) {
                auditionManager->stopAll(fadeMs);
                return {true, QJsonObject{{"action", "stop_all"}, {"fade_ms", static_cast<double>(fadeMs)}}};
            }
            auditionManager->stop(tid, fadeMs);
            return {true, QJsonObject{{"auditionID", static_cast<int>(tid)}, {"action", "stopped"}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "audition_set_position";
        def.displayName = "Audition Set Position";
        def.category = "audition";
        def.paramSchema = QJsonObject{{"auditionID", "uint32"}, {"samplePosition", "int64"}};
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
            return AdvancedActionDetail::fromEngineResult(
                engine && engine->isInitialized(), result.result,
                QJsonObject{{"auditionID", static_cast<int>(tid)},
                            {"samplePosition", static_cast<double>(samplePos)},
                            {"audition_seek_request_id",
                             QString::number(result.requestId)},
                            {"state", state}},
                QStringLiteral("Set audition position"));
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
            uint32_t tid = static_cast<uint32_t>(args["auditionID"].toInt(0));
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
        def.paramSchema = QJsonObject{{"auditionID", "uint32"}};
        def.undoable = false;
        def.handler = [auditionManager](const QJsonObject& args) -> ActionResult {
            if (!auditionManager) return auditionNotAvailable();
            uint32_t tid = static_cast<uint32_t>(args["auditionID"].toInt(0));
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
        def.paramSchema = QJsonObject{{"authoring_handle", "int64"}};
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
