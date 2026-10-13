
#include "../ActionManager.h"
#include "../AudioEngine.h"
#include "../AuditionManager.h"
#include "../CurveSelection.h"
#include "ActionSelectors.h"
#include "../../MainWindow.h"
#include "../BuiltinLayouts.h"
#include "../../Widgets/Inspector/InspectorPanel.h"
#include <DockManager.h>
#include <DockWidget.h>
#include <QVector3D>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QMetaObject>
#include <QDateTime>
#include <QDebug>
#include <QThread>
#include <QUuid>

#include <cstdint>

namespace VNX {


static ActionResult engineNotAvailable()
{
    return {false, {}, "ENGINE_NOT_AVAILABLE", "AudioEngine is not available"};
}

static ActionResult mainWindowNotAvailable()
{
    return {false, {}, "MAIN_WINDOW_NOT_AVAILABLE", "MainWindow is not available"};
}


void registerSystemActions(ActionManager& mgr,
                           AudioEngine* engine,
                           MainWindow* mainWindow,
                           QMap<quintptr, QJsonObject>* activePlaybacks)
{

    {
        ActionDef def;
        def.id = "init";
        def.displayName = "Initialize Engine";
        def.category = "system";
        def.paramSchema = QJsonObject{{"sample_rate", "int"}, {"buffer_size", "int"}, {"max_voices", "int"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            int sampleRate = args["sample_rate"].toInt(0);
            int bufferSize = args["buffer_size"].toInt(0);
            int maxVoices = args["max_voices"].toInt(64);
            bool success = engine->initialize(sampleRate, bufferSize, maxVoices);
            if (success)
                return {true, QJsonObject{{"initialized", true}}};
            return {false, {}, "INIT_FAILED", "Failed to initialize audio engine"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "shutdown";
        def.displayName = "Shutdown Engine";
        def.category = "system";
        def.undoable = false;
        def.handler = [engine](const QJsonObject&) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->shutdown();
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "start_output";
        def.displayName = "Start Audio Output";
        def.category = "system";
        def.undoable = false;
        def.handler = [engine](const QJsonObject&) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized())
                return {false, {}, "NOT_INITIALIZED", "Audio engine is not initialized"};
            bool success = engine->startOutput();
            if (success)
                return {true, QJsonObject{{"playing", true}}};
            return {false, {}, "START_OUTPUT_FAILED", "Failed to start audio output"};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "stop_output";
        def.displayName = "Stop Audio Output";
        def.category = "system";
        def.undoable = false;
        def.handler = [engine](const QJsonObject&) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->stopOutput();
            return {true, QJsonObject{{"playing", false}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "post_action";
        def.displayName = "Post Action";
        def.category = "system";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [engine, mainWindow](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QString name = args["name"].toString();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "Action name is required"};
            auto result = engine->playByNameDetailed(name);
            if (mainWindow) {
                auto* tm = mainWindow->auditionManager();
                if (tm && result.playbackHandle != 0) {
                    tm->registerExternalPlayback(name, result.playbackHandle);
                }
            }
            return {true, QJsonObject{
                {"action_handle", static_cast<qint64>(result.playbackHandle)},
                {"action_name",   name},
                {"has_sound",    result.hasSound}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "post_action_3d";
        def.displayName = "Post Action 3D";
        def.category = "system";
        def.paramSchema = QJsonObject{{"name", "string"}, {"x", "float"}, {"y", "float"}, {"z", "float"}};
        def.undoable = false;
        def.handler = [engine, activePlaybacks](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QString name = args["name"].toString();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "Action name is required"};
            float x = args["x"].toDouble(0.0);
            float y = args["y"].toDouble(0.0);
            float z = args["z"].toDouble(0.0);
            QVector3D position(x, y, z);
            quintptr handle = engine->playByName3D(name, position);
            if (activePlaybacks) {
                (*activePlaybacks)[handle] = QJsonObject{
                    {"handle", static_cast<qint64>(handle)},
                    {"name", name},
                    {"position", QJsonArray{x, y, z}},
                    {"type", "emitter"}
                };
            }
            return {true, QJsonObject{
                {"action_handle", static_cast<qint64>(handle)},
                {"action_name", name},
                {"position", QJsonArray{x, y, z}}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "stop_action";
        def.displayName = "Stop Action";
        def.category = "system";
        def.paramSchema = QJsonObject{
            {"handle", "int64"}, {"fade_ms", "float"}
        };
        def.undoable = false;
        def.handler = [engine, activePlaybacks](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            qint64 handle = args["handle"].toInteger(0);
            float fadeMs = static_cast<float>(args["fade_ms"].toDouble(0.0));
            if (handle == 0)
                return {false, {}, "INVALID_ARGS",
                        "handle is required"};
            engine->stopPlayback(static_cast<quintptr>(handle), fadeMs);
            if (activePlaybacks)
                activePlaybacks->remove(static_cast<quintptr>(handle));
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "stop_all";
        def.displayName = "Stop All Actions";
        def.category = "system";
        def.paramSchema = QJsonObject{{"fade_ms", "float"}};
        def.undoable = false;
        def.handler = [engine, mainWindow, activePlaybacks](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            float fadeMs = static_cast<float>(args["fade_ms"].toDouble(0.0));
            engine->stopAll(fadeMs);
            if (activePlaybacks) activePlaybacks->clear();
            if (mainWindow) {
                auto* tm = mainWindow->auditionManager();
                if (tm) tm->stopAll(fadeMs);
            }
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "trigger_action";
        def.displayName = "Trigger Action";
        def.category = "system";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [engine, mainWindow](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QString name = args.value("name").toString();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "MISSING_PARAM", "Missing required parameter: name"};
            quintptr handle = engine->playByName(name);
            Q_UNUSED(mainWindow);
            return {true, QJsonObject{
                {"action_name", name},
                {"action_handle", static_cast<qint64>(handle)},
                {"triggered", (handle != 0)}
            }};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_listener";
        def.displayName = "Set Listener Transform";
        def.category = "system";
        def.paramSchema = QJsonObject{{"pos", "array"}, {"forward", "array"}, {"up", "array"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QJsonArray posArray = args["pos"].toArray();
            if (posArray.size() < 3)
                return {false, {}, "INVALID_PARAM", "pos must be an array of 3 numbers [x, y, z]"};
            QJsonArray fwdArray = args["forward"].toArray();
            QJsonArray upArray = args["up"].toArray();
            QVector3D pos(posArray.at(0).toDouble(), posArray.at(1).toDouble(), posArray.at(2).toDouble());
            QVector3D forward(
                fwdArray.size() > 0 ? fwdArray.at(0).toDouble() : 0.0,
                fwdArray.size() > 1 ? fwdArray.at(1).toDouble() : 0.0,
                fwdArray.size() > 2 ? fwdArray.at(2).toDouble() : -1.0);
            QVector3D up(
                upArray.size() > 0 ? upArray.at(0).toDouble() : 0.0,
                upArray.size() > 1 ? upArray.at(1).toDouble() : 1.0,
                upArray.size() > 2 ? upArray.at(2).toDouble() : 0.0);
            engine->setListenerTransform(pos, forward, up);
            return {true, QJsonObject{
                {"position", QJsonArray{pos.x(), pos.y(), pos.z()}},
                {"forward", QJsonArray{forward.x(), forward.y(), forward.z()}},
                {"up", QJsonArray{up.x(), up.y(), up.z()}}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_action_position";
        def.displayName = "Set Action Position";
        def.category = "system";
        def.paramSchema = QJsonObject{
            {"handle", "int64"},
            {"x", "float"}, {"y", "float"}, {"z", "float"}
        };
        def.undoable = false;
        def.handler = [engine, activePlaybacks](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            qint64 handle = args["handle"].toInteger(0);
            float x = args["x"].toDouble(0.0);
            float y = args["y"].toDouble(0.0);
            float z = args["z"].toDouble(0.0);
            if (handle == 0)
                return {false, {}, "INVALID_ARGS",
                        "handle is required"};
            QVector3D position(x, y, z);
            engine->setPlaybackPosition(static_cast<quintptr>(handle), position);
            if (activePlaybacks && activePlaybacks->contains(static_cast<quintptr>(handle)))
                (*activePlaybacks)[static_cast<quintptr>(handle)]["position"] = QJsonArray{x, y, z};
            return {true, QJsonObject{
                {"handle", handle},
                {"position", QJsonArray{x, y, z}}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "add_listener";
        def.displayName = "Add Listener";
        def.category = "system";
        def.undoable = false;
        def.handler = [engine](const QJsonObject&) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t id = engine->addListener();
            return {true, QJsonObject{{"listener_id", static_cast<int>(id)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "remove_listener";
        def.displayName = "Remove Listener";
        def.category = "system";
        def.paramSchema = QJsonObject{{"listener_id", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->removeListener(static_cast<uint32_t>(args["listener_id"].toInt()));
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_listener_position";
        def.displayName = "Set Listener Position";
        def.category = "system";
        def.paramSchema = QJsonObject{{"listener_id", "uint32"}, {"pos", "array"}, {"forward", "array"}, {"up", "array"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QJsonArray pos = args["pos"].toArray();
            if (pos.size() < 3)
                return {false, {}, "INVALID_PARAM", "pos must be an array of 3 numbers [x, y, z]"};
            uint32_t listenerId = static_cast<uint32_t>(args["listener_id"].toInt());
            if (listenerId >= 8)
                return {false, {}, "INVALID_PARAM",
                    QString("listener_id %1 out of range [0..7]").arg(listenerId)};
            if (listenerId > 0 && !engine->isListenerActive(listenerId))
                return {false, {}, "LISTENER_NOT_ACTIVE",
                    QString("listener_id %1 is not active — call add_listener first").arg(listenerId)};
            float fx = 0, fy = 0, fz = -1;
            QJsonArray fwd = args["forward"].toArray();
            if (fwd.size() >= 3) {
                fx = static_cast<float>(fwd[0].toDouble());
                fy = static_cast<float>(fwd[1].toDouble());
                fz = static_cast<float>(fwd[2].toDouble());
            }
            float ux = 0, uy = 1, uz = 0;
            QJsonArray up = args["up"].toArray();
            if (up.size() >= 3) {
                ux = static_cast<float>(up[0].toDouble());
                uy = static_cast<float>(up[1].toDouble());
                uz = static_cast<float>(up[2].toDouble());
            }
            engine->setListenerPositionById(listenerId,
                static_cast<float>(pos[0].toDouble()),
                static_cast<float>(pos[1].toDouble()),
                static_cast<float>(pos[2].toDouble()),
                fx, fy, fz, ux, uy, uz);
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_game_object_listeners";
        def.displayName = "Set Game Object Listeners";
        def.category = "system";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"listener_ids", "array"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goId = static_cast<uint64_t>(args["game_object_id"].toDouble());
            QJsonArray ids = args["listener_ids"].toArray();
            std::vector<uint32_t> listenerIds;
            for (const auto& v : ids) listenerIds.push_back(static_cast<uint32_t>(v.toInt()));
            engine->setGameObjectListeners(goId, listenerIds.data(),
                                           static_cast<uint32_t>(listenerIds.size()));
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_listener_spatialization";
        def.displayName = "Set Listener Spatialization";
        def.category = "system";
        def.paramSchema = QJsonObject{{"listener_id", "uint32"}, {"spatialized", "bool"},
                                      {"panning_rule", "uint8"}, {"speaker_config", "uint8"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t listenerId = static_cast<uint32_t>(args["listener_id"].toInt(0));
            bool spatialized = args["spatialized"].toBool(true);
            uint8_t panningRule = static_cast<uint8_t>(args["panning_rule"].toInt(0));
            uint8_t speakerConfig = static_cast<uint8_t>(args["speaker_config"].toInt(0));
            engine->setListenerSpatialization(listenerId, spatialized, panningRule, speakerConfig);
            return {true, QJsonObject{
                {"listener_id", static_cast<int>(listenerId)},
                {"spatialized", spatialized},
                {"panning_rule", panningRule},
                {"speaker_config", speakerConfig}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_multi_position";
        def.displayName = "Set Multi-Position";
        def.category = "system";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"positions", "array"}, {"type", "uint8"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goId = static_cast<uint64_t>(args["game_object_id"].toDouble());
            QJsonArray posArr = args["positions"].toArray();
            std::vector<float> positions;
            for (const auto& v : posArr) positions.push_back(v.toDouble());
            uint32_t count = static_cast<uint32_t>(positions.size()) / 6;
            uint8_t type = static_cast<uint8_t>(args["type"].toInt(0));
            engine->setMultiPosition(goId, positions.data(), count, type);
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "clear_multi_position";
        def.displayName = "Clear Multi-Position";
        def.category = "system";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goId = static_cast<uint64_t>(args["game_object_id"].toDouble());
            engine->clearMultiPosition(goId);
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_automation_path";
        def.displayName = "Set Automation Path";
        def.category = "system";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"points", "array"},
                                      {"interpolation", "uint8"}, {"loop", "bool"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goId = static_cast<uint64_t>(args["game_object_id"].toDouble());
            QJsonArray ptArr = args["points"].toArray();
            std::vector<float> points;
            for (const auto& v : ptArr) points.push_back(v.toDouble());
            uint32_t count = static_cast<uint32_t>(points.size()) / 5;
            uint8_t interp = static_cast<uint8_t>(args["interpolation"].toInt(0));
            uint8_t loop = args["loop"].toBool(false) ? 1 : 0;
            engine->setAutomationPath(goId, points.data(), count, interp, loop);
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "start_automation";
        def.displayName = "Start Automation";
        def.category = "system";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->startAutomation(static_cast<uint64_t>(args["game_object_id"].toDouble()));
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "stop_automation";
        def.displayName = "Stop Automation";
        def.category = "system";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->stopAutomation(static_cast<uint64_t>(args["game_object_id"].toDouble()));
            return {true, QJsonObject{}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "load_pack";
        def.displayName = "Load Pack";
        def.category = "system";
        def.paramSchema = QJsonObject{{"path", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QString path = args["path"].toString();
            if (path.isEmpty())
                return {false, {}, "INVALID_ARGS", "Pack path is required"};
            bool success = engine->loadPack(path);
            if (success)
                return {true, QJsonObject{{"path", path}, {"loaded", true}}};
            return {false, {}, "LOAD_PACK_FAILED",
                    QString("Failed to load pack: %1").arg(path)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "unload_pack";
        def.displayName = "Unload Pack";
        def.category = "system";
        def.paramSchema = QJsonObject{{"path", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QString path = args["path"].toString();
            if (path.isEmpty())
                return {false, {}, "INVALID_ARGS", "Pack path is required"};
            engine->unloadPack(path);
            return {true, QJsonObject{{"path", path}, {"unloaded", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_pack_routing_priority";
        def.displayName = "Set Pack Routing Priority";
        def.category = "system";
        def.paramSchema = QJsonObject{{"pack_name", "string"}, {"priority", "int"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "ENGINE_NOT_AVAILABLE", "PackManager not available"};
            QString packName = args["pack_name"].toString();
            {
                auto sel = VNX::resolveSelector(packName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                packName = sel.name;
            }
            if (packName.isEmpty())
                return {false, {}, "INVALID_ARGS", "pack_name is required"};
            int priority = args["priority"].toInt(0);
            if (!engine->setPackRoutingPriority(packName, priority))
                return {false, {}, "ENGINE_NOT_AVAILABLE", "PackManager not available"};
            return {true, QJsonObject{{"pack_name", packName}, {"priority", priority}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "set_cpu_budget_config";
        def.displayName = "Set CPU Budget Config";
        def.category = "system";
        def.paramSchema = QJsonObject{{"escalation_pct", "float"}, {"recovery_pct", "float"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "ENGINE_NOT_INITIALIZED", "Engine not initialized"};
            float escalation = static_cast<float>(args.value("escalation_pct").toDouble(80.0));
            float recovery = static_cast<float>(args.value("recovery_pct").toDouble(60.0));
            if (escalation <= recovery || escalation > 100.0f || recovery < 0.0f)
                return {false, {}, "INVALID_PARAM", "escalation_pct must be > recovery_pct, both in [0,100]"};
            if (!engine->setCpuBudgetConfig(escalation, recovery))
                return {false, {}, "NOT_AVAILABLE", "MixerThread not available"};
            return {true, QJsonObject{
                {"escalation_pct", static_cast<double>(escalation)},
                {"recovery_pct", static_cast<double>(recovery)}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_memory_budget_limit";
        def.displayName = "Set Memory Budget Limit";
        def.category = "system";
        def.paramSchema = QJsonObject{{"category", "uint32"}, {"max_bytes", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "ENGINE_NOT_INITIALIZED", "Engine not initialized"};
            uint32_t category = static_cast<uint32_t>(args.value("category").toInt(-1));
            double maxBytesD = args.value("max_bytes").toDouble(0.0);
            if (category >= 8)
                return {false, {}, "INVALID_PARAM", "category must be 0-7"};
            uint64_t maxBytes = static_cast<uint64_t>(maxBytesD);
            if (engine->setMemoryBudgetLimit(category, maxBytes) != HarkenEditor::Bridge::kEngineOk)
                return {false, {}, "SET_FAILED", "Failed to set memory budget limit"};
            return {true, QJsonObject{
                {"category", static_cast<int>(category)},
                {"max_bytes", static_cast<double>(maxBytes)}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_listener_output_bus";
        def.displayName = "Set Listener Output Bus";
        def.category = "system";
        def.paramSchema = QJsonObject{{"listener_id", "uint32"}, {"bus_id", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "ENGINE_NOT_INITIALIZED", "Engine not initialized"};
            uint32_t listenerId = static_cast<uint32_t>(args.value("listener_id").toInt(-1));
            harkenstudio::ObjectId busId{};
            if (!parseObjectIdJson(args.value("bus_id"), &busId))
                return {false, {}, "INVALID_ARGS", "bus_id must be an object UUID"};
            if (engine->setListenerOutputBus(listenerId, busId) != HarkenEditor::Bridge::kEngineOk)
                return {false, {}, "SET_FAILED", "Failed to set listener output bus"};
            return {true, QJsonObject{
                {"listener_id", static_cast<int>(listenerId)},
                {"bus_id", objectIdString(busId)}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_spatial_fallback_policy";
        def.displayName = "Set Spatial Fallback Policy";
        def.category = "system";
        def.paramSchema = QJsonObject{{"policy", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!args.contains("policy"))
                return {false, {}, "INVALID_ARGS", "Required: policy"};
            QString policyStr = args["policy"].toString().toLower();
            {
                auto sel = VNX::resolveSelector(policyStr);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                policyStr = sel.name;
            }
            uint8_t policy;
            if (policyStr == "retry") policy = 0;
            else if (policyStr == "channel_fallback") policy = 1;
            else if (policyStr == "silent") policy = 2;
            else return {false, {}, "INVALID_ARGS", "Unknown policy: " + policyStr};
            if (!engine->setSpatialFallbackPolicy(policy))
                return {false, {}, "OBJECT_ROUTER_UNAVAILABLE", "ObjectRouter not initialized"};
            static const char* policyNames[] = {"retry", "channel_fallback", "silent"};
            return {true, QJsonObject{{"policy", policyNames[policy]}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_preferred_format";
        def.displayName = "Set Preferred Format";
        def.category = "system";
        def.paramSchema = QJsonObject{{"sample_rate", "int"}, {"buffer_size", "int"}};
        def.undoable = false;
        def.handler = [](const QJsonObject& args) -> ActionResult {
            int sampleRate = args.value("sample_rate").toInt(0);
            int bufferSize = args.value("buffer_size").toInt(0);
            if (sampleRate <= 0 && bufferSize <= 0)
                return {false, {}, "INVALID_ARGS", "Provide sample_rate or buffer_size"};
            return {true, QJsonObject{
                {"sample_rate", sampleRate > 0 ? sampleRate : 48000},
                {"buffer_size", bufferSize > 0 ? bufferSize : 256},
                {"note", "Format preference recorded. Restart output to apply."}
            }};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "open_remote_connect";
        def.displayName = "Open Remote Connect";
        def.category = "system";
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject&) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            mainWindow->showRemoteConnect();
            return {true, QJsonObject{{"opened", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "watch_parameter";
        def.displayName = "Watch Parameter";
        def.category = "system";
        def.paramSchema = QJsonObject{{"object_id", "uint32"}, {"param_id", "uint32"}, {"watch_type", "uint8"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t objectId = args["object_id"].toInt(0);
            uint32_t paramId = args["param_id"].toInt(0);
            uint8_t type = static_cast<uint8_t>(args["watch_type"].toInt(0));
            int32_t slot = engine->watchParameter(objectId, paramId, type);
            if (slot == INT32_MIN)
                return {false, {}, "NOT_INIT", "LiveParameterInspector not available"};
            if (slot < 0) return {false, {}, "FULL", "Watch list full (max 64)"};
            return {true, QJsonObject{{"slot", slot}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "unwatch_parameter";
        def.displayName = "Unwatch Parameter";
        def.category = "system";
        def.paramSchema = QJsonObject{{"object_id", "uint32"}, {"param_id", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "NOT_INIT", "LiveParameterInspector not available"};
            uint32_t objectId = args["object_id"].toInt(0);
            uint32_t paramId = args["param_id"].toInt(0);
            engine->unwatchParameter(objectId, paramId);
            return {true, QJsonObject{{"removed", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_monitor_filter";
        def.displayName = "Set Monitor Filter";
        def.category = "system";
        def.paramSchema = QJsonObject{{"mask", "uint64"}, {"mask_hex", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "NOT_INIT", "HarkenSystem not available"};
            uint64_t mask = 0;
            if (args.contains("mask_hex")) {
                bool ok = false;
                mask = args["mask_hex"].toString().toULongLong(&ok, 16);
                if (!ok) return {false, {}, "INVALID_PARAM", "Invalid hex mask"};
            } else {
                mask = static_cast<uint64_t>(args["mask"].toDouble(0xFFFFFFFFFFFFFFFF));
            }
            engine->setMonitorFilterMask(mask);
            return {true, QJsonObject{{"mask", QString::number(mask, 16)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_metering_interval";
        def.displayName = "Set Metering Interval";
        def.category = "system";
        def.paramSchema = QJsonObject{{"interval_ms", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            if (!engine->isInitialized() || engine->isSimulationMode())
                return {false, {}, "NOT_INIT", "HarkenSystem not available"};
            uint32_t intervalMs = static_cast<uint32_t>(args["interval_ms"].toInt(16));
            engine->setMeteringInterval(intervalMs);
            return {true, QJsonObject{{"interval_ms", static_cast<int>(intervalMs)}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "switch_layout";
        def.displayName = "Switch Layout";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"layout", "string"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString layoutName = args["layout"].toString();
            {
                auto sel = VNX::resolveSelector(layoutName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                layoutName = sel.name;
            }
            if (layoutName.isEmpty())
                return {false, {}, "INVALID_ARGS", "Layout name is required"};
            const auto* builtin = VNX::BuiltinLayouts::findByToken(layoutName);
            if (!builtin)
                return {false, {}, "INVALID_LAYOUT", QString("Unknown layout: %1").arg(layoutName)};
            bool success = mainWindow->switchToView(builtin->id);
            if (success)
                return {true, QJsonObject{{"layout", builtin->id}, {"switched", true}}};
            return {false, {}, "SWITCH_LAYOUT_FAILED",
                    QString("Failed to switch to layout: %1").arg(layoutName)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "switch_view";
        def.displayName = "Switch View";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"view", "string"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            qWarning() << "[deprecated] switch_view, use switch_layout";
            if (!mainWindow) return mainWindowNotAvailable();
            QString viewName = args["view"].toString();
            {
                auto sel = VNX::resolveSelector(viewName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                viewName = sel.name;
            }
            if (viewName.isEmpty())
                return {false, {}, "INVALID_ARGS", "View name is required"};
            const auto* builtin = VNX::BuiltinLayouts::findByToken(viewName);
            if (!builtin)
                return {false, {}, "INVALID_VIEW", QString("Unknown view: %1").arg(viewName)};
            bool success = mainWindow->switchToView(builtin->id);
            if (success)
                return {true, QJsonObject{{"view", builtin->id}, {"switched", true}}};
            return {false, {}, "SWITCH_VIEW_FAILED",
                    QString("Failed to switch to view: %1").arg(viewName)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "reset_layout";
        def.displayName = "Reset Layout";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"view", "string"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString target = args["view"].toString();
            {
                auto sel = VNX::resolveSelector(target);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                target = sel.name;
            }
            const auto resolved = VNX::BuiltinLayouts::resolveResetTarget(
                target, mainWindow->currentView());
            if (resolved.kind == VNX::BuiltinLayouts::ResetTargetKind::Invalid)
                return {false, {}, "INVALID_VIEW",
                        QString("Unknown view: %1").arg(resolved.canonical)};
            if (resolved.kind == VNX::BuiltinLayouts::ResetTargetKind::Builtin
                && resolved.canonical != mainWindow->currentView())
                mainWindow->switchToView(resolved.canonical);
            if (!mainWindow->resetCurrentLayout())
                return {false, {}, "RESET_FAILED",
                        QString("Failed to reset layout: %1").arg(resolved.canonical)};
            return {true, QJsonObject{{"view", resolved.canonical}, {"reset", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "toggle_panel";
        def.displayName = "Toggle Panel";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"name", "string"}, {"visible", "bool"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString panelName = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(panelName);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                panelName = sel.name;
            }
            if (panelName.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required (panel objectName)"};
            QVariantMap current;
            for (const QVariantMap& summary : mainWindow->workspaceWindowSummaries()) {
                if (workspaceWindowSelectorMatches(
                        panelName,
                        summary.value(QStringLiteral("typeId")).toString(),
                        summary.value(QStringLiteral("name")).toString(),
                        summary.value(QStringLiteral("title")).toString(),
                        summary.value(QStringLiteral("instanceId")).toInt())) {
                    current = summary;
                    break;
                }
            }
            if (current.isEmpty())
                return {false, {}, "PANEL_NOT_FOUND", QString("Panel '%1' not found").arg(panelName)};
            bool newState;
            if (args.contains("visible"))
                newState = args["visible"].toBool();
            else
                newState = !current.value(QStringLiteral("visible")).toBool();

            QVariantMap updated;
            if (!mainWindow->setWorkspaceWindowVisible(panelName, newState, &updated))
                return {false, {}, "PANEL_TOGGLE_FAILED",
                        QString("Failed to set panel '%1' visible=%2").arg(panelName).arg(newState)};
            return {true, QJsonObject::fromVariantMap(updated)};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "open_curve_editor";
        def.displayName = "Open Curve Editor";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{
            {"name", "string"},
            {"attenuation", "string"},
            {"guid", "string"},
            {"attenuation_guid", "string"},
            {"dimension", "string"}
        };
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            QString attenuationName = args.value("name").toString().trimmed();
            if (attenuationName.isEmpty())
                attenuationName = args.value("attenuation").toString().trimmed();
            QString guidText = args.value("guid").toString().trimmed();
            if (guidText.isEmpty())
                guidText = args.value("attenuation_guid").toString().trimmed();

            AttenuationDimension dimension = AttenuationDimension::Volume;
            const QJsonValue dimensionValue = args.value("dimension");
            if (!dimensionValue.isUndefined()) {
                const QString dimensionName = dimensionValue.toString().trimmed();
                if (!dimensionValue.isString()
                    || (!dimensionName.isEmpty()
                        && !attenuationDimensionFromName(dimensionName, &dimension))) {
                    return {false, {}, "DIMENSION_REQUIRED",
                            "A valid curve dimension is required."};
                }
            }

            if (!mainWindow) return mainWindowNotAvailable();

            const QUuid guid = QUuid::fromString(guidText);
            const CurveSelection selection = CurveSelection::attenuation(
                guid, attenuationName, dimension);
            if (QThread::currentThread() == mainWindow->thread()) {
                mainWindow->openCurveEditor(selection);
            } else {
                QMetaObject::invokeMethod(mainWindow,
                                          [mainWindow, selection]() {
                                              mainWindow->openCurveEditor(selection);
                                          },
                                          Qt::BlockingQueuedConnection);
            }
            return {true, QJsonObject{
                {"opened", true},
                {"name", attenuationName},
                {"guid", guidText},
                {"dimension", QLatin1String(attenuationDimensionName(dimension))},
                {"dock", QStringLiteral("CurveEditorDock")}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "save_named_layout";
        def.displayName = "Save Named Layout";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString name = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required"};
            QString filePath;
            if (!mainWindow->saveNamedLayout(name, &filePath))
                return {false, {}, "FILE_WRITE_ERROR", "Cannot write layout file"};
            return {true, QJsonObject{{"name", name}, {"path", filePath}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "load_named_layout";
        def.displayName = "Load Named Layout";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString name = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required"};
            if (!mainWindow->loadNamedLayout(name))
                return {false, {}, "LAYOUT_NOT_FOUND", QString("Named layout '%1' not found").arg(name)};
            return {true, QJsonObject{{"name", name}, {"loaded", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "delete_named_layout";
        def.displayName = "Delete Named Layout";
        def.category = "ui_control";
        def.paramSchema = QJsonObject{{"name", "string"}};
        def.undoable = false;
        def.handler = [mainWindow](const QJsonObject& args) -> ActionResult {
            if (!mainWindow) return mainWindowNotAvailable();
            QString name = args["name"].toString().trimmed();
            {
                auto sel = VNX::resolveSelector(name);
                if (!sel.ok) return {false, {}, sel.errorCode, sel.errorMessage};
                name = sel.name;
            }
            if (name.isEmpty())
                return {false, {}, "INVALID_ARGS", "name is required"};
            if (!mainWindow->deleteNamedLayout(name))
                return {false, {}, "LAYOUT_NOT_FOUND", QString("Named layout '%1' not found").arg(name)};
            return {true, QJsonObject{{"name", name}, {"deleted", true}}};
        };
        mgr.registerAction(def);
    }
}

}
