#include "IPCServerInternal.h"
#include "ThemeManager.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "../BuiltinLayouts.h"
#include "../../Widgets/Inspector/InspectorPanel.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QThread>


namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

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
    commandHandlers_["ping"] = [this](const QJsonObject& args) { return handlePing(args); };
    commandHandlers_["get_version"] = [this](const QJsonObject& args) { return handleGetVersion(args); };
    commandHandlers_["get_status"] = [this](const QJsonObject& args) { return handleGetStatus(args); };

    commandHandlers_["set_theme"] = [this](const QJsonObject& args) -> QJsonObject {
        QString themeId = args.value("theme").toString().trimmed();
        if (themeId == QStringLiteral("dark"))
            themeId = builtinThemeId();
        if (themeId.isEmpty())
            return errorResponse("INVALID_ARGS", "theme is required");

        ThemeManager& manager = ThemeManager::instance();
        ThemeManagerOperationResult result;
        const auto select = [&]() { result = manager.selectTheme(themeId); };
        if (QThread::currentThread() == manager.thread())
            select();
        else
            QMetaObject::invokeMethod(&manager, select, Qt::BlockingQueuedConnection);
        if (!result.ok())
        {
            const ThemePackageDiagnostic& diagnostic = result.diagnostics.front();
            return errorResponse(
                themePackageErrorCodeName(diagnostic.code), diagnostic.message);
        }
        return successResponse(QJsonObject{{"theme", manager.currentThemeId()}});
    };
    commandHandlers_["get_theme"] = [this](const QJsonObject&) -> QJsonObject {
        ThemeManager& manager = ThemeManager::instance();
        QString themeId;
        const auto read = [&]() { themeId = manager.currentThemeId(); };
        if (QThread::currentThread() == manager.thread())
            read();
        else
            QMetaObject::invokeMethod(&manager, read, Qt::BlockingQueuedConnection);
        return successResponse(QJsonObject{{"theme", themeId}});
    };


    commandHandlers_["get_3d_scene"] = [this](const QJsonObject& args) { return handleGet3DScene(args); };
    commandHandlers_["get_listener_info"] = [this](const QJsonObject& args) {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
        uint32_t listenerId = static_cast<uint32_t>(args["listener_id"].toInt(0));
        float x = 0, y = 0, z = 0, fwdX = 0, fwdY = 0, fwdZ = 0;
        int spatialized = 1;
        uint32_t panningRule = 0;
        bool active = engine_->getListenerInfo(listenerId, &x, &y, &z,
                                                &fwdX, &fwdY, &fwdZ,
                                                &spatialized, &panningRule);
        if (!active)
            return errorResponse("LISTENER_NOT_ACTIVE",
                                 "Listener " + QString::number(listenerId) + " is not active");
        QJsonObject result;
        result["listener_id"] = static_cast<int>(listenerId);
        result["active"] = true;
        result["position"] = QJsonArray{x, y, z};
        result["forward"] = QJsonArray{fwdX, fwdY, fwdZ};
        result["spatialized"] = (spatialized != 0);
        result["panning_rule"] = static_cast<int>(panningRule);
        return successResponse(result);
    };
    commandHandlers_["get_loaded_packs"] = [this](const QJsonObject& args) { return handleGetLoadedPacks(args); };

    commandHandlers_["get_pack_metrics"] = [this](const QJsonObject& args) { return handleGetPackMetrics(args); };
    commandHandlers_["get_pack_load_history"] = [this](const QJsonObject& args) { return handleGetPackLoadHistory(args); };
    commandHandlers_["get_action_routing"] = [this](const QJsonObject& args) { return handleGetActionRouting(args); };
    commandHandlers_["resolve_action_packs"] = [this](const QJsonObject& args) { return handleResolveActionPacks(args); };
    commandHandlers_["validate_multipack_collisions"] = [this](const QJsonObject& args) { return handleValidateMultipackCollisions(args); };
    commandHandlers_["get_codec_capabilities"] = [this](const QJsonObject& args) { return handleGetCodecCapabilities(args); };

    commandHandlers_["get_remote_connection_status"] = [this](const QJsonObject&) -> QJsonObject {
        if (!mainWindow_ || !mainWindow_->remoteConnection())
            return errorResponse("REMOTE_NOT_AVAILABLE", "Remote connection controller is unavailable");
        const auto* remote = mainWindow_->remoteConnection();
        const auto target = remote->target();
        const auto snapshot = remote->snapshot();
        return successResponse(QJsonObject{
            {"connected", remote->state() == Bridge::RemoteConnectionState::Connected},
            {"status", remote->statusText()}, {"application", target.application},
            {"address", target.address}, {"port", target.port},
            {"instance_id", target.instanceId}, {"engine_version", target.engineVersion},
            {"capture_enabled", remote->captureEnabled()}, {"snapshot_valid", snapshot.valid},
            {"block_index", QString::number(snapshot.blockIndex)},
            {"active_voices", static_cast<int>(snapshot.activeVoices)}
        });
    };
    commandHandlers_["get_watched_parameters"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        std::vector<Bridge::WatchedParamEntry> entries;
        if (Bridge::watchedParameters(entries) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NOT_INIT", "LiveParameterInspector not available");
        QJsonArray watches;
        for (const auto& w : entries) {
            watches.append(QJsonObject{
                {"slot", static_cast<int>(w.slot)},
                {"object_id", static_cast<int>(w.objectId)},
                {"param_id", static_cast<int>(w.paramId)},
                {"watch_type", w.watchType}
            });
        }
        return successResponse(QJsonObject{{"watches", watches},
            {"count", watches.size()}});
    };
    commandHandlers_["get_live_parameters"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        std::vector<Bridge::LiveParamSnapshotEntry> snaps;
        if (Bridge::liveParameters(snaps) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NOT_INIT", "LiveParameterInspector not available");
        QJsonArray params;
        for (const auto& s : snaps) {
            params.append(QJsonObject{
                {"object_id", static_cast<int>(s.objectId)},
                {"param_id", static_cast<int>(s.paramId)},
                {"watch_type", s.watchType},
                {"value", static_cast<double>(s.value)},
                {"prev_value", static_cast<double>(s.prevValue)},
                {"changed", s.changed},
                {"timestamp_us", static_cast<qint64>(s.timestampUs)}
            });
        }
        return successResponse(QJsonObject{{"parameters", params},
            {"count", static_cast<int>(snaps.size())}});
    };
    commandHandlers_["get_monitor_filter"] = [this](const QJsonObject&) -> QJsonObject {
        auto mask = Bridge::monitorGlobalFilterMask();
        return successResponse(QJsonObject{
            {"global_filter", QString::number(mask, 16)},
            {"notification_type_count", 46}
        });
    };
    commandHandlers_["get_metering_snapshot"] = [this](const QJsonObject&) -> QJsonObject {
        return handleGetMixerLevels(QJsonObject{});
    };

    commandHandlers_["get_panel_rect"] = [this](const QJsonObject& args) { return handleGetPanelRect(args); };
    commandHandlers_["get_project_state"] = [this](const QJsonObject& args) { return handleGetProjectState(args); };
    commandHandlers_["get_playing_actions"] = [this](const QJsonObject& args) { return handleGetPlaybacks(args); };


    commandHandlers_["get_engine_snapshot"] = [this](const QJsonObject& args) { return handleGetEngineSnapshot(args); };
    commandHandlers_["get_active_voices"]   = [this](const QJsonObject& args) { return handleGetActiveVoices(args); };
    commandHandlers_["get_voice_node_virtual_states"] = [this](const QJsonObject& args) { return handleGetVoiceNodeVirtualStates(args); };
    commandHandlers_["get_bus_topology_plan_count"] = [this](const QJsonObject& args) { return handleGetBusTopologyPlanCount(args); };
    commandHandlers_["get_voice_metadata_chain"] = [this](const QJsonObject& args) { return handleGetVoiceMetadataChain(args); };
    commandHandlers_["get_voice_contribution"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!engine_)
            return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        uint32_t voiceId = static_cast<uint32_t>(args["voice_id"].toDouble());
        Bridge::VoiceContributionData contrib;
        if (Bridge::voiceContribution(voiceId, contrib) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("VOICE_NOT_FOUND",
                                 QString("Voice %1 not found").arg(voiceId));
        return successResponse(QJsonObject{
            {"voice_id",                  static_cast<qint64>(voiceId)},
            {"base_volume_db",            static_cast<double>(contrib.baseVolumeDb)},
            {"variable_volume_db",        static_cast<double>(contrib.variableVolumeDb)},
            {"action_prop_volume_db",     static_cast<double>(contrib.actionPropVolumeDb)},
            {"state_volume_db",           static_cast<double>(contrib.stateVolumeDb)},
            {"modulator_volume_db",       static_cast<double>(contrib.modulatorVolumeDb)},
            {"spatial_volume_db",         static_cast<double>(contrib.spatialVolumeDb)},
            {"bus_hierarchy_volume_db",   static_cast<double>(contrib.busHierarchyVolumeDb)},
            {"fade_volume_db",            static_cast<double>(contrib.fadeVolumeDb)},
            {"final_volume_db",           static_cast<double>(contrib.finalVolumeDb)},
            {"base_pitch",                static_cast<double>(contrib.basePitch)},
            {"variable_pitch",            static_cast<double>(contrib.variablePitch)},
            {"action_prop_pitch",         static_cast<double>(contrib.actionPropPitch)},
            {"state_pitch",               static_cast<double>(contrib.statePitch)},
            {"modulator_pitch",           static_cast<double>(contrib.modulatorPitch)},
            {"bus_hierarchy_pitch",       static_cast<double>(contrib.busHierarchyPitch)},
            {"final_pitch",               static_cast<double>(contrib.finalPitch)},
            {"variable_lpf",              static_cast<double>(contrib.variableLpf)},
            {"state_lpf",                 static_cast<double>(contrib.stateLpf)},
            {"final_lpf",                 static_cast<double>(contrib.finalLpf)},
            {"variable_hpf",              static_cast<double>(contrib.variableHpf)},
            {"state_hpf",                 static_cast<double>(contrib.stateHpf)},
            {"final_hpf",                 static_cast<double>(contrib.finalHpf)}
        });
    };
    commandHandlers_["get_voice_path_latency"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!engine_)
            return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        uint32_t voiceId = static_cast<uint32_t>(args["voice_id"].toDouble());
        if (voiceId == 0)
            return errorResponse("INVALID_ARGS", "voice_id is required (non-zero)");
        Bridge::VoicePathLatencyData latency;
        uint32_t rawResult = 0;
        const auto status = Bridge::voicePathLatency(voiceId, latency, rawResult);
        if (status == Bridge::IpcQueryStatus::NotFound)
        {
            return successResponse(QJsonObject{
                {"voice_id",          static_cast<qint64>(voiceId)},
                {"found",             false},
                {"insert_fx_samples", 0},
                {"bus_fx_samples",    0},
                {"total_samples",     0}
            });
        }
        if (status != Bridge::IpcQueryStatus::Ok)
            return errorResponse("QUERY_FAILED",
                                 QString("voice path latency query failed with code %1").arg(rawResult));
        return successResponse(QJsonObject{
            {"voice_id",          static_cast<qint64>(latency.voiceId)},
            {"found",             true},
            {"insert_fx_samples", static_cast<qint64>(latency.insertFxSamples)},
            {"bus_fx_samples",    static_cast<qint64>(latency.busFxSamples)},
            {"total_samples",     static_cast<qint64>(latency.totalSamples)}
        });
    };
    commandHandlers_["get_bus_info"]        = [this](const QJsonObject& args) { return handleGetBusInfo(args); };
    commandHandlers_["get_performance"]     = [this](const QJsonObject& args) { return handleGetPerformance(args); };

    commandHandlers_["get_session_recap"] = [this](const QJsonObject&) {
        if (!engine_)
            return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        AudioEngine::SessionRecapData recap;
        if (!engine_->getSessionRecap(recap))
            return errorResponse("COLLECT_FAILED", "Failed to collect session recap");
        QJsonObject result;
        result["peak_cpu_percent"]    = static_cast<double>(recap.peakCpuPercent);
        result["peak_active_voices"]  = static_cast<int>(recap.peakActiveVoices);
        result["peak_virtual_voices"] = static_cast<int>(recap.peakVirtualVoices);
        result["peak_total_voices"]   = static_cast<int>(recap.peakTotalVoices);
        result["peak_active_buses"]   = static_cast<int>(recap.peakActiveBuses);
        result["completed_and_active_voice_count"] = static_cast<int>(recap.completedAndActiveVoiceCount);
        result["total_underruns"]     = static_cast<int>(recap.totalUnderruns);
        result["total_frames"]        = static_cast<int>(recap.totalFrames);
        result["avg_cpu_percent"]     = static_cast<double>(recap.avgCpuPercent);
        result["avg_active_voices"]   = static_cast<double>(recap.avgActiveVoices);
        result["session_duration_sec"] = recap.sessionDurationSec;
        result["active"]              = recap.active;
        return successResponse(result);
    };

    commandHandlers_["get_cpu_budget_state"]    = [this](const QJsonObject& args) { return handleGetCpuBudgetState(args); };
    commandHandlers_["get_memory_budget_status"]   = [this](const QJsonObject& args) { return handleGetMemoryBudgetStatus(args); };
    commandHandlers_["get_listener_output_bus"]     = [this](const QJsonObject& args) { return handleGetListenerOutputBus(args); };
    commandHandlers_["get_render_plan"]          = [this](const QJsonObject& args) { return handleGetRenderPlan(args); };
    commandHandlers_["get_job_stats"]            = [this](const QJsonObject& args) { return handleGetJobStats(args); };
    commandHandlers_["get_object_audio_status"]   = [this](const QJsonObject& args) { return handleGetObjectAudioStatus(args); };

    commandHandlers_["get_device_capabilities"] = [this](const QJsonObject& args) {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        int32_t devIdx = args.contains("device_index") ? args["device_index"].toInt(-1) : -1;
        Bridge::DeviceCapabilityData info;
        const auto status = Bridge::deviceCapabilities(devIdx, info);
        if (status == Bridge::IpcQueryStatus::NoEngine)
            return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
        if (status == Bridge::IpcQueryStatus::NoManager)
            return errorResponse("NO_OUTPUT_MANAGER", "OutputManager not available");
        if (status != Bridge::IpcQueryStatus::Ok)
            return errorResponse("QUERY_FAILED", "Failed to query device capabilities");

        QJsonArray rates;
        for (uint32_t rate : info.supportedRates)
            rates.append(static_cast<int>(rate));

        return successResponse(QJsonObject{
            {"device_name", info.name},
            {"device_index", static_cast<int>(info.deviceIndex)},
            {"native_sample_rate", static_cast<int>(info.nativeSampleRate)},
            {"native_channels", static_cast<int>(info.nativeChannels)},
            {"supports_exclusive", info.supportsExclusive},
            {"supports_spatial", info.supportsSpatial},
            {"is_available", info.isAvailable},
            {"supported_sample_rates", rates}
        });
    };

    commandHandlers_["get_active_format"] = [this](const QJsonObject& args) {
        (void)args;
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        Bridge::ActiveFormatData fmt;
        const auto status = Bridge::activeOutputFormat(fmt);
        if (status == Bridge::IpcQueryStatus::NoEngine)
            return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
        if (status == Bridge::IpcQueryStatus::NoManager)
            return errorResponse("NO_OUTPUT_MANAGER", "OutputManager not available");
        if (status != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NO_DEVICE", "No active output device");
        return successResponse(QJsonObject{
            {"sample_rate", fmt.sampleRate},
            {"buffer_size", fmt.bufferSize},
            {"latency_ms", fmt.latencyMs},
            {"state", fmt.state},
            {"device_count", static_cast<int>(fmt.deviceCount)}
        });
    };

    commandHandlers_["get_hotplug_events"] = [this](const QJsonObject& args) {
        (void)args;
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        uint32_t eventCount = 0, deviceCount = 0;
        const auto status = Bridge::hotplugCounts(eventCount, deviceCount);
        if (status == Bridge::IpcQueryStatus::NoEngine)
            return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
        if (status != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NO_OUTPUT_MANAGER", "OutputManager not available");
        return successResponse(QJsonObject{
            {"hotplug_event_count", static_cast<int>(eventCount)},
            {"device_count", static_cast<int>(deviceCount)}
        });
    };


    commandHandlers_["get_current_layout"] = [this](const QJsonObject& args) { return handleGetCurrentLayout(args); };
    commandHandlers_["get_current_view"] = [this](const QJsonObject& args) {
        qWarning() << "[deprecated] get_current_view, use get_current_layout";
        return handleGetCurrentView(args);
    };
    commandHandlers_["get_layout_state"] = [this](const QJsonObject& args) { return handleGetLayoutState(args); };
    commandHandlers_["get_inspector_context"] = [this](const QJsonObject& args) { return handleGetInspectorContext(args); };

    commandHandlers_["get_inspector_schema"] = [this](const QJsonObject& args) -> QJsonObject {
        QJsonObject errOut;
        auto* ip = resolveInspectorForRouting(args, errOut);
        if (!ip) {
            if (!errOut.isEmpty()) return errOut;
            return errorResponse("NO_INSPECTOR", "InspectorPanel not available");
        }
        QString schemaId = args["schema_id"].toString();
        if (schemaId.isEmpty()) {
            return errorResponse("MISSING_PARAM", "Required: schema_id");
        }
        auto schema = ip->getInspectorSchema(schemaId);
        if (schema.isEmpty()) {
            return errorResponse("SCHEMA_NOT_FOUND",
                QString("No schema found for '%1'").arg(schemaId));
        }
        return successResponse(QJsonObject{{"schema_id", schemaId}, {"schema", schema}});
    };

    commandHandlers_["create_inspector_panel"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

        const QString path = args.value("path").toString();
        int panelId = 0;
        if (!path.isEmpty()) {
            auto* project = ProjectManager::instance().currentProject();
            if (!project) return errorResponse("NO_PROJECT", "No project open");
            auto* item = findItemByPathInProject(project, path);
            if (!item) return errorResponse("NOT_FOUND", "Object not found: " + path);
            mainWindow_->openItemInNewInspector(item, MainWindow::PreferredOpenArea::CentralTab);
            panelId = mainWindow_->lastFocusedInspectorPanelId();
        } else {
            auto* panel = mainWindow_->createInspectorPanel();
            if (!panel) return errorResponse("CREATE_FAILED", "Failed to create inspector panel");
            panelId = panel->panelId();
        }

        auto* ip = mainWindow_->findInspectorPanel(panelId);
        return successResponse(QJsonObject{
            {"panel_id", panelId},
            {"type", ip ? ip->contextType() : QString()},
            {"name", ip ? ip->contextName() : QString()},
            {"pinned", ip ? ip->isPinned() : false}
        });
    };

    commandHandlers_["create_editor"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

        const QString want = args.value("type").toString().trimmed();
        if (want.isEmpty())
            return errorResponse("INVALID_ARGS", "type is required",
                "Pass a creatable editor type string or label, e.g. \"ExternalEditor\" or \"External Editor\".");

        QString resolved;
        QStringList available;
        for (const QString& t : mainWindow_->workspacePanelTypes()) {
            const QString label = mainWindow_->workspacePanelTitle(t);
            if (label.isEmpty()) continue;
            available << t;
            if (t.compare(want, Qt::CaseInsensitive) == 0 ||
                label.compare(want, Qt::CaseInsensitive) == 0) {
                resolved = t;
                break;
            }
        }
        if (resolved.isEmpty())
            return errorResponse("UNKNOWN_EDITOR_TYPE",
                QStringLiteral("No creatable editor type '%1'").arg(want),
                QStringLiteral("Available: %1").arg(available.join(QStringLiteral(", "))));

        const QList<int> before = mainWindow_->workspacePanelInstanceIds(resolved);

        if (!mainWindow_->createWorkspacePanel(resolved))
            return errorResponse("CREATE_FAILED",
                QStringLiteral("Failed to create editor type '%1'").arg(resolved));

        int newId = -1;
        for (const int id : mainWindow_->workspacePanelInstanceIds(resolved)) {
            if (!before.contains(id)) { newId = id; break; }
        }
        if (newId < 0)
            newId = mainWindow_->lastFocusedWorkspacePanelId(resolved);

        QString dockObjectName;
        if (QWidget* panel = mainWindow_->workspacePanelContent(resolved, newId)) {
            if (auto* dock = mainWindow_->dockOfPanel(panel))
                dockObjectName = dock->objectName();
        }

        return successResponse(QJsonObject{
            {"success", true},
            {"type", resolved},
            {"panel_id", newId},
            {"dock_object_name", dockObjectName}
        });
    };

    commandHandlers_["set_inspector_pinned"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
        if (!args.contains("panel_id"))
            return errorResponse("INVALID_ARGS", "panel_id required");
        const int panelId = args["panel_id"].toInt();
        const bool pinned = args.value("pinned").toBool(true);

        auto* ip = mainWindow_->findInspectorPanel(panelId);
        if (!ip)
            return errorResponse("PANEL_NOT_FOUND",
                QStringLiteral("No Inspector panel with panel_id=%1").arg(panelId));

        ip->setPinned(pinned);
        return successResponse(QJsonObject{{"panel_id", panelId}, {"pinned", ip->isPinned()}});
    };

    commandHandlers_["list_inspector_panels"] = [this](const QJsonObject&) -> QJsonObject {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
        QJsonArray panels;
        for (InspectorPanel* ip : mainWindow_->allInspectorPanels()) {
            if (!ip) continue;
            panels.append(QJsonObject{
                {"panel_id", ip->panelId()},
                {"pinned", ip->isPinned()},
                {"type", ip->contextType()},
                {"name", ip->contextName()}
            });
        }
        return successResponse(QJsonObject{{"panels", panels}, {"count", panels.size()}});
    };

    commandHandlers_["close_inspector_panel"] = [this](const QJsonObject& args) -> QJsonObject {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
        if (!args.contains("panel_id"))
            return errorResponse("INVALID_ARGS", "panel_id required");
        const int panelId = args["panel_id"].toInt();
        QMetaObject::invokeMethod(mainWindow_, [this, panelId]() {
            mainWindow_->closeInspectorPanel(panelId);
        }, Qt::QueuedConnection);
        return successResponse(QJsonObject{{"panel_id", panelId}});
    };


    commandHandlers_["get_recent_logs"] = [this](const QJsonObject& args) { return handleGetRecentLogs(args); };
    commandHandlers_["list_named_layouts"] = [this](const QJsonObject& args) { return handleListNamedLayouts(args); };
    commandHandlers_["list_panels"] = [this](const QJsonObject& args) { return handleListPanels(args); };

    commandHandlers_["get_app_state"] = [this](const QJsonObject& args) { return handleGetAppState(args); };

    commandHandlers_["get_stream_status"] = [this](const QJsonObject& args) { return handleGetStreamStatus(args); };
    commandHandlers_["get_stream_position"] = [this](const QJsonObject& args) { return handleGetStreamPosition(args); };

    commandHandlers_["list_codecs"] = [this](const QJsonObject& args) { return handleListCodecs(args); };
    commandHandlers_["get_codec_info"] = [this](const QJsonObject& args) { return handleGetCodecInfo(args); };
    commandHandlers_["get_stream_device_info"] = [this](const QJsonObject& args) { return handleGetStreamDeviceInfo(args); };
    commandHandlers_["get_stream_device_count"] = [this](const QJsonObject& args) { return handleGetStreamDeviceCount(args); };

    commandHandlers_["list_modulators"] = [this](const QJsonObject& args) { return handleListModulators(args); };

    commandHandlers_["list_modulator_chains"] = [this](const QJsonObject& args) { return handleListModulatorChains(args); };


    commandHandlers_["get_modulator_presets"] = [this](const QJsonObject& args) { return handleGetModulatorPresets(args); };

    commandHandlers_["get_buffer_config"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        int32_t blockSize = engine_->getBufferSize();
        int32_t sr = Bridge::mixerSampleRate();
        return successResponse(QJsonObject{
            {"block_size", blockSize},
            {"sample_rate", sr}
        });
    };

    commandHandlers_["get_stream_throughput"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        Bridge::StreamThroughputData info;
        if (Bridge::streamThroughput(info) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NOT_INIT", "HarkenSystem not available");
        return successResponse(QJsonObject{
            {"avg_throughput_bps", info.avgThroughputBps},
            {"adaptive_resize_count", static_cast<int>(info.adaptiveResizeCount)},
            {"adaptive_enabled", info.adaptiveEnabled},
            {"target_latency_ms", static_cast<int>(info.targetLatencyMs)}
        });
    };

    commandHandlers_["get_mmap_status"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        Bridge::MmapStatusData info;
        if (Bridge::mmapStatus(info) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NOT_INIT", "HarkenSystem not available");
        return successResponse(QJsonObject{
            {"enabled", info.enabled},
            {"total_attempts", static_cast<int>(info.totalAttempts)},
            {"successes", static_cast<int>(info.successes)},
            {"fallbacks", static_cast<int>(info.fallbacks)},
            {"last_error_code", static_cast<int>(info.lastErrorCode)}
        });
    };

    commandHandlers_["get_device_fairness"] = [this](const QJsonObject&) -> QJsonObject {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");
        Bridge::DeviceFairnessData info;
        if (Bridge::deviceFairness(info) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NOT_INIT", "HarkenSystem not available");
        QJsonArray counts;
        for (uint32_t i = 0; i < 8; ++i)
            counts.append(static_cast<int>(info.activeCounts[i]));
        return successResponse(QJsonObject{
            {"device_active_counts", counts},
            {"allocated_devices", static_cast<int>(info.allocatedDevices)},
            {"route_count", static_cast<int>(info.routeCount)}
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
        {"output_latency_ms", engine_->outputLatency()},
        {"sample_rate", engine_->actualSampleRate()},
        {"master_volume", engine_->masterVolume()}
    };

    AudioEngine::VoiceStateBreakdown sb;
    if (engine_->getVoiceStateBreakdown(sb)) {
        result["voice_states"] = QJsonObject{
            {"idle", sb.idle},
            {"starting", sb.starting},
            {"running", sb.running},
            {"virtual", sb.virtualState},
            {"releasing", sb.releasing},
            {"stopping", sb.stopping},
            {"paused", sb.paused}
        };
    }

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
                             "Call 'init' command first.");
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

    if (mainWindow_) {
        auto* tm = mainWindow_->auditionManager();
        if (tm)
            tm->stopAll(fadeMs);
    }
    
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
    
    QVector3D pos(posArray.at(0).toDouble(), posArray.at(1).toDouble(), posArray.at(2).toDouble());
    QVector3D forward(
        fwdArray.size() > 0 ? fwdArray.at(0).toDouble() : 0.0,
        fwdArray.size() > 1 ? fwdArray.at(1).toDouble() : 0.0,
        fwdArray.size() > 2 ? fwdArray.at(2).toDouble() : -1.0);
    QVector3D up(
        upArray.size() > 0 ? upArray.at(0).toDouble() : 0.0,
        upArray.size() > 1 ? upArray.at(1).toDouble() : 1.0,
        upArray.size() > 2 ? upArray.at(2).toDouble() : 0.0);
    
    engine_->setListenerTransform(pos, forward, up);
    
    return successResponse(QJsonObject{
        {"position", QJsonArray{pos.x(), pos.y(), pos.z()}},
        {"forward", QJsonArray{forward.x(), forward.y(), forward.z()}},
        {"up", QJsonArray{up.x(), up.y(), up.z()}}
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
    
    QVector3D listenerPos = engine_->listenerPosition();
    objects.append(QJsonObject{
        {"type", "listener"},
        {"position", QJsonArray{listenerPos.x(), listenerPos.y(), listenerPos.z()}}
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
    {
        uint32_t count = engine_->getLoadedPackCount();
        if (count > 0)
        {
            QJsonObject info;
            info["count"] = static_cast<int>(count);
            packArray.append(info);
        }
    }

    return successResponse(QJsonObject{
        {"packs", packArray},
        {"count", static_cast<int>(packArray.size())}
    });
}


QJsonObject IPCServer::handleGetPackMetrics(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    QString packName = args["pack_name"].toString();

    if (!packName.isEmpty())
    {
        Bridge::PackMetricsData m;
        if (Bridge::packMetrics(packName, m) != Bridge::IpcQueryStatus::Ok)
            return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");
        QJsonObject obj;
        obj["pack_name"] = packName;
        obj["load_time_us"] = static_cast<qint64>(m.loadTimeUs);
        obj["parse_time_us"] = static_cast<qint64>(m.parseTimeUs);
        obj["io_time_us"] = static_cast<qint64>(m.ioTimeUs);
        obj["load_timestamp"] = static_cast<qint64>(m.loadTimestamp);
        obj["last_access_time"] = static_cast<qint64>(m.lastAccessTime);
        obj["peak_memory_bytes"] = static_cast<qint64>(m.peakMemoryBytes);
        obj["memory_usage"] = static_cast<qint64>(m.memoryUsage);
        obj["object_count"] = static_cast<int>(m.objectCount);
        obj["media_count"] = static_cast<int>(m.mediaCount);
        obj["streaming_media_count"] = static_cast<int>(m.streamingMediaCount);
        obj["in_memory_media_count"] = static_cast<int>(m.inMemoryMediaCount);
        obj["is_lazy_loaded"] = m.isLazyLoaded;
        obj["active_voices"] = static_cast<int>(m.activeVoices);
        return successResponse(obj);
    }

    std::vector<Bridge::PackMetricsSummary> allMetrics;
    if (Bridge::allPackMetrics(allMetrics) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");
    QJsonArray arr;
    for (const auto& m : allMetrics)
    {
        QJsonObject obj;
        obj["pack_name"] = m.packName;
        obj["load_time_us"] = static_cast<qint64>(m.loadTimeUs);
        obj["parse_time_us"] = static_cast<qint64>(m.parseTimeUs);
        obj["io_time_us"] = static_cast<qint64>(m.ioTimeUs);
        obj["peak_memory_bytes"] = static_cast<qint64>(m.peakMemoryBytes);
        obj["load_timestamp"] = static_cast<qint64>(m.loadTimestamp);
        arr.append(obj);
    }
    return successResponse(QJsonObject{{"packs", arr}, {"count", static_cast<int>(arr.size())}});
}

QJsonObject IPCServer::handleGetPackLoadHistory(const QJsonObject& )
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    std::vector<Bridge::PackLoadHistoryEntry> history;
    if (Bridge::packLoadHistory(history) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");

    QJsonArray arr;
    for (const auto& h : history)
    {
        QJsonObject obj;
        obj["pack_name"] = h.packName;
        obj["timestamp"] = static_cast<qint64>(h.timestamp);
        obj["load_time_us"] = static_cast<qint64>(h.loadTimeUs);
        obj["memory_size_bytes"] = static_cast<qint64>(h.memorySizeBytes);
        obj["status"] = h.status;
        arr.append(obj);
    }
    return successResponse(QJsonObject{{"history", arr}, {"count", static_cast<int>(arr.size())}});
}


QJsonObject IPCServer::handleGetActionRouting(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    QString actionName = args["action_name"].toString();
    if (actionName.isEmpty())
        return errorResponse("INVALID_ARGS", "action_name is required");

    uint32_t nameHash = HarkenEditor::hashString(actionName.toUtf8().constData());

    std::vector<Bridge::ActionRoutingEntry> entries;
    if (Bridge::actionRouting(nameHash, entries) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");

    QJsonArray arr;
    for (const auto& re : entries)
    {
        QJsonObject obj;
        obj["pack_id"] = static_cast<int>(re.packId);
        obj["language_id"] = re.languageId;
        obj["priority"] = re.priority;
        obj["is_voice_pack"] = re.isVoicePack;
        if (re.packNameKnown)
            obj["pack_name"] = re.packName;
        arr.append(obj);
    }
    return successResponse(QJsonObject{
        {"action_name", actionName},
        {"action_name_hash", static_cast<qint64>(nameHash)},
        {"routing_entries", arr},
        {"count", static_cast<int>(arr.size())}
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

QJsonObject IPCServer::handleResolveActionPacks(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    QString actionName = args["action_name"].toString();
    if (actionName.isEmpty())
        return errorResponse("INVALID_ARGS", "action_name is required");

    uint32_t nameHash = HarkenEditor::hashString(actionName.toUtf8().constData());

    uint32_t packID = 0;
    QString resolvedPack;
    if (Bridge::resolveActionPack(nameHash, packID, resolvedPack) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");

    QJsonObject result;
    result["action_name"] = actionName;
    result["action_name_hash"] = static_cast<qint64>(nameHash);
    result["resolved_pack_id"] = static_cast<int>(packID);
    result["resolved_pack_name"] = resolvedPack;
    result["found"] = (packID != 0);
    return successResponse(result);
}

QJsonObject IPCServer::handleValidateMultipackCollisions(const QJsonObject& )
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    std::vector<Bridge::PackCollisionEntry> collisions;
    if (Bridge::multiPackCollisions(collisions) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("ENGINE_NOT_AVAILABLE", "PackManager not available");

    QJsonArray arr;
    for (const auto& c : collisions)
    {
        QJsonObject obj;
        obj["action_name_hash"] = static_cast<qint64>(c.actionNameHash);
        QJsonArray packs;
        for (const auto& bn : c.packNames)
            packs.append(bn);
        obj["pack_names"] = packs;
        obj["pack_count"] = static_cast<int>(c.packNames.size());
        arr.append(obj);
    }
    return successResponse(QJsonObject{
        {"collisions", arr},
        {"collision_count", static_cast<int>(arr.size())}
    });
}


QJsonObject IPCServer::handleGetCodecCapabilities(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    int formatId = args["format_id"].toInt(-1);
    if (formatId < 0)
        return errorResponse("INVALID_ARGS", "format_id is required");

    Bridge::CodecDesc info;
    if (Bridge::codecInfo(static_cast<uint32_t>(formatId), info) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_FOUND",
            QString("No codec registered for format_id %1").arg(formatId));

    QJsonObject obj;
    obj["format_id"] = static_cast<int>(info.formatId);
    obj["name"] = info.name;
    obj["file_extension"] = info.fileExtension;
    obj["version"] = static_cast<int>(info.version);
    obj["preferred_buffer_frames"] = static_cast<int>(info.preferredBufferFrames);
    obj["min_buffer_frames"] = static_cast<int>(info.minBufferFrames);
    obj["max_buffer_frames"] = static_cast<int>(info.maxBufferFrames);
    obj["max_channels"] = static_cast<int>(info.maxChannels);
    obj["capabilities"] = static_cast<int>(info.capabilities);
    obj["active_streams"] = static_cast<int>(info.activeStreams);
    obj["supported_bit_depths"] = static_cast<int>(info.supportedBitDepths);
    obj["channel_layout_mask"] = static_cast<int>(info.channelLayoutMask);
    obj["optimal_concurrent_streams"] = static_cast<int>(info.optimalConcurrentStreams);
    obj["estimated_cpu_cost_per_frame"] = info.estimatedCpuCostPerFrame;
    obj["codec_flags"] = static_cast<int>(info.codecFlags);

    QJsonArray rates;
    for (uint32_t rate : info.supportedSampleRates)
        rates.append(static_cast<int>(rate));
    obj["supported_sample_rates"] = rates;

    return successResponse(obj);
}


QJsonObject IPCServer::handleGetPanelRect(const QJsonObject& args)
{
    QString panelName = args["name"].toString();
    
    {
        QWidget* panel = nullptr;
        
        if ((panelName == "blueprint_canvas" || panelName == "node_graph") && mainWindow_)
            panel = qobject_cast<QWidget*>(mainWindow_->blueprintCanvas());
        
        if (panel && panel->isVisible())
        {
            QRect globalRect = QRect(panel->mapToGlobal(QPoint(0, 0)), panel->size());
            return successResponse(QJsonObject{
                {"x", globalRect.x()},
                {"y", globalRect.y()},
                {"width", globalRect.width()},
                {"height", globalRect.height()}
            });
        }
    }
    
    QMap<QString, QJsonObject> defaultRects = {
        {"3d_preview", QJsonObject{{"x", 400}, {"y", 100}, {"width", 800}, {"height", 600}}},
        {"node_graph", QJsonObject{{"x", 400}, {"y", 100}, {"width", 800}, {"height", 600}}},
        {"inspector", QJsonObject{{"x", 1400}, {"y", 100}, {"width", 520}, {"height", 600}}},
        {"explorer", QJsonObject{{"x", 0}, {"y", 100}, {"width", 400}, {"height", 600}}},
        {"console", QJsonObject{{"x", 0}, {"y", 900}, {"width", 1920}, {"height", 180}}}
    };
    
    if (defaultRects.contains(panelName))
    {
        return successResponse(defaultRects[panelName]);
    }
    else
    {
        return errorResponse("UNKNOWN_PANEL", 
                             QString("Unknown panel: %1").arg(panelName),
                             "Valid panels: 3d_preview, node_graph, inspector, explorer, console");
    }
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
        state["is_dirty"] = false;
        
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


QJsonObject IPCServer::handleSwitchView(const QJsonObject& args)
{
    QString viewName = args["view"].toString();
    
    if (viewName.isEmpty())
    {
        return errorResponse("INVALID_ARGS",
                             "View name is required",
                             QStringLiteral("Valid views: %1")
                                 .arg(BuiltinLayouts::ids().join(QStringLiteral(", "))));
    }

    const BuiltinLayouts::Entry* builtin = BuiltinLayouts::findByToken(viewName);
    if (!builtin)
    {
        return errorResponse("INVALID_VIEW",
                             QString("Unknown view: %1").arg(viewName),
                             QStringLiteral("Valid views: %1")
                                 .arg(BuiltinLayouts::ids().join(QStringLiteral(", "))));
    }
    viewName = builtin->id;
    
    if (!mainWindow_)
    {
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE", 
                             "MainWindow is not available",
                             "Ensure IPCServer.setMainWindow() is called.");
    }
    
    bool success = mainWindow_->switchToView(viewName);

    if (success)
    {
        return successResponse(QJsonObject{
            {"view", viewName},
            {"switched", true}
        });
    }
    else
    {
        return errorResponse("SWITCH_VIEW_FAILED", 
                             QString("Failed to switch to view: %1").arg(viewName));
    }
}


QJsonObject IPCServer::handleGetCurrentView(const QJsonObject& args)
{
    Q_UNUSED(args);

    if (!mainWindow_)
    {
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE",
                             "MainWindow is not available",
                             "Ensure IPCServer.setMainWindow() is called.");
    }

    QString currentView = mainWindow_->currentView();

    return successResponse(QJsonObject{
        {"view", currentView}
    });
}


QJsonObject IPCServer::handleGetCurrentLayout(const QJsonObject& args)
{
    Q_UNUSED(args);

    if (!mainWindow_)
    {
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE",
                             "MainWindow is not available",
                             "Ensure IPCServer.setMainWindow() is called.");
    }

    QString currentLayout = mainWindow_->currentView();

    return successResponse(QJsonObject{
        {"layout", currentLayout}
    });
}


QJsonObject IPCServer::handleGetLayoutState(const QJsonObject& args)
{
    Q_UNUSED(args);

    if (!mainWindow_)
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE",
                             "MainWindow is not available");

    QString currentView = mainWindow_->currentView();

    QJsonObject layouts;
    for (const auto& item : BuiltinLayouts::describeLayoutState(currentView)) {
        QJsonObject info;
        info["index"] = item.index;
        info["is_current"] = item.isCurrent;
        layouts[item.name] = info;
    }

    return successResponse(QJsonObject{
        {"current_layout", currentView},
        {"layouts", layouts}
    });
}

QJsonObject IPCServer::handleResetLayout(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE",
                             "MainWindow is not available");

    const auto resolved = BuiltinLayouts::resolveResetTarget(
        args["view"].toString(), mainWindow_->currentView());
    if (resolved.kind == BuiltinLayouts::ResetTargetKind::Invalid)
        return errorResponse("INVALID_VIEW",
                             QString("Unknown view: %1").arg(resolved.canonical),
                             QStringLiteral("Valid views: %1")
                                 .arg(BuiltinLayouts::ids().join(QStringLiteral(", "))));

    if (resolved.kind == BuiltinLayouts::ResetTargetKind::Builtin
        && resolved.canonical != mainWindow_->currentView())
        mainWindow_->switchToView(resolved.canonical);

    if (!mainWindow_->resetCurrentLayout())
        return errorResponse("RESET_FAILED",
                             QString("Failed to reset layout: %1").arg(resolved.canonical));

    return successResponse(QJsonObject{
        {"view", resolved.canonical},
        {"reset", true}
    });
}

InspectorPanel* IPCServer::resolveInspectorForRouting(const QJsonObject& args, QJsonObject& errOut)
{
    if (args.contains("panel_id")) {
        const int id = args.value("panel_id").toInt(-1);
        InspectorPanel* panel = mainWindow_ ? mainWindow_->findInspectorPanel(id) : nullptr;
        if (panel)
            return panel;
        errOut = errorResponse("PANEL_NOT_FOUND",
            QStringLiteral("No Inspector panel with panel_id=%1").arg(id));
        return nullptr;
    }
    return mainWindow_ ? mainWindow_->activeInspectorPanel() : nullptr;
}

QJsonObject IPCServer::handleGetInspectorContext(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("INSPECTOR_NOT_AVAILABLE",
                             "InspectorPanel is not available");

    QJsonObject errOut;
    auto* ip = resolveInspectorForRouting(args, errOut);
    if (!ip) {
        if (!errOut.isEmpty()) return errOut;
        return errorResponse("INSPECTOR_NOT_AVAILABLE",
                             "InspectorPanel is not available");
    }

    QJsonObject result;
    result["type"] = ip->contextType();
    result["name"] = ip->contextName();
    return successResponse(result);
}

QJsonObject IPCServer::handleSaveNamedLayout(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE", "MainWindow not available");

    QString name = args["name"].toString().trimmed();
    if (name.isEmpty())
        return errorResponse("INVALID_ARGS", "name is required");

    QString filePath;
    if (!mainWindow_->saveNamedLayout(name, &filePath))
        return errorResponse("FILE_WRITE_ERROR", "Cannot write layout file");

    return successResponse(QJsonObject{{"name", name}, {"path", filePath}});
}

QJsonObject IPCServer::handleLoadNamedLayout(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE", "MainWindow not available");

    QString name = args["name"].toString().trimmed();
    if (name.isEmpty())
        return errorResponse("INVALID_ARGS", "name is required");

    if (!mainWindow_->loadNamedLayout(name))
        return errorResponse("LAYOUT_NOT_FOUND", QString("Named layout '%1' not found").arg(name));

    return successResponse(QJsonObject{{"name", name}, {"loaded", true}});
}

QJsonObject IPCServer::handleListNamedLayouts(const QJsonObject& )
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/layouts";
    QDir d(dir);
    QJsonArray layouts;
    for (const auto& entry : d.entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonObject info;
        info["name"] = json["name"].toString();
        info["saved_at"] = json["savedAt"].toString();
        layouts.append(info);
    }
    return successResponse(QJsonObject{{"layouts", layouts}, {"count", layouts.size()}});
}

QJsonObject IPCServer::handleDeleteNamedLayout(const QJsonObject& args)
{
    QString name = args["name"].toString().trimmed();
    if (name.isEmpty())
        return errorResponse("INVALID_ARGS", "name is required");

    if (mainWindow_ && mainWindow_->deleteNamedLayout(name))
        return successResponse(QJsonObject{{"name", name}, {"deleted", true}});

    return errorResponse("LAYOUT_NOT_FOUND", QString("Named layout '%1' not found").arg(name));
}

QJsonObject IPCServer::handleListPanels(const QJsonObject& )
{
    if (!mainWindow_)
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE", "MainWindow not available");

    QJsonArray panels;
    for (const QVariantMap& summary : mainWindow_->workspaceWindowSummaries())
        panels.append(QJsonObject::fromVariantMap(summary));
    return successResponse(QJsonObject{{"panels", panels}, {"count", panels.size()}});
}

QJsonObject IPCServer::handleTogglePanel(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE", "MainWindow not available");

    QString panelName = args["name"].toString().trimmed();
    if (panelName.isEmpty())
        return errorResponse("INVALID_ARGS", "name is required (panel objectName)");

    QVariantMap current;
    for (const QVariantMap& summary : mainWindow_->workspaceWindowSummaries()) {
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
        return errorResponse("PANEL_NOT_FOUND", QString("Panel '%1' not found").arg(panelName));

    bool newState;
    if (args.contains("visible"))
        newState = args["visible"].toBool();
    else
        newState = !current.value(QStringLiteral("visible")).toBool();

    QVariantMap updated;
    if (!mainWindow_->setWorkspaceWindowVisible(panelName, newState, &updated))
        return errorResponse("PANEL_TOGGLE_FAILED",
            QString("Failed to set panel '%1' visible=%2").arg(panelName).arg(newState));

    return successResponse(QJsonObject::fromVariantMap(updated));
}

QJsonObject IPCServer::handleGetRecentLogs(const QJsonObject& args)
{
    if (!mainWindow_)
    {
        return errorResponse("MAIN_WINDOW_NOT_AVAILABLE",
                             "MainWindow is not available");
    }

    int count = args["count"].toInt(100);
    QString levelFilter = args["level"].toString();

    QString logs = mainWindow_->getRecentLogs(count, levelFilter);

    return successResponse(QJsonObject{
        {"logs", logs},
        {"count", count},
        {"level_filter", levelFilter.isEmpty() ? "all" : levelFilter}
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
        perfJson["max_voices"]     = static_cast<int>(perf.maxVoices);
        perfJson["active_buses"]   = static_cast<int>(perf.activeBuses);
        perfJson["underrun_count"] = static_cast<int>(perf.underrunCount);
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
            vj["object_id"]    = Bridge::jsonIntIfAvailable(v.objectIDAvailable, static_cast<int>(v.objectID));
            vj["bus_id"]       = Bridge::jsonStringIfAvailable(v.busIdAvailable, objectIdString(v.busId));
            vj["priority"]     = Bridge::jsonIntIfAvailable(v.priorityAvailable, v.priority);
            vj["state"]        = Bridge::jsonIntIfAvailable(v.stateAvailable, v.state);
            vj["volume"]       = Bridge::jsonNumberIfAvailable(v.volumeAvailable, static_cast<double>(v.volume));
            vj["pitch"]        = Bridge::jsonNumberIfAvailable(v.pitchAvailable, static_cast<double>(v.pitch));
            vj["peak_level"]   = Bridge::jsonNumberIfAvailable(v.peakLevelAvailable, static_cast<double>(v.peakLevel));
            vj["playing_id"]   = Bridge::isAvailable(v.playingIDAvailable)
                ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue();
            vj["container_id"] = Bridge::jsonIntIfAvailable(v.containerIdAvailable, v.containerId);
            vj["is_muted"]     = Bridge::jsonBoolIfAvailable(v.isMutedAvailable, v.isMuted);
            vj["hdr_gain_db"]  = Bridge::jsonNumberIfAvailable(v.hdrGainDbAvailable, static_cast<double>(v.hdrGainDb));
            vj["insert_fx"]    = Bridge::jsonIntIfAvailable(v.insertEffectCountAvailable, static_cast<int>(v.insertEffectCount));
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
            bj["bus_id"]        = Bridge::jsonStringIfAvailable(b.busIdAvailable, objectIdString(b.busId));
            bj["name"]          = Bridge::jsonStringIfAvailable(b.nameAvailable, b.name);
            bj["bus_type"]      = Bridge::jsonIntIfAvailable(b.busTypeAvailable, static_cast<int>(b.busType));
            bj["volume"]        = Bridge::jsonNumberIfAvailable(b.volumeAvailable, static_cast<double>(b.volume));
            bj["peak_l"]        = Bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakL));
            bj["peak_r"]        = Bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakR));
            bj["rms_l"]         = Bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsL));
            bj["rms_r"]         = Bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsR));
            bj["voice_count"]   = Bridge::jsonIntIfAvailable(b.voiceCountAvailable, static_cast<int>(b.voiceCount));
            bj["cpu_pct"]       = Bridge::jsonNumberIfAvailable(b.cpuPctAvailable, static_cast<double>(b.cpuPct));
            bj["muted"]         = Bridge::jsonBoolIfAvailable(b.mutedAvailable, b.muted);
            bj["soloed"]        = Bridge::jsonBoolIfAvailable(b.soloedAvailable, b.soloed);
            bj["hdr_enabled"]   = Bridge::jsonBoolIfAvailable(b.hdrEnabledAvailable, b.hdrEnabled);
            bj["effect_count"]  = Bridge::jsonIntIfAvailable(b.effectCountAvailable, static_cast<int>(b.effectCount));
            if (Bridge::isAvailable(b.parentBusIdAvailable))
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
    if (!engine_->getDebugVoices(voices))
        return errorResponse("COLLECT_FAILED", "Failed to collect voice data");

    QJsonArray arr;
    for (const auto& v : voices)
    {
        QJsonObject vj;
        vj["object_id"]    = Bridge::jsonIntIfAvailable(v.objectIDAvailable, static_cast<int>(v.objectID));
        vj["bus_id"]       = Bridge::jsonStringIfAvailable(v.busIdAvailable, objectIdString(v.busId));
        vj["priority"]     = Bridge::jsonIntIfAvailable(v.priorityAvailable, v.priority);
        vj["state"]        = Bridge::jsonIntIfAvailable(v.stateAvailable, v.state);
        vj["volume"]       = Bridge::jsonNumberIfAvailable(v.volumeAvailable, static_cast<double>(v.volume));
        vj["pitch"]        = Bridge::jsonNumberIfAvailable(v.pitchAvailable, static_cast<double>(v.pitch));
        vj["peak_level"]   = Bridge::jsonNumberIfAvailable(v.peakLevelAvailable, static_cast<double>(v.peakLevel));
        vj["playing_id"]   = Bridge::isAvailable(v.playingIDAvailable)
            ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue();
        vj["container_id"]    = Bridge::jsonIntIfAvailable(v.containerIdAvailable, v.containerId);
        vj["is_muted"]        = Bridge::jsonBoolIfAvailable(v.isMutedAvailable, v.isMuted);
        vj["hdr_gain_db"]     = Bridge::jsonNumberIfAvailable(v.hdrGainDbAvailable, static_cast<double>(v.hdrGainDb));
        vj["insert_fx"]       = Bridge::jsonIntIfAvailable(v.insertEffectCountAvailable, static_cast<int>(v.insertEffectCount));
        arr.append(vj);
    }

    return successResponse(QJsonObject{
        {"voices", arr},
        {"count", static_cast<int>(voices.size())}
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
            {"voice_id",  Bridge::isAvailable(v.playingIDAvailable)
                ? QJsonValue(static_cast<qint64>(v.playingID)) : QJsonValue()},
            {"object_id", Bridge::jsonIntIfAvailable(v.objectIDAvailable, static_cast<int>(v.objectID))},
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
            {"voice_id", Bridge::isAvailable(v.playingIDAvailable)
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
        bj["bus_id"]       = Bridge::jsonStringIfAvailable(b.busIdAvailable, objectIdString(b.busId));
        bj["name"]         = Bridge::jsonStringIfAvailable(b.nameAvailable, b.name);
        bj["bus_type"]     = Bridge::jsonIntIfAvailable(b.busTypeAvailable, static_cast<int>(b.busType));
        bj["volume"]       = Bridge::jsonNumberIfAvailable(b.volumeAvailable, static_cast<double>(b.volume));
        bj["peak_l"]       = Bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakL));
        bj["peak_r"]       = Bridge::jsonNumberIfAvailable(b.peakAvailable, static_cast<double>(b.peakR));
        bj["rms_l"]        = Bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsL));
        bj["rms_r"]        = Bridge::jsonNumberIfAvailable(b.rmsAvailable, static_cast<double>(b.rmsR));
        bj["voice_count"]  = Bridge::jsonIntIfAvailable(b.voiceCountAvailable, static_cast<int>(b.voiceCount));
        bj["cpu_pct"]      = Bridge::jsonNumberIfAvailable(b.cpuPctAvailable, static_cast<double>(b.cpuPct));
        bj["muted"]        = Bridge::jsonBoolIfAvailable(b.mutedAvailable, b.muted);
        bj["soloed"]       = Bridge::jsonBoolIfAvailable(b.soloedAvailable, b.soloed);
        bj["hdr_enabled"]  = Bridge::jsonBoolIfAvailable(b.hdrEnabledAvailable, b.hdrEnabled);
        bj["effect_count"] = Bridge::jsonIntIfAvailable(b.effectCountAvailable, static_cast<int>(b.effectCount));
        if (Bridge::isAvailable(b.parentBusIdAvailable))
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
    result["max_voices"]     = static_cast<int>(perf.maxVoices);
    result["active_buses"]   = static_cast<int>(perf.activeBuses);
    result["underrun_count"] = static_cast<int>(perf.underrunCount);
    result["parameter_commit_count"] = static_cast<int>(perf.parameterCommitCount);
    result["smoothed_cpu_load_pct"] = static_cast<double>(perf.smoothedCpuLoadPct);
    result["budget_level"]   = static_cast<int>(perf.budgetLevel);

    float peakL = 0.0f, peakR = 0.0f;
    engine_->getMasterMeterPeak(peakL, peakR);
    result["master_peak_l"]  = static_cast<double>(peakL);
    result["master_peak_r"]  = static_cast<double>(peakR);
    result["output_latency_ms"] = engine_->outputLatency();
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

    const auto budgetStatus = Bridge::cpuBudgetManagerStatus();
    if (budgetStatus == Bridge::IpcQueryStatus::NoEngine)
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
    if (budgetStatus != Bridge::IpcQueryStatus::Ok)
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
    engine_->getJobStats(js);

    QJsonObject result;
    result["used_parallel"]        = js.usedParallel;
    result["active_worker_count"]  = static_cast<int>(js.activeWorkerCount);
    result["total_voices_processed"] = static_cast<int>(js.totalVoicesProcessed);
    result["total_chunks_stolen"]  = static_cast<int>(js.totalChunksStolen);
    result["max_worker_render_us"] = static_cast<double>(js.maxWorkerRenderUs);
    result["min_worker_render_us"] = static_cast<double>(js.minWorkerRenderUs);
    result["parallel_efficiency"]  = static_cast<double>(js.parallelEfficiency);
    result["chunk_size"]           = static_cast<int>(js.chunkSize);
    result["total_estimated_cost"] = static_cast<double>(js.totalEstimatedCost);
    result["active_voice_count"]   = static_cast<int>(js.activeVoiceCount);
    result["cost_threshold"]       = static_cast<double>(js.costThreshold);

    QJsonArray workersArr;
    for (uint32_t i = 0; i < js.activeWorkerCount && i < 8; ++i)
    {
        QJsonObject w;
        w["voices_processed"] = static_cast<int>(js.workers[i].voicesProcessed);
        w["chunks_stolen"]    = static_cast<int>(js.workers[i].chunksStolen);
        w["render_us"]        = static_cast<double>(js.workers[i].renderUs);
        workersArr.append(w);
    }
    result["workers"] = workersArr;

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

    std::vector<Bridge::ActiveModulatorInfo> active;
    const auto status = Bridge::listActiveModulators(active);
    if (status == Bridge::IpcQueryStatus::NoEngine)
        return errorResponse("NOT_INIT", "HarkenSystem not available");
    if (status != Bridge::IpcQueryStatus::Ok)
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

    std::vector<Bridge::ActiveModulatorChainInfo> active;
    const auto status = Bridge::listActiveModulatorChains(active);
    if (status == Bridge::IpcQueryStatus::NoEngine)
        return errorResponse("NOT_INIT", "HarkenSystem not available");
    if (status != Bridge::IpcQueryStatus::Ok)
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
    const auto presets = Bridge::listModulatorPresets(typeFilter);
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
    result["total_presets"] = static_cast<int>(Bridge::modulatorPresetTotalCount());
    result["presets"] = presetsArr;
    return successResponse(result);
}

QJsonObject IPCServer::handleApplyModulatorPreset(const QJsonObject& args)
{
    QString presetName = args["preset_name"].toString();
    if (presetName.isEmpty())
        return errorResponse("INVALID_PARAM", "preset_name required");

    Bridge::ModulatorPresetDesc preset;
    if (!Bridge::findModulatorPreset(presetName, preset))
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

    Bridge::ModulatorPresetDesc preset;
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

    if (!Bridge::saveModulatorUserPreset(preset))
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

    if (!Bridge::deleteModulatorUserPreset(modType, presetName))
        return errorResponse("NOT_FOUND", "Preset not found or is a factory preset");

    QJsonObject result;
    result["preset_name"] = presetName;
    static const char* typeNames[] = { "lfo", "envelope", "time", "step" };
    result["type"] = QString(typeNames[modType]);
    return successResponse(result);
}


QJsonObject IPCServer::handleGetStreamStatus(const QJsonObject& )
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    Bridge::StreamStatusData info;
    if (Bridge::streamStatus(info) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_INIT", "HarkenSystem not available");

    QJsonObject result;
    result["active_streams"]       = static_cast<int>(info.activeStreams);
    result["paused_streams"]       = static_cast<int>(info.pausedStreams);
    result["total_buffered_frames"] = static_cast<qint64>(info.totalBufferedFrames);

    result["device_block_size"]     = static_cast<int>(info.deviceBlockSize);
    result["device_supports_seek"]  = info.deviceSupportsSeek;
    result["device_supports_async"] = info.deviceSupportsAsync;

    return successResponse(result);
}

QJsonObject IPCServer::handleGetStreamPosition(const QJsonObject& args)
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    if (!Bridge::engineSessionActive())
        return errorResponse("NOT_INIT", "HarkenSystem not available");

    int voiceId = args.value("voice_id").toInt(-1);
    if (voiceId >= 0)
    {
        Bridge::StreamPositionData posInfo;
        if (Bridge::streamPositionForVoice(static_cast<uint32_t>(voiceId), posInfo)
                != Bridge::IpcQueryStatus::Ok)
            return errorResponse("NOT_FOUND", "Voice not found or has no stream buffer");

        QJsonObject result;
        result["voice_id"]          = voiceId;
        result["decoded_frame_pos"] = static_cast<qint64>(posInfo.decodedFramePos);
        result["total_file_frames"] = static_cast<qint64>(posInfo.totalFileFrames);
        result["progress"]          = static_cast<double>(posInfo.progress);
        result["stream_result"]     = static_cast<int>(posInfo.streamResult);
        return successResponse(result);
    }

    Bridge::StreamStatusData info;
    if (Bridge::streamStatus(info) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_INIT", "HarkenSystem not available");
    QJsonObject result;
    result["active_streams"]        = static_cast<int>(info.activeStreams);
    result["paused_streams"]        = static_cast<int>(info.pausedStreams);
    result["total_buffered_frames"] = static_cast<qint64>(info.totalBufferedFrames);
    result["device_block_size"]     = static_cast<int>(info.deviceBlockSize);
    result["device_supports_seek"]  = info.deviceSupportsSeek;
    result["device_supports_async"] = info.deviceSupportsAsync;
    return successResponse(result);
}


QJsonObject IPCServer::handleListCodecs(const QJsonObject& )
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    std::vector<Bridge::CodecDesc> codecs;
    if (Bridge::listCodecs(codecs) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_INIT", "HarkenSystem not available");

    QJsonArray codecArray;
    for (const auto& info : codecs)
    {
        QJsonObject codec;
        codec["format_id"]              = static_cast<int>(info.formatId);
        codec["name"]                   = info.name;
        codec["file_extension"]         = info.fileExtension;
        codec["version"]                = QString::asprintf("%d.%d", info.version >> 16, info.version & 0xFFFF);
        codec["preferred_buffer_frames"] = static_cast<int>(info.preferredBufferFrames);
        codec["max_channels"]           = static_cast<int>(info.maxChannels);
        codec["capabilities"]           = static_cast<int>(info.capabilities);
        codec["active_streams"]         = static_cast<int>(info.activeStreams);
        codecArray.append(codec);
    }

    QJsonObject result;
    result["codecs"] = codecArray;
    result["count"]  = static_cast<int>(codecs.size());
    return successResponse(result);
}

QJsonObject IPCServer::handleGetCodecInfo(const QJsonObject& args)
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    if (!Bridge::engineSessionActive())
        return errorResponse("NOT_INIT", "HarkenSystem not available");

    int formatId = args.value("format_id").toInt(-1);
    if (formatId < 0) return errorResponse("MISSING_PARAM", "Missing 'format_id' parameter");

    Bridge::CodecDesc info;
    if (Bridge::codecInfo(static_cast<uint32_t>(formatId), info) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_FOUND", "No codec registered for format_id " + QString::number(formatId));

    QJsonObject result;
    result["format_id"]              = static_cast<int>(info.formatId);
    result["name"]                   = info.name;
    result["file_extension"]         = info.fileExtension;
    result["version"]                = QString::asprintf("%d.%d", info.version >> 16, info.version & 0xFFFF);
    result["preferred_buffer_frames"] = static_cast<int>(info.preferredBufferFrames);
    result["max_channels"]           = static_cast<int>(info.maxChannels);
    result["seekable"]               = (info.capabilities & (1u << 0)) != 0;
    result["streamable"]             = (info.capabilities & (1u << 1)) != 0;
    result["full_decode"]            = (info.capabilities & (1u << 2)) != 0;
    result["variable_bitrate"]       = (info.capabilities & (1u << 3)) != 0;
    result["lossless"]               = (info.capabilities & (1u << 4)) != 0;
    result["active_streams"]         = static_cast<int>(info.activeStreams);
    return successResponse(result);
}


QJsonObject IPCServer::handleGetStreamDeviceInfo(const QJsonObject& args)
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    if (!Bridge::engineSessionActive())
        return errorResponse("NOT_INIT", "HarkenSystem not available");

    int deviceIndex = args.value("device_index").toInt(-1);
    if (deviceIndex < 0) return errorResponse("MISSING_PARAM", "Missing 'device_index' parameter");

    Bridge::StreamDeviceDesc desc;
    if (Bridge::streamDeviceInfo(static_cast<uint32_t>(deviceIndex), desc)
            != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_FOUND", "No stream device at index " + QString::number(deviceIndex));

    QJsonObject result;
    result["device_index"]      = static_cast<int>(desc.deviceIndex);
    result["name"]              = desc.name;
    result["device_id"]         = static_cast<int>(desc.deviceId);
    result["block_size"]        = static_cast<int>(desc.blockSize);
    result["max_concurrent_io"] = static_cast<int>(desc.maxConcurrentIo);
    result["supports_seek"]     = desc.supportsSeek;
    result["supports_async"]    = desc.supportsAsync;
    result["supports_mmap"]     = desc.supportsMmap;
    result["is_removable"]      = desc.isRemovable;
    result["total_bytes_read"]  = static_cast<qint64>(desc.totalBytesRead);
    result["active_io_count"]   = static_cast<int>(desc.activeIoCount);
    return successResponse(result);
}

QJsonObject IPCServer::handleGetStreamDeviceCount(const QJsonObject& )
{
    if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

    uint32_t count = 0;
    if (Bridge::streamDeviceCount(count) != Bridge::IpcQueryStatus::Ok)
        return errorResponse("NOT_INIT", "HarkenSystem not available");

    QJsonObject result;
    result["count"] = static_cast<int>(count);
    return successResponse(result);
}


QJsonObject IPCServer::handleGetMemoryBudgetStatus(const QJsonObject& )
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    Bridge::MemoryBudgetStatusData status;
    const auto rc = Bridge::memoryBudgetStatus(status);
    if (rc == Bridge::IpcQueryStatus::NoEngine)
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
    if (rc != Bridge::IpcQueryStatus::Ok)
        return errorResponse("QUERY_FAILED", "Failed to query memory budget status");

    static const char* catNames[] = {
        "General", "Media", "Streaming", "DSP",
        "Voice", "Object", "SpatialAudio", "Debug"
    };

    QJsonArray cats;
    for (uint32_t i = 0; i < 8; ++i)
    {
        const auto& c = status.categories[i];
        cats.append(QJsonObject{
            {"name",           catNames[i]},
            {"budget_bytes",   static_cast<double>(c.budgetBytes)},
            {"heap_usage",     static_cast<double>(c.heapUsageBytes)},
            {"mmap_usage",     static_cast<double>(c.mmapUsageBytes)},
            {"total_usage",    static_cast<double>(c.totalUsageBytes)},
            {"is_over_budget", c.isOverBudget}
        });
    }

    return successResponse(QJsonObject{
        {"categories",        cats},
        {"total_usage_bytes", static_cast<double>(status.totalUsageBytes)},
        {"total_budget_bytes", static_cast<double>(status.totalBudgetBytes)},
        {"over_budget_count", static_cast<int>(status.overBudgetCount)}
    });
}

QJsonObject IPCServer::handleSetMemoryBudgetLimit(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    if (!Bridge::engineSessionActive())
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");

    uint32_t category = static_cast<uint32_t>(args.value("category").toInt(-1));
    double maxBytesD  = args.value("max_bytes").toDouble(0.0);
    if (category >= 8)
        return errorResponse("INVALID_PARAM", "category must be 0-7",
                             "0=General,1=Media,2=Streaming,3=DSP,4=Voice,5=Object,6=SpatialAudio,7=Debug");
    uint64_t maxBytes = static_cast<uint64_t>(maxBytesD);

    if (engine_->setMemoryBudgetLimit(category, maxBytes) != HarkenEditor::Bridge::kEngineOk)
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

    if (!Bridge::engineSessionActive())
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");

    uint32_t listenerId = static_cast<uint32_t>(args.value("listener_id").toInt(-1));
    harkenstudio::ObjectId busId{};
    if (!parseObjectIdJson(args.value("bus_id"), &busId))
        return errorResponse("INVALID_ARGS", "bus_id must be an object UUID");

    if (engine_->setListenerOutputBus(listenerId, busId) != HarkenEditor::Bridge::kEngineOk)
        return errorResponse("SET_FAILED", "Failed to set listener output bus");

    return successResponse(QJsonObject{
        {"listener_id", static_cast<int>(listenerId)},
        {"bus_id",      objectIdString(busId)}
    });
}

QJsonObject IPCServer::handleGetListenerOutputBus(const QJsonObject& args)
{
    if (!engine_)
        return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine not available");

    uint32_t listenerId = static_cast<uint32_t>(args.value("listener_id").toInt(-1));

    Bridge::ListenerBusBindingData binding;
    const auto rc = Bridge::listenerOutputBus(listenerId, binding);
    if (rc == Bridge::IpcQueryStatus::NoEngine)
        return errorResponse("ENGINE_NOT_INITIALIZED", "Engine not initialized");
    if (rc != Bridge::IpcQueryStatus::Ok)
        return errorResponse("QUERY_FAILED", "Failed to get listener output bus");

    return successResponse(QJsonObject{
        {"listener_id", static_cast<int>(binding.listenerId)},
        {"output_bus_id", objectIdString(binding.outputBusId)},
        {"is_active",   binding.isActive}
    });
}

}
