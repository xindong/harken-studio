#include "Core/SchemaCompatibilityProbe.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

namespace {

QMap<QString, QJsonObject> projectDocuments(const QString& projectDirectory)
{
    QMap<QString, QJsonObject> result;
    QDirIterator files(projectDirectory,
                       QStringList{QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                       QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
        const QString id = document.value(QStringLiteral("harkenDocument")).toObject()
                               .value(QStringLiteral("id")).toString();
        EXPECT_FALSE(id.isEmpty());
        EXPECT_FALSE(result.contains(id));
        result.insert(id, document);
    }
    return result;
}

QJsonObject probe(const QString& mode, const QString& path)
{
    return VNX::runSchemaCompatibilityProbe({
        {QStringLiteral("requestVersion"), 1},
        {QStringLiteral("mode"), mode},
        {mode == QStringLiteral("create") ? QStringLiteral("outputDirectory")
                                          : QStringLiteral("projectPath"), path},
    });
}

void writeDocument(const QString& path, const QJsonObject& document)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(document).toJson();
    ASSERT_EQ(file.write(bytes), bytes.size());
}

TEST(SchemaCompatibilityProbe, CreatesAllDocumentTypesAndEditsOnlyReportedFields)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectDirectory = directory.filePath(QStringLiteral("Fixture"));
    const QJsonObject created = probe(QStringLiteral("create"), projectDirectory);
    ASSERT_TRUE(created.value(QStringLiteral("ok")).toBool())
        << created.value(QStringLiteral("error")).toString().toStdString();
    const QString projectPath = created.value(QStringLiteral("projectPath")).toString();
    const auto before = projectDocuments(projectDirectory);
    ASSERT_EQ(before.size(), 9);
    QSet<QString> types;
    for (const QJsonObject& document : before)
        types.insert(document.value(QStringLiteral("harkenDocument")).toObject()
                         .value(QStringLiteral("type")).toString());
    EXPECT_EQ(types.size(), 6);
    const QJsonObject read = probe(QStringLiteral("read"), projectPath);
    ASSERT_TRUE(read.value(QStringLiteral("ok")).toBool())
        << read.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(read.value(QStringLiteral("edits")).toArray().isEmpty());
    EXPECT_EQ(projectDocuments(projectDirectory), before);
    const QJsonObject edited = probe(QStringLiteral("edit"), projectPath);
    ASSERT_TRUE(edited.value(QStringLiteral("ok")).toBool())
        << edited.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(edited.value(QStringLiteral("schemaSnapshotHash")),
              created.value(QStringLiteral("schemaSnapshotHash")));
    EXPECT_EQ(edited.value(QStringLiteral("gitCommit")), created.value(QStringLiteral("gitCommit")));
    const auto after = projectDocuments(projectDirectory);
    ASSERT_EQ(before.keys(), after.keys());
    QMap<QString, QJsonObject> edits;
    for (const QJsonValue& value : edited.value(QStringLiteral("edits")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("documentId")).toString();
        ASSERT_TRUE(before.contains(id));
        ASSERT_FALSE(edits.contains(id));
        edits.insert(id, entry);
    }
    EXPECT_EQ(edits.size(), 8);
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        QJsonObject expected = it.value();
        QJsonObject header = expected.value(QStringLiteral("harkenDocument")).toObject();
        header.insert(QStringLiteral("savedStudioVersion"), edited.value(QStringLiteral("version")));
        expected.insert(QStringLiteral("harkenDocument"), header);
        QJsonObject content = expected.value(QStringLiteral("content")).toObject();
        const QJsonObject actualContent = after.value(it.key()).value(QStringLiteral("content")).toObject();
        const QJsonObject entry = edits.value(it.key());
        if (header.value(QStringLiteral("type")) == QStringLiteral("project.root")) {
            ASSERT_EQ(entry.value(QStringLiteral("jsonPath")),
                      QStringLiteral("/content/metadata/description"));
            QJsonObject metadata = content.value(QStringLiteral("metadata")).toObject();
            EXPECT_EQ(entry.value(QStringLiteral("before")), metadata.value(QStringLiteral("description")));
            EXPECT_EQ(entry.value(QStringLiteral("after")).toString(),
                      metadata.value(QStringLiteral("description")).toString()
                          + QStringLiteral("\nSchema compatibility edit"));
            metadata.insert(QStringLiteral("description"), entry.value(QStringLiteral("after")));
            metadata.insert(QStringLiteral("modifiedDate"),
                            actualContent.value(QStringLiteral("metadata")).toObject()
                                .value(QStringLiteral("modifiedDate")));
            content.insert(QStringLiteral("metadata"), metadata);
            const QJsonArray beforeHistory =
                content.value(QStringLiteral("actionPublishedNameHistory")).toArray();
            const QJsonArray afterHistory =
                actualContent.value(QStringLiteral("actionPublishedNameHistory")).toArray();
            ASSERT_GT(afterHistory.size(), beforeHistory.size());
            bool sawRename = false;
            for (const QJsonValue& value : afterHistory) {
                const QJsonObject row = value.toObject();
                if (row.value(QStringLiteral("event")).toString() == QStringLiteral("renamed_from")
                    && row.value(QStringLiteral("priorName")).toString() == QStringLiteral("Trigger")
                    && row.value(QStringLiteral("name")).toString()
                        == QStringLiteral("Trigger_schema_edit")) {
                    sawRename = true;
                    break;
                }
            }
            EXPECT_TRUE(sawRename);
            content.insert(QStringLiteral("actionPublishedNameHistory"), afterHistory);
        } else if (!entry.isEmpty()) {
            EXPECT_EQ(entry.value(QStringLiteral("jsonPath")), QStringLiteral("/content/name"));
            EXPECT_EQ(entry.value(QStringLiteral("before")), content.value(QStringLiteral("name")));
            EXPECT_EQ(entry.value(QStringLiteral("after")).toString(),
                      content.value(QStringLiteral("name")).toString()
                          + QStringLiteral("_schema_edit"));
            content.insert(QStringLiteral("name"), entry.value(QStringLiteral("after")));
            if (header.value(QStringLiteral("type")) == QStringLiteral("project.action")) {
                const qint64 beforeRevision =
                    content.value(QStringLiteral("nameBindingRevision")).toInteger();
                const qint64 afterRevision =
                    actualContent.value(QStringLiteral("nameBindingRevision")).toInteger();
                EXPECT_EQ(afterRevision, beforeRevision + 1);
                content.insert(QStringLiteral("nameBindingRevision"),
                               actualContent.value(QStringLiteral("nameBindingRevision")));
            }
        }
        if (content.contains(QStringLiteral("objectPath"))) {
            QStringList originalNames;
            QStringList renamedNames;
            QString ancestorId = it.key();
            QSet<QString> ancestors;
            while (!ancestorId.isEmpty()) {
                ASSERT_TRUE(before.contains(ancestorId));
                ASSERT_FALSE(ancestors.contains(ancestorId));
                ancestors.insert(ancestorId);
                const QJsonObject ancestor = HarkenTest::projectContent(before.value(ancestorId));
                const QString name = ancestor.value(QStringLiteral("name")).toString();
                originalNames.prepend(name);
                renamedNames.prepend(edits.contains(ancestorId)
                    ? edits.value(ancestorId).value(QStringLiteral("after")).toString() : name);
                const QJsonObject parent = ancestor.value(QStringLiteral("parentRef")).toObject();
                if (parent.value(QStringLiteral("kind")) == QStringLiteral("root")) break;
                ancestorId = parent.value(QStringLiteral("id")).toString();
                ASSERT_FALSE(ancestorId.isEmpty());
            }
            EXPECT_EQ(content.value(QStringLiteral("objectPath")).toString(),
                      QStringLiteral("\\") + originalNames.join(QLatin1Char('\\')));
            content.insert(QStringLiteral("objectPath"),
                           QStringLiteral("\\") + renamedNames.join(QLatin1Char('\\')));
        }
        if (header.value(QStringLiteral("type")) == QStringLiteral("project.attenuation")) {
            const auto attenuation = content.value(QStringLiteral("components")).toObject()
                .value(QStringLiteral("attenuation")).toObject();
            EXPECT_FALSE(attenuation.contains(QStringLiteral("name")));
            EXPECT_FALSE(attenuation.contains(QStringLiteral("guid")));
        }
        expected.insert(QStringLiteral("content"), content);
        EXPECT_EQ(expected, after.value(it.key())) << it.key().toStdString();
    }
}

TEST(SchemaCompatibilityProbe, ExistingOutputAndUpgradeRequestsAreRejected)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QJsonObject existing = probe(QStringLiteral("create"), directory.path());
    EXPECT_FALSE(existing.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(QDir(directory.path()).isEmpty());
    const QJsonObject upgrade = VNX::runSchemaCompatibilityProbe({
        {QStringLiteral("requestVersion"), 1},
        {QStringLiteral("mode"), QStringLiteral("read")},
        {QStringLiteral("projectPath"), directory.filePath(QStringLiteral("Test.hsp"))},
        {QStringLiteral("allowUpgrade"), true},
    });
    EXPECT_FALSE(upgrade.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(QDir(directory.path()).isEmpty());
}

TEST(SchemaCompatibilityProbe, RetiredProjectsCannotBeReadOrMigrated)
{
    for (int version : {1, 2}) {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const QString projectPath = directory.filePath(QStringLiteral("Legacy.hsp"));
        const QJsonObject original = HarkenTest::projectRoot({
            {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Legacy")}}},
            {QStringLiteral("paths"), QJsonObject{{QStringLiteral("audioFolder"), QStringLiteral("AudioSources")}}},
        }, version);
        writeDocument(projectPath, original);
        for (const QString& mode : {QStringLiteral("read"), QStringLiteral("migrate")}) {
            const auto report = probe(mode, projectPath);
            EXPECT_FALSE(report.value(QStringLiteral("ok")).toBool());
            EXPECT_TRUE(report.value(QStringLiteral("error")).toString().contains(
                mode == QStringLiteral("read") ? QStringLiteral("no longer supports")
                                               : QStringLiteral("PROJECT_SCHEMA_UNSUPPORTED")))
                << QJsonDocument(report).toJson().toStdString();
            EXPECT_EQ(projectDocuments(directory.path()).value(HarkenTest::kProjectId), original);
        }
    }
}

}
