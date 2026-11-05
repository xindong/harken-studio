#include "IPCDesktopInternal.h"
#include "ThemeManager.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "../BuiltinLayouts.h"
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
    registerDesktopHandler("get_panel_rect", [this](const QJsonObject& args) { return handleGetPanelRect(args); });
    registerDesktopHandler("get_current_layout", [this](const QJsonObject& args) { return handleGetCurrentLayout(args); });
    registerDesktopHandler("get_current_view", [this](const QJsonObject& args) {
        qWarning() << "[deprecated] get_current_view, use get_current_layout";
        return handleGetCurrentView(args);
    });
    registerDesktopHandler("get_layout_state", [this](const QJsonObject& args) { return handleGetLayoutState(args); });
    registerDesktopHandler("get_inspector_context", [this](const QJsonObject& args) { return handleGetInspectorContext(args); });
    registerDesktopHandler("get_inspector_schema", [this](const QJsonObject& args) -> QJsonObject {
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
    });
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
        return successResponse(QJsonObject{
            {"panel_id", panelId},
            {"type", ip ? ip->contextType() : QString()},
            {"name", ip ? ip->contextName() : QString()},
            {"pinned", ip ? ip->isPinned() : false}
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
        QMetaObject::invokeMethod(mainWindow_, [this, panelId]() {
            mainWindow_->closeInspectorPanel(panelId);
        }, Qt::QueuedConnection);
        return successResponse(QJsonObject{{"panel_id", panelId}});
    });
    registerDesktopHandler("get_recent_logs", [this](const QJsonObject& args) { return handleGetRecentLogs(args); });
    registerDesktopHandler("list_panels", [this](const QJsonObject& args) { return handleListPanels(args); });
}

QJsonObject IPCServer::handleGetPanelRect(const QJsonObject& args)
{
    QString panelName = args["name"].toString();

    {
        QWidget* panel = nullptr;

        if ((panelName == "blueprint" || panelName == "node_graph") && mainWindow_)
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
