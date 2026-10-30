
#include "MigrationManager.h"
#include "ItemTypeCatalog.h"
#include "ProjectDataPolicy.h"
#include "ProjectDocument.h"
#include "ProjectContract.h"
#include "ProjectTypes.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonObject>
#include <QUuid>

namespace VNX {
namespace {

QStringList projectDocumentRelativePaths(const QString& projectDirectory,
                                         const QString& projectFile)
{
    const ProjectDataPolicy dataPolicy = ProjectDataPolicy::forProjectFile(projectFile);
    const QDir root(projectDirectory);
    QStringList paths{ProjectDataPolicy::normalizeRelativePath(
        root.relativeFilePath(projectFile))};
    for (const auto bucket : ItemTypeCatalog::allBuckets()) {
        QDirIterator files(root.filePath(ItemTypeCatalog::folderNameOf(bucket)),
                           {QStringLiteral("*.vp")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString relative = ProjectDataPolicy::normalizeRelativePath(
                root.relativeFilePath(files.next()));
            if (dataPolicy.classify(relative) == ProjectDataClass::Raw)
                paths.append(relative);
        }
    }
    paths.sort();
    paths.removeDuplicates();
    return paths;
}

void expandWriteSetPlaceholders(QStringList* writeSet,
                                const QString& projectDirectory,
                                const QString& projectFile)
{
    if (!writeSet)
        return;
    const QString relative = ProjectDataPolicy::normalizeRelativePath(
        QDir(projectDirectory).relativeFilePath(projectFile));
    QStringList expanded;
    bool sawAllDocuments = false;
    for (const QString& path : *writeSet) {
        if (path == QStringLiteral("<project-file>")) {
            if (!relative.isEmpty())
                expanded.append(relative);
        } else if (path == QStringLiteral("<all-documents>")) {
            sawAllDocuments = true;
        } else {
            expanded.append(path);
        }
    }
    if (sawAllDocuments)
        expanded.append(projectDocumentRelativePaths(projectDirectory, projectFile));
    expanded.sort();
    expanded.removeDuplicates();
    *writeSet = expanded;
}

}

MigrationManager::MigrationManager()
{
    for (const int sourceSchema : {7, 8}) {
        const int targetSchema = sourceSchema + 1;
        ProjectMigrationStep step;
        step.id = QStringLiteral("project-schema-%1-to-%2")
                      .arg(sourceSchema).arg(targetSchema);
        step.fromSchema = sourceSchema;
        step.toSchema = targetSchema;
        step.writeSet = {QStringLiteral("<all-documents>")};
        step.transform = [targetSchema](ProjectMigrationWorkspace& workspace,
                                       QStringList*, QString* error) {
            for (const QString& relative : workspace.writeSet()) {
                QJsonObject document;
                if (!workspace.readObject(relative, &document, error))
                    return false;
                QJsonObject header = document.value(QStringLiteral("harkenDocument")).toObject();
                header.insert(QStringLiteral("schemaVersion"), targetSchema);
                if (targetSchema == CURRENT_PROJECT_SCHEMA_VERSION) {
                    header = ProjectDocument::createHeader(
                        header.value(QStringLiteral("type")).toString(),
                        header.value(QStringLiteral("id")).toString(),
                        header.value(QStringLiteral("projectId")).toString(), header);
                    auto content = document.value(QStringLiteral("content")).toObject();
                    if (!ProjectDocument::normalizeContent(&content, error)) return false;
                    document.insert(QStringLiteral("content"), content);
                }
                document.insert(QStringLiteral("harkenDocument"), header);
                if (!workspace.writeObject(relative, document, error))
                    return false;
            }
            return true;
        };
        registry_.registerStep(std::move(step));
    }
}

MigrationManager::MigrationManager(ProjectMigrationRegistry registry)
    : registry_(std::move(registry))
{
}

MigrationManager& MigrationManager::instance()
{
    static MigrationManager inst;
    return inst;
}

int MigrationManager::detectVersion(const QJsonObject& root) const
{
    return root.value(QStringLiteral("harkenDocument")).toObject()
        .value(QStringLiteral("schemaVersion")).toInt(0);
}

bool MigrationManager::registerMigration(ProjectMigrationStep step, QString* error)
{
    return registry_.registerStep(std::move(step), error);
}

bool MigrationManager::validateSchema(const QJsonObject& root,
                                      ProjectData& project)
{
    Q_UNUSED(project)
    const int version = detectVersion(root);

    if (version != CURRENT_PROJECT_SCHEMA_VERSION) {
        qWarning() << "[MigrationManager] Project Schema" << version
                   << "does not match the required HarkenStudio Schema"
                   << CURRENT_PROJECT_SCHEMA_VERSION << "- refusing to open.";
        return false;
    }

    return true;
}

ProjectMigrationPlan MigrationManager::planProject(
    const QString& projectDirectory, const QString& projectFile,
    int sourceSchema, int targetSchema) const
{
    ProjectMigrationPlan plan = registry_.plan(sourceSchema, targetSchema,
                                                projectDirectory);
    if (!plan.ok)
        return plan;
    expandWriteSetPlaceholders(&plan.writeSet, projectDirectory, projectFile);
    for (ProjectMigrationStep& step : plan.steps)
        expandWriteSetPlaceholders(&step.writeSet, projectDirectory, projectFile);
    plan.fileCount = plan.writeSet.size();
    plan.recoveryBytes = 0;
    for (const QString& path : plan.writeSet) {
        const QFileInfo info(QDir(projectDirectory).filePath(path));
        if (info.isFile())
            plan.recoveryBytes += info.size();
    }
    return plan;
}

bool MigrationManager::migrateProject(const ProjectMigrationPlan& plan,
                                      const QString& sourceDirectory,
                                      const QString& stagingDirectory,
                                      const QString& projectFile,
                                      QStringList* warnings,
                                      QString* error,
                                      ProjectMigrationProgressCallback progress) const
{
    const QString relative = ProjectDataPolicy::normalizeRelativePath(
        QDir(sourceDirectory).relativeFilePath(projectFile));
    ProjectMigrationWorkspace workspace(sourceDirectory, stagingDirectory,
                                        plan.writeSet, relative);
    const auto validator = [this](ProjectMigrationWorkspace& migrationWorkspace,
                              int expectedVersion,
                              QString* validationError) {
        QJsonObject root;
        if (!migrationWorkspace.readObject(migrationWorkspace.projectFile(), &root,
                                           validationError))
            return false;
        const int actualVersion = root.value(QStringLiteral("harkenDocument"))
                                      .toObject()
                                      .value(QStringLiteral("schemaVersion"))
                                      .toInt(0);
        if (actualVersion != expectedVersion) {
            if (validationError)
                *validationError = QStringLiteral("PROJECT_MIGRATION_VERSION_MISMATCH");
            return false;
        }
        if (treeValidatorForTesting_)
            return treeValidatorForTesting_(migrationWorkspace.stagingDirectory(),
                                            expectedVersion, validationError);
        const QStringList errors = ProjectContractRegistry::instance().validateTree(
            migrationWorkspace.stagingDirectory(), root, false,
            migrationWorkspace.loadedObjects());
        if (errors.isEmpty())
            return true;
        if (validationError)
            *validationError = QStringLiteral("PROJECT_MIGRATION_VALIDATION_FAILED:")
                + errors.first();
        return false;
    };
    return registry_.execute(plan, workspace, warnings, error,
                             std::move(progress), validator);
}

void MigrationManager::backfillItemGuidAndPath(ProjectItem* item, ProjectItem* parent)
{
    if (!item) return;

    if (item->guid.isNull()) {
        item->guid = QUuid::createUuid();
    }
    if (item->objectPath.isEmpty()) {
        const QString parentPath = parent ? parent->objectPath : QString();
        item->objectPath = parentPath + QStringLiteral("\\") + item->name;
    }
    for (auto& child : item->children) {
        backfillItemGuidAndPath(child.get(), item);
    }
}

}
