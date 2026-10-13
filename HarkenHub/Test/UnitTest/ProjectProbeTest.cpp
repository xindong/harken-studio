#include "Projects/ProjectProbe.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using VNH::ProjectProbe;
using VNH::ProjectProbeResult;

namespace {

QString writeProjectFile(const QString& path, int schemaVersion,
                         const QString& name,
                         const QString& projectId =
                             QStringLiteral("11111111-2222-3333-4444-555555555555"))
{
    QJsonObject header{
        {QStringLiteral("type"), QStringLiteral("project.root")},
        {QStringLiteral("id"), projectId},
        {QStringLiteral("projectId"), projectId},
        {QStringLiteral("schemaVersion"), schemaVersion},
    };
    if (schemaVersion >= 2)
        header.insert(QStringLiteral("savedStudioVersion"), QStringLiteral("1.4.0"));
    const QJsonObject root{
        {QStringLiteral("harkenDocument"), header},
        {QStringLiteral("content"), QJsonObject{
            {QStringLiteral("metadata"), QJsonObject{
                {QStringLiteral("name"), name},
            }},
        }},
    };

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(QJsonDocument(root).toJson());
    file.close();
    return path;
}

}

TEST(ProjectProbe, ReadsSchemaNameAndCanonicalPath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeProjectFile(dir.filePath(QStringLiteral("Demo.hsp")), 5,
                                          QStringLiteral("Demo Project"));
    ASSERT_FALSE(path.isEmpty());

    const ProjectProbeResult result = ProjectProbe::probe(path);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.schemaVersion, 5);
    EXPECT_EQ(result.savedStudioVersion, QStringLiteral("1.4.0"));
    EXPECT_EQ(result.displayName, QStringLiteral("Demo Project"));
    EXPECT_FALSE(result.projectId.isEmpty());
    EXPECT_FALSE(result.canonicalPath.isEmpty());
}

TEST(ProjectProbe, ReadsProjectIdWhenPresent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeProjectFile(dir.filePath(QStringLiteral("Future.hsp")), 6,
                                          QStringLiteral("Future"),
                                          QStringLiteral("11111111-2222-3333-4444-555555555555"));
    ASSERT_FALSE(path.isEmpty());

    const ProjectProbeResult result = ProjectProbe::probe(path);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.schemaVersion, 6);
    EXPECT_EQ(result.projectId,
              QStringLiteral("11111111-2222-3333-4444-555555555555"));
}

TEST(ProjectProbe, ResolvesProjectFileInsideDirectory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString subDirPath = dir.filePath(QStringLiteral("MyGame"));
    ASSERT_TRUE(QDir().mkpath(subDirPath));
    ASSERT_FALSE(writeProjectFile(subDirPath + QStringLiteral("/MyGame.hsp"), 5,
                                  QStringLiteral("MyGame"))
                     .isEmpty());

    const ProjectProbeResult result = ProjectProbe::probe(subDirPath);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.displayName, QStringLiteral("MyGame"));
}

TEST(ProjectProbe, MissingPathFails)
{
    const ProjectProbeResult result =
        ProjectProbe::probe(QStringLiteral("Z:/does/not/exist.hsp"));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
}

TEST(ProjectProbe, InvalidJsonIsLeftForStudio)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Broken.hsp"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not json at all");
    file.close();

    const ProjectProbeResult result = ProjectProbe::probe(path);
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.metadataWarning.isEmpty());
}

TEST(ProjectProbe, UnknownHeaderIsLeftForStudioWithoutWriting)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Development.hsp"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{
        {QStringLiteral("schemaVersion"), 9},
    }).toJson());
    file.close();
    const QByteArray before = [&]() {
        QFile input(path);
        EXPECT_TRUE(input.open(QIODevice::ReadOnly));
        return input.readAll();
    }();

    const ProjectProbeResult result = ProjectProbe::probe(path);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.metadataWarning.size() > 0);
    QFile after(path);
    ASSERT_TRUE(after.open(QIODevice::ReadOnly));
    EXPECT_EQ(after.readAll(), before);
}

TEST(ProjectProbe, MissingSavedVersionAndUnknownFieldsDoNotChangeTheFile)
{
    QTemporaryDir dir;
    const auto path = writeProjectFile(dir.filePath(QStringLiteral("Demo.hsp")), 2, QStringLiteral("Demo"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto header = root.value(QStringLiteral("harkenDocument")).toObject();
    header.remove(QStringLiteral("savedStudioVersion"));
    header.insert(QStringLiteral("futureMetadata"), QJsonObject{{QStringLiteral("keep"), true}});
    root.insert(QStringLiteral("harkenDocument"), header);
    const auto bytes = QJsonDocument(root).toJson();
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
    const auto probe = ProjectProbe::probe(path);
    EXPECT_TRUE(probe.ok);
    EXPECT_TRUE(probe.savedStudioVersion.isEmpty());
    EXPECT_FALSE(probe.projectId.isEmpty());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), bytes);
}

TEST(ProjectProbe, MultipleProjectsInFolderAreNotGuessed)
{
    QTemporaryDir dir;
    writeProjectFile(dir.filePath(QStringLiteral("One.hsp")), 2, QStringLiteral("One"));
    writeProjectFile(dir.filePath(QStringLiteral("Two.hsp")), 2, QStringLiteral("Two"));
    const auto probe = ProjectProbe::probe(dir.path());
    EXPECT_FALSE(probe.ok);
    EXPECT_EQ(probe.candidates.size(), 2);
}
