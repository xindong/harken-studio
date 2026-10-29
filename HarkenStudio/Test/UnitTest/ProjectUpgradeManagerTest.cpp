#include "Core/ProjectUpgradeManager.h"
#include "Core/ProjectTransaction.h"
#include "Core/FieldGroupJson.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

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

TEST(ProjectUpgradeManager, RepeatedAnalysisObservesExternalDocumentChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString layerPath = directory.filePath(QStringLiteral("Layers/Main.vp"));
    VNX::ProjectUpgradeManager manager;
    VNX::ProjectContractReadContext context;
    const auto compare = [&]() {
        const auto repeated = manager.analyze(projectPath, {}, &context);
        const auto fresh = manager.analyze(projectPath);
        EXPECT_EQ(repeated.ok, fresh.ok);
        EXPECT_EQ(repeated.issue, fresh.issue);
        EXPECT_EQ(repeated.detail, fresh.detail);
        EXPECT_EQ(repeated.blockingErrors, fresh.blockingErrors);
        EXPECT_EQ(repeated.warnings, fresh.warnings);
        EXPECT_EQ(repeated.sourceFingerprint, fresh.sourceFingerprint);
        EXPECT_EQ(repeated.sourceTreeFingerprint, fresh.sourceTreeFingerprint);
        return repeated.ok;
    };
    ASSERT_TRUE(compare());

    QFile layer(layerPath);
    ASSERT_TRUE(layer.open(QIODevice::ReadOnly));
    const QByteArray original = layer.readAll();
    layer.close();
    const QDateTime modified = QFileInfo(layerPath).lastModified();
    QByteArray invalid = original;
    ASSERT_TRUE(invalid.contains("\"Main\""));
    invalid.replace("\"Main\"", "false ");
    ASSERT_EQ(invalid.size(), original.size());
    ASSERT_TRUE(layer.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(layer.write(invalid), invalid.size());
    ASSERT_TRUE(layer.flush());
    ASSERT_TRUE(layer.setFileTime(modified, QFileDevice::FileModificationTime));
    layer.close();
    EXPECT_EQ(QFileInfo(layerPath).lastModified(), modified);
    EXPECT_FALSE(compare());

    ASSERT_TRUE(layer.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(layer.write(original), original.size());
    ASSERT_TRUE(layer.flush());
    ASSERT_TRUE(layer.setFileTime(modified, QFileDevice::FileModificationTime));
    layer.close();
    EXPECT_EQ(QFileInfo(layerPath).lastModified(), modified);
    EXPECT_TRUE(compare());

    const QString duplicatePath = directory.filePath(QStringLiteral("Layers/Copy.vp"));
    ASSERT_TRUE(QFile::copy(layerPath, duplicatePath));
    EXPECT_FALSE(compare());
    ASSERT_TRUE(QFile::remove(duplicatePath));
    EXPECT_TRUE(compare());

    QJsonObject root = readJson(projectPath);
    const QJsonObject originalRoot = root;
    QJsonObject header = root.value(QStringLiteral("harkenDocument")).toObject();
    header.insert(QStringLiteral("schemaVersion"), 0);
    root.insert(QStringLiteral("harkenDocument"), header);
    writeJson(projectPath, root);
    EXPECT_FALSE(compare());
    writeJson(projectPath, originalRoot);
    EXPECT_TRUE(compare());
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
    {
        std::unique_ptr<VNX::ProjectTransaction> transaction =
            VNX::ProjectTransaction::begin(
                projectPath, {QStringLiteral("Baseline.hsp")},
                recoveryRoot, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
            << error.toStdString();
    }
    EXPECT_NE(readJson(projectPath), before);

    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error))
        << error.toStdString();
    EXPECT_EQ(readJson(projectPath), before);
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

TEST(ProjectTransaction, MixedReplacementRecoversChangedAddedDeletedAndUnchangedFiles)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject projectBefore = readJson(projectPath);
    const QJsonObject retained{{QStringLiteral("retained"), true}};
    const QJsonObject deleted{{QStringLiteral("deleted"), true}};
    const QString retainedPath = directory.filePath(QStringLiteral("Layers/Retained.bin"));
    const QString deletedPath = directory.filePath(QStringLiteral("Layers/Deleted.bin"));
    const QString addedPath = directory.filePath(QStringLiteral("Layers/Added.bin"));
    writeJson(retainedPath, retained);
    writeJson(deletedPath, deleted);
    writeJson(staging.filePath(QFileInfo(projectPath).fileName()),
              QJsonObject{{QStringLiteral("changed"), true}});
    writeJson(staging.filePath(QStringLiteral("Layers/Retained.bin")), retained);
    writeJson(staging.filePath(QStringLiteral("Layers/Added.bin")),
              QJsonObject{{QStringLiteral("added"), true}});
    const QStringList writeSet{QFileInfo(projectPath).fileName(),
        QStringLiteral("Layers/Added.bin"), QStringLiteral("Layers/Deleted.bin"),
        QStringLiteral("Layers/Retained.bin")};
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::beginReplacement(
            projectPath, writeSet, staging.path(), &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        EXPECT_THROW(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error,
            [&](int completed, int, qint64, qint64) {
                if (completed == 2)
                    throw std::runtime_error("Interrupted replacement");
            }), std::runtime_error);
        EXPECT_EQ(readJson(retainedPath), retained);
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), projectBefore);
    EXPECT_EQ(readJson(retainedPath), retained);
    EXPECT_EQ(readJson(deletedPath), deleted);
    EXPECT_FALSE(QFileInfo::exists(addedPath));
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
}

TEST(ProjectTransaction, UnchangedReplacementFilesStillRejectExternalChanges)
{
    for (const int phase : {0, 1, 2, 3}) {
        SCOPED_TRACE(phase);
        QTemporaryDir directory;
        QTemporaryDir staging;
        const QString projectPath = createBaselineProject(directory);
        const QJsonObject projectBefore = readJson(projectPath);
        const QJsonObject projectAfter{{QStringLiteral("changed"), true}};
        const QJsonObject retained{{QStringLiteral("retained"), true}};
        const QJsonObject external{{QStringLiteral("external"), true}};
        const QString retainedPath = directory.filePath(QStringLiteral("Layers/Retained.bin"));
        writeJson(retainedPath, retained);
        writeJson(staging.filePath(QFileInfo(projectPath).fileName()), projectAfter);
        writeJson(staging.filePath(QStringLiteral("Layers/Retained.bin")), retained);
        QString error;
        auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
            {QFileInfo(projectPath).fileName(), QStringLiteral("Layers/Retained.bin")},
            staging.path(), &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        const bool replaced = phase == 1 || phase == 2;
        if (replaced)
            ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
                << error.toStdString();
        writeJson(retainedPath, external);
        if (phase == 0) {
            EXPECT_FALSE(transaction->replaceFromDirectory(staging.path(), &error));
        } else if (phase == 1 || phase == 3) {
            EXPECT_FALSE(transaction->commit(false, &error));
        } else {
            transaction.reset();
            EXPECT_FALSE(VNX::ProjectTransaction::recover(projectPath, &error));
        }
        EXPECT_EQ(readJson(retainedPath), external);
        EXPECT_EQ(readJson(projectPath), replaced ? projectAfter : projectBefore);
        transaction.reset();
        EXPECT_TRUE(VNX::ProjectTransaction::recoveryRequired(projectPath));
        writeJson(retainedPath, retained);
        ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
        EXPECT_EQ(readJson(projectPath), projectBefore);
        EXPECT_EQ(readJson(retainedPath), retained);
    }
}

TEST(ProjectTransaction, UnchangedReplacementCanCommitWithoutInstallation)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject original = readJson(projectPath);
    writeJson(staging.filePath(QFileInfo(projectPath).fileName()), original);
    QString error;
    auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
        {QFileInfo(projectPath).fileName()}, staging.path(), &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), original);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
}

TEST(ProjectTransaction, ChangedStagingCannotRetargetAnUnchangedReplacement)
{
    for (const bool alreadyReplaced : {false, true}) {
        SCOPED_TRACE(alreadyReplaced);
        QTemporaryDir directory;
        QTemporaryDir staging;
        const QString projectPath = createBaselineProject(directory);
        const QJsonObject projectBefore = readJson(projectPath);
        const QJsonObject retained{{QStringLiteral("retained"), true}};
        const QString retainedPath = directory.filePath(QStringLiteral("Layers/Retained.bin"));
        const QString stagedPath = staging.filePath(QStringLiteral("Layers/Retained.bin"));
        writeJson(retainedPath, retained);
        writeJson(stagedPath, retained);
        QString error;
        auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
            {QStringLiteral("Layers/Retained.bin")}, staging.path(), &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        if (alreadyReplaced)
            ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
                << error.toStdString();
        writeJson(stagedPath, QJsonObject{{QStringLiteral("retargeted"), true}});
        EXPECT_FALSE(transaction->replaceFromDirectory(staging.path(), &error));
        EXPECT_EQ(readJson(retainedPath), retained);
        EXPECT_EQ(readJson(projectPath), projectBefore);
        ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
        EXPECT_EQ(readJson(retainedPath), retained);
    }
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

TEST(ProjectTransaction, InterruptedInstallationRestoresOriginalFiles)
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
                if (completed == 1) {
                    throw std::runtime_error("Interrupted installation");
                }
            }), std::runtime_error);
    }

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
#endif

#if defined(Q_OS_WIN)
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
#endif

#if defined(Q_OS_WIN)
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
}

TEST(ProjectTransaction, IdenticalReplacementKeepsCaseOnlyRenameAndRecovery)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject contents{{QStringLiteral("unchanged"), true}};
    const QString original = directory.filePath(QStringLiteral("Layers/Upper/File.bin"));
    writeJson(original, contents);
    if (!QFileInfo::exists(directory.filePath(QStringLiteral("Layers/upper/file.bin"))))
        GTEST_SKIP() << "Requires a case-insensitive filesystem";
    writeJson(staging.filePath(QStringLiteral("Layers/upper/file.bin")), contents);
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
            {QStringLiteral("Layers/upper/file.bin")}, staging.path(), &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers"))).entryList().contains(QStringLiteral("upper")));
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/upper"))).entryList().contains(QStringLiteral("file.bin")));
        EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Layers/upper/file.bin"))), contents);
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers"))).entryList().contains(QStringLiteral("Upper")));
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/Upper"))).entryList().contains(QStringLiteral("File.bin")));
    EXPECT_EQ(readJson(original), contents);
}

TEST(ProjectTransaction, RootSpellingChangeCannotRetargetAnUnchangedReplacement)
{
    QTemporaryDir directory;
    QTemporaryDir staging;
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject retained{{QStringLiteral("retained"), true}};
    const QString original = directory.filePath(QStringLiteral("Layers/Retained.bin"));
    const QString staged = staging.filePath(QStringLiteral("Layers/Retained.bin"));
    writeJson(original, retained);
    if (!QFileInfo::exists(directory.filePath(QStringLiteral("layers/Retained.bin"))))
        GTEST_SKIP() << "Requires a case-insensitive filesystem";
    writeJson(staged, retained);
    QString error;
    auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
        {QStringLiteral("Layers/Retained.bin")}, staging.path(), &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    QDir projectRoot(directory.path());
    ASSERT_TRUE(projectRoot.rename(QStringLiteral("Layers"), QStringLiteral("LayersTemporary")));
    ASSERT_TRUE(projectRoot.rename(QStringLiteral("LayersTemporary"), QStringLiteral("layers")));
    writeJson(staged, QJsonObject{{QStringLiteral("retargeted"), true}});
    EXPECT_FALSE(transaction->replaceFromDirectory(staging.path(),
        QStringList{QStringLiteral("Layers")}, &error));
    EXPECT_EQ(readJson(original), retained);
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_EQ(readJson(original), retained);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
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
}
#endif
}
