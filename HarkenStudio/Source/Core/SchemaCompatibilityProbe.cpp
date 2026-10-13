#include "SchemaCompatibilityProbe.h"

#include "PackSchemaGenerated.h"
#include "ProjectManager.h"
#include "ProjectOpenProtocol.h"
#include "ProjectSchemaGenerated.h"
#include "ProjectTypes.h"
#include "SchemaBuildGenerated.h"
#include "harken_buildinfo.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QObject>

#include <algorithm>
#include <string_view>
#include <utility>

namespace VNX {

namespace {

QString buildString(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QHash<QUuid, ProjectItem*> documentItems(ProjectData& project)
{
    QHash<QUuid, ProjectItem*> items;
    const auto collect = [&](auto&& self, ProjectItem* item) -> void {
        if (!item) return;
        items.insert(item->guid, item);
        for (const auto& child : item->children) self(self, child.get());
    };
    for (ProjectFileBinding* binding : project.allFileBindings())
        if (binding) collect(collect, binding->rootItem.get());
    return items;
}

bool createFixture(ProjectManager& manager, const QString& outputDirectory)
{
    const QFileInfo output(outputDirectory);
    ProjectConfig config;
    config.name = output.fileName();
    config.location = output.absolutePath();
    config.description = QStringLiteral("Schema compatibility fixture");
    if (!manager.createProject(config)) return false;
    ProjectItem* layer = manager.createItem(
        nullptr, QStringLiteral("Main"), ProjectItem::Type::Layer);
    if (!layer) return false;
    ProjectItem* nested = manager.createItem(
        layer, QStringLiteral("Nested"), ProjectItem::Type::Layer);
    if (!nested || !manager.createItem(
            nested, QStringLiteral("Tone"), ProjectItem::Type::Sound)) return false;
    for (const auto& [name, type] : {
             std::pair{QStringLiteral("Trigger"), ProjectItem::Type::Action},
             std::pair{QStringLiteral("Space"), ProjectItem::Type::Attenuation},
             std::pair{QStringLiteral("Intensity"), ProjectItem::Type::Variable},
         }) {
        if (!manager.createItem(nullptr, name, type)) return false;
    }
    if (!manager.createItem(manager.getMasterBusItem(), QStringLiteral("Music"),
                            ProjectItem::Type::Bus)) return false;
    return manager.saveProject();
}

}

QJsonObject runSchemaCompatibilityProbe(const QJsonObject& request)
{
    const QString mode = request.value(QStringLiteral("mode")).toString();
    QJsonObject report{
        {QStringLiteral("reportVersion"), 1},
        {QStringLiteral("ok"), false},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
        {QStringLiteral("version"), buildString(Harken::BuildInfo::Version)},
        {QStringLiteral("buildId"), buildString(Harken::BuildInfo::BuildId)},
        {QStringLiteral("gitCommit"), buildString(Harken::BuildInfo::GitCommit)},
        {QStringLiteral("gitDirty"), Harken::BuildInfo::GitDirty},
        {QStringLiteral("editorSchemaVersion"), ProjectSchema::kProjectSchemaVersion},
        {QStringLiteral("packSchemaVersion"), PackSchema::kPackManifestSchemaVersion},
        {QStringLiteral("schemaSnapshotHash"), QString::fromLatin1(SchemaBuild::kSnapshotHash)},
        {QStringLiteral("edits"), QJsonArray{}},
    };
    const auto invalid = [&](const QString& error) {
        report.insert(QStringLiteral("error"), error);
        return report;
    };
    const bool create = mode == QStringLiteral("create");
    const bool edit = mode == QStringLiteral("edit");
    const bool migrate = mode == QStringLiteral("migrate");
    const QString pathKey = create ? QStringLiteral("outputDirectory")
                                   : QStringLiteral("projectPath");
    if (request.value(QStringLiteral("requestVersion")).toInt() != 1
        || (!create && !edit && !migrate && mode != QStringLiteral("read"))
        || request.size() != 3 || !request.value(pathKey).isString())
        return invalid(QStringLiteral("Invalid schema round-trip request"));
    const QString path = QDir::cleanPath(request.value(pathKey).toString());
    const QFileInfo info(path);
    if (!info.isAbsolute() || info.isSymLink())
        return invalid(QStringLiteral("Schema round-trip paths must be absolute and direct"));
    if (create && (info.exists() || !QDir(info.absolutePath()).exists()))
        return invalid(QStringLiteral("Fixture output directory must not exist and its parent must exist"));
    if (!create && (!info.isFile() || info.suffix() != QStringLiteral("hsp")))
        return invalid(QStringLiteral("Schema round-trip project file is unavailable"));

    ProjectManager& manager = ProjectManager::instance();
    if (manager.currentProject())
        return invalid(QStringLiteral("Schema round-trip requires no open project"));
    QString operationError;
    QObject diagnostics;
    QObject::connect(&manager, &ProjectManager::errorOccurred, &diagnostics,
                     [&](const QString& error) { operationError = error; });
    const auto failed = [&](const QString& fallback) {
        report.insert(QStringLiteral("error"), operationError.isEmpty() ? fallback
                                                                         : operationError);
        if (manager.currentProject()) manager.closeProject();
        return report;
    };

    if (migrate) {
        const ProjectUpgradeAnalysis analysis = ProjectUpgradeManager().analyzeForProjectOpen(path);
        if (!analysis.ok || !analysis.upgradeRequired
            || analysis.sourceSchema >= analysis.targetSchema
            || analysis.targetSchema != ProjectSchema::kProjectSchemaVersion)
            return invalid(analysis.blockingErrors.isEmpty()
                ? QStringLiteral("Schema migration requires a supported older generation")
                : analysis.blockingErrors.join(QLatin1Char('\n')));
        const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString digest = projectOpenApprovalDigest(analysis, requestId);
        PreparedProject prepared = ProjectManager::prepareProject(path, true, {},
            [&](const ProjectUpgradeAnalysis& current) {
                return current.ok && current.upgradeRequired
                    && current.sourceSchema < current.targetSchema
                    && current.targetSchema == ProjectSchema::kProjectSchemaVersion
                    && projectOpenApprovalDigest(current, requestId) == digest;
            });
        if (!prepared.project) return invalid(prepared.error);
        if (!manager.commitPreparedProject(std::move(prepared)))
            return failed(QStringLiteral("Schema migration could not open the upgraded fixture"));
        report.insert(QStringLiteral("fromVersion"), analysis.sourceSchema);
    } else if (create ? !createFixture(manager, path) : !manager.loadProject(path, false)) {
        return failed(QStringLiteral("Schema round-trip could not open the fixture"));
    }
    if (!manager.currentProject())
        return failed(QStringLiteral("Schema round-trip did not produce an open project"));
    ProjectData& project = *manager.currentProject();
    const QString projectPath = QFileInfo(project.projectPath).absoluteFilePath();
    report.insert(QStringLiteral("projectPath"), projectPath);
    QJsonArray edits;
    if (edit) {
        const QString before = project.description;
        project.description += QStringLiteral("\nSchema compatibility edit");
        edits.append(QJsonObject{
            {QStringLiteral("documentId"), project.projectId},
            {QStringLiteral("jsonPath"), QStringLiteral("/content/metadata/description")},
            {QStringLiteral("before"), before},
            {QStringLiteral("after"), project.description},
        });
        const auto items = documentItems(project);
        QList<QUuid> ids = items.keys();
        std::sort(ids.begin(), ids.end());
        for (const QUuid& id : ids) {
            ProjectItem* item = items.value(id);
            if (item == manager.getMasterBusItem()) continue;
            const QString originalName = item->name;
            const QString editedName = originalName + QStringLiteral("_schema_edit");
            if (!manager.renameItem(item, editedName))
                return failed(QStringLiteral("Schema round-trip could not edit a document"));
            edits.append(QJsonObject{
                {QStringLiteral("documentId"), id.toString(QUuid::WithoutBraces)},
                {QStringLiteral("jsonPath"), QStringLiteral("/content/name")},
                {QStringLiteral("before"), originalName},
                {QStringLiteral("after"), editedName},
            });
        }
    }
    if ((edit || migrate) && !manager.saveProject())
        return failed(QStringLiteral("Schema round-trip could not save the fixture"));
    if (!manager.closeProject())
        return failed(QStringLiteral("Schema round-trip could not close the fixture"));
    if (create || edit || migrate) {
        if (!manager.loadProject(projectPath, false) || !manager.currentProject())
            return failed(QStringLiteral("Schema round-trip could not reopen the fixture"));
        const auto reopenedItems = documentItems(*manager.currentProject());
        for (const QJsonValue& value : edits) {
            const QJsonObject entry = value.toObject();
            const QString id = entry.value(QStringLiteral("documentId")).toString();
            const bool root = id == manager.currentProject()->projectId;
            ProjectItem* item = reopenedItems.value(QUuid::fromString(id), nullptr);
            if ((!root && !item)
                || (root ? manager.currentProject()->description : item->name)
                    != entry.value(QStringLiteral("after")).toString())
                return failed(QStringLiteral("Schema round-trip edits did not survive reopening"));
        }
        if (!manager.closeProject())
            return failed(QStringLiteral("Schema round-trip could not close the reopened fixture"));
    }
    report.insert(QStringLiteral("edits"), edits);
    report.insert(QStringLiteral("ok"), true);
    return report;
}

}
