
#include "../ActionManager.h"
#include "../AudioEngine.h"
#include "../ProjectManager.h"
#include "../HarkenMath.h"
#include "AdvancedActionResult.h"
#include "ActionSelectors.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace VNX { ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path); }

namespace VNX {


static ActionResult engineNotAvailable()
{
    return {false, {}, "ENGINE_NOT_AVAILABLE", "AudioEngine not available"};
}

static QJsonValue exactUint64Json(uint64_t value)
{
    constexpr uint64_t kMaximumExactJsonInteger = (uint64_t{1} << 53) - 1;
    if (value <= kMaximumExactJsonInteger)
        return QJsonValue(static_cast<qint64>(value));
    return QJsonValue(QString::number(value));
}

static bool parseExactPositiveUint64(const QJsonValue& value, uint64_t& parsed)
{
    if (value.isString()) {
        bool ok = false;
        const qulonglong id = value.toString().toULongLong(&ok, 10);
        if (!ok || id == 0)
            return false;
        parsed = static_cast<uint64_t>(id);
        return true;
    }

    constexpr double kMaximumExactJsonInteger = 9007199254740991.0;
    const double number = value.toDouble(0.0);
    if (!std::isfinite(number) || number <= 0.0
        || number > kMaximumExactJsonInteger || std::trunc(number) != number) {
        return false;
    }
    parsed = static_cast<uint64_t>(number);
    return true;
}

static bool parseOptionalBool(const QJsonObject& args, const char* name,
                              bool& parsed)
{
    const QString key = QString::fromLatin1(name);
    if (!args.contains(key)) {
        parsed = false;
        return true;
    }
    const QJsonValue value = args.value(key);
    if (!value.isBool())
        return false;
    parsed = value.toBool();
    return true;
}

static QString seekStateName(HarkenEditor::Bridge::ActionSeekState state)
{
    switch (state) {
    case HarkenEditor::Bridge::ActionSeekState::Accepted:
        return QStringLiteral("accepted");
    case HarkenEditor::Bridge::ActionSeekState::Applied:
        return QStringLiteral("applied");
    case HarkenEditor::Bridge::ActionSeekState::Cancelled:
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("accepted");
}

static QJsonObject seekInfoJson(
    uint64_t operationId, const HarkenEditor::Bridge::ActionSeekInfo& info)
{
    return {
        {QStringLiteral("seek_operation_id"), exactUint64Json(operationId)},
        {QStringLiteral("action_handle"), exactUint64Json(info.playbackHandle)},
        {QStringLiteral("action"), info.actionName},
        {QStringLiteral("state"), seekStateName(info.state)},
        {QStringLiteral("engine_result"),
         static_cast<int32_t>(info.result)},
        {QStringLiteral("service_result"),
         static_cast<int32_t>(info.serviceResult)},
        {QStringLiteral("native_release_result"),
         static_cast<int32_t>(info.nativeReleaseResult)},
        {QStringLiteral("native_released"), info.nativeReleased},
        {QStringLiteral("target_count"), static_cast<int>(info.targetCount)},
        {QStringLiteral("applied_count"), static_cast<int>(info.appliedCount)},
        {QStringLiteral("cancelled_count"), static_cast<int>(info.cancelledCount)},
        {QStringLiteral("accepted_logic_sequence"),
         exactUint64Json(info.acceptedLogicSequence)},
        {QStringLiteral("applied_audio_block_sequence"),
         exactUint64Json(info.appliedAudioBlockSequence)},
        {QStringLiteral("minimum_applied_source_frame"),
         exactUint64Json(info.minimumAppliedSourceFrame)},
        {QStringLiteral("maximum_applied_source_frame"),
         exactUint64Json(info.maximumAppliedSourceFrame)},
    };
}

ActionResult AdvancedActionDetail::fromEngineResult(
    bool engineInitialized, HarkenEditor::Bridge::EngineResult result,
    QJsonObject successData, const QString& operation)
{
    const int32_t signedResult = static_cast<int32_t>(result);
    successData.insert(QStringLiteral("engine_result"), signedResult);
    if (result == HarkenEditor::Bridge::kEngineOk)
        return {true, successData};

    QString errorCode = QStringLiteral("ENGINE_ERROR");
    QString errorMessage = QStringLiteral("%1 failed (engine result %2)")
                               .arg(operation)
                               .arg(signedResult);
    if (result == HarkenEditor::Bridge::kEngineNotInit) {
        if (engineInitialized) {
            errorCode = QStringLiteral("NOT_AVAILABLE");
            errorMessage = QStringLiteral("%1 is not available in the current engine path")
                               .arg(operation);
        } else {
            errorCode = QStringLiteral("NOT_INIT");
            errorMessage = QStringLiteral("Audio engine not initialized");
        }
    } else if (result == HarkenEditor::Bridge::kEngineInvalidArg) {
        errorCode = QStringLiteral("INVALID_PARAM");
    } else if (result == HarkenEditor::Bridge::kEngineUnknownId) {
        errorCode = QStringLiteral("UNKNOWN_ID");
    } else if (result == HarkenEditor::Bridge::kEngineStaleHandle) {
        errorCode = QStringLiteral("STALE_HANDLE");
    } else if (result == HarkenEditor::Bridge::kEngineNoVoice) {
        errorCode = QStringLiteral("NO_VOICE");
    } else if (result == HarkenEditor::Bridge::kEngineBackpressure) {
        errorCode = QStringLiteral("BACKPRESSURE");
    } else if (result == HarkenEditor::Bridge::kEngineInUse) {
        errorCode = QStringLiteral("IN_USE");
    } else if (result == HarkenEditor::Bridge::kEngineSendCycle) {
        errorCode = QStringLiteral("SEND_CYCLE");
    } else if (result == HarkenEditor::Bridge::kEngineCapabilityUnavailable) {
        errorCode = QStringLiteral("NOT_AVAILABLE");
    } else if (result == HarkenEditor::Bridge::kEngineRuntimeRecreateRequired) {
        errorCode = QStringLiteral("RUNTIME_RECREATE_REQUIRED");
    } else if (result == HarkenEditor::Bridge::kEngineDeviceError) {
        errorCode = QStringLiteral("DEVICE_ERROR");
    }
    return {false, successData, errorCode, errorMessage};
}

void registerAdvancedActions(ActionManager& mgr,
                             AudioEngine* engine)
{

    {
        ActionDef def;
        def.id = "set_master_volume";
        def.displayName = "Set Master Volume";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"volume", "float"}, {"fade_ms", "int"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            float volume = args["volume"].toDouble(1.0);
            int fadeMs = args["fade_ms"].toInt(0);
            engine->setMasterVolume(volume, fadeMs);
            return {true, QJsonObject{{"volume", volume}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_game_object_volume";
        def.displayName = "Set Game Object Volume";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"volume", "float"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t gameObjId = 0;
            if (!parseExactPositiveUint64(args["game_object_id"], gameObjId))
                return {false, {}, "INVALID_PARAM", "game_object_id must be a positive exact uint64 number or decimal string"};
            float volume = static_cast<float>(args["volume"].toDouble(1.0));
            const auto result = engine->setGameObjectVolume(gameObjId, volume);
            return AdvancedActionDetail::fromEngineResult(
                engine->isInitialized(), result,
                QJsonObject{{"game_object_id", exactUint64Json(gameObjId)},
                            {"volume", volume}},
                QStringLiteral("Set game object volume"));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_game_object_mute";
        def.displayName = "Set Game Object Mute";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"mute", "bool"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t gameObjId = 0;
            if (!parseExactPositiveUint64(args["game_object_id"], gameObjId))
                return {false, {}, "INVALID_PARAM", "game_object_id must be a positive exact uint64 number or decimal string"};
            bool mute = args["mute"].toBool(false);
            const auto result = engine->setGameObjectMute(gameObjId, mute);
            return AdvancedActionDetail::fromEngineResult(
                engine->isInitialized(), result,
                QJsonObject{{"game_object_id", exactUint64Json(gameObjId)},
                            {"mute", mute}},
                QStringLiteral("Set game object mute"));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "seek_action";
        def.displayName = "Seek Action";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"action_handle", "uint64"}, {"name", "string"}, {"position", "float"}, {"is_relative", "bool"}, {"snap_to_marker", "bool"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            bool snapToMarker = false;
            if (!parseOptionalBool(args, "snap_to_marker", snapToMarker))
                return {false, {}, "INVALID_PARAM", "snap_to_marker must be a boolean"};
            if (snapToMarker) {
                return {false, {}, "UNSUPPORTED", "snap_to_marker is not supported: Action seek is time-based only (audio sources carry no marker data)"};
            }
            uint64_t actionHandle = 0;
            if (!parseExactPositiveUint64(args["action_handle"], actionHandle))
                return {false, {}, "INVALID_PARAM", "action_handle must be a positive exact uint64 number or decimal string"};
            QString name = args["name"].toString();
            if (!name.isEmpty()) {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            const QJsonValue positionValue = args["position"];
            if (!positionValue.isDouble())
                return {false, {}, "INVALID_PARAM", "position must be a finite number"};
            const double pos = positionValue.toDouble();
            if (!std::isfinite(pos))
                return {false, {}, "INVALID_PARAM", "position must be a finite number"};
            bool relative = false;
            if (!parseOptionalBool(args, "is_relative", relative))
                return {false, {}, "INVALID_PARAM", "is_relative must be a boolean"};

            const auto result = engine->seekAction(
                static_cast<quintptr>(actionHandle), name, pos, relative);
            const QString state = result.result
                    == HarkenEditor::Bridge::kEngineOk
                ? QStringLiteral("accepted")
                : QStringLiteral("rejected");
            return AdvancedActionDetail::fromEngineResult(
                engine->isInitialized(), result.result,
                QJsonObject{{"action", name},
                            {"action_handle", exactUint64Json(actionHandle)},
                            {"seek_operation_id", exactUint64Json(result.operationId)},
                            {"state", state},
                            {"position", pos},
                            {"is_relative", relative},
                            {"snapped", snapToMarker}},
                QStringLiteral("Seek action"));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "query_seek_action";
        def.displayName = "Query Seek Action";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"seek_operation_id", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t operationId = 0;
            if (!parseExactPositiveUint64(args["seek_operation_id"], operationId))
                return {false, {}, "INVALID_PARAM", "seek_operation_id must be a positive exact uint64 number or decimal string"};
            HarkenEditor::Bridge::ActionSeekInfo info;
            const auto queryResult = engine->queryActionSeek(operationId, info);
            if (queryResult != HarkenEditor::Bridge::kEngineOk) {
                return AdvancedActionDetail::fromEngineResult(
                    engine->isInitialized(), queryResult,
                    QJsonObject{{"seek_operation_id", exactUint64Json(operationId)}},
                    QStringLiteral("Query seek action"));
            }
            QJsonObject data = seekInfoJson(operationId, info);
            if (info.state == HarkenEditor::Bridge::ActionSeekState::Accepted
                && info.serviceResult != HarkenEditor::Bridge::kEngineOk) {
                return AdvancedActionDetail::fromEngineResult(
                    engine->isInitialized(), info.serviceResult, std::move(data),
                    QStringLiteral("Service seek action"));
            }
            return AdvancedActionDetail::fromEngineResult(
                engine->isInitialized(), info.result, std::move(data),
                QStringLiteral("Query seek action"));
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "release_seek_action";
        def.displayName = "Release Seek Action";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"seek_operation_id", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t operationId = 0;
            if (!parseExactPositiveUint64(args["seek_operation_id"], operationId))
                return {false, {}, "INVALID_PARAM", "seek_operation_id must be a positive exact uint64 number or decimal string"};
            const auto result = engine->releaseActionSeek(operationId);
            return AdvancedActionDetail::fromEngineResult(
                engine->isInitialized(), result,
                QJsonObject{{"seek_operation_id", exactUint64Json(operationId)}},
                QStringLiteral("Release seek action"));
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "apply_modulator_preset";
        def.displayName = "Apply Modulator Preset";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"preset_name", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            QString presetName = args["preset_name"].toString();
            {
                auto sel = VNX::resolveSelector(presetName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                presetName = sel.name;
            }
            if (presetName.isEmpty())
                return {false, {}, "INVALID_PARAM", "preset_name required"};
            HarkenEditor::Bridge::ModulatorPresetDesc presetDesc;
            if (!HarkenEditor::Bridge::findModulatorPreset(presetName, presetDesc))
                return {false, {}, "NOT_FOUND",
                    QString("Modulator preset '%1' not found").arg(presetName)};
            const HarkenEditor::Bridge::ModulatorPresetDesc* preset = &presetDesc;
            static const char* typeNames[] = { "lfo", "envelope", "time", "step" };
            QJsonObject result;
            result["preset_name"] = presetName;
            result["type"] = QString(typeNames[preset->modulatorType]);
            result["category"] = QString(preset->category);
            result["is_factory"] = preset->isFactory;
            result["scope"] = static_cast<int>(preset->scope);
            QJsonObject params;
            if (preset->modulatorType == 0) {
                params["frequency"] = static_cast<double>(preset->lfoFrequency);
                params["depth"] = static_cast<double>(preset->lfoDepth);
                params["offset"] = static_cast<double>(preset->lfoOffset);
                params["pwm"] = static_cast<double>(preset->lfoPwm);
                params["smooth"] = static_cast<double>(preset->lfoSmooth);
                params["waveform"] = static_cast<int>(preset->lfoWaveform);
                params["unipolar"] = preset->lfoUnipolar != 0;
            } else if (preset->modulatorType == 1) {
                params["attack"] = static_cast<double>(preset->envAttackTime);
                params["hold"] = static_cast<double>(preset->envHoldTime);
                params["decay"] = static_cast<double>(preset->envDecayTime);
                params["sustain"] = static_cast<double>(preset->envSustainLevel);
                params["release"] = static_cast<double>(preset->envReleaseTime);
                params["attack_curve"] = static_cast<int>(preset->envAttackCurve);
                params["decay_curve"] = static_cast<int>(preset->envDecayCurve);
                params["release_curve"] = static_cast<int>(preset->envReleaseCurve);
                params["auto_release"] = preset->envAutoRelease != 0;
            } else if (preset->modulatorType == 2) {
                params["duration"] = static_cast<double>(preset->timeDurationSec);
                params["initial_value"] = static_cast<double>(preset->timeInitialValue);
                params["target_value"] = static_cast<double>(preset->timeTargetValue);
                params["curve_type"] = static_cast<int>(preset->timeCurveType);
                params["loop"] = preset->timeLoop != 0;
                params["ping_pong"] = preset->timePingPong != 0;
            } else if (preset->modulatorType == 3) {
                params["step_count"] = static_cast<int>(preset->stepCount);
                params["step_duration_ms"] = static_cast<double>(preset->stepDurationMs);
                params["transition_ms"] = static_cast<double>(preset->stepTransitionMs);
                params["loop"] = preset->stepLoop != 0;
                QJsonArray vals;
                for (uint32_t i = 0; i < preset->stepCount && i < 32; ++i)
                    vals.append(static_cast<double>(preset->stepValues[i]));
                params["values"] = vals;
            }
            result["params"] = params;
            QJsonObject binding;
            binding["target_param"] = static_cast<int>(preset->bindTargetParam);
            binding["curve_type"] = static_cast<int>(preset->bindCurveType);
            binding["mapped_min"] = static_cast<double>(preset->bindMappedMin);
            binding["mapped_max"] = static_cast<double>(preset->bindMappedMax);
            result["default_binding"] = binding;
            return {true, result};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "save_modulator_user_preset";
        def.displayName = "Save Modulator User Preset";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"preset_name", "string"}, {"type", "string"}, {"category", "string"}, {"scope", "int"}, {"params", "object"}, {"binding", "object"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            QString presetName = args["preset_name"].toString();
            {
                auto sel = VNX::resolveSelector(presetName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                presetName = sel.name;
            }
            if (presetName.isEmpty())
                return {false, {}, "INVALID_PARAM", "preset_name required"};
            QString typeName = args["type"].toString();
            uint8_t modType = 255;
            if (typeName == "lfo")        modType = 0;
            else if (typeName == "envelope") modType = 1;
            else if (typeName == "time")     modType = 2;
            else if (typeName == "step")     modType = 3;
            if (modType > 3)
                return {false, {}, "INVALID_PARAM", "type required: lfo, envelope, time, step"};
            HarkenEditor::Bridge::ModulatorPresetDesc preset;
            preset.name = presetName;
            preset.category = args["category"].toString("User");
            preset.modulatorType = modType;
            preset.scope = static_cast<uint8_t>(args["scope"].toInt(0));
            QJsonObject params = args["params"].toObject();
            if (modType == 0) {
                preset.lfoFrequency = static_cast<float>(params["frequency"].toDouble(1.0));
                preset.lfoDepth     = static_cast<float>(params["depth"].toDouble(1.0));
                preset.lfoOffset    = static_cast<float>(params["offset"].toDouble(0.0));
                preset.lfoPwm       = static_cast<float>(params["pwm"].toDouble(0.5));
                preset.lfoSmooth    = static_cast<float>(params["smooth"].toDouble(0.0));
                preset.lfoWaveform  = static_cast<uint8_t>(params["waveform"].toInt(0));
                preset.lfoUnipolar  = params["unipolar"].toBool(false) ? 1 : 0;
            } else if (modType == 1) {
                preset.envAttackTime   = static_cast<float>(params["attack"].toDouble(0.01));
                preset.envHoldTime     = static_cast<float>(params["hold"].toDouble(0.0));
                preset.envDecayTime    = static_cast<float>(params["decay"].toDouble(0.1));
                preset.envSustainLevel = static_cast<float>(params["sustain"].toDouble(0.7));
                preset.envReleaseTime  = static_cast<float>(params["release"].toDouble(0.3));
                preset.envAttackCurve  = static_cast<uint8_t>(params["attack_curve"].toInt(0));
                preset.envDecayCurve   = static_cast<uint8_t>(params["decay_curve"].toInt(2));
                preset.envReleaseCurve = static_cast<uint8_t>(params["release_curve"].toInt(2));
                preset.envAutoRelease  = params["auto_release"].toBool(false) ? 1 : 0;
            } else if (modType == 2) {
                preset.timeDurationSec  = static_cast<float>(params["duration"].toDouble(1.0));
                preset.timeInitialValue = static_cast<float>(params["initial_value"].toDouble(0.0));
                preset.timeTargetValue  = static_cast<float>(params["target_value"].toDouble(1.0));
                preset.timeCurveType    = static_cast<uint8_t>(params["curve_type"].toInt(0));
                preset.timeLoop         = params["loop"].toBool(false) ? 1 : 0;
                preset.timePingPong     = params["ping_pong"].toBool(false) ? 1 : 0;
            } else {
                preset.stepCount        = static_cast<uint32_t>(params["step_count"].toInt(4));
                preset.stepDurationMs   = static_cast<float>(params["step_duration_ms"].toDouble(250.0));
                preset.stepTransitionMs = static_cast<float>(params["transition_ms"].toDouble(5.0));
                preset.stepLoop         = params["loop"].toBool(true) ? 1 : 0;
                QJsonArray vals = params["values"].toArray();
                for (int i = 0; i < vals.size() && i < 32; ++i)
                    preset.stepValues[i] = static_cast<float>(vals[i].toDouble());
            }
            QJsonObject bindingObj = args["binding"].toObject();
            preset.bindTargetParam = static_cast<uint8_t>(bindingObj["target_param"].toInt(0));
            preset.bindCurveType   = static_cast<uint8_t>(bindingObj["curve_type"].toInt(0));
            preset.bindMappedMin   = static_cast<float>(bindingObj["mapped_min"].toDouble(0.0));
            preset.bindMappedMax   = static_cast<float>(bindingObj["mapped_max"].toDouble(1.0));
            bool ok = HarkenEditor::Bridge::saveModulatorUserPreset(preset);
            if (!ok)
                return {false, {}, "PRESET_FULL", "Max user presets reached for this modulator type"};
            static const char* typeNames[] = { "lfo", "envelope", "time", "step" };
            return {true, QJsonObject{
                {"preset_name", presetName},
                {"type", QString(typeNames[modType])}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "delete_modulator_user_preset";
        def.displayName = "Delete Modulator User Preset";
        def.category = "advanced";
        def.paramSchema = QJsonObject{{"preset_name", "string"}, {"type", "string"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            QString presetName = args["preset_name"].toString();
            {
                auto sel = VNX::resolveSelector(presetName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                presetName = sel.name;
            }
            if (presetName.isEmpty())
                return {false, {}, "INVALID_PARAM", "preset_name required"};
            QString typeName = args["type"].toString();
            uint8_t modType = 255;
            if (typeName == "lfo")        modType = 0;
            else if (typeName == "envelope") modType = 1;
            else if (typeName == "time")     modType = 2;
            else if (typeName == "step")     modType = 3;
            if (modType > 3)
                return {false, {}, "INVALID_PARAM", "type required: lfo, envelope, time, step"};
            bool ok = HarkenEditor::Bridge::deleteModulatorUserPreset(modType, presetName);
            if (!ok)
                return {false, {}, "NOT_FOUND", "Preset not found or is a factory preset"};
            static const char* typeNames[] = { "lfo", "envelope", "time", "step" };
            return {true, QJsonObject{
                {"preset_name", presetName},
                {"type", QString(typeNames[modType])}}};
        };
        mgr.registerAction(def);
    }
}

}
