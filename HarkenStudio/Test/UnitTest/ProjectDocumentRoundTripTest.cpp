#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_buildinfo.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ProjectDocument.h"
#include "Core/BindingSemantics.h"
#include "Core/EffectChainSemantics.h"
#include "Core/ComponentCatalog.h"
#include "Core/Components/VPiece.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VSpatial.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTransaction.h"
#include "Core/ProjectTypes.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <utility>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

using namespace VNX;

QString studioVersion()
{
    return QString::fromUtf8(Harken::BuildInfo::Version.data(),
                             static_cast<int>(Harken::BuildInfo::Version.size()));
}

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
    EXPECT_FALSE(savedAddress.contains(QStringLiteral("path")));
    EXPECT_DOUBLE_EQ(savedBinding.value(QStringLiteral("range")).toArray().at(1).toDouble(),
                     12.0);
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
    EXPECT_FALSE(savedEffect.contains(QStringLiteral("bypass")));
    EXPECT_DOUBLE_EQ(savedEffect.value(QStringLiteral("parameters")).toArray().at(0).toDouble(),
                     -6.0);
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
            header.insert(QStringLiteral("savedStudioVersion"), savedVersion_);
            header.insert(QStringLiteral("futureHeaderMetadata"), metadata);
            document.insert(QStringLiteral("harkenDocument"), header);
            QJsonObject content = HarkenTest::projectContent(document);
            content.insert(QStringLiteral("extensions"), metadata);
            document.insert(QStringLiteral("content"), content);
            writeObject(path, document);
            documentsById_.insert(id, document);
            pathsById_.insert(id, path);
        }
        ASSERT_EQ(documentsById_.size(), 9);
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    void expectModelMetadata(ProjectData& project, const QString& savedVersion)
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
            EXPECT_EQ(header.value(QStringLiteral("savedStudioVersion")).toString(),
                      savedVersion);
        }
    }

    QTemporaryDir directory_;
    QString projectPath_;
    QString projectDirectory_;
    QMap<QString, QJsonObject> documentsById_;
    QMap<QString, QString> pathsById_;
    const QString savedVersion_ = QStringLiteral("9.7.1");
};

TEST_F(ProjectDocumentRoundTripTest, ChildDocumentsKeepIndependentPhysicalLocationsAcrossSave)
{
    QMap<QString, QString> expectedPaths;
    QMap<QString, QJsonValue> expectedParents;
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        const auto content = HarkenTest::projectContent(it.value());
        const QString name = content.value(QStringLiteral("name")).toString();
        if (name != QStringLiteral("Nested") && name != QStringLiteral("Tone")) continue;
        const QString directory = QDir(projectDirectory_).filePath(QStringLiteral("Layers/Physical_") + name);
        ASSERT_TRUE(QDir().mkpath(directory));
        const QString destination = QDir(directory).filePath(QFileInfo(pathsById_.value(it.key())).fileName());
        ASSERT_TRUE(QFile::rename(pathsById_.value(it.key()), destination));
        expectedPaths.insert(it.key(), destination);
        expectedParents.insert(it.key(), content.value(QStringLiteral("parentRef")));
    }
    ASSERT_EQ(expectedPaths.size(), 2);
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    for (auto it = expectedPaths.cbegin(); it != expectedPaths.cend(); ++it) {
        ProjectItem* item = findItem(*manager.currentProject(), QUuid(it.key()));
        ASSERT_NE(item, nullptr);
        ASSERT_NE(item->parent, nullptr);
        EXPECT_EQ(manager.currentProject()->objectFilePath(item), it.value());
        EXPECT_NE(manager.currentProject()->objectFolder(item), manager.currentProject()->objectFolder(item->parent));
    }
    ProjectItem* nested = manager.currentProject()->findItemBySelector(QStringLiteral("Nested"));
    ASSERT_NE(nested, nullptr);
    ProjectItem* created = manager.createItem(nested, QStringLiteral("Added"), ProjectItem::Type::Sound);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(manager.currentProject()->objectFolder(created), manager.currentProject()->objectFolder(nested));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(QFileInfo(manager.currentProject()->objectFilePath(created)).absolutePath(),
              QFileInfo(manager.currentProject()->objectFilePath(nested)).absolutePath());
    for (auto it = expectedPaths.cbegin(); it != expectedPaths.cend(); ++it) {
        EXPECT_TRUE(QFileInfo::exists(it.value()));
        EXPECT_FALSE(QFileInfo::exists(pathsById_.value(it.key())));
        EXPECT_EQ(HarkenTest::projectContent(readObject(it.value())).value(QStringLiteral("parentRef")),
                  expectedParents.value(it.key()));
    }
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    for (auto it = expectedPaths.cbegin(); it != expectedPaths.cend(); ++it) {
        ProjectItem* item = findItem(*manager.currentProject(), QUuid(it.key()));
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(manager.currentProject()->objectFilePath(item), it.value());
    }
}

TEST_F(ProjectDocumentRoundTripTest, AllDocumentMetadataSurvivesEditingSavingAndReopening)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    expectModelMetadata(project, savedVersion_);
    for (auto it = pathsById_.cbegin(); it != pathsById_.cend(); ++it)
        EXPECT_EQ(readObject(it.value()), documentsById_.value(it.key()));

    project.description = QStringLiteral("Edited by the baseline reader");
    QMap<QString, QString> renamedItems;
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        const QJsonObject header = HarkenTest::projectHeader(it.value());
        if (header.value(QStringLiteral("type")) == QStringLiteral("project.root"))
            continue;
        ProjectItem* item = findItem(project, QUuid::fromString(it.key()));
        ASSERT_NE(item, nullptr);
        if (item == manager.getMasterBusItem()) continue;
        const QString name = QStringLiteral("Edited_") + item->name;
        ASSERT_TRUE(manager.renameItem(item, name));
        renamedItems.insert(it.key(), name);
    }
    ASSERT_TRUE(manager.saveProject());
    expectModelMetadata(project, studioVersion());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& reopened = *manager.currentProject();
    expectModelMetadata(reopened, studioVersion());
    EXPECT_EQ(reopened.description, QStringLiteral("Edited by the baseline reader"));
    for (auto it = renamedItems.cbegin(); it != renamedItems.cend(); ++it) {
        ProjectItem* item = findItem(reopened, QUuid::fromString(it.key()));
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->name, it.value());
    }
    const auto savedPaths = documentPathsById(projectDirectory_);
    EXPECT_EQ(savedPaths.keys(), documentsById_.keys());
    for (auto it = savedPaths.cbegin(); it != savedPaths.cend(); ++it) {
        const QJsonObject document = readObject(it.value());
        const QJsonObject header = HarkenTest::projectHeader(document);
        EXPECT_EQ(header.value(QStringLiteral("schemaVersion")).toInt(), ProjectSchema::kProjectSchemaVersion);
        EXPECT_EQ(header.value(QStringLiteral("savedStudioVersion")).toString(),
                  studioVersion());
        EXPECT_EQ(header.value(QStringLiteral("futureHeaderMetadata")),
                  HarkenTest::projectHeader(documentsById_.value(it.key()))
                      .value(QStringLiteral("futureHeaderMetadata")));
        EXPECT_EQ(HarkenTest::projectContent(document).value(QStringLiteral("extensions")),
                  extensionMetadata(it.key()));
    }
}

TEST_F(ProjectDocumentRoundTripTest, WholeProjectSavePreservesOutputDestinations)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    const QString packDirectory = directory_.filePath(QStringLiteral("Exports/Packs"));
    const QString waveformDirectory = directory_.filePath(QStringLiteral("Exports/Waveforms"));
    manager.currentProject()->packBuildOutputDir = packDirectory;
    manager.currentProject()->buildWaveformOutputDir = waveformDirectory;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    const auto* reopened = manager.currentProject();
    EXPECT_EQ(PathUtils::packBuildOutputRoot(reopened->projectPath, reopened->packBuildOutputDir), packDirectory);
    EXPECT_EQ(PathUtils::waveformBuildOutputRoot(reopened->projectPath, reopened->buildWaveformOutputDir),
              waveformDirectory);
}

TEST_F(ProjectDocumentRoundTripTest, SaveAsUsesDestinationProjectForOutputPaths)
{
    QTemporaryDir destination(QStringLiteral(HARKEN_TEMP_DIR "/project-save-as-XXXXXX"));
    ASSERT_TRUE(destination.isValid());
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    const QString packDirectory = directory_.filePath(QStringLiteral("Exports/Packs"));
    const QString destinationPath = destination.filePath(QStringLiteral("SavedAs.hsp"));
    const QByteArray original = readFile(projectPath_);
    manager.currentProject()->packBuildOutputDir = packDirectory;
    manager.currentProject()->buildWaveformOutputDir = QStringLiteral("Waveforms");
    manager.currentProject()->projectPath = destinationPath;
    manager.currentProject()->name = QStringLiteral("SavedAs");
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(readFile(projectPath_), original);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(destinationPath));
    const auto* reopened = manager.currentProject();
    EXPECT_EQ(PathUtils::packBuildOutputRoot(reopened->projectPath, reopened->packBuildOutputDir), packDirectory);
    EXPECT_EQ(PathUtils::waveformBuildOutputRoot(reopened->projectPath, reopened->buildWaveformOutputDir),
              destination.filePath(QStringLiteral("Waveforms")));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(destinationPath));
    EXPECT_EQ(PathUtils::packBuildOutputRoot(manager.currentProject()->projectPath,
        manager.currentProject()->packBuildOutputDir), packDirectory);
    ASSERT_TRUE(manager.closeProject());
}

TEST_F(ProjectDocumentRoundTripTest, StagedSaveRejectsOutputInsideFinalProjectDocuments)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    QMap<QString, QByteArray> before;
    for (const QString& path : pathsById_)
        before.insert(path, readFile(path));
    for (const auto& path : {projectDirectory_,
                            QDir(projectDirectory_).filePath(QStringLiteral("Layers/Generated"))}) {
        manager.currentProject()->buildWaveformOutputDir = path;
        EXPECT_FALSE(manager.saveProject());
        EXPECT_FALSE(ProjectTransaction::recoveryRequired(projectPath_));
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
    }
}

TEST_F(ProjectDocumentRoundTripTest, FailedWholeProjectSavePreservesVersionsAndRawDocuments)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    QMap<QString, QByteArray> before;
    for (const QString& path : pathsById_)
        before.insert(path, readFile(path));
    const AudioChannelConfig channelConfig = project.channelConfig;
    project.channelConfig = static_cast<AudioChannelConfig>(999);
    EXPECT_FALSE(manager.saveProject());
    EXPECT_FALSE(ProjectTransaction::recoveryRequired(projectPath_));
    expectModelMetadata(project, savedVersion_);
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
    {
        PreparedProject restored = ProjectManager::prepareProject(projectPath_);
        ASSERT_NE(restored.project, nullptr) << restored.error.toStdString();
        expectModelMetadata(*restored.project, savedVersion_);
        EXPECT_EQ(restored.project->channelConfig, channelConfig);
    }
    project.channelConfig = channelConfig;
    ASSERT_TRUE(manager.saveProject());
    expectModelMetadata(project, studioVersion());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    expectModelMetadata(*manager.currentProject(), studioVersion());
}

TEST_F(ProjectDocumentRoundTripTest, FailedSaveRestoresWatchingProjectDocuments)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    const AudioChannelConfig channelConfig = project.channelConfig;
    project.channelConfig = static_cast<AudioChannelConfig>(999);
    EXPECT_FALSE(manager.saveProject());
    project.channelConfig = channelConfig;
    EXPECT_FALSE(ProjectTransaction::recoveryRequired(projectPath_));

    FileWatcher* watcher = manager.fileWatcher();
    ASSERT_TRUE(watcher->isWatching());
    watcher->suppressSelfWrites(50);
    QEventLoop settle;
    QTimer::singleShot(100, &settle, &QEventLoop::quit);
    settle.exec();

    QSet<QString> pending;
    for (const QString& path : pathsById_)
        if (path.endsWith(QStringLiteral(".vp")))
            pending.insert(path);
    ASSERT_FALSE(pending.isEmpty());

    QEventLoop changes;
    QObject::connect(watcher, &FileWatcher::packExternallyModified, &changes,
                     [&](const QString& path) {
        pending.remove(path);
        if (pending.isEmpty())
            changes.quit();
    });
    const auto paths = pending.values();
    for (const QString& path : paths) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::Append)) << path.toStdString();
        ASSERT_EQ(file.write("\n"), 1);
    }
    QTimer::singleShot(3000, &changes, &QEventLoop::quit);
    changes.exec();
    EXPECT_TRUE(pending.isEmpty()) << pending.values().join(", ").toStdString();
}

TEST_F(ProjectDocumentRoundTripTest, SavePreservesStoppedFileWatcher)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    manager.fileWatcher()->stopWatching();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(manager.fileWatcher()->isWatching());

    ProjectData& project = *manager.currentProject();
    const AudioChannelConfig channelConfig = project.channelConfig;
    project.channelConfig = static_cast<AudioChannelConfig>(999);
    EXPECT_FALSE(manager.saveProject());
    project.channelConfig = channelConfig;
    EXPECT_FALSE(manager.fileWatcher()->isWatching());
    EXPECT_FALSE(ProjectTransaction::recoveryRequired(projectPath_));
}

#ifdef Q_OS_WIN
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
    const QUuid soundGuid = sound->guid;
    ASSERT_TRUE(manager.saveProject());
    const HANDLE occupied = CreateFileW(reinterpret_cast<LPCWSTR>(ordinaryPath.utf16()),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    auto release = qScopeGuard([&] { CloseHandle(occupied); });
    manager.currentProject()->description = QStringLiteral("Saved with occupied ordinary content");
    ASSERT_TRUE(manager.saveProject());
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
    EXPECT_FALSE(ProjectTransaction::recoveryRequired(projectPath_));
    for (auto it = committed.cbegin(); it != committed.cend(); ++it)
        EXPECT_EQ(readFile(it.key()), it.value()) << it.key().toStdString();
    EXPECT_EQ(readFile(ordinaryPath), QByteArray("retained bytes"));
    EXPECT_TRUE(QDir(emptyPath).exists());
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    EXPECT_EQ(manager.currentProject()->description, QStringLiteral("Pending failed save"));

    CloseHandle(occupied);
    release.dismiss();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(QDir(folderPath).exists());
    UndoRedoManager::instance().undo();
    ASSERT_NE(manager.findItemByGuid(soundGuid), nullptr);
    ASSERT_TRUE(manager.saveProject());
    EXPECT_TRUE(QDir(emptyPath).exists());
    EXPECT_EQ(readFile(ordinaryPath), QByteArray("retained bytes"));
    EXPECT_FALSE(ProjectTransaction::recoveryRequired(projectPath_));
}
#endif

TEST_F(ProjectDocumentRoundTripTest, ExternalChangesIncludeProjectDirectoriesAndOrdinaryFilesButExcludeCache)
{
    FileWatcher watcher;
    watcher.setDebounceInterval(50);
    watcher.startWatching(projectDirectory_);
    QSet<QString> changed;
    QSet<QString> expected;
    QEventLoop events;
    QObject::connect(&watcher, &FileWatcher::packExternallyModified, &events,
                     [&](const QString& path) {
        changed.insert(path);
        if ((expected - changed).isEmpty()) events.quit();
    });
    const auto waitForChanges = [&] {
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &events, &QEventLoop::quit);
        timeout.start(3000);
        events.exec();
        EXPECT_TRUE((expected - changed).isEmpty())
            << (expected - changed).values().join(", ").toStdString();
    };
    const QString folder = QDir(projectDirectory_).filePath(QStringLiteral("Actions/External"));
    const QString ordinary = QDir(folder).filePath(QStringLiteral("notes.txt"));
    expected = {folder, ordinary, projectPath_};
    ASSERT_TRUE(QDir().mkpath(folder));
    QFile note(ordinary);
    ASSERT_TRUE(note.open(QIODevice::WriteOnly));
    ASSERT_EQ(note.write("first"), 5);
    note.close();
    QFile projectFile(projectPath_);
    ASSERT_TRUE(projectFile.open(QIODevice::Append));
    ASSERT_EQ(projectFile.write("\n"), 1);
    projectFile.close();
    waitForChanges();

    changed.clear();
    expected = {ordinary};
    ASSERT_TRUE(note.open(QIODevice::Append));
    ASSERT_EQ(note.write("second"), 6);
    note.close();
    waitForChanges();

    changed.clear();
    expected = {folder, ordinary};
    ASSERT_TRUE(QFile::remove(ordinary));
    ASSERT_TRUE(QDir().rmdir(folder));
    waitForChanges();

    changed.clear();
    const QString cache = QDir(projectDirectory_).filePath(QStringLiteral(".cache/external"));
    ASSERT_TRUE(QDir().mkpath(cache));
    QFile cacheFile(QDir(cache).filePath(QStringLiteral("ignored.vp")));
    ASSERT_TRUE(cacheFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(cacheFile.write("cache"), 5);
    cacheFile.close();
    QTimer::singleShot(200, &events, &QEventLoop::quit);
    events.exec();
    EXPECT_TRUE(changed.isEmpty()) << changed.values().join(", ").toStdString();
}

TEST_F(ProjectDocumentRoundTripTest, CancelledRootSaveLeavesSavedVersionUnchanged)
{
    ProjectSerializer serializer;
    PreparedProject prepared = ProjectManager::prepareProject(projectPath_);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    ProjectData& project = *prepared.project;
    const QByteArray before = readFile(projectPath_);
    const QJsonObject header = project.documentHeader;
    project.description = QStringLiteral("Prepared then cancelled");
    ASSERT_TRUE(serializer.prepareProjectSave(projectPath_, project));
    EXPECT_EQ(project.documentHeader, header);
    EXPECT_EQ(readFile(projectPath_), before);
    serializer.cancelProjectSave();
    EXPECT_EQ(project.documentHeader, header);
    EXPECT_EQ(readFile(projectPath_), before);
    ASSERT_TRUE(serializer.saveProject(projectPath_, project));
    EXPECT_EQ(project.documentHeader.value(QStringLiteral("savedStudioVersion"))
                  .toString(), studioVersion());
    EXPECT_EQ(HarkenTest::projectHeader(readObject(projectPath_)), project.documentHeader);
}

TEST_F(ProjectDocumentRoundTripTest, LegacyAttenuationAuthoringIsRejectedWithoutWriting)
{
    QString soundId;
    QString attenuationId;
    for (auto document = documentsById_.cbegin(); document != documentsById_.cend(); ++document) {
        const QString type = HarkenTest::projectContent(document.value())
            .value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("Sound")) soundId = document.key();
        if (type == QStringLiteral("Attenuation")) attenuationId = document.key();
    }
    ASSERT_FALSE(soundId.isEmpty());
    ASSERT_FALSE(attenuationId.isEmpty());
    const QString soundPath = pathsById_.value(soundId);
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        QJsonObject document = documentsById_.value(soundId);
        QJsonObject content = HarkenTest::projectContent(document);
        QJsonObject components = content.value(QStringLiteral("components")).toObject();
        QJsonObject spatial = components.value(QStringLiteral("spatial")).toObject();
        spatial.insert(QStringLiteral("attenuationGuid"), attenuationId);
        spatial.insert(QStringLiteral("attenuation"), QStringLiteral("Space"));
        components.insert(QStringLiteral("spatial"), spatial);
        content.insert(QStringLiteral("components"), components);
        if (scenario != 0) {
            content.insert(QStringLiteral("playableProperties"), QJsonObject{
                {QStringLiteral("presence"), QStringLiteral("0000000000002000")},
                {QStringLiteral("spatial.attenuationRef"), scenario == 2
                    ? QJsonValue(QJsonValue::Null)
                    : QJsonValue(QJsonObject{{QStringLiteral("id"), attenuationId}})},
            });
        }
        document.insert(QStringLiteral("content"), content);
        writeObject(soundPath, document);
        const QByteArray before = readFile(soundPath);
        PreparedProject prepared = ProjectManager::prepareProject(projectPath_);
        EXPECT_EQ(readFile(soundPath), before);
        EXPECT_EQ(prepared.project, nullptr);
        EXPECT_FALSE(prepared.error.isEmpty());
    }
}

TEST_F(ProjectDocumentRoundTripTest, UnknownBehaviorAndMissingSavedVersionAreRejected)
{
    QString layerPath;
    for (auto it = documentsById_.cbegin(); it != documentsById_.cend(); ++it) {
        if (HarkenTest::projectContent(it.value()).value(QStringLiteral("type"))
            == QStringLiteral("Sound")) {
            layerPath = pathsById_.value(it.key());
            break;
        }
    }
    ASSERT_FALSE(layerPath.isEmpty());
    const QJsonObject originalLayer = readObject(layerPath);
    const QJsonObject originalRoot = readObject(projectPath_);
    for (int scenario = 0; scenario < 6; ++scenario) {
        SCOPED_TRACE(scenario);
        const bool layerScenario = scenario < 2 || scenario >= 4;
        QJsonObject document = layerScenario ? originalLayer : originalRoot;
        QJsonObject content = HarkenTest::projectContent(document);
        if (scenario == 0) {
            content.insert(QStringLiteral("futurePlaybackBehavior"), true);
        } else if (scenario == 1) {
            QJsonObject components = content.value(QStringLiteral("components")).toObject();
            components.insert(QStringLiteral("FutureComponent"), QJsonObject{});
            content.insert(QStringLiteral("components"), components);
        } else if (scenario == 2) {
            content.insert(QStringLiteral("conversionFailurePolicy"),
                           QStringLiteral("FuturePolicy"));
        } else if (scenario == 3) {
            QJsonObject header = HarkenTest::projectHeader(document);
            header.remove(QStringLiteral("savedStudioVersion"));
            document.insert(QStringLiteral("harkenDocument"), header);
        } else {
            QJsonObject properties =
                content.value(QStringLiteral("playableProperties")).toObject();
            properties.insert(scenario == 4 ? QStringLiteral("streamingMode")
                                           : QStringLiteral("spatial.attenuationCurveType"),
                              999);
            content.insert(QStringLiteral("playableProperties"), properties);
        }
        document.insert(QStringLiteral("content"), content);
        const QString path = layerScenario ? layerPath : projectPath_;
        writeObject(path, document);
        const QByteArray before = readFile(path);
        PreparedProject prepared = ProjectManager::prepareProject(projectPath_);
        EXPECT_EQ(prepared.project, nullptr);
        EXPECT_FALSE(prepared.error.isEmpty());
        EXPECT_EQ(readFile(path), before);
        writeObject(path, layerScenario ? originalLayer : originalRoot);
    }
}

TEST_F(ProjectDocumentRoundTripTest, UnknownNestedBehaviorIsRejectedBeforeLossyParsing)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData& project = *manager.currentProject();
    ProjectItem* container = manager.createItem(
        nullptr, QStringLiteral("Selector"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ProjectItem* sound = manager.createItem(
        container, QStringLiteral("SelectedSound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_FALSE(project.variables.empty());
    const QUuid variable = project.variables.front().rootItem->guid;
    auto* selection = container->comp<VSelection>();
    ASSERT_NE(selection, nullptr);
    ASSERT_TRUE(selection->setField(QStringLiteral("mode"), static_cast<int>(SelectionMode::Blend)));
    MixingChildDef child{sound->guid, {{0.0f, 0.0f}, {0.2f, 1.0f}, {1.0f, 1.0f}}, {}};
    child.normalize();
    ASSERT_TRUE(selection->selections.shared.setMixing(MixingDef{{{variable, {child}}}}));
    selection->transitions.rules.front().exitTiming = TransitionExitTiming::Immediate;

    ProjectItem* piece = manager.createItem(
        nullptr, QStringLiteral("MusicalPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    auto* timeline = piece->comp<VPiece>();
    ASSERT_NE(timeline, nullptr);
    timeline->pieceTime = PieceTimeSpec::musical();
    ASSERT_TRUE(manager.saveProject());
    const auto paths = documentPathsById(projectDirectory_);
    const QString containerPath = paths.value(container->guid.toString(QUuid::WithoutBraces));
    const QString piecePath = paths.value(piece->guid.toString(QUuid::WithoutBraces));
    ASSERT_FALSE(containerPath.isEmpty());
    ASSERT_FALSE(piecePath.isEmpty());
    ASSERT_TRUE(manager.closeProject());
    {
        const PreparedProject baseline = ProjectManager::prepareProject(projectPath_);
        ASSERT_NE(baseline.project, nullptr) << baseline.error.toStdString();
    }

    const QString selectionPrefix = QStringLiteral("content/components/")
        + ComponentCatalog::componentName(ComponentType::Selection);
    const QString timePrefix = QStringLiteral("content/components/")
        + ComponentCatalog::componentName(ComponentType::Piece)
        + QStringLiteral("/pieceTime/");
    const std::vector<std::pair<QString, QString>> cases{
        {containerPath, selectionPrefix + QStringLiteral("/selections/shared/entries/0")},
        {containerPath, selectionPrefix + QStringLiteral("/selections/shared/entries/0/curves/0")},
        {containerPath, selectionPrefix + QStringLiteral("/transitions/rules/0/sourceFadeOut/curve/0")},
        {containerPath, selectionPrefix + QStringLiteral("/transitions/rules/0/sourceFadeOut/handles/0")},
        {piecePath, timePrefix + QStringLiteral("tempoMap/0")},
        {piecePath, timePrefix + QStringLiteral("meterMap/0")},
    };
    for (const auto& [path, jsonPath] : cases) {
        SCOPED_TRACE(jsonPath.toStdString());
        const QJsonObject original = readObject(path);
        const QStringList keys = jsonPath.split(QLatin1Char('/'));
        std::vector<QJsonValue> ancestors{original};
        for (const QString& key : keys) {
            const QJsonValue parent = ancestors.back();
            QJsonValue value;
            if (parent.isArray()) {
                const QJsonArray array = parent.toArray();
                ASSERT_LT(key.toInt(), array.size());
                value = array.at(key.toInt());
            } else {
                value = parent.toObject().value(key);
            }
            ASSERT_FALSE(value.isUndefined());
            ancestors.push_back(value);
        }
        QJsonObject extra = ancestors.back().toObject();
        extra.insert(QStringLiteral("futureBehavior"), true);
        QJsonValue changed = extra;
        for (qsizetype i = keys.size(); i > 0; --i) {
            const QJsonValue parent = ancestors[static_cast<size_t>(i - 1)];
            if (parent.isArray()) {
                QJsonArray array = parent.toArray();
                array[keys.at(i - 1).toInt()] = changed;
                changed = array;
            } else {
                QJsonObject object = parent.toObject();
                object.insert(keys.at(i - 1), changed);
                changed = object;
            }
        }
        writeObject(path, changed.toObject());
        const QByteArray before = readFile(path);
        const PreparedProject rejected = ProjectManager::prepareProject(projectPath_);
        EXPECT_EQ(rejected.project, nullptr);
        EXPECT_FALSE(rejected.error.isEmpty());
        EXPECT_EQ(readFile(path), before);
        writeObject(path, original);
    }
}

}
