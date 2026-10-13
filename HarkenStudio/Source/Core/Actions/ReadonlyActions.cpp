
#include "ActionManager.h"
#include "../ProjectPlatformCommandSchema.h"
#include "../IPC/IPCServer.h"
#include <QDebug>

namespace VNX {

static QString humanizeCommandName(const QString& cmd)
{
    QStringList parts = cmd.split('_');
    for (auto& p : parts) {
        if (!p.isEmpty())
            p[0] = p[0].toUpper();
    }
    return parts.join(' ');
}

static QString inferCategory(const QString& cmd)
{
    if (cmd.contains(QLatin1String("mixer")) ||
        cmd.contains(QLatin1String("bus")) ||
        cmd == QLatin1String("get_send_filter") ||
        cmd.contains(QLatin1String("vca")) ||
        cmd.contains(QLatin1String("snapshot")) ||
        cmd.contains(QLatin1String("device_list")) ||
        cmd.contains(QLatin1String("output_info")) ||
        cmd.contains(QLatin1String("output_bus_routing")))
        return QStringLiteral("mixer");

    if (cmd.contains(QLatin1String("node_graph")) ||
        cmd.contains(QLatin1String("nodegraph")) ||
        cmd.contains(QLatin1String("blueprint")) ||
        cmd.contains(QLatin1String("action_definition")) ||
        cmd.contains(QLatin1String("list_actions")) ||
        cmd.contains(QLatin1String("dialogue")) ||
        cmd.contains(QLatin1String("preset")) ||
        cmd.contains(QLatin1String("signal_chain")) ||
        cmd.contains(QLatin1String("referencing_actions")) ||
        cmd.contains(QLatin1String("action_tx")))
        return QStringLiteral("editor");

    if (cmd.contains(QLatin1String("meter")) ||
        cmd.contains(QLatin1String("peak_data")) ||
        cmd.contains(QLatin1String("rms_data")) ||
        cmd.contains(QLatin1String("lufs_data")) ||
        cmd.contains(QLatin1String("spectrum_data")) ||
        cmd.contains(QLatin1String("spectrogram")) ||
        cmd.contains(QLatin1String("oscilloscope")) ||
        cmd.contains(QLatin1String("stereo_data")) ||
        cmd.contains(QLatin1String("waveform")) ||
        cmd.contains(QLatin1String("vu_data")))
        return QStringLiteral("meter");

    if (cmd.contains(QLatin1String("pack")) ||
        cmd.contains(QLatin1String("asset")) ||
        cmd.contains(QLatin1String("unused")) ||
        cmd.contains(QLatin1String("media_status")) ||
        cmd.contains(QLatin1String("conversion")) ||
        cmd.contains(QLatin1String("platform_profile")) ||
        cmd.contains(QLatin1String("available_languages")) ||
        cmd.contains(QLatin1String("project_languages")))
        return QStringLiteral("asset");

    if (cmd.contains(QLatin1String("container")) ||
        cmd.contains(QLatin1String("playlist_entries")) ||
        cmd.contains(QLatin1String("segment_info")) ||
        cmd.contains(QLatin1String("transition_rules")) ||
        cmd.contains(QLatin1String("active_transitions")) ||
        cmd.contains(QLatin1String("music_context")))
        return QStringLiteral("container");

    if (cmd.contains(QLatin1String("project_info")) ||
        cmd.contains(QLatin1String("pack")) ||
        cmd.contains(QLatin1String("item_info")) ||
        cmd == QLatin1String("list_items"))
        return QStringLiteral("project");

    if (cmd.contains(QLatin1String("spatial")) ||
        cmd.contains(QLatin1String("emitter")) ||
        cmd.contains(QLatin1String("binaural")))
        return QStringLiteral("spatial");

    if (cmd.contains(QLatin1String("ui_tree")) ||
        cmd.contains(QLatin1String("widget")) ||
        cmd.contains(QLatin1String("table_data")) ||
        cmd.contains(QLatin1String("list_items")) ||
        cmd.contains(QLatin1String("tree_data")) ||
        cmd.contains(QLatin1String("panel")) ||
        cmd.contains(QLatin1String("inspector")) ||
        cmd.contains(QLatin1String("layout")) ||
        cmd.contains(QLatin1String("current_view")) ||
        cmd.contains(QLatin1String("focused_widget")) ||
        cmd.contains(QLatin1String("interacted_widget")))
        return QStringLiteral("ui");

    if (cmd.contains(QLatin1String("music")) &&
        !cmd.contains(QLatin1String("levels")))
        return QStringLiteral("music");

    if (cmd.contains(QLatin1String("audition")))
        return QStringLiteral("audition");

    if (cmd.startsWith(QLatin1String("can_")))
        return QStringLiteral("undo");

    return QStringLiteral("system");
}

void registerReadonlyActions(ActionManager& mgr, IPCServer* ipcServer)
{
    if (!ipcServer) return;

    const QStringList allNames = ipcServer->commandNames();
    int count = 0;

    for (const auto& name : allNames) {
        if (mgr.hasAction(name))
            continue;

        if (!name.startsWith(QLatin1String("get_")) &&
            !name.startsWith(QLatin1String("list_")) &&
            !name.startsWith(QLatin1String("can_")) &&
            !name.startsWith(QLatin1String("query_")) &&
            !name.startsWith(QLatin1String("find_")) &&
            name != QLatin1String("ping"))
            continue;

        ActionDef def;
        def.id = name;
        def.paramSchema = projectPlatformCommandSchema(name);
        def.displayName = humanizeCommandName(name);
        def.category = inferCategory(name);
        def.undoable = false;
        def.handler = [ipcServer, name](const QJsonObject& args) -> ActionResult {
            QJsonObject response = ipcServer->invokeCommandHandler(name, args);
            const bool ok = response.value(QLatin1String("status")).toString()
                            == QLatin1String("ok");
            return ActionResult{
                ok,
                ok ? response.value(QLatin1String("result")).toObject() : QJsonObject{},
                ok ? QString{} : response.value(QLatin1String("code")).toString(),
                ok ? QString{} : response.value(QLatin1String("message")).toString()
            };
        };
        mgr.registerAction(def);
        ++count;
    }

    qDebug() << "[ActionManager] Registered" << count << "read-only commands via ReadonlyActions";
}

}
