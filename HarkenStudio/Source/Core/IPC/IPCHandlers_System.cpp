#include "IPCServerInternal.h"
#include "Widgets/Inspector/PropertySchemaJson.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QThread>


namespace harken::studio {


VBus* IPCServer::resolveBusByName(const QString& rawName) const
{
    const QString busName = rawName.trimmed();
    if (busName.isEmpty()) {
        return nullptr;
    }

    auto& pm = ProjectManager::instance();
    if (auto* item = pm.findBusItemByName(busName)) {
        if (auto* bc = item->comp<VBus>())
            return bc;
    }
    return nullptr;
}

void IPCServer::registerSystemHandlers()
{
    commandHandlers_["list_commands"] = [this](const QJsonObject& args) {
        if (!args.isEmpty())
            return errorResponse("INVALID_ARGS", "list_commands does not accept arguments.");
        return successResponse(listEditorCommands(this));
    };
    commandHandlers_["get_command_schema"] = [this](const QJsonObject& args) {
        const QString name = args.value("command").toString();
        if (args.size() != 1 || name.isEmpty())
            return errorResponse("INVALID_ARGS", "command must be a non-empty string.");
        const auto description = describeEditorCommand(name, this);
        if (!description.value("registered").toBool())
            return errorResponse("UNKNOWN_COMMAND", QString("Unknown command: %1").arg(name));
        return successResponse(description);
    };
    commandHandlers_["ping"] = [this](const QJsonObject& args) { return handlePing(args); };
    commandHandlers_["get_version"] = [this](const QJsonObject& args) { return handleGetVersion(args); };
    commandHandlers_["get_status"] = [this](const QJsonObject& args) { return handleGetStatus(args); };

    registerDesktopHandler("set_theme");
    registerDesktopHandler("get_theme");


    commandHandlers_["get_3d_scene"] = [this](const QJsonObject& args) { return handleGet3DScene(args); };
    commandHandlers_["get_listener_info"] = [this](const QJsonObject& args) {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
        const auto requestedId = args.contains("listener_id") ? args.value("listener_id").toInteger(-1) : 1;
        if (requestedId < 0 || requestedId > UINT32_MAX)
            return errorResponse("INVALID_PARAM", "listener_id must be an unsigned integer");
        const auto listenerId = static_cast<uint32_t>(requestedId);
        float x = 0, y = 0, z = 0, fwdX = 0, fwdY = 0, fwdZ = 0;
        bool active = engine_->getListenerInfo(listenerId, &x, &y, &z,
                                                &fwdX, &fwdY, &fwdZ);
        if (!active)
            return errorResponse("LISTENER_NOT_ACTIVE",
                                 "Listener " + QString::number(listenerId) + " is not active");
        QJsonObject result;
        result["listener_id"] = static_cast<int>(listenerId);
        result["active"] = true;
        result["position"] = QJsonArray{x, y, z};
        result["forward"] = QJsonArray{fwdX, fwdY, fwdZ};
        return successResponse(result);
    };
    commandHandlers_["get_loaded_packs"] = [this](const QJsonObject& args) { return handleGetLoadedPacks(args); };

    registerDesktopHandler("get_remote_connection_status");
    commandHandlers_["get_monitor_filter"] = [this](const QJsonObject&) -> QJsonObject {
        auto mask = bridge::monitorGlobalFilterMask();
        return successResponse(QJsonObject{
            {"global_filter", QString::number(mask, 16)},
            {"notification_type_count", 46}
        });
    };
    commandHandlers_["get_metering_snapshot"] = [this](const QJsonObject&) -> QJsonObject {
        return handleGetMixerLevels(QJsonObject{});
    };

    registerDesktopHandler("get_panel_rect");
    commandHandlers_["get_project_state"] = [this](const QJsonObject& args) { return handleGetProjectState(args); };
    commandHandlers_["get_playing_actions"] = [this](const QJsonObject& args) { return handleGetPlaybacks(args); };


    commandHandlers_["get_engine_snapshot"] = [this](const QJsonObject& args) { return handleGetEngineSnapshot(args); };
    commandHandlers_["get_active_voices"]   = [this](const QJsonObject& args) { return handleGetActiveVoices(args); };
    commandHandlers_["get_voice_node_virtual_states"] = [this](const QJsonObject& args) { return handleGetVoiceNodeVirtualStates(args); };
    commandHandlers_["get_bus_topology_plan_count"] = [this](const QJsonObject& args) { return handleGetBusTopologyPlanCount(args); };
    commandHandlers_["get_voice_metadata_chain"] = [this](const QJsonObject& args) { return handleGetVoiceMetadataChain(args); };
    commandHandlers_["get_bus_info"]        = [this](const QJsonObject& args) { return handleGetBusInfo(args); };
    commandHandlers_["get_performance"]     = [this](const QJsonObject& args) { return handleGetPerformance(args); };

    commandHandlers_["get_session_recap"] = [this](const QJsonObject&) {
        if (!engine_)
            return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        AudioEngine::SessionRecapData recap;
        if (!engine_->getSessionRecap(recap))
            return errorResponse("COLLECT_FAILED", "Failed to collect session recap");
        QJsonObject result;
        result["peak_active_voices"] = static_cast<int>(recap.peakActiveVoices);
        result["peak_virtual_voices"] = static_cast<int>(recap.peakVirtualVoices);
        result["active_buses"] = static_cast<int>(recap.peakActiveBuses);
        result["completed_and_active_voice_count"] = static_cast<int>(recap.completedAndActiveVoiceCount);
        result["backpressure_count"] = static_cast<int>(recap.totalUnderruns);
        result["last_cpu_percent"] = static_cast<double>(recap.avgCpuPercent);
        result["active_voices"] = static_cast<int>(recap.avgActiveVoices);
        result["active"] = recap.active;
        return successResponse(result);
    };

    commandHandlers_["get_cpu_budget_state"]    = [this](const QJsonObject& args) { return handleGetCpuBudgetState(args); };
    commandHandlers_["get_render_plan"]          = [this](const QJsonObject& args) { return handleGetRenderPlan(args); };
    commandHandlers_["get_job_stats"]            = [this](const QJsonObject& args) { return handleGetJobStats(args); };
    commandHandlers_["get_object_audio_status"]   = [this](const QJsonObject& args) { return handleGetObjectAudioStatus(args); };

    registerDesktopHandler("get_current_layout");
    registerDesktopHandler("get_layout_state");
    registerDesktopHandler("get_inspector_context");

    commandHandlers_["get_inspector_schema"] = [this](const QJsonObject& args) {
        const QString schemaId = args.value("schema_id").toString();
        if (schemaId.isEmpty())
            return errorResponse("MISSING_PARAM", "Required: schema_id");
        const auto schema = inspectorSchemaToJson(schemaId);
        if (schema.isEmpty())
            return errorResponse("SCHEMA_NOT_FOUND", QString("No schema found for '%1'").arg(schemaId));
        return successResponse(QJsonObject{{"schema_id", schemaId}, {"schema", schema}});
    };

    registerDesktopHandler("create_inspector_panel");

    registerDesktopHandler("create_editor");

    registerDesktopHandler("set_inspector_pinned");

    registerDesktopHandler("list_inspector_panels");

    registerDesktopHandler("close_inspector_panel");


    registerDesktopHandler("get_recent_logs");
    registerDesktopHandler("list_panels");
    registerDesktopHandler("list_named_layouts");
    registerDesktopHandler("get_selected_bus");

    commandHandlers_["get_app_state"] = [this](const QJsonObject& args) { return handleGetAppState(args); };

    commandHandlers_["list_modulators"] = [this](const QJsonObject& args) { return handleListModulators(args); };

    commandHandlers_["list_modulator_chains"] = [this](const QJsonObject& args) { return handleListModulatorChains(args); };


    commandHandlers_["get_modulator_presets"] = [this](const QJsonObject& args) { return handleGetModulatorPresets(args); };

    commandHandlers_["get_buffer_config"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        int32_t blockSize = engine_->getBufferSize();
        int32_t sr = bridge::mixerSampleRate();
        return successResponse(QJsonObject{
            {"block_size", blockSize},
            {"sample_rate", sr}
        });
    };

    commandHandlers_["get_modulator_binding"] = [this](const QJsonObject& args) -> QJsonObject {
        auto* project = ProjectManager::instance().currentProject();
        if (!project) return errorResponse("NO_PROJECT", "No project open");
        QString path = args["path"].toString();
        if (path.isEmpty()) return errorResponse("MISSING_PARAM", "Missing 'path' parameter");
        auto* item = findItemByPathInProject(project, path);
        if (!item) return errorResponse("NOT_FOUND", "Object not found: " + path);
        const auto* mix = item->comp<VMixNode>();
        return successResponse(QJsonObject{
            {"modulators", mix ? mix->bindings.toJson() : QJsonArray{}}
        });
    };

}


QJsonObject IPCServer::handlePing(const QJsonObject& args)
{
    Q_UNUSED(args);
    return successResponse(QJsonObject{{"pong", true}});
}


QJsonObject IPCServer::handleGetVersion(const QJsonObject& args)
{
    Q_UNUSED(args);
    return successResponse(QJsonObject{
        {"version", QCoreApplication::applicationVersion()},
        {"app", "HarkenStudio"},
        {"ipc_protocol", "1.0"}
    });
}


QJsonObject IPCServer::handleGetStatus(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    bool isPlaying = engine_->isPlaying() || engine_->activeVoiceCount() > 0;

    QJsonObject result{
        {"initialized", engine_->isInitialized()},
        {"playing", isPlaying},
        {"simulation_mode", engine_->isSimulationMode()},
        {"active_voices", engine_->activeVoiceCount()},
        {"cpu_usage", engine_->cpuUsage()},
        {"output_latency_ms", engine_->outputLatency() * 1000.0},
        {"sample_rate", engine_->actualSampleRate()},
        {"master_volume", engine_->masterVolume()}
    };

    return successResponse(result);
}


QJsonObject IPCServer::handleInit(const QJsonObject& args)
{
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    int sampleRate = args["sample_rate"].toInt(0);
    int bufferSize = args["buffer_size"].toInt(0);
    int maxVoices = args["max_voices"].toInt(64);
    
    bool success = engine_->initialize(sampleRate, bufferSize, maxVoices);
    
    if (success)
    {
        return successResponse(QJsonObject{{"initialized", true}});
    }
    else
    {
        return errorResponse("INIT_FAILED", "Failed to initialize audio engine");
    }
}


QJsonObject IPCServer::handleShutdown(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    engine_->shutdown();
    return successResponse();
}


QJsonObject IPCServer::handleStartOutput(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    if (!engine_->isInitialized())
    {
        return errorResponse("NOT_INITIALIZED", 
                             "Audio engine is not initialized",
                             "Call 'init_engine' command first.");
    }
    
    bool success = engine_->startOutput();
    
    if (success)
    {
        return successResponse(QJsonObject{{"playing", true}});
    }
    else
    {
        return errorResponse("START_OUTPUT_FAILED", "Failed to start audio output");
    }
}


QJsonObject IPCServer::handleStopOutput(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    engine_->stopOutput();
    return successResponse(QJsonObject{{"playing", false}});
}





QJsonObject IPCServer::handleStopAll(const QJsonObject& args)
{
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    float fadeMs = static_cast<float>(args["fade_ms"].toDouble(0.0));
    
    engine_->stopAll(fadeMs);
    activePlaybacks_.clear();

    if (audition_)
        audition_->stopAll(fadeMs);
    
    return successResponse();
}

QJsonObject IPCServer::handleSetMasterVolume(const QJsonObject& args)
{
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }

    float volume = args["volume"].toDouble(1.0);
    int fadeMs = args["fade_ms"].toInt(0);
    
    engine_->setMasterVolume(volume, fadeMs);
    
    return successResponse(QJsonObject{
        {"volume", volume}
    });
}


QJsonObject IPCServer::handleSetListener(const QJsonObject& args)
{
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    QJsonArray posArray = args["pos"].toArray();
    if (posArray.size() < 3)
        return errorResponse("INVALID_PARAM", "pos must be an array of 3 numbers [x, y, z]");

    QJsonArray fwdArray = args["forward"].toArray();
    QJsonArray upArray = args["up"].toArray();
    
    harken::studio::Vec3 pos(posArray.at(0).toDouble(), posArray.at(1).toDouble(), posArray.at(2).toDouble());
    harken::studio::Vec3 forward(
        fwdArray.size() > 0 ? fwdArray.at(0).toDouble() : 0.0,
        fwdArray.size() > 1 ? fwdArray.at(1).toDouble() : 0.0,
        fwdArray.size() > 2 ? fwdArray.at(2).toDouble() : -1.0);
    harken::studio::Vec3 up(
        upArray.size() > 0 ? upArray.at(0).toDouble() : 0.0,
        upArray.size() > 1 ? upArray.at(1).toDouble() : 1.0,
        upArray.size() > 2 ? upArray.at(2).toDouble() : 0.0);
    
    engine_->setListenerTransform(pos, forward, up);
    
    return successResponse(QJsonObject{
        {"position", QJsonArray{pos.x, pos.y, pos.z}},
        {"forward", QJsonArray{forward.x, forward.y, forward.z}},
        {"up", QJsonArray{up.x, up.y, up.z}}
    });
}



QJsonObject IPCServer::handleGet3DScene(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    QJsonArray objects;
    
    harken::studio::Vec3 listenerPos = engine_->listenerPosition();
    objects.append(QJsonObject{
        {"type", "listener"},
        {"position", QJsonArray{listenerPos.x, listenerPos.y, listenerPos.z}}
    });
    
    for (auto it = activePlaybacks_.constBegin(); it != activePlaybacks_.constEnd(); ++it)
    {
        QJsonObject obj = it.value();
        obj["type"] = "emitter";
        objects.append(obj);
    }
    
    return successResponse(QJsonObject{{"objects", objects}});
}


QJsonObject IPCServer::handleLoadPack(const QJsonObject& args)
{
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    QString path = args["path"].toString();
    if (path.isEmpty())
    {
        return errorResponse("INVALID_ARGS", "Pack path is required");
    }
    
    bool success = engine_->loadPack(path);
    
    if (success)
    {
        return successResponse(QJsonObject{
            {"path", path},
            {"loaded", true}
        });
    }
    else
    {
        return errorResponse("LOAD_PACK_FAILED", 
                             QString("Failed to load pack: %1").arg(path),
                             "Check if the pack file exists and is valid.");
    }
}


QJsonObject IPCServer::handleUnloadPack(const QJsonObject& args)
{
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    QString path = args["path"].toString();
    if (path.isEmpty())
    {
        return errorResponse("INVALID_ARGS", "Pack path is required");
    }
    
    engine_->unloadPack(path);
    
    return successResponse(QJsonObject{
        {"path", path},
        {"unloaded", true}
    });
}


QJsonObject IPCServer::handleGetLoadedPacks(const QJsonObject& args)
{
    Q_UNUSED(args);

    QJsonArray packArray;
    if (engine_)
        for (const auto& path : engine_->getLoadedPackPaths())
            packArray.append(QJsonObject{{"path", path}, {"name", QFileInfo(path).completeBaseName()}});

    return successResponse(QJsonObject{
        {"packs", packArray},
        {"count", static_cast<int>(packArray.size())}
    });
}


QJsonObject IPCServer::handleSetPackRoutingPriority(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    QString packName = args["pack_name"].toString();
    if (packName.isEmpty())
        return errorResponse("INVALID_ARGS", "pack_name is required");

    int priority = args["priority"].toInt(0);
    if (!engine_->setPackRoutingPriority(packName, priority))
        return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");

    return successResponse(QJsonObject{
        {"pack_name", packName},
        {"priority", priority}
    });
}

QJsonObject IPCServer::handleGetProjectState(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!projectManager_)
    {
        return errorResponse("PROJECT_MANAGER_NOT_AVAILABLE", "ProjectManager is not available");
    }
    
    QJsonObject state;
    state["project_loaded"] = projectManager_->isProjectOpen();
    
    auto* project = projectManager_->currentProject();
    if (project)
    {
        state["project_name"] = project->name;
        state["project_path"] = project->projectPath;
        state["is_dirty"] = projectManager_->hasUnsavedChanges();
        
        if (engine_)
        {
            state["active_voices"] = engine_->activeVoiceCount();
            state["is_playing"] = engine_->isPlaying();
        }
        else
        {
            state["active_voices"] = 0;
            state["is_playing"] = false;
        }
    }
    else
    {
        state["project_name"] = "";
        state["project_path"] = "";
        state["is_dirty"] = false;
        state["active_voices"] = 0;
        state["is_playing"] = false;
    }
    
    return successResponse(state);
}


QJsonObject IPCServer::handleGetPlaybacks(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    if (!engine_)
    {
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
    }
    
    QJsonArray playbacks;
    for (auto it = activePlaybacks_.constBegin(); it != activePlaybacks_.constEnd(); ++it)
    {
        playbacks.append(it.value());
    }
    
    return successResponse(QJsonObject{
        {"actions", playbacks},
        {"count", playbacks.size()}
    });
}























QJsonObject IPCServer::handleGetAppState(const QJsonObject& args)
{
    Q_UNUSED(args);
    
    QString state;
    switch (appState_) {
        case AppState::Starting:
            state = "starting";
            break;
        case AppState::ProjectPicker:
            state = "projectPicker";
            break;
        case AppState::Ready:
            if (projectManager_ && projectManager_->isProjectOpen())
                state = "project_loaded";
            else
                state = "no_project";
            break;
    }
    
    QJsonObject result;
    result["state"] = state;
    result["ipc_ready"] = true;
    
    if (projectManager_) {
        auto* project = projectManager_->currentProject();
        if (project) {
            result["project_name"] = project->name;
            result["project_path"] = project->projectPath;
        }
    }

    return successResponse(result);
}



QJsonObject IPCServer::handleGetEngineSnapshot(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    QJsonObject result;

    AudioEngine::DebugPerfData perf;
    if (engine_->getDebugPerformance(perf))
    {
        QJsonObject perfJson;
        perfJson["cpu_percent"]    = static_cast<double>(perf.audioCpuPercent);
        perfJson["active_voices"]  = static_cast<int>(perf.activeVoices);
        perfJson["virtual_voices"] = static_cast<int>(perf.virtualVoices);
        perfJson["voice_high_water"]     = static_cast<int>(perf.maxVoices);
        perfJson["active_buses"]   = static_cast<int>(perf.activeBuses);
        perfJson["backpressure_count"] = static_cast<int>(perf.underrunCount);
        perfJson["parameter_commit_count"] = static_cast<int>(perf.parameterCommitCount);
        result["performance"] = perfJson;
    }

    std::vector<AudioEngine::DebugVoiceData> voices;
    if (engine_->getDebugVoices(voices))
    {
        QJsonArray voiceArr;
        for (const auto& v : voices)
        {
            QJsonObject vj;
            vj["object_id"]    = bridge::jsonIntIfAvailable(v.objectIDAvailable, static_cast<int>(v.objectID));
            vj["bus_id"]       = bridge::jsonStringIfAvailable(v.busIdAvailable, objectIdString(v.busId));
            vj["priority"]     = bridge::jsonIntIfAvailable(v.priorityAvailable, v.priority);
            vj["state"]        = bridge::jsonIntIfAvailable(v.stateAvailable, v.state);
            vj["volume"]       = bridge::jsonNumberIfAvailable(v.volumeAvailable, static_cast<double>(v.volume));
            vj["pitch"]        = bridge::jsonNumberIfAvailable(v.pitchAvailable, static_cast<double>(v.pitch));
            vj["peak_level"]   = bridge::jsonNumberIfAvailable(v.peakLevelAvailable, static_cast<double>(v.peakLevel));
            vj["playing_id"]   = bridge::isAvailable(v.playingIDAvailable)
                ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue();
            vj["container_id"] = bridge::jsonIntIfAvailable(v.containerIdAvailable, v.containerId);
            vj["is_muted"]     = bridge::jsonBoolIfAvailable(v.isMutedAvailable, v.isMuted);
            vj["hdr_gain_db"]  = bridge::jsonNumberIfAvailable(v.hdrGainDbAvailable, static_cast<double>(v.hdrGainDb));
            vj["insert_fx"]    = bridge::jsonIntIfAvailable(v.insertEffectCountAvailable, static_cast<int>(v.insertEffectCount));
            voiceArr.append(vj);
        }
        result["voices"] = voiceArr;
    }

    std::vector<AudioEngine::DebugBusData> buses;
    if (engine_->getDebugBuses(buses))
    {
        QJsonArray busArr;
        for (const auto& b : buses)
        {
            QJsonObject bj;
            bj["bus_id"]        = bridge::jsonStringIfAvailable(b.busIdAvailable, objectIdString(b.busId));
            bj["name"]          = bridge::jsonStringIfAvailable(b.nameAvailable, b.name);
            bj["bus_type"]      = bridge::jsonIntIfAvailable(b.busTypeAvailable, static_cast<int>(b.busType));
            bj["volume"]        = bridge::jsonNumberIfAvailable(b.volumeAvailable, static_cast<double>(b.volume));
            bj["peak_l"]        = bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakL));
            bj["peak_r"]        = bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakR));
            bj["rms_l"]         = bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsL));
            bj["rms_r"]         = bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsR));
            bj["voice_count"]   = bridge::jsonIntIfAvailable(b.voiceCountAvailable, static_cast<int>(b.voiceCount));
            bj["cpu_pct"]       = bridge::jsonNumberIfAvailable(b.cpuPctAvailable, static_cast<double>(b.cpuPct));
            bj["muted"]         = bridge::jsonBoolIfAvailable(b.mutedAvailable, b.muted);
            bj["soloed"]        = bridge::jsonBoolIfAvailable(b.soloedAvailable, b.soloed);
            bj["hdr_enabled"]   = bridge::jsonBoolIfAvailable(b.hdrEnabledAvailable, b.hdrEnabled);
            bj["effect_count"]  = bridge::jsonIntIfAvailable(b.effectCountAvailable, static_cast<int>(b.effectCount));
            if (bridge::isAvailable(b.parentBusIdAvailable))
                bj["parent_bus_id"] = objectIdString(b.parentBusId);
            busArr.append(bj);
        }
        result["buses"] = busArr;
    }

    return successResponse(result);
}


QJsonObject IPCServer::handleGetActiveVoices(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    std::vector<AudioEngine::DebugVoiceData> voices;
    AudioEngine::DebugPerfData performance;
    if (!engine_->getDebugVoices(voices) || !engine_->getDebugPerformance(performance))
        return errorResponse("COLLECT_FAILED", "Failed to collect voice data");

    QJsonArray arr;
    for (const auto& v : voices)
    {
        QJsonObject vj;
        vj["object_id"]    = bridge::jsonIntIfAvailable(v.objectIDAvailable, static_cast<int>(v.objectID));
        vj["bus_id"]       = bridge::jsonStringIfAvailable(v.busIdAvailable, objectIdString(v.busId));
        vj["priority"]     = bridge::jsonIntIfAvailable(v.priorityAvailable, v.priority);
        vj["state"]        = bridge::jsonIntIfAvailable(v.stateAvailable, v.state);
        vj["volume"]       = bridge::jsonNumberIfAvailable(v.volumeAvailable, static_cast<double>(v.volume));
        vj["pitch"]        = bridge::jsonNumberIfAvailable(v.pitchAvailable, static_cast<double>(v.pitch));
        vj["peak_level"]   = bridge::jsonNumberIfAvailable(v.peakLevelAvailable, static_cast<double>(v.peakLevel));
        vj["playing_id"]   = bridge::isAvailable(v.playingIDAvailable)
            ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue();
        vj["container_id"]    = bridge::jsonIntIfAvailable(v.containerIdAvailable, v.containerId);
        vj["is_muted"]        = bridge::jsonBoolIfAvailable(v.isMutedAvailable, v.isMuted);
        vj["hdr_gain_db"]     = bridge::jsonNumberIfAvailable(v.hdrGainDbAvailable, static_cast<double>(v.hdrGainDb));
        vj["insert_fx"]       = bridge::jsonIntIfAvailable(v.insertEffectCountAvailable, static_cast<int>(v.insertEffectCount));
        arr.append(vj);
    }

    return successResponse(QJsonObject{
        {"voices", arr},
        {"count", static_cast<int>(performance.activeVoices)},
        {"reported_count", static_cast<int>(voices.size())}
    });
}


QJsonObject IPCServer::handleGetVoiceNodeVirtualStates(const QJsonObject& args)
{

    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    const uint32_t filterVoiceId = static_cast<uint32_t>(args.value("voice_id").toDouble(0));

    std::vector<AudioEngine::DebugVoiceData> voices;
    if (!engine_->getDebugVoices(voices))
        return errorResponse("COLLECT_FAILED", "Failed to collect voice data");

    static const char* kVirtualStateNames[] = {
        "Active",
        "VirtualOn",
        "VirtualOff",
        "Bypassed_CPU",
        "Bypassed_SR",
        "Bypassed_Disabled"
    };

    QJsonArray result;
    for (const auto& v : voices)
    {
        if (filterVoiceId != 0 && static_cast<uint32_t>(v.playingID) != filterVoiceId)
            continue;

        QJsonArray nodes;

        result.append(QJsonObject{
            {"voice_id",  bridge::isAvailable(v.playingIDAvailable)
                ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue()},
            {"object_id", bridge::jsonIntIfAvailable(v.objectIDAvailable, static_cast<int>(v.objectID))},
            {"nodes",     nodes},
            {"note",      "per-node virtual states are unavailable (no runtime data source)"}
        });
    }

    (void)kVirtualStateNames;
    return successResponse(QJsonObject{
        {"voices", result},
        {"count",  static_cast<int>(result.size())}
    });
}



QJsonObject IPCServer::handleGetBusTopologyPlanCount(const QJsonObject& args)
{
    Q_UNUSED(args);

    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    uint64_t count = 0;
    if (!engine_->getBusTopologyPlanCount(count))
        return errorResponse("RUNTIME_SNAPSHOT_UNAVAILABLE", "Runtime snapshot unavailable");

    return successResponse(QJsonObject{
        {"count", static_cast<qint64>(count)}
    });
}


QJsonObject IPCServer::handleGetVoiceMetadataChain(const QJsonObject& args)
{

    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    const uint32_t filterVoiceId = static_cast<uint32_t>(args.value("voice_id").toDouble(0));

    std::vector<AudioEngine::DebugVoiceData> voices;
    if (!engine_->getDebugVoices(voices))
        return errorResponse("COLLECT_FAILED", "Failed to collect voice data");

    QJsonArray result;
    for (const auto& v : voices)
    {
        if (filterVoiceId != 0 && static_cast<uint32_t>(v.playingID) != filterVoiceId)
            continue;

        std::vector<AudioEngine::DebugVoiceMetadataNode> chain;
        const bool haveChain = engine_->getVoiceMetadataChain(
            static_cast<uint32_t>(v.playingID), chain);

        QJsonArray chainArr;
        if (haveChain)
        {
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const auto& n = chain[i];
                QJsonObject metaObj{
                    {"curve_snapshot_id", static_cast<qint64>(n.curveSnapshotID)},
                    {"voice_priority",    static_cast<qint64>(n.voicePriority)},
                    {"virtual_state",     static_cast<qint64>(n.virtualState)},
                    {"reserved",          static_cast<qint64>(n.reserved)}
                };
                chainArr.append(QJsonObject{
                    {"node_index", static_cast<int>(i)},
                    {"node_type",  n.nodeType},
                    {"metadata",   metaObj}
                });
            }
        }

        QJsonObject voiceObj{
            {"voice_id", bridge::isAvailable(v.playingIDAvailable)
                ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue()},
            {"chain",    chainArr}
        };
        if (!haveChain)
        {
            voiceObj.insert("note",
                QStringLiteral("per-node metadata is unavailable "
                               "(no runtime data source)"));
        }

        result.append(voiceObj);
    }

    return successResponse(QJsonObject{
        {"voices", result},
        {"count",  static_cast<int>(result.size())}
    });
}


QJsonObject IPCServer::handleGetBusInfo(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    std::vector<AudioEngine::DebugBusData> buses;
    if (!engine_->getDebugBuses(buses))
        return errorResponse("COLLECT_FAILED", "Failed to collect bus data");

    QJsonArray arr;
    for (const auto& b : buses)
    {
        QJsonObject bj;
        bj["bus_id"]       = bridge::jsonStringIfAvailable(b.busIdAvailable, objectIdString(b.busId));
        bj["name"]         = bridge::jsonStringIfAvailable(b.nameAvailable, b.name);
        bj["bus_type"]     = bridge::jsonIntIfAvailable(b.busTypeAvailable, static_cast<int>(b.busType));
        bj["volume"]       = bridge::jsonNumberIfAvailable(b.volumeAvailable, static_cast<double>(b.volume));
        bj["peak_l"]       = bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakL));
        bj["peak_r"]       = bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakR));
        bj["rms_l"]        = bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsL));
        bj["rms_r"]        = bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsR));
        bj["voice_count"]  = bridge::jsonIntIfAvailable(b.voiceCountAvailable, static_cast<int>(b.voiceCount));
        bj["cpu_pct"]      = bridge::jsonNumberIfAvailable(b.cpuPctAvailable, static_cast<double>(b.cpuPct));
        bj["muted"]        = bridge::jsonBoolIfAvailable(b.mutedAvailable, b.muted);
        bj["soloed"]       = bridge::jsonBoolIfAvailable(b.soloedAvailable, b.soloed);
        bj["hdr_enabled"]  = bridge::jsonBoolIfAvailable(b.hdrEnabledAvailable, b.hdrEnabled);
        bj["effect_count"] = bridge::jsonIntIfAvailable(b.effectCountAvailable, static_cast<int>(b.effectCount));
        if (bridge::isAvailable(b.parentBusIdAvailable))
            bj["parent_bus_id"] = objectIdString(b.parentBusId);
        arr.append(bj);
    }

    return successResponse(QJsonObject{
        {"buses", arr},
        {"count", static_cast<int>(buses.size())}
    });
}



QJsonObject IPCServer::handleGetPerformance(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    AudioEngine::DebugPerfData perf;
    if (!engine_->getDebugPerformance(perf))
        return errorResponse("COLLECT_FAILED", "Failed to collect performance data");

    QJsonObject result;
    result["cpu_percent"]    = static_cast<double>(perf.audioCpuPercent);
    result["active_voices"]  = static_cast<int>(perf.activeVoices);
    result["virtual_voices"] = static_cast<int>(perf.virtualVoices);
    result["voice_high_water"] = static_cast<int>(perf.maxVoices);
    result["active_buses"]   = static_cast<int>(perf.activeBuses);
    result["backpressure_count"] = static_cast<int>(perf.underrunCount);
    result["parameter_commit_count"] = static_cast<int>(perf.parameterCommitCount);

    float peakL = 0.0f, peakR = 0.0f;
    engine_->getMasterMeterPeak(peakL, peakR);
    result["master_peak_l"]  = static_cast<double>(peakL);
    result["master_peak_r"]  = static_cast<double>(peakR);
    result["output_latency_ms"] = engine_->outputLatency() * 1000.0;
    result["sample_rate"]    = engine_->actualSampleRate();

    return successResponse(result);
}



QJsonObject IPCServer::handleGetCpuBudgetState(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    AudioEngine::CpuBudgetState state;
    if (!engine_->getCpuBudgetState(state))
        return errorResponse("NOT_AVAILABLE", "CPUBudgetManager not available");

    static const char* levelNames[] = {
        "Normal", "VirtualizeLowPriority", "ReduceEffectQuality",
        "DisableSends", "DropVoices"
    };

    QJsonObject result;
    result["smoothed_load_pct"] = static_cast<double>(state.smoothedLoadPct);
    result["level"]             = static_cast<int>(state.level);
    result["level_name"]        = (state.level < 5) ? levelNames[state.level] : "Unknown";
    return successResponse(result);
}


QJsonObject IPCServer::handleSetCpuBudgetConfig(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    const auto budgetStatus = bridge::cpuBudgetManagerStatus();
    if (budgetStatus == bridge::IpcQueryStatus::NoEngine)
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
    if (budgetStatus != bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_AVAILABLE", "MixerThread not available");

    float escalation = static_cast<float>(args.value("escalation_pct").toDouble(80.0));
    float recovery   = static_cast<float>(args.value("recovery_pct").toDouble(60.0));

    if (escalation <= recovery || escalation > 100.0f || recovery < 0.0f)
        return errorResponse("INVALID_PARAM", "escalation_pct must be > recovery_pct, both in [0,100]");

    if (!engine_->setCpuBudgetConfig(escalation, recovery))
        return errorResponse("NOT_AVAILABLE", "MixerThread not available");

    return successResponse(QJsonObject{
        {"escalation_pct", static_cast<double>(escalation)},
        {"recovery_pct", static_cast<double>(recovery)}
    });
}


QJsonObject IPCServer::handleGetRenderPlan(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    AudioEngine::RenderPlanInfo rp;
    if (!engine_->getRenderPlan(rp))
        return errorResponse("NOT_AVAILABLE", "RenderPlan not valid");

    QJsonArray orderArr;
    for (uint32_t i = 0; i < rp.busCount; ++i)
        orderArr.append(objectIdString(rp.busOrder[i]));

    QJsonObject result;
    result["bus_order"]       = orderArr;
    result["bus_count"]       = static_cast<int>(rp.busCount);
    result["standard_bus_end"] = static_cast<int>(rp.standardBusEnd);
    result["receive_bus_end"]     = static_cast<int>(rp.receiveBusEnd);
    result["version"]         = static_cast<int>(rp.version);
    return successResponse(result);
}


QJsonObject IPCServer::handleGetJobStats(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    AudioEngine::JobStatsInfo js;
    if (!engine_->getJobStats(js))
        return errorResponse("NOT_AVAILABLE", "Render worker diagnostics are not available");

    QJsonObject result;
    result["active_worker_count"] = static_cast<int>(js.activeWorkerCount);
    result["worker_busy_peak_us"] = static_cast<double>(js.maxWorkerRenderUs);
    result["block_size"] = static_cast<int>(js.chunkSize);
    result["active_voice_count"] = static_cast<int>(js.activeVoiceCount);
    result["voice_stage_last_us"] = static_cast<double>(js.totalEstimatedCost);

    return successResponse(result);
}


QJsonObject IPCServer::handleGetObjectAudioStatus(const QJsonObject& args)
{
    Q_UNUSED(args);
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    AudioEngine::ObjectAudioStatusInfo info;
    if (!engine_->getObjectAudioStatus(info))
        return errorResponse("OBJECT_AUDIO_UNAVAILABLE", "Failed to query object audio status");

    static const char* policyNames[] = {"retry", "channel_fallback", "silent"};

    QJsonObject result;
    result["active_dynamic_objects"]  = static_cast<int>(info.activeDynamicObjects);
    result["max_dynamic_objects"]     = static_cast<int>(info.maxDynamicObjects);
    result["channel_based_count"]     = static_cast<int>(info.channelBasedCount);
    result["object_based_count"]      = static_cast<int>(info.objectBasedCount);
    result["fallback_count"]          = static_cast<int>(info.fallbackCount);
    result["capacity_pct"]            = static_cast<double>(info.capacityPct);
    result["fallback_policy"]         = policyNames[info.fallbackPolicy < 3 ? info.fallbackPolicy : 0];
    result["sink_available"]          = info.sinkAvailable;

    return successResponse(result);
}

QJsonObject IPCServer::handleSetSpatialFallbackPolicy(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    if (!args.contains("policy"))
        return errorResponse("INVALID_ARGS",
            "Required: policy",
            "hint: valid policies: retry, channel_fallback, silent");

    QString policyStr = args["policy"].toString().toLower();
    uint8_t policy;

    if (policyStr == "retry") policy = 0;
    else if (policyStr == "channel_fallback") policy = 1;
    else if (policyStr == "silent") policy = 2;
    else
        return errorResponse("INVALID_ARGS",
            "Unknown policy: " + policyStr,
            "hint: valid policies: retry, channel_fallback, silent");

    if (!engine_->setSpatialFallbackPolicy(policy))
        return errorResponse("OBJECT_ROUTER_UNAVAILABLE",
            "ObjectRouter not initialized");

    QJsonObject result;
    static const char* policyNames[] = {"retry", "channel_fallback", "silent"};
    result["policy"] = policyNames[policy];
    return successResponse(result);
}




QJsonObject IPCServer::handleListModulators(const QJsonObject& args)
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    std::vector<bridge::ActiveModulatorInfo> active;
    const auto status = bridge::listActiveModulators(active);
    if (status == bridge::IpcQueryStatus::NoEngine)
        return errorResponse("NOT_INIT", "HarkenSystem not available");
    if (status != bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_INIT", "ModulatorManager not available");

    QJsonArray modulators;
    for (const auto& m : active) {
        QJsonObject entry;
        entry["owner_id"] = static_cast<qint64>(m.ownerId);

        const char* scopeNames[] = {"voice", "note", "game_object", "global"};
        entry["scope"] = (m.scope < 4) ? scopeNames[m.scope] : "unknown";

        const char* typeNames[] = {"lfo", "envelope", "time"};
        entry["type"] = (m.type < 3) ? typeNames[m.type] : "unknown";

        const char* stateNames[] = {"new", "triggered", "paused", "release_triggered", "finished", "terminated"};
        entry["state"] = (m.state < 6) ? stateNames[m.state] : "unknown";

        entry["output_value"] = static_cast<double>(m.outputValue);
        entry["voice_count"] = m.voiceCount;
        entry["binding_count"] = m.bindingCount;
        entry["elapsed_frames"] = static_cast<qint64>(m.elapsedFrames);
        modulators.append(entry);
    }

    QJsonObject result;
    result["count"] = static_cast<int>(active.size());
    result["modulators"] = modulators;
    return successResponse(result);
}


QJsonObject IPCServer::handleListModulatorChains(const QJsonObject& )
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    std::vector<bridge::ActiveModulatorChainInfo> active;
    const auto status = bridge::listActiveModulatorChains(active);
    if (status == bridge::IpcQueryStatus::NoEngine)
        return errorResponse("NOT_INIT", "HarkenSystem not available");
    if (status != bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_INIT", "ModulatorChainManager not available");

    QJsonArray chains;
    for (const auto& chain : active) {
        QJsonObject entry;
        entry["chain_id"] = static_cast<qint64>(chain.chainId);
        entry["owner_id"] = static_cast<qint64>(chain.ownerId);

        const char* scopeNames[] = {"voice", "note", "game_object", "global"};
        entry["scope"] = (chain.scope < 4) ? scopeNames[chain.scope] : "unknown";

        entry["node_count"] = chain.nodeCount;
        entry["binding_count"] = chain.bindingCount;
        entry["final_output"] = static_cast<double>(chain.finalOutput);

        QJsonArray nodeArr;
        for (const auto& node : chain.nodes)
        {
            QJsonObject nObj;
            if (node.hasCtx)
            {
                const char* typeNames[] = {"lfo", "envelope", "time", "step"};
                nObj["type"] = (node.type < 4) ? typeNames[node.type] : "unknown";
                nObj["output"] = static_cast<double>(node.output);
            }
            const char* modeNames[] = {"multiply", "additive", "override"};
            nObj["mode"] = (node.mode < 3) ? modeNames[node.mode] : "unknown";
            nObj["depth"] = static_cast<double>(node.depth);
            nodeArr.append(nObj);
        }
        entry["nodes"] = nodeArr;
        chains.append(entry);
    }

    QJsonObject result;
    result["count"] = static_cast<int>(active.size());
    result["chains"] = chains;
    return successResponse(result);
}


QJsonObject IPCServer::handleGetModulatorPresets(const QJsonObject& args)
{
    QString typeName = args["type"].toString();
    int typeFilter = -1;
    if (!typeName.isEmpty())
    {
        if (typeName == "lfo")        typeFilter = 0;
        else if (typeName == "envelope") typeFilter = 1;
        else if (typeName == "time")     typeFilter = 2;
        else if (typeName == "step")     typeFilter = 3;
        else return errorResponse("INVALID_PARAM",
            "type must be: lfo, envelope, time, step",
            "hint: omit type to list all presets");
    }

    static const char* typeNames[] = { "lfo", "envelope", "time", "step" };

    QJsonArray presetsArr;
    const auto presets = bridge::listModulatorPresets(typeFilter);
    for (const auto& p : presets)
    {
        QJsonObject pObj;
        pObj["name"] = p.name;
        pObj["category"] = p.category;
        pObj["type"] = QString(typeNames[p.modulatorType]);
        pObj["is_factory"] = p.isFactory;

        if (p.modulatorType == 0) {
            pObj["frequency"] = static_cast<double>(p.lfoFrequency);
            pObj["depth"] = static_cast<double>(p.lfoDepth);
            pObj["waveform"] = static_cast<int>(p.lfoWaveform);
        } else if (p.modulatorType == 1) {
            pObj["attack"] = static_cast<double>(p.envAttackTime);
            pObj["decay"] = static_cast<double>(p.envDecayTime);
            pObj["sustain"] = static_cast<double>(p.envSustainLevel);
            pObj["release"] = static_cast<double>(p.envReleaseTime);
        } else if (p.modulatorType == 2) {
            pObj["duration"] = static_cast<double>(p.timeDurationSec);
            pObj["curve"] = static_cast<int>(p.timeCurveType);
        } else if (p.modulatorType == 3) {
            pObj["step_count"] = static_cast<int>(p.stepCount);
            pObj["step_duration_ms"] = static_cast<double>(p.stepDurationMs);
        }

        pObj["bind_target"] = static_cast<int>(p.bindTargetParam);
        presetsArr.append(pObj);
    }

    QJsonObject result;
    result["count"] = presetsArr.size();
    result["total_presets"] = static_cast<int>(bridge::modulatorPresetTotalCount());
    result["presets"] = presetsArr;
    return successResponse(result);
}

QJsonObject IPCServer::handleApplyModulatorPreset(const QJsonObject& args)
{
    QString presetName = args["preset_name"].toString();
    if (presetName.isEmpty())
        return errorResponse("INVALID_PARAM", "preset_name required");

    bridge::ModulatorPresetDesc preset;
    if (!bridge::findModulatorPreset(presetName, preset))
        return errorResponse("NOT_FOUND",
            QString("Modulator preset '%1' not found").arg(presetName)
                .toStdString().c_str());

    static const char* typeNames[] = { "lfo", "envelope", "time", "step" };

    QJsonObject result;
    result["preset_name"] = presetName;
    result["type"] = QString(typeNames[preset.modulatorType]);
    result["category"] = preset.category;
    result["is_factory"] = preset.isFactory;
    result["scope"] = static_cast<int>(preset.scope);

    QJsonObject params;
    if (preset.modulatorType == 0) {
        params["frequency"] = static_cast<double>(preset.lfoFrequency);
        params["depth"] = static_cast<double>(preset.lfoDepth);
        params["offset"] = static_cast<double>(preset.lfoOffset);
        params["pwm"] = static_cast<double>(preset.lfoPwm);
        params["smooth"] = static_cast<double>(preset.lfoSmooth);
        params["waveform"] = static_cast<int>(preset.lfoWaveform);
        params["unipolar"] = preset.lfoUnipolar != 0;
    } else if (preset.modulatorType == 1) {
        params["attack"] = static_cast<double>(preset.envAttackTime);
        params["hold"] = static_cast<double>(preset.envHoldTime);
        params["decay"] = static_cast<double>(preset.envDecayTime);
        params["sustain"] = static_cast<double>(preset.envSustainLevel);
        params["release"] = static_cast<double>(preset.envReleaseTime);
        params["attack_curve"] = static_cast<int>(preset.envAttackCurve);
        params["decay_curve"] = static_cast<int>(preset.envDecayCurve);
        params["release_curve"] = static_cast<int>(preset.envReleaseCurve);
        params["auto_release"] = preset.envAutoRelease != 0;
    } else if (preset.modulatorType == 2) {
        params["duration"] = static_cast<double>(preset.timeDurationSec);
        params["initial_value"] = static_cast<double>(preset.timeInitialValue);
        params["target_value"] = static_cast<double>(preset.timeTargetValue);
        params["curve_type"] = static_cast<int>(preset.timeCurveType);
        params["loop"] = preset.timeLoop != 0;
        params["ping_pong"] = preset.timePingPong != 0;
    } else if (preset.modulatorType == 3) {
        params["step_count"] = static_cast<int>(preset.stepCount);
        params["step_duration_ms"] = static_cast<double>(preset.stepDurationMs);
        params["transition_ms"] = static_cast<double>(preset.stepTransitionMs);
        params["loop"] = preset.stepLoop != 0;
        QJsonArray vals;
        for (uint32_t i = 0; i < preset.stepCount && i < 32; ++i)
            vals.append(static_cast<double>(preset.stepValues[i]));
        params["values"] = vals;
    }
    result["params"] = params;

    QJsonObject binding;
    binding["target_param"] = static_cast<int>(preset.bindTargetParam);
    binding["curve_type"] = static_cast<int>(preset.bindCurveType);
    binding["mapped_min"] = static_cast<double>(preset.bindMappedMin);
    binding["mapped_max"] = static_cast<double>(preset.bindMappedMax);
    result["default_binding"] = binding;

    return successResponse(result);
}

QJsonObject IPCServer::handleSaveModulatorUserPreset(const QJsonObject& args)
{
    QString presetName = args["preset_name"].toString();
    if (presetName.isEmpty())
        return errorResponse("INVALID_PARAM", "preset_name required");

    QString typeName = args["type"].toString();
    uint8_t modType = 255;
    if (typeName == "lfo")        modType = 0;
    else if (typeName == "envelope") modType = 1;
    else if (typeName == "time")     modType = 2;
    else if (typeName == "step")     modType = 3;

    if (modType > 3)
        return errorResponse("INVALID_PARAM",
            "type required: lfo, envelope, time, step");

    bridge::ModulatorPresetDesc preset;
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

    if (!bridge::saveModulatorUserPreset(preset))
        return errorResponse("PRESET_FULL", "Max user presets reached for this modulator type");

    QJsonObject result;
    result["preset_name"] = presetName;
    static const char* typeNames[] = { "lfo", "envelope", "time", "step" };
    result["type"] = QString(typeNames[modType]);
    return successResponse(result);
}

QJsonObject IPCServer::handleDeleteModulatorUserPreset(const QJsonObject& args)
{
    QString presetName = args["preset_name"].toString();
    if (presetName.isEmpty())
        return errorResponse("INVALID_PARAM", "preset_name required");

    QString typeName = args["type"].toString();
    uint8_t modType = 255;
    if (typeName == "lfo")        modType = 0;
    else if (typeName == "envelope") modType = 1;
    else if (typeName == "time")     modType = 2;
    else if (typeName == "step")     modType = 3;

    if (modType > 3)
        return errorResponse("INVALID_PARAM",
            "type required: lfo, envelope, time, step");

    if (!bridge::deleteModulatorUserPreset(modType, presetName))
        return errorResponse("NOT_FOUND", "Preset not found or is a factory preset");

    QJsonObject result;
    result["preset_name"] = presetName;
    static const char* typeNames[] = { "lfo", "envelope", "time", "step" };
    result["type"] = QString(typeNames[modType]);
    return successResponse(result);
}


QJsonObject IPCServer::handleSetMemoryBudgetLimit(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    if (!bridge::engineSessionActive())
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");

    uint32_t category = static_cast<uint32_t>(args.value("category").toInt(-1));
    double maxBytesD  = args.value("max_bytes").toDouble(0.0);
    if (category >= 8)
        return errorResponse("INVALID_PARAM", "category must be 0-7",
                             "0=General,1=Media,2=Streaming,3=DSP,4=Voice,5=Object,6=SpatialAudio,7=Debug");
    uint64_t maxBytes = static_cast<uint64_t>(maxBytesD);

    if (engine_->setMemoryBudgetLimit(category, maxBytes) != harken::studio::bridge::kEngineOk)
        return errorResponse("SET_FAILED", "Failed to set memory budget limit");

    return successResponse(QJsonObject{
        {"category", static_cast<int>(category)},
        {"max_bytes", static_cast<double>(maxBytes)}
    });
}


QJsonObject IPCServer::handleSetListenerOutputBus(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    if (!bridge::engineSessionActive())
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");

    uint32_t listenerId = static_cast<uint32_t>(args.value("listener_id").toInt(-1));
    harken::studio::types::ObjectId busId{};
    if (!parseObjectIdJson(args.value("bus_id"), &busId))
        return errorResponse("INVALID_ARGS", "bus_id must be an object UUID");

    if (engine_->setListenerOutputBus(listenerId, busId) != harken::studio::bridge::kEngineOk)
        return errorResponse("SET_FAILED", "Failed to set listener output bus");

    return successResponse(QJsonObject{
        {"listener_id", static_cast<int>(listenerId)},
        {"bus_id",      objectIdString(busId)}
    });
}

}
