#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ProjectDataPolicy.h"
#include "Core/ProjectLocator.h"
#include "Core/ProjectTransaction.h"
#include "Core/ProjectVolume.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

void writeFile(const QString& path, const QByteArray& content)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(content), content.size());
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

TEST(ProjectSafetyFoundation, LocatorDoesNotGuessBetweenProjects)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    writeFile(directory.filePath(QStringLiteral("A.hsp")), QByteArrayLiteral("{}"));
    writeFile(directory.filePath(QStringLiteral("B.hsp")), QByteArrayLiteral("{}"));

    const VNX::ProjectLocation location = VNX::ProjectLocator::locate(directory.path());
    EXPECT_EQ(location.issue, VNX::ProjectIssue::ProjectAmbiguous);
    ASSERT_EQ(location.candidates.size(), 2);
    EXPECT_TRUE(location.candidates.at(0).endsWith(QStringLiteral("A.hsp")));
    EXPECT_TRUE(location.candidates.at(1).endsWith(QStringLiteral("B.hsp")));
}

TEST(ProjectSafetyFoundation, PathGettersDoNotCreateDirectories)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString audio = VNX::PathUtils::getAudioSourcesDir(directory.path());
    const QString media = VNX::PathUtils::getMediaCacheDir(directory.path());
    const QString index = VNX::PathUtils::getProjectIndexCacheDir(directory.path());

    EXPECT_FALSE(QDir(audio).exists());
    EXPECT_FALSE(QDir(media).exists());
    EXPECT_FALSE(QDir(index).exists());
}

TEST(ProjectSafetyFoundation, TransactionControlDataHasOneClassification)
{
    const VNX::ProjectDataPolicy policy;
    EXPECT_EQ(policy.classify(QStringLiteral(".harken-upgrade-receipt.json")),
              VNX::ProjectDataClass::TransactionControl);
    EXPECT_FALSE(policy.includeInContentFingerprint(
        QStringLiteral(".harken-upgrade-receipt.json")));
    EXPECT_FALSE(policy.protectByDefault(
        QStringLiteral(".harken-upgrade-receipt.json")));
}

TEST(ProjectSafetyFoundation, AudioSourcesAreNotHashedOrCopiedForDataUpgrade)
{
    const VNX::ProjectDataPolicy policy;
    EXPECT_FALSE(policy.includeInContentFingerprint(
        QStringLiteral("AudioSources/large.wav")));
    EXPECT_FALSE(policy.protectByDefault(
        QStringLiteral("AudioSources/large.wav")));
    EXPECT_TRUE(policy.includeInContentFingerprint(
        QStringLiteral("Layers/Main.vp")));
}

TEST(ProjectSafetyFoundation, CustomPackOutputDoesNotExcludeSiblingFiles)
{
    const VNX::ProjectDataPolicy policy(QStringLiteral(" ./Exports\\Unused/../Packs/ "));
    EXPECT_EQ(policy.classify(QStringLiteral("Exports/Packs/Windows/Main.vp")),
              VNX::ProjectDataClass::Derived);
    EXPECT_FALSE(policy.protectByDefault(QStringLiteral("Exports/Packs/HarkenPackIndex.json")));
    EXPECT_FALSE(policy.includeInContentFingerprint(QStringLiteral("Exports"), true));
    EXPECT_TRUE(policy.includeInContentFingerprint(QStringLiteral("Exports")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Exports/PacksNotes/readme.txt")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Exports/Notes.txt")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Layers/Main.vp")));
    EXPECT_FALSE(policy.protectByDefault(QStringLiteral("BuildPacks/Windows/Main.vpack")));
    EXPECT_FALSE(policy.protectByDefault(QStringLiteral("GeneratedPacks/Windows/Main.vpack")));
}

TEST(ProjectSafetyFoundation, ProjectRootPackOutputStillProtectsProjectDocuments)
{
    const VNX::ProjectDataPolicy policy(QStringLiteral("."));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Windows/Main.vpack")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("HarkenPackIndex.json")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Android_Low/Main.vpack")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Demo.hsp")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Layers/Main.vp")));
    EXPECT_TRUE(policy.protectByDefault(QStringLiteral("Notes.txt")));
}

TEST(ProjectSafetyFoundation, PackOutputRejectsProjectRootAliases)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString project = directory.filePath(QStringLiteral("Demo.hsp"));
    for (const auto& path : {QStringLiteral("."), QStringLiteral(" ./ "),
                            QStringLiteral("Packs/.."), QStringLiteral("../") + QDir(directory.path()).dirName(),
                            directory.path()}) {
        EXPECT_FALSE(VNX::PathUtils::isValidPackBuildOutputDir(path, project)) << path.toStdString();
        EXPECT_FALSE(VNX::PathUtils::isValidWaveformBuildOutputDir(path, project)) << path.toStdString();
        EXPECT_TRUE(VNX::PathUtils::isProjectRootDirectory(
            VNX::PathUtils::packBuildOutputRoot(project, path), project));
    }
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir({}, project));
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(QStringLiteral("Packs"), project));
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(QStringLiteral("../Delivery"), project));
    const VNX::ProjectDataPolicy policy(QStringLiteral("Packs"));
    EXPECT_FALSE(policy.includeInContentFingerprint(QStringLiteral("Packs/Android_Low/Init.pack")));
    EXPECT_FALSE(policy.includeInContentFingerprint(QStringLiteral("Packs/Retired/Init.pack")));
}

TEST(ProjectSafetyFoundation, PackOutputRejectsObjectBucketsAndTheirDescendants)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString project = directory.filePath(QStringLiteral("Demo.hsp"));
    for (const auto* name : {"Actions", "Layers", "Busses", "Variables", "Attenuations"}) {
        const QString bucket = QLatin1String(name);
        for (const auto& path : {bucket, bucket + QStringLiteral("/BuildPacks"),
                                QStringLiteral("Exports/../") + bucket + QStringLiteral("/Generated"),
                                QStringLiteral("../") + QDir(directory.path()).dirName()
                                    + QLatin1Char('/') + bucket + QStringLiteral("/BuildPacks"),
                                directory.filePath(bucket + QStringLiteral("/BuildPacks"))}) {
            EXPECT_FALSE(VNX::PathUtils::isValidPackBuildOutputDir(path, project)) << path.toStdString();
            EXPECT_FALSE(VNX::PathUtils::isValidWaveformBuildOutputDir(path, project)) << path.toStdString();
            EXPECT_TRUE(VNX::PathUtils::isProjectObjectDirectory(
                VNX::PathUtils::packBuildOutputRoot(project, path), project));
        }
        EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(bucket + QStringLiteral("Exports"), project));
#if defined(Q_OS_WIN)
        EXPECT_FALSE(VNX::PathUtils::isValidPackBuildOutputDir(bucket.toLower() + QStringLiteral("/Packs"), project));
#endif
    }
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir({}, project));
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(QStringLiteral("BuildPacks"), project));
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(QStringLiteral("../Layers/BuildPacks"), project));
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(QStringLiteral("Layers/../BuildPacks"), project));
}

TEST(ProjectSafetyFoundation, BuildOutputNormalizationPreservesDestination)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString project = directory.filePath(QStringLiteral("Demo.hsp"));
    for (const auto& path : {QStringLiteral(" ./Exports\\Unused/../Packs/ "),
                            directory.filePath(QStringLiteral("Exports/Packs"))}) {
        const QString normalized = VNX::PathUtils::normalizePackBuildOutputDir(path, project);
        EXPECT_TRUE(QDir::isRelativePath(normalized));
        EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(normalized, project));
        EXPECT_TRUE(VNX::PathUtils::isValidWaveformBuildOutputDir(normalized, project));
        EXPECT_EQ(VNX::PathUtils::packBuildOutputRoot(project, normalized),
                  directory.filePath(QStringLiteral("Exports/Packs")));
        EXPECT_EQ(VNX::PathUtils::waveformBuildOutputRoot(project, normalized),
                  directory.filePath(QStringLiteral("Exports/Packs")));
    }
#if defined(Q_OS_WIN)
    const QString windowsProject = QStringLiteral("D:/Audio/Project/Demo.hsp");
    for (const auto& path : {QStringLiteral("E:/Game/Assets/GeneratedPacks"),
                            QStringLiteral("E:/"), QStringLiteral("//localhost/Audio/GeneratedPacks")}) {
        const QString normalized = VNX::PathUtils::normalizePackBuildOutputDir(
            QDir::toNativeSeparators(path), windowsProject);
        EXPECT_TRUE(QDir::isAbsolutePath(normalized));
        EXPECT_EQ(VNX::PathUtils::packBuildOutputRoot(windowsProject, normalized), path);
        EXPECT_EQ(VNX::PathUtils::waveformBuildOutputRoot(windowsProject, normalized), path);
    }
    EXPECT_TRUE(VNX::PathUtils::isValidPackBuildOutputDir(QStringLiteral("E:/GeneratedPacks"), windowsProject));
    EXPECT_TRUE(VNX::PathUtils::isValidWaveformBuildOutputDir(QStringLiteral("E:/Waveforms"), windowsProject));
    for (const auto& path : {QStringLiteral("D:GeneratedPacks"), QStringLiteral("E:GeneratedPacks"),
                            QStringLiteral("D:"), QStringLiteral("1:/GeneratedPacks")}) {
        EXPECT_FALSE(VNX::PathUtils::isValidPackBuildOutputDir(path, windowsProject));
        const QString normalized = VNX::PathUtils::normalizePackBuildOutputDir(path, windowsProject);
        EXPECT_FALSE(VNX::PathUtils::isValidPackBuildOutputDir(normalized, windowsProject));
        EXPECT_FALSE(VNX::PathUtils::isValidWaveformBuildOutputDir(normalized, windowsProject));
    }
    EXPECT_FALSE(VNX::PathUtils::isValidWaveformBuildOutputDir(QStringLiteral("E:/Wave:forms"), windowsProject));
#endif
}

TEST(ProjectSafetyFoundation, PackOutputPolicyResolvesParentRelativePaths)
{
    QTemporaryDir project;
    ASSERT_TRUE(project.isValid());
    const QString projectFile = project.filePath(QStringLiteral("Demo.hsp"));
    const auto policyFor = [&](const QString& outputDir) {
        const QJsonObject root{{QStringLiteral("content"), QJsonObject{
            {QStringLiteral("packBuildOutputDir"), outputDir}}}};
        writeFile(projectFile, QJsonDocument(root).toJson());
        return VNX::ProjectDataPolicy::forProjectFile(projectFile);
    };
    const VNX::ProjectDataPolicy external = policyFor(QStringLiteral("../Exports/Packs"));
    EXPECT_TRUE(external.protectByDefault(QStringLiteral("Exports/Packs/Notes.txt")));
    EXPECT_TRUE(external.protectByDefault(QStringLiteral("Layers/Main.vp")));

    const VNX::ProjectDataPolicy internal = policyFor(
        QStringLiteral("../") + QDir(project.path()).dirName()
        + QStringLiteral("/Exports/Packs"));
    EXPECT_FALSE(internal.protectByDefault(QStringLiteral("Exports/Packs/Windows/Main.vpack")));
    EXPECT_TRUE(internal.protectByDefault(QStringLiteral("Exports/PacksNotes/readme.txt")));
    const VNX::ProjectDataPolicy rejected = policyFor(QStringLiteral("Layers/BuildPacks"));
    EXPECT_TRUE(rejected.protectByDefault(QStringLiteral("Layers/BuildPacks/Windows/Main.vpack")));
    EXPECT_TRUE(rejected.protectByDefault(QStringLiteral("Layers/Main.vp")));

    const QJsonObject absoluteRoot{{QStringLiteral("content"), QJsonObject{
        {QStringLiteral("packBuildOutputDir"), project.filePath(QStringLiteral("Exports/Packs"))},
        {QStringLiteral("buildWaveformOutputDir"), project.filePath(QStringLiteral("Exports/Waveforms"))}}}};
    writeFile(projectFile, QJsonDocument(absoluteRoot).toJson());
    const VNX::ProjectDataPolicy absolute = VNX::ProjectDataPolicy::forProjectFile(projectFile);
    EXPECT_FALSE(absolute.protectByDefault(QStringLiteral("Exports/Packs/Windows/Main.vpack")));
    EXPECT_FALSE(absolute.protectByDefault(QStringLiteral("Exports/Waveforms/Main.vkd")));
    EXPECT_TRUE(absolute.protectByDefault(QStringLiteral("Exports/Notes.txt")));
}

TEST(ProjectSafetyFoundation, DefaultTransactionDoesNotBackUpOrRollBackCustomPacks)
{
    QTemporaryDir project;
    ASSERT_TRUE(project.isValid());
    const QString projectFile = project.filePath(QStringLiteral("Demo.hsp"));
    const QJsonObject root{{QStringLiteral("content"), QJsonObject{
        {QStringLiteral("packBuildOutputDir"), QStringLiteral("Exports/Packs")}}}};
    writeFile(projectFile, QJsonDocument(root).toJson());
    const QString generated = QStringLiteral("Exports/Packs/Windows/Main.vp");
    const QString notes = QStringLiteral("Exports/PacksNotes/readme.txt");
    writeFile(project.filePath(generated), QByteArrayLiteral("generated"));
    writeFile(project.filePath(notes), QByteArrayLiteral("user notes"));

    QString error;
    auto transaction = VNX::ProjectTransaction::begin(project.path(), &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    EXPECT_FALSE(transaction->writeSet().contains(generated));
    EXPECT_FALSE(QFileInfo::exists(QDir(transaction->backupDirectory()).filePath(generated)));
    EXPECT_TRUE(transaction->writeSet().contains(QStringLiteral("Demo.hsp")));
    EXPECT_TRUE(transaction->writeSet().contains(notes));
    EXPECT_EQ(readFile(QDir(transaction->backupDirectory()).filePath(notes)),
              QByteArrayLiteral("user notes"));

    writeFile(project.filePath(generated), QByteArrayLiteral("rebuilt"));
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_EQ(readFile(project.filePath(generated)), QByteArrayLiteral("rebuilt"));
    EXPECT_EQ(readFile(project.filePath(notes)), QByteArrayLiteral("user notes"));
}

TEST(ProjectSafetyFoundation, RollbackLeavesAlreadyRestoredFilesUntouched)
{
    QTemporaryDir project;
    QTemporaryDir staging;
    ASSERT_TRUE(project.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString name = QStringLiteral("Demo.hsp");
    const QString projectFile = project.filePath(name);
    writeFile(projectFile, QByteArrayLiteral("old"));
    writeFile(staging.filePath(name), QByteArrayLiteral("new"));

    QString error;
    auto transaction = VNX::ProjectTransaction::begin(projectFile, {name}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
        << error.toStdString();
    writeFile(projectFile, QByteArrayLiteral("old"));
    QFile restored(projectFile);
    ASSERT_TRUE(restored.open(QIODevice::ReadWrite));
    ASSERT_TRUE(restored.setFileTime(QDateTime::fromSecsSinceEpoch(946684800),
                                     QFileDevice::FileModificationTime));
    restored.close();
    const auto restoredTime = QFileInfo(projectFile).lastModified().toMSecsSinceEpoch();

    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectFile));
    EXPECT_EQ(readFile(projectFile), QByteArrayLiteral("old"));
    EXPECT_EQ(QFileInfo(projectFile).lastModified().toMSecsSinceEpoch(), restoredTime);
}

TEST(ProjectSafetyFoundation, WriteSetTransactionLeavesLargeAndPreservedDataUntouched)
{
    QTemporaryDir project;
    QTemporaryDir staging;
    ASSERT_TRUE(project.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString projectFile = project.filePath(QStringLiteral("Demo.hsp"));
    const QString audio = project.filePath(QStringLiteral("AudioSources/large.wav"));
    const QString trash = project.filePath(QStringLiteral(".trash/old.vp"));
    writeFile(projectFile, QByteArrayLiteral("old"));
    writeFile(audio, QByteArray(1024 * 1024, 'w'));
    writeFile(trash, QByteArrayLiteral("recoverable"));
    writeFile(staging.filePath(QStringLiteral("Demo.hsp")), QByteArrayLiteral("new"));
    const QByteArray audioBefore = readFile(audio);
    const QByteArray trashBefore = readFile(trash);

    QString error;
    std::unique_ptr<VNX::ProjectTransaction> transaction =
        VNX::ProjectTransaction::begin(
            projectFile, {QStringLiteral("Demo.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
        << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();

    EXPECT_EQ(readFile(projectFile), QByteArrayLiteral("new"));
    EXPECT_EQ(readFile(audio), audioBefore);
    EXPECT_EQ(readFile(trash), trashBefore);
}

TEST(ProjectSafetyFoundation, StorageClassifierRejectsUnknownRemotePath)
{
    const VNX::ProjectVolumeInfo remote = VNX::ProjectVolume::classify(
        QStringLiteral("//invalid-harken-host/share/project"));
    EXPECT_EQ(remote.kind, VNX::ProjectVolumeKind::Remote);

    QTemporaryDir local;
    ASSERT_TRUE(local.isValid());
    const VNX::ProjectVolumeInfo localInfo = VNX::ProjectVolume::classify(local.path());
    EXPECT_NE(localInfo.kind, VNX::ProjectVolumeKind::Remote);
}

TEST(ProjectSafetyFoundation, StorageClassifierRejectsCloudFolder)
{
    QTemporaryDir local;
    ASSERT_TRUE(local.isValid());
    const QByteArray previous = qgetenv("OneDrive");
    ASSERT_TRUE(qputenv("OneDrive", QFile::encodeName(local.path())));
    const VNX::ProjectVolumeInfo cloud = VNX::ProjectVolume::classify(local.path());
    if (previous.isNull())
        qunsetenv("OneDrive");
    else
        qputenv("OneDrive", previous);
    EXPECT_EQ(cloud.kind, VNX::ProjectVolumeKind::Remote);
}

}
