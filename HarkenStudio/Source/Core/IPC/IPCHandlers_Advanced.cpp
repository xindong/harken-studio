#include "IPCServerInternal.h"
#include "PackGenerationRecord.h"
#include <QDateTime>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

void IPCServer::registerAdvancedHandlers()
{
    commandHandlers_["ai_response"] = [this](const QJsonObject& args) { return handleAIResponse(args); };
    commandHandlers_["ai_event"] = [this](const QJsonObject& args) { return handleAIEvent(args); };

    commandHandlers_["replay_verify"] = [this](const QJsonObject& args) {
        QString refPath = args["reference_path"].toString();
        QString repPath = args["replay_path"].toString();
        if (refPath.isEmpty() || repPath.isEmpty())
            return errorResponse("INVALID_PARAM", "reference_path and replay_path required");

        Bridge::ReplayCompareResult result;
        Bridge::replayCompare(refPath, repPath, result);
        QJsonObject r;
        r["max_diff"] = static_cast<double>(result.maxDiff);
        r["psnr_db"] = result.psnrDb;
        r["is_bitexact"] = result.isBitExact;
        r["total_samples"] = static_cast<qint64>(result.totalSamples);
        r["diff_samples"] = static_cast<qint64>(result.diffSamples);
        r["length_match"] = result.lengthMatch;
        return successResponse(r);
    };

    const auto unavailable = [this](const QString& message) {
        return errorResponse("UNAVAILABLE", message);
    };

    commandHandlers_["set_clock_pll"] = [this, unavailable](const QJsonObject& args) {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
        bool enabled = args["enabled"].toBool(false);
        if (Bridge::setClockPll(enabled) != Bridge::IpcQueryStatus::Ok)
            return unavailable("Clock PLL is not available in the current runtime bridge");
        return successResponse(QJsonObject{{"action", enabled ? "pll_enabled" : "pll_disabled"}});
    };

    commandHandlers_["get_clock_sync_status"] = [this, unavailable](const QJsonObject&) {
        if (!engine_) return errorResponse("ENGINE_NOT_AVAILABLE", "AudioEngine is not available");
        Bridge::ClockSyncStatus st;
        if (Bridge::clockSyncStatus(st) != Bridge::IpcQueryStatus::Ok)
            return unavailable("Clock sync status is not available in the current runtime bridge");
        QJsonObject r;
        r["actual_sample_rate"] = st.actualSampleRate;
        r["drift_ppm"] = st.driftPpm;
        r["sync_status"] = "NoSync";
        return successResponse(r);
    };

    commandHandlers_["link_get_status"] = [this](const QJsonObject&) {
        return successResponse(QJsonObject{
            {"connected", false},
            {"client_ip", QString()},
            {"latency_ms", 0.0},
            {"status", QStringLiteral("retired")},
            {"message", QStringLiteral("HarkenLink is retired; live-connect is a future runtime-wire plan")}
        });
    };

    commandHandlers_["link_post_action"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink action posting is retired");
    };
    commandHandlers_["link_set_property"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink property updates are retired");
    };
    commandHandlers_["link_mute_bus"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink bus mute is retired");
    };
    commandHandlers_["link_solo_bus"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink bus solo is retired");
    };
    commandHandlers_["link_stop_all"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink stop-all is retired");
    };
    commandHandlers_["link_pause_engine"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink pause is retired");
    };
    commandHandlers_["link_resume_engine"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink resume is retired");
    };

    commandHandlers_["link_capture_start"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink capture is retired");
    };
    commandHandlers_["link_capture_stop"] = [unavailable](const QJsonObject&) {
        return unavailable("HarkenLink capture is retired");
    };

    commandHandlers_["get_exec_errors"] = [this](const QJsonObject& args) {
        if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");

        int domainFilter = -1;
        if (args.contains("domain")) {
            const QString domain = args["domain"].toString();
            if (domain == QLatin1String("Music"))
                domainFilter = static_cast<int>(AudioEngine::ExecErrorDomain::Music);
            else if (domain == QLatin1String("Stop"))
                domainFilter = static_cast<int>(AudioEngine::ExecErrorDomain::Stop);
            else if (domain == QLatin1String("Lifecycle"))
                domainFilter = static_cast<int>(AudioEngine::ExecErrorDomain::Lifecycle);
            else if (domain == QLatin1String("Dispatch"))
                domainFilter = static_cast<int>(AudioEngine::ExecErrorDomain::Dispatch);
            else if (domain == QLatin1String("Gate"))
                domainFilter = static_cast<int>(AudioEngine::ExecErrorDomain::Gate);
            else
                return errorResponse("INVALID_ARGS",
                    QString("Unknown domain '%1' (expected Music/Stop/"
                            "Lifecycle/Dispatch/Gate)").arg(domain));
        }

        int maxCount = args["max_count"].toInt(50);
        bool clearAfter = args["clear"].toBool(false);

        static const char* const kDomainNames[] =
            { "Music", "Stop", "Lifecycle", "Dispatch", "Gate" };

        auto errors = engine_->recentExecErrors(maxCount, domainFilter);
        QJsonArray errArray;
        for (const auto& err : errors) {
            errArray.append(QJsonObject{
                {"domain", kDomainNames[static_cast<int>(err.domain)]},
                {"severity", static_cast<int>(err.severity)},
                {"object_id", static_cast<qint64>(err.objectID)},
                {"playing_id", static_cast<qint64>(err.playingID)},
                {"object_name", err.objectName},
                {"error_type", err.errorType},
                {"message", err.message},
                {"timestamp_ms", static_cast<double>(err.timestampMs)}
            });
        }

        if (clearAfter)
            engine_->clearExecErrors(domainFilter);

        return successResponse(QJsonObject{
            {"errors", errArray},
            {"count", errArray.size()},
            {"cleared", clearAfter}
        });
    };

    commandHandlers_["get_format_version"] = [this](const QJsonObject&) {
        auto* project = ProjectManager::instance().currentProject();
        if (!project)
            return errorResponse("NO_PROJECT_OPEN", "No project is open");
        QJsonObject r;
        r["schemaVersion"] = MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION;
        r["currentSchemaVersion"] = MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION;
        return successResponse(r);
    };

    commandHandlers_["get_file_watch_status"] = [this](const QJsonObject&) {
        auto* fw = ProjectManager::instance().fileWatcher();
        QJsonObject r;
        r["watching"] = fw->isWatching();
        r["debounceMs"] = fw->debounceInterval();
        return successResponse(r);
    };

    registerDesktopHandler("get_ui_tree");
    registerDesktopHandler("find_widget");
    registerDesktopHandler("grab_widget");
    registerDesktopHandler("get_focused_widget");
    registerDesktopHandler("get_last_interacted_widget");

    registerDesktopHandler("get_widget_value");
    registerDesktopHandler("get_widget_children");
    registerDesktopHandler("get_table_data");
    registerDesktopHandler("get_list_items");
    registerDesktopHandler("get_tree_data");

    registerDesktopHandler("click_widget");
    registerDesktopHandler("key_press");
    registerDesktopHandler("set_widget_value");
    registerDesktopHandler("select_table_row");
    registerDesktopHandler("trigger_menu_action");
    registerDesktopHandler("answer_dialog");

    registerDesktopHandler("flush_ui_events");
}


QJsonObject IPCServer::handleAIResponse(const QJsonObject& args)
{
    QString text = args["text"].toString();
    
    if (text.isEmpty())
    {
        return errorResponse("INVALID_ARGS", "Response text is required");
    }
    
    emit aiResponseReceived(text);

    qDebug() << "AI Response received:" << text.left(50) << "...";

    return successResponse(QJsonObject{{"received", true}});
}

QJsonObject IPCServer::handleAIEvent(const QJsonObject& args)
{
    const QString kind = args.value("kind").toString();
    if (kind.isEmpty())
        return errorResponse("INVALID_ARGS", "ai_event requires a 'kind' field");

    emit aiAgentEvent(args);
    qDebug() << "AI Event received:" << kind;

    return successResponse(QJsonObject{{"received", true}});
}


}
