
#include "Core/ActionManager.h"
#include "Core/AuthoringPolicy.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>

namespace VNX {

namespace {

ActionResult fail(const QString& code, const QString& fmtPrefix,
                  const ActionResult& sub) {
    return {false, {}, code,
            QStringLiteral("%1 failed: %2 (%3)")
                .arg(fmtPrefix, sub.errorMessage, sub.errorCode)};
}

}


void registerTemplateActions(ActionManager& mgr, ProjectManager* projectManager)
{
    ActionDef def;
    def.id          = "apply_template_from_data";
    def.displayName = "Apply Project Template (from data)";
    def.category    = "editor";
    def.undoable    = false;
    def.handler = [&mgr, projectManager](const QJsonObject& args) -> ActionResult {
        auto* project = projectManager ? projectManager->currentProject() : nullptr;
        if (!project)
            return {false, {}, "NO_PROJECT",
                    "No active project — call create_project first"};

        if (!args.contains("template_data") || !args.value("template_data").isObject())
            return {false, {}, "INVALID_ARGS",
                    "Missing 'template_data' object in args"};
        const QJsonObject td = args.value("template_data").toObject();
        const auto authoring = AuthoringPolicy::validateTemplate(td);
        if (!authoring)
            return {false, {}, "AUTHORING_RESTRICTED", authoring.message};

        QJsonArray createdAttenuations;
        QJsonArray createdBusIds;

        for (const auto& v : td.value("attenuation").toArray()) {
            const auto a       = v.toObject();
            const auto name    = a.value("name").toString();
            const double maxD  = a.value("max").toDouble();
            auto sub = mgr.execute("create_attenuation", QJsonObject{
                {"name",         name},
                {"definition",   QJsonObject{}},
                {"max_distance", maxD}
            }, ActionSource::Internal);
            if (!sub.success)
                return fail("ATTEN_CREATE_FAILED",
                            QStringLiteral("create_attenuation(%1)").arg(name),
                            sub);
            const auto vol = a.value("volume").toObject();
            auto cv = mgr.execute("set_attenuation_curve", QJsonObject{
                {"name",              name},
                {"curve",             QStringLiteral("volume")},
                {"points",            vol.value("points")},
                {"max_distance",      maxD},
                {"create_if_missing", false}
            }, ActionSource::Internal);
            if (!cv.success)
                return fail("ATTEN_CURVE_FAILED",
                            QStringLiteral("set_attenuation_curve(%1)").arg(name),
                            cv);
            createdAttenuations.append(name);
        }

        const auto rootJson = td.value("bus").toObject();
        ProjectItem* masterItem = projectManager->getMasterBusItem();
        if (!masterItem)
            return {false, {}, "NO_MASTER_BUS",
                    "Project has no master bus (ensureDefaultMasterBus did not run?)"};

        std::function<ActionResult(const QJsonObject&, uint32_t)> createTree;
        createTree = [&](const QJsonObject& node, uint32_t parentBusId) -> ActionResult {
            const QString name = node.value("name").toString();
            auto cb = mgr.execute("create_bus", QJsonObject{
                {"name",          name},
                {"parent_bus_id", static_cast<qint64>(parentBusId)}
            }, ActionSource::Internal);
            if (!cb.success)
                return fail("BUS_CREATE_FAILED",
                            QStringLiteral("create_bus(%1)").arg(name),
                            cb);
            const uint32_t busId = static_cast<uint32_t>(
                cb.data.value("bus_id").toInteger());
            createdBusIds.append(static_cast<qint64>(busId));

            for (const auto& c : node.value("children").toArray()) {
                if (auto sub = createTree(c.toObject(), busId); !sub.success)
                    return sub;
            }
            return {true, {}, {}, {}};
        };

        for (const auto& c : rootJson.value("children").toArray()) {
            if (auto wr = createTree(c.toObject(), kMasterBusID); !wr.success)
                return wr;
        }

        ActionResult r;
        r.success = true;
        r.data = QJsonObject{
            {"success",                 r.success},
            {"created_attenuations",    createdAttenuations},
            {"created_bus_ids",         createdBusIds}
        };
        return r;
    };
    mgr.registerAction(def);
}

}
