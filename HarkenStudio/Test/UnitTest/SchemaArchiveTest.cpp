#include "Core/ProjectSchemaGenerated.h"
#include "Core/PackSchemaGenerated.h"
#include "Core/ProjectContract.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTypes.h"
#include "Core/ProjectSchemaValidator.generated.h"
#include "Core/ProjectSchemaValidator.h"
#include "Core/SchemaArchive.h"
#include "Core/SchemaBuildGenerated.h"
#include "Core/Components/VSelection.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <utility>
#include <vector>

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

class RecordingRuleExecutor final : public VNX::ProjectSchemaRuleExecutor
{
public:
    QStringList calls;
    QStringList uniqueFields;
    QStringList mirrorDocumentTypes;
    QList<VNX::ProjectSchemaReferenceRule> references;
    QStringList acyclicPath;
    QString singletonDocumentType;
    QStringList singletonPath;
    QString singletonValue;
    QString singletonMessage;

    void uniqueAcrossProject(const QStringList& fields) override
    {
        calls.append(QStringLiteral("uniqueAcrossProject"));
        uniqueFields = fields;
    }
    void documentIdMirrorsRoot(const QStringList& documentTypes) override
    {
        calls.append(QStringLiteral("documentIdMirrorsRoot"));
        mirrorDocumentTypes = documentTypes;
    }
    void projectIdUniform() override { calls.append(QStringLiteral("projectIdUniform")); }
    void referencesResolve(
        const QList<VNX::ProjectSchemaReferenceRule>& value) override
    {
        calls.append(QStringLiteral("referencesResolve"));
        references = value;
    }
    void acyclic(const QStringList& referencePath) override
    {
        calls.append(QStringLiteral("acyclic"));
        acyclicPath = referencePath;
    }
    void singleton(const QString& documentType,
                   const QStringList& predicatePath,
                   const QString& expectedValue,
                   const QString& message) override
    {
        calls.append(QStringLiteral("singleton"));
        singletonDocumentType = documentType;
        singletonPath = predicatePath;
        singletonValue = expectedValue;
        singletonMessage = message;
    }
    void schemaVersionUniform() override { calls.append(QStringLiteral("schemaVersionUniform")); }
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

QJsonObject objectReference(const QString& id)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("documentId"), id},
    };
}

QJsonObject layerContent(const QString& type,
                         const QString& guid,
                         const QJsonObject& parentRef)
{
    return {
        {QStringLiteral("type"), type},
        {QStringLiteral("guid"), guid},
        {QStringLiteral("name"), type},
        {QStringLiteral("parentRef"), parentRef},
        {QStringLiteral("children"), QJsonArray{}},
        {QStringLiteral("order"), 0},
    };
}

QJsonObject validProjectRoot(int version = VNX::ProjectSchema::kProjectSchemaVersion)
{
    QJsonObject content{
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Test")},
        }},
        {QStringLiteral("paths"), QJsonObject{
            {QStringLiteral("audioFolder"), QStringLiteral("AudioSources")},
        }},
    };
    return HarkenTest::projectRoot(content, version);
}

void expectProjectOpensUsingCompiledSchema()
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = directory.filePath(QStringLiteral("Test.hsp"));
    QJsonObject root = validProjectRoot(VNX::ProjectSchema::kProjectSchemaVersion);
    const QString busId = QStringLiteral("bbbbbbbb-1111-2222-3333-444444444444");
    const QJsonObject bus{
        {QStringLiteral("type"), QStringLiteral("Bus")},
        {QStringLiteral("guid"), busId},
        {QStringLiteral("name"), QStringLiteral("Master")},
        {QStringLiteral("parentRef"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("root")},
        }},
        {QStringLiteral("componentData"), QJsonObject{}},
    };
    writeJson(projectPath, root);
    writeJson(directory.filePath(QStringLiteral("Busses/Master.vp")),
              HarkenTest::projectDocument(QStringLiteral("project.bus"), busId,
                                         HarkenTest::kProjectId, bus,
                                         VNX::ProjectSchema::kProjectSchemaVersion));
    const auto probe = VNX::ProjectContractRegistry::instance().probe(projectPath);
    ASSERT_TRUE(probe.ok) << probe.detail.toStdString();
    const QStringList errors = VNX::ProjectContractRegistry::instance().validateTree(
        directory.path(), root, true);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    ASSERT_TRUE(serializer.loadProject(projectPath, project))
        << serializer.lastError().toStdString();
    EXPECT_EQ(project.projectId, HarkenTest::kProjectId);
    EXPECT_EQ(project.name, QStringLiteral("Test"));
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), validProjectRoot(),
                    QStringLiteral("Legacy.hsp"))
                    .isEmpty());

    QJsonObject content = HarkenTest::projectContent(root);
    content.insert(QStringLiteral("futureBehavior"), true);
    root.insert(QStringLiteral("content"), content);
    writeJson(projectPath, root);
    EXPECT_FALSE(serializer.loadProject(projectPath, project));
    EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("futureBehavior")));
}

TEST(SchemaArchive, BuildSnapshotMatchesCompiledIdentityAndSeparateGenerations)
{
    QString error;
    VNX::SchemaArchive& archive = VNX::SchemaArchive::instance();
    ASSERT_TRUE(archive.verifyBuildSnapshot(&error)) << error.toStdString();
    const QJsonObject manifest = archive.buildManifest(&error);
    ASSERT_FALSE(manifest.isEmpty()) << error.toStdString();
    EXPECT_EQ(manifest.value(QStringLiteral("snapshotHash")).toString(),
              QString::fromLatin1(VNX::SchemaBuild::kSnapshotHash));
    const QJsonObject trees = manifest.value(QStringLiteral("trees")).toObject();
    EXPECT_EQ(trees.value(QStringLiteral("Editor")).toObject()
                  .value(QStringLiteral("currentVersion")).toInt(),
              VNX::ProjectSchema::kProjectSchemaVersion);
    QJsonArray editorVersions;
    for (const int version : VNX::ProjectSchema::kProjectSchemaVersions)
        editorVersions.append(version);
    EXPECT_EQ(trees.value(QStringLiteral("Editor")).toObject()
                  .value(QStringLiteral("supportedVersions")).toArray(), editorVersions);
    EXPECT_EQ(trees.value(QStringLiteral("Pack")).toObject()
                  .value(QStringLiteral("currentVersion")).toInt(),
              VNX::PackSchema::kPackManifestSchemaVersion);
    const QDir root(archive.rootPath());
    for (const int version : VNX::ProjectSchema::kProjectSchemaVersions) {
        EXPECT_TRUE(root.exists(QStringLiteral("Editor/%1/ProjectSerializedSchema.json").arg(version)));
        const auto snapshot = archive.projectSchema(version, &error);
        ASSERT_FALSE(snapshot.isEmpty()) << error.toStdString();
        EXPECT_EQ(snapshot.value(QStringLiteral("projectSchemaVersion")).toInt(), version);
    }
    EXPECT_FALSE(root.exists(QStringLiteral("Editor/1")));
    EXPECT_FALSE(root.exists(QStringLiteral("Editor/2")));
    const auto packVersions = trees.value(QStringLiteral("Pack")).toObject()
                                  .value(QStringLiteral("supportedVersions")).toArray();
    EXPECT_TRUE(packVersions.contains(VNX::PackSchema::kPackManifestSchemaVersion));
    for (const auto versionValue : packVersions) {
        const int version = versionValue.toInt();
        EXPECT_TRUE(root.exists(QStringLiteral("Pack/%1/PackManifestSchema.json").arg(version)));
        const auto snapshot = archive.packSchema(version, &error);
        ASSERT_FALSE(snapshot.isEmpty()) << error.toStdString();
        EXPECT_EQ(snapshot.value(QStringLiteral("x-harken-format")).toObject()
                      .value(QStringLiteral("formatVersion")).toInt(), version);
    }
    EXPECT_FALSE(root.exists(QStringLiteral("Editor/current")));
    EXPECT_FALSE(root.exists(QStringLiteral("Pack/current")));
    EXPECT_FALSE(root.exists(QStringLiteral("Pack/%1").arg(
        VNX::PackSchema::kPackManifestSchemaVersion + 1)));
    EXPECT_FALSE(root.exists(QStringLiteral("versions")));
    EXPECT_FALSE(root.exists(QStringLiteral("Editor/versions")));
    EXPECT_FALSE(root.exists(QStringLiteral("Pack/versions")));
    const QJsonObject current = archive.projectSchema(VNX::ProjectSchema::kProjectSchemaVersion, &error);
    ASSERT_FALSE(current.isEmpty()) << error.toStdString();
    EXPECT_EQ(current.value(QStringLiteral("projectSchemaVersion")).toInt(),
              VNX::ProjectSchema::kProjectSchemaVersion);
    EXPECT_EQ(current.value(QStringLiteral("x-harken-documentTypes")).toArray().size(), 6);
    const QJsonObject pack = archive.packSchema(
        VNX::PackSchema::kPackManifestSchemaVersion, &error);
    ASSERT_FALSE(pack.isEmpty()) << error.toStdString();
    EXPECT_EQ(pack.value(QStringLiteral("x-harken-format")).toObject()
                  .value(QStringLiteral("formatId")).toString(),
              QStringLiteral("generated.pack-companion"));
    EXPECT_TRUE(archive.projectSchema(VNX::ProjectSchema::kProjectSchemaVersion + 1, &error).isEmpty());
    EXPECT_TRUE(archive.packSchema(VNX::PackSchema::kPackManifestSchemaVersion + 1, &error).isEmpty());
    EXPECT_TRUE(archive.packSchema(VNX::PackSchema::kPackManifestSchemaVersion + 2, &error).isEmpty());

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
    EXPECT_EQ(error, QStringLiteral("SCHEMA_SNAPSHOT_MISMATCH"));
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
    EXPECT_EQ(error, QStringLiteral("SCHEMA_SNAPSHOT_MISMATCH"));
}

TEST(SchemaArchive, CurrentAliasCannotAppearInInstalledSnapshot)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto& archive = VNX::SchemaArchive::instance();
    copyTree(archive.rootPath(), directory.path());
    ASSERT_TRUE(QDir(directory.path()).mkpath(QStringLiteral("Editor/current")));
    SchemaRootOverride override(directory.path());
    QString error;
    EXPECT_FALSE(archive.verifyBuildSnapshot(&error));
    EXPECT_EQ(error, QStringLiteral("SCHEMA_SNAPSHOT_MISMATCH"));
}

TEST(ProjectContract, MissingInstalledSnapshotDoesNotBlockOpening)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    SchemaRootOverride override(directory.path());
    QString error;
    EXPECT_FALSE(VNX::SchemaArchive::instance().verifyBuildSnapshot(&error));
    expectProjectOpensUsingCompiledSchema();
}

TEST(ProjectContract, DamagedInstalledSnapshotDoesNotBlockOpening)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    copyTree(VNX::SchemaArchive::instance().rootPath(), directory.path());
    writeJson(directory.filePath(QStringLiteral("Editor/%1/ProjectSerializedSchema.json")
                                     .arg(VNX::ProjectSchema::kProjectSchemaVersion)),
              QJsonObject{{QStringLiteral("invalid"), true}});
    SchemaRootOverride override(directory.path());
    QString error;
    EXPECT_FALSE(VNX::SchemaArchive::instance().verifyBuildSnapshot(&error));
    expectProjectOpensUsingCompiledSchema();
}

TEST(ProjectSchemaValidator, RegistryCoversFormalVersions)
{
    EXPECT_FALSE(VNX::ProjectSchemaValidator::supportsProjectVersion(1));
    EXPECT_FALSE(VNX::ProjectSchemaValidator::supportsProjectVersion(2));
    for (const int version : VNX::ProjectSchema::kProjectSchemaVersions)
        EXPECT_TRUE(VNX::ProjectSchemaValidator::supportsProjectVersion(version));
    EXPECT_FALSE(VNX::ProjectSchemaValidator::supportsProjectVersion(
        VNX::ProjectSchema::kProjectSchemaVersion + 1));
    EXPECT_TRUE(VNX::ProjectSchemaValidator::supportsPackVersion(
        VNX::PackSchema::kPackManifestSchemaVersion));
    EXPECT_FALSE(VNX::ProjectSchemaValidator::supportsPackVersion(
        VNX::PackSchema::kPackManifestSchemaVersion + 1));
    EXPECT_FALSE(VNX::ProjectSchemaValidator::supportsPackVersion(
        VNX::PackSchema::kPackManifestSchemaVersion + 2));

}

TEST(ProjectSchemaValidator, AcceptsValidProjectRoot)
{
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), validProjectRoot(),
                    QStringLiteral("Test.hsp"))
                    .isEmpty());
}

TEST(ProjectSchemaValidator, RejectsInvalidProjectFields)
{
    QJsonObject missing = validProjectRoot();
    QJsonObject missingContent = HarkenTest::projectContent(missing);
    missingContent.remove(QStringLiteral("metadata"));
    missing.insert(QStringLiteral("content"), missingContent);
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), missing,
                    QStringLiteral("Test.hsp"))
                    .join(QLatin1Char('\n'))
                    .contains(QStringLiteral("$.content.metadata")));

    QJsonObject wrongType = validProjectRoot();
    QJsonObject wrongTypeContent = HarkenTest::projectContent(wrongType);
    wrongTypeContent.insert(QStringLiteral("paths"), QJsonArray{});
    wrongType.insert(QStringLiteral("content"), wrongTypeContent);
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), wrongType,
                    QStringLiteral("Test.hsp"))
                    .join(QLatin1Char('\n'))
                    .contains(QStringLiteral("expected object, got array")));

    QJsonObject wrongVersion = validProjectRoot();
    QJsonObject wrongVersionHeader = HarkenTest::projectHeader(wrongVersion);
    wrongVersionHeader.insert(QStringLiteral("schemaVersion"), 2);
    wrongVersion.insert(QStringLiteral("harkenDocument"), wrongVersionHeader);
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), wrongVersion,
                    QStringLiteral("Test.hsp"))
                    .join(QLatin1Char('\n'))
                    .contains(QStringLiteral("fixed value %1").arg(VNX::ProjectSchema::kProjectSchemaVersion)));

    QJsonObject wrongUuid = validProjectRoot();
    QJsonObject wrongUuidHeader = HarkenTest::projectHeader(wrongUuid);
    wrongUuidHeader.insert(QStringLiteral("projectId"), QStringLiteral("not-a-uuid"));
    wrongUuid.insert(QStringLiteral("harkenDocument"), wrongUuidHeader);
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), wrongUuid,
                    QStringLiteral("Test.hsp"))
                    .join(QLatin1Char('\n'))
                    .contains(QStringLiteral("valid UUID")));

    QJsonObject unknown = validProjectRoot();
    QJsonObject unknownContent = HarkenTest::projectContent(unknown);
    unknownContent.insert(QStringLiteral("futureField"), true);
    unknown.insert(QStringLiteral("content"), unknownContent);
    EXPECT_TRUE(VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), unknown,
                    QStringLiteral("Test.hsp"))
                    .join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Unknown field")));
}

TEST(ProjectSchemaValidator, RejectsInvalidNestedLayerData)
{
    const QString guid =
        QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QJsonObject content{
        {QStringLiteral("type"), QStringLiteral("Layer")},
        {QStringLiteral("guid"),
         guid},
        {QStringLiteral("name"), QStringLiteral("Layer")},
        {QStringLiteral("runtimeStableId"), 1},
        {QStringLiteral("parentRef"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("root")},
        }},
        {QStringLiteral("children"), QJsonArray{}},
        {QStringLiteral("order"), 0},
        {QStringLiteral("futureField"), true},
    };
    const QJsonObject layer = HarkenTest::projectDocument(
        QStringLiteral("project.layer"), guid, HarkenTest::kProjectId, content, VNX::ProjectSchema::kProjectSchemaVersion);
    const QString errors = VNX::ProjectSchemaValidator::validateProjectDocument(
                               VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"), layer,
                               QStringLiteral("Layers/Layer.vp"))
                               .join(QLatin1Char('\n'));
    EXPECT_TRUE(errors.contains(QStringLiteral("$.content.futureField")));
}

TEST(ProjectSchemaValidator, SoundLanguageDimensionIsOptional)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QString entryId = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const auto validate = [&](const QJsonObject& sound) {
        QJsonObject content = layerContent(
            QStringLiteral("Sound"), guid,
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}});
        content.insert(QStringLiteral("components"),
                       QJsonObject{{QStringLiteral("sound"), sound}});
        return VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"),
            HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                                       HarkenTest::kProjectId, content),
            QStringLiteral("Layers/Sound.vp"));
    };
    QJsonObject source{
        {QStringLiteral("entryId"), entryId},
        {QStringLiteral("sourceId"), 1},
    };
    QJsonObject set{{"entries", QJsonArray{source}}, {"active", entryId}, {"platforms", QJsonObject{}}};
    QJsonObject sources{{"plain", set}, {"localized", QJsonObject{}}};
    QJsonObject sound{{"sources", sources}};
    EXPECT_TRUE(validate(sound).isEmpty())
        << validate(sound).join(QLatin1Char('\n')).toStdString();
    set.remove("active");
    sources["plain"] = set;
    sound["sources"] = sources;
    EXPECT_FALSE(validate(sound).isEmpty());
    set["active"] = QJsonValue::Null;
    sources["plain"] = set;
    source["entryId"] = QStringLiteral("ffffffff-bbbb-cccc-dddd-eeeeeeeeeeee");
    set["entries"] = QJsonArray{source};
    sources["localized"] = QJsonObject{{"English", set}};
    sound["sources"] = sources;
    sound["distinguishLanguage"] = true;
    EXPECT_TRUE(validate(sound).isEmpty())
        << validate(sound).join(QLatin1Char('\n')).toStdString();

    sound.insert(QStringLiteral("distinguishLanguage"), QStringLiteral("true"));
    EXPECT_TRUE(validate(sound).join(QLatin1Char('\n')).contains(
        QStringLiteral("distinguishLanguage")));
}

TEST(ProjectSchemaValidator, ContainerRequiresSelectionsAndRejectsRetiredFields)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QJsonObject table{{"entries", QJsonArray{}}, {"occurrenceOrdinalHighWater", 0},
        {"ruleOrdinalHighWater", 0}, {"matchAxes", QJsonArray{}}, {"blendAxes", QJsonArray{}}};
    const QJsonObject selections{{"shared", table}, {"platforms", QJsonObject{}}};
    const QJsonObject selection{{"selections", selections}};
    auto content = layerContent(QStringLiteral("Container"), guid,
        {{QStringLiteral("kind"), QStringLiteral("root")}});
    content["components"] = QJsonObject{{"selection", selection}};
    const auto validate = [&](const QJsonObject& value) {
        return VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"),
            HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                HarkenTest::kProjectId, value), QStringLiteral("Layers/Container.vp"));
    };
    ASSERT_TRUE(validate(content).isEmpty());
    auto retired = content;
    retired["childOccurrences"] = table;
    EXPECT_TRUE(validate(retired).join(QLatin1Char('\n')).contains(QStringLiteral("childOccurrences")));
    auto invalidSelection = selection;
    invalidSelection["behavior"] = QJsonObject{{"childWeights", QJsonArray{}}};
    retired = content;
    retired["components"] = QJsonObject{{"selection", invalidSelection}};
    EXPECT_TRUE(validate(retired).join(QLatin1Char('\n')).contains(QStringLiteral("childWeights")));
    retired["components"] = QJsonObject{{"selection", QJsonObject{}}};
    EXPECT_TRUE(validate(retired).join(QLatin1Char('\n')).contains(QStringLiteral("selections")));
}

TEST(ProjectSchemaValidator, BlendCurveShapesAgreeWithComponentParsing)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QString childGuid = QStringLiteral("22222222-3333-4444-5555-666666666666");
    const QString variableGuid = QStringLiteral("33333333-4444-5555-6666-777777777777");
    for (bool hasVariable : {false, true}) {
        const QJsonValue variable = hasVariable ? QJsonValue(variableGuid) : QJsonValue(QJsonValue::Null);
        for (int pointCount = 0; pointCount <= 9; ++pointCount) {
            QJsonArray points;
            for (int index = 0; index < pointCount; ++index)
                points.append(QJsonArray{index, 0.5});
            for (int handleCount = 0; handleCount <= 8; ++handleCount) {
                SCOPED_TRACE(testing::Message() << "hasVariable=" << hasVariable
                    << " points=" << pointCount << " handles=" << handleCount);
                QJsonArray handles;
                for (int index = 0; index < handleCount; ++index)
                    handles.append(QJsonArray{index + 0.25, 0.5, index + 0.75, 0.5});
                const QJsonObject curve{{"order", 0}, {"variable", variable},
                    {"points", points}, {"handles", handles}};
                const QJsonObject entry{{"childGuid", childGuid}, {"stableMemberOrdinal", 0},
                    {"occurrences", QJsonArray{QJsonObject{
                        {"stableOccurrenceOrdinal", 0}, {"order", 0}, {"weight", 50}}}},
                    {"conditions", QJsonArray{}}, {"curves", QJsonArray{curve}}};
                const QJsonObject table{{"entries", QJsonArray{entry}}, {"occurrenceOrdinalHighWater", 1},
                    {"ruleOrdinalHighWater", 0}, {"matchAxes", QJsonArray{}}, {"blendAxes", QJsonArray{variable}}};
                const QJsonObject selection{{"mode", static_cast<int>(VNX::SelectionMode::Blend)},
                    {"selections", QJsonObject{{"shared", table}, {"platforms", QJsonObject{}}}}};
                auto content = layerContent(QStringLiteral("Container"), guid,
                    {{QStringLiteral("kind"), QStringLiteral("root")}});
                content["components"] = QJsonObject{{"selection", selection}};
                const auto errors = VNX::ProjectSchemaValidator::validateProjectDocument(
                    VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"),
                    HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                        HarkenTest::kProjectId, content), QStringLiteral("Layers/Container.vp"));
                const bool expected = (!hasVariable && pointCount == 0 && handleCount == 0)
                    || (pointCount >= 2 && pointCount <= 8 && handleCount == pointCount - 1);
                EXPECT_EQ(errors.isEmpty(), expected) << errors.join(QLatin1Char('\n')).toStdString();
                VNX::VSelection parsed;
                EXPECT_EQ(parsed.fromJson(selection), expected);
            }
        }
    }
}

TEST(ProjectSchemaValidator, BlendIdentityKeepsUnboundGroupsDistinctAndRejectsAmbiguousCurveReferences)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QString childGuid = QStringLiteral("22222222-3333-4444-5555-666666666666");
    const QString firstBlend = QStringLiteral("33333333-4444-5555-6666-777777777777");
    const QString secondBlend = QStringLiteral("44444444-5555-6666-7777-888888888888");
    const QJsonObject firstCurve{{"order", 0}, {"blendGuid", firstBlend},
        {"points", QJsonArray{}}, {"handles", QJsonArray{}}};
    auto secondCurve = firstCurve;
    secondCurve["blendGuid"] = secondBlend;
    const QJsonArray axes{
        QJsonObject{{"guid", firstBlend}, {"variable", QJsonValue::Null}},
        QJsonObject{{"guid", secondBlend}, {"variable", QJsonValue::Null}}};
    const auto makeSelection = [&](const QJsonArray& blendAxes, const QJsonArray& curves) {
        const QJsonObject entry{{"childGuid", childGuid}, {"stableMemberOrdinal", 0},
            {"occurrences", QJsonArray{QJsonObject{
                {"stableOccurrenceOrdinal", 0}, {"order", 0}, {"weight", 50}}}},
            {"conditions", QJsonArray{}}, {"curves", curves}};
        const QJsonObject table{{"entries", QJsonArray{entry}}, {"occurrenceOrdinalHighWater", 1},
            {"ruleOrdinalHighWater", 0}, {"matchAxes", QJsonArray{}}, {"blendAxes", blendAxes}};
        return QJsonObject{{"mode", static_cast<int>(VNX::SelectionMode::Blend)},
            {"selections", QJsonObject{{"shared", table}, {"platforms", QJsonObject{}}}}};
    };
    const auto validate = [&](const QJsonObject& selection) {
        auto content = layerContent(QStringLiteral("Container"), guid,
            {{QStringLiteral("kind"), QStringLiteral("root")}});
        content["components"] = QJsonObject{{"selection", selection}};
        return VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"),
            HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                HarkenTest::kProjectId, content), QStringLiteral("Layers/Container.vp"));
    };
    const auto selection = makeSelection(axes, {firstCurve, secondCurve});
    const auto errors = validate(selection);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    VNX::VSelection parsed;
    ASSERT_TRUE(parsed.fromJson(selection));
    EXPECT_EQ(parsed.selections.toJson(), selection.value(QStringLiteral("selections")).toObject());

    auto ambiguous = firstCurve;
    ambiguous["variable"] = QJsonValue::Null;
    EXPECT_FALSE(validate(makeSelection(axes, {ambiguous})).isEmpty());
    auto missingIdentity = firstCurve;
    missingIdentity.remove(QStringLiteral("blendGuid"));
    EXPECT_FALSE(validate(makeSelection(axes, {missingIdentity})).isEmpty());
    auto nullIdentity = firstCurve;
    nullIdentity["blendGuid"] = QJsonValue::Null;
    EXPECT_FALSE(validate(makeSelection(axes, {nullIdentity})).isEmpty());
    auto invalidAxis = axes.at(0).toObject();
    invalidAxis.remove(QStringLiteral("guid"));
    EXPECT_FALSE(validate(makeSelection({invalidAxis}, {firstCurve})).isEmpty());
}

TEST(ProjectSchemaValidator, ValidatesNullableBusReferences)
{
    const QString guid =
        QStringLiteral("11111111-2222-3333-4444-555555555555");
    QJsonObject content{
        {QStringLiteral("type"), QStringLiteral("Bus")},
        {QStringLiteral("guid"), guid},
        {QStringLiteral("name"), QStringLiteral("Bus")},
        {QStringLiteral("parentRef"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("root")},
        }},
        {QStringLiteral("componentData"), QJsonObject{}},
        {QStringLiteral("playableProperties"), QJsonObject{
            {QStringLiteral("presence"), QStringLiteral("0000000000000040")},
            {QStringLiteral("mainOutputBusRef"), QJsonValue::Null},
        }},
    };
    const auto validate = [&](const QJsonObject& value) {
        return VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.bus"),
            HarkenTest::projectDocument(QStringLiteral("project.bus"), guid,
                                       HarkenTest::kProjectId, value, VNX::ProjectSchema::kProjectSchemaVersion),
            QStringLiteral("Busses/Bus.vp"));
    };
    EXPECT_TRUE(validate(content).isEmpty());

    QJsonObject playable = content.value(QStringLiteral("playableProperties")).toObject();
    playable.insert(QStringLiteral("mainOutputBusRef"), QStringLiteral("missing"));
    content.insert(QStringLiteral("playableProperties"), playable);
    EXPECT_TRUE(validate(content).join(QLatin1Char('\n'))
                    .contains(QStringLiteral("expected object or null, got string")));
}

TEST(ProjectSchemaValidator, AcceptsRetiredSignalGraphInputButRejectsBindingPropertyAndPresence)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QJsonObject signalGraph{
        {QStringLiteral("presence"), QStringLiteral("0000000000008000")},
        {QStringLiteral("signalGraph"), QJsonArray{}},
    };
    for (int location = 0; location < 2; ++location) {
        SCOPED_TRACE(location);
        const auto validate = [&](const QJsonObject& properties) {
            QJsonObject content = layerContent(
                QStringLiteral("Container"), guid,
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}});
            if (location == 0) {
                content.insert(QStringLiteral("playableProperties"), properties);
            } else if (location == 1) {
                QJsonObject platform = properties;
                platform.insert(QStringLiteral("platformId"), HarkenTest::platformIdString(QStringLiteral("Windows")));
                content.insert(QStringLiteral("playableProperties"), QJsonObject{
                    {QStringLiteral("presence"), QStringLiteral("0000000000000000")},
                    {QStringLiteral("platformOverrides"), QJsonArray{platform}},
                });
            }
            return VNX::ProjectSchemaValidator::validateProjectDocument(
                VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"),
                HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                                           HarkenTest::kProjectId, content),
                QStringLiteral("Layers/Container.vp"));
        };
        const QStringList signalGraphErrors = validate(signalGraph);
        EXPECT_TRUE(signalGraphErrors.isEmpty())
            << signalGraphErrors.join(QLatin1Char('\n')).toStdString();
        QJsonObject retired = signalGraph;
        retired.insert(QStringLiteral("bindingAttachmentRef"), QStringLiteral("0"));
        EXPECT_TRUE(validate(retired).join(QLatin1Char('\n'))
                        .contains(QStringLiteral("bindingAttachmentRef")));
        for (const QString& presence : {QStringLiteral("0000000000004000"),
                                        QStringLiteral("000000000000c000")}) {
            SCOPED_TRACE(presence.toStdString());
            QJsonObject invalidPresence = signalGraph;
            invalidPresence.insert(QStringLiteral("presence"), presence);
            EXPECT_TRUE(validate(invalidPresence).join(QLatin1Char('\n'))
                            .contains(QStringLiteral("presence")));
        }
    }
}

TEST(ProjectSchemaValidator, PackSidecarAcceptsCurrentAndRejectsUnlistedVersions)
{
    constexpr int current = VNX::PackSchema::kPackManifestSchemaVersion;
    const QJsonObject action{{"name", "Play_Sound"},
                             {"guid", "11111111-2222-3333-4444-555555555555"}};
    const QJsonObject init{{"variables", QJsonArray{}}};
    for (const QJsonObject& companion : {action, init}) {
        EXPECT_TRUE(VNX::ProjectSchemaValidator::validatePackManifest(
                        current, companion, QStringLiteral("Pack companion")).isEmpty());
        for (const int version : {0, current + 1, current + 2}) {
            EXPECT_FALSE(VNX::ProjectSchemaValidator::validatePackManifest(
                             version, companion, QStringLiteral("Pack companion")).isEmpty());
        }
    }
}

TEST(ProjectSchemaValidator, PackSidecarRequiresExactlyOneCurrentDocumentShape)
{
    const auto validate = [](const QJsonObject& value) {
        return VNX::ProjectSchemaValidator::validatePackManifest(
            VNX::PackSchema::kPackManifestSchemaVersion, value, QStringLiteral("Pack sidecar"));
    };
    QJsonObject action{{"name", "Play_Sound"},
                       {"guid", "11111111-2222-3333-4444-555555555555"}};
    EXPECT_TRUE(validate(action).isEmpty());
    EXPECT_FALSE(validate(QJsonObject{}).isEmpty());
    action.insert("schema_version", VNX::PackSchema::kPackManifestSchemaVersion);
    EXPECT_FALSE(validate(action).isEmpty());
    action.remove("schema_version");
    action.insert("variables", QJsonArray{});
    EXPECT_FALSE(validate(action).isEmpty());
    action.remove("variables");
    action.insert("guid", "00000000-0000-0000-0000-000000000000");
    EXPECT_FALSE(validate(action).isEmpty());

    QJsonObject label{{"name", "Stone"},
                      {"guid", "22222222-3333-4444-5555-666666666666"}};
    QJsonObject variable{{"name", "Surface"},
                         {"guid", "33333333-4444-5555-6666-777777777777"},
                         {"labels", QJsonArray{label}}};
    EXPECT_TRUE(validate(QJsonObject{{"variables", QJsonArray{variable}}}).isEmpty());
    variable.insert("unit", "meters");
    EXPECT_FALSE(validate(QJsonObject{{"variables", QJsonArray{variable}}}).isEmpty());
    variable.remove("unit");
    label.insert("guid", "00000000-0000-0000-0000-000000000000");
    variable.insert("labels", QJsonArray{label});
    EXPECT_FALSE(validate(QJsonObject{{"variables", QJsonArray{variable}}}).isEmpty());
}

TEST(ProjectSchemaValidator, CurrentVersionPreservesMetadataBoundariesAcrossDocumentTypes)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QJsonObject rootRef{{QStringLiteral("kind"), QStringLiteral("root")}};
    std::vector<std::pair<QString, QJsonObject>> documents{
        {QStringLiteral("project.root"), HarkenTest::projectContent(validProjectRoot())},
        {QStringLiteral("project.layer"), layerContent(QStringLiteral("Layer"), guid, rootRef)},
    };
    for (const QString& type : {QStringLiteral("Action"), QStringLiteral("Bus"),
                                QStringLiteral("Variable"), QStringLiteral("Attenuation")}) {
        QJsonObject content{
            {QStringLiteral("guid"), guid},
            {QStringLiteral("name"), type},
            {QStringLiteral("type"), type},
            {QStringLiteral("parentRef"), rootRef},
        };
        if (type == QLatin1String("Action"))
            content.insert(QStringLiteral("actions"), QJsonArray{});
        else if (type == QLatin1String("Bus"))
            content.insert(QStringLiteral("componentData"), QJsonObject{});
        else
            content.insert(QStringLiteral("components"), QJsonObject{});
        documents.emplace_back(QStringLiteral("project.") + type.toLower(), content);
    }
    const QJsonObject metadata{
        {QStringLiteral("futureAnnotation"), QJsonObject{
            {QStringLiteral("labels"), QJsonArray{QStringLiteral("tag"), 7, QJsonValue::Null}},
        }},
    };
    for (const auto& [documentType, originalContent] : documents) {
        QJsonObject content = originalContent;
        content.insert(QStringLiteral("extensions"), metadata);
        QJsonObject document = HarkenTest::projectDocument(
            documentType, guid, HarkenTest::kProjectId, content, VNX::ProjectSchema::kProjectSchemaVersion);
        QJsonObject header = HarkenTest::projectHeader(document);
        header.insert(QStringLiteral("savedStudioVersion"), QStringLiteral("1.1.0"));
        header.insert(QStringLiteral("futurePublisherMetadata"), metadata);
        document.insert(QStringLiteral("harkenDocument"), header);
        const auto validate = [&]() {
            return VNX::ProjectSchemaValidator::validateProjectDocument(
                VNX::ProjectSchema::kProjectSchemaVersion, documentType, document, QStringLiteral("test.vp"));
        };
        EXPECT_TRUE(validate().isEmpty())
            << documentType.toStdString() << ": " << validate().join(QLatin1Char('\n')).toStdString();

        content.insert(QStringLiteral("futureBehavior"), true);
        document.insert(QStringLiteral("content"), content);
        EXPECT_TRUE(validate().join(QLatin1Char('\n')).contains(QStringLiteral("$.content.futureBehavior")));
        content.remove(QStringLiteral("futureBehavior"));
        content.insert(QStringLiteral("extensions"), QJsonArray{});
        document.insert(QStringLiteral("content"), content);
        EXPECT_TRUE(validate().join(QLatin1Char('\n')).contains(QStringLiteral("$.content.extensions")));
    }
}

TEST(ProjectSchemaValidator, CurrentVersionRequiresSavedStudioVersion)
{
    QJsonObject document = HarkenTest::projectRoot(HarkenTest::projectContent(validProjectRoot()), VNX::ProjectSchema::kProjectSchemaVersion);
    QJsonObject header = HarkenTest::projectHeader(document);
    const auto validate = [&]() {
        document.insert(QStringLiteral("harkenDocument"), header);
        return VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.root"), document, QStringLiteral("Test.hsp"));
    };
    header.insert(QStringLiteral("savedStudioVersion"), QStringLiteral("1.1.0"));
    EXPECT_TRUE(validate().isEmpty());
    for (const QJsonValue& invalid : {QJsonValue(QString()), QJsonValue(2), QJsonValue(QJsonArray{})}) {
        header.insert(QStringLiteral("savedStudioVersion"), invalid);
        EXPECT_TRUE(validate().join(QLatin1Char('\n')).contains(QStringLiteral("$.harkenDocument.savedStudioVersion")));
    }
    header.remove(QStringLiteral("savedStudioVersion"));
    EXPECT_TRUE(validate().join(QLatin1Char('\n')).contains(QStringLiteral("$.harkenDocument.savedStudioVersion")));
}

TEST(ProjectSchemaValidator, CurrentVersionRejectsUnknownComponentsAndEnums)
{
    const QString guid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    QJsonObject content = layerContent(
        QStringLiteral("Layer"), guid,
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}});
    const auto validate = [&]() {
        return VNX::ProjectSchemaValidator::validateProjectDocument(
            VNX::ProjectSchema::kProjectSchemaVersion, QStringLiteral("project.layer"),
            HarkenTest::projectDocument(QStringLiteral("project.layer"), guid,
                                       HarkenTest::kProjectId, content, VNX::ProjectSchema::kProjectSchemaVersion),
            QStringLiteral("Layers/Layer.vp"));
    };
    ASSERT_TRUE(validate().isEmpty());
    content.insert(QStringLiteral("components"), QJsonObject{
        {QStringLiteral("futureComponent"), QJsonObject{}},
    });
    EXPECT_TRUE(validate().join(QLatin1Char('\n')).contains(QStringLiteral("$.content.components.futureComponent")));
    content.remove(QStringLiteral("components"));
    content.insert(QStringLiteral("type"), QStringLiteral("FutureLayer"));
    EXPECT_TRUE(validate().join(QLatin1Char('\n')).contains(QStringLiteral("an allowed value")));
}

TEST(ProjectSchemaRules, GeneratedDispatchRunsEveryCurrentRuleInSchemaOrder)
{
    RecordingRuleExecutor executor;
    QStringList errors;
    EXPECT_TRUE(VNX::GeneratedSchemaRegistry::validateProjectRules(
        VNX::ProjectSchema::kProjectSchemaVersion, executor, errors));
    EXPECT_TRUE(errors.isEmpty());
    EXPECT_EQ(executor.calls,
              QStringList({QStringLiteral("acyclic"),
                           QStringLiteral("documentIdMirrorsRoot"),
                           QStringLiteral("projectIdUniform"),
                           QStringLiteral("referencesResolve"),
                           QStringLiteral("schemaVersionUniform"),
                           QStringLiteral("singleton"),
                           QStringLiteral("uniqueAcrossProject")}));
    EXPECT_EQ(executor.uniqueFields,
              QStringList({QStringLiteral("guid"),
                           QStringLiteral("documentId")}));
    EXPECT_EQ(executor.mirrorDocumentTypes.size(), 5);
    EXPECT_EQ(executor.references.size(), 14);
    EXPECT_EQ(executor.references.first().kind,
              VNX::ProjectSchemaReferenceKind::Parent);
    EXPECT_EQ(executor.references.first().path,
              QStringList({QStringLiteral("parentRef")}));
    EXPECT_EQ(executor.acyclicPath,
              QStringList({QStringLiteral("parentRef")}));
    EXPECT_EQ(executor.singletonDocumentType, QStringLiteral("project.bus"));
    EXPECT_EQ(executor.singletonPath,
              QStringList({QStringLiteral("parentRef"),
                           QStringLiteral("kind")}));
    EXPECT_EQ(executor.singletonValue, QStringLiteral("root"));
    EXPECT_EQ(executor.singletonMessage,
              QStringLiteral("Project must contain exactly one root Bus"));
}

TEST(ProjectContract, RejectsBrokenGuidReferencesAcrossAllPlayableLocations)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectId =
        QStringLiteral("aaaaaaaa-1111-2222-3333-444444444444");
    const QString busId =
        QStringLiteral("bbbbbbbb-1111-2222-3333-444444444444");
    const QString attenuationId =
        QStringLiteral("cccccccc-1111-2222-3333-444444444444");
    const QString containerId =
        QStringLiteral("dddddddd-1111-2222-3333-444444444444");
    const QString soundId =
        QStringLiteral("eeeeeeee-1111-2222-3333-444444444444");
    const QString trackId =
        QStringLiteral("ffffffff-1111-2222-3333-444444444444");
    const QString clipId =
        QStringLiteral("99999999-1111-2222-3333-444444444444");
    const QString missingId =
        QStringLiteral("77777777-1111-2222-3333-444444444444");
    const QJsonObject rootRef{
        {QStringLiteral("kind"), QStringLiteral("root")},
    };

    const QJsonObject root = HarkenTest::projectRoot(QJsonObject{
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Test")},
        }},
        {QStringLiteral("paths"), QJsonObject{
            {QStringLiteral("audioFolder"), QStringLiteral("AudioSources")},
        }},
    }, VNX::ProjectSchema::kProjectSchemaVersion, projectId);

    QJsonObject bus{
        {QStringLiteral("type"), QStringLiteral("Bus")},
        {QStringLiteral("guid"), busId},
        {QStringLiteral("name"), QStringLiteral("Main")},
        {QStringLiteral("parentRef"), rootRef},
        {QStringLiteral("componentData"), QJsonObject{}},
    };
    const QJsonObject attenuation{
        {QStringLiteral("type"), QStringLiteral("Attenuation")},
        {QStringLiteral("guid"), attenuationId},
        {QStringLiteral("name"), QStringLiteral("Near")},
        {QStringLiteral("parentRef"), rootRef},
        {QStringLiteral("components"), QJsonObject{}},
    };
    QJsonObject container = layerContent(
        QStringLiteral("Container"), containerId, rootRef);
    const QJsonObject playable{
        {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
        {QStringLiteral("volume"), -3.0},
    };
    QJsonObject selectionTable{{"occurrenceOrdinalHighWater", 1},
        {"ruleOrdinalHighWater", 0}, {"matchAxes", QJsonArray{}}, {"blendAxes", QJsonArray{}},
        {"entries", QJsonArray{QJsonObject{{"childGuid", soundId}, {"stableMemberOrdinal", 0},
            {"occurrences", QJsonArray{QJsonObject{{"stableOccurrenceOrdinal", 0}, {"order", 0}, {"weight", 50}}}},
            {"conditions", QJsonArray{}}, {"curves", QJsonArray{}}}}}};
    const auto writeSelections = [&] {
        container["components"] = QJsonObject{{"selection", QJsonObject{
            {"mode", 0}, {"selections", QJsonObject{
                {"shared", selectionTable}, {"platforms", QJsonObject{}}}}}}};
    };
    writeSelections();
    QJsonObject sound = layerContent(
        QStringLiteral("Sound"), soundId,
        QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("item")},
            {QStringLiteral("id"), containerId},
            {QStringLiteral("documentId"), containerId},
        });
    QJsonObject track = layerContent(QStringLiteral("Track"), trackId, rootRef);
    track.insert(QStringLiteral("components"), QJsonObject{
        {QStringLiteral("tracktimeline"), QJsonObject{
            {QStringLiteral("clips"), QJsonArray{QJsonObject{
                {QStringLiteral("clipGuid"), clipId},
                {QStringLiteral("targetGuid"), soundId},
                {QStringLiteral("start"), 0},
                {QStringLiteral("duration"), 1},
                {QStringLiteral("stableOccurrenceOrdinal"), 0},
                {QStringLiteral("playableProperties"), playable},
            }}},
            {QStringLiteral("occurrenceOrdinalHighWater"), 1},
        }},
    });

    const auto writeDocument = [&](const QString& relativePath,
                                   const QString& type,
                                   const QString& id,
                                   const QJsonObject& content) {
        writeJson(QDir(directory.path()).filePath(relativePath),
                  HarkenTest::projectDocument(type, id, projectId, content));
    };
    const auto writeAll = [&] {
        writeDocument(QStringLiteral("Busses/Main.vp"),
                      QStringLiteral("project.bus"), busId, bus);
        writeDocument(QStringLiteral("Attenuations/Near.vp"),
                      QStringLiteral("project.attenuation"), attenuationId,
                      attenuation);
        writeDocument(QStringLiteral("Layers/Container.vp"),
                      QStringLiteral("project.layer"), containerId, container);
        writeDocument(QStringLiteral("Layers/Sound.vp"),
                      QStringLiteral("project.layer"), soundId, sound);
        writeDocument(QStringLiteral("Layers/Track.vp"),
                      QStringLiteral("project.layer"), trackId, track);
    };
    const auto validateWritten = [&] {
        return VNX::ProjectContractRegistry::instance().validateTree(
            directory.path(), root, true);
    };
    const auto validate = [&] {
        writeAll();
        return validateWritten();
    };

    ASSERT_TRUE(validate().isEmpty()) << validateWritten().join('\n').toStdString();

    QFile::remove(QDir(directory.path()).filePath(QStringLiteral("Busses/Main.vp")));
    EXPECT_TRUE(validateWritten().join(QLatin1Char('\n'))
                    .contains(QStringLiteral(
                        "Project must contain exactly one root Bus")));
    writeAll();

    QJsonObject secondBus = bus;
    const QString secondBusId =
        QStringLiteral("bbbbbbbb-1111-2222-3333-555555555555");
    secondBus.insert(QStringLiteral("guid"), secondBusId);
    writeDocument(QStringLiteral("Busses/Second.vp"),
                  QStringLiteral("project.bus"), secondBusId, secondBus);
    const QString singletonErrors = validate().join(QLatin1Char('\n'));
    EXPECT_TRUE(singletonErrors.contains(QStringLiteral(
        "Project must contain exactly one root Bus")));
    QFile::remove(QDir(directory.path()).filePath(QStringLiteral("Busses/Second.vp")));

    bus.insert(QStringLiteral("parentRef"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("bus")},
        {QStringLiteral("id"), busId},
        {QStringLiteral("documentId"), busId},
    });
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Bus parent references contain a cycle")));
    bus.insert(QStringLiteral("parentRef"), rootRef);

    bus.insert(QStringLiteral("guid"), secondBusId);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Document identity does not match its content")));
    bus.insert(QStringLiteral("guid"), busId);

    QJsonObject wrongProjectDocument = HarkenTest::projectDocument(
        QStringLiteral("project.bus"), busId,
        QStringLiteral("aaaaaaaa-1111-2222-3333-666666666666"), bus);
    writeJson(QDir(directory.path()).filePath(QStringLiteral("Busses/Main.vp")),
              wrongProjectDocument);
    EXPECT_TRUE(validateWritten().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Project identity does not match")));
    writeAll();

    QJsonObject wrongVersionDocument = HarkenTest::projectDocument(
        QStringLiteral("project.bus"), busId, projectId, bus);
    auto wrongVersionHeader = HarkenTest::projectHeader(wrongVersionDocument);
    wrongVersionHeader.insert(QStringLiteral("schemaVersion"), VNX::ProjectSchema::kProjectSchemaVersion + 1);
    wrongVersionDocument.insert(QStringLiteral("harkenDocument"), wrongVersionHeader);
    writeJson(QDir(directory.path()).filePath(QStringLiteral("Busses/Main.vp")),
              wrongVersionDocument);
    EXPECT_TRUE(validateWritten().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Project Schema version does not match")));
    writeAll();

    const auto soundParent = sound.value(QStringLiteral("parentRef")).toObject();
    QJsonObject noDocumentRef = soundParent;
    noDocumentRef.remove(QStringLiteral("documentId"));
    sound.insert(QStringLiteral("parentRef"), noDocumentRef);
    EXPECT_TRUE(validate().isEmpty());

    QJsonObject badDocument = soundParent;
    badDocument.insert(QStringLiteral("documentId"), attenuationId);
    sound.insert(QStringLiteral("parentRef"), badDocument);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Reference document does not match")));

    auto invalidParent = soundParent;
    invalidParent.insert(QStringLiteral("id"), missingId);
    invalidParent.insert(QStringLiteral("documentId"), missingId);
    sound.insert(QStringLiteral("parentRef"), invalidParent);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Reference target is missing")));

    invalidParent.insert(QStringLiteral("id"), attenuationId);
    invalidParent.insert(QStringLiteral("documentId"), attenuationId);
    sound.insert(QStringLiteral("parentRef"), invalidParent);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Reference target has the wrong type")));

    sound.insert(QStringLiteral("parentRef"), soundParent);
    QJsonArray rows = selectionTable.value(QStringLiteral("entries")).toArray();
    QJsonObject row = rows.first().toObject();
    row.insert(QStringLiteral("childGuid"), missingId);
    rows.replace(0, row);
    selectionTable.insert(QStringLiteral("entries"), rows);
    writeSelections();
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("selections.shared.entries[0].childGuid")));

    row.insert(QStringLiteral("childGuid"), soundId);
    rows.replace(0, row);
    selectionTable.insert(QStringLiteral("entries"), rows);
    writeSelections();
    QJsonObject components = track.value(QStringLiteral("components")).toObject();
    QJsonObject timeline = components.value(QStringLiteral("tracktimeline")).toObject();
    QJsonArray clips = timeline.value(QStringLiteral("clips")).toArray();
    QJsonObject clip = clips.first().toObject();
    QJsonObject clipPlayable = clip.value(QStringLiteral("playableProperties")).toObject();
    clipPlayable.insert(QStringLiteral("mainOutputBusRef"), objectReference(missingId));
    clip.insert(QStringLiteral("playableProperties"), clipPlayable);
    clips.replace(0, clip);
    timeline.insert(QStringLiteral("clips"), clips);
    components.insert(QStringLiteral("tracktimeline"), timeline);
    track.insert(QStringLiteral("components"), components);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("tracktimeline.clips[0].playableProperties")));

    clip.insert(QStringLiteral("playableProperties"), playable);
    clip.insert(QStringLiteral("targetGuid"), busId);
    clips.replace(0, clip);
    timeline.insert(QStringLiteral("clips"), clips);
    components.insert(QStringLiteral("tracktimeline"), timeline);
    track.insert(QStringLiteral("components"), components);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("tracktimeline.clips[0].targetGuid")));

    clip.insert(QStringLiteral("targetGuid"), soundId);
    clips.replace(0, clip);
    timeline.insert(QStringLiteral("clips"), clips);
    components.insert(QStringLiteral("tracktimeline"), timeline);
    track.insert(QStringLiteral("components"), components);
    bus.insert(QStringLiteral("parentRef"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("root")},
        {QStringLiteral("id"), containerId},
    });
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Root parent reference contains a target")));

    bus.insert(QStringLiteral("parentRef"), rootRef);
    track.insert(QStringLiteral("guid"), soundId);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Duplicate guid")));
    track.insert(QStringLiteral("guid"), trackId);

    container.insert(QStringLiteral("runtimeStableId"), 7);
    track.insert(QStringLiteral("runtimeStableId"), 7);
    EXPECT_TRUE(validate().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Unknown field")));
    container.remove(QStringLiteral("runtimeStableId"));
    track.remove(QStringLiteral("runtimeStableId"));

    writeAll();
    writeDocument(QStringLiteral("Layers/Sound.vp"),
                  QStringLiteral("project.layer"), containerId, sound);
    EXPECT_TRUE(validateWritten().join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Duplicate document identity")));
}

}
