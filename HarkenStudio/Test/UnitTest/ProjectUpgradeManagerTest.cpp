#include "Core/ProjectUpgradeManager.h"
#include "Core/FileWatcher.h"
#include "Core/ProjectTransaction.h"
#include "Core/ProjectSaveSnapshot.h"
#include "Core/FieldGroupJson.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/SelectionData.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QCryptographicHash>
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

#include <limits>
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

TEST(ProjectSaveSnapshot, DocumentBytesMatchTextFilesAndRejectDirectoryCollisions)
{
    QTemporaryDir directory;
    QTemporaryDir preparedDirectory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(preparedDirectory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkpath(QStringLiteral("Layers")));
    ASSERT_TRUE(QDir(preparedDirectory.path()).mkpath(QStringLiteral("Layers/Occupied.vp")));
    writeJson(preparedDirectory.filePath(QStringLiteral("Layers/Existing.vp")),
              QJsonObject{{QStringLiteral("preserved"), true}});
    VNX::ProjectSaveSnapshot prepared(preparedDirectory.path());
    prepared.prepareDirectoryLayout();
    const QJsonObject document{
        {QStringLiteral("name"), QString::fromUtf8("\xc3\xa9\xe6\x96\x87")},
        {QStringLiteral("text"), QStringLiteral("first\r\nsecond\nthird")},
        {QStringLiteral("values"), QJsonArray{
            true, 1.5, QJsonValue::Null, -0.0,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            (std::numeric_limits<double>::min)(),
            (std::numeric_limits<double>::max)(),
            std::numeric_limits<double>::lowest(),
            std::numeric_limits<double>::denorm_min(),
            QJsonValue((std::numeric_limits<qint64>::min)()),
            QJsonValue((std::numeric_limits<qint64>::max)()),
            QJsonArray{QJsonObject{{QStringLiteral("nested"), QJsonArray{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(), -0.0,
            }}}},
        }},
    };
    const auto verifyDocument = [&](VNX::ProjectSaveSnapshot& target,
                                    const QString& relative, const QJsonObject& input) {
        SCOPED_TRACE(relative.toStdString());
        QFile file(directory.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray json = QJsonDocument(input).toJson(QJsonDocument::Indented);
        ASSERT_EQ(file.write(json), json.size());
        file.close();
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray disk = file.readAll();
        file.close();
        ASSERT_TRUE(target.addDocument(file.fileName(), input));
        EXPECT_EQ(target.files().value(relative), disk);
        EXPECT_EQ(target.documents().value(relative), QJsonDocument::fromJson(disk).object());
        EXPECT_EQ(prepared.prepareDocument(preparedDirectory.filePath(relative), input),
                  VNX::ProjectSaveSnapshot::WriteResult::Stored);
        EXPECT_EQ(prepared.files().value(relative), disk);
        EXPECT_EQ(prepared.documents().value(relative), QJsonDocument::fromJson(disk).object());
    };
    VNX::ProjectSaveSnapshot snapshot(directory.path());
    verifyDocument(snapshot, QStringLiteral("Layers/Main.vp"), document);
    VNX::ProjectSaveSnapshot previous(directory.path(), &snapshot);
    verifyDocument(previous, QStringLiteral("Layers/Main.vp"), document);
    previous.clearPrevious();
    QJsonObject input = QJsonDocument::fromJson(QJsonDocument(document).toJson()).object();
    const QJsonArray numbers{
        QJsonValue(qint64(10000000000000001LL)), QJsonValue(qint64(10000000000000001LL)),
        10000000000000000.0, QJsonValue(qint64(9007199254740993LL)), 9007199254740992.0,
        QJsonValue((std::numeric_limits<qint64>::max)()),
        static_cast<double>((std::numeric_limits<qint64>::max)()),
        -0.0, 0.0, std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        QJsonValue::Null,
    };
    for (const QJsonValue& number : numbers) {
        input.insert(QStringLiteral("nested"), QJsonObject{
            {QStringLiteral("number"), number},
            {QStringLiteral("array"), QJsonArray{QJsonObject{{QStringLiteral("value"), number}}}},
        });
        VNX::ProjectSaveSnapshot current(directory.path(), &previous);
        verifyDocument(current, QStringLiteral("Layers/Main.vp"), input);
        verifyDocument(current, QStringLiteral("Layers/Renamed.vp"), input);
        current.clearPrevious();
        previous = std::move(current);
    }
    const QJsonObject changes{
        {QStringLiteral("name"), QStringLiteral("Revised")},
        {QStringLiteral("harkenDocument"), QJsonObject{{QStringLiteral("id"), QStringLiteral("new-id")}}},
        {QStringLiteral("reference"), QJsonObject{{QStringLiteral("id"), QStringLiteral("new-target")}}},
    };
    for (auto change = changes.constBegin(); change != changes.constEnd(); ++change) {
        input.insert(change.key(), change.value());
        VNX::ProjectSaveSnapshot current(directory.path(), &previous);
        verifyDocument(current, QStringLiteral("Layers/Main.vp"), input);
        current.clearPrevious();
        previous = std::move(current);
    }
    ASSERT_TRUE(QDir(directory.path()).mkpath(QStringLiteral("Layers/Occupied.vp")));
    EXPECT_FALSE(snapshot.addDocument(
        directory.filePath(QStringLiteral("Layers/Occupied.vp")), document));
    EXPECT_TRUE(QFileInfo(directory.filePath(QStringLiteral("Layers/Occupied.vp"))).isDir());
    EXPECT_FALSE(snapshot.documents().contains(QStringLiteral("Layers/Occupied.vp")));
    EXPECT_EQ(prepared.prepareDocument(
        preparedDirectory.filePath(QStringLiteral("Layers/Occupied.vp")), document),
        VNX::ProjectSaveSnapshot::WriteResult::Physical);
    EXPECT_EQ(prepared.prepareDocument(
        preparedDirectory.filePath(QStringLiteral("Layers/Existing.vp")), document),
        VNX::ProjectSaveSnapshot::WriteResult::Physical);
    EXPECT_EQ(prepared.prepareDocument(
        preparedDirectory.filePath(QStringLiteral("Missing/New.vp")), document),
        VNX::ProjectSaveSnapshot::WriteResult::Failed);

    const QString generatedPath = QStringLiteral("Layers/Generated.vp");
    const QString generatedFile = directory.filePath(generatedPath);
    QString type = QStringLiteral("project.layer");
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString projectId = HarkenTest::kProjectId;
    QJsonObject header{{QStringLiteral("external"), QJsonObject{{QStringLiteral("revision"), 1}}}};
    QJsonObject content = HarkenTest::projectDocument(type, id, projectId, {
        {QStringLiteral("type"), QStringLiteral("Layer")},
        {QStringLiteral("name"), QStringLiteral("Generated")},
        {QStringLiteral("guid"), id},
        {QStringLiteral("children"), QJsonArray{}},
        {QStringLiteral("extensions"), document},
    }).value(QStringLiteral("content")).toObject();
    const auto verifyCreated = [&](VNX::ProjectSaveSnapshot& target,
                                   const QJsonObject& authored, bool valid) {
        const auto expected = VNX::ProjectDocument::create(type, id, projectId, authored, header);
        const auto actual = target.createDocument(generatedFile, type, id, projectId, authored, header);
        EXPECT_EQ(actual.isEmpty(), !valid);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(QJsonDocument(actual).toJson(QJsonDocument::Indented),
                  QJsonDocument(expected).toJson(QJsonDocument::Indented));
        if (valid) verifyDocument(target, generatedPath, actual);
    };
    VNX::ProjectSaveSnapshot generated(directory.path());
    verifyCreated(generated, content, true);
    for (const QJsonValue& number : numbers) {
        content.insert(QStringLiteral("extensions"), QJsonObject{{QStringLiteral("value"), number}});
        for (int repeat = 0; repeat < 2; ++repeat) {
            VNX::ProjectSaveSnapshot current(directory.path(), &generated);
            verifyCreated(current, content, true);
            current.clearPrevious();
            generated = std::move(current);
        }
    }
    const auto verifyGeneration = [&] {
        VNX::ProjectSaveSnapshot current(directory.path(), &generated);
        verifyCreated(current, content, true);
        current.clearPrevious();
        generated = std::move(current);
    };
    header.insert(QStringLiteral("external"), QJsonObject{{QStringLiteral("revision"), 2}});
    verifyGeneration();
    header.remove(QStringLiteral("external"));
    verifyGeneration();
    id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    verifyGeneration();
    projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    verifyGeneration();
    type = QStringLiteral("project.bus");
    verifyGeneration();

    for (const auto& changes : QList<QJsonObject>{
             {{QStringLiteral("fieldGroups"), QJsonObject{}}},
             {{QStringLiteral("fieldGroups"), false}},
             {{QStringLiteral("sectionGroups"), QJsonObject{}}},
             {{QStringLiteral("outputBusOverride"), true}},
             {{QStringLiteral("childOccurrences"), QJsonArray{}}},
             {{QStringLiteral("children"), QJsonArray{1}}},
             {{QStringLiteral("type"), QStringLiteral("Action")}},
             {{QStringLiteral("components"), QJsonObject{
                 {QStringLiteral("mixnode"), QJsonObject{{QStringLiteral("outputBusId"), 1}}}}}},
         }) {
        QJsonObject invalid = content;
        for (auto change = changes.constBegin(); change != changes.constEnd(); ++change)
            invalid.insert(change.key(), change.value());
        VNX::ProjectSaveSnapshot current(directory.path(), &generated);
        verifyCreated(current, invalid, false);
        verifyCreated(current, content, true);

        VNX::ProjectSaveSnapshot unvalidated(directory.path());
        ASSERT_TRUE(unvalidated.addDocument(generatedFile, {
            {QStringLiteral("harkenDocument"), VNX::ProjectDocument::createHeader(type, id, projectId, header)},
            {QStringLiteral("content"), invalid},
        }));
        VNX::ProjectSaveSnapshot replay(directory.path(), &unvalidated);
        verifyCreated(replay, invalid, false);
    }
}

TEST(ProjectContract, SnapshotDiskAndIncrementalValidationRemainEquivalentAcrossChanges)
{
    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = createBaselineProject(staging);
    QJsonObject root = readJson(projectPath);
    const QJsonObject originalRoot = root;
    const QString mainPath = QStringLiteral("Layers/Main.vp");
    const QString busPath = QStringLiteral("Busses/Root.vp");
    const QJsonObject main = readJson(staging.filePath(mainPath));
    QHash<QString, QJsonObject> documents{
        {mainPath, main}, {busPath, readJson(staging.filePath(busPath))},
    };
    ASSERT_TRUE(QFile::remove(staging.filePath(mainPath)));
    ASSERT_TRUE(QFile::remove(staging.filePath(busPath)));
    VNX::ProjectContractReadContext context;
    VNX::ProjectContractReadContext incrementalContext;
    VNX::ProjectContractValidationBaseline baseline;
    const auto compare = [&](bool valid) {
        QTemporaryDir disk;
        EXPECT_TRUE(disk.isValid());
        writeJson(disk.filePath(QStringLiteral("Baseline.hsp")), root);
        for (auto it = documents.cbegin(); it != documents.cend(); ++it)
            writeJson(disk.filePath(it.key()), it.value());
        auto expected = VNX::ProjectContractRegistry::instance().validateTree(
            disk.path(), root, true);
        auto actual = VNX::ProjectContractRegistry::instance().validateSnapshot(
            staging.path(), root, true, documents, &context,
            baseline.isValid() ? nullptr : &baseline);
        auto fresh = VNX::ProjectContractRegistry::instance().validateSnapshot(
            staging.path(), root, true, documents);
        expected.sort();
        actual.sort();
        fresh.sort();
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(actual, fresh);
        EXPECT_EQ(actual.isEmpty(), valid) << actual.join(QLatin1Char('\n')).toStdString();
        if (!baseline.isValid()) return;
        QHash<QString, QJsonObject> changed;
        QSet<QString> removed;
        for (auto document = documents.cbegin(); document != documents.cend(); ++document)
            if (!baseline.documents().contains(document.key())
                || baseline.documents().value(document.key()) != document.value())
                changed.insert(document.key(), document.value());
        for (auto document = baseline.documents().cbegin(); document != baseline.documents().cend(); ++document)
            if (!documents.contains(document.key())) removed.insert(document.key());
        const auto previousRoot = baseline.projectRoot();
        const auto previousDocuments = baseline.documents();
        VNX::ProjectContractValidationBaseline candidate;
        auto incremental = VNX::ProjectContractRegistry::instance().validateIncrementalSnapshot(
            staging.path(), root, true, changed, removed, baseline, &candidate, &incrementalContext);
        incremental.sort();
        EXPECT_EQ(incremental, expected);
        EXPECT_EQ(candidate.isValid(), valid);
        EXPECT_EQ(baseline.projectRoot(), previousRoot);
        EXPECT_EQ(baseline.documents(), previousDocuments);
        if (candidate.isValid()) baseline = std::move(candidate);
    };
    compare(true);

    QJsonObject changed = main;
    QJsonObject content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("expanded"), true);
    changed.insert(QStringLiteral("content"), content);
    documents.insert(mainPath, changed);
    compare(true);

    content.insert(QStringLiteral("order"), false);
    changed.insert(QStringLiteral("content"), content);
    documents.insert(mainPath, changed);
    compare(false);

    changed = main;
    content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("components"), QJsonObject{
        {QStringLiteral("unknownComponent"), QJsonObject{}}});
    changed.insert(QStringLiteral("content"), content);
    documents.insert(mainPath, changed);
    compare(false);

    changed = main;
    content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("name"), false);
    changed.insert(QStringLiteral("content"), content);
    documents.insert(mainPath, changed);
    compare(false);

    changed = main;
    QJsonObject header = HarkenTest::projectHeader(changed);
    header.insert(QStringLiteral("type"), QStringLiteral("project.bus"));
    changed.insert(QStringLiteral("harkenDocument"), header);
    documents.insert(mainPath, changed);
    compare(false);
    documents.insert(mainPath, main);
    compare(true);

    for (const auto* key : {"savedStudioVersion", "packSchemaVersion"}) {
        changed = main;
        header = HarkenTest::projectHeader(changed);
        header.insert(QLatin1String(key), HarkenTest::projectHeader(root).value(QLatin1String(key)));
        changed.insert(QStringLiteral("harkenDocument"), header);
        documents.insert(mainPath, changed);
        compare(false);
    }
    documents.insert(mainPath, main);
    for (const auto& value : {QJsonValue(QJsonValue::Undefined), QJsonValue(0), QJsonValue(-1),
                             QJsonValue(1.5), QJsonValue(true), QJsonValue(QStringLiteral("4"))}) {
        header = HarkenTest::projectHeader(originalRoot);
        header.insert(QStringLiteral("packSchemaVersion"), value);
        root.insert(QStringLiteral("harkenDocument"), header);
        compare(false);
    }
    header = HarkenTest::projectHeader(originalRoot);
    header.insert(QStringLiteral("packSchemaVersion"), HARKEN_PACK_SCHEMA_VERSION + 1);
    root.insert(QStringLiteral("harkenDocument"), header);
    compare(true);
    root = originalRoot;
    compare(true);

    const QString parentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject parent = main;
    header = HarkenTest::projectHeader(parent);
    header.insert(QStringLiteral("id"), parentId);
    parent.insert(QStringLiteral("harkenDocument"), header);
    content = HarkenTest::projectContent(parent);
    content.insert(QStringLiteral("guid"), parentId);
    content.insert(QStringLiteral("name"), QStringLiteral("Parent"));
    parent.insert(QStringLiteral("content"), content);
    documents.insert(QStringLiteral("Layers/Parent.vp"), parent);
    changed = main;
    content = HarkenTest::projectContent(main);
    content.insert(QStringLiteral("parentRef"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("item")},
        {QStringLiteral("id"), parentId},
    });
    changed.insert(QStringLiteral("content"), content);
    documents.insert(mainPath, changed);
    compare(true);
    documents.remove(QStringLiteral("Layers/Parent.vp"));
    compare(false);
    documents.insert(QStringLiteral("Layers/Parent.vp"), parent);
    compare(true);

    header = HarkenTest::projectHeader(root);
    const QString otherProject = QUuid::createUuid().toString(QUuid::WithoutBraces);
    header.insert(QStringLiteral("id"), otherProject);
    header.insert(QStringLiteral("projectId"), otherProject);
    root.insert(QStringLiteral("harkenDocument"), header);
    compare(false);
    root = originalRoot;
    context.clear();
    compare(true);
}

TEST(ProjectContract, IncrementalDependenciesFollowReferencesWithoutPullingInUnrelatedDocuments)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject root = readJson(projectPath);
    const QString mainPath = QStringLiteral("Layers/Main.vp");
    QJsonObject main = readJson(directory.filePath(mainPath));
    QJsonObject other = main;
    const QString otherGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto header = HarkenTest::projectHeader(other);
    header.insert(QStringLiteral("id"), otherGuid);
    other.insert(QStringLiteral("harkenDocument"), header);
    auto content = HarkenTest::projectContent(other);
    content.insert(QStringLiteral("guid"), otherGuid);
    content.insert(QStringLiteral("name"), QStringLiteral("Other"));
    other.insert(QStringLiteral("content"), content);
    writeJson(directory.filePath(QStringLiteral("Layers/Other.vp")), other);
    writeJson(directory.filePath(QStringLiteral("A.hsp")), root);

    VNX::ProjectContractValidationBaseline baseline;
    ASSERT_TRUE(VNX::ProjectContractRegistry::instance().validateTree(
        directory.path(), root, true, {}, nullptr, &baseline,
        QFileInfo(projectPath).fileName()).isEmpty());
    ASSERT_TRUE(baseline.isValid());
    content = HarkenTest::projectContent(main);
    content.insert(QStringLiteral("expanded"), true);
    main.insert(QStringLiteral("content"), content);
    VNX::ProjectContractValidationBaseline candidate;
    VNX::ProjectContractValidationDetails details;
    ASSERT_TRUE(VNX::ProjectContractRegistry::instance().validateIncrementalSnapshot(
        directory.path(), root, true, {{mainPath, main}}, {}, baseline,
        &candidate, nullptr, &details).isEmpty());
    EXPECT_FALSE(details.fullValidation);
    EXPECT_TRUE(details.readPaths.contains(QStringLiteral("Baseline.hsp")));
    EXPECT_TRUE(details.readPaths.contains(QStringLiteral("Busses/Root.vp")));
    EXPECT_TRUE(details.readPaths.contains(mainPath));
    EXPECT_FALSE(details.readPaths.contains(QStringLiteral("Layers/Other.vp")));
    EXPECT_FALSE(details.readPaths.contains(QStringLiteral("A.hsp")));
    EXPECT_NE(candidate.documents().value(mainPath), baseline.documents().value(mainPath));

    QTemporaryDir emptyDirectory;
    ASSERT_TRUE(emptyDirectory.isValid());
    VNX::ProjectContractValidationBaseline renamedBaseline;
    ASSERT_TRUE(VNX::ProjectContractRegistry::instance().validateSnapshot(
        emptyDirectory.path(), root, true, baseline.documents(), nullptr,
        &renamedBaseline, QStringLiteral("Saved.hsp")).isEmpty());
    EXPECT_TRUE(renamedBaseline.dependencyPaths({}).isEmpty());
    EXPECT_TRUE(renamedBaseline.dependencyPaths({mainPath}).contains(QStringLiteral("Saved.hsp")));

    content.insert(QStringLiteral("parentRef"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("item")},
        {QStringLiteral("id"), HarkenTest::projectContent(main).value(QStringLiteral("guid"))}});
    main.insert(QStringLiteral("content"), content);
    EXPECT_FALSE(VNX::ProjectContractRegistry::instance().validateIncrementalSnapshot(
        directory.path(), root, true, {{mainPath, main}}, {}, baseline,
        &candidate, nullptr, &details).isEmpty());
    EXPECT_TRUE(details.fullValidation);
    EXPECT_TRUE(details.readPaths.contains(QStringLiteral("Layers/Other.vp")));
    EXPECT_FALSE(candidate.isValid());
    EXPECT_TRUE(baseline.isValid());
}

TEST(ProjectContract, ParentDependenciesKeepInheritedTargetsWithoutExpandingPlaybackSubtrees)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject root = readJson(projectPath);
    const QJsonObject original = readJson(directory.filePath(QStringLiteral("Layers/Main.vp")));
    const auto newGuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    const QString parentId = newGuid();
    const QString childId = HarkenTest::projectContent(original).value(QStringLiteral("guid")).toString();
    const QString siblingId = newGuid();
    const QString descendantId = newGuid();
    const QString variableId = newGuid();
    const QString attenuationId = newGuid();
    const auto definition = [&](const QString& id, const QString& name,
                                const QString& type, const QString& parent) {
        auto content = HarkenTest::projectContent(original);
        content.insert(QStringLiteral("guid"), id);
        content.insert(QStringLiteral("name"), name);
        content.insert(QStringLiteral("type"), type);
        content.insert(QStringLiteral("parentRef"), parent.isEmpty()
            ? QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}}
            : QJsonObject{{QStringLiteral("kind"), QStringLiteral("item")},
                          {QStringLiteral("id"), parent}});
        return HarkenTest::projectDocument(QStringLiteral("project.layer"), id,
                                           HarkenTest::kProjectId, content);
    };
    const auto selection = [&](const QStringList& children, const QString& variable = QString()) {
        VNX::SelectionData data;
        data.shared.occurrenceOrdinalHighWater = children.size();
        for (qsizetype index = 0; index < children.size(); ++index) {
            const auto ordinal = static_cast<uint32_t>(index);
            data.shared.entries.push_back({QUuid(children.at(index)), ordinal,
                {{ordinal, ordinal, 50}}, {}, {}});
        }
        if (!variable.isEmpty()) data.shared.matchAxes.push_back(QUuid(variable));
        return QJsonObject{{QStringLiteral("selection"), QJsonObject{
            {QStringLiteral("selections"), data.toJson()}}}};
    };
    auto parent = definition(parentId, QStringLiteral("Parent"), QStringLiteral("Container"), {});
    auto content = HarkenTest::projectContent(parent);
    content.insert(QStringLiteral("components"), selection({childId, siblingId}, variableId));
    auto groups = content.value(QStringLiteral("fieldGroups")).toObject();
    auto shared = groups.value(QStringLiteral("shared")).toObject();
    shared.insert(QStringLiteral("spatialRating"), QJsonObject{
        {QStringLiteral("inherit"), false},
        {QStringLiteral("values"), QJsonObject{
            {QStringLiteral("spatialAttenuationRef"), attenuationId},
            {QStringLiteral("spatialIs3DEnabled"), true},
            {QStringLiteral("spatialPanning"), 0},
            {QStringLiteral("spatialAttenuationDistanceScaling"), 1}}}});
    groups.insert(QStringLiteral("shared"), shared);
    content.insert(QStringLiteral("fieldGroups"), groups);
    parent.insert(QStringLiteral("content"), content);
    auto child = definition(childId, QStringLiteral("Main"), QStringLiteral("Sound"), parentId);
    content = HarkenTest::projectContent(child);
    content.insert(QStringLiteral("extensions"), QJsonObject{{QStringLiteral("opaqueGuid"), descendantId}});
    child.insert(QStringLiteral("content"), content);
    auto sibling = definition(siblingId, QStringLiteral("Sibling"), QStringLiteral("Container"), parentId);
    content = HarkenTest::projectContent(sibling);
    content.insert(QStringLiteral("components"), selection({descendantId}));
    sibling.insert(QStringLiteral("content"), content);
    const auto independent = [&](const QString& id, const QString& name, const QString& type,
                                  const QJsonObject& properties = QJsonObject{}) {
        return HarkenTest::projectDocument(QStringLiteral("project.") + type.toLower(), id,
            HarkenTest::kProjectId, QJsonObject{
                {QStringLiteral("guid"), id}, {QStringLiteral("type"), type},
                {QStringLiteral("name"), name},
                {QStringLiteral("parentRef"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}}},
                {QStringLiteral("components"), QJsonObject{{type.toLower(), properties}}}});
    };
    const QHash<QString, QJsonObject> documents{
        {QStringLiteral("Busses/Root.vp"), readJson(directory.filePath(QStringLiteral("Busses/Root.vp")))},
        {QStringLiteral("Layers/Main.vp"), child},
        {QStringLiteral("Layers/Parent.vp"), parent},
        {QStringLiteral("Layers/Sibling.vp"), sibling},
        {QStringLiteral("Layers/Descendant.vp"), definition(descendantId,
            QStringLiteral("Descendant"), QStringLiteral("Sound"), siblingId)},
        {QStringLiteral("Variables/Selector.vp"), independent(variableId,
            QStringLiteral("Selector"), QStringLiteral("Variable"))},
        {QStringLiteral("Attenuations/Distance.vp"), independent(attenuationId,
            QStringLiteral("Distance"), QStringLiteral("Attenuation"), QJsonObject{
                {QStringLiteral("maxDistance"), 100},
                {QStringLiteral("dimensions"), QJsonArray{}}})}};
    VNX::ProjectContractValidationBaseline baseline;
    const auto errors = VNX::ProjectContractRegistry::instance().validateSnapshot(
        directory.path(), root, true, documents, nullptr, &baseline,
        QStringLiteral("Baseline.hsp"), false);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    const auto childReads = baseline.dependencyPaths({QStringLiteral("Layers/Main.vp")});
    for (const auto& required : {QStringLiteral("Layers/Parent.vp"), QStringLiteral("Busses/Root.vp"),
                                QStringLiteral("Variables/Selector.vp"), QStringLiteral("Attenuations/Distance.vp")})
        EXPECT_TRUE(childReads.contains(required)) << required.toStdString();
    EXPECT_FALSE(childReads.contains(QStringLiteral("Layers/Sibling.vp")));
    EXPECT_FALSE(childReads.contains(QStringLiteral("Layers/Descendant.vp")));
    const auto parentReads = baseline.dependencyPaths({QStringLiteral("Layers/Parent.vp")});
    EXPECT_TRUE(parentReads.contains(QStringLiteral("Layers/Main.vp")));
    EXPECT_TRUE(parentReads.contains(QStringLiteral("Layers/Sibling.vp")));
    EXPECT_FALSE(parentReads.contains(QStringLiteral("Layers/Descendant.vp")));
}

TEST(ProjectContract, SnapshotOverridesDiskDocumentsWithoutHidingOtherInvalidFiles)
{
    QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const QString projectPath = createBaselineProject(staging);
    const QJsonObject root = readJson(projectPath);
    const QString mainPath = staging.filePath(QStringLiteral("Layers/Main.vp"));
    const QJsonObject main = readJson(mainPath);
    const QHash<QString, QJsonObject> documents{{QStringLiteral("Layers/Main.vp"), main}};
    writeJson(mainPath, QJsonObject{});
    VNX::ProjectContractReadContext context;
    EXPECT_TRUE(VNX::ProjectContractRegistry::instance().validateSnapshot(
        staging.path(), root, true, documents, &context).isEmpty());
#if defined(Q_OS_WIN)
    const QString alternatePath = QStringLiteral("layers/mAIN.vp");
    if (QFileInfo::exists(staging.filePath(alternatePath)))
        EXPECT_TRUE(VNX::ProjectContractRegistry::instance().validateSnapshot(
            staging.path(), root, true, {{alternatePath, main}}, &context).isEmpty());
#endif

    const QString extraPath = staging.filePath(QStringLiteral("Layers/Extra.vp"));
    writeJson(extraPath, QJsonObject{});
    auto loadedDocuments = documents;
    loadedDocuments.insert(QStringLiteral("Busses/Root.vp"),
        readJson(staging.filePath(QStringLiteral("Busses/Root.vp"))));
    VNX::ProjectContractValidationBaseline loadedBaseline;
    EXPECT_TRUE(VNX::ProjectContractRegistry::instance().validateSnapshot(
        staging.path(), root, true, loadedDocuments, nullptr, &loadedBaseline,
        QStringLiteral("Baseline.hsp"), false).isEmpty());
    EXPECT_TRUE(loadedBaseline.isValid());
    EXPECT_FALSE(loadedBaseline.documents().contains(QStringLiteral("Layers/Extra.vp")));
    auto actual = VNX::ProjectContractRegistry::instance().validateSnapshot(
        staging.path(), root, true, documents, &context);
    EXPECT_FALSE(actual.isEmpty());
    writeJson(mainPath, main);
    auto expected = VNX::ProjectContractRegistry::instance().validateTree(
        staging.path(), root, true);
    actual.sort();
    expected.sort();
    EXPECT_EQ(actual, expected);

    QFile extra(extraPath);
    ASSERT_TRUE(extra.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(extra.write("{"), 1);
    extra.close();
    actual = VNX::ProjectContractRegistry::instance().validateSnapshot(
        staging.path(), root, true, documents, &context);
    expected = VNX::ProjectContractRegistry::instance().validateTree(
        staging.path(), root, true);
    actual.sort();
    expected.sort();
    EXPECT_FALSE(actual.isEmpty());
    EXPECT_EQ(actual, expected);
    ASSERT_TRUE(QFile::remove(extraPath));
    EXPECT_TRUE(VNX::ProjectContractRegistry::instance().validateSnapshot(
        staging.path(), root, true, documents, &context).isEmpty());
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

TEST(ProjectTransaction, DeltaRecoveryRestoresOnlyItsExplicitMutations)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject original = readJson(projectPath);
    const QJsonObject payload{{QStringLiteral("value"), 1}};
    writeJson(directory.filePath(QStringLiteral("Nodes/remove.vp")), payload);
    writeJson(directory.filePath(QStringLiteral("Nodes/old.vp")), payload);
    writeJson(directory.filePath(QStringLiteral("Nodes/unrelated.vp")), payload);
    ASSERT_TRUE(QDir().mkpath(directory.filePath(QStringLiteral("Empty"))));
    const QJsonObject changed{{QStringLiteral("value"), 2}};
    VNX::ProjectTransaction::DeltaPlan plan;
    plan.writes = {
        {QStringLiteral("Baseline.hsp"), QJsonDocument(changed).toJson()},
        {QStringLiteral("New/Nested/added.vp"), QJsonDocument(changed).toJson()}};
    plan.deletes = {QStringLiteral("Nodes/remove.vp")};
    plan.renames = {{QStringLiteral("Nodes/old.vp"), QStringLiteral("Nodes/renamed.vp")}};
    plan.directories = {{QStringLiteral("Empty"), false}};
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        writeJson(directory.filePath(QStringLiteral("Nodes/remove.vp")), changed);
        ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
        EXPECT_EQ(readJson(projectPath), changed);
        EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("Nodes/remove.vp"))));
        EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("Nodes/old.vp"))));
        EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Nodes/renamed.vp"))), payload);
        EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("Empty"))));
        writeJson(directory.filePath(QStringLiteral("Nodes/unrelated.vp")), changed);
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), original);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Nodes/remove.vp"))), payload);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Nodes/old.vp"))), payload);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Nodes/unrelated.vp"))), changed);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("Nodes/renamed.vp"))));
    EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("New"))));
    EXPECT_TRUE(QFileInfo(directory.filePath(QStringLiteral("Empty"))).isDir());
}

TEST(ProjectTransaction, BatchedDeltaWritesCommitAndRecoverAsOneChangeSet)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject original{{QStringLiteral("version"), 1}};
    const QJsonObject saved{{QStringLiteral("version"), 2}};
    VNX::ProjectTransaction::DeltaPlan plan;
    for (int index = 0; index < 96; ++index) {
        const QString path = QStringLiteral("Layers/Batch%1.vp").arg(index, 3, 10, QLatin1Char('0'));
        writeJson(directory.filePath(path), original);
        plan.writes.append({path, QJsonDocument(saved).toJson()});
    }
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
        for (const auto& write : plan.writes)
            EXPECT_EQ(readJson(directory.filePath(write.path)), saved);
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    for (const auto& write : plan.writes)
        EXPECT_EQ(readJson(directory.filePath(write.path)), original);
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    for (const auto& write : plan.writes)
        EXPECT_EQ(readJson(directory.filePath(write.path)), saved);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
}

TEST(ProjectTransaction, FailedBatchProtectsExternalContentUntilRecoveryCanFinish)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject original{{QStringLiteral("version"), 1}};
    const QJsonObject saved{{QStringLiteral("version"), 2}};
    VNX::ProjectTransaction::DeltaPlan plan;
    for (int index = 0; index < 96; ++index) {
        const QString path = QStringLiteral("Layers/Batch%1.vp").arg(index, 3, 10, QLatin1Char('0'));
        writeJson(directory.filePath(path), original);
        plan.writes.append({path, QJsonDocument(saved).toJson()});
    }
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(QDir().mkpath(QDir(transaction->backupDirectory()).filePath(
            QStringLiteral(".staged/") + plan.writes.first().path)));
        EXPECT_FALSE(transaction->applyDelta(&error));
        ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
        for (const auto& write : plan.writes)
            EXPECT_EQ(readJson(directory.filePath(write.path)), original);
    }
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    const QString blocked = directory.filePath(plan.writes.first().path);
    ASSERT_TRUE(QFile::remove(blocked));
    ASSERT_TRUE(QDir().mkpath(blocked));
    const QString externalPath = QDir(blocked).filePath(QStringLiteral("External.json"));
    const QJsonObject external{{QStringLiteral("external"), true}};
    writeJson(externalPath, external);
    EXPECT_FALSE(transaction->applyDelta(&error));
    EXPECT_FALSE(transaction->rollback(&error));
    EXPECT_EQ(readJson(externalPath), external);
    ASSERT_TRUE(QFile::remove(externalPath));
    ASSERT_TRUE(QDir().rmdir(blocked));
    writeJson(blocked, original);
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    for (const auto& write : plan.writes)
        EXPECT_EQ(readJson(directory.filePath(write.path)), original);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
}

TEST(ProjectTransaction, ReadOnlyDependenciesRejectConflictsWithoutPreparingRecovery)
{
    QTemporaryDir container;
    ASSERT_TRUE(container.isValid());
    QTemporaryDir directory(container.filePath(QStringLiteral("project-XXXXXX")));
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    QFile file(projectPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray hash = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    file.close();
    const QList<VNX::ProjectTransaction::ReadDependency> readSet{
        {QStringLiteral("Baseline.hsp"), hash},
        {QStringLiteral("NotCreated.vp"), {}, false}};
    const auto entries = QDir(container.path()).entryList(QDir::AllEntries | QDir::Hidden
        | QDir::System | QDir::NoDotAndDotDot);
    QString error;
    ASSERT_TRUE(VNX::ProjectTransaction::verifyReadSet(projectPath, readSet, &error))
        << error.toStdString();
    writeJson(directory.filePath(QStringLiteral("NotCreated.vp")),
              QJsonObject{{QStringLiteral("external"), true}});
    EXPECT_FALSE(VNX::ProjectTransaction::verifyReadSet(projectPath, readSet, &error));
    ASSERT_TRUE(QFile::remove(directory.filePath(QStringLiteral("NotCreated.vp"))));
    const QJsonObject external{{QStringLiteral("external"), true}};
    writeJson(projectPath, external);
    EXPECT_FALSE(VNX::ProjectTransaction::verifyReadSet(projectPath, readSet, &error));
    EXPECT_EQ(readJson(projectPath), external);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
    EXPECT_EQ(QDir(container.path()).entryList(QDir::AllEntries | QDir::Hidden
        | QDir::System | QDir::NoDotAndDotDot), entries);
}

TEST(ProjectTransaction, DeltaReadConflictDoesNotRestoreExternalDependencyContents)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject original = readJson(projectPath);
    const QString dependency = directory.filePath(QStringLiteral("Nodes/dependency.vp"));
    writeJson(dependency, QJsonObject{{QStringLiteral("value"), 1}});
    QFile file(dependency);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray hash = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    file.close();
    VNX::ProjectTransaction::DeltaPlan plan;
    plan.writes = {{QStringLiteral("Baseline.hsp"), QByteArrayLiteral("{\"value\":2}")}};
    plan.readSet = {{QStringLiteral("Nodes/dependency.vp"), hash}};
    QString error;
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
    const QJsonObject external{{QStringLiteral("external"), true}};
    writeJson(dependency, external);
    EXPECT_FALSE(transaction->commit(false, &error));
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), original);
    EXPECT_EQ(readJson(dependency), external);
    transaction.reset();
    EXPECT_EQ(VNX::ProjectTransaction::beginDelta(projectPath, plan, &error), nullptr);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
}

TEST(ProjectTransaction, DeltaOwnsItsInstallationAndProtectsLaterExternalChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    VNX::ProjectTransaction::DeltaPlan plan;
    const QJsonObject saved{{QStringLiteral("saved"), true}};
    plan.writes = {{QStringLiteral("Baseline.hsp"), QJsonDocument(saved).toJson()}};
    QString error;
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    const QJsonObject external{{QStringLiteral("external"), true}};
    writeJson(projectPath, external);
    ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    EXPECT_EQ(readJson(projectPath), saved);
    transaction.reset();
    transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
    writeJson(projectPath, external);
    EXPECT_FALSE(transaction->commit(false, &error));
    EXPECT_FALSE(transaction->rollback(&error));
    EXPECT_EQ(readJson(projectPath), external);
    writeJson(projectPath, saved);
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
}

TEST(ProjectTransaction, DeltaDirectoryRemovalCannotConsumeUnplannedFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QString outsider = directory.filePath(QStringLiteral("Nodes/unrelated.vp"));
    const QJsonObject payload{{QStringLiteral("external"), true}};
    writeJson(outsider, payload);
    VNX::ProjectTransaction::DeltaPlan plan;
    plan.directories = {{QStringLiteral("Nodes"), false}};
    QString error;
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    EXPECT_FALSE(transaction->applyDelta(&error));
    EXPECT_EQ(readJson(outsider), payload);
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
}

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
TEST(ProjectTransaction, DeltaKeepsUnplannedParentDirectorySpelling)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject original{{QStringLiteral("value"), 1}};
    const QJsonObject changed{{QStringLiteral("value"), 2}};
    writeJson(directory.filePath(QStringLiteral("PARENT/Child/Voice.vp")), original);
    VNX::ProjectTransaction::DeltaPlan plan;
    plan.writes = {{QStringLiteral("parent/child/Voice.vp"), QJsonDocument(changed).toJson()},
                   {QStringLiteral("parent/child/New/Added.vp"), QJsonDocument(changed).toJson()}};
    plan.directories = {{QStringLiteral("parent/child/New"), true}};
    QString error;
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
    ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Dirs).contains(QStringLiteral("PARENT")));
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("PARENT")))
                    .entryList(QDir::Dirs).contains(QStringLiteral("Child")));
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("PARENT/Child/Voice.vp"))), changed);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("PARENT/Child/New/Added.vp"))), changed);
}

TEST(ProjectTransaction, DeltaCaseOnlyRenameRecoversSpellingAndFrozenContents)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectPath = createBaselineProject(directory);
    const QJsonObject payload{{QStringLiteral("value"), 1}};
    writeJson(directory.filePath(QStringLiteral("Original/Voice.vp")), payload);
    VNX::ProjectTransaction::DeltaPlan plan;
    plan.renames = {{QStringLiteral("Original/Voice.vp"), QStringLiteral("ORIGINAL/VOICE.vp")}};
    plan.directories = {{QStringLiteral("Original"), false}, {QStringLiteral("ORIGINAL"), true}};
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        writeJson(directory.filePath(QStringLiteral("Original/Voice.vp")),
                  QJsonObject{{QStringLiteral("external"), true}});
        ASSERT_TRUE(transaction->applyDelta(&error)) << error.toStdString();
        EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Dirs).contains(QStringLiteral("ORIGINAL")));
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("ORIGINAL")))
                        .entryList(QDir::Files).contains(QStringLiteral("VOICE.vp")));
        EXPECT_EQ(readJson(directory.filePath(QStringLiteral("ORIGINAL/VOICE.vp"))), payload);
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Dirs).contains(QStringLiteral("Original")));
    EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Original")))
                    .entryList(QDir::Files).contains(QStringLiteral("Voice.vp")));
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Original/Voice.vp"))), payload);
    const QJsonObject external{{QStringLiteral("external"), true}};
    writeJson(directory.filePath(QStringLiteral("Original/Unplanned.vp")), external);
    auto transaction = VNX::ProjectTransaction::beginDelta(projectPath, plan, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    writeJson(directory.filePath(QStringLiteral("Original/Voice.vp")), external);
    EXPECT_FALSE(transaction->applyDelta(&error));
    ASSERT_TRUE(transaction->rollback(&error)) << error.toStdString();
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Original/Voice.vp"))), payload);
    EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Original/Unplanned.vp"))), external);
}
#endif

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

TEST(ProjectTransaction, SnapshotReplacementFreezesDocumentsAndPreservesRecovery)
{
    for (const bool commit : {false, true}) {
        SCOPED_TRACE(commit);
        QTemporaryDir directory;
        QTemporaryDir staging;
        const QString projectPath = createBaselineProject(directory);
        const QJsonObject projectBefore = readJson(projectPath);
        const QJsonObject mainBefore = readJson(directory.filePath(QStringLiteral("Layers/Main.vp")));
        const QJsonObject changed{{QStringLiteral("revision"), 1}};
        const QJsonObject added{{QStringLiteral("newDocument"), true}};
        const QJsonObject deleted{{QStringLiteral("deleted"), true}};
        const QJsonObject auxiliary{{QStringLiteral("auxiliary"), true}};
        const QString mainPath = QStringLiteral("Layers/Main.vp");
        const QString addedPath = QStringLiteral("Layers/Added.vp");
        const QString deletedPath = QStringLiteral("Layers/Deleted.vp");
        const QString auxiliaryPath = QStringLiteral("Layers/Auxiliary.bin");
        const QString middleChangedPath = QStringLiteral("Layers/Retained_063_changed.vp");
        const QString middleDeletedPath = QStringLiteral("Layers/Retained_127_deleted.vp");
        writeJson(directory.filePath(deletedPath), deleted);
        writeJson(directory.filePath(middleChangedPath), projectBefore);
        writeJson(directory.filePath(middleDeletedPath), deleted);
        writeJson(directory.filePath(auxiliaryPath), auxiliary);
        writeJson(staging.filePath(auxiliaryPath), auxiliary);
        writeJson(staging.filePath(QFileInfo(projectPath).fileName()), changed);
        ASSERT_TRUE(QDir(directory.path()).mkpath(QStringLiteral("Layers/Old/Empty")));
        ASSERT_TRUE(QDir(staging.path()).mkpath(QStringLiteral("Layers/New/Empty")));
        VNX::ProjectSaveSnapshot snapshot(staging.path());
        ASSERT_TRUE(snapshot.addDocument(staging.filePath(mainPath), changed));
        ASSERT_TRUE(snapshot.addDocument(staging.filePath(addedPath), added));
        ASSERT_TRUE(snapshot.addDocument(staging.filePath(middleChangedPath), changed));
        QStringList writeSet{QFileInfo(projectPath).fileName(), mainPath, addedPath,
                             deletedPath, auxiliaryPath, middleChangedPath, middleDeletedPath};
        QStringList retainedPaths;
        for (int index = 0; index < 192; ++index) {
            const QString relative = QStringLiteral("Layers/Retained_%1.vp")
                .arg(index, 3, 10, QLatin1Char('0'));
            ASSERT_TRUE(snapshot.addDocument(staging.filePath(relative),
                QJsonObject{{QStringLiteral("retained"), index}}));
            QFile file(directory.filePath(relative));
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
            const QByteArray bytes = snapshot.files().value(relative);
            ASSERT_EQ(file.write(bytes), bytes.size());
            retainedPaths.append(relative);
            writeSet.prepend(relative);
        }
        QString error;
#if defined(Q_OS_WIN)
        {
            const QString first = directory.filePath(retainedPaths.at(17));
            const QString last = directory.filePath(retainedPaths.at(111));
            const HANDLE firstHandle = CreateFileW(reinterpret_cast<LPCWSTR>(first.utf16()),
                GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            ASSERT_NE(firstHandle, INVALID_HANDLE_VALUE);
            auto closeFirst = qScopeGuard([&] { CloseHandle(firstHandle); });
            const HANDLE lastHandle = CreateFileW(reinterpret_cast<LPCWSTR>(last.utf16()),
                GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            ASSERT_NE(lastHandle, INVALID_HANDLE_VALUE);
            auto closeLast = qScopeGuard([&] { CloseHandle(lastHandle); });
            EXPECT_EQ(VNX::ProjectTransaction::beginReplacement(
                projectPath, writeSet, staging.path(), snapshot, &error), nullptr);
            EXPECT_TRUE(QDir::fromNativeSeparators(error).contains(retainedPaths.at(17)))
                << error.toStdString();
            EXPECT_FALSE(error.contains(retainedPaths.at(111))) << error.toStdString();
            EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
        }
        error.clear();
#endif
        auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
            writeSet, staging.path(), snapshot, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();

        const QJsonObject retargeted{{QStringLiteral("revision"), 2}};
        ASSERT_TRUE(snapshot.addDocument(staging.filePath(mainPath), retargeted));
        ASSERT_TRUE(snapshot.addDocument(staging.filePath(addedPath), retargeted));
        writeJson(staging.filePath(mainPath), retargeted);
        writeJson(staging.filePath(addedPath), retargeted);
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_EQ(readJson(projectPath), changed);
        EXPECT_EQ(readJson(directory.filePath(mainPath)), changed);
        EXPECT_EQ(readJson(directory.filePath(addedPath)), added);
        EXPECT_EQ(readJson(directory.filePath(auxiliaryPath)), auxiliary);
        EXPECT_EQ(readJson(directory.filePath(middleChangedPath)), changed);
        EXPECT_FALSE(QFileInfo::exists(directory.filePath(deletedPath)));
        EXPECT_FALSE(QFileInfo::exists(directory.filePath(middleDeletedPath)));
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/New/Empty"))).exists());
        EXPECT_FALSE(QDir(directory.filePath(QStringLiteral("Layers/Old"))).exists());
        if (commit)
            ASSERT_TRUE(transaction->commit(false, &error)) << error.toStdString();
        transaction.reset();
        ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
        EXPECT_EQ(readJson(projectPath), commit ? changed : projectBefore);
        EXPECT_EQ(readJson(directory.filePath(mainPath)), commit ? changed : mainBefore);
        EXPECT_EQ(readJson(directory.filePath(auxiliaryPath)), auxiliary);
        EXPECT_EQ(readJson(directory.filePath(middleChangedPath)), commit ? changed : projectBefore);
        EXPECT_EQ(QFileInfo::exists(directory.filePath(addedPath)), commit);
        EXPECT_EQ(QDir(directory.filePath(QStringLiteral("Layers/New/Empty"))).exists(), commit);
        EXPECT_EQ(QDir(directory.filePath(QStringLiteral("Layers/Old/Empty"))).exists(), !commit);
        if (!commit) {
            EXPECT_EQ(readJson(directory.filePath(deletedPath)), deleted);
            EXPECT_EQ(readJson(directory.filePath(middleDeletedPath)), deleted);
        } else {
            EXPECT_FALSE(QFileInfo::exists(directory.filePath(middleDeletedPath)));
        }
        for (const QString& relative : retainedPaths) {
            QFile file(directory.filePath(relative));
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            EXPECT_EQ(file.readAll(), snapshot.files().value(relative));
        }
        EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
    }
}

TEST(ProjectTransaction, RetainedSnapshotDocumentsStillRejectExternalChanges)
{
    for (const int documentCount : {1, 128}) {
        SCOPED_TRACE(documentCount);
        for (const int phase : {0, 1, 2, 3}) {
            SCOPED_TRACE(phase);
            QTemporaryDir directory;
            QTemporaryDir staging;
            const QString projectPath = createBaselineProject(directory);
            ASSERT_TRUE(QDir(staging.path()).mkpath(QStringLiteral("Layers")));
            const QJsonObject external{{QStringLiteral("external"), true}};
            VNX::ProjectSaveSnapshot snapshot(staging.path());
            QStringList writeSet;
            for (int index = 0; index < documentCount; ++index) {
                const QString relative = QStringLiteral("Layers/Retained_%1.vp")
                    .arg(index, 3, 10, QLatin1Char('0'));
                ASSERT_TRUE(snapshot.addDocument(staging.filePath(relative),
                    QJsonObject{{QStringLiteral("retained"), index}}));
                QFile file(directory.filePath(relative));
                ASSERT_TRUE(file.open(QIODevice::WriteOnly));
                const QByteArray bytes = snapshot.files().value(relative);
                ASSERT_EQ(file.write(bytes), bytes.size());
                writeSet.prepend(relative);
            }
            QString error;
            auto transaction = VNX::ProjectTransaction::beginReplacement(projectPath,
                writeSet, staging.path(), snapshot, &error);
            ASSERT_NE(transaction, nullptr) << error.toStdString();
            if (phase == 1 || phase == 2)
                ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(), &error))
                    << error.toStdString();
            QStringList changedPaths{writeSet.at(documentCount / 4)};
            if (documentCount > 1)
                changedPaths.append(writeSet.at(3 * documentCount / 4));
            changedPaths.sort();
            for (const QString& relative : changedPaths)
                writeJson(directory.filePath(relative), external);
            if (phase == 0) {
                EXPECT_FALSE(transaction->replaceFromDirectory(staging.path(), &error));
            } else if (phase == 1 || phase == 3) {
                EXPECT_FALSE(transaction->commit(false, &error));
            } else {
                transaction.reset();
                EXPECT_FALSE(VNX::ProjectTransaction::recover(projectPath, &error));
            }
            EXPECT_TRUE(QDir::fromNativeSeparators(error).contains(changedPaths.first()))
                << error.toStdString();
            if (changedPaths.size() > 1)
                EXPECT_FALSE(error.contains(changedPaths.last())) << error.toStdString();
            for (const QString& relative : changedPaths)
                EXPECT_EQ(readJson(directory.filePath(relative)), external);
            transaction.reset();
            for (const QString& relative : changedPaths) {
                QFile file(directory.filePath(relative));
                ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
                const QByteArray bytes = snapshot.files().value(relative);
                ASSERT_EQ(file.write(bytes), bytes.size());
            }
            ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
            for (const QString& relative : writeSet) {
                QFile file(directory.filePath(relative));
                ASSERT_TRUE(file.open(QIODevice::ReadOnly));
                EXPECT_EQ(file.readAll(), snapshot.files().value(relative));
            }
            EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(projectPath));
        }
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
    VNX::FileWatcher watcher;
    watcher.startWatching(directory.path());
    const auto resumeWatcher = qScopeGuard([&] { watcher.endProjectWrite(); });
    QString error;
    {
        auto transaction = VNX::ProjectTransaction::begin(projectPath,
            {QStringLiteral("Layers/File"), QStringLiteral("Layers/File/new.bin"),
             QStringLiteral("Layers/Directory"), QStringLiteral("Layers/Directory/child.bin")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        ASSERT_TRUE(watcher.beginProjectWrite(directory.path(), transaction->changedPaths()));
        ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
            QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
        EXPECT_TRUE(QFileInfo(directory.filePath(QStringLiteral("Layers/Directory"))).isFile());
        EXPECT_TRUE(QFileInfo(directory.filePath(QStringLiteral("Layers/File"))).isDir());
    }
    ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
    watcher.endProjectWrite();
    EXPECT_TRUE(watcher.isWatching());
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
    for (const bool useSnapshot : {false, true}) {
        SCOPED_TRACE(useSnapshot);
        QTemporaryDir directory;
        QTemporaryDir staging;
        const QString projectPath = createBaselineProject(directory);
        const QJsonObject contents{{QStringLiteral("unchanged"), true}};
        const QString original = directory.filePath(QStringLiteral("Layers/Upper/File.vp"));
        writeJson(original, contents);
        if (!QFileInfo::exists(directory.filePath(QStringLiteral("Layers/upper/file.vp"))))
            GTEST_SKIP() << "Requires a case-insensitive filesystem";
        writeJson(staging.filePath(QStringLiteral("Layers/upper/file.vp")), contents);
        VNX::ProjectSaveSnapshot snapshot(staging.path());
        ASSERT_TRUE(snapshot.addDocument(
            staging.filePath(QStringLiteral("Layers/upper/file.vp")), contents));
        if (useSnapshot) {
            QFile originalFile(original);
            ASSERT_TRUE(originalFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray bytes = snapshot.files().value(QStringLiteral("Layers/upper/file.vp"));
            ASSERT_EQ(originalFile.write(bytes), bytes.size());
        }
        QString error;
        {
            auto transaction = useSnapshot
                ? VNX::ProjectTransaction::beginReplacement(projectPath,
                    {QStringLiteral("Layers/upper/file.vp")}, staging.path(), snapshot, &error)
                : VNX::ProjectTransaction::beginReplacement(projectPath,
                    {QStringLiteral("Layers/upper/file.vp")}, staging.path(), &error);
            ASSERT_NE(transaction, nullptr) << error.toStdString();
            ASSERT_TRUE(transaction->replaceFromDirectory(staging.path(),
                QStringList{QStringLiteral("Layers")}, &error)) << error.toStdString();
            EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers"))).entryList().contains(QStringLiteral("upper")));
            EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/upper"))).entryList().contains(QStringLiteral("file.vp")));
            EXPECT_EQ(readJson(directory.filePath(QStringLiteral("Layers/upper/file.vp"))), contents);
        }
        ASSERT_TRUE(VNX::ProjectTransaction::recover(projectPath, &error)) << error.toStdString();
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers"))).entryList().contains(QStringLiteral("Upper")));
        EXPECT_TRUE(QDir(directory.filePath(QStringLiteral("Layers/Upper"))).entryList().contains(QStringLiteral("File.vp")));
        EXPECT_EQ(readJson(original), contents);
    }
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
