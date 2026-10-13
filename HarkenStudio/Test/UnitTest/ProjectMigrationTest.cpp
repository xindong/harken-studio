#include "Core/ProjectMigration.h"
#include "Core/MigrationManager.h"
#include "Core/ProjectFormatVersion.h"
#include "Core/ProjectSchemaValidator.h"
#include "Core/ProjectUpgradeManager.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <utility>

namespace {

void writeObject(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QJsonDocument(object).toJson()), 0);
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
}

TEST(ProjectMigration, TenVersionJumpUsesOneReadAndOneWrite)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString relative = QStringLiteral("Demo.hsp");
    writeObject(source.filePath(relative),
                QJsonObject{{QStringLiteral("projectSchemaVersion"), 1},
                            {QStringLiteral("migrationCount"), 0}});

    VNX::ProjectMigrationRegistry registry;
    for (int version = 1; version <= 10; ++version) {
        VNX::ProjectMigrationStep step;
        step.id = QStringLiteral("project-%1-to-%2").arg(version).arg(version + 1);
        step.fromSchema = version;
        step.toSchema = version + 1;
        step.writeSet = {relative};
        step.transform = [relative, version](VNX::ProjectMigrationWorkspace& workspace,
                                             QStringList*, QString* error) {
            QJsonObject root;
            if (!workspace.readObject(relative, &root, error))
                return false;
            root.insert(QStringLiteral("projectSchemaVersion"), version + 1);
            root.insert(QStringLiteral("migrationCount"),
                        root.value(QStringLiteral("migrationCount")).toInt() + 1);
            return workspace.writeObject(relative, root, error);
        };
        ASSERT_TRUE(registry.registerStep(std::move(step)));
    }

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 11, source.path());
    ASSERT_TRUE(plan.ok) << plan.error.toStdString();
    ASSERT_EQ(plan.steps.size(), 10u);
    ASSERT_EQ(plan.writeSet, QStringList{relative});
    VNX::ProjectMigrationWorkspace workspace(source.path(), staging.path(),
                                             plan.writeSet);
    QString error;
    ASSERT_TRUE(registry.execute(plan, workspace, nullptr, &error))
        << error.toStdString();
    EXPECT_EQ(workspace.parseCount(relative), 1);
    EXPECT_EQ(workspace.writeCount(relative), 1);
    const QJsonObject result = readObject(staging.filePath(relative));
    EXPECT_EQ(result.value(QStringLiteral("projectSchemaVersion")).toInt(), 11);
    EXPECT_EQ(result.value(QStringLiteral("migrationCount")).toInt(), 10);
}

TEST(ProjectMigration, EmptyRegistryAcceptsCurrentVersion)
{
    VNX::ProjectMigrationRegistry registry;
    const VNX::ProjectMigrationPlan plan = registry.plan(1, 1);
    EXPECT_TRUE(plan.ok) << plan.error.toStdString();
    EXPECT_TRUE(plan.steps.empty());
    EXPECT_TRUE(plan.writeSet.isEmpty());
}

TEST(ProjectMigration, EveryVersionBoundaryIsValidated)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString relative = QStringLiteral("Demo.hsp");
    writeObject(source.filePath(relative),
                QJsonObject{{QStringLiteral("schemaVersion"), 1}});

    VNX::ProjectMigrationRegistry registry;
    for (int version = 1; version <= 2; ++version) {
        VNX::ProjectMigrationStep step;
        step.id = QStringLiteral("project-%1-to-%2").arg(version).arg(version + 1);
        step.fromSchema = version;
        step.toSchema = version + 1;
        step.writeSet = {relative};
        step.transform = [relative, version](VNX::ProjectMigrationWorkspace& workspace,
                                             QStringList*, QString* error) {
            QJsonObject root;
            if (!workspace.readObject(relative, &root, error))
                return false;
            root.insert(QStringLiteral("schemaVersion"), version + 1);
            return workspace.writeObject(relative, root, error);
        };
        ASSERT_TRUE(registry.registerStep(std::move(step)));
    }

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 3, source.path());
    VNX::ProjectMigrationWorkspace workspace(source.path(), staging.path(),
                                             plan.writeSet, relative);
    QList<int> validatedVersions;
    QString error;
    ASSERT_TRUE(registry.execute(
        plan, workspace, nullptr, &error, {},
        [&validatedVersions, &relative](VNX::ProjectMigrationWorkspace& current,
                                        int expected,
                                        QString* validationError) {
            QJsonObject root;
            if (!current.readObject(relative, &root, validationError))
                return false;
            validatedVersions.append(expected);
            return root.value(QStringLiteral("schemaVersion")).toInt() == expected;
        })) << error.toStdString();
    EXPECT_EQ(validatedVersions, (QList<int>{1, 2, 2, 3}));
    EXPECT_EQ(workspace.parseCount(relative), 1);
    EXPECT_EQ(workspace.writeCount(relative), 1);
}

TEST(ProjectMigration, WrongTargetVersionStopsBeforeFlush)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString relative = QStringLiteral("Demo.hsp");
    writeObject(source.filePath(relative),
                QJsonObject{{QStringLiteral("schemaVersion"), 1}});

    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("project-1-to-2");
    step.fromSchema = 1;
    step.toSchema = 2;
    step.writeSet = {relative};
    step.transform = [relative](VNX::ProjectMigrationWorkspace& workspace,
                                QStringList*, QString* error) {
        QJsonObject root;
        if (!workspace.readObject(relative, &root, error))
            return false;
        root.insert(QStringLiteral("schemaVersion"), 99);
        return workspace.writeObject(relative, root, error);
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2, source.path());
    VNX::ProjectMigrationWorkspace workspace(source.path(), staging.path(),
                                             plan.writeSet, relative);
    QString error;
    EXPECT_FALSE(registry.execute(
        plan, workspace, nullptr, &error, {},
        [&relative](VNX::ProjectMigrationWorkspace& current, int expected,
                    QString* validationError) {
            QJsonObject root;
            if (!current.readObject(relative, &root, validationError))
                return false;
            if (root.value(QStringLiteral("schemaVersion")).toInt() == expected)
                return true;
            if (validationError)
                *validationError = QStringLiteral("wrong target version");
            return false;
        }));
    EXPECT_EQ(error, QStringLiteral("wrong target version"));
    EXPECT_FALSE(QFileInfo::exists(staging.filePath(relative)));
}

TEST(ProjectMigration, RetiredGenerationsHaveNoProductionMigration)
{
    VNX::MigrationManager migrations;
    for (int version = 1; version < VNX::kCurrentProjectSchemaVersion; ++version) {
        if (VNX::ProjectSchemaValidator::supportsProjectVersion(version))
            continue;
        const auto plan = migrations.planProject({}, {}, version,
                                                VNX::kCurrentProjectSchemaVersion);
        EXPECT_FALSE(plan.ok);
        EXPECT_EQ(plan.error, QStringLiteral("PROJECT_MIGRATION_MISSING"));
    }
}

QString writeCurrentProjectWithDocument(const QTemporaryDir& directory)
{
    const QString projectPath = directory.filePath(QStringLiteral("Baseline.hsp"));
    const QString packGuid = QStringLiteral("aaaaaaaa-1111-2222-3333-444444444444");
    writeObject(projectPath,
                HarkenTest::projectRoot(QJsonObject{
                    {QStringLiteral("metadata"), QJsonObject{
                        {QStringLiteral("name"), QStringLiteral("Baseline")},
                    }},
                    {QStringLiteral("paths"), QJsonObject{
                        {QStringLiteral("audioFolder"), QStringLiteral("Layers")},
                    }},
                }, VNX::kCurrentProjectSchemaVersion));
    QDir().mkpath(directory.filePath(QStringLiteral("Layers")));
    writeObject(directory.filePath(QStringLiteral("Layers/Main.vp")),
                HarkenTest::projectDocument(
                    QStringLiteral("project.layer"), packGuid, HarkenTest::kProjectId,
                    QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("Layer")},
                        {QStringLiteral("guid"), packGuid},
                        {QStringLiteral("name"), QStringLiteral("Main")},
                        {QStringLiteral("parentRef"), QJsonObject{
                            {QStringLiteral("kind"), QStringLiteral("root")},
                        }},
                        {QStringLiteral("children"), QJsonArray{}},
                        {QStringLiteral("order"), 0},
                    },
                    VNX::kCurrentProjectSchemaVersion));
    EXPECT_TRUE(HarkenTest::writeRootBusForProject(projectPath));
    return projectPath;
}

TEST(ProjectMigration, CurrentGenerationNeedsNoProductionMigration)
{
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    const QString projectPath = writeCurrentProjectWithDocument(source);
    const QJsonObject before = readObject(projectPath);
    VNX::ProjectUpgradeManager manager;
    const auto analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok) << analysis.blockingErrors.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(analysis.upgradeRequired);
    const auto result = manager.execute(analysis);
    EXPECT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(readObject(projectPath), before);
}

TEST(ProjectMigration, AllDocumentsPlaceholderSkipsTrashAndDerivedFiles)
{
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    const QString projectPath = writeCurrentProjectWithDocument(source);
    ASSERT_TRUE(QDir().mkpath(source.filePath(QStringLiteral(".trash/Layers"))));
    ASSERT_TRUE(QDir().mkpath(source.filePath(QStringLiteral("GeneratedPacks"))));
    ASSERT_TRUE(QDir().mkpath(source.filePath(QStringLiteral("BuildPacks"))));
    writeObject(source.filePath(QStringLiteral(".trash/Layers/Deleted.vp")),
                QJsonObject{{QStringLiteral("deleted"), true}});
    writeObject(source.filePath(QStringLiteral("GeneratedPacks/Stale.vp")),
                QJsonObject{{QStringLiteral("stale"), true}});
    writeObject(source.filePath(QStringLiteral("BuildPacks/Stale.vp")),
                QJsonObject{{QStringLiteral("stale"), true}});

    VNX::MigrationManager migrations;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("test-current-to-next");
    step.fromSchema = VNX::kCurrentProjectSchemaVersion;
    step.toSchema = VNX::kCurrentProjectSchemaVersion + 1;
    step.writeSet = {QStringLiteral("<all-documents>")};
    step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) {
        return true;
    };
    ASSERT_TRUE(migrations.registerMigration(std::move(step)));
    const VNX::ProjectMigrationPlan plan = migrations.planProject(
        source.path(), projectPath, VNX::kCurrentProjectSchemaVersion,
        VNX::kCurrentProjectSchemaVersion + 1);
    ASSERT_TRUE(plan.ok) << plan.error.toStdString();
    EXPECT_TRUE(plan.writeSet.contains(QStringLiteral("Baseline.hsp")));
    EXPECT_TRUE(plan.writeSet.contains(QStringLiteral("Layers/Main.vp")));
    EXPECT_TRUE(plan.writeSet.contains(QStringLiteral("Busses/Root.vp")));
    EXPECT_FALSE(plan.writeSet.contains(QStringLiteral(".trash/Layers/Deleted.vp")));
    EXPECT_FALSE(plan.writeSet.contains(QStringLiteral("GeneratedPacks/Stale.vp")));
    EXPECT_FALSE(plan.writeSet.contains(QStringLiteral("BuildPacks/Stale.vp")));
    EXPECT_EQ(plan.fileCount, plan.writeSet.size());
}

TEST(ProjectMigration, OmittingDocumentsFromWriteSetFailsStagingValidation)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = writeCurrentProjectWithDocument(source);
    const QString layerFile = QStringLiteral("Layers/Main.vp");
    ASSERT_TRUE(QDir().mkpath(staging.filePath(QStringLiteral("Layers"))));
    ASSERT_TRUE(QFile::copy(source.filePath(layerFile), staging.filePath(layerFile)));

    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("test-current-to-next.root-only");
    step.fromSchema = VNX::kCurrentProjectSchemaVersion;
    step.toSchema = VNX::kCurrentProjectSchemaVersion + 1;
    step.writeSet = {QStringLiteral("<project-file>")};
    step.transform = [](VNX::ProjectMigrationWorkspace& workspace,
                        QStringList*, QString* error) {
        QJsonObject root;
        if (!workspace.readObject(workspace.projectFile(), &root, error))
            return false;
        QJsonObject header = root.value(QStringLiteral("harkenDocument")).toObject();
        header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion + 1);
        header.insert(QStringLiteral("savedStudioVersion"), QStringLiteral("1.0.0"));
        root.insert(QStringLiteral("harkenDocument"), header);
        return workspace.writeObject(workspace.projectFile(), root, error);
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));

    VNX::MigrationManager migrations(std::move(registry));
    QList<int> validatedVersions;
    migrations.setTreeValidatorForTesting(
        [&validatedVersions, layerFile](const QString& directory, int version, QString* error) {
            validatedVersions.append(version);
            const QJsonObject header = HarkenTest::projectHeader(
                readObject(QDir(directory).filePath(layerFile)));
            if (header.value(QStringLiteral("schemaVersion")).toInt() == version)
                return true;
            if (error) *error = layerFile + QStringLiteral(": document was not migrated");
            return false;
        });
    const VNX::ProjectMigrationPlan plan = migrations.planProject(
        source.path(), projectPath, VNX::kCurrentProjectSchemaVersion,
        VNX::kCurrentProjectSchemaVersion + 1);
    ASSERT_TRUE(plan.ok) << plan.error.toStdString();
    EXPECT_EQ(plan.writeSet, QStringList{QStringLiteral("Baseline.hsp")});

    QString error;
    EXPECT_FALSE(migrations.migrateProject(
        plan, source.path(), staging.path(), projectPath, nullptr, &error));
    EXPECT_EQ(error, layerFile + QStringLiteral(": document was not migrated"));
    EXPECT_EQ(validatedVersions, QList<int>({VNX::kCurrentProjectSchemaVersion,
                                            VNX::kCurrentProjectSchemaVersion + 1}));
    EXPECT_FALSE(QFileInfo::exists(staging.filePath(QStringLiteral("Baseline.hsp"))));
    EXPECT_EQ(readObject(staging.filePath(layerFile)), readObject(source.filePath(layerFile)));
}

TEST(ProjectMigration, MissingAdjacentVersionStopsTheWholePlan)
{
    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("one-to-two");
    step.fromSchema = 1;
    step.toSchema = 2;
    step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) {
        return true;
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));
    const VNX::ProjectMigrationPlan plan = registry.plan(1, 3);
    EXPECT_FALSE(plan.ok);
    EXPECT_EQ(plan.error, QStringLiteral("PROJECT_MIGRATION_MISSING"));
}

TEST(ProjectMigration, WriteOutsideDeclaredSetIsRejected)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    VNX::ProjectMigrationWorkspace workspace(
        source.path(), staging.path(), {QStringLiteral("Demo.hsp")});
    QString error;
    EXPECT_FALSE(workspace.writeObject(QStringLiteral("AudioSources/source.wav"),
                                       QJsonObject{}, &error));
    EXPECT_EQ(error, QStringLiteral("PROJECT_MIGRATION_WRITE_OUTSIDE_PLAN"));
}

TEST(ProjectMigration, MissingDependencyStopsThePlan)
{
    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("root");
    step.fromSchema = 1;
    step.toSchema = 2;
    step.dependencies = {QStringLiteral("missing")};
    step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) {
        return true;
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2);
    EXPECT_FALSE(plan.ok);
    EXPECT_EQ(plan.error, QStringLiteral("PROJECT_MIGRATION_DEPENDENCY_MISSING"));
}

TEST(ProjectMigration, CyclicDependenciesStopThePlan)
{
    VNX::ProjectMigrationRegistry registry;
    for (const QString& id : {QStringLiteral("first"), QStringLiteral("second")}) {
        VNX::ProjectMigrationStep step;
        step.id = id;
        step.fromSchema = 1;
        step.toSchema = 2;
        step.dependencies = {id == QStringLiteral("first")
                                 ? QStringLiteral("second")
                                 : QStringLiteral("first")};
        step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) {
            return true;
        };
        ASSERT_TRUE(registry.registerStep(std::move(step)));
    }

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2);
    EXPECT_FALSE(plan.ok);
    EXPECT_EQ(plan.error, QStringLiteral("PROJECT_MIGRATION_DEPENDENCY_INVALID"));
}

TEST(ProjectMigration, ProgressComesFromPlannedStepsAndFiles)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString relative = QStringLiteral("Project.hsp");
    writeObject(source.filePath(relative), QJsonObject{{QStringLiteral("value"), 1}});

    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("root");
    step.fromSchema = 1;
    step.toSchema = 2;
    step.writeSet = {relative};
    step.transform = [relative](VNX::ProjectMigrationWorkspace& workspace,
                                QStringList*, QString* error) {
        QJsonObject root;
        return workspace.readObject(relative, &root, error)
            && workspace.writeObject(relative, root, error);
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));
    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2, source.path());
    VNX::ProjectMigrationWorkspace workspace(source.path(), staging.path(),
                                             plan.writeSet, relative);
    std::vector<VNX::ProjectMigrationProgress> updates;
    QString error;
    ASSERT_TRUE(registry.execute(
        plan, workspace, nullptr, &error,
        [&updates](const VNX::ProjectMigrationProgress& value) {
            updates.push_back(value);
        })) << error.toStdString();
    ASSERT_GE(updates.size(), 3u);
    EXPECT_EQ(updates.back().completedSteps, 1);
    EXPECT_EQ(updates.back().totalSteps, 1);
    EXPECT_EQ(updates.back().completedFiles, 1);
    EXPECT_EQ(updates.back().totalFiles, 1);
    EXPECT_EQ(updates.back().completedBytes, updates.back().totalBytes);
}

}
