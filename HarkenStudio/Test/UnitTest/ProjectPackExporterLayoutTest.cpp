#include "ProjectPackExporter.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

TEST(EditorProjectPackExporterLayout, AlwaysNestsPlatformDirectory)
{
    const QString root = QStringLiteral("C:/BuildPacks");
    EXPECT_EQ(VNX::ProjectPackExporter::platformOutputDir(
                  root, HarkenTest::platforms({"Windows"}).front()),
              QDir(root).filePath(QStringLiteral("Windows")));
}

TEST(EditorProjectPackExporterLayout, RejectsUnbuiltPlatformWithoutCreatingOutputs)
{
    QTemporaryDir dir;
    QString error;
    EXPECT_FALSE(VNX::ProjectPackExporter::ValidatePackOutputs(
        dir.path(), HarkenTest::platforms({"Windows"}), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFile::exists(QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName())));
}

TEST(EditorProjectPackExporterLayout, FailedFirstBuildLeavesNoPublishedIndex)
{
    QTemporaryDir dir;
    QString error;
    ASSERT_TRUE(VNX::ProjectPackExporter::FinalizePackOutputs(dir.path(),
        HarkenTest::platforms({"Windows", "Android"}), HarkenTest::platformIdStrings({"Windows", "Android"}), {}, &error));
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty());
}

TEST(EditorProjectPackExporterLayout, MultiPlatformRootKeepsPlatformDirectoriesWithoutIndex)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const auto platforms = HarkenTest::platforms({"Windows", "Android"});
    QString error;
    ASSERT_TRUE(VNX::ProjectPackExporter::FinalizePackOutputs(dir.path(), platforms, {}, {}, &error))
        << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName())));
    for (const auto& platform : platforms) {
        const QString nested = VNX::ProjectPackExporter::platformOutputDir(
            dir.path(), platform);
        EXPECT_EQ(QFileInfo(nested).fileName(), platform.name);
        EXPECT_EQ(QFileInfo(nested).absolutePath(), QFileInfo(dir.path()).absoluteFilePath());
    }
}
