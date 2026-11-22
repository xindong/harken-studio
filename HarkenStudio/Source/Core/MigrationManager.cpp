
#include "MigrationManager.h"
#include "ConversionMigration.h"
#include "AssetPipeline/PathUtils.h"
#include "ItemTypeCatalog.h"
#include "ProjectDataPolicy.h"
#include "ProjectDocument.h"
#include "ProjectContract.h"
#include "ProjectTypes.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

namespace harken::studio {
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

bool migrateLegacyContent(QJsonObject* content, QString* error)
{
    auto components = content->value(QStringLiteral("components")).toObject();
    auto mix = components.value(QStringLiteral("mixnode")).toObject();
    if (mix.value(QStringLiteral("effects")).isArray()) {
        auto effects = mix.value(QStringLiteral("effects")).toArray();
        for (qsizetype index = 0; index < effects.size(); ++index) {
            auto effect = effects[index].toObject();
            if (!effect.contains(QStringLiteral("type"))) continue;
            if (effect.contains(QStringLiteral("mode")) || effect.contains(QStringLiteral("custom"))) {
                if (error) *error = QStringLiteral("Conflicting legacy Effect fields");
                return false;
            }
            effect.insert(QStringLiteral("custom"), QJsonObject{
                {QStringLiteral("type"), effect.take(QStringLiteral("type"))},
                {QStringLiteral("parameters"), effect.take(QStringLiteral("parameters"))}});
            effect.insert(QStringLiteral("mode"), QStringLiteral("custom"));
            effects[index] = effect;
        }
        mix.insert(QStringLiteral("effects"), effects);
    }
    if (content->value(QStringLiteral("fieldGroups")).isObject()) {
        auto groups = content->value(QStringLiteral("fieldGroups")).toObject();
        const auto migrateOutput = [](QJsonObject* scope) {
            if (!scope->contains(QStringLiteral("mainOutput"))) return;
            auto output = scope->take(QStringLiteral("mainOutput")).toObject();
            if (output.value(QStringLiteral("values")).isObject()) {
                auto values = output.value(QStringLiteral("values")).toObject();
                if (values.contains(QStringLiteral("mainOutputBusRef")))
                    values.insert(QStringLiteral("outputBusRef"),
                                  values.take(QStringLiteral("mainOutputBusRef")));
                output.insert(QStringLiteral("values"), values);
            }
            scope->insert(QStringLiteral("output"), output);
        };
        auto shared = groups.value(QStringLiteral("shared")).toObject();
        migrateOutput(&shared);
        if (!shared.contains(QStringLiteral("effect")))
            shared.insert(QStringLiteral("effect"), QJsonObject{
                {QStringLiteral("inherit"), false},
                {QStringLiteral("values"), QJsonObject{
                    {QStringLiteral("effects"), mix.take(QStringLiteral("effects")).toArray()}}}});
        groups.insert(QStringLiteral("shared"), shared);
        if (groups.value(QStringLiteral("platforms")).isObject()) {
            auto platforms = groups.value(QStringLiteral("platforms")).toObject();
            for (const auto& platform : platforms.keys()) {
                auto scope = platforms.value(platform).toObject();
                migrateOutput(&scope);
                platforms.insert(platform, scope);
            }
            groups.insert(QStringLiteral("platforms"), platforms);
        }
        content->insert(QStringLiteral("fieldGroups"), groups);
    }
    if (components.contains(QStringLiteral("mixnode"))) {
        components.insert(QStringLiteral("mixnode"), mix);
        content->insert(QStringLiteral("components"), components);
    }
    if (content->value(QStringLiteral("type")) == QStringLiteral("Bus")) {
        auto properties = content->value(QStringLiteral("playableProperties")).toObject();
        const auto migrateReference = [](QJsonObject* values) {
            if (values->contains(QStringLiteral("mainOutputBusRef")))
                values->insert(QStringLiteral("outputBusRef"),
                               values->take(QStringLiteral("mainOutputBusRef")));
        };
        migrateReference(&properties);
        if (properties.value(QStringLiteral("platformOverrides")).isArray()) {
            auto overlays = properties.value(QStringLiteral("platformOverrides")).toArray();
            for (qsizetype index = 0; index < overlays.size(); ++index) {
                auto overlay = overlays[index].toObject();
                migrateReference(&overlay);
                overlays[index] = overlay;
            }
            properties.insert(QStringLiteral("platformOverrides"), overlays);
        }
        content->insert(QStringLiteral("playableProperties"), properties);
    }
    if (content->value(QStringLiteral("children")).isArray()) {
        auto children = content->value(QStringLiteral("children")).toArray();
        for (qsizetype index = 0; index < children.size(); ++index) {
            auto child = children[index].toObject();
            if (!migrateLegacyContent(&child, error)) return false;
            children[index] = child;
        }
        content->insert(QStringLiteral("children"), children);
    }
    return true;
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
            const auto paths = projectDocumentRelativePaths(workspace.sourceDirectory(),
                QDir(workspace.sourceDirectory()).filePath(workspace.projectFile()));
            for (const QString& relative : paths) {
                QJsonObject document;
                if (!workspace.readObject(relative, &document, error))
                    return false;
                QJsonObject header = document.value(QStringLiteral("harkenDocument")).toObject();
                header.insert(QStringLiteral("schemaVersion"), targetSchema);
                document.insert(QStringLiteral("harkenDocument"), header);
                if (!workspace.writeObject(relative, document, error))
                    return false;
            }
            return true;
        };
        registry_.registerStep(std::move(step));
    }
    ProjectMigrationStep conversion;
    conversion.id = QStringLiteral("project-schema-9-to-10");
    conversion.fromSchema = 9;
    conversion.toSchema = 10;
    conversion.writeSet = {QStringLiteral("<all-documents>")};
    conversion.transform = [](ProjectMigrationWorkspace&, QStringList*, QString* error) {
        if (error) *error = QStringLiteral("Conversion migration requires a project plan");
        return false;
    };
    registry_.registerStep(std::move(conversion));

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
    for (ProjectMigrationStep& step : plan.steps) {
        if (step.id != QStringLiteral("project-schema-9-to-10")) continue;
        QMap<QString, QJsonObject> documents;
        for (const auto& relative : step.writeSet) {
            QFile file(QDir(projectDirectory).filePath(relative));
            if (!file.open(QIODevice::ReadOnly)) {
                plan.ok = false;
                plan.error = QStringLiteral("Cannot read migration input: %1").arg(relative);
                return plan;
            }
            QJsonParseError parseError;
            const auto json = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
                plan.ok = false;
                plan.error = QStringLiteral("Invalid migration input: %1").arg(relative);
                return plan;
            }
            auto document = json.object();
            auto header = document.value(QStringLiteral("harkenDocument")).toObject();
            header = ProjectDocument::createHeader(header.value(QStringLiteral("type")).toString(),
                header.value(QStringLiteral("id")).toString(),
                header.value(QStringLiteral("projectId")).toString(), header);
            auto content = document.value(QStringLiteral("content")).toObject();
            if (header.value(QStringLiteral("type")) == QStringLiteral("project.root")
                && !content.contains(QStringLiteral("audio"))) {
                auto metadata = content.value(QStringLiteral("metadata")).toObject();
                content.insert(QStringLiteral("audio"), QJsonObject{
                    {QStringLiteral("renderSampleRate"), metadata.take(QStringLiteral("sampleRate"))},
                    {QStringLiteral("channelConfig"), metadata.take(QStringLiteral("channelConfig"))}});
                content.insert(QStringLiteral("deployPlatforms"), metadata.take(QStringLiteral("platforms")));
                content.insert(QStringLiteral("deployLanguages"), content.take(QStringLiteral("projectLanguages")));
                content.insert(QStringLiteral("metadata"), metadata);
            }
            if (!migrateLegacyContent(&content, &plan.error)
                || !ProjectDocument::normalizeContent(&content, &plan.error)) {
                plan.ok = false;
                return plan;
            }
            document.insert(QStringLiteral("harkenDocument"), header);
            document.insert(QStringLiteral("content"), content);
            documents.insert(relative, document);
        }
        const QString registryPath = QDir::fromNativeSeparators(QDir(projectDirectory).relativeFilePath(
            QDir(PathUtils::getAssetRegistryCacheDir(projectDirectory)).filePath(QStringLiteral("assets.json"))));
        QFile registryFile(QDir(projectDirectory).filePath(registryPath));
        QByteArray fingerprint;
        if (registryFile.exists()) {
            if (!registryFile.open(QIODevice::ReadOnly)) {
                plan.ok = false;
                plan.error = QStringLiteral("Cannot read the legacy asset registry");
                return plan;
            }
            fingerprint = QCryptographicHash::hash(registryFile.readAll(), QCryptographicHash::Sha256);
        }
        plan.inputFingerprints.insert(registryPath, fingerprint);
        QStringList migrationWarnings;
        const QString relativeProject = QDir::fromNativeSeparators(QDir(projectDirectory).relativeFilePath(projectFile));
        if (!prepareConversionMigration(projectDirectory, relativeProject, documents,
                                         plan.inputFingerprints, &migrationWarnings, &plan.error)) {
            plan.ok = false;
            return plan;
        }
        step.writeSet = documents.keys();
        plan.writeSet.append(step.writeSet);
        plan.writeSet.sort();
        plan.writeSet.removeDuplicates();
        step.transform = [documents, migrationWarnings](ProjectMigrationWorkspace& workspace,
                                                        QStringList* warnings, QString* error) {
            for (auto entry = documents.cbegin(); entry != documents.cend(); ++entry)
                if (!workspace.writeObject(entry.key(), entry.value(), error)) return false;
            if (warnings) warnings->append(migrationWarnings);
            return true;
        };
    }
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
        const QStringList errors = ProjectContractRegistry::instance().validateSnapshot(
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

}
