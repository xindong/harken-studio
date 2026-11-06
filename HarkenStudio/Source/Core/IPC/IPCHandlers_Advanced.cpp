#include "IPCServerInternal.h"
#include "PackGenerationRecord.h"
#include <QDateTime>
#include <optional>
#include <climits>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

void IPCServer::registerAdvancedHandlers()
{
    commandHandlers_["ai_response"] = [this](const QJsonObject& args) { return handleAIResponse(args); };
    commandHandlers_["ai_event"] = [this](const QJsonObject& args) { return handleAIEvent(args); };

    commandHandlers_["compare_files"] = [this](const QJsonObject& args) {
        QString refPath = args["reference_path"].toString();
        QString repPath = args["candidate_path"].toString();
        if (args.size() != 2 || refPath.isEmpty() || repPath.isEmpty())
            return errorResponse("INVALID_PARAM", "reference_path and candidate_path are required file paths");

        Bridge::FileCompareResult result;
        if (!Bridge::compareFiles(refPath, repPath, result))
            return errorResponse("COMPARE_FAILED", "Cannot read the reference or candidate file");
        QJsonObject r;
        r["identical"] = result.identical;
        r["compared_bytes"] = static_cast<qint64>(result.comparedBytes);
        r["different_bytes"] = static_cast<qint64>(result.differentBytes);
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

    const auto execErrorDomain = [](const QJsonObject& args) -> std::optional<int> {
        if (!args.contains("domain")) return -1;
        const QStringList domains{"Music", "Stop", "Lifecycle", "Dispatch", "Gate"};
        const int index = domains.indexOf(args.value("domain").toString());
        if (index < 0) return std::nullopt;
        return index;
    };
    commandHandlers_["get_exec_errors"] = [this, execErrorDomain](const QJsonObject& args) {
        if (args.contains("clear"))
            return errorResponse("INVALID_ARGS", "Use clear_exec_errors to clear execution errors");
        if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");
        const auto domain = execErrorDomain(args);
        if (!domain)
            return errorResponse("INVALID_ARGS", "domain must be Music, Stop, Lifecycle, Dispatch or Gate");
        const auto countValue = args.value("max_count");
        const qint64 count = args.contains("max_count") ? countValue.toInteger(-1) : 50;
        if (count < 0 || count > INT_MAX
            || (args.contains("max_count") && (!countValue.isDouble() || countValue.toDouble() != count)))
            return errorResponse("INVALID_ARGS", "max_count must be a non-negative integer");
        static const char* const names[] = {"Music", "Stop", "Lifecycle", "Dispatch", "Gate"};
        const auto errors = engine_->recentExecErrors(static_cast<int>(count), *domain);
        QJsonArray entries;
        for (const auto& error : errors) {
            entries.append(QJsonObject{
                {"domain", names[static_cast<int>(error.domain)]},
                {"severity", static_cast<int>(error.severity)},
                {"object_id", static_cast<qint64>(error.objectID)},
                {"playing_id", static_cast<qint64>(error.playingID)},
                {"object_name", error.objectName}, {"error_type", error.errorType},
                {"message", error.message}, {"timestamp_ms", static_cast<double>(error.timestampMs)}
            });
        }
        return successResponse(QJsonObject{{"errors", entries}, {"count", entries.size()}});
    };
    commandHandlers_["clear_exec_errors"] = [this, execErrorDomain](const QJsonObject& args) {
        if (!engine_) return errorResponse("NOT_INIT", "Audio engine not initialized");
        if (args.size() != args.contains("domain"))
            return errorResponse("INVALID_ARGS", "Only the optional domain argument is supported");
        const auto domain = execErrorDomain(args);
        if (!domain)
            return errorResponse("INVALID_ARGS", "domain must be Music, Stop, Lifecycle, Dispatch or Gate");
        engine_->clearExecErrors(*domain);
        return successResponse(QJsonObject{{"cleared", true}});
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
