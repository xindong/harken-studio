#include "Core/ProjectPreflight/ProjectPreflight.h"

#include "Core/MigrationManager.h"
#include "Core/ProjectUpgradeManager.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

using VNX::MigrationManager;
using VNX::PreflightReport;
using VNX::ProjectPreflight;

namespace {

QString writeProject(const QTemporaryDir& dir, int schemaVersion)
{
    const QString path = dir.filePath(QStringLiteral("Demo.hsp"));
    const QJsonObject root = HarkenTest::projectRoot(QJsonObject{
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Demo")},
        }},
        {QStringLiteral("paths"), QJsonObject{
            {QStringLiteral("audioFolder"), QStringLiteral("AudioSources")},
        }},
    }, schemaVersion);
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(QJsonDocument(root).toJson());
    file.close();
    EXPECT_TRUE(HarkenTest::writeRootBusForProject(path));
    return path;
}

void writeLayerPack(const QTemporaryDir& dir,
                    const QString& parentKind,
                    const QJsonArray& children)
{
    QDir root(dir.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("Layers")));
    QFile file(root.filePath(QStringLiteral("Layers/Test.vp")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QJsonObject parentRef{
        {QStringLiteral("kind"), parentKind},
    };
    if (parentKind != QStringLiteral("root")) {
        parentRef.insert(QStringLiteral("id"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    const QString guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject content{
        {QStringLiteral("type"), QStringLiteral("Layer")},
        {QStringLiteral("guid"), guid},
        {QStringLiteral("name"), QStringLiteral("Test")},
        {QStringLiteral("runtimeStableId"), 1},
        {QStringLiteral("parentRef"), parentRef},
        {QStringLiteral("children"), children},
        {QStringLiteral("order"), 0},
    };
    const QJsonObject pack = HarkenTest::projectDocument(
        QStringLiteral("project.layer"), guid, HarkenTest::kProjectId, content);
    file.write(QJsonDocument(pack).toJson());
}

TEST(ProjectPreflight, AllowsMatchingSchema)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);

    const PreflightReport report = ProjectPreflight::run(path, 0);
    EXPECT_TRUE(report.blockingErrors.isEmpty());
    EXPECT_EQ(report.currentSchema,
              MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
    EXPECT_EQ(report.targetSchema, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
}

TEST(ProjectPreflight, SavedStudioVersionDoesNotGateMatchingSchema)
{
    for (const QString& savedVersion : {
             QStringLiteral("0.0.1"), QStringLiteral("9999.99.99")}) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString path = writeProject(
            dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonObject header = HarkenTest::projectHeader(document);
        header.insert(QStringLiteral("savedStudioVersion"), savedVersion);
        document.insert(QStringLiteral("harkenDocument"), header);
        const QByteArray bytes = QJsonDocument(document).toJson();
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(bytes), bytes.size());
        file.close();

        const PreflightReport report = ProjectPreflight::run(path, 0);
        EXPECT_TRUE(report.blockingErrors.isEmpty());
        const VNX::ProjectUpgradeAnalysis analysis =
            VNX::ProjectUpgradeManager().analyzeForProjectOpen(path);
        EXPECT_TRUE(analysis.ok) << analysis.blockingErrors.join('\n').toStdString();
        EXPECT_FALSE(analysis.upgradeRequired);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), bytes);
    }
}

TEST(ProjectPreflight, BlocksMissingFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PreflightReport report = ProjectPreflight::run(
        dir.filePath(QStringLiteral("Missing.hsp")), 0);
    EXPECT_FALSE(report.blockingErrors.isEmpty());
}

TEST(ProjectPreflight, BlocksUnparsableFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Broken.hsp"));
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(QByteArrayLiteral("{not json"));
    file.close();

    const PreflightReport report = ProjectPreflight::run(path, 0);
    EXPECT_FALSE(report.blockingErrors.isEmpty());
}

TEST(ProjectPreflight, BlocksNewerSchema)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION + 1);

    const PreflightReport report = ProjectPreflight::run(path, 0);
    ASSERT_FALSE(report.blockingErrors.isEmpty());
    EXPECT_TRUE(report.blockingErrors.first().contains(QStringLiteral("newer")));
}

TEST(ProjectPreflight, BlocksMissingFormalBaseline)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeProject(
        dir, 0);

    const PreflightReport report = ProjectPreflight::run(path, 0);
    ASSERT_FALSE(report.blockingErrors.isEmpty());
    EXPECT_TRUE(report.blockingErrors.first().contains(
        QStringLiteral("PROJECT_FILE_DAMAGED")));
}

TEST(ProjectPreflight, BlocksTargetSchemaMismatch)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);

    const PreflightReport report = ProjectPreflight::run(
        path, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION + 1);
    ASSERT_FALSE(report.blockingErrors.isEmpty());
}

TEST(ProjectPreflight, BlocksUnknownRootKeys)
{
    const QStringList keys{
        QStringLiteral("unknownRootField"),
        QStringLiteral("anotherUnknownRootField"),
    };
    for (const QString& key : keys) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString path =
            writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonObject content = HarkenTest::projectContent(root);
        content[key] = QJsonArray{};
        root[QStringLiteral("content")] = content;
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();

        const PreflightReport report = ProjectPreflight::run(path, 0);
        ASSERT_FALSE(report.blockingErrors.isEmpty()) << key.toStdString();
        EXPECT_TRUE(report.blockingErrors.join(QLatin1Char('\n')).contains(key))
            << key.toStdString();
    }
}

TEST(ProjectPreflight, BlocksInlineLayerChildren)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
    writeLayerPack(dir, QStringLiteral("root"),
                   QJsonArray{QJsonObject{{QStringLiteral("name"),
                                          QStringLiteral("Child")}}});

    const PreflightReport report = ProjectPreflight::run(path, 0);
    ASSERT_FALSE(report.blockingErrors.isEmpty());
    EXPECT_TRUE(report.blockingErrors.join(QLatin1Char('\n')).contains(
        QStringLiteral("children")));
}

TEST(ProjectPreflight, BlocksLegacyLayerParentKind)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
    writeLayerPack(dir, QStringLiteral("layer"), QJsonArray{});

    const PreflightReport report = ProjectPreflight::run(path, 0);
    ASSERT_FALSE(report.blockingErrors.isEmpty());
    EXPECT_TRUE(report.blockingErrors.join(QLatin1Char('\n')).contains(
        QStringLiteral("parentRef.kind")));
}

TEST(ProjectPreflight, ReportSerializesToJson)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path =
        writeProject(dir, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);

    const PreflightReport report = ProjectPreflight::run(path, 0);
    const QJsonObject json = report.toJson();
    EXPECT_EQ(json[QStringLiteral("currentSchema")].toInt(),
              MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION);
    EXPECT_TRUE(json.contains(QStringLiteral("blockingErrors")));
    EXPECT_FALSE(json.contains(QStringLiteral("migrationRequired")));
}

}
