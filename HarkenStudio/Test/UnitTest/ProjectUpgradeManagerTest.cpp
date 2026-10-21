#include "Core/ProjectUpgradeManager.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/MigrationManager.h"
#include "Core/ProjectMigration.h"
#include "Core/ProjectTransaction.h"
#include "Core/ProjectFormatVersion.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectOpenProtocol.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectSchemaValidator.h"
#include "Core/FieldGroupJson.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <array>
#include <stdexcept>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

QString writeJson(const QString& path, const QJsonObject& object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    file.close();
    return path;
}

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

QString createBaselineProject(const QTemporaryDir& directory)
{
    const QString projectPath = directory.filePath(QStringLiteral("Baseline.hsp"));
    const QString packGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::string groupJson;
    std::string groupError;
    EXPECT_TRUE(VNX::WriteFieldGroupsJson(VNX::MakeRootFieldGroups(
        VNX::toFieldGroupGuid(QUuid(QStringLiteral("99999999-9999-4999-8999-999999999999")))),
        groupJson, groupError)) << groupError;
    writeJson(projectPath,
              HarkenTest::projectRoot(QJsonObject{
                  {QStringLiteral("metadata"), QJsonObject{
                      {QStringLiteral("name"), QStringLiteral("Baseline")},
                  }},
                  {QStringLiteral("paths"), QJsonObject{
                      {QStringLiteral("audioFolder"), QStringLiteral("Layers")},
                  }},
                  {QStringLiteral("extensions"), QJsonObject{
                      {QStringLiteral("test"), QJsonObject{
                          {QStringLiteral("payload"), QStringLiteral("must-survive")},
                      }},
                  }},
              }));
    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")),
              HarkenTest::projectDocument(
                  QStringLiteral("project.layer"), packGuid,
                  HarkenTest::kProjectId, QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("Layer")},
                  {QStringLiteral("guid"), packGuid},
                  {QStringLiteral("name"), QStringLiteral("Main")},
                  {QStringLiteral("fieldGroups"),
                   QJsonDocument::fromJson(QByteArray::fromStdString(groupJson)).object()},
                  {QStringLiteral("parentRef"), QJsonObject{
                      {QStringLiteral("kind"), QStringLiteral("root")},
                  }},
                  {QStringLiteral("children"), QJsonArray{}},
                  {QStringLiteral("order"), 0},
              }));
    EXPECT_TRUE(HarkenTest::writeRootBusForProject(projectPath));
    return projectPath;
}

TEST(ProjectUpgradeManager, FormalBaselineOpensWithoutUpgrade)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok);
    ASSERT_FALSE(analysis.upgradeRequired);

    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_FALSE(result.changed);
    ASSERT_TRUE(result.backupPath.isEmpty());

    const QJsonObject upgraded = readJson(projectPath);
    const QJsonObject header = HarkenTest::projectHeader(upgraded);
    const QJsonObject content = HarkenTest::projectContent(upgraded);
    EXPECT_EQ(header.value(QStringLiteral("id")).toString(),
              header.value(QStringLiteral("projectId")).toString());
    EXPECT_EQ(content.value(QStringLiteral("extensions")).toObject()
                  .value(QStringLiteral("test")).toObject()
                  .value(QStringLiteral("payload")).toString(),
              QStringLiteral("must-survive"));

    const VNX::ProjectUpgradeAnalysis current = manager.analyze(projectPath);
    EXPECT_TRUE(current.ok);
    EXPECT_FALSE(current.upgradeRequired);

    ASSERT_TRUE(manager.rollback(result));
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, AnalysisBlocksUnfinishedTransactionRecovery)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    QString error;
    const std::unique_ptr<VNX::ProjectTransaction> transaction =
        VNX::ProjectTransaction::begin(
            projectPath, {QStringLiteral("Baseline.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();

    const VNX::ProjectUpgradeAnalysis analysis =
        VNX::ProjectUpgradeManager().analyzeForProjectOpen(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_EQ(analysis.issue,
              VNX::ProjectIssue::ProjectUpgradeRecoveryRequired);
    EXPECT_EQ(analysis.projectFilePath, projectPath);
}

TEST(ProjectUpgradeManager, UpgradeRejectsNonWritableDocumentsBeforeMutation)
{
    for (const QString& relative : {QStringLiteral("Baseline.hsp"),
                                    QStringLiteral("Layers/Main.vp")}) {
        SCOPED_TRACE(relative.toStdString());
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const QString projectPath = createBaselineProject(directory);
        const QString protectedPath = directory.filePath(relative);
        const QByteArray projectBefore = readBytes(projectPath);
        const QByteArray protectedBefore = readBytes(protectedPath);
        const auto permissions = QFile::permissions(protectedPath);
        const auto restore = qScopeGuard([&]() { QFile::setPermissions(protectedPath, permissions); });
        VNX::MigrationManager migrations;
        VNX::ProjectMigrationStep step;
        step.id = QStringLiteral("test-write-access");
        step.fromSchema = VNX::kCurrentProjectSchemaVersion;
        step.toSchema = VNX::kCurrentProjectSchemaVersion + 1;
        step.writeSet = {QStringLiteral("<all-documents>")};
        bool migrationStarted = false;
        step.transform = [&](VNX::ProjectMigrationWorkspace& workspace, QStringList*, QString* error) {
            migrationStarted = true;
            for (const QString& path : workspace.writeSet()) {
                QJsonObject document;
                if (!workspace.readObject(path, &document, error))
                    return false;
                QJsonObject header = HarkenTest::projectHeader(document);
                header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion + 1);
                document.insert(QStringLiteral("harkenDocument"), header);
                if (!workspace.writeObject(path, document, error))
                    return false;
            }
            return true;
        };
        ASSERT_TRUE(migrations.registerMigration(std::move(step)));
        VNX::ProjectUpgradeManager manager(&migrations);
        manager.setTargetSchemaForTesting(VNX::kCurrentProjectSchemaVersion + 1);
        const auto approved = manager.analyzeForProjectOpen(projectPath);
        ASSERT_TRUE(approved.ok);
        ASSERT_TRUE(QFile::setPermissions(protectedPath,
            permissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                            | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
        const auto blockedPermissions = QFile::permissions(protectedPath);
        const auto blocked = manager.analyzeForProjectOpen(projectPath);
        EXPECT_FALSE(blocked.ok);
        EXPECT_EQ(blocked.issue, VNX::ProjectIssue::ProjectUpgradeFilesNotWritable);
        EXPECT_TRUE(blocked.detail.contains(QDir::toNativeSeparators(protectedPath)));
        const auto report = VNX::projectOpenAnalysisReport(blocked, QStringLiteral("request"));
        EXPECT_EQ(report.value("errorCode").toString(), QStringLiteral("PROJECT_UPGRADE_FILES_NOT_WRITABLE"));
        EXPECT_TRUE(report.value("technicalDetails").toString().contains(QDir::toNativeSeparators(protectedPath)));
        bool copyingStarted = false;
        const auto result = manager.execute(approved, {}, [&](const VNX::ProjectUpgradeProgress& progress) {
            copyingStarted |= progress.phase == VNX::ProjectUpgradePhase::CopyingStaging;
        });
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.issue, VNX::ProjectIssue::ProjectUpgradeFilesNotWritable);
        EXPECT_FALSE(copyingStarted);
        EXPECT_FALSE(migrationStarted);
        EXPECT_TRUE(result.backupPath.isEmpty());
        EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
        EXPECT_EQ(readBytes(projectPath), projectBefore);
        EXPECT_EQ(readBytes(protectedPath), protectedBefore);
        EXPECT_EQ(QFile::permissions(protectedPath), blockedPermissions);
        EXPECT_TRUE(VNX::ProjectUpgradeManager().analyzeForProjectOpen(projectPath).ok);
        ASSERT_TRUE(QFile::setPermissions(protectedPath, permissions));
        EXPECT_TRUE(manager.analyzeForProjectOpen(projectPath).ok);
        migrations.setTreeValidatorForTesting([](const QString&, int, QString*) { return true; });
        manager.setProjectTreeValidatorForTesting(
            [](const QString&, const QJsonObject&, bool) { return QStringList{}; });
        const auto changedWhileStaging = manager.execute(approved, [&](const QString&, QString*) {
            EXPECT_TRUE(QFile::setPermissions(protectedPath, blockedPermissions));
            return true;
        });
        EXPECT_TRUE(migrationStarted);
        EXPECT_FALSE(changedWhileStaging.ok);
        EXPECT_EQ(changedWhileStaging.issue, VNX::ProjectIssue::ProjectUpgradeFilesNotWritable)
            << changedWhileStaging.error.toStdString();
        EXPECT_TRUE(changedWhileStaging.backupPath.isEmpty());
        EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
        EXPECT_EQ(readBytes(projectPath), projectBefore);
        EXPECT_EQ(readBytes(protectedPath), protectedBefore);
    }
}

TEST(ProjectUpgradeManager, ReadOnlyFilesOutsideTheWriteSetDoNotBlockUpgradeAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString untouchedPath = directory.filePath(QStringLiteral("notes.txt"));
    QFile untouched(untouchedPath);
    ASSERT_TRUE(untouched.open(QIODevice::WriteOnly));
    untouched.write("preserved data");
    untouched.close();
    const auto permissions = QFile::permissions(untouchedPath);
    const auto restore = qScopeGuard([&]() { QFile::setPermissions(untouchedPath, permissions); });
    ASSERT_TRUE(QFile::setPermissions(untouchedPath,
        permissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                        | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    VNX::MigrationManager migrations;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("test-affected-files-only");
    step.fromSchema = VNX::kCurrentProjectSchemaVersion;
    step.toSchema = VNX::kCurrentProjectSchemaVersion + 1;
    step.writeSet = {QStringLiteral("<all-documents>")};
    step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) { return true; };
    ASSERT_TRUE(migrations.registerMigration(std::move(step)));
    VNX::ProjectUpgradeManager manager(&migrations);
    manager.setTargetSchemaForTesting(VNX::kCurrentProjectSchemaVersion + 1);
    const auto analysis = manager.analyzeForProjectOpen(projectPath);
    EXPECT_TRUE(analysis.ok) << analysis.detail.toStdString();
    EXPECT_EQ(readBytes(untouchedPath), QByteArrayLiteral("preserved data"));
}

TEST(ProjectSerializer, ReadOnlyUpgradeFailureRetainsTheAffectedFile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    for (const QString& relative : {QStringLiteral("Baseline.hsp"),
                                    QStringLiteral("Layers/Main.vp"),
                                    QStringLiteral("Busses/Root.vp")}) {
        const QString path = directory.filePath(relative);
        QJsonObject document = readJson(path);
        QJsonObject header = HarkenTest::projectHeader(document);
        header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion - 1);
        document.insert(QStringLiteral("harkenDocument"), header);
        writeJson(path, document);
    }
    ASSERT_TRUE(VNX::ProjectUpgradeManager().analyzeForProjectOpen(projectPath).ok);
    const auto permissions = QFile::permissions(projectPath);
    const auto restore = qScopeGuard([&]() { QFile::setPermissions(projectPath, permissions); });
    ASSERT_TRUE(QFile::setPermissions(projectPath,
        permissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                        | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    EXPECT_FALSE(serializer.loadProject(projectPath, project, true));
    EXPECT_TRUE(serializer.lastError().startsWith(QStringLiteral("PROJECT_UPGRADE_FILES_NOT_WRITABLE:")));
    EXPECT_TRUE(serializer.lastError().contains(QDir::toNativeSeparators(projectPath)));
    EXPECT_EQ(HarkenTest::projectHeader(readJson(projectPath)).value("schemaVersion").toInt(),
              VNX::kCurrentProjectSchemaVersion - 1);
}

TEST(ProjectUpgradeManager, FailedAnalysisKeepsRequestedProjectPath)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(
        QStringLiteral("Missing.hsp"));

    const VNX::ProjectUpgradeAnalysis analysis =
        VNX::ProjectUpgradeManager().analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_EQ(analysis.projectFilePath,
              QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath()));
}

TEST(ProjectUpgradeManager, RetiredGenerationsAreRejectedWithoutChangingAnyDocument)
{
    for (int version = 1; version < VNX::kCurrentProjectSchemaVersion; ++version) {
        if (VNX::ProjectSchemaValidator::supportsProjectVersion(version))
            continue;
        for (bool savedVersion : {false, true}) {
            SCOPED_TRACE(::testing::Message() << version << ":" << savedVersion);
            QTemporaryDir directory;
            ASSERT_TRUE(directory.isValid());
            const QString projectPath = createBaselineProject(directory);
            QMap<QString, QByteArray> originals;
            QDirIterator files(directory.path(), {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                               QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const QString path = files.next();
                QJsonObject document = readJson(path);
                QJsonObject header = HarkenTest::projectHeader(document);
                header.insert(QStringLiteral("schemaVersion"), version);
                if (!savedVersion) header.remove(QStringLiteral("savedStudioVersion"));
                document.insert(QStringLiteral("harkenDocument"), header);
                writeJson(path, document);
                originals.insert(path, readBytes(path));
            }
            VNX::ProjectUpgradeManager manager;
            const auto analysis = manager.analyze(projectPath);
            EXPECT_FALSE(analysis.ok);
            EXPECT_FALSE(analysis.upgradeRequired);
            EXPECT_EQ(analysis.issue, VNX::ProjectIssue::ProjectSchemaUnsupported);
            EXPECT_EQ(analysis.sourceSchema, version);
            EXPECT_EQ(analysis.targetSchema, VNX::kCurrentProjectSchemaVersion);
            EXPECT_FALSE(manager.execute(analysis).ok);
            VNX::ProjectSerializer serializer;
            VNX::ProjectData project;
            bool approvalAsked = false;
            EXPECT_FALSE(serializer.loadProject(projectPath, project, true, {},
                [&](const VNX::ProjectUpgradeAnalysis&) { approvalAsked = true; return true; }));
            EXPECT_FALSE(approvalAsked);
            EXPECT_TRUE(serializer.lastError().startsWith(QStringLiteral("PROJECT_SCHEMA_UNSUPPORTED")));
            for (auto it = originals.cbegin(); it != originals.cend(); ++it)
                EXPECT_EQ(readBytes(it.key()), it.value());
            EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
        }
    }
}

TEST(ProjectUpgradeManager, InjectedMigrationRunsThroughValidationAndTransaction)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QStringList backupFiles{
        QStringLiteral("Other.hsp"), QStringLiteral("Backups/Old.hsp"),
        QStringLiteral("Layers/Nested/Old.hsp")};
    QMap<QString, QByteArray> backupBytes;
    for (const QString& relative : backupFiles) {
        const QString path = directory.filePath(relative);
        ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
        ASSERT_TRUE(QFile::copy(projectPath, path));
        backupBytes.insert(relative, readBytes(path));
    }

    const QStringList documents{QStringLiteral("Baseline.hsp"),
                                QStringLiteral("Layers/Main.vp"),
                                QStringLiteral("Busses/Root.vp")};
    QMap<QString, QByteArray> originals;
    for (const QString& path : documents)
        originals.insert(path, readBytes(directory.filePath(path)));
    const auto validateDocuments = [documents](const QString& root, int expectedVersion) {
        QStringList errors;
        for (const QString& path : documents) {
            const QJsonObject document = readJson(QDir(root).filePath(path));
            const auto header = HarkenTest::projectHeader(document);
            if (header.value(QStringLiteral("schemaVersion")).toInt() != expectedVersion)
                errors.append(path + QStringLiteral(": unexpected test generation"));
        }
        return errors;
    };
    QList<int> validatedVersions;
    VNX::MigrationManager migrations;
    migrations.setTreeValidatorForTesting(
        [validateDocuments, &validatedVersions](const QString& root, int version, QString* error) {
            validatedVersions.append(version);
            const auto errors = validateDocuments(root, VNX::kCurrentProjectSchemaVersion);
            if (error) *error = errors.join(QLatin1Char('\n'));
            return errors.isEmpty();
        });
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("test-current-to-next");
    step.fromSchema = VNX::kCurrentProjectSchemaVersion;
    step.toSchema = VNX::kCurrentProjectSchemaVersion + 1;
    step.writeSet = {QStringLiteral("<all-documents>")};
    step.transform = [](VNX::ProjectMigrationWorkspace& workspace,
                        QStringList*, QString* error) {
        for (const QString& path : workspace.writeSet()) {
            QJsonObject document;
            if (!workspace.readObject(path, &document, error)) return false;
            QJsonObject header = HarkenTest::projectHeader(document);
            header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion + 1);
            document.insert(QStringLiteral("harkenDocument"), header);
            if (!workspace.writeObject(path, document, error)) return false;
        }
        return true;
    };
    ASSERT_TRUE(migrations.registerMigration(std::move(step)));

    VNX::ProjectUpgradeManager manager(&migrations);
    manager.setTargetSchemaForTesting(VNX::kCurrentProjectSchemaVersion + 1);
    manager.setProjectTreeValidatorForTesting(
        [validateDocuments](const QString& root, const QJsonObject&, bool) {
            return validateDocuments(root, VNX::kCurrentProjectSchemaVersion + 1);
        });

    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok)
        << analysis.blockingErrors.join(QStringLiteral("; ")).toStdString();
    ASSERT_TRUE(analysis.upgradeRequired);
    for (const QString& relative : backupFiles)
        EXPECT_FALSE(analysis.migrationPlan.writeSet.contains(relative));
    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_TRUE(result.changed);
    EXPECT_EQ(validatedVersions, QList<int>({VNX::kCurrentProjectSchemaVersion,
                                            VNX::kCurrentProjectSchemaVersion + 1}));
    for (const QString& path : documents) {
        QJsonObject expected = QJsonDocument::fromJson(originals.value(path)).object();
        QJsonObject header = HarkenTest::projectHeader(expected);
        header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion + 1);
        expected.insert(QStringLiteral("harkenDocument"), header);
        EXPECT_EQ(readJson(directory.filePath(path)), expected);
    }
    for (const QString& relative : backupFiles)
        EXPECT_EQ(readBytes(directory.filePath(relative)), backupBytes.value(relative));
    ASSERT_TRUE(manager.rollback(result));
    for (const QString& path : documents)
        EXPECT_EQ(readBytes(directory.filePath(path)), originals.value(path));
    for (const QString& relative : backupFiles)
        EXPECT_EQ(readBytes(directory.filePath(relative)), backupBytes.value(relative));
}

class ProjectPackOutputUpgradeTest : public testing::TestWithParam<QString>
{
};

TEST_P(ProjectPackOutputUpgradeTest, PackRebuildDoesNotAffectUpgradeProtection)
{
    QTemporaryDir directory;
    QTemporaryDir externalOutput;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(externalOutput.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString outputDir = GetParam() == QStringLiteral("external")
        ? QDir(directory.path()).relativeFilePath(externalOutput.path()) : GetParam();
    QJsonObject originalRoot = readJson(projectPath);
    QJsonObject content = HarkenTest::projectContent(originalRoot);
    content.insert(QStringLiteral("packBuildOutputDir"), outputDir);
    originalRoot.insert(QStringLiteral("content"), content);
    writeJson(projectPath, originalRoot);
    const QString outputRoot = VNX::PathUtils::packBuildOutputRoot(
        projectPath, VNX::PathUtils::normalizePackBuildOutputDir(outputDir));
    const QString generatedPath = QDir(outputRoot).filePath(QStringLiteral("Android_Low/Main.vp"));
    const QString indexPath = QDir(outputRoot).filePath(QStringLiteral("HarkenPackIndex.json"));
    const QString generatedRelative = QDir(directory.path()).relativeFilePath(generatedPath);

    VNX::MigrationManager migrations;
    migrations.setTreeValidatorForTesting(
        [](const QString&, int, QString*) { return true; });
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("test-custom-pack-output");
    step.fromSchema = VNX::kCurrentProjectSchemaVersion;
    step.toSchema = VNX::kCurrentProjectSchemaVersion + 1;
    step.writeSet = {QStringLiteral("<all-documents>")};
    step.transform = [](VNX::ProjectMigrationWorkspace& workspace,
                        QStringList*, QString* error) {
        QJsonObject root;
        if (!workspace.readObject(workspace.projectFile(), &root, error))
            return false;
        QJsonObject header = root.value(QStringLiteral("harkenDocument")).toObject();
        header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion + 1);
        root.insert(QStringLiteral("harkenDocument"), header);
        return workspace.writeObject(workspace.projectFile(), root, error);
    };
    ASSERT_TRUE(migrations.registerMigration(std::move(step)));
    VNX::ProjectUpgradeManager manager(&migrations);
    manager.setTargetSchemaForTesting(VNX::kCurrentProjectSchemaVersion + 1);
    manager.setProjectTreeValidatorForTesting(
        [](const QString&, const QJsonObject&, bool) { return QStringList{}; });

    const VNX::ProjectUpgradeAnalysis before = manager.analyze(projectPath);
    ASSERT_TRUE(before.ok) << before.blockingErrors.join(';').toStdString();
    ASSERT_TRUE(before.upgradeRequired);
    writeJson(generatedPath, QJsonObject{{QStringLiteral("generated"), QString(1024 * 1024, 'a')}});
    writeJson(indexPath, QJsonObject{{QStringLiteral("generation"), 1}});

    const VNX::ProjectUpgradeAnalysis after = manager.analyze(projectPath);
    ASSERT_TRUE(after.ok) << after.blockingErrors.join(';').toStdString();
    EXPECT_EQ(after.sourceTreeFingerprint, before.sourceTreeFingerprint);
    EXPECT_EQ(after.stagingBytes, before.stagingBytes);
    EXPECT_EQ(after.stagingFileCount, before.stagingFileCount);
    EXPECT_EQ(after.transactionBytes, before.transactionBytes);
    EXPECT_EQ(after.requiredProjectBytes, before.requiredProjectBytes);
    EXPECT_EQ(after.requiredRecoveryBytes, before.requiredRecoveryBytes);
    EXPECT_EQ(after.migrationPlan.writeSet, before.migrationPlan.writeSet);
    EXPECT_FALSE(after.migrationPlan.writeSet.contains(generatedRelative));

    bool validatedStaging = false;
    const VNX::ProjectUpgradeResult result = manager.execute(before,
        [&](const QString& stagedProject, QString*) {
            const QDir stagedDirectory(QFileInfo(stagedProject).absolutePath());
            EXPECT_TRUE(QFileInfo::exists(stagedDirectory.filePath(QStringLiteral("Layers/Main.vp"))));
            if (GetParam() != QStringLiteral("external"))
                EXPECT_FALSE(QFileInfo::exists(stagedDirectory.filePath(generatedRelative)));
            writeJson(generatedPath, QJsonObject{{QStringLiteral("generation"), 2}});
            writeJson(indexPath, QJsonObject{{QStringLiteral("generation"), 2}});
            validatedStaging = true;
            return true;
        });
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_TRUE(result.changed);
    EXPECT_TRUE(validatedStaging);
    if (GetParam() != QStringLiteral("external"))
        EXPECT_FALSE(QFileInfo::exists(QDir(result.backupPath).filePath(generatedRelative)));
    ASSERT_TRUE(manager.rollback(result));
    EXPECT_EQ(readJson(projectPath), originalRoot);
    EXPECT_EQ(readJson(generatedPath).value(QStringLiteral("generation")).toInt(), 2);
    EXPECT_EQ(readJson(indexPath).value(QStringLiteral("generation")).toInt(), 2);
}

INSTANTIATE_TEST_SUITE_P(ConfiguredOutput, ProjectPackOutputUpgradeTest,
    testing::Values(QStringLiteral("Exports/Packs"),
                    QStringLiteral(" ./Exports\\Unused/../Packs/ "),
                    QString{}, QStringLiteral("external")));

TEST(ProjectUpgradeManager, PackProjectIdentityMismatchIsRejectedBeforeWriting)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString packPath = directory.filePath(QStringLiteral("Layers/Main.vp"));
    QJsonObject pack = readJson(packPath);
    QJsonObject header = HarkenTest::projectHeader(pack);
    header.insert(QStringLiteral("projectId"),
                  QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    pack.insert(QStringLiteral("harkenDocument"), header);
    writeJson(packPath, pack);
    const QByteArray before = readBytes(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_TRUE(analysis.blockingErrors.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Project identity does not match")));
    EXPECT_EQ(readBytes(projectPath), before);
}

TEST(ProjectUpgradeManager, FutureSchemaIsRejectedWithoutWriting)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    QJsonObject root = readJson(projectPath);
    QJsonObject header = HarkenTest::projectHeader(root);
    header.insert(QStringLiteral("schemaVersion"), VNX::kCurrentProjectSchemaVersion + 1);
    root.insert(QStringLiteral("harkenDocument"), header);
    writeJson(projectPath, root);
    const QByteArray before = readBytes(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_EQ(analysis.issue, VNX::ProjectIssue::ProjectRequiresNewerStudio);
    EXPECT_EQ(readBytes(projectPath), before);
}

TEST(ProjectUpgradeManager, RootProjectIdentityMismatchIsBlockedWithoutChangingFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    QJsonObject root = readJson(projectPath);
    QJsonObject header = HarkenTest::projectHeader(root);
    header.insert(QStringLiteral("projectId"),
                  QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    root.insert(QStringLiteral("harkenDocument"), header);
    writeJson(projectPath, root);
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_FALSE(analysis.blockingErrors.isEmpty());
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, MissingFormalBaselineIsBlockedSafely)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    QJsonObject root = readJson(projectPath);
    root.remove(QStringLiteral("harkenDocument"));
    writeJson(projectPath, root);
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_FALSE(analysis.ok);
    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.issue, VNX::ProjectIssue::ProjectHasNoFormalBaseline);
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, InvalidPackIsBlockedAfterGovernance)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeResult result = manager.execute(manager.analyze(projectPath));
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("Layer")},
                  {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
              });
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_TRUE(analysis.blockingErrors.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Project document envelope is missing")));
}

TEST(ProjectUpgradeManager, InvalidBaselineIsRejectedBeforeCommit)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("Layer")},
              });
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeResult result =
        manager.execute(manager.analyze(projectPath));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, ChangedProjectIsNotOverwrittenByStaleAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok);
    QJsonObject root = readJson(projectPath);
    QJsonObject content = HarkenTest::projectContent(root);
    QJsonObject extensions = content.value(QStringLiteral("extensions")).toObject();
    QJsonObject test = extensions.value(QStringLiteral("test")).toObject();
    test.insert(QStringLiteral("payload"), QStringLiteral("newer-content"));
    extensions.insert(QStringLiteral("test"), test);
    content.insert(QStringLiteral("extensions"), extensions);
    root.insert(QStringLiteral("content"), content);
    writeJson(projectPath, root);

    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.issue, VNX::ProjectIssue::ProjectChangedDuringUpgrade);
    EXPECT_EQ(HarkenTest::projectContent(readJson(projectPath))
                  .value(QStringLiteral("extensions")).toObject()
                  .value(QStringLiteral("test")).toObject()
                  .value(QStringLiteral("payload")).toString(),
              QStringLiteral("newer-content"));
}

TEST(ProjectTransaction, TargetChangeIsRejectedBeforeWrite)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);

    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const QString stagedProject = staging.filePath(QStringLiteral("Baseline.hsp"));
    writeJson(stagedProject, QJsonObject{
        {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Changed")}}},
    });

    QString error;
    std::unique_ptr<VNX::ProjectTransaction> transaction =
        VNX::ProjectTransaction::begin(
            projectPath, {QStringLiteral("Baseline.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();

    const QJsonObject externalChange{
        {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("External")}}},
    };
    writeJson(projectPath, externalChange);

    EXPECT_FALSE(transaction->replaceFromDirectory(staging.path(), &error));
    EXPECT_EQ(readJson(projectPath), externalChange);
}

TEST(ProjectTransaction, TargetChangeIsRejectedBeforeCommit)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);

    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const QString stagedProject = staging.filePath(QStringLiteral("Baseline.hsp"));
    writeJson(stagedProject, QJsonObject{
        {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Changed")}}},
    });

    QString error;
    std::unique_ptr<VNX::ProjectTransaction> transaction =
        VNX::ProjectTransaction::begin(
            projectPath, {QStringLiteral("Baseline.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
        << error.toStdString();

    const QJsonObject externalChange{
        {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("External")}}},
    };
    writeJson(projectPath, externalChange);

    EXPECT_FALSE(transaction->commit(false, &error));
    EXPECT_EQ(readJson(projectPath), externalChange);
}

TEST(ProjectTransaction, InterruptedCommitIsRecoveredFromVerifiedBackup)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);

    QString error;
    {
        QTemporaryDir staging;
        ASSERT_TRUE(staging.isValid());
        const QString stagedProject = staging.filePath(QStringLiteral("Baseline.hsp"));
        writeJson(stagedProject, QJsonObject{
            {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Changed")}}},
        });
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
            << error.toStdString();
    }

    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error))
        << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_FALSE(QFileInfo::exists(
        QFileInfo(projectPath).absolutePath()
        + QStringLiteral(".harken-project-transaction.json")));
}

TEST(ProjectTransaction, RecoveryHandlesJournaledNewHashBeforeWrite)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);

    QString error;
    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const QString stagedProject = staging.filePath(QStringLiteral("Baseline.hsp"));
    writeJson(stagedProject, QJsonObject{
        {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Changed")}}},
    });
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
    }

    const QString transactionPath = QFileInfo(projectPath).absolutePath()
        + QStringLiteral(".harken-project-transaction.json");
    QJsonObject journal = readJson(transactionPath);
    QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    ASSERT_EQ(entries.size(), 1);
    QJsonObject entry = entries.at(0).toObject();
    entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
    entry.insert(QStringLiteral("newHash"), QString::fromLatin1(
        QCryptographicHash::hash(readBytes(stagedProject), QCryptographicHash::Sha256).toHex()));
    entry.insert(QStringLiteral("state"), QStringLiteral("planned"));
    entries.replace(0, entry);
    journal.insert(QStringLiteral("state"), QStringLiteral("writing"));
    journal.insert(QStringLiteral("entries"), entries);
    writeJson(transactionPath, journal);

    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error))
        << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_FALSE(QFileInfo::exists(transactionPath));
}

TEST(ProjectTransaction, RecoveryHandlesJournaledNewHashAfterWrite)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);

    QString error;
    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const QString stagedProject = staging.filePath(QStringLiteral("Baseline.hsp"));
    writeJson(stagedProject, QJsonObject{
        {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Changed")}}},
    });
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
    }

    const QString transactionPath = QFileInfo(projectPath).absolutePath()
        + QStringLiteral(".harken-project-transaction.json");
    QJsonObject journal = readJson(transactionPath);
    QJsonArray entries = journal.value(QStringLiteral("entries")).toArray();
    ASSERT_EQ(entries.size(), 1);
    QJsonObject entry = entries.at(0).toObject();
    entry.insert(QStringLiteral("operation"), QStringLiteral("write"));
    entry.insert(QStringLiteral("newHash"), QString::fromLatin1(
        QCryptographicHash::hash(readBytes(stagedProject), QCryptographicHash::Sha256).toHex()));
    entry.insert(QStringLiteral("state"), QStringLiteral("planned"));
    entries.replace(0, entry);
    journal.insert(QStringLiteral("state"), QStringLiteral("writing"));
    journal.insert(QStringLiteral("entries"), entries);
    writeJson(transactionPath, journal);
    ASSERT_TRUE(QFile::remove(projectPath));
    ASSERT_TRUE(QFile::copy(stagedProject, projectPath));

    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error))
        << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_FALSE(QFileInfo::exists(transactionPath));
}

TEST(ProjectUpgradeManager, PackChangeInvalidatesStaleAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok);
    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")),
              QJsonObject{
                  {QStringLiteral("version"), 1},
                  {QStringLiteral("type"), QStringLiteral("Layer")},
                  {QStringLiteral("guid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                  {QStringLiteral("name"), QStringLiteral("Changed")},
              });

    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.issue, VNX::ProjectIssue::ProjectChangedDuringUpgrade);
}

TEST(ProjectUpgradeManager, DirectoryPathResolvesNamedProjectFile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(directory.path());
    ASSERT_TRUE(analysis.ok);
    EXPECT_EQ(analysis.projectFilePath, QFileInfo(projectPath).absoluteFilePath());
}

TEST(ProjectUpgradeManager, UnknownRootFieldIsBlockedBeforeTransaction)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    QJsonObject root = readJson(projectPath);
    QJsonObject content = HarkenTest::projectContent(root);
    content.insert(QStringLiteral("unknownRootField"), QStringLiteral("must-block"));
    root.insert(QStringLiteral("content"), content);
    writeJson(projectPath, root);
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_FALSE(analysis.blockingErrors.isEmpty());
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, UnknownPackFieldIsBlockedBeforeTransaction)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeResult upgraded =
        manager.execute(manager.analyze(projectPath));
    ASSERT_TRUE(upgraded.ok) << upgraded.error.toStdString();

    QJsonObject pack = readJson(directory.filePath(QStringLiteral("Layers/Main.vp")));
    QJsonObject content = HarkenTest::projectContent(pack);
    content.insert(QStringLiteral("futurePackField"), QStringLiteral("must-block"));
    pack.insert(QStringLiteral("content"), content);
    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")), pack);
    const QJsonObject before = readJson(projectPath);

    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_TRUE(analysis.blockingErrors.join(QStringLiteral("; "))
                    .contains(QStringLiteral("Unknown field")));
    EXPECT_EQ(analysis.issue, VNX::ProjectIssue::ProjectFileDamaged);
    EXPECT_EQ(analysis.detail, analysis.blockingErrors.join(QLatin1Char('\n')));
    const auto report = VNX::projectOpenAnalysisReport(analysis, QStringLiteral("invalid-pack"));
    EXPECT_EQ(report.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PROJECT_FILE_DAMAGED"));
    EXPECT_EQ(report.value(QStringLiteral("technicalDetails")).toString(), analysis.detail);
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, LayerDocumentIdentityMismatchIsBlocked)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeResult upgraded =
        manager.execute(manager.analyze(projectPath));
    ASSERT_TRUE(upgraded.ok) << upgraded.error.toStdString();

    QJsonObject pack = readJson(directory.filePath(QStringLiteral("Layers/Main.vp")));
    QJsonObject header = HarkenTest::projectHeader(pack);
    header.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    pack.insert(QStringLiteral("harkenDocument"), header);
    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")), pack);

    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_TRUE(analysis.blockingErrors.join(QStringLiteral("; "))
                    .contains(QStringLiteral("Document identity does not match")));
}

TEST(ProjectUpgradeManager, LayerParentReferenceCycleIsBlocked)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeResult upgraded =
        manager.execute(manager.analyze(projectPath));
    ASSERT_TRUE(upgraded.ok) << upgraded.error.toStdString();

    QJsonObject pack = readJson(directory.filePath(QStringLiteral("Layers/Main.vp")));
    QJsonObject content = HarkenTest::projectContent(pack);
    const QString guid = content.value(QStringLiteral("guid")).toString();
    const QJsonObject selfRef{
        {QStringLiteral("kind"), QStringLiteral("item")},
        {QStringLiteral("id"), guid},
    };
    content.insert(QStringLiteral("parentRef"), selfRef);
    pack.insert(QStringLiteral("content"), content);
    writeJson(directory.filePath(QStringLiteral("Layers/Main.vp")), pack);

    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_TRUE(analysis.blockingErrors.join(QStringLiteral("; "))
                    .contains(QStringLiteral("cycle")));
}

TEST(ProjectUpgradeManager, EmptyDirectoryChangeInvalidatesStaleAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok);
    ASSERT_TRUE(QDir(directory.filePath(QStringLiteral("EmptyFolder"))).mkpath(QStringLiteral(".")));

    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.issue, VNX::ProjectIssue::ProjectChangedDuringUpgrade);
}

TEST(ProjectUpgradeManager, ProjectIndexCacheChangeDoesNotInvalidateStaleAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok);

    const QString cacheDirectory = directory.filePath(
        QStringLiteral(".cache/project_index"));
    ASSERT_TRUE(QDir().mkpath(cacheDirectory));
    writeJson(QDir(cacheDirectory).filePath(QStringLiteral("index.json")),
              QJsonObject{{QStringLiteral("version"), 2},
                          {QStringLiteral("generation"), 1}});

    const VNX::ProjectUpgradeResult result = manager.execute(analysis);
    EXPECT_TRUE(result.ok) << result.error.toStdString();
}

TEST(ProjectUpgradeManager, FolderMetadataIsRejectedWithoutChangingFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeResult upgraded =
        manager.execute(manager.analyze(projectPath));
    ASSERT_TRUE(upgraded.ok) << upgraded.error.toStdString();

    QJsonObject root = readJson(projectPath);
    QJsonObject content = HarkenTest::projectContent(root);
    content.insert(QStringLiteral("folderMetadata"), QJsonArray{
        QJsonObject{
            {QStringLiteral("bucket"), QStringLiteral("Actions")},
            {QStringLiteral("path"), QStringLiteral("Gameplay")},
            {QStringLiteral("color"), 0},
            {QStringLiteral("expanded"), false},
        },
    });
    root.insert(QStringLiteral("content"), content);
    writeJson(projectPath, root);
    const QJsonObject before = readJson(projectPath);

    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    EXPECT_FALSE(analysis.ok);
    EXPECT_TRUE(analysis.blockingErrors.join(QStringLiteral("; "))
                    .contains(QStringLiteral("folderMetadata")));
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectUpgradeManager, CurrentFixtureSavesAndReopensWithoutUpgrade)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString layerPath = directory.filePath(QStringLiteral("Layers/Main.vp"));
    const QString busPath = directory.filePath(QStringLiteral("Busses/Root.vp"));

    VNX::ProjectUpgradeManager manager;
    const VNX::ProjectUpgradeAnalysis analysis = manager.analyze(projectPath);
    ASSERT_TRUE(analysis.ok)
        << analysis.blockingErrors.join(QStringLiteral("; ")).toStdString();
    ASSERT_FALSE(analysis.upgradeRequired);
    EXPECT_EQ(analysis.sourceSchema, VNX::kCurrentProjectSchemaVersion);
    EXPECT_EQ(analysis.targetSchema, VNX::kCurrentProjectSchemaVersion);

    VNX::ProjectUpgradeResult result = manager.execute(analysis);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_FALSE(result.changed);
    QString finalizeError;
    ASSERT_TRUE(manager.finalize(result, &finalizeError))
        << finalizeError.toStdString();
    for (const QString& path : {projectPath, layerPath, busPath}) {
        EXPECT_EQ(HarkenTest::projectHeader(readJson(path))
                      .value(QStringLiteral("schemaVersion")).toInt(),
                  VNX::kCurrentProjectSchemaVersion);
    }

    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    ASSERT_TRUE(serializer.loadProject(projectPath, project, false))
        << serializer.lastError().toStdString();
    EXPECT_FALSE(serializer.upgradeResult().changed);
    const auto prepared = VNX::ProjectManager::prepareProject(projectPath, false);
    ASSERT_NE(prepared.project, nullptr);
    ASSERT_TRUE(serializer.saveProject(projectPath, *prepared.project))
        << serializer.lastError().toStdString();

    VNX::ProjectSerializer reopenedSerializer;
    VNX::ProjectData reopenedProject;
    ASSERT_TRUE(reopenedSerializer.loadProject(
        projectPath, reopenedProject, false))
        << reopenedSerializer.lastError().toStdString();
    EXPECT_FALSE(reopenedSerializer.upgradeResult().changed);
}

TEST(ProjectSerializer, FormalBaselineOpensWithoutApproval)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    std::vector<VNX::ProjectUpgradeProgress> progress;
    ASSERT_TRUE(serializer.loadProject(
        projectPath, project, false,
        [&progress](const VNX::ProjectUpgradeProgress& value) {
            progress.push_back(value);
        }))
        << serializer.lastError().toStdString();
    EXPECT_FALSE(serializer.upgradeResult().changed);
    EXPECT_EQ(readJson(projectPath), before);
    ASSERT_GE(progress.size(), 2u);
    EXPECT_EQ(progress.front().phase, VNX::ProjectUpgradePhase::Analyzing);
    EXPECT_EQ(progress.front().completedItems, 0);
    EXPECT_EQ(progress.back().phase, VNX::ProjectUpgradePhase::Analyzing);
    EXPECT_EQ(progress.back().completedItems, 1);
}

TEST(ProjectSerializer, CurrentProjectDoesNotRequestUpgradeApproval)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QByteArray before = readBytes(projectPath);
    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    bool approvalAsked = false;
    ASSERT_TRUE(serializer.loadProject(projectPath, project, true, {},
        [&](const VNX::ProjectUpgradeAnalysis&) { approvalAsked = true; return false; }))
        << serializer.lastError().toStdString();
    EXPECT_FALSE(approvalAsked);
    EXPECT_EQ(readBytes(projectPath), before);
}

TEST(ProjectSerializer, BoundApprovalDoesNotRecoverUnfinishedTransaction)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString layerPath = directory.filePath(QStringLiteral("Layers/Main.vp"));
    QString error;
    {
        const std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath,
                {QStringLiteral("Baseline.hsp"),
                 QStringLiteral("Layers/Main.vp")},
                &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
    }
    const QString journalPath = directory.path()
        + QStringLiteral(".harken-project-transaction.json");
    ASSERT_TRUE(QFileInfo::exists(journalPath));
    const QByteArray projectBefore = readBytes(projectPath);
    const QByteArray layerBefore = readBytes(layerPath);

    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    EXPECT_FALSE(serializer.loadProject(
        projectPath, project, true, {},
        [](const VNX::ProjectUpgradeAnalysis&) { return true; }));
    EXPECT_TRUE(serializer.lastError().startsWith(
        QStringLiteral("PROJECT_UPGRADE_RECOVERY_REQUIRED")));
    EXPECT_TRUE(QFileInfo::exists(journalPath));
    EXPECT_EQ(readBytes(projectPath), projectBefore);
    EXPECT_EQ(readBytes(layerPath), layerBefore);

    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error))
        << error.toStdString();
}

TEST(ProjectSerializer, InterruptedUpdateReportsRecoveryProgress)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);
    QJsonObject changed = before;
    QJsonObject content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("extensions"), QJsonObject{{QStringLiteral("interrupted"), true}});
    changed.insert(QStringLiteral("content"), content);
    writeJson(staging.filePath(QStringLiteral("Baseline.hsp")), changed);
    QString error;
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
            << error.toStdString();
    }

    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    std::vector<VNX::ProjectUpgradeProgress> progress;
    ASSERT_TRUE(serializer.loadProject(
        projectPath, project, false,
        [&progress](const VNX::ProjectUpgradeProgress& value) {
            progress.push_back(value);
        })) << serializer.lastError().toStdString();
    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_NE(std::find_if(
                  progress.begin(), progress.end(),
                  [](const VNX::ProjectUpgradeProgress& value) {
                      return value.phase == VNX::ProjectUpgradePhase::RollingBack
                          && value.completedItems == value.totalItems;
                  }),
              progress.end());
}

TEST(ProjectUpgradeManager, PreparedProjectRejectsInvalidActiveGroupWithoutChangingFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString layerPath = directory.filePath(QStringLiteral("Layers/Main.vp"));
    QJsonObject layer = readJson(layerPath);
    QJsonObject content = HarkenTest::projectContent(layer);
    QJsonObject groups = content.value(QStringLiteral("fieldGroups")).toObject();
    QJsonObject shared = groups.value(QStringLiteral("shared")).toObject();
    shared[QStringLiteral("mainOutput")] = QJsonObject{
        {QStringLiteral("inherit"), false},
        {QStringLiteral("values"), QJsonObject{
            {QStringLiteral("mainOutputBusRef"), QStringLiteral("00000000-0000-0000-0000-000000000777")},
            {QStringLiteral("outputBusVolume"), 0.0},
        }},
    };
    groups[QStringLiteral("shared")] = shared;
    content[QStringLiteral("fieldGroups")] = groups;
    layer[QStringLiteral("content")] = content;
    writeJson(layerPath, layer);
    const QJsonObject before = readJson(projectPath);
    const QByteArray layerBefore = readBytes(layerPath);

    {
        const VNX::PreparedProject prepared =
            VNX::ProjectManager::prepareProject(projectPath, true);
        EXPECT_FALSE(prepared.project);
    }

    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_EQ(readBytes(layerPath), layerBefore);
    EXPECT_FALSE(QFileInfo::exists(
        directory.filePath(QStringLiteral(".harken-project-transaction.json"))));
}

TEST(ProjectUpgradeManager, FinalizedUpgradeHasExplicitRollbackEntry)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);

    VNX::ProjectUpgradeManager manager;
    VNX::ProjectUpgradeResult result;
    QString error;
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->commit(true, &error)) << error.toStdString();
        result.ok = true;
        result.changed = true;
        result.projectFilePath = projectPath;
        result.transactionPath = transaction->transactionPath();
        result.backupPath = transaction->backupDirectory();
        result.backupFingerprint = transaction->backupFingerprint();
        result.committedFingerprint = transaction->committedFingerprint();
    }
    ASSERT_TRUE(manager.finalize(result)) << "finalization must create a receipt";
    EXPECT_TRUE(QFileInfo::exists(
        directory.path() + QStringLiteral(".harken-upgrade-receipt.json")));

    ASSERT_TRUE(manager.rollbackLastUpgrade(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_FALSE(QFileInfo::exists(
        directory.path() + QStringLiteral(".harken-upgrade-receipt.json")));
    EXPECT_FALSE(QFileInfo::exists(result.backupPath));
}

TEST(ProjectTransaction, SaveTransactionPreservesUpgradeReceipt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectUpgradeManager manager;
    VNX::ProjectUpgradeResult result;
    QString error;
    {
        std::unique_ptr<VNX::ProjectTransaction> upgrade =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(upgrade, nullptr) << error.toStdString();
        ASSERT_TRUE(upgrade->commit(true, &error)) << error.toStdString();
        result.ok = true;
        result.changed = true;
        result.projectFilePath = projectPath;
        result.transactionPath = upgrade->transactionPath();
    }
    ASSERT_TRUE(manager.finalize(result));

    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(projectPath, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    }
    EXPECT_TRUE(QFileInfo::exists(
        directory.path() + QStringLiteral(".harken-upgrade-receipt.json")));
}

TEST(ProjectUpgradeManager, StorageEstimateIncludesStagingBackupAndReplacement)
{
    constexpr qint64 stagingBytes = 12 * 1024 * 1024;
    constexpr qint64 sourceBytes = 3 * 1024 * 1024;
    constexpr qint64 targetBytes = 4 * 1024 * 1024;
    const VNX::ProjectUpgradeStorageRequirements required =
        VNX::ProjectUpgradeManager::storageRequirements(
            stagingBytes, sourceBytes, targetBytes, 40,
            VNX::ProjectUpgradeProtection::AtomicInPlace);
    EXPECT_GE(required.projectBytes,
              stagingBytes + sourceBytes + targetBytes);
    EXPECT_GT(required.safetyBytes, 0);
    EXPECT_EQ(required.recoveryBytes, 0);
}

TEST(ProjectUpgradeManager, RemovableDriveEstimateSplitsProjectAndRecoverySpace)
{
    constexpr qint64 stagingBytes = 12 * 1024 * 1024;
    constexpr qint64 sourceBytes = 3 * 1024 * 1024;
    constexpr qint64 targetBytes = 4 * 1024 * 1024;
    const VNX::ProjectUpgradeStorageRequirements required =
        VNX::ProjectUpgradeManager::storageRequirements(
            stagingBytes, sourceBytes, targetBytes, 40,
            VNX::ProjectUpgradeProtection::RecoverableInPlace);
    EXPECT_GE(required.projectBytes, stagingBytes + targetBytes);
    EXPECT_GE(required.recoveryBytes, sourceBytes + targetBytes);
    EXPECT_LT(required.projectBytes,
              stagingBytes + sourceBytes + targetBytes
                  + required.safetyBytes);
}

TEST(ProjectTransaction, ManagedExternalRecoveryRestoresInterruptedReplacement)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);
    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    writeJson(staging.filePath(QStringLiteral("Baseline.hsp")),
              QJsonObject{{QStringLiteral("changed"), true}});

    const QString recoveryRoot = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("ProjectRecovery"));
    ASSERT_TRUE(QDir().mkpath(recoveryRoot));
    QString error;
    QString backupDirectory;
    std::vector<std::array<qint64, 4>> backupProgress;
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")},
                recoveryRoot, &error,
                [&backupProgress](int files, int totalFiles,
                                  qint64 bytes, qint64 totalBytes) {
                    backupProgress.push_back(
                        {files, totalFiles, bytes, totalBytes});
                });
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        backupDirectory = transaction->backupDirectory();
        EXPECT_TRUE(QDir::fromNativeSeparators(QDir::cleanPath(backupDirectory))
                        .startsWith(
            QDir::fromNativeSeparators(QDir::cleanPath(recoveryRoot))
                + QLatin1Char('/'),
            Qt::CaseInsensitive))
            << backupDirectory.toStdString() << " root="
            << recoveryRoot.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
            << error.toStdString();
    }
    ASSERT_GE(backupProgress.size(), 2u);
    EXPECT_EQ(backupProgress.front()[0], 0);
    EXPECT_EQ(backupProgress.back()[0], backupProgress.back()[1]);
    EXPECT_NE(readJson(projectPath), before);
    EXPECT_TRUE(QDir(backupDirectory).exists());

    std::vector<std::array<qint64, 4>> recoveryProgress;
    ASSERT_TRUE(VNX::ProjectTransaction::recover(
        projectPath, &error,
        [&recoveryProgress](int files, int totalFiles,
                            qint64 bytes, qint64 totalBytes) {
            recoveryProgress.push_back({files, totalFiles, bytes, totalBytes});
        })) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
    EXPECT_FALSE(QDir(backupDirectory).exists());
    ASSERT_GE(recoveryProgress.size(), 2u);
    EXPECT_EQ(recoveryProgress.front()[0], 0);
    EXPECT_EQ(recoveryProgress.back()[0], recoveryProgress.back()[1]);
}

TEST(ProjectTransaction, UnmanagedRecoveryDirectoryIsRejected)
{
    QTemporaryDir directory;
    QTemporaryDir recovery;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(recovery.isValid());
    const QString projectPath = createBaselineProject(directory);
    QString error;
    const std::unique_ptr<VNX::ProjectTransaction> transaction =
        VNX::ProjectTransaction::begin(
            projectPath, {QStringLiteral("Baseline.hsp")},
            recovery.path(), &error);
    EXPECT_EQ(transaction, nullptr);
    EXPECT_EQ(error, QStringLiteral("Project recovery location is invalid"));
}

TEST(ProjectTransaction, TamperedRecoveryRecordCannotOverwriteProject)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject before = readJson(projectPath);
    const QJsonObject changed{{QStringLiteral("changed"), true}};
    writeJson(staging.filePath(QStringLiteral("Baseline.hsp")), changed);

    QString error;
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
            << error.toStdString();
    }

    const QString journalPath =
        directory.path() + QStringLiteral(".harken-project-transaction.json");
    const QJsonObject validJournal = readJson(journalPath);
    QJsonObject tamperedJournal = validJournal;
    tamperedJournal[QStringLiteral("backupFingerprint")] =
        QString(64, QLatin1Char('0'));
    writeJson(journalPath, tamperedJournal);

    EXPECT_FALSE(VNX::ProjectTransaction::recover(projectPath, &error));
    EXPECT_EQ(readJson(projectPath), changed);
    EXPECT_TRUE(error.contains(QStringLiteral("does not match")));

    writeJson(journalPath, validJournal);
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error))
        << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
}

TEST(ProjectTransaction, ManagedDirectoriesRecoverEmptyFoldersAndFileContents)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString originalPath = directory.filePath(QStringLiteral("Layers/Old/data.bin"));
    const QJsonObject original{{QStringLiteral("value"), 7}};
    writeJson(originalPath, original);
    ASSERT_TRUE(QDir().mkpath(directory.filePath(QStringLiteral("Layers/Old/Empty/Nested"))));
    writeJson(staging.filePath(QStringLiteral("Layers/New/data.bin")),
              QJsonObject{{QStringLiteral("value"), 11}});
    ASSERT_TRUE(QDir().mkpath(staging.filePath(QStringLiteral("Layers/New/Empty"))));
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {QStringLiteral("Layers/Old/data.bin"), QStringLiteral("Layers/New/data.bin")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_FALSE(QDir(directory.filePath(QStringLiteral("Layers/Old"))).exists());
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/New/Empty"))).exists());
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(originalPath), original);
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/Old/Empty/Nested"))).exists());
    EXPECT_FALSE(QDir(directory.filePath(QStringLiteral("Layers/New"))).exists());
}

TEST(ProjectTransaction, FileAndDirectoryReplacementsRecoverTheirOriginalKinds)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject originalFile{{QStringLiteral("originalFile"), true}};
    const QJsonObject originalChild{{QStringLiteral("originalChild"), true}};
    writeJson(directory.filePath(QStringLiteral("Layers/File")), originalFile);
    writeJson(directory.filePath(QStringLiteral("Layers/Directory/child.bin")), originalChild);
    writeJson(staging.filePath(QStringLiteral("Layers/File/new.bin")), QJsonObject{{QStringLiteral("new"), true}});
    writeJson(staging.filePath(QStringLiteral("Layers/Directory")), QJsonObject{{QStringLiteral("newFile"), true}});
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {QStringLiteral("Layers/File"), QStringLiteral("Layers/File/new.bin"),
             QStringLiteral("Layers/Directory"), QStringLiteral("Layers/Directory/child.bin")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_TRUE(QFileInfo(directory.filePath(QStringLiteral("Layers/Directory"))).isFile());
        EXPECT_TRUE(QFileInfo(directory.filePath(QStringLiteral("Layers/File"))).isDir());
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Layers/File"))), originalFile);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Layers/Directory/child.bin"))), originalChild);
}

TEST(ProjectTransaction, InterruptedInstallationKeepsTheProjectFileAvailableForRecovery)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject projectBefore = readJson(projectPath);
    const QJsonObject deletedBytes{{QStringLiteral("deleted"), true}};
    const QString deleted = directory.filePath(QStringLiteral("Actions/Deleted.bin"));
    writeJson(deleted, deletedBytes);
    writeJson(staging.filePath(QStringLiteral(".before-project.bin")), QJsonObject{});
    writeJson(staging.filePath(QFileInfo(projectPath).fileName()), QJsonObject{{QStringLiteral("changed"), true}});
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {QFileInfo(projectPath).fileName(), QStringLiteral("Actions/Deleted.bin"),
             QStringLiteral(".before-project.bin")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        EXPECT_THROW(transaction->replaceFromDirectory(staging.path(), &error,
            [&](int completed, int, qint64, qint64) {
                EXPECT_TRUE(QFileInfo::exists(projectPath));
                if (completed == 1) {
                    EXPECT_EQ(readJson(projectPath), projectBefore);
                    throw std::runtime_error("Interrupted installation");
                }
            }), std::runtime_error);
    }
    EXPECT_TRUE(QFileInfo::exists(projectPath));
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), projectBefore);
    EXPECT_EQ(readJson(deleted), deletedBytes);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral(".before-project.bin"))));
}

TEST(ProjectTransaction, UnexpectedDirectoryContentIsPreservedUntilRecoveryCanFinish)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    writeJson(staging.filePath(QStringLiteral("Layers/New/owned.bin")), QJsonObject{});
    QString error;
    auto transaction = VNX::ProjectTransaction::begin(projectPath,
        {QStringLiteral("Layers/New/owned.bin")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
        QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
    const QString external = directory.filePath(QStringLiteral("Layers/New/external.bin"));
    const QJsonObject externalContent{{QStringLiteral("external"), true}};
    writeJson(external, externalContent);
    EXPECT_FALSE(transaction->rollback(&error));
    EXPECT_EQ(readJson(external), externalContent);
    EXPECT_TRUE(VNX::ProjectTransaction::recoveryRequired(projectPath));
    ASSERT_TRUE(QFile::remove(external));
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_FALSE(QDir(directory.filePath(QStringLiteral("Layers/New"))).exists());
}

#if defined(Q_OS_WIN)
TEST(ProjectTransaction, LongPathsKeepFileAttributesAcrossInstallationAndRecovery)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QString nested = QStringLiteral("Layers/") + QStringLiteral("Nested/").repeated(35);
    const QString originalRelative = nested + QStringLiteral("original.bin");
    const QString installedRelative = nested + QStringLiteral("installed.bin");
    const QString original = directory.filePath(originalRelative);
    const QString nativeOriginal = QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(original);
    const QJsonObject content{{QStringLiteral("original"), true}};
    writeJson(original, content);
    ASSERT_GT(original.size(), MAX_PATH);
    ASSERT_NE(SetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeOriginal.utf16()),
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM), 0);
    writeJson(staging.filePath(installedRelative), QJsonObject{{QStringLiteral("installed"), true}});
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {originalRelative, installedRelative}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_FALSE(QFileInfo::exists(original));
        EXPECT_TRUE(QFileInfo::exists(directory.filePath(installedRelative)));
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(original), content);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath(installedRelative)));
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeOriginal.utf16()));
    ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
}

TEST(ProjectTransaction, HiddenFilesAreIncludedAndRecoverAfterDirectoryDeletion)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QString secret = directory.filePath(QStringLiteral("Layers/Hidden/secret.bin"));
    const QJsonObject original{{QStringLiteral("hidden"), true}};
    writeJson(secret, original);
    ASSERT_NE(SetFileAttributesW(reinterpret_cast<LPCWSTR>(secret.utf16()),
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM), 0);
    const QString hiddenDirectory = QFileInfo(secret).absolutePath();
    ASSERT_NE(SetFileAttributesW(reinterpret_cast<LPCWSTR>(hiddenDirectory.utf16()),
        FILE_ATTRIBUTE_HIDDEN), 0);
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        EXPECT_TRUE(transaction->writeSet().contains(QStringLiteral("Layers/Hidden/secret.bin")));
        ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    }
    ASSERT_TRUE(QDir().mkpath(staging.filePath(QStringLiteral("Layers"))));
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {QStringLiteral("Layers/Hidden/secret.bin")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(secret), original);
    const DWORD restoredAttributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(secret.utf16()));
    EXPECT_NE(restoredAttributes, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(restoredAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
}

TEST(ProjectTransaction, UnchangedLockedFileIsKeptAndFailedDeletionRestoresOtherFiles)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QString locked = directory.filePath(QStringLiteral("Layers/zz_locked.bin"));
    const QJsonObject lockedBytes{{QStringLiteral("locked"), true}};
    const QJsonObject projectBefore = readJson(projectPath);
    writeJson(locked, lockedBytes);
    writeJson(staging.filePath(QStringLiteral("Layers/zz_locked.bin")), lockedBytes);
    writeJson(staging.filePath(QFileInfo(projectPath).fileName()), projectBefore);
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(locked.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([&] { CloseHandle(handle); });
    QString error;
    const QStringList writeSet{QFileInfo(projectPath).fileName(), QStringLiteral("Layers/zz_locked.bin")};
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath, writeSet, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    }
    ASSERT_TRUE(QFile::remove(staging.filePath(QStringLiteral("Layers/zz_locked.bin"))));
    writeJson(staging.filePath(QFileInfo(projectPath).fileName()), QJsonObject{{QStringLiteral("changed"), true}});
    auto transaction = VNX::ProjectTransaction::begin(projectPath, writeSet, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    EXPECT_FALSE(transaction->replaceFromDirectory(staging.path(),
        QStringList{QStringLiteral("Layers")}, &error));
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), projectBefore);
    EXPECT_EQ(readJson(locked), lockedBytes);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
}

#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
TEST(ProjectTransaction, CaseOnlyFileAndFolderChangesPreserveSpellingAcrossRecovery)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject contents{{QStringLiteral("unchanged"), true}};
    writeJson(directory.filePath(QStringLiteral("Layers/Upper/File.bin")), contents);
    if (!QFileInfo::exists(directory.filePath(QStringLiteral("Layers/upper/file.bin"))))
        GTEST_SKIP() << "Requires a case-insensitive filesystem";
    writeJson(staging.filePath(QStringLiteral("Layers/upper/file.bin")), contents);
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {QStringLiteral("Layers/upper/file.bin")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers"))).entryList().contains(QStringLiteral("upper")));
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/upper"))).entryList().contains(QStringLiteral("file.bin")));
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers"))).entryList().contains(QStringLiteral("Upper")));
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/Upper"))).entryList().contains(QStringLiteral("File.bin")));
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Layers/Upper/File.bin"))), contents);
    auto transaction = VNX::ProjectTransaction::begin(projectPath,
        {QStringLiteral("Layers/upper/file.bin")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
        QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
}
#endif

#if defined(Q_OS_WIN)
TEST(ProjectTransaction, ManagedRootsAndUnmanagedParentsKeepExistingSpelling)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    QDir projectRoot(directory.path());
    ASSERT_TRUE(projectRoot.rename(QStringLiteral("Layers"), QStringLiteral("LayersTemporary")));
    ASSERT_TRUE(projectRoot.rename(QStringLiteral("LayersTemporary"), QStringLiteral("layers")));
    const QJsonObject content{{QStringLiteral("original"), true}};
    const QString locked = directory.filePath(QStringLiteral("layers/Unchanged.bin"));
    writeJson(locked, content);
    writeJson(directory.filePath(QStringLiteral(".CACHE/asset_registry/assets.json")), content);
    writeJson(directory.filePath(QStringLiteral(".CACHE/unmanaged.bin")), content);
    writeJson(staging.filePath(QStringLiteral("Layers/Unchanged.bin")), content);
    writeJson(staging.filePath(QStringLiteral(".cache/asset_registry/assets.json")),
              QJsonObject{{QStringLiteral("changed"), true}});
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(locked.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([&] { CloseHandle(handle); });
    QString error;
    auto transaction = VNX::ProjectTransaction::begin(projectPath,
        {QStringLiteral("Layers/Unchanged.bin"), QStringLiteral(".cache/asset_registry/assets.json")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
        QStringList{QStringLiteral("Layers"), QStringLiteral(".cache/asset_registry")}, &error))
        << error.toStdString();
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_TRUE(projectRoot.entryList(QDir::Dirs | QDir::Hidden).contains(QStringLiteral("layers")));
    EXPECT_TRUE(projectRoot.entryList(QDir::Dirs | QDir::Hidden).contains(QStringLiteral(".CACHE")));
    EXPECT_EQ(readJson(locked), content);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral(".CACHE/asset_registry/assets.json"))), content);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral(".CACHE/unmanaged.bin"))), content);
    transaction.reset();
    transaction = VNX::ProjectTransaction::begin(projectPath,
        {QStringLiteral("Layers/Unchanged.bin"), QStringLiteral(".cache/asset_registry/assets.json")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
        QStringList{QStringLiteral("Layers"), QStringLiteral(".cache/asset_registry")}, &error))
        << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    EXPECT_TRUE(projectRoot.entryList(QDir::Dirs | QDir::Hidden).contains(QStringLiteral("layers")));
    EXPECT_TRUE(projectRoot.entryList(QDir::Dirs | QDir::Hidden).contains(QStringLiteral(".CACHE")));
}
#endif

}
