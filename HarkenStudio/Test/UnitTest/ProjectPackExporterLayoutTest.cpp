#include "ProjectPackExporter.h"
#include "PackSchemaGenerated.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <gtest/gtest.h>

TEST(EditorProjectPackExporterLayout, AlwaysNestsPlatformDirectory)
{
    const QString root = QStringLiteral("C:/BuildPacks");
    EXPECT_EQ(VNX::ProjectPackExporter::platformOutputDir(
                  root, HarkenTest::platforms({"Windows"}).front()),
              QDir(root).filePath(QStringLiteral("Windows")));
}

TEST(EditorProjectPackExporterLayout, RejectsUnbuiltPlatformWithoutCreatingIndex)
{
    QTemporaryDir dir;
    QString error;
    EXPECT_FALSE(VNX::ProjectPackExporter::WritePackIndex(
        dir.path(), HarkenTest::platforms({"Windows"}), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFile::exists(QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName())));
}

TEST(EditorProjectPackExporterLayout, FailedFirstBuildPublishesEmptyIndex)
{
    QTemporaryDir dir;
    QString error;
    ASSERT_TRUE(VNX::ProjectPackExporter::PublishPackIndex(dir.path(),
        HarkenTest::platforms({"Windows", "Android"}), HarkenTest::platformIdStrings({"Windows", "Android"}), {}, &error));
    QFile file(QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName()));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto index = QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_EQ(index.value("schema_version").toInt(),
              VNX::PackSchema::kPackManifestSchemaVersion);
    EXPECT_TRUE(index.value("platforms").toArray().isEmpty());
}

TEST(EditorProjectPackExporterLayout, MultiPlatformRootAlwaysHasIndex)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const auto platforms = HarkenTest::platforms({"Windows", "Android"});
    QString error;
    ASSERT_TRUE(VNX::ProjectPackExporter::PublishPackIndex(dir.path(), platforms, {}, {}, &error))
        << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(
        QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName())));
    for (const auto& platform : platforms) {
        const QString nested = VNX::ProjectPackExporter::platformOutputDir(
            dir.path(), platform);
        EXPECT_EQ(QFileInfo(nested).fileName(), platform.name);
        EXPECT_EQ(QFileInfo(nested).absolutePath(), QFileInfo(dir.path()).absoluteFilePath());
    }
}
