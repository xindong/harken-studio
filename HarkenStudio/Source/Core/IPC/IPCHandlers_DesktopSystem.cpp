#include "IPCDesktopInternal.h"
#include "ThemeManager.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "../BuiltinLayouts.h"
#include "../NamedLayoutStore.h"
#include "../SelectionRouter.h"
#include "../../Widgets/Inspector/InspectorPanel.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QThread>


namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

void IPCServer::registerDesktopSystemHandlers()
{
    registerDesktopHandler("set_theme", [this](const QJsonObject& args) -> QJsonObject {
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
    });
    registerDesktopHandler("get_theme", [this](const QJsonObject&) -> QJsonObject {
        ThemeManager& manager = ThemeManager::instance();
        QString themeId;
        const auto read = [&]() { themeId = manager.currentThemeId(); };
        if (QThread::currentThread() == manager.thread())
            read();
        else
            QMetaObject::invokeMethod(&manager, read, Qt::BlockingQueuedConnection);
        return successResponse(QJsonObject{{"theme", themeId}});
    });
    registerDesktopHandler("get_remote_connection_status", [this](const QJsonObject&) -> QJsonObject {
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
    });
    registerDesktopHandler("list_named_layouts", [this](const QJsonObject&) {
        QJsonArray layouts;
        for (const auto& layout : NamedLayoutStore::list())
            layouts.append(QJsonObject{{"name", layout.name}, {"saved_at", layout.savedAt}});
        return successResponse(QJsonObject{{"layouts", layouts}, {"count", layouts.size()}});
    });
    registerDesktopHandler("get_selected_bus", [this](const QJsonObject&) {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
        if (!projectManager_ || !projectManager_->isProjectOpen())
            return errorResponse("NO_PROJECT_OPEN", "No project is currently open");
        const auto* router = mainWindow_->findChild<SelectionRouter*>();
        if (!router)
            return errorResponse("SELECTION_NOT_AVAILABLE", "Selection controller is unavailable");
        const auto* item = router->activeItem();
        if (!item || item->type != ProjectItem::Type::Bus)
            return successResponse(QJsonObject{{"selected", false}});
        return successResponse(QJsonObject{{"selected", true},
            {"bus_id", static_cast<qint64>(item->objectID)}, {"name", item->name}});
    });
    registerDesktopHandler("get_panel_rect", [this](const QJsonObject& args) { return handleGetPanelRect(args); });
    registerDesktopHandler("get_current_layout", [this](const QJsonObject& args) { return handleGetCurrentLayout(args); });
    registerDesktopHandler("get_layout_state", [this](const QJsonObject& args) { return handleGetLayoutState(args); });
    registerDesktopHandler("get_inspector_context", [this](const QJsonObject& args) { return handleGetInspectorContext(args); });
    registerDesktopHandler("create_inspector_panel", [this](const QJsonObject& args) -> QJsonObject {
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
        if (!ip)
            return errorResponse("CREATE_FAILED", "Failed to create inspector panel");
        return successResponse(QJsonObject{
            {"panel_id", panelId},
            {"type", ip->contextType()},
            {"name", ip->contextName()},
            {"pinned", ip->isPinned()}
        });
    });
    registerDesktopHandler("create_editor", [this](const QJsonObject& args) -> QJsonObject {
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
    });
    registerDesktopHandler("set_inspector_pinned", [this](const QJsonObject& args) -> QJsonObject {
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
    });
    registerDesktopHandler("list_inspector_panels", [this](const QJsonObject&) -> QJsonObject {
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
    });
    registerDesktopHandler("close_inspector_panel", [this](const QJsonObject& args) -> QJsonObject {
        if (!mainWindow_)
            return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
        if (!args.contains("panel_id"))
            return errorResponse("INVALID_ARGS", "panel_id required");
        const int panelId = args["panel_id"].toInt();
        if (!mainWindow_->findInspectorPanel(panelId))
            return errorResponse("PANEL_NOT_FOUND", QString("No Inspector panel with panel_id=%1").arg(panelId));
        mainWindow_->closeInspectorPanel(panelId);
        return successResponse(QJsonObject{{"panel_id", panelId}, {"closed", true}});
    });
    registerDesktopHandler("get_recent_logs", [this](const QJsonObject& args) { return handleGetRecentLogs(args); });
    registerDesktopHandler("list_panels", [this](const QJsonObject& args) { return handleListPanels(args); });
}

QJsonObject IPCServer::handleGetPanelRect(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
    const QString name = args.value("name").toString().trimmed();
    if (name.isEmpty())
        return errorResponse("INVALID_ARGS", "name is required; use list_panels to discover panel names");
    QWidget* panel = nullptr;
    for (const auto& summary : mainWindow_->workspaceWindowSummaries()) {
        if (!workspaceWindowSelectorMatches(name,
                summary.value("typeId").toString(), summary.value("name").toString(),
                summary.value("title").toString(), summary.value("instanceId").toInt()))
            continue;
        if (panel)
            return errorResponse("AMBIGUOUS_PANEL", "Use a unique panel name from list_panels");
        panel = mainWindow_->findChild<ads::CDockWidget*>(summary.value("name").toString());
    }
    if (!panel)
        return errorResponse("PANEL_NOT_FOUND", QString("Panel '%1' not found").arg(name));
    if (!panel->isVisible())
        return errorResponse("PANEL_NOT_VISIBLE", QString("Panel '%1' is not visible").arg(name));
    const QRect rect(panel->mapToGlobal(QPoint(0, 0)), panel->size());
    return successResponse(QJsonObject{{"name", panel->objectName()},
        {"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}});
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
        args["layout"].toString(), mainWindow_->currentView());
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
        {"layout", resolved.canonical},
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

}
