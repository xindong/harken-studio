#include "Core/ProjectSchemaGenerated.h"
#include "Core/SchemaArchive.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

class SchemaRootOverride
{
public:
    explicit SchemaRootOverride(const QString& path)
        : previous_(qgetenv("HARKEN_SCHEMA_ROOT"))
    {
        qputenv("HARKEN_SCHEMA_ROOT", path.toUtf8());
    }

    ~SchemaRootOverride()
    {
        if (previous_.isNull())
            qunsetenv("HARKEN_SCHEMA_ROOT");
        else
            qputenv("HARKEN_SCHEMA_ROOT", previous_);
    }

private:
    QByteArray previous_;
};

void copyTree(const QString& source, const QString& target)
{
    QDirIterator iterator(source, QDir::Files, QDirIterator::Subdirectories);
    const QDir root(source);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString destination = QDir(target).filePath(root.relativeFilePath(path));
        ASSERT_TRUE(QDir().mkpath(QFileInfo(destination).absolutePath()));
        ASSERT_TRUE(QFile::copy(path, destination));
    }
}

void writeJson(const QString& path, const QJsonObject& object)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)), 0);
}

TEST(SchemaArchive, SnapshotFileTamperingIsRejected)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source = VNX::SchemaArchive::instance().rootPath();
    copyTree(source, directory.path());
    QFile schema(directory.filePath(QStringLiteral("Editor/%1/ProjectSerializedSchema.json")
                                       .arg(VNX::ProjectSchema::kProjectSchemaVersion)));
    ASSERT_TRUE(schema.open(QIODevice::Append));
    ASSERT_EQ(schema.write(" "), 1);
    schema.close();

    SchemaRootOverride override(directory.path());
    QString error;
    EXPECT_FALSE(VNX::SchemaArchive::instance().verifyBuildSnapshot(&error));
    EXPECT_TRUE(VNX::SchemaArchive::instance().projectSchema(
        VNX::ProjectSchema::kProjectSchemaVersion, &error).isEmpty());
}

TEST(SchemaArchive, ManifestIdentityTamperingIsRejected)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto& archive = VNX::SchemaArchive::instance();
    copyTree(archive.rootPath(), directory.path());
    QJsonObject manifest = archive.buildManifest();
    manifest.insert(QStringLiteral("inputs"), QJsonArray{});
    writeJson(directory.filePath(QStringLiteral("build.json")), manifest);
    SchemaRootOverride override(directory.path());
    QString error;
    EXPECT_FALSE(archive.verifyBuildSnapshot(&error));
}

}
