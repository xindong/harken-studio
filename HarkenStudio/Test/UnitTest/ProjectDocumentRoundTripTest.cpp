#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/BindingSemantics.h"
#include "Core/ActionManager.h"
#include "Core/ActionStepEditing.h"
#include "Core/EffectChainSemantics.h"
#include "Core/Components/VSegment.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTypes.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <utility>

namespace {

using namespace VNX;

QByteArray readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

QJsonObject readObject(const QString& path)
{
    const QJsonDocument document = QJsonDocument::fromJson(readFile(path));
    EXPECT_TRUE(document.isObject()) << path.toStdString();
    return document.object();
}

void writeObject(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QMap<QString, QString> documentPathsById(const QString& projectDirectory)
{
    QMap<QString, QString> paths;
    QDirIterator files(projectDirectory,
                       QStringList{QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                       QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString id = HarkenTest::projectHeader(readObject(path))
                               .value(QStringLiteral("id")).toString();
        EXPECT_FALSE(id.isEmpty()) << path.toStdString();
        EXPECT_FALSE(paths.contains(id)) << id.toStdString();
        paths.insert(id, path);
    }
    return paths;
}

QJsonObject extensionMetadata(const QString& id)
{
    return {
        {QStringLiteral("futureAuthorNotes"), QJsonObject{
            {QStringLiteral("blob"), QString(4096, QLatin1Char('a'))},
            {QStringLiteral("document"), id},
            {QStringLiteral("values"), QJsonArray{
                QJsonValue::Null, true, QJsonValue(qint64(9007199254740993LL)),
                -2.5, QStringLiteral("Unedited metadata"), QJsonObject{}, QJsonArray{},
                QJsonObject{{QStringLiteral("type"), QStringLiteral("Piece")},
                    {QStringLiteral("pieceTime"), QStringLiteral("piecetimeline")}},
            }},
        }},
    };
}

ProjectItem* findItem(ProjectData& project, const QUuid& guid)
{
    const auto find = [&](auto&& self, ProjectItem* item) -> ProjectItem* {
        if (!item) return nullptr;
        if (item->guid == guid) return item;
        for (const auto& child : item->children)
            if (ProjectItem* found = self(self, child.get())) return found;
        return nullptr;
    };
    for (ProjectFileBinding* binding : project.allFileBindings())
        if (ProjectItem* item = find(find, binding->rootItem.get())) return item;
    return nullptr;
}

TEST(ProjectDocumentModuleData, KnownBindingAndEffectEditsPreserveRawModuleFields)
{
    const QJsonObject futureData = extensionMetadata(QStringLiteral("module"));
    ParameterBindingDef binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(41));
    binding.address.path = BindingPath{3};
    QJsonObject bindingJson = binding.toJson().toObject();
    QJsonObject address = bindingJson.value(QStringLiteral("address")).toObject();
    address.insert(QStringLiteral("futureAddressData"), futureData);
    bindingJson.insert(QStringLiteral("address"), address);
    ParameterBindingDef loadedBinding = ParameterBindingDef::fromJson(bindingJson);
    ASSERT_TRUE(loadedBinding.typed);
    loadedBinding.outputMax = 12.0f;
    loadedBinding.address.path.reset();
    const QJsonObject savedBinding = loadedBinding.toJson().toObject();
    const QJsonObject savedAddress = savedBinding.value(QStringLiteral("address")).toObject();
    EXPECT_EQ(savedAddress.value(QStringLiteral("futureAddressData")), futureData);
    const ParameterBindingDef reopenedBinding = ParameterBindingDef::fromJson(savedBinding);
    ASSERT_TRUE(reopenedBinding.typed);
    EXPECT_EQ(reopenedBinding.addressExtensions.value(QStringLiteral("futureAddressData")),
              futureData);

    const QJsonObject effectJson{
        {QStringLiteral("runtimeStableId"), 42},
        {QStringLiteral("type"), QStringLiteral("gain")},
        {QStringLiteral("parameters"), QJsonArray{-3.0}},
        {QStringLiteral("bypass"), true},
        {QStringLiteral("futureEffectData"), futureData},
    };
    EffectEntry effect = EffectEntry::fromJson(effectJson);
    ASSERT_TRUE(effect.typed);
    effect.parameters[0] = -6.0f;
    effect.bypass = false;
    const QJsonObject savedEffect = effect.toJson().toObject();
    EXPECT_EQ(savedEffect.value(QStringLiteral("futureEffectData")), futureData);
    const EffectEntry reopenedEffect = EffectEntry::fromJson(savedEffect);
    ASSERT_TRUE(reopenedEffect.typed);
    EXPECT_EQ(reopenedEffect.extensions.value(QStringLiteral("futureEffectData")),
              futureData);
}

class ProjectDocumentRoundTripTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char executable[] = "harken_editor_tests";
            static char* argv[] = {executable, nullptr};
            static QApplication app(argc, argv);
        }
        ASSERT_TRUE(directory_.isValid());
        auto& manager = ProjectManager::instance();
        ProjectConfig config;
        config.name = QStringLiteral("MetadataRoundTrip");
        config.location = directory_.path();
        ASSERT_TRUE(manager.createProject(config));
        auto* layer = manager.createItem(
            nullptr, QStringLiteral("Main"), ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        auto* childLayer = manager.createItem(
            layer, QStringLiteral("Nested"), ProjectItem::Type::Layer);
        ASSERT_NE(childLayer, nullptr);
        ASSERT_NE(manager.createItem(childLayer, QStringLiteral("Tone"),
                                     ProjectItem::Type::Sound), nullptr);
        auto* segment = manager.createItem(layer, QStringLiteral("Piece"),
                                           ProjectItem::Type::Segment);
        ASSERT_NE(segment, nullptr);
        segmentGuid_ = segment->guid;
        ASSERT_NE(manager.createItem(segment, QStringLiteral("Timeline"),
                                     ProjectItem::Type::Track), nullptr);
        segment->comp<VSegment>()->markers.items.push_back(
            {QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("pieceTime"), 1000000});
        for (const auto& [name, type] : {
                 std::pair{QStringLiteral("Trigger"), ProjectItem::Type::Action},
                 std::pair{QStringLiteral("Space"), ProjectItem::Type::Attenuation},
                 std::pair{QStringLiteral("Intensity"), ProjectItem::Type::Variable},
             }) {
            ASSERT_NE(manager.createItem(nullptr, name, type), nullptr);
        }
        ASSERT_NE(manager.createItem(manager.getMasterBusItem(),
                                     QStringLiteral("Music"),
                                     ProjectItem::Type::Bus), nullptr);
        ASSERT_TRUE(manager.saveProject());
        projectPath_ = manager.currentProject()->projectPath;
        projectDirectory_ = QFileInfo(projectPath_).absolutePath();
        ASSERT_TRUE(manager.closeProject());

        QDirIterator documents(projectDirectory_,
                                {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                                QDir::Files, QDirIterator::Subdirectories);
        while (documents.hasNext()) {
            const QString path = documents.next();
            QJsonObject document = readObject(path);
            QJsonObject header = HarkenTest::projectHeader(document);
            const QString id = header.value(QStringLiteral("id")).toString();
            const QJsonObject metadata = extensionMetadata(id);
            header.insert(QStringLiteral("futureHeaderMetadata"), metadata);
            document.insert(QStringLiteral("harkenDocument"), header);
            QJsonObject content = HarkenTest::projectContent(document);
            content.insert(QStringLiteral("extensions"), metadata);
            document.insert(QStringLiteral("content"), content);
            writeObject(path, document);
            documentsById_.insert(id, document);
            pathsById_.insert(id, path);
        }
        ASSERT_FALSE(documentsById_.isEmpty());
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    void expectModelMetadata(ProjectData& project)
    {
        for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
            const QJsonObject expectedHeader = HarkenTest::projectHeader(it.value());
            QJsonObject header;
            QJsonObject extensions;
            if (expectedHeader.value(QStringLiteral("type"))
                == QStringLiteral("project.root")) {
                header = project.documentHeader;
                extensions = project.extensions;
            } else {
                ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
                ASSERT_NE(item, nullptr) << it.key().toStdString();
                header = item->documentHeader;
                extensions = item->extensions;
            }
            EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                      expectedHeader.value(QStringLiteral("futureHeaderMetadata")));
            EXPECT_EQ(extensions, extensionMetadata(it.key()));
        }
    }

    QMap<QString, QByteArray> savedDocumentContent()
    {
        QMap<QString, QByteArray> result;
        const auto paths = documentPathsById(projectDirectory_);
        for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
            auto bytes = readFile(it.value());
            if (it.value() == projectPath_) {
                auto document = QJsonDocument::fromJson(bytes).object();
                auto content = HarkenTest::projectContent(document);
                auto metadata = content.value(QStringLiteral("metadata")).toObject();
                metadata.remove(QStringLiteral("modifiedDate"));
                content.insert(QStringLiteral("metadata"), metadata);
                document.insert(QStringLiteral("content"), content);
                bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
            }
            result.insert(it.value(), bytes);
        }
        return result;
    }

    QTemporaryDir directory_;
    QString projectPath_;
    QString projectDirectory_;
    QUuid segmentGuid_;
    QMap<QString, QJsonObject> documentsById_;
    QMap<QString, QString> pathsById_;
};

TEST_F(ProjectDocumentRoundTripTest, AllDocumentMetadataSurvivesEditingSavingAndReopening)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    expectModelMetadata(project);
    for (auto it = pathsById_.cbegin(); it != pathsById_.cend(); ++it)
        EXPECT_EQ(readObject(it.value()), documentsById_.value(it.key()));

    project.description = QStringLiteral("Edited by the baseline reader");
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        const QJsonObject header = HarkenTest::projectHeader(it.value());
        if (header.value(QStringLiteral("type")) == QStringLiteral("project.root"))
            continue;
        ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
        ASSERT_NE(item, nullptr);
        if (item == manager.getMasterBusItem()) continue;
        const QString name = QStringLiteral("Edited_") + item->name;
        ASSERT_TRUE(manager.renameItem(item, name));
    }
    ASSERT_TRUE(manager.saveProject());
    expectModelMetadata(project);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& reopened = *manager.currentProject();
    expectModelMetadata(reopened);
    const auto savedPaths = documentPathsById(projectDirectory_);
    EXPECT_EQ(savedPaths.keys(), documentsById_.keys());
    for (auto it = savedPaths.cbegin(); it != savedPaths.cend(); ++it) {
        const QJsonObject document = readObject(it.value());
        const QJsonObject header = HarkenTest::projectHeader(document);
        EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                  HarkenTest::projectHeader(documentsById_.value(it.key()))
                      .value(QStringLiteral("futureHeaderMetadata")));
        EXPECT_EQ(HarkenTest::projectContent(document).value(QStringLiteral("extensions")),
                  extensionMetadata(it.key()));
    }
}

TEST_F(ProjectDocumentRoundTripTest, SupportedDocumentVersionsPreserveContentThroughOpenSave)
{
    auto& manager = ProjectManager::instance();
    for (const int version : {7, 8, ProjectSchema::kProjectSchemaVersion}) {
        SCOPED_TRACE(version);
        QMap<QString, QByteArray> beforeOpen;
        for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
            auto document = it.value();
            auto header = HarkenTest::projectHeader(document);
            header.insert(QStringLiteral("schemaVersion"), version);
            document.insert(QStringLiteral("harkenDocument"), header);
            auto content = HarkenTest::projectContent(document);
            if (version == 7 && header.value(QStringLiteral("type")) == QStringLiteral("project.root")) {
                const auto platform = content.value(QStringLiteral("metadata")).toObject()
                    .value(QStringLiteral("platforms")).toArray().first().toObject()
                    .value(QStringLiteral("id"));
                content.insert(QStringLiteral("packExportCurrentPlatformId"), platform);
                content.insert(QStringLiteral("packExportSelectedPlatformIds"), QJsonArray{platform});
            }
            if (content.value(QStringLiteral("type")) == QStringLiteral("Segment")) {
                content.insert(QStringLiteral("type"), QStringLiteral("Piece"));
                auto components = content.value(QStringLiteral("components")).toObject();
                auto timeline = components.take(QStringLiteral("segmenttimeline")).toObject();
                timeline.insert(QStringLiteral("pieceTime"),
                                QJsonObject{{QStringLiteral("domain"), QStringLiteral("Absolute")}});
                components.insert(QStringLiteral("piecetimeline"), timeline);
                content.insert(QStringLiteral("components"), components);
            }
            document.insert(QStringLiteral("content"), content);
            const QString path = pathsById_.value(it.key());
            writeObject(path, document);
            beforeOpen.insert(path, readFile(path));
        }
        ProjectUpgradeManager upgrades;
        const auto analysis = upgrades.analyze(projectPath_);
        ASSERT_TRUE(analysis.ok) << analysis.blockingErrors.join(QLatin1Char('\n')).toStdString();
        EXPECT_EQ(analysis.upgradeRequired, version < ProjectSchema::kProjectSchemaVersion);
        for (auto it = beforeOpen.cbegin(); it != beforeOpen.cend(); ++it)
            EXPECT_EQ(readFile(it.key()), it.value());
        if (analysis.upgradeRequired) {
            auto result = upgrades.execute(analysis);
            ASSERT_TRUE(result.ok) << result.error.toStdString();
            QString error;
            ASSERT_TRUE(upgrades.finalize(result, &error)) << error.toStdString();
        }
        QString error;
        ASSERT_TRUE(manager.loadProject(projectPath_, false, &error)) << error.toStdString();
        UndoRedoManager::instance().clearHistory();
        expectModelMetadata(*manager.currentProject());
        auto* segment = manager.findItemByGuid(segmentGuid_);
        ASSERT_NE(segment, nullptr);
        EXPECT_EQ(segment->type, ProjectItem::Type::Segment);
        EXPECT_EQ(segment->name, QStringLiteral("Piece"));
        ASSERT_NE(segment->comp<VSegment>(), nullptr);
        ASSERT_EQ(segment->comp<VSegment>()->markers.items.size(), 1u);
        EXPECT_EQ(segment->comp<VSegment>()->markers.items.front().name, QStringLiteral("pieceTime"));
        if (!analysis.upgradeRequired)
            for (auto it = beforeOpen.cbegin(); it != beforeOpen.cend(); ++it)
                EXPECT_EQ(readFile(it.key()), it.value());
        ASSERT_TRUE(manager.saveProject());
        ASSERT_TRUE(manager.closeProject());
        const auto savedPaths = documentPathsById(projectDirectory_);
        EXPECT_EQ(savedPaths.keys(), documentsById_.keys());
        for (auto it = savedPaths.cbegin(); it != savedPaths.cend(); ++it) {
            const auto document = readObject(it.value());
            EXPECT_EQ(HarkenTest::projectHeader(document).value(QStringLiteral("schemaVersion")),
                      ProjectSchema::kProjectSchemaVersion);
            const auto content = HarkenTest::projectContent(document);
            const auto original = HarkenTest::projectContent(documentsById_.value(it.key()));
            for (const auto* key : {"type", "name", "guid", "parentRef", "extensions"})
                EXPECT_EQ(content.value(QLatin1String(key)), original.value(QLatin1String(key)));
            if (it.key() == segmentGuid_.toString(QUuid::WithoutBraces))
                EXPECT_EQ(content.value(QStringLiteral("components")),
                          original.value(QStringLiteral("components")));
        }
        const auto normalized = savedDocumentContent();
        ASSERT_TRUE(manager.loadProject(projectPath_));
        UndoRedoManager::instance().clearHistory();
        ASSERT_TRUE(manager.saveProject());
        EXPECT_EQ(savedDocumentContent(), normalized);
        ASSERT_TRUE(manager.closeProject());
    }
}

TEST_F(ProjectDocumentRoundTripTest, UnchangedSavePreservesDocumentBytesAndModificationTimes)
{
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());

    const QString savedDate = QStringLiteral("2001-02-03T04:05:06Z");
    const auto savedTime = QDateTime::fromString(savedDate, Qt::ISODate);
    auto document = readObject(projectPath_);
    auto content = HarkenTest::projectContent(document);
    auto metadata = content.value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("modifiedDate"), savedDate);
    content.insert(QStringLiteral("metadata"), metadata);
    document.insert(QStringLiteral("content"), content);
    writeObject(projectPath_, document);

    QMap<QString, QByteArray> committed;
    QMap<QString, QDateTime> modificationTimes;
    const auto paths = documentPathsById(projectDirectory_);
    for (const auto& path : paths) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadWrite)) << path.toStdString();
        ASSERT_TRUE(file.setFileTime(savedTime, QFileDevice::FileModificationTime))
            << path.toStdString();
        file.close();
        committed.insert(path, readFile(path));
        modificationTimes.insert(path, QFileInfo(path).lastModified());
    }

    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
        EXPECT_EQ(QFileInfo(it.key()).lastModified(), modificationTimes.value(it.key()))
            << it.key().toStdString();
    }

    const QString description = QStringLiteral("Edited after an unchanged save");
    manager.currentProject()->description = description;
    ASSERT_TRUE(manager.saveProject());
    const auto savedMetadata = HarkenTest::projectContent(readObject(projectPath_))
        .value(QStringLiteral("metadata")).toObject();
    EXPECT_EQ(savedMetadata.value(QStringLiteral("description")).toString(), description);
    const auto modifiedTime = QDateTime::fromString(
        savedMetadata.value(QStringLiteral("modifiedDate")).toString(), Qt::ISODate);
    EXPECT_TRUE(modifiedTime.isValid());
    EXPECT_GT(modifiedTime, savedTime);
    EXPECT_GT(QFileInfo(projectPath_).lastModified(), modificationTimes.value(projectPath_));
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        if (it.key() == projectPath_) continue;
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
        EXPECT_EQ(QFileInfo(it.key()).lastModified(), modificationTimes.value(it.key()))
            << it.key().toStdString();
    }
}

TEST_F(ProjectDocumentRoundTripTest, OrdinaryEditsAndUndoPreserveTheSameDocumentsAcrossSavePaths)
{
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto& undo = UndoRedoManager::instance();
    actions.clearAllForTesting();
    const auto cleanup = qScopeGuard([&] {
        undo.clearHistory();
        actions.clearAllForTesting();
    });
    registerProjectActions(actions, &manager, nullptr);
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    const auto original = savedDocumentContent();
    QList<QUuid> edited;
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        if (HarkenTest::projectHeader(it.value()).value(QStringLiteral("type"))
            == QStringLiteral("project.root")) continue;
        auto* item = manager.findItemByGuid(QUuid(it.key()));
        ASSERT_NE(item, nullptr);
        SCOPED_TRACE(item->name.toStdString());
        QString property = QStringLiteral("notes");
        QJsonValue value = QStringLiteral("Edited after opening");
        if (item->type == ProjectItem::Type::Bus) {
            property = QStringLiteral("volume");
            value = -6.0;
        } else if (item->type == ProjectItem::Type::Action) {
            const auto* target = manager.findItemByGuid(segmentGuid_);
            ASSERT_NE(target, nullptr);
            VActionStep step;
            step.targetObjectID = target->objectID;
            step.targetName = target->name;
            property = QStringLiteral("steps");
            value = ActionStepEditing::stepsToJson({step});
        }
        const auto result = actions.execute(QStringLiteral("set_item_property"), {
            {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
            {QStringLiteral("property"), property}, {QStringLiteral("value"), value}});
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        EXPECT_TRUE(item->dirtyBit);
        edited.append(item->guid);
    }
    ASSERT_FALSE(edited.isEmpty());
    for (qsizetype i = 0; i < edited.size(); ++i) ASSERT_TRUE(undo.undo());
    for (const auto& guid : edited) EXPECT_FALSE(manager.findItemByGuid(guid)->dirtyBit);
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), original);

    for (qsizetype i = 0; i < edited.size(); ++i) ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(manager.saveProject());
    const auto saved = savedDocumentContent();
    const auto authoredItems = [&] {
        ProjectSerializer serializer;
        EXPECT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
        QMap<QUuid, QJsonObject> states;
        for (const auto& guid : edited) {
            const auto* item = manager.findItemByGuid(guid);
            EXPECT_NE(item, nullptr);
            if (item) states.insert(guid, serializer.projectItemToJson(*item, false));
        }
        return states;
    };
    const auto authored = authoredItems();
    EXPECT_NE(saved, original);
    for (const auto& guid : edited) EXPECT_FALSE(manager.findItemByGuid(guid)->dirtyBit);
    undo.clear();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), saved);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    expectModelMetadata(*manager.currentProject());
    EXPECT_EQ(authoredItems(), authored);
    for (const auto& guid : edited) {
        const auto* item = manager.findItemByGuid(guid);
        ASSERT_NE(item, nullptr);
        EXPECT_FALSE(item->dirtyBit);
    }
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), saved);
}

#ifdef Q_OS_WIN
TEST_F(ProjectDocumentRoundTripTest, FailedObjectSaveRetainsEditsForRetryAndSubsequentUndo)
{
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto& undo = UndoRedoManager::instance();
    actions.clearAllForTesting();
    const auto cleanup = qScopeGuard([&] {
        undo.clearHistory();
        actions.clearAllForTesting();
    });
    registerProjectActions(actions, &manager, nullptr);
    ASSERT_TRUE(manager.loadProject(projectPath_));
    undo.clearHistory();
    ASSERT_TRUE(manager.saveProject());
    const auto committed = savedDocumentContent();
    const auto committedRoot = readFile(projectPath_);
    auto* item = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(item, nullptr);
    const QString occupiedPath = manager.currentProject()->objectFilePath(item);
    ASSERT_TRUE(committed.contains(occupiedPath));
    HANDLE occupied = CreateFileW(reinterpret_cast<LPCWSTR>(occupiedPath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([&] {
        if (occupied != INVALID_HANDLE_VALUE) CloseHandle(occupied);
    });
    const auto result = actions.execute(QStringLiteral("set_item_property"), {
        {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
        {QStringLiteral("property"), QStringLiteral("notes")},
        {QStringLiteral("value"), QStringLiteral("Retained after failed save")}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_FALSE(manager.saveProject());
    EXPECT_EQ(savedDocumentContent(), committed);
    EXPECT_EQ(readFile(projectPath_), committedRoot);
    EXPECT_TRUE(item->dirtyBit);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(CloseHandle(occupied));
    occupied = INVALID_HANDLE_VALUE;
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(item->dirtyBit);
    EXPECT_TRUE(undo.isClean());
    EXPECT_NE(savedDocumentContent(), committed);
    ASSERT_TRUE(undo.undo());
    EXPECT_TRUE(item->dirtyBit);
    ASSERT_TRUE(manager.saveProject());
    const auto restored = savedDocumentContent();
    EXPECT_EQ(restored.keys(), committed.keys());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it) {
        if (it.key() == occupiedPath)
            EXPECT_EQ(QJsonDocument::fromJson(restored.value(it.key())),
                      QJsonDocument::fromJson(it.value()));
        else
            EXPECT_EQ(restored.value(it.key()), it.value()) << it.key().toStdString();
    }
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    item = manager.findItemByGuid(segmentGuid_);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->notes.isEmpty());
    expectModelMetadata(*manager.currentProject());
}

TEST_F(ProjectDocumentRoundTripTest, OccupiedFilesDoNotDamageCommittedContentWhenSavingFails)
{
    const QDir root(projectDirectory_);
    const QString folderPath = root.filePath(QStringLiteral("Layers/Occupied"));
    const QString emptyPath = QDir(folderPath).filePath(QStringLiteral("Empty"));
    const QString ordinaryPath = QDir(folderPath).filePath(QStringLiteral("000_locked.bin"));
    ASSERT_TRUE(QDir().mkpath(emptyPath));
    QFile ordinary(ordinaryPath);
    ASSERT_TRUE(ordinary.open(QIODevice::WriteOnly));
    ASSERT_EQ(ordinary.write("retained bytes"), 14);
    ordinary.close();

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    auto* sound = manager.createItem(nullptr, QStringLiteral("OccupiedSound"),
        ProjectItem::Type::Sound, QStringLiteral("Occupied"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(manager.saveProject());
    const HANDLE occupied = CreateFileW(reinterpret_cast<LPCWSTR>(ordinaryPath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    auto release = qScopeGuard([&] { CloseHandle(occupied); });
    QMap<QString, QByteArray> committed;
    QDirIterator files(projectDirectory_, {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
        QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        committed.insert(path, readFile(path));
    }
    auto* folder = manager.currentProject()->findFolderByPath(
        QStringLiteral("Layers"), QStringLiteral("Occupied"));
    ASSERT_NE(folder, nullptr);
    ASSERT_TRUE(manager.deleteEntries({}, {folder}));
    manager.currentProject()->description = QStringLiteral("Pending failed save");
    EXPECT_FALSE(manager.saveProject());
    for (auto it = committed.cbegin(); it != committed.cend(); ++it)
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
    EXPECT_EQ(readFile(ordinaryPath), QByteArray("retained bytes"));
    EXPECT_TRUE(QDir(emptyPath).exists());
}
#endif

}
