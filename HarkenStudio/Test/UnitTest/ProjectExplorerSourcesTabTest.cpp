#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/ImportAssetsCommand.h"
#include "AssetPipeline/ImportSoundSourcesCommand.h"
#include "AssetPipeline/MediaPool.h"
#include "Core/FileWatcher.h"
#include "Core/SoundSourceSemantics.h"
#include "AssetPipeline/SourceFileSystem.h"
#include "Components/VSound.h"
#include "ProjectManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Core/ToastManager.h"
#include "UndoCommands.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"
#include "Widgets/ProjectExplorer/ExplorerSection.h"
#include "Widgets/ProjectExplorer/SourcesTreeWidget.h"
#include "Widgets/DragDropOverlay.h"
#include "Widgets/Shared/RefIdMime.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QInputDialog>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <vector>

#include <memory>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

using namespace VNX;

namespace {

class ProjectTreeMimeProbe : public ProjectTreeWidget
{
public:
    using ProjectTreeWidget::mimeData;
};

Qt::DropAction dropMime(QWidget* target, const QPoint& position, const QMimeData* mime)
{
    const auto actions = Qt::CopyAction | Qt::MoveAction;
    QDragEnterEvent enter(position, actions, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    if (!enter.isAccepted()) return Qt::IgnoreAction;
    QDragMoveEvent move(position, actions, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    if (!move.isAccepted()) {
        QDragLeaveEvent leave;
        QApplication::sendEvent(target, &leave);
        return Qt::IgnoreAction;
    }
    QDropEvent drop(position, actions, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    return drop.isAccepted() ? drop.dropAction() : Qt::IgnoreAction;
}

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QTreeWidgetItem* findRowIn(QTreeWidgetItem* row, const QString& label)
{
    if (row->text(0) == label || row->text(0) == label + QStringLiteral(" *")) return row;
    for (int i = 0; i < row->childCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(row->child(i), label))
            return hit;
    }
    return nullptr;
}

QTreeWidgetItem* findRow(QTreeWidget* tree, const QString& label)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(tree->topLevelItem(i), label))
            return hit;
    }
    return nullptr;
}

AssetInfo makeAsset(uint32_t sourceId, const QString& relativePath)
{
    AssetInfo info;
    info.sourceID = sourceId;
    info.relativePath = relativePath;
    info.metadata.originalFormat = QStringLiteral("WAV");
    info.metadata.durationSec = 1.25;
    info.metadata.sampleRate = 48000;
    info.metadata.channels = 2;
    info.absolutePath = QDir(PathUtils::getAudioSourcesDir(AssetRegistry::instance().projectRoot()))
                            .filePath(relativePath);
    QDir().mkpath(QFileInfo(info.absolutePath).absolutePath());
    QFile file(info.absolutePath);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("audio");
    return info;
}

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeWavF32(const QString& path, const std::vector<float>& samples)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t channels = 1;
    const uint32_t frames = static_cast<uint32_t>(samples.size());
    const uint32_t dataBytes = frames * static_cast<uint32_t>(sizeof(float));
    const uint16_t blockAlign = channels * sizeof(float);
    const uint32_t fmtSize = 18;
    const uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()),
                 static_cast<qsizetype>(dataBytes));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

class ProjectExplorerSourcesTabTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        QSettings().remove(QStringLiteral("Explorer"));
        explorer_ = std::make_unique<ProjectExplorer>();
        ProjectConfig config;
        config.name = QStringLiteral("SourcesTabProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
    }

    void TearDown() override
    {
        explorer_.reset();
        UndoRedoManager::instance().clear();
        if (ProjectManager::instance().isProjectOpen())
            ProjectManager::instance().closeProject();
        AssetRegistry::instance().clear();
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<ProjectExplorer> explorer_;
};

}

TEST_F(ProjectExplorerSourcesTabTest, RestoringMissingFilesPreservesSharedIdentityAcrossUndoAndReload)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    ASSERT_TRUE(QDir().mkdir(SourceFileSystem::absolutePath(QStringLiteral("FX"))));
    const QString input = tempDir_.filePath(QStringLiteral("Hit.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(input)});
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    ASSERT_TRUE(tree->importDroppedAudio(&mime, QStringLiteral("FX")));
    const AssetInfo original = *registry.getAssetByPath(QStringLiteral("FX/Hit.wav"));
    auto* sound = manager.createRootAudioItem(QStringLiteral("Independent Name"),
        original.relativePath, original.sourceID);
    ASSERT_NE(sound, nullptr);
    const auto guid = sound->guid;
    const auto authored = sound->comp<VSound>()->sources;
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());

    QString incomingHash;
    for (float amplitude : {0.5f, 0.25f}) {
        ASSERT_TRUE(QFile::remove(original.absolutePath));
        writeWavF32(input, {0.0f, amplitude, -amplitude, 0.0f});
        incomingHash = AssetImporter::computeContentHash(input);
        int dialogs = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                ++dialogs;
                dialog->reject();
            }
        });
        timer.start(0);
        const bool imported = tree->importDroppedAudio(&mime, QStringLiteral("FX"));
        timer.stop();
        ASSERT_TRUE(imported);
        EXPECT_EQ(dialogs, 0);
        EXPECT_EQ(registry.count(), 1);
        EXPECT_EQ(registry.getAsset(original.sourceID)->contentHash, incomingHash);
        EXPECT_EQ(registry.getAssetByPath(original.relativePath)->sourceID, original.sourceID);
        EXPECT_EQ(sound->comp<VSound>()->sources, authored);
        ASSERT_TRUE(QFile::remove(input));
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        EXPECT_FALSE(QFileInfo(original.absolutePath).exists());
        EXPECT_TRUE(registry.contains(original.sourceID));
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        EXPECT_EQ(AssetImporter::computeContentHash(original.absolutePath), incomingHash);
        EXPECT_EQ(sound->comp<VSound>()->sources, authored);
    }

    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    sound = manager.findItemByGuid(guid);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->comp<VSound>()->sources, authored);
    ASSERT_NE(registry.getAsset(original.sourceID), nullptr);
    EXPECT_EQ(registry.getAsset(original.sourceID)->contentHash, incomingHash);
    EXPECT_TRUE(registry.sourceFileExists(original.sourceID));
    EXPECT_EQ(registry.count(), 1);
}

TEST_F(ProjectExplorerSourcesTabTest, ExistingPhysicalFileKeepsIdentityAcrossImportUndoAndDiscovery)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    MediaPool::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Existing.wav"));
    const QString destination = SourceFileSystem::absolutePath(QStringLiteral("Existing.wav"));
    writeWavF32(destination, {0.0f, 0.25f, -0.25f, 0.0f});
    const QString before = AssetImporter::computeContentHash(destination);
    writeWavF32(input, {0.0f, 0.75f, -0.75f, 0.0f});
    const QString after = AssetImporter::computeContentHash(input);
    auto* imported = ImportAssetsCommand::pushImportSources(
        {input}, {}, {{QStringLiteral("Existing.wav"), input}});
    ASSERT_EQ(imported->successCount(), 1);
    const uint32_t id = imported->importedIDs().first();
    auto* sound = ImportAssetsCommand::pushImportAtRoot({destination});
    ASSERT_EQ(sound->successCount(), 1);
    const QUuid guid = sound->importedItemGuids().first();
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(AssetImporter::computeContentHash(destination), before);
    manager.fileWatcher()->refreshSources();
    ASSERT_NE(registry.findAssetByPath(destination), nullptr);
    EXPECT_EQ(registry.findAssetByPath(destination)->sourceID, id);
    ASSERT_TRUE(QFile::remove(input));
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(AssetImporter::computeContentHash(destination), after);
    ASSERT_NE(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(guid)->sourceID, id);
    EXPECT_TRUE(registry.sourceFileExists(id));
    EXPECT_EQ(registry.count(), 1);
}

TEST_F(ProjectExplorerSourcesTabTest, RedoConflictsPreserveExternalFilesAndDoNotRestoreDanglingSounds)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    MediaPool::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Conflict.wav"));
    writeWavF32(input, {0.0f, 0.25f, -0.25f, 0.0f});
    auto* imported = ImportAssetsCommand::pushImportAtRoot({input});
    ASSERT_EQ(imported->successCount(), 1);
    const uint32_t id = imported->importedIDs().first();
    const QUuid guid = imported->importedItemGuids().first();
    const QString destination = registry.getAsset(id)->absolutePath;
    ASSERT_TRUE(undo.undo());
    writeWavF32(destination, {0.0f, 0.75f, -0.75f, 0.0f});
    const QString externalHash = AssetImporter::computeContentHash(destination);
    manager.fileWatcher()->refreshSources();
    ASSERT_NE(registry.findAssetByPath(destination), nullptr);
    const uint32_t externalId = registry.findAssetByPath(destination)->sourceID;
    ASSERT_NE(externalId, id);
    const int index = undo.undoStack()->index();
    EXPECT_FALSE(undo.redo());
    EXPECT_EQ(undo.undoStack()->index(), index);
    EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(AssetImporter::computeContentHash(destination), externalHash);
    EXPECT_EQ(registry.findAssetByPath(destination)->sourceID, externalId);
    EXPECT_FALSE(registry.contains(id));
}

TEST_F(ProjectExplorerSourcesTabTest, ReuseValidatesCurrentAudioAndRefreshesItsMetadata)
{
    auto& registry = AssetRegistry::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Reuse.wav"));
    writeWavF32(input, {0.0f, 0.25f, -0.25f, 0.0f});
    auto* imported = ImportAssetsCommand::pushImportAtRoot({input});
    ASSERT_EQ(imported->successCount(), 1);
    const uint32_t id = imported->importedIDs().first();
    const QString destination = registry.getAsset(id)->absolutePath;
    {
        QFile file(destination);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("invalid audio");
    }
    const int history = UndoRedoManager::instance().undoStack()->count();
    auto* rejected = ImportAssetsCommand::pushImportAtRoot({destination});
    EXPECT_EQ(rejected->successCount(), 0);
    EXPECT_EQ(rejected->failCount(), 1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), history);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(destination)});
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    EXPECT_FALSE(tree->importDroppedAudio(&mime));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), history);
    writeWavF32(destination, {0.0f, 0.75f, -0.75f, 0.0f, 0.5f, -0.5f});
    EXPECT_TRUE(tree->importDroppedAudio(&mime));
    EXPECT_EQ(registry.getAsset(id)->metadata.totalSamples, 6u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), history);
    auto* reused = ImportAssetsCommand::pushImportAtRoot({destination});
    ASSERT_EQ(reused->successCount(), 1);
    EXPECT_EQ(reused->importedIDs().first(), id);
    EXPECT_EQ(registry.getAsset(id)->metadata.totalSamples, 6u);
    EXPECT_EQ(registry.getAsset(id)->contentHash, AssetImporter::computeContentHash(destination));
    EXPECT_EQ(registry.count(), 1);
}

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, ImportUndoRollsBackFileLocksAcrossDestinationsAndKeepsRedoCopies)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    MediaPool::instance();
    for (const QString& mode : {QStringLiteral("Sources"), QStringLiteral("Root"),
                               QStringLiteral("Layer"), QStringLiteral("Sound")}) {
        SCOPED_TRACE(mode.toStdString());
        auto* layer = manager.createItem(nullptr, mode, ProjectItem::Type::Layer);
        auto* target = manager.createItem(layer, QStringLiteral("Target"), ProjectItem::Type::Sound);
        auto* binding = findFileBindingForItem(*manager.currentProject(), layer);
        QStringList paths;
        for (const auto& name : {QStringLiteral("A"), QStringLiteral("B")}) {
            const QString path = tempDir_.filePath(mode + name + QStringLiteral(".wav"));
            writeWavF32(path, {0.0f, 0.25f, -0.25f, 0.0f});
            paths.append(path);
        }
        QList<uint32_t> ids;
        if (mode == QLatin1String("Sound"))
            ids = ImportSoundSourcesCommand::pushImport(paths, binding, target)->importedIDs();
        else if (mode == QLatin1String("Sources"))
            ids = ImportAssetsCommand::pushImportSources(paths)->importedIDs();
        else if (mode == QLatin1String("Root"))
            ids = ImportAssetsCommand::pushImportAtRoot(paths)->importedIDs();
        else
            ids = ImportAssetsCommand::pushImport(paths, binding, layer)->importedIDs();
        ASSERT_EQ(ids.size(), 2);
        const AssetInfo first = *registry.getAsset(ids.front());
        const AssetInfo second = *registry.getAsset(ids.back());
        const int index = undo.undoStack()->index();
        const auto authored = target->comp<VSound>()->sources;
        {
            HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(first.absolutePath.utf16()),
                GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            ASSERT_NE(handle, INVALID_HANDLE_VALUE);
            const auto release = qScopeGuard([&] { CloseHandle(handle); });
            EXPECT_FALSE(undo.undo());
            EXPECT_EQ(undo.undoStack()->index(), index);
            EXPECT_EQ(target->comp<VSound>()->sources, authored);
            manager.fileWatcher()->refreshSources();
            for (const auto& asset : {first, second}) {
                ASSERT_NE(registry.findAssetByPath(asset.absolutePath), nullptr);
                EXPECT_EQ(registry.findAssetByPath(asset.absolutePath)->sourceID, asset.sourceID);
                EXPECT_EQ(AssetImporter::computeContentHash(asset.absolutePath), asset.contentHash);
            }
        }
        for (const QString& path : paths) ASSERT_TRUE(QFile::remove(path));
        ASSERT_TRUE(undo.undo());
        manager.fileWatcher()->refreshSources();
        for (uint32_t id : ids) EXPECT_FALSE(registry.contains(id));
        ASSERT_TRUE(undo.redo());
        for (const auto& asset : {first, second}) {
            EXPECT_TRUE(registry.sourceFileExists(asset.sourceID));
            EXPECT_EQ(AssetImporter::computeContentHash(asset.absolutePath), asset.contentHash);
        }
        EXPECT_EQ(target->comp<VSound>()->sources, authored);
    }
}
#endif

TEST_F(ProjectExplorerSourcesTabTest, ListsRegisteredAssetsAndNestedFolders)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(12, QStringLiteral("fx/whoosh.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(13, QStringLiteral("fx/impacts/hit.wav"))));

    QTreeWidget* tree = explorer_->sourcesTree();
    ASSERT_NE(tree, nullptr);
    QTreeWidgetItem* kick = findRow(tree, QStringLiteral("kick.wav"));
    QTreeWidgetItem* fx = findRow(tree, QStringLiteral("fx"));
    QTreeWidgetItem* whoosh = findRow(tree, QStringLiteral("whoosh.wav"));
    QTreeWidgetItem* impacts = findRow(tree, QStringLiteral("impacts"));
    QTreeWidgetItem* hit = findRow(tree, QStringLiteral("hit.wav"));
    ASSERT_NE(kick, nullptr);
    ASSERT_NE(fx, nullptr);
    ASSERT_NE(whoosh, nullptr);
    ASSERT_NE(impacts, nullptr);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(kick->parent(), nullptr);
    EXPECT_EQ(whoosh->parent(), fx);
    EXPECT_EQ(impacts->parent(), fx);
    EXPECT_EQ(hit->parent(), impacts);
    EXPECT_TRUE(kick->toolTip(0).contains(QStringLiteral("kick.wav")));
    EXPECT_TRUE(kick->toolTip(0).contains(QStringLiteral("48000 Hz")));
}

TEST_F(ProjectExplorerSourcesTabTest, UnregisterRemovesRowAndSearchHidesUnmatched)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(21, QStringLiteral("rain.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(22, QStringLiteral("wind.wav"))));

    QTreeWidget* tree = explorer_->sourcesTree();
    ASSERT_NE(findRow(tree, QStringLiteral("rain.wav")), nullptr);
    ASSERT_NE(findRow(tree, QStringLiteral("wind.wav")), nullptr);

    AssetRegistry::instance().unregisterAsset(21);
    EXPECT_EQ(findRow(tree, QStringLiteral("rain.wav")), nullptr);
    EXPECT_NE(findRow(tree, QStringLiteral("wind.wav")), nullptr);

    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(23, QStringLiteral("storm.wav"))));
    auto& manager = ProjectManager::instance();
    auto* windItem = manager.createItem(nullptr, QStringLiteral("Wind Layer"), ProjectItem::Type::Layer);
    auto* stormItem = manager.createItem(nullptr, QStringLiteral("Storm Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(windItem, nullptr);
    ASSERT_NE(stormItem, nullptr);
    ASSERT_TRUE(explorer_->selectProjectItem(windItem));
    auto* windLayer = explorer_->treeWidget()->currentItem();
    ASSERT_TRUE(explorer_->selectProjectItem(stormItem));
    auto* stormLayer = explorer_->treeWidget()->currentItem();
    ASSERT_NE(windLayer, nullptr);
    ASSERT_NE(stormLayer, nullptr);
    auto* search = explorer_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(search, nullptr);
    EXPECT_EQ(explorer_->findChildren<QLineEdit*>().size(), 1);
    search->setText(QStringLiteral("wind"));
    QTreeWidgetItem* wind = findRow(tree, QStringLiteral("wind.wav"));
    QTreeWidgetItem* storm = findRow(tree, QStringLiteral("storm.wav"));
    ASSERT_NE(wind, nullptr);
    ASSERT_NE(storm, nullptr);
    EXPECT_FALSE(wind->isHidden());
    EXPECT_TRUE(storm->isHidden());
    EXPECT_FALSE(windLayer->isHidden());
    EXPECT_TRUE(stormLayer->isHidden());

    explorer_->sourcesSection()->setExpanded(false);
    search->setText(QStringLiteral("storm"));
    explorer_->sourcesSection()->setExpanded(true);
    EXPECT_TRUE(wind->isHidden());
    EXPECT_FALSE(storm->isHidden());
    EXPECT_TRUE(windLayer->isHidden());
    EXPECT_FALSE(stormLayer->isHidden());
    search->clear();
    EXPECT_FALSE(wind->isHidden());
    EXPECT_FALSE(storm->isHidden());
    EXPECT_FALSE(windLayer->isHidden());
    EXPECT_FALSE(stormLayer->isHidden());
}

TEST_F(ProjectExplorerSourcesTabTest, ClosingProjectClearsTheList)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(31, QStringLiteral("loop.wav"))));
    QTreeWidget* tree = explorer_->sourcesTree();
    ASSERT_NE(findRow(tree, QStringLiteral("loop.wav")), nullptr);

    ProjectManager::instance().closeProject();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
}

TEST_F(ProjectExplorerSourcesTabTest, PingSourceRevealsCollapsedSourceWithoutChangingSelection)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(41, QStringLiteral("keep.wav"))));
    for (uint32_t i = 0; i < 40; ++i) {
        ASSERT_TRUE(AssetRegistry::instance().registerAsset(
            makeAsset(100 + i, QStringLiteral("fx/impacts/hit%1.wav").arg(i, 2, 10, QLatin1Char('0')))));
    }
    auto* selectedObject = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Selected Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(selectedObject, nullptr);
    ASSERT_TRUE(explorer_->selectProjectItem(selectedObject));
    QTreeWidget* tree = explorer_->sourcesTree();
    auto* selectedSource = findRow(tree, QStringLiteral("keep.wav"));
    auto* target = findRow(tree, QStringLiteral("hit39.wav"));
    auto* impacts = findRow(tree, QStringLiteral("impacts"));
    auto* fx = findRow(tree, QStringLiteral("fx"));
    ASSERT_NE(selectedSource, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(impacts, nullptr);
    ASSERT_NE(fx, nullptr);
    tree->setCurrentItem(selectedSource);
    impacts->setExpanded(false);
    fx->setExpanded(false);
    explorer_->sourcesSection()->setExpanded(false);
    explorer_->resize(360, 400);
    explorer_->show();
    QApplication::processEvents();

    int objectSelectionChanges = 0;
    int sourceSelectionChanges = 0;
    QObject connections;
    QObject::connect(explorer_.get(), &ProjectExplorer::objectSelectionChanged, &connections,
        [&](const QList<ProjectItem*>&) { ++objectSelectionChanges; });
    QObject::connect(tree, &QTreeWidget::itemSelectionChanged, &connections,
        [&]() { ++sourceSelectionChanges; });

    ASSERT_TRUE(explorer_->pingSource(139));
    EXPECT_TRUE(explorer_->sourcesSection()->isExpanded());
    EXPECT_TRUE(fx->isExpanded());
    EXPECT_TRUE(impacts->isExpanded());
    const QRect targetRect = tree->visualItemRect(target);
    EXPECT_FALSE(targetRect.isEmpty());
    EXPECT_TRUE(tree->viewport()->rect().contains(targetRect.center()));
    EXPECT_EQ(tree->currentItem(), selectedSource);
    EXPECT_EQ(tree->selectedItems(), QList<QTreeWidgetItem*>{selectedSource});
    EXPECT_EQ(explorer_->selectedItem(), selectedObject);
    EXPECT_EQ(objectSelectionChanges, 0);
    EXPECT_EQ(sourceSelectionChanges, 0);
}

TEST_F(ProjectExplorerSourcesTabTest, PingSourceClearsSearchOnlyWhenTargetIsFiltered)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(41, QStringLiteral("keep.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(42, QStringLiteral("fx/impacts/hit.wav"))));
    QTreeWidget* tree = explorer_->sourcesTree();
    auto* target = findRow(tree, QStringLiteral("hit.wav"));
    ASSERT_NE(target, nullptr);
    auto* search = explorer_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("keep"));
    ASSERT_TRUE(target->isHidden());
    EXPECT_TRUE(explorer_->pingSource(41));
    EXPECT_EQ(search->text(), QStringLiteral("keep"));

    EXPECT_TRUE(explorer_->pingSource(42));
    EXPECT_TRUE(search->text().isEmpty());
    for (auto* row = target; row; row = row->parent())
        EXPECT_FALSE(row->isHidden());
}

TEST_F(ProjectExplorerSourcesTabTest, DoubleClickingSourceRequestsSourceEditor)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(41, QStringLiteral("fx/whoosh.wav"))));

    QTreeWidget* tree = explorer_->sourcesTree();
    QTreeWidgetItem* file = findRow(tree, QStringLiteral("whoosh.wav"));
    QTreeWidgetItem* folder = findRow(tree, QStringLiteral("fx"));
    ASSERT_NE(file, nullptr);
    ASSERT_NE(folder, nullptr);
    ASSERT_TRUE(folder->isExpanded());

    QList<uint32_t> requested;
    QObject::connect(explorer_.get(), &ProjectExplorer::sourceEditorRequested,
                     [&](uint32_t sourceId) { requested.append(sourceId); });

    emit tree->itemDoubleClicked(folder, 0);
    EXPECT_TRUE(requested.isEmpty());
    EXPECT_FALSE(folder->isExpanded());

    emit tree->itemDoubleClicked(file, 0);
    ASSERT_EQ(requested.size(), 1);
    EXPECT_EQ(requested.front(), 41u);
}

TEST_F(ProjectExplorerSourcesTabTest, SourceRowsDragOutAudioAssetMime)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(51, QStringLiteral("fx/whoosh.wav"))));

    QTreeWidget* tree = explorer_->sourcesTree();
    ASSERT_NE(tree, nullptr);
    EXPECT_TRUE(tree->dragEnabled());
    EXPECT_EQ(tree->dragDropMode(), QAbstractItemView::DragDrop);
    EXPECT_TRUE(tree->acceptDrops());

    QTreeWidgetItem* file = findRow(tree, QStringLiteral("whoosh.wav"));
    QTreeWidgetItem* folder = findRow(tree, QStringLiteral("fx"));
    ASSERT_NE(file, nullptr);
    ASSERT_NE(folder, nullptr);

    auto* sourcesTree = static_cast<SourcesTreeWidget*>(tree);
    std::unique_ptr<QMimeData> fileMime(sourcesTree->mimeData({file}));
    ASSERT_NE(fileMime, nullptr);
    EXPECT_TRUE(RefIdMime::has(fileMime.get(), RefDomain::AudioAsset));
    EXPECT_EQ(RefIdMime::readSingle(fileMime.get(), RefDomain::AudioAsset), 51u);
    EXPECT_FALSE(RefIdMime::has(fileMime.get(), RefDomain::ProjectObject));

    std::unique_ptr<QMimeData> folderMime(sourcesTree->mimeData({folder}));
    ASSERT_NE(folderMime, nullptr);
    EXPECT_FALSE(RefIdMime::has(folderMime.get(), RefDomain::AudioAsset));
}

TEST_F(ProjectExplorerSourcesTabTest, DroppingWavsImportsOnlySourcesAndRoundTripsUndo)
{
    auto& undo = UndoRedoManager::instance();
    undo.clear();
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());
    const auto bindingCount = project->allFileBindings().size();

    QList<QUrl> urls;
    for (int i = 0; i < 4; ++i) {
        const QString wavPath = tempDir_.filePath(
            QStringLiteral("SourcesTabDrop%1.wav").arg(i));
        const float sample = 0.1f * static_cast<float>(i + 1);
        writeWavF32(wavPath, {0.0f, sample, -sample, 0.0f});
        urls.append(QUrl::fromLocalFile(wavPath));
    }

    QMimeData mime;
    mime.setUrls(urls);

    QTreeWidget* tree = explorer_->sourcesTree();
    ASSERT_NE(tree, nullptr);
    auto* sourcesTree = static_cast<SourcesTreeWidget*>(tree);
    ASSERT_TRUE(sourcesTree->importDroppedAudio(&mime));

    auto& registry = AssetRegistry::instance();
    const QList<AssetInfo> imported = registry.snapshot();
    ASSERT_EQ(imported.size(), 4);
    EXPECT_EQ(tree->topLevelItemCount(), 4);
    EXPECT_TRUE(project->layers.empty());
    EXPECT_EQ(project->allFileBindings().size(), bindingCount);
    for (const AssetInfo& asset : imported) {
        EXPECT_TRUE(asset.ownerObjectGuid.isEmpty());
        EXPECT_TRUE(QFile::exists(asset.absolutePath));
        QTreeWidgetItem* row = findRow(tree, asset.relativePath);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->data(0, kSourceIdRole).toUInt(), asset.sourceID);
    }

    ASSERT_TRUE(undo.canUndo());
    EXPECT_FALSE(undo.isClean());
    undo.undo();
    EXPECT_EQ(registry.count(), 0);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    EXPECT_TRUE(project->layers.empty());
    EXPECT_EQ(project->allFileBindings().size(), bindingCount);
    EXPECT_TRUE(undo.isClean());
    for (const AssetInfo& asset : imported)
        EXPECT_FALSE(QFile::exists(asset.absolutePath));
    for (const QUrl& url : urls)
        EXPECT_TRUE(QFile::exists(url.toLocalFile()));

    ASSERT_TRUE(undo.canRedo());
    undo.redo();
    EXPECT_EQ(registry.count(), 4);
    EXPECT_EQ(tree->topLevelItemCount(), 4);
    EXPECT_TRUE(project->layers.empty());
    EXPECT_EQ(project->allFileBindings().size(), bindingCount);
    for (const AssetInfo& asset : imported) {
        const AssetInfo* restored = registry.getAsset(asset.sourceID);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->absolutePath, asset.absolutePath);
        EXPECT_EQ(restored->contentHash, asset.contentHash);
        EXPECT_TRUE(restored->ownerObjectGuid.isEmpty());
        EXPECT_TRUE(QFile::exists(asset.absolutePath));
        QTreeWidgetItem* row = findRow(tree, asset.relativePath);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->data(0, kSourceIdRole).toUInt(), asset.sourceID);
    }
}

TEST_F(ProjectExplorerSourcesTabTest, DroppedSourceSurvivesSaveAndReopenWithoutSound)
{
    const QString wavName = QString(QChar(0x97F3)) + QChar(0x6548)
        + QStringLiteral(".wav");
    const QString wavPath = tempDir_.filePath(wavName);
    writeWavF32(wavPath, {0.0f, 0.5f, -0.5f, 0.0f});

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(wavPath)});
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    ASSERT_TRUE(tree->importDroppedAudio(&mime));
    auto& registry = AssetRegistry::instance();
    const AssetInfo* asset = registry.getAssetByPath(wavName);
    ASSERT_NE(asset, nullptr);
    const uint32_t sourceID = asset->sourceID;
    const QString importedPath = asset->absolutePath;

    auto& manager = ProjectManager::instance();
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.currentProject()->layers.empty());
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));

    EXPECT_TRUE(manager.currentProject()->layers.empty());
    ASSERT_EQ(registry.count(), 1);
    asset = registry.getAsset(sourceID);
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->absolutePath, importedPath);
    EXPECT_TRUE(asset->ownerObjectGuid.isEmpty());
    EXPECT_TRUE(QFile::exists(importedPath));
    QTreeWidgetItem* row = findRow(tree, wavName);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->data(0, kSourceIdRole).toUInt(), sourceID);
}

TEST_F(ProjectExplorerSourcesTabTest, DroppingDuplicateAudioPreservesExistingSoundAndSource)
{
    const QString firstPath = tempDir_.filePath(QStringLiteral("Existing.wav"));
    writeWavF32(firstPath, {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData firstMime;
    firstMime.setUrls({QUrl::fromLocalFile(firstPath)});
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    ASSERT_TRUE(tree->importDroppedAudio(&firstMime));

    auto& registry = AssetRegistry::instance();
    const AssetInfo* asset = registry.getAssetByPath(QStringLiteral("Existing.wav"));
    ASSERT_NE(asset, nullptr);
    const uint32_t sourceID = asset->sourceID;
    const QString importedPath = asset->absolutePath;
    auto& manager = ProjectManager::instance();
    ProjectItem* sound = manager.createRootAudioItem(
        QStringLiteral("Existing Sound"), asset->relativePath, sourceID);
    ASSERT_NE(sound, nullptr);
    const QUuid soundGuid = sound->guid;

    ASSERT_TRUE(QDir(tempDir_.path()).mkdir(QStringLiteral("Reimport")));
    const QString duplicatePath = tempDir_.filePath(QStringLiteral("Reimport/Existing.wav"));
    ASSERT_TRUE(QFile::copy(firstPath, duplicatePath));
    const QString newPath = tempDir_.filePath(QStringLiteral("New.wav"));
    writeWavF32(newPath, {0.0f, 0.25f, -0.25f, 0.0f});
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(duplicatePath), QUrl::fromLocalFile(newPath)});
    ASSERT_TRUE(tree->importDroppedAudio(&mime));

    EXPECT_EQ(registry.count(), 2);
    EXPECT_EQ(tree->topLevelItemCount(), 2);
    ASSERT_EQ(manager.currentProject()->layers.size(), 1u);
    EXPECT_EQ(manager.findItemByGuid(soundGuid), sound);

    UndoRedoManager::instance().undo();
    EXPECT_EQ(registry.count(), 1);
    EXPECT_TRUE(registry.contains(sourceID));
    EXPECT_TRUE(QFile::exists(importedPath));
    EXPECT_NE(findRow(tree, QStringLiteral("Existing.wav")), nullptr);
    EXPECT_EQ(findRow(tree, QStringLiteral("New.wav")), nullptr);
    ASSERT_EQ(manager.currentProject()->layers.size(), 1u);
    EXPECT_EQ(manager.findItemByGuid(soundGuid), sound);
}

TEST_F(ProjectExplorerSourcesTabTest, PartialReplacementPreservesIndependentReferencesAcrossUndoAndSave)
{
    auto& registry = AssetRegistry::instance();
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    const QString first = tempDir_.filePath(QStringLiteral("First.wav"));
    const QString second = tempDir_.filePath(QStringLiteral("Second.wav"));
    writeWavF32(first, {0.0f, 0.5f, -0.5f, 0.0f});
    ASSERT_TRUE(QFile::copy(first, second));
    QMimeData initial;
    initial.setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
    ASSERT_TRUE(tree->importDroppedAudio(&initial));
    ASSERT_EQ(registry.count(), 2);
    const auto* firstAsset = registry.getAssetByPath(QStringLiteral("First.wav"));
    const auto* secondAsset = registry.getAssetByPath(QStringLiteral("Second.wav"));
    ASSERT_NE(firstAsset, nullptr);
    ASSERT_NE(secondAsset, nullptr);
    const uint32_t firstID = firstAsset->sourceID;
    const uint32_t secondID = secondAsset->sourceID;
    EXPECT_NE(firstID, secondID);
    const QString beforeHash = firstAsset->contentHash;
    QList<QUuid> soundGuids;
    for (const QString& name : {QStringLiteral("First use"), QStringLiteral("Shared use")}) {
        auto* sound = manager.createRootAudioItem(name, QStringLiteral("First.wav"), firstID);
        ASSERT_NE(sound, nullptr);
        soundGuids.append(sound->guid);
    }
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    const int beforeIndex = undo.undoStack()->index();
    ASSERT_TRUE(tree->importDroppedAudio(&initial));
    EXPECT_EQ(undo.undoStack()->index(), beforeIndex);
    EXPECT_TRUE(undo.isClean());

    writeWavF32(first, {0.0f, 0.25f, -0.25f, 0.25f, -0.25f, 0.0f});
    writeWavF32(second, {0.0f, 0.75f, -0.75f, 0.0f});
    const QString added = tempDir_.filePath(QStringLiteral("Added.wav"));
    ASSERT_TRUE(QFile::copy(first, added));
    const QString afterHash = AssetImporter::computeContentHash(first);
    QMimeData replacement;
    replacement.setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second), QUrl::fromLocalFile(added)});
    bool confirmed = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        auto* conflicts = dialog->findChild<QTreeWidget*>(QStringLiteral("sourceImportConflicts"));
        ASSERT_NE(conflicts, nullptr);
        ASSERT_EQ(conflicts->topLevelItemCount(), 2);
        for (int i = 0; i < conflicts->topLevelItemCount(); ++i) {
            auto* row = conflicts->topLevelItem(i);
            EXPECT_EQ(row->checkState(0), Qt::Checked);
            if (row->text(0) == QStringLiteral("Second.wav"))
                row->setCheckState(0, Qt::Unchecked);
        }
        confirmed = true;
        dialog->accept();
    });
    ASSERT_TRUE(tree->importDroppedAudio(&replacement));
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(undo.undoStack()->index(), beforeIndex + 1);
    ASSERT_EQ(registry.count(), 3);
    const uint32_t addedID = registry.getAssetByPath(QStringLiteral("Added.wav"))->sourceID;
    EXPECT_NE(addedID, firstID);
    EXPECT_NE(addedID, secondID);
    ASSERT_TRUE(QFile::remove(first));
    const auto expectRevision = [&](const QString& hash, uint64_t frames) {
        ASSERT_NE(registry.getAsset(firstID), nullptr);
        ASSERT_NE(registry.getAsset(secondID), nullptr);
        EXPECT_EQ(registry.getAsset(firstID)->relativePath, QStringLiteral("First.wav"));
        EXPECT_EQ(registry.getAsset(firstID)->contentHash, hash);
        EXPECT_EQ(registry.getAsset(firstID)->metadata.totalSamples, frames);
        EXPECT_EQ(AssetImporter::computeContentHash(registry.getAsset(firstID)->absolutePath), hash);
        EXPECT_EQ(registry.getAsset(secondID)->contentHash, beforeHash);
        EXPECT_EQ(AssetImporter::computeContentHash(registry.getAsset(secondID)->absolutePath), beforeHash);
        for (const auto& guid : soundGuids) {
            const auto* sound = manager.findItemByGuid(guid);
            ASSERT_NE(sound, nullptr);
            EXPECT_EQ(sound->sourceID, firstID);
            EXPECT_EQ(sound->comp<VSound>()->sources.plain.entries.front().sourceId, firstID);
        }
    };
    expectRevision(afterHash, 6);
    ASSERT_TRUE(undo.undo());
    expectRevision(beforeHash, 4);
    EXPECT_FALSE(registry.contains(addedID));
    ASSERT_TRUE(undo.redo());
    expectRevision(afterHash, 6);
    EXPECT_TRUE(registry.contains(addedID));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(undo.undo());
    expectRevision(beforeHash, 4);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    expectRevision(afterHash, 6);
    EXPECT_TRUE(registry.contains(addedID));
}

TEST_F(ProjectExplorerSourcesTabTest, CancelledOrInvalidReplacementPreservesFilesAndHistory)
{
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    const QString input = tempDir_.filePath(QStringLiteral("Existing.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData initial;
    initial.setUrls({QUrl::fromLocalFile(input)});
    ASSERT_TRUE(tree->importDroppedAudio(&initial));
    ASSERT_TRUE(ProjectManager::instance().saveProject());
    const auto asset = *registry.getAssetByPath(QStringLiteral("Existing.wav"));
    const int index = undo.undoStack()->index();
    const QString added = tempDir_.filePath(QStringLiteral("Added.wav"));
    writeWavF32(added, {0.0f, 0.25f, -0.25f, 0.0f});
    {
        QFile file(input);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("invalid audio"), 13);
    }
    QMimeData replacement;
    replacement.setUrls({QUrl::fromLocalFile(input), QUrl::fromLocalFile(added)});
    for (bool accept : {false, true}) {
        QTimer::singleShot(0, [accept]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            if (accept) dialog->accept();
            else dialog->reject();
        });
        EXPECT_FALSE(tree->importDroppedAudio(&replacement));
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_TRUE(undo.isClean());
        EXPECT_EQ(registry.count(), 1);
        EXPECT_EQ(AssetImporter::computeContentHash(asset.absolutePath), asset.contentHash);
        EXPECT_EQ(registry.getAsset(asset.sourceID)->contentHash, asset.contentHash);
    }
}

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, ReplacementReplayFailurePreservesContentReferencesAndHistory)
{
    auto& registry = AssetRegistry::instance();
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    const QStringList names{QStringLiteral("A.wav"), QStringLiteral("B.wav")};
    QStringList files;
    QStringList oldHashes;
    QStringList newHashes;
    QList<uint32_t> ids;
    QMap<QString, QString> replacements;
    for (const QString& name : names) {
        const QString file = tempDir_.filePath(name);
        writeWavF32(file, {0.0f, 0.5f, -0.5f, 0.0f});
        auto* command = ImportAssetsCommand::pushImportSources({file});
        ASSERT_EQ(command->successCount(), 1);
        ids.append(command->importedIDs().first());
        files.append(file);
        oldHashes.append(AssetImporter::computeContentHash(file));
        replacements.insert(name, file);
        writeWavF32(file, {0.0f, 0.25f, -0.25f, 0.0f});
        newHashes.append(AssetImporter::computeContentHash(file));
    }
    ASSERT_TRUE(manager.saveProject());
    ASSERT_EQ(ImportAssetsCommand::pushImportSources(files, {}, replacements)->successCount(), 2);
    for (bool redo : {false, true}) {
        const int index = undo.undoStack()->index();
        const QString blocked = SourceFileSystem::absolutePath(redo ? names.last() : names.first());
        const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(blocked.utf16()),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        {
            const auto release = qScopeGuard([&]() { CloseHandle(handle); });
            EXPECT_FALSE(redo ? undo.redo() : undo.undo());
        }
        EXPECT_EQ(undo.undoStack()->index(), index);
        for (int i = 0; i < names.size(); ++i) {
            const QString hash = redo ? oldHashes[i] : newHashes[i];
            EXPECT_EQ(registry.getAssetByPath(names[i])->sourceID, ids[i]);
            EXPECT_EQ(registry.getAsset(ids[i])->contentHash, hash);
            EXPECT_EQ(AssetImporter::computeContentHash(SourceFileSystem::absolutePath(names[i])), hash);
        }
        ASSERT_TRUE(redo ? undo.redo() : undo.undo());
    }
}
#endif

TEST_F(ProjectExplorerSourcesTabTest, FailedDropDoesNotChangeProjectOrUndoStack)
{
    auto& undo = UndoRedoManager::instance();
    undo.clear();
    const QString wavPath = tempDir_.filePath(QStringLiteral("Corrupt.wav"));
    {
        QFile file(wavPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("RIFF"), 4);
    }

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(wavPath)});
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    EXPECT_FALSE(tree->importDroppedAudio(&mime));
    EXPECT_EQ(AssetRegistry::instance().count(), 0);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    EXPECT_TRUE(ProjectManager::instance().currentProject()->layers.empty());
    EXPECT_FALSE(undo.canUndo());
    EXPECT_TRUE(undo.isClean());
    EXPECT_TRUE(QFile::exists(wavPath));
}

TEST_F(ProjectExplorerSourcesTabTest, InternalAudioAssetMimeDoesNotImport)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    QMimeData mime;
    RefIdMime::write(&mime, RefDomain::AudioAsset, {51u});

    auto* sourcesTree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    ASSERT_NE(sourcesTree, nullptr);
    EXPECT_FALSE(sourcesTree->importDroppedAudio(&mime));
    EXPECT_TRUE(project->layers.empty());
}

TEST_F(ProjectExplorerSourcesTabTest, SectionsStackResizeAndCollapseIndependently)
{
    explorer_->resize(340, 800);
    explorer_->show();
    QApplication::processEvents();
    auto* assets = explorer_->assetsSection();
    auto* sources = explorer_->sourcesSection();
    auto* splitter = explorer_->findChild<QSplitter*>(QStringLiteral("projectExplorerSplitter"));
    ASSERT_NE(splitter, nullptr);
    auto* content = explorer_->findChild<QFrame*>(QStringLiteral("projectExplorerContent"));
    auto* search = explorer_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(content, nullptr);
    ASSERT_NE(search, nullptr);
    EXPECT_EQ(content->frameWidth(), 1);
    EXPECT_TRUE(content->isAncestorOf(assets));
    EXPECT_TRUE(content->isAncestorOf(sources));
    EXPECT_FALSE(content->isAncestorOf(search));
    EXPECT_LT(search->geometry().bottom(), content->y());
    EXPECT_EQ(search->width(), content->width());
    EXPECT_TRUE(content->contentsRect().contains(splitter->geometry()));
    EXPECT_EQ(splitter->orientation(), Qt::Vertical);
    EXPECT_EQ(splitter->handleWidth(), 0);
    EXPECT_TRUE(assets->isExpanded());
    EXPECT_TRUE(sources->isExpanded());
    EXPECT_TRUE(explorer_->treeWidget()->isVisible());
    EXPECT_TRUE(explorer_->sourcesTree()->isVisible());
    EXPECT_LT(assets->mapTo(explorer_.get(), QPoint()).y(), sources->mapTo(explorer_.get(), QPoint()).y());
    EXPECT_NE(explorer_->treeWidget()->verticalScrollBar(), explorer_->sourcesTree()->verticalScrollBar());

    splitter->setSizes({500, 250});
    QApplication::processEvents();
    auto* handle = splitter->handle(1);
    ASSERT_TRUE(handle->isEnabled());
    ASSERT_GT(handle->height(), 0);
    EXPECT_EQ(sources->y(), assets->geometry().bottom() + 1);
    const auto beforeDrag = splitter->sizes();
    const QPoint pressPosition = handle->rect().center();
    const QPoint globalPress = handle->mapToGlobal(pressPosition);
    QMouseEvent press(QEvent::MouseButtonPress, pressPosition, globalPress,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &press);
    const QPoint delta(0, -60);
    QMouseEvent move(QEvent::MouseMove, pressPosition + delta, globalPress + delta,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(globalPress + delta),
                        globalPress + delta, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &release);
    QApplication::processEvents();
    const auto expanded = splitter->sizes();
    EXPECT_LT(expanded[0], beforeDrag[0]);
    EXPECT_GT(expanded[1], beforeDrag[1]);
    ASSERT_GT(expanded[0], expanded[1]);
    assets->setExpanded(false);
    QApplication::processEvents();
    EXPECT_TRUE(sources->isExpanded());
    EXPECT_FALSE(explorer_->treeWidget()->isVisible());
    EXPECT_TRUE(explorer_->sourcesTree()->isVisible());
    EXPECT_GT(sources->height(), expanded[1]);

    sources->setExpanded(false);
    QApplication::processEvents();
    EXPECT_FALSE(assets->content()->isVisible());
    EXPECT_FALSE(sources->content()->isVisible());
    EXPECT_TRUE(assets->findChild<QToolButton*>(QStringLiteral("explorerSectionToggle"))->isVisible());
    EXPECT_TRUE(sources->findChild<QToolButton*>(QStringLiteral("explorerSectionToggle"))->isVisible());
    EXPECT_LE(sources->y(), assets->height() + splitter->handleWidth() + 1);
    EXPECT_TRUE(search->isVisible());
    EXPECT_FALSE(handle->isEnabled());
    EXPECT_TRUE(content->contentsRect().contains(splitter->geometry()));

    assets->setExpanded(true);
    sources->setExpanded(true);
    QApplication::processEvents();
    EXPECT_NEAR(static_cast<double>(splitter->sizes()[0]) / splitter->sizes()[1],
                static_cast<double>(expanded[0]) / expanded[1], 0.05);
    explorer_->resize(200, 320);
    QApplication::processEvents();
    EXPECT_GT(explorer_->treeWidget()->viewport()->height(), 0);
    EXPECT_GT(explorer_->sourcesTree()->viewport()->height(), 0);
    EXPECT_LE(sources->geometry().bottom(), splitter->height());
    EXPECT_EQ(explorer_->width(), 200);
    EXPECT_TRUE(content->contentsRect().contains(splitter->geometry()));
}

TEST_F(ProjectExplorerSourcesTabTest, SourceHeaderCreatesRootFolderAndTreeMenuCreatesSubfolder)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("FX")));
    auto* tree = explorer_->sourcesTree();
    tree->setCurrentItem(findRow(tree, QStringLiteral("FX")));
    explorer_->resize(340, 800);
    explorer_->show();
    explorer_->sourcesSection()->setExpanded(false);
    QApplication::processEvents();
    EXPECT_EQ(explorer_->findChild<QToolButton*>(QStringLiteral("newSourceFolderButton")), nullptr);

    const auto createFromMenu = [&](QWidget* target, const QPoint& position) {
        bool menuOpened = false;
        bool promptOpened = false;
        QTimer menuDriver;
        menuDriver.setSingleShot(true);
        QTimer promptDriver;
        promptDriver.setSingleShot(true);
        QObject::connect(&promptDriver, &QTimer::timeout, [&]() {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                promptOpened = true;
                dialog->reject();
            }
        });
        QObject::connect(&menuDriver, &QTimer::timeout, [&]() {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            ASSERT_NE(menu, nullptr);
            menuOpened = true;
            ASSERT_FALSE(menu->actions().isEmpty());
            auto* action = menu->actions().first();
            EXPECT_EQ(action->text(), QStringLiteral("New Folder"));
            EXPECT_TRUE(action->isEnabled());
            menu->setActiveAction(action);
            promptDriver.start(0);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &press);
        });
        menuDriver.start(0);
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, target->mapToGlobal(position));
        QApplication::sendEvent(target, &event);
        EXPECT_TRUE(menuOpened);
        EXPECT_FALSE(promptOpened);
    };

    auto* toggle = explorer_->sourcesSection()->findChild<QToolButton*>(QStringLiteral("explorerSectionToggle"));
    ASSERT_NE(toggle, nullptr);
    createFromMenu(toggle, toggle->rect().center());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("New Folder"))).exists());
    EXPECT_FALSE(QDir(SourceFileSystem::absolutePath(QStringLiteral("FX/New Folder"))).exists());
    EXPECT_TRUE(explorer_->sourcesSection()->isExpanded());
    auto* rootFolder = findRow(tree, QStringLiteral("New Folder"));
    ASSERT_NE(rootFolder, nullptr);
    EXPECT_EQ(rootFolder->parent(), nullptr);

    auto* parent = findRow(tree, QStringLiteral("FX"));
    ASSERT_NE(parent, nullptr);
    tree->setCurrentItem(parent);
    QApplication::processEvents();
    createFromMenu(tree->viewport(), tree->visualItemRect(parent).center());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("FX/New Folder"))).exists());
    createFromMenu(toggle, toggle->rect().center());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("New Folder_01"))).exists());
}

TEST_F(ProjectExplorerSourcesTabTest, RefreshPreservesFolderExpansionAndSourceSelection)
{
    auto& registry = AssetRegistry::instance();
    ASSERT_TRUE(registry.registerAsset(makeAsset(61, QStringLiteral("fx/hit.wav"))));
    ASSERT_TRUE(registry.registerAsset(makeAsset(62, QStringLiteral("loop.wav"))));
    auto* tree = explorer_->sourcesTree();
    findRow(tree, QStringLiteral("fx"))->setExpanded(false);
    tree->setCurrentItem(findRow(tree, QStringLiteral("loop.wav")));
    ASSERT_TRUE(registry.registerAsset(makeAsset(63, QStringLiteral("another.wav"))));
    EXPECT_FALSE(findRow(tree, QStringLiteral("fx"))->isExpanded());
    EXPECT_TRUE(findRow(tree, QStringLiteral("loop.wav"))->isSelected());
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->data(0, kSourceIdRole).toUInt(), 62u);
}

TEST_F(ProjectExplorerSourcesTabTest, SectionStateAndHeightsSurvivePanelRecreation)
{
    explorer_->resize(340, 800);
    explorer_->show();
    QApplication::processEvents();
    auto* splitter = explorer_->findChild<QSplitter*>(QStringLiteral("projectExplorerSplitter"));
    ASSERT_NE(splitter, nullptr);
    splitter->setSizes({500, 250});
    emit splitter->splitterMoved(500, 1);
    const auto sizes = splitter->sizes();
    explorer_->sourcesSection()->setExpanded(false);
    explorer_.reset();
    explorer_ = std::make_unique<ProjectExplorer>();
    explorer_->resize(340, 800);
    explorer_->show();
    QApplication::processEvents();
    EXPECT_TRUE(explorer_->assetsSection()->isExpanded());
    EXPECT_FALSE(explorer_->sourcesSection()->isExpanded());
    explorer_->sourcesSection()->setExpanded(true);
    QApplication::processEvents();
    splitter = explorer_->findChild<QSplitter*>(QStringLiteral("projectExplorerSplitter"));
    ASSERT_NE(splitter, nullptr);
    EXPECT_NEAR(static_cast<double>(splitter->sizes()[0]) / splitter->sizes()[1],
                static_cast<double>(sizes[0]) / sizes[1], 0.05);
}

TEST_F(ProjectExplorerSourcesTabTest, ClosingProjectClearsPhysicalFoldersAndDisablesFileOperations)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Saved")));
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    const QString physicalFolder = SourceFileSystem::absolutePath(QStringLiteral("Saved"));
    ASSERT_TRUE(manager.closeProject());
    EXPECT_TRUE(QDir(physicalFolder).exists());
    EXPECT_EQ(explorer_->sourcesTree()->topLevelItemCount(), 0);
    bool menuOpened = false;
    QTimer menuDriver;
    menuDriver.setSingleShot(true);
    QObject::connect(&menuDriver, &QTimer::timeout, [&]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        menuOpened = true;
        ASSERT_FALSE(menu->actions().isEmpty());
        EXPECT_FALSE(menu->actions().first()->isEnabled());
        EXPECT_FALSE(menu->actions().last()->isEnabled());
        menu->close();
    });
    menuDriver.start(0);
    static_cast<SourcesTreeWidget*>(explorer_->sourcesTree())->showRootContextMenu(QPoint(10, 10));
    EXPECT_TRUE(menuOpened);
    EXPECT_FALSE(SourceFileSystem::createFolder({}, QStringLiteral("Unexpected")));
    const QString input = tempDir_.filePath(QStringLiteral("Closed.wav"));
    writeWavF32(input, {0.0f, 0.25f, -0.25f, 0.0f});
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(input)});
    EXPECT_FALSE(static_cast<SourcesTreeWidget*>(explorer_->sourcesTree())->importDroppedAudio(&mime));
    EXPECT_EQ(AssetRegistry::instance().count(), 0);
}

TEST_F(ProjectExplorerSourcesTabTest, PhysicalEmptyFoldersRoundTripUndoAndProjectReopen)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("FX")));
    ASSERT_TRUE(SourceFileSystem::createFolder(QStringLiteral("FX"), QStringLiteral("Empty")));
    const QString path = SourceFileSystem::absolutePath(QStringLiteral("FX/Empty"));
    ASSERT_TRUE(QDir(path).exists());
    auto* tree = explorer_->sourcesTree();
    ASSERT_NE(findRow(tree, QStringLiteral("Empty")), nullptr);
    EXPECT_EQ(findRow(tree, QStringLiteral("Empty"))->parent(), findRow(tree, QStringLiteral("FX")));
    UndoRedoManager::instance().undo();
    EXPECT_FALSE(QDir(path).exists());
    EXPECT_EQ(findRow(tree, QStringLiteral("Empty")), nullptr);
    UndoRedoManager::instance().redo();
    EXPECT_TRUE(QDir(path).exists());
    auto& manager = ProjectManager::instance();
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_TRUE(QDir(path).exists());
    EXPECT_NE(findRow(tree, QStringLiteral("Empty")), nullptr);
    EXPECT_TRUE(manager.currentProject()->layers.empty());
}

TEST_F(ProjectExplorerSourcesTabTest, SourcesDragIntoLayerAndSoundReusesAudio)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("FX")));
    const QString input = tempDir_.filePath(QStringLiteral("Hit.wav"));
    writeWavF32(input, {0.0f, 0.25f, -0.25f, 0.0f});
    QMimeData external;
    external.setUrls({QUrl::fromLocalFile(input)});
    auto* sources = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    ASSERT_TRUE(sources->importDroppedAudio(&external, QStringLiteral("FX")));
    auto& registry = AssetRegistry::instance();
    const auto* asset = registry.getAssetByPath(QStringLiteral("FX/Hit.wav"));
    ASSERT_NE(asset, nullptr);
    const uint32_t id = asset->sourceID;
    const QString copied = asset->absolutePath;
    ASSERT_TRUE(QFile::exists(copied));
    auto* sourceRow = findRow(sources, QStringLiteral("Hit.wav"));
    ASSERT_NE(sourceRow, nullptr);
    std::unique_ptr<QMimeData> mime(sources->mimeData({sourceRow}));
    ASSERT_TRUE(mime->hasUrls());
    EXPECT_EQ(mime->urls().first().toLocalFile(), copied);
    EXPECT_EQ(RefIdMime::readSingle(mime.get(), RefDomain::AudioAsset), id);

    auto& manager = ProjectManager::instance();
    auto* layer = manager.createItem(nullptr, QStringLiteral("Drop Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    explorer_->resize(360, 800);
    explorer_->show();
    auto* assets = explorer_->treeWidget();
    const auto dropOn = [&](ProjectItem* target) {
        ASSERT_TRUE(explorer_->selectProjectItem(target));
        assets->expandAll();
        QApplication::processEvents();
        const QPoint position = assets->visualItemRect(assets->currentItem()).center();
        QDragEnterEvent enter(position, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(assets->viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        QDragMoveEvent move(position, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(assets->viewport(), &move);
        ASSERT_TRUE(move.isAccepted());
        QDropEvent drop(position, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(assets->viewport(), &drop);
        ASSERT_TRUE(drop.isAccepted());
    };
    dropOn(layer);
    ASSERT_EQ(layer->children.size(), 1u);
    auto* sound = layer->children.front().get();
    ASSERT_EQ(sound->type, ProjectItem::Type::Sound);
    EXPECT_EQ(sound->sourceID, id);
    EXPECT_EQ(sound->sourcePath, QStringLiteral("FX/Hit.wav"));
    ASSERT_NE(sound->comp<VSound>(), nullptr);
    const auto sourceCount = sound->comp<VSound>()->sources.plain.entries.size();
    dropOn(sound);
    EXPECT_EQ(sound->comp<VSound>()->sources.plain.entries.size(), sourceCount + 1);
    EXPECT_EQ(layer->children.size(), 1u);
    EXPECT_EQ(registry.count(), 1);
    EXPECT_TRUE(QFile::exists(copied));
}

TEST_F(ProjectExplorerSourcesTabTest, LayerBucketDropsCreateSoundsWithoutAudioPathFolders)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("FX")));
    const QString directInput = tempDir_.filePath(QStringLiteral("Direct.wav"));
    const QString registeredInput = tempDir_.filePath(QStringLiteral("Registered.wav"));
    writeWavF32(directInput, {0.0f, 0.25f, -0.25f, 0.0f});
    writeWavF32(registeredInput, {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData external;
    external.setUrls({QUrl::fromLocalFile(directInput)});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({registeredInput}, QStringLiteral("FX"))->successCount(), 1);
    auto* sources = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    auto* sourceRow = findRow(sources, QStringLiteral("Registered.wav"));
    ASSERT_NE(sourceRow, nullptr);
    std::unique_ptr<QMimeData> internal(sources->mimeData({sourceRow}));

    auto& manager = ProjectManager::instance();
    const QString projectPath = manager.currentProject()->projectPath;
    auto* assets = explorer_->treeWidget();
    explorer_->resize(360, 800);
    explorer_->show();
    QApplication::processEvents();
    const auto dropOnBucket = [&](const QMimeData* mime) {
        auto* bucket = findRow(assets, QStringLiteral("Layer"));
        ASSERT_NE(bucket, nullptr);
        const QPoint position = assets->visualItemRect(bucket).center();
        QDragEnterEvent enter(position, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(assets->viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        QDragMoveEvent move(position, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(assets->viewport(), &move);
        ASSERT_TRUE(move.isAccepted());
        QDropEvent drop(position, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(assets->viewport(), &drop);
        ASSERT_TRUE(drop.isAccepted());
    };
    dropOnBucket(&external);
    dropOnBucket(internal.get());
    ASSERT_EQ(manager.currentProject()->layers.size(), 2u);
    QList<QUuid> guids;
    for (const auto& binding : manager.currentProject()->layers) {
        ASSERT_NE(binding.rootItem, nullptr);
        guids.append(binding.rootItem->guid);
    }
    const auto expectDirectSounds = [&]() {
        auto* project = manager.currentProject();
        ASSERT_NE(project, nullptr);
        EXPECT_TRUE(project->folders.empty());
        EXPECT_EQ(project->layers.size(), 2u);
        auto* bucket = findRow(assets, QStringLiteral("Layer"));
        ASSERT_NE(bucket, nullptr);
        EXPECT_EQ(bucket->childCount(), 2);
        for (const QUuid& guid : guids) {
            auto* sound = manager.findItemByGuid(guid);
            ASSERT_NE(sound, nullptr);
            EXPECT_EQ(sound->type, ProjectItem::Type::Sound);
            EXPECT_EQ(sound->parent, nullptr);
            EXPECT_TRUE(sound->children.empty());
            ASSERT_TRUE(explorer_->selectProjectItem(sound));
            EXPECT_EQ(assets->currentItem()->parent(), bucket);
            const auto* audio = AssetRegistry::instance().getAsset(sound->sourceID);
            ASSERT_NE(audio, nullptr);
            EXPECT_EQ(sound->sourcePath, audio->relativePath);
        }
    };
    expectDirectSounds();
    auto& undo = UndoRedoManager::instance();
    undo.undo();
    undo.undo();
    EXPECT_TRUE(manager.currentProject()->layers.empty());
    EXPECT_TRUE(manager.currentProject()->folders.empty());
    undo.redo();
    undo.redo();
    expectDirectSounds();
    explorer_->onProjectOpened(manager.currentProject());
    expectDirectSounds();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    expectDirectSounds();
}

TEST_F(ProjectExplorerSourcesTabTest, SourceRowsMatchObjectRowGeometryAndSelection)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(makeAsset(81, QStringLiteral("hit.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(makeAsset(82, QStringLiteral("FX/impact.wav"))));
    explorer_->resize(340, 800);
    explorer_->show();
    ASSERT_TRUE(explorer_->selectProjectItem(ProjectManager::instance().getMasterBusItem()));
    auto* objects = explorer_->treeWidget();
    auto* sources = explorer_->sourcesTree();
    auto* sourceRow = findRow(sources, QStringLiteral("hit.wav"));
    ASSERT_NE(sourceRow, nullptr);
    sources->setCurrentItem(sourceRow);
    QApplication::processEvents();

    const QRect objectRect = objects->visualItemRect(objects->currentItem());
    const QRect sourceRect = sources->visualItemRect(sourceRow);
    EXPECT_EQ(sourceRect.height(), objectRect.height());
    EXPECT_EQ(sourceRect.left(), objectRect.left());
    EXPECT_EQ(sources->font(), objects->font());
    EXPECT_EQ(sources->iconSize(), objects->iconSize());
    const auto& colors = ThemeManager::instance().colors();
    for (QTreeWidget* tree : {static_cast<QTreeWidget*>(objects), sources}) {
        QImage image(tree->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        tree->viewport()->render(&painter);
        painter.end();
        const int y = tree->visualItemRect(tree->currentItem()).center().y();
        EXPECT_EQ(image.pixelColor(0, y), colors.accent);
        EXPECT_EQ(image.pixelColor(1, y), colors.accent);
        EXPECT_EQ(image.pixelColor(2, y), colors.bgSelected);
    }
    const auto sourceIcon = ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AudioSource)).icon.pixmap(QSize(20, 20)).toImage();
    const auto soundIcon = ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::ProjectItemSound)).icon.pixmap(QSize(20, 20)).toImage();
    EXPECT_EQ(sourceRow->icon(0).pixmap(QSize(20, 20)).toImage(), sourceIcon);
    EXPECT_NE(sourceIcon, soundIcon);

    auto* folder = findRow(sources, QStringLiteral("FX"));
    ASSERT_NE(folder, nullptr);
    sources->setCurrentItem(folder);
    QApplication::processEvents();
    const QRect folderRect = sources->visualItemRect(folder);
    QImage image(sources->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    sources->viewport()->render(&painter);
    painter.end();
    bool hasChevron = false;
    for (int y = folderRect.top() + 2; y < folderRect.bottom() - 2; ++y) {
        for (int x = 3; x < folderRect.left(); ++x)
            hasChevron |= image.pixelColor(x, y) != colors.bgSelected;
    }
    EXPECT_TRUE(hasChevron);
}

TEST_F(ProjectExplorerSourcesTabTest, AssetAndSourceFoldersShareExpansionIconsAndHideEmptyChevrons)
{
    auto& manager = ProjectManager::instance();
    auto* folder = manager.createFolder(
        nullptr, QStringLiteral("Folder"), QStringLiteral("Actions"));
    ASSERT_NE(folder, nullptr);
    auto* empty = manager.createFolder(
        nullptr, QStringLiteral("Empty"), QStringLiteral("Actions"));
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(manager.createItem(nullptr, QStringLiteral("Trigger"), ProjectItem::Type::Action,
                                 QStringLiteral("Folder")), nullptr);
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Folder")));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Empty")));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(makeAsset(83, QStringLiteral("Folder/hit.wav"))));
    explorer_->onProjectOpened(manager.currentProject());
    explorer_->resize(360, 900);
    explorer_->show();
    auto* objects = explorer_->treeWidget();
    auto* sources = explorer_->sourcesTree();
    ASSERT_TRUE(explorer_->selectFolder(folder));
    auto* objectFolder = objects->currentItem();
    ASSERT_TRUE(explorer_->selectFolder(empty));
    auto* emptyObjectFolder = objects->currentItem();
    auto* sourceFolder = findRow(sources, QStringLiteral("Folder"));
    auto* emptySourceFolder = findRow(sources, QStringLiteral("Empty"));
    ASSERT_NE(sourceFolder, nullptr);
    ASSERT_NE(emptySourceFolder, nullptr);
    const auto render = [](QTreeWidget* tree, QTreeWidgetItem* row) {
        tree->setCurrentItem(row);
        tree->scrollToItem(row);
        QApplication::processEvents();
        QImage image(tree->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        tree->viewport()->render(&painter);
        painter.end();
        return image;
    };
    const auto renderIcon = [&](QTreeWidget* tree, QTreeWidgetItem* row, bool expanded) {
        row->setExpanded(expanded);
        const QImage image = render(tree, row);
        const QRect rect = tree->visualItemRect(row);
        return image.copy(QRect(rect.left(), rect.top() + (rect.height() - 20) / 2, 20, 20));
    };
    const auto objectClosed = renderIcon(objects, objectFolder, false);
    const auto sourceClosed = renderIcon(sources, sourceFolder, false);
    const auto objectOpen = renderIcon(objects, objectFolder, true);
    const auto sourceOpen = renderIcon(sources, sourceFolder, true);
    EXPECT_EQ(objectClosed, sourceClosed);
    EXPECT_EQ(objectOpen, sourceOpen);
    EXPECT_NE(objectClosed, objectOpen);
    EXPECT_NE(sourceClosed, sourceOpen);

    const auto expectNoChevron = [&](QTreeWidget* tree, QTreeWidgetItem* row) {
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->childCount(), 0);
        EXPECT_FALSE(tree->model()->hasChildren(tree->indexFromItem(row)));
        const QImage image = render(tree, row);
        const QRect rect = tree->visualItemRect(row);
        const auto background = ThemeManager::instance().colors().bgSelected;
        for (int y = rect.top() + 2; y < rect.bottom() - 2; ++y) {
            for (int x = 3; x < rect.left(); ++x)
                EXPECT_EQ(image.pixelColor(x, y), background) << x << "," << y;
        }
    };
    expectNoChevron(objects, emptyObjectFolder);
    expectNoChevron(sources, emptySourceFolder);
    AssetRegistry::instance().unregisterAsset(83);
    expectNoChevron(sources, findRow(sources, QStringLiteral("Folder")));
}

TEST_F(ProjectExplorerSourcesTabTest, ExternalDragPreviewMatchesInternalSourceIcons)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("FX")));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(makeAsset(84, QStringLiteral("hit.wav"))));
    auto* sources = explorer_->sourcesTree();
    DragDropOverlay overlay(explorer_.get());
    QDrag internal(sources);
    for (const QString& label : {QStringLiteral("hit.wav"), QStringLiteral("FX")}) {
        auto* row = findRow(sources, label);
        ASSERT_NE(row, nullptr);
        internal.setPixmap(row->icon(0).pixmap(sources->iconSize()));
        overlay.showOverlay({SourceFileSystem::absolutePath(label)}, Qt::CopyAction, false);
        ASSERT_TRUE(overlay.isVisible());
        EXPECT_EQ(overlay.size(), internal.pixmap().deviceIndependentSize().toSize());
        EXPECT_EQ(overlay.pos(), QCursor::pos() - internal.hotSpot());
        EXPECT_TRUE(overlay.testAttribute(Qt::WA_TransparentForMouseEvents));
        EXPECT_TRUE(overlay.testAttribute(Qt::WA_ShowWithoutActivating));
        EXPECT_TRUE(overlay.windowFlags().testFlag(Qt::WindowTransparentForInput));
        const qreal dpr = overlay.devicePixelRatioF();
        QImage image(overlay.size() * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        overlay.render(&painter);
        painter.end();
        EXPECT_EQ(image, row->icon(0).pixmap(sources->iconSize(), dpr).toImage());
        EXPECT_EQ(image.pixelColor(0, 0).alpha(), 0);
        overlay.hideOverlay();
        EXPECT_FALSE(overlay.isVisible());
        const QPoint iconOffset = -internal.hotSpot();
        const QSize previewSize = internal.dragCursor(Qt::CopyAction).deviceIndependentSize().toSize()
            .expandedTo(internal.dragCursor(Qt::IgnoreAction).deviceIndependentSize().toSize())
            .expandedTo(sources->iconSize() + QSize(iconOffset.x(), iconOffset.y()));
        for (Qt::DropAction action : {Qt::CopyAction, Qt::IgnoreAction}) {
            overlay.showOverlay({SourceFileSystem::absolutePath(label)}, action, true);
            EXPECT_EQ(overlay.size(), previewSize);
            EXPECT_EQ(overlay.pos(), QCursor::pos());
            QImage expected(previewSize * dpr, QImage::Format_ARGB32_Premultiplied);
            expected.setDevicePixelRatio(dpr);
            expected.fill(Qt::transparent);
            QPainter compose(&expected);
            compose.drawPixmap(iconOffset, row->icon(0).pixmap(sources->iconSize(), dpr));
            compose.drawPixmap(QPoint(), internal.dragCursor(action));
            compose.end();
            QImage actual = expected;
            actual.fill(Qt::transparent);
            QPainter render(&actual);
            overlay.render(&render);
            render.end();
            EXPECT_EQ(actual, expected);
        }
    }
    overlay.showOverlay({SourceFileSystem::absolutePath(QStringLiteral("hit.wav")),
                         SourceFileSystem::absolutePath(QStringLiteral("FX"))}, Qt::CopyAction, false);
    EXPECT_EQ(overlay.size(), sources->iconSize());
    overlay.showOverlay({}, Qt::IgnoreAction, false);
    EXPECT_FALSE(overlay.isVisible());
}

TEST_F(ProjectExplorerSourcesTabTest, EmptyAndNonAudioDirectoryDropsReportFailureOnTreeAndHeader)
{
    const QString empty = tempDir_.filePath(QStringLiteral("Empty"));
    const QString nonAudio = tempDir_.filePath(QStringLiteral("Documents"));
    ASSERT_TRUE(QDir().mkdir(empty));
    ASSERT_TRUE(QDir().mkpath(nonAudio + QStringLiteral("/Nested")));
    QFile document(nonAudio + QStringLiteral("/Nested/Notes.txt"));
    ASSERT_TRUE(document.open(QIODevice::WriteOnly));
    ASSERT_EQ(document.write("notes"), 5);
    document.close();
    explorer_->resize(360, 800);
    explorer_->show();
    QApplication::processEvents();
    ToastManager::instance().setParentWidget(explorer_.get());
    const auto cleanup = qScopeGuard([&]() {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* toast = qobject_cast<QToastWidget*>(widget)) {
                emit toast->finished();
                toast->close();
            }
        }
        ToastManager::instance().setParentWidget(nullptr);
    });
    auto* tree = explorer_->sourcesTree();
    const int index = UndoRedoManager::instance().undoStack()->index();
    for (auto* target : {tree->viewport(), explorer_->sourcesSection()->header()}) {
        for (const QString& path : {empty, nonAudio}) {
            const auto previousWidgets = QApplication::topLevelWidgets();
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(path)});
            EXPECT_EQ(dropMime(target, target->rect().center(), &mime), Qt::IgnoreAction);
            bool reported = false;
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (!previousWidgets.contains(widget) && qobject_cast<QToastWidget*>(widget))
                    reported |= widget->isVisible();
            }
            EXPECT_TRUE(reported);
            EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), index);
            EXPECT_EQ(AssetRegistry::instance().count(), 0);
            EXPECT_TRUE(ProjectManager::instance().currentProject()->layers.empty());
        }
    }
    const QString valid = tempDir_.filePath(QStringLiteral("Audio/Nested"));
    ASSERT_TRUE(QDir().mkpath(valid));
    writeWavF32(valid + QStringLiteral("/Hit.wav"), {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData mixed;
    mixed.setUrls({QUrl::fromLocalFile(empty), QUrl::fromLocalFile(nonAudio),
                   QUrl::fromLocalFile(tempDir_.filePath(QStringLiteral("Audio")))});
    auto* header = explorer_->sourcesSection()->header();
    EXPECT_EQ(dropMime(header, header->rect().center(), &mixed), Qt::CopyAction);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    EXPECT_NE(findRow(tree, QStringLiteral("Hit.wav")), nullptr);
    EXPECT_TRUE(ProjectManager::instance().currentProject()->layers.empty());
}

TEST_F(ProjectExplorerSourcesTabTest, InternalSourceDropMovesFilesAndPreservesIdentity)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("A")));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("B")));
    const QString input = tempDir_.filePath(QStringLiteral("Move.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    auto* command = ImportAssetsCommand::pushImportSources({input}, QStringLiteral("A"));
    ASSERT_EQ(command->successCount(), 1);
    const uint32_t id = command->importedIDs().first();
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    std::unique_ptr<QMimeData> mime(tree->mimeData({findRow(tree, QStringLiteral("Move.wav"))}));
    ASSERT_TRUE(tree->moveDroppedSources(mime.get(), QStringLiteral("B")));
    auto& registry = AssetRegistry::instance();
    EXPECT_EQ(registry.getAsset(id)->relativePath, QStringLiteral("B/Move.wav"));
    EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("B/Move.wav"))));
    EXPECT_FALSE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("A/Move.wav"))));
    EXPECT_EQ(findRow(tree, QStringLiteral("Move.wav"))->parent(), findRow(tree, QStringLiteral("B")));
    UndoRedoManager::instance().undo();
    EXPECT_EQ(registry.getAsset(id)->relativePath, QStringLiteral("A/Move.wav"));
    UndoRedoManager::instance().redo();
    EXPECT_EQ(registry.getAsset(id)->relativePath, QStringLiteral("B/Move.wav"));
    EXPECT_TRUE(ProjectManager::instance().currentProject()->layers.empty());
}

TEST_F(ProjectExplorerSourcesTabTest, SourceFileAndFolderDropsMoveThroughTreeAndHeader)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("A")));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("B")));
    const QString input = tempDir_.filePath(QStringLiteral("Move.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData external;
    external.setUrls({QUrl::fromLocalFile(input)});
    explorer_->resize(360, 800);
    explorer_->show();
    auto* tree = static_cast<SourcesTreeWidget*>(explorer_->sourcesTree());
    const auto dropOnFolder = [&](const QMimeData* mime, const QString& folder) {
        QApplication::processEvents();
        auto* target = findRow(tree, folder);
        if (!target) return Qt::IgnoreAction;
        return dropMime(tree->viewport(), tree->visualItemRect(target).center(), mime);
    };
    ASSERT_EQ(dropOnFolder(&external, QStringLiteral("A")), Qt::CopyAction);
    const auto* audio = AssetRegistry::instance().getAssetByPath(QStringLiteral("A/Move.wav"));
    ASSERT_NE(audio, nullptr);
    const uint32_t id = audio->sourceID;
    std::unique_ptr<QMimeData> fileMime(tree->mimeData({findRow(tree, QStringLiteral("Move.wav"))}));
    ASSERT_EQ(dropOnFolder(fileMime.get(), QStringLiteral("B")), Qt::MoveAction);
    EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("B/Move.wav"))));
    EXPECT_FALSE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("A/Move.wav"))));
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("B/Move.wav"));
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_TRUE(tree->currentItem()->isSelected());
    EXPECT_EQ(tree->currentItem()->parent(), findRow(tree, QStringLiteral("B")));

    std::unique_ptr<QMimeData> folderMime(tree->mimeData({findRow(tree, QStringLiteral("B")),
                                                        findRow(tree, QStringLiteral("Move.wav"))}));
    ASSERT_EQ(folderMime->urls().size(), 1);
    ASSERT_EQ(dropOnFolder(folderMime.get(), QStringLiteral("A")), Qt::MoveAction);
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("A/B/Move.wav"));
    EXPECT_EQ(findRow(tree, QStringLiteral("B"))->parent(), findRow(tree, QStringLiteral("A")));
    std::unique_ptr<QMimeData> cycleMime(tree->mimeData({findRow(tree, QStringLiteral("A"))}));
    EXPECT_EQ(dropOnFolder(cycleMime.get(), QStringLiteral("B")), Qt::IgnoreAction);

    auto* header = explorer_->sourcesSection()->header();
    std::unique_ptr<QMimeData> rootFolderMime(tree->mimeData({findRow(tree, QStringLiteral("B"))}));
    explorer_->sourcesSection()->setExpanded(false);
    QApplication::processEvents();
    ASSERT_EQ(dropMime(header, header->rect().center(), rootFolderMime.get()), Qt::MoveAction);
    EXPECT_TRUE(explorer_->sourcesSection()->isExpanded());
    EXPECT_EQ(findRow(tree, QStringLiteral("B"))->parent(), nullptr);
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("B/Move.wav"));
    std::unique_ptr<QMimeData> rootFileMime(tree->mimeData({findRow(tree, QStringLiteral("Move.wav"))}));
    ASSERT_EQ(dropMime(header, header->rect().center(), rootFileMime.get()), Qt::MoveAction);
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("Move.wav"));
    EXPECT_EQ(findRow(tree, QStringLiteral("Move.wav"))->parent(), nullptr);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    auto& undo = UndoRedoManager::instance();
    const int rootMoveIndex = undo.undoStack()->index();
    for (const QString& label : {QStringLiteral("Move.wav"), QStringLiteral("B")}) {
        std::unique_ptr<QMimeData> noOpMime(tree->mimeData({findRow(tree, label)}));
        EXPECT_EQ(dropMime(header, header->rect().center(), noOpMime.get()), Qt::IgnoreAction);
        EXPECT_EQ(undo.undoStack()->index(), rootMoveIndex);
    }
    undo.undo();
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("B/Move.wav"));
    undo.redo();
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("Move.wav"));
    auto& manager = ProjectManager::instance();
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("Move.wav"))));
    EXPECT_EQ(findRow(tree, QStringLiteral("Move.wav"))->parent(), nullptr);
    EXPECT_TRUE(manager.currentProject()->layers.empty());
}

TEST_F(ProjectExplorerSourcesTabTest, ObjectAndFolderDropsReturnToTheirBucketRoots)
{
    auto& manager = ProjectManager::instance();
    auto* layer = manager.createItem(nullptr, QStringLiteral("Parent"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = manager.createItem(layer, QStringLiteral("Child"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    auto* outerFolder = manager.createFolder(nullptr, QStringLiteral("Outer"), QStringLiteral("Actions"));
    ASSERT_NE(outerFolder, nullptr);
    auto* nested = manager.createFolder(outerFolder, QStringLiteral("Inner"), QStringLiteral("Actions"));
    ASSERT_NE(nested, nullptr);
    auto* action = manager.createItem(nullptr, QStringLiteral("Trigger"), ProjectItem::Type::Action,
                                      QStringLiteral("Outer/Inner"));
    ASSERT_NE(action, nullptr);
    auto* master = manager.getMasterBusItem();
    auto* bus = manager.createItem(master, QStringLiteral("Mix"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    auto* childBus = manager.createItem(bus, QStringLiteral("Effects"), ProjectItem::Type::Bus);
    ASSERT_NE(childBus, nullptr);
    explorer_->onProjectOpened(manager.currentProject());
    explorer_->resize(400, 1000);
    explorer_->show();
    explorer_->sourcesSection()->setExpanded(false);
    auto* tree = explorer_->treeWidget();
    const auto moveToBucket = [&](const QList<uint32_t>& ids, const QString& bucketName) {
        QMimeData mime;
        RefIdMime::write(&mime, RefDomain::ProjectObject, ids);
        EXPECT_TRUE(explorer_->selectProjectItem(layer));
        auto* bucket = findRow(tree, bucketName);
        if (!bucket) return Qt::IgnoreAction;
        tree->scrollToItem(bucket);
        QApplication::processEvents();
        return dropMime(tree->viewport(), tree->visualItemRect(bucket).center(), &mime);
    };
    const auto moveRowsToBucket = [&](const QStringList& names, const QString& bucketName) {
        ProjectTreeMimeProbe mimeTree;
        mimeTree.setPanel(explorer_.get());
        QList<QTreeWidgetItem*> rows;
        for (const QString& name : names) {
            auto* row = findRow(tree, name);
            if (!row) return Qt::IgnoreAction;
            rows.append(row);
        }
        std::unique_ptr<QMimeData> mime(mimeTree.mimeData(rows));
        auto* bucket = findRow(tree, bucketName);
        if (!bucket) return Qt::IgnoreAction;
        tree->scrollToItem(bucket);
        QApplication::processEvents();
        return dropMime(tree->viewport(), tree->visualItemRect(bucket).center(), mime.get());
    };
    const auto expectNoRootMove = [&](ProjectItem* item, const QString& bucketName) {
        const int index = UndoRedoManager::instance().undoStack()->index();
        EXPECT_EQ(moveToBucket({item->objectID}, bucketName), Qt::IgnoreAction);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), index);
    };
    expectNoRootMove(layer, QStringLiteral("Layer"));
    EXPECT_EQ(moveToBucket({layer->objectID, sound->objectID}, QStringLiteral("Layer")), Qt::IgnoreAction);
    auto* outer = manager.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Outer"));
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(moveRowsToBucket({outer->name, action->name}, QStringLiteral("Action")), Qt::IgnoreAction);
    ASSERT_EQ(moveToBucket({sound->objectID}, QStringLiteral("Layer")), Qt::MoveAction);
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_TRUE(layer->children.empty());
    UndoRedoManager::instance().undo();
    EXPECT_EQ(sound->parent, layer);
    UndoRedoManager::instance().redo();
    EXPECT_EQ(sound->parent, nullptr);
    expectNoRootMove(sound, QStringLiteral("Layer"));
    ASSERT_EQ(moveToBucket({action->objectID}, QStringLiteral("Action")), Qt::MoveAction);
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(action).isEmpty());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(manager.currentProject()->itemFolderPath(action), QStringLiteral("Outer/Inner"));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(action).isEmpty());
    expectNoRootMove(action, QStringLiteral("Action"));
    ASSERT_EQ(moveRowsToBucket({nested->name}, QStringLiteral("Action")), Qt::MoveAction);
    EXPECT_EQ(nested->parent, nullptr);
    EXPECT_EQ(manager.currentProject()->folderRelPath(nested), QStringLiteral("Inner"));
    const int folderMoveIndex = UndoRedoManager::instance().undoStack()->index();
    EXPECT_EQ(moveRowsToBucket({nested->name}, QStringLiteral("Action")), Qt::IgnoreAction);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), folderMoveIndex);
    ASSERT_EQ(moveToBucket({childBus->objectID}, QStringLiteral("Bus")), Qt::MoveAction);
    EXPECT_EQ(childBus->parent, master);
    UndoRedoManager::instance().undo();
    EXPECT_EQ(childBus->parent, bus);
    UndoRedoManager::instance().redo();
    EXPECT_EQ(childBus->parent, master);
    expectNoRootMove(childBus, QStringLiteral("Bus"));
}

TEST_F(ProjectExplorerSourcesTabTest, CrossFolderChildDropsUndoParentAndFolderTogether)
{
    auto& manager = ProjectManager::instance();
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Organization"), QStringLiteral("Layers"));
    auto* layer = manager.createItem(nullptr, QStringLiteral("Parent"), ProjectItem::Type::Layer);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(layer, nullptr);
    auto* first = manager.createItem(layer, QStringLiteral("First"), ProjectItem::Type::Sound);
    auto* second = manager.createItem(layer, QStringLiteral("Second"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    explorer_->resize(400, 700);
    explorer_->show();
    explorer_->sourcesSection()->setExpanded(false);
    auto* tree = explorer_->treeWidget();
    auto* folderRow = findRow(tree, folder->name);
    ASSERT_NE(folderRow, nullptr);
    QMimeData mime;
    RefIdMime::write(&mime, RefDomain::ProjectObject, {first->objectID, second->objectID});
    tree->scrollToItem(folderRow);
    QApplication::processEvents();
    auto& undo = UndoRedoManager::instance();
    const int initialHistory = undo.undoStack()->count();
    ASSERT_EQ(dropMime(tree->viewport(), tree->visualItemRect(folderRow).center(), &mime), Qt::MoveAction);
    EXPECT_EQ(undo.undoStack()->count(), initialHistory + 1);
    EXPECT_EQ(first->parent, nullptr);
    EXPECT_EQ(second->parent, nullptr);
    EXPECT_EQ(manager.currentProject()->itemFolderPath(first), QStringLiteral("Organization"));
    EXPECT_EQ(manager.currentProject()->itemFolderPath(second), QStringLiteral("Organization"));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(first->parent, layer);
    EXPECT_EQ(second->parent, layer);
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(first).isEmpty());
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(second).isEmpty());
    ASSERT_TRUE(undo.redo());

    ASSERT_TRUE(explorer_->selectProjectItem(layer));
    auto* parentRow = tree->currentItem();
    ASSERT_NE(parentRow, nullptr);
    tree->scrollToItem(parentRow);
    QApplication::processEvents();
    ASSERT_EQ(dropMime(tree->viewport(), tree->visualItemRect(parentRow).center(), &mime), Qt::MoveAction);
    EXPECT_EQ(undo.undoStack()->count(), initialHistory + 2);
    EXPECT_EQ(first->parent, layer);
    EXPECT_EQ(second->parent, layer);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(first->parent, nullptr);
    EXPECT_EQ(second->parent, nullptr);
    EXPECT_EQ(manager.currentProject()->itemFolderPath(first), QStringLiteral("Organization"));
    ASSERT_TRUE(undo.redo());
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(first).isEmpty());
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(second).isEmpty());
    const QUuid firstGuid = first->guid;
    const QUuid secondGuid = second->guid;
    const QUuid layerGuid = layer->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_EQ(manager.findItemByGuid(firstGuid)->parent, manager.findItemByGuid(layerGuid));
    EXPECT_EQ(manager.findItemByGuid(secondGuid)->parent, manager.findItemByGuid(layerGuid));
}

TEST_F(ProjectExplorerSourcesTabTest, MixedDirectoryAndObjectDropChangesMemoryInOneUndoCommand)
{
    auto& manager = ProjectManager::instance();
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Source"), QStringLiteral("Actions"));
    auto* destination = manager.createFolder(nullptr, QStringLiteral("Destination"), QStringLiteral("Actions"));
    auto* action = manager.createItem(nullptr, QStringLiteral("Trigger"), ProjectItem::Type::Action);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(destination, nullptr);
    ASSERT_NE(action, nullptr);
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();
    const QString originalActionFile = manager.currentProject()->objectFilePath(action);
    QFile savedAction(originalActionFile);
    ASSERT_TRUE(savedAction.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = savedAction.readAll();
    savedAction.close();
    action->extensions.insert(QStringLiteral("test"), QStringLiteral("Unsaved"));
    action->markDirty();
    const QDir projectRoot(QFileInfo(manager.currentProject()->projectPath).absolutePath());
    const QString originalFolder = projectRoot.filePath(QStringLiteral("Actions/Source"));
    auto* tree = explorer_->treeWidget();
    explorer_->resize(400, 700);
    explorer_->show();
    auto* folderRow = findRow(tree, folder->name);
    auto* actionRow = findRow(tree, action->name);
    auto* destinationRow = findRow(tree, destination->name);
    ASSERT_NE(folderRow, nullptr);
    ASSERT_NE(actionRow, nullptr);
    ASSERT_NE(destinationRow, nullptr);
    ProjectTreeMimeProbe mimeTree;
    mimeTree.setPanel(explorer_.get());
    std::unique_ptr<QMimeData> mime(mimeTree.mimeData({folderRow, actionRow}));
    tree->scrollToItem(destinationRow);
    QApplication::processEvents();
    const int beforeMove = UndoRedoManager::instance().undoStack()->index();
    ASSERT_EQ(dropMime(tree->viewport(), tree->visualItemRect(destinationRow).center(), mime.get()),
              Qt::MoveAction);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), beforeMove + 1);
    EXPECT_EQ(folder->parent, destination);
    EXPECT_EQ(manager.currentProject()->itemFolderPath(action), QStringLiteral("Destination"));
    EXPECT_TRUE(action->isDirtyObject());
    EXPECT_TRUE(QFileInfo::exists(originalActionFile));
    EXPECT_TRUE(QDir(originalFolder).exists());
    ASSERT_TRUE(savedAction.open(QIODevice::ReadOnly));
    EXPECT_EQ(savedAction.readAll(), originalBytes);
    savedAction.close();
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(folder->parent, nullptr);
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(action).isEmpty());
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(QFileInfo::exists(originalActionFile));
    EXPECT_FALSE(QDir(originalFolder).exists());
    QFile movedAction(manager.currentProject()->objectFilePath(action));
    ASSERT_TRUE(movedAction.open(QIODevice::ReadOnly));
    EXPECT_TRUE(movedAction.readAll().contains("Unsaved"));
}

TEST_F(ProjectExplorerSourcesTabTest, AudioDropOnPhysicalFolderDoesNotCreateRootSound)
{
    auto& manager = ProjectManager::instance();
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Organization"),
                                        QStringLiteral("Layers"));
    ASSERT_NE(folder, nullptr);
    const QString input = tempDir_.filePath(QStringLiteral("FolderDrop.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(input)});
    UndoRedoManager::instance().clear();
    explorer_->resize(400, 700);
    explorer_->show();
    auto* tree = explorer_->treeWidget();
    ASSERT_TRUE(explorer_->selectFolder(folder));
    auto* row = tree->currentItem();
    ASSERT_NE(row, nullptr);
    tree->scrollToItem(row);
    QApplication::processEvents();
    EXPECT_EQ(dropMime(tree->viewport(), tree->visualItemRect(row).center(), &mime),
              Qt::IgnoreAction);
    EXPECT_TRUE(manager.currentProject()->layers.empty());
    EXPECT_EQ(AssetRegistry::instance().count(), 0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectExplorerSourcesTabTest, SourceRenameEditsInPlaceAndRejectsInvalidNamesWithoutWarnings)
{
    const QString input = tempDir_.filePath(QStringLiteral("Hit.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    auto* command = ImportAssetsCommand::pushImportSources({input});
    ASSERT_EQ(command->successCount(), 1);
    const uint32_t id = command->importedIDs().first();
    UndoRedoManager::instance().clear();
    explorer_->resize(420, 600);
    explorer_->show();
    auto* tree = explorer_->sourcesTree();
    auto* row = findRow(tree, QStringLiteral("Hit.wav"));
    ASSERT_NE(row, nullptr);
    tree->setCurrentItem(row);
    QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
    QApplication::sendEvent(tree, &f2);
    auto* editor = tree->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("Hit.wav"));
    for (const auto& value : {QStringLiteral(""), QStringLiteral("Bad/Name.wav"),
                              QStringLiteral("Hit.txt")}) {
        editor->setText(value);
        EXPECT_TRUE(editor->property("inputInvalid").toBool());
        EXPECT_FALSE(editor->finish(true));
        EXPECT_FALSE(editor->isHidden());
        EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("Hit.wav"));
        EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
    }
    editor->setText(QStringLiteral("Impact.wav"));
    EXPECT_FALSE(editor->property("inputInvalid").toBool());
    EXPECT_TRUE(editor->finish(true));
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("Impact.wav"));
    EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("Impact.wav"))));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("Hit.wav"));
}

TEST_F(ProjectExplorerSourcesTabTest, ConfirmedRenamePreservesSourceIdentityAndAllReferenceSettingsAcrossHistoryAndReload)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    const QString first = tempDir_.filePath(QStringLiteral("A.wav"));
    const QString second = tempDir_.filePath(QStringLiteral("B.wav"));
    writeWavF32(first, {0.0f, 0.5f, -0.5f, 0.0f});
    writeWavF32(second, {0.0f, 0.25f, -0.25f, 0.0f});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({first, second})->successCount(), 2);
    const auto missing = *registry.getAssetByPath(QStringLiteral("A.wav"));
    const auto source = *registry.getAssetByPath(QStringLiteral("B.wav"));
    auto* usesA = manager.createRootAudioItem(QStringLiteral("Uses A"), missing.relativePath, missing.sourceID);
    auto* usesB = manager.createRootAudioItem(QStringLiteral("Uses B"), source.relativePath, source.sourceID);
    ASSERT_NE(usesA, nullptr);
    ASSERT_NE(usesB, nullptr);
    manager.currentProject()->projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    auto& sources = usesA->comp<VSound>()->sources;
    sources.localized[QStringLiteral("English")] = {};
    sources.localized[QStringLiteral("Japanese")] = {};
    const QString platform = manager.currentProject()->platformIds().first();
    for (auto* set : SoundSourceSemantics::sourceSets(sources)) {
        SoundSourceDef entry;
        entry.entryId = QUuid::createUuid();
        entry.sourceId = missing.sourceID;
        entry.trimStartFrame = 1;
        entry.trimEndFrame = 4;
        entry.loopRegion = SoundLoopRegion{2, 3};
        entry.rangeContentHash = missing.contentHash;
        SoundSourceDef alternate = entry;
        alternate.entryId = QUuid::createUuid();
        alternate.sourceId = source.sourceID;
        alternate.trimStartFrame = 0;
        set->entries = {entry, alternate};
        set->active = entry.entryId;
        set->platforms[platform].active = alternate.entryId;
    }
    const auto beforeA = sources;
    const auto beforeB = usesB->comp<VSound>()->sources;
    auto afterA = beforeA;
    for (auto* set : SoundSourceSemantics::sourceSets(afterA)) set->entries.front().sourceId = source.sourceID;
    const QUuid guidA = usesA->guid;
    const QUuid guidB = usesB->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(QFile::remove(missing.absolutePath));
    manager.fileWatcher()->refreshSources();
    const int history = undo.undoStack()->count();

    explorer_->resize(420, 600);
    explorer_->show();
    auto* tree = explorer_->sourcesTree();
    for (bool confirm : {false, true}) {
        auto* row = findRow(tree, source.relativePath);
        ASSERT_NE(row, nullptr);
        tree->setCurrentItem(row);
        QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(tree, &f2);
        auto* editor = tree->findChild<InlineRenameEdit*>();
        ASSERT_NE(editor, nullptr);
        editor->setText(missing.relativePath);
        EXPECT_FALSE(editor->property("inputInvalid").toBool());
        EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
        int dialogs = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                EXPECT_EQ(dialog->objectName(), QStringLiteral("sourceRenameRestoreDialog"));
                ++dialogs;
                if (confirm) dialog->accept();
                else dialog->reject();
            }
        });
        timer.start(1);
        EXPECT_TRUE(editor->finish(true));
        timer.stop();
        EXPECT_EQ(dialogs, 1);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!confirm) {
            EXPECT_EQ(undo.undoStack()->count(), history);
            EXPECT_EQ(usesA->comp<VSound>()->sources, beforeA);
            EXPECT_EQ(registry.getAsset(source.sourceID)->relativePath, source.relativePath);
            EXPECT_NE(findRow(tree, source.relativePath), nullptr);
            EXPECT_FALSE(QFile::exists(missing.absolutePath));
        }
    }
    EXPECT_EQ(undo.undoStack()->count(), history + 1);
    EXPECT_FALSE(registry.contains(missing.sourceID));
    EXPECT_EQ(registry.getAsset(source.sourceID)->relativePath, missing.relativePath);
    EXPECT_EQ(usesA->comp<VSound>()->sources, afterA);
    EXPECT_EQ(usesB->comp<VSound>()->sources, beforeB);
    EXPECT_FALSE(QFile::exists(source.absolutePath));
    EXPECT_EQ(AssetImporter::computeContentHash(missing.absolutePath), source.contentHash);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(usesA->comp<VSound>()->sources, beforeA);
    EXPECT_EQ(usesB->comp<VSound>()->sources, beforeB);
    EXPECT_TRUE(registry.contains(missing.sourceID));
    EXPECT_EQ(registry.getAsset(source.sourceID)->relativePath, source.relativePath);
    EXPECT_FALSE(QFile::exists(missing.absolutePath));
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(usesA->comp<VSound>()->sources, afterA);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_FALSE(registry.contains(missing.sourceID));
    ASSERT_NE(registry.getAsset(source.sourceID), nullptr);
    EXPECT_EQ(registry.getAsset(source.sourceID)->relativePath, missing.relativePath);
    EXPECT_EQ(AssetImporter::computeContentHash(missing.absolutePath), source.contentHash);
    EXPECT_FALSE(QFile::exists(source.absolutePath));
    ASSERT_NE(manager.findItemByGuid(guidA), nullptr);
    ASSERT_NE(manager.findItemByGuid(guidB), nullptr);
    EXPECT_EQ(manager.findItemByGuid(guidA)->comp<VSound>()->sources, afterA);
    EXPECT_EQ(manager.findItemByGuid(guidB)->comp<VSound>()->sources, beforeB);
}

TEST_F(ProjectExplorerSourcesTabTest, DiscardingSourceMergeRestoresTheSavedPathsAndReferences)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    const QString first = tempDir_.filePath(QStringLiteral("A.wav"));
    const QString second = tempDir_.filePath(QStringLiteral("B.wav"));
    writeWavF32(first, {0.0f, 0.5f, -0.5f, 0.0f});
    writeWavF32(second, {0.0f, 0.25f, -0.25f, 0.0f});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({first, second})->successCount(), 2);
    const auto missing = *registry.getAssetByPath(QStringLiteral("A.wav"));
    const auto source = *registry.getAssetByPath(QStringLiteral("B.wav"));
    auto* sound = manager.createRootAudioItem(QStringLiteral("Uses A"), missing.relativePath, missing.sourceID);
    ASSERT_NE(sound, nullptr);
    const auto guid = sound->guid;
    const auto authored = sound->comp<VSound>()->sources;
    const auto projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(QFile::remove(missing.absolutePath));
#ifdef Q_OS_WIN
    {
        const int history = UndoRedoManager::instance().undoStack()->count();
        const HANDLE locked = CreateFileW(reinterpret_cast<const wchar_t*>(source.absolutePath.utf16()),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(locked, INVALID_HANDLE_VALUE);
        const auto unlock = qScopeGuard([&] { CloseHandle(locked); });
        EXPECT_FALSE(SourceFileSystem::restoreByRename(source.sourceID, missing.sourceID));
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), history);
        EXPECT_TRUE(registry.contains(missing.sourceID));
        EXPECT_EQ(registry.getAsset(source.sourceID)->relativePath, source.relativePath);
        EXPECT_EQ(sound->comp<VSound>()->sources, authored);
    }
#endif
    for (bool undone : {false, true}) {
        ASSERT_TRUE(SourceFileSystem::restoreByRename(source.sourceID, missing.sourceID));
        if (undone) ASSERT_TRUE(UndoRedoManager::instance().undo());
        ASSERT_TRUE(manager.closeProject());
        ASSERT_TRUE(manager.loadProject(projectPath));
        ASSERT_NE(registry.getAsset(missing.sourceID), nullptr);
        ASSERT_NE(registry.getAsset(source.sourceID), nullptr);
        EXPECT_EQ(registry.getAsset(source.sourceID)->relativePath, source.relativePath);
        EXPECT_FALSE(QFile::exists(missing.absolutePath));
        EXPECT_EQ(AssetImporter::computeContentHash(source.absolutePath), source.contentHash);
        ASSERT_NE(manager.findItemByGuid(guid), nullptr);
        EXPECT_EQ(manager.findItemByGuid(guid)->comp<VSound>()->sources, authored);
    }
}

TEST_F(ProjectExplorerSourcesTabTest, MissingSourcePathsRemainReservedForFileFolderAndHistoryOperations)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Hit.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    for (const auto& folder : {QStringLiteral("Reserved"), QStringLiteral("Moving")}) {
        ASSERT_TRUE(SourceFileSystem::createFolder({}, folder));
        ASSERT_EQ(ImportAssetsCommand::pushImportSources({input}, folder)->successCount(), 1);
    }
    const auto reserved = *registry.getAssetByPath(QStringLiteral("Reserved/Hit.wav"));
    const auto moving = *registry.getAssetByPath(QStringLiteral("Moving/Hit.wav"));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(QFile::remove(reserved.absolutePath));
    const int history = undo.undoStack()->count();
    QString error;
    EXPECT_FALSE(SourceFileSystem::move({moving.relativePath}, QStringLiteral("Reserved"), &error));
    EXPECT_FALSE(error.isEmpty());
    ASSERT_TRUE(QDir().rmdir(QFileInfo(reserved.absolutePath).absolutePath()));
    EXPECT_FALSE(SourceFileSystem::rename(QStringLiteral("Moving"), QStringLiteral("Reserved"), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(undo.undoStack()->count(), history);
    EXPECT_EQ(registry.getAsset(moving.sourceID)->relativePath, moving.relativePath);
    EXPECT_TRUE(QFile::exists(moving.absolutePath));
    ASSERT_TRUE(SourceFileSystem::rename(moving.relativePath, QStringLiteral("Changed.wav")));
    writeWavF32(moving.absolutePath, {0.0f, 0.25f, -0.25f, 0.0f});
    manager.fileWatcher()->refreshSources();
    ASSERT_NE(registry.findAssetByPath(moving.absolutePath), nullptr);
    const uint32_t externalID = registry.findAssetByPath(moving.absolutePath)->sourceID;
    ASSERT_NE(externalID, moving.sourceID);
    ASSERT_TRUE(QFile::remove(moving.absolutePath));
    const int index = undo.undoStack()->index();
    EXPECT_FALSE(undo.undo());
    EXPECT_EQ(undo.undoStack()->index(), index);
    EXPECT_EQ(registry.getAsset(moving.sourceID)->relativePath, QStringLiteral("Moving/Changed.wav"));
    EXPECT_EQ(registry.getAsset(externalID)->relativePath, moving.relativePath);
    ASSERT_TRUE(manager.saveProject());
}

TEST_F(ProjectExplorerSourcesTabTest, ExternalRenameRestoresTheDestinationIdentityAndLeavesTheOriginalMissing)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    const QString first = tempDir_.filePath(QStringLiteral("A.wav"));
    const QString second = tempDir_.filePath(QStringLiteral("B.wav"));
    writeWavF32(first, {0.0f, 0.5f, -0.5f, 0.0f});
    writeWavF32(second, {0.0f, 0.25f, -0.25f, 0.0f});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({first, second})->successCount(), 2);
    const auto missing = *registry.getAssetByPath(QStringLiteral("A.wav"));
    const auto source = *registry.getAssetByPath(QStringLiteral("B.wav"));
    auto* usesA = manager.createRootAudioItem(QStringLiteral("Uses A"), missing.relativePath, missing.sourceID);
    auto* usesB = manager.createRootAudioItem(QStringLiteral("Uses B"), source.relativePath, source.sourceID);
    ASSERT_NE(usesA, nullptr);
    ASSERT_NE(usesB, nullptr);
    const auto guidA = usesA->guid;
    const auto guidB = usesB->guid;
    const auto beforeA = usesA->comp<VSound>()->sources;
    const auto beforeB = usesB->comp<VSound>()->sources;
    const auto projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(QFile::remove(missing.absolutePath));
    manager.fileWatcher()->refreshSources();
    ASSERT_TRUE(QFile::rename(source.absolutePath, missing.absolutePath));
    manager.fileWatcher()->refreshSources();
    EXPECT_EQ(registry.findAssetByPath(missing.absolutePath)->sourceID, missing.sourceID);
    EXPECT_EQ(registry.findAssetByPath(source.absolutePath)->sourceID, source.sourceID);
    EXPECT_EQ(registry.getAsset(missing.sourceID)->contentHash, source.contentHash);
    EXPECT_TRUE(registry.sourceFileExists(missing.sourceID));
    EXPECT_FALSE(registry.sourceFileExists(source.sourceID));
    EXPECT_EQ(usesA->comp<VSound>()->sources, beforeA);
    EXPECT_EQ(usesB->comp<VSound>()->sources, beforeB);
    EXPECT_NE(findRow(explorer_->sourcesTree(), missing.relativePath), nullptr);
    EXPECT_EQ(findRow(explorer_->sourcesTree(), source.relativePath), nullptr);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_TRUE(registry.sourceFileExists(missing.sourceID));
    EXPECT_FALSE(registry.sourceFileExists(source.sourceID));
    ASSERT_NE(manager.findItemByGuid(guidA), nullptr);
    ASSERT_NE(manager.findItemByGuid(guidB), nullptr);
    EXPECT_EQ(manager.findItemByGuid(guidA)->comp<VSound>()->sources, beforeA);
    EXPECT_EQ(manager.findItemByGuid(guidB)->comp<VSound>()->sources, beforeB);
}

TEST_F(ProjectExplorerSourcesTabTest, InvalidSourceRegistryCannotSilentlyFailProjectSave)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Hit.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({input})->successCount(), 1);
    const auto source = *registry.getAssetByPath(QStringLiteral("Hit.wav"));
    ASSERT_TRUE(manager.saveProject());
    QFile project(manager.currentProject()->projectPath);
    ASSERT_TRUE(project.open(QIODevice::ReadOnly));
    const auto saved = project.readAll();
    project.close();
    QStringList errors;
    QObject observer;
    QObject::connect(&manager, &ProjectManager::errorOccurred, &observer,
        [&](const QString& error) { errors.append(error); });
    registry.getAsset(source.sourceID)->relativePath = QStringLiteral("../Hit.wav");
    EXPECT_FALSE(manager.saveProject());
    ASSERT_FALSE(errors.isEmpty());
    EXPECT_FALSE(errors.last().isEmpty());
    ASSERT_TRUE(project.open(QIODevice::ReadOnly));
    EXPECT_EQ(project.readAll(), saved);
    project.close();
    registry.getAsset(source.sourceID)->relativePath = source.relativePath;
    ASSERT_TRUE(manager.saveProject());
}

TEST_F(ProjectExplorerSourcesTabTest, FolderMoveRenameAndSavePreserveSharedSoundReferences)
{
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("FX")));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Destination")));
    const QString input = tempDir_.filePath(QStringLiteral("Shared.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    auto* command = ImportAssetsCommand::pushImportSources({input}, QStringLiteral("FX"));
    ASSERT_EQ(command->successCount(), 1);
    const uint32_t id = command->importedIDs().first();
    auto& manager = ProjectManager::instance();
    auto* first = manager.createRootAudioItem(QStringLiteral("First"), QStringLiteral("FX/Shared.wav"), id);
    auto* second = manager.createRootAudioItem(QStringLiteral("Second"), QStringLiteral("FX/Shared.wav"), id);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(manager.createFolder(nullptr, QStringLiteral("Organization"), QStringLiteral("Layers")),
              nullptr);
    ASSERT_TRUE(manager.moveEntriesToFolder({first}, {}, QStringLiteral("Layers"), QStringLiteral("Organization")));
    const QUuid firstGuid = first->guid;
    const QUuid secondGuid = second->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());

    ASSERT_TRUE(SourceFileSystem::move({QStringLiteral("FX")}, QStringLiteral("Destination")));
    ASSERT_TRUE(SourceFileSystem::rename(QStringLiteral("Destination/FX"), QStringLiteral("Impacts")));
    const QString path = QStringLiteral("Destination/Impacts/Shared.wav");
    EXPECT_EQ(first->sourcePath, path);
    EXPECT_EQ(second->sourcePath, path);
    EXPECT_EQ(manager.currentProject()->itemFolderPath(first), QStringLiteral("Organization"));
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, path);
    EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(path)));
    UndoRedoManager::instance().undo();
    EXPECT_EQ(first->sourcePath, QStringLiteral("Destination/FX/Shared.wav"));
    UndoRedoManager::instance().undo();
    EXPECT_EQ(first->sourcePath, QStringLiteral("FX/Shared.wav"));
    EXPECT_EQ(manager.currentProject()->itemFolderPath(first), QStringLiteral("Organization"));
    UndoRedoManager::instance().redo();
    UndoRedoManager::instance().redo();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    ASSERT_NE(manager.findItemByGuid(firstGuid), nullptr);
    ASSERT_NE(manager.findItemByGuid(secondGuid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(firstGuid)->sourcePath, path);
    EXPECT_EQ(manager.findItemByGuid(secondGuid)->sourcePath, path);
    EXPECT_EQ(manager.currentProject()->itemFolderPath(manager.findItemByGuid(firstGuid)), QStringLiteral("Organization"));
    EXPECT_TRUE(manager.currentProject()->itemFolderPath(manager.findItemByGuid(secondGuid)).isEmpty());
    ASSERT_NE(AssetRegistry::instance().getAsset(id), nullptr);
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, path);
    EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(path)));
}

TEST_F(ProjectExplorerSourcesTabTest, BatchReplayFailureRestoresFilesReferencesAndHistoryBeforeRetry)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Target")));
    const QStringList names{QStringLiteral("A.wav"), QStringLiteral("B.wav")};
    QList<uint32_t> ids;
    QList<ProjectItem*> sounds;
    QList<QByteArray> contents;
    for (const QString& name : names) {
        const QString input = tempDir_.filePath(name);
        writeWavF32(input, {0.0f, 0.25f + ids.size() * 0.25f, -0.5f, 0.0f});
        auto* command = ImportAssetsCommand::pushImportSources({input});
        ASSERT_EQ(command->successCount(), 1);
        ids.append(command->importedIDs().first());
        sounds.append(manager.createRootAudioItem(name, name, ids.last()));
        ASSERT_NE(sounds.last(), nullptr);
        QFile file(input);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        contents.append(file.readAll());
    }
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(SourceFileSystem::move(names, QStringLiteral("Target")));
    const auto expectPaths = [&](const QString& prefix) {
        for (int i = 0; i < names.size(); ++i) {
            const QString path = prefix + names[i];
            EXPECT_EQ(registry.getAsset(ids[i])->relativePath, path);
            ASSERT_NE(registry.getAssetByPath(path), nullptr);
            EXPECT_EQ(registry.getAssetByPath(path)->sourceID, ids[i]);
            EXPECT_EQ(sounds[i]->sourcePath, path);
            QFile file(SourceFileSystem::absolutePath(path));
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            EXPECT_EQ(file.readAll(), contents[i]);
        }
    };
    for (bool redo : {false, true}) {
        if (redo)
            ASSERT_TRUE(undo.undo());
        const QString before = redo ? QString() : QStringLiteral("Target/");
        const QString after = redo ? QStringLiteral("Target/") : QString();
        const QString conflict = SourceFileSystem::absolutePath(after + (redo ? names.last() : names.first()));
        writeWavF32(conflict, {0.0f, 0.75f, -0.75f, 0.0f});
        const int index = undo.undoStack()->index();
        const int count = undo.undoStack()->count();
        const int cleanIndex = undo.undoStack()->cleanIndex();
        const bool clean = undo.isClean();
        int notifications = 0;
        {
            QObject observer;
            QObject::connect(&registry, &AssetRegistry::assetModified, &observer,
                [&](uint32_t) { ++notifications; });
            QObject::connect(&registry, &AssetRegistry::sourceFoldersChanged, &observer,
                [&]() { ++notifications; });
            QObject::connect(&manager, &ProjectManager::itemPropertyChanged, &observer,
                [&](ProjectItem*, const QString&) { ++notifications; });
            QObject::connect(&undo, &UndoRedoManager::indexChanged, &observer,
                [&](int) { ++notifications; });
            QObject::connect(&undo, &UndoRedoManager::cleanChanged, &observer,
                [&](bool) { ++notifications; });
            EXPECT_FALSE(redo ? undo.redo() : undo.undo());
        }
        EXPECT_EQ(notifications, 0);
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_EQ(undo.undoStack()->count(), count);
        EXPECT_EQ(undo.undoStack()->cleanIndex(), cleanIndex);
        EXPECT_EQ(undo.isClean(), clean);
        expectPaths(before);
        const QString restored = SourceFileSystem::absolutePath(after + (redo ? names.first() : names.last()));
        EXPECT_FALSE(QFile::exists(restored));
        EXPECT_TRUE(QFile::exists(conflict));
        ASSERT_TRUE(QFile::remove(conflict));
        ASSERT_TRUE(redo ? undo.redo() : undo.undo());
        expectPaths(after);
        if (!redo)
            ASSERT_TRUE(undo.redo());
    }
    const QString audioRoot = SourceFileSystem::absolutePath({});
    ASSERT_TRUE(manager.closeProject());
    for (const QString& name : names) {
        EXPECT_TRUE(QFile::exists(QDir(audioRoot).filePath(name)));
        EXPECT_FALSE(QFile::exists(QDir(audioRoot).filePath(QStringLiteral("Target/") + name)));
    }
    ASSERT_TRUE(manager.loadProject(projectPath));
    for (int i = 0; i < names.size(); ++i)
        EXPECT_EQ(registry.getAsset(ids[i])->relativePath, names[i]);
}

TEST_F(ProjectExplorerSourcesTabTest, BatchFolderUndoRestoresEarlierFoldersWhenALaterSourceIsMissing)
{
    auto& undo = UndoRedoManager::instance();
    auto& manager = ProjectManager::instance();
    for (const QString& name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Target")})
        ASSERT_TRUE(SourceFileSystem::createFolder({}, name));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(SourceFileSystem::move({QStringLiteral("A"), QStringLiteral("B")}, QStringLiteral("Target")));
    const QString missing = SourceFileSystem::absolutePath(QStringLiteral("Target/A"));
    const QString renamed = SourceFileSystem::absolutePath(QStringLiteral("External"));
    ASSERT_TRUE(QDir().rename(missing, renamed));
    const int index = undo.undoStack()->index();
    EXPECT_FALSE(undo.undo());
    EXPECT_EQ(undo.undoStack()->index(), index);
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("Target/B"))).exists());
    EXPECT_FALSE(QDir(SourceFileSystem::absolutePath(QStringLiteral("B"))).exists());
    ASSERT_TRUE(QDir().rename(renamed, missing));
    ASSERT_TRUE(undo.undo());
    EXPECT_TRUE(undo.isClean());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("A"))).exists());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("B"))).exists());
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("Target/A"))).exists());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("Target/B"))).exists());
}

TEST_F(ProjectExplorerSourcesTabTest, CaseOnlySourceRenamePreservesIdentityAndDiscardsTemporaryPaths)
{
    const QString input = tempDir_.filePath(QStringLiteral("Hit.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    auto* command = ImportAssetsCommand::pushImportSources({input});
    ASSERT_EQ(command->successCount(), 1);
    const uint32_t id = command->importedIDs().first();
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* sound = manager.createRootAudioItem(QStringLiteral("Hit"), QStringLiteral("Hit.wav"), id);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(manager.saveProject());
    const QString audioRoot = SourceFileSystem::absolutePath({});
    ASSERT_TRUE(SourceFileSystem::rename(QStringLiteral("Hit.wav"), QStringLiteral("hit.wav")));
    EXPECT_EQ(sound->sourcePath, QStringLiteral("hit.wav"));
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("hit.wav"));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(sound->sourcePath, QStringLiteral("Hit.wav"));
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(sound->sourcePath, QStringLiteral("hit.wav"));
    ASSERT_TRUE(manager.closeProject());
    EXPECT_EQ(QDir(audioRoot).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("Hit.wav")});
}

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, InitialBatchFileLockRollsBackWithoutPublishingOrRecordingChanges)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Target")));
    QList<uint32_t> ids;
    const QStringList names{QStringLiteral("A.wav"), QStringLiteral("B.wav")};
    for (const QString& name : names) {
        const QString input = tempDir_.filePath(name);
        writeWavF32(input, {0.0f, 0.25f + ids.size() * 0.25f, -0.5f, 0.0f});
        auto* command = ImportAssetsCommand::pushImportSources({input});
        ASSERT_EQ(command->successCount(), 1);
        ids.append(command->importedIDs().first());
    }
    ASSERT_TRUE(manager.saveProject());
    const QString lockedPath = SourceFileSystem::absolutePath(names.last());
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto closeHandle = qScopeGuard([&]() { CloseHandle(handle); });
    const int index = undo.undoStack()->index();
    const int count = undo.undoStack()->count();
    QObject observer;
    int notifications = 0;
    QObject::connect(&registry, &AssetRegistry::assetModified, &observer,
        [&](uint32_t) { ++notifications; });
    QObject::connect(&registry, &AssetRegistry::sourceFoldersChanged, &observer,
        [&]() { ++notifications; });
    QString error;
    EXPECT_FALSE(SourceFileSystem::move(names, QStringLiteral("Target"), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(undo.undoStack()->index(), index);
    EXPECT_EQ(undo.undoStack()->count(), count);
    EXPECT_TRUE(undo.isClean());
    for (int i = 0; i < names.size(); ++i) {
        EXPECT_EQ(registry.getAsset(ids[i])->relativePath, names[i]);
        EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(names[i])));
        EXPECT_FALSE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("Target/") + names[i])));
    }
    EXPECT_EQ(notifications, 0);
    ASSERT_TRUE(manager.closeProject());
    EXPECT_EQ(notifications, 0);
}
#endif

TEST_F(ProjectExplorerSourcesTabTest, DiscardRestoresSavedFilesAndRemovesUncommittedSourceFolders)
{
    const QString input = tempDir_.filePath(QStringLiteral("Keep.wav"));
    writeWavF32(input, {0.0f, 0.5f, -0.5f, 0.0f});
    const QString savedHash = AssetImporter::computeContentHash(input);
    auto* command = ImportAssetsCommand::pushImportSources({input});
    ASSERT_EQ(command->successCount(), 1);
    const uint32_t id = command->importedIDs().first();
    auto& manager = ProjectManager::instance();
    auto* sound = manager.createRootAudioItem(QStringLiteral("Keep"), QStringLiteral("Keep.wav"), id);
    ASSERT_NE(sound, nullptr);
    const QUuid guid = sound->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    const QString audioRoot = SourceFileSystem::absolutePath({});
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("New")));
    ASSERT_TRUE(SourceFileSystem::move({QStringLiteral("Keep.wav")}, QStringLiteral("New")));
    ASSERT_TRUE(SourceFileSystem::rename(QStringLiteral("New"), QStringLiteral("Renamed")));
    const QString extra = tempDir_.filePath(QStringLiteral("Unsaved.wav"));
    writeWavF32(extra, {0.0f, 0.25f, -0.25f, 0.0f});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({extra}, QStringLiteral("Renamed"))->successCount(), 1);
    writeWavF32(input, {0.0f, 0.75f, -0.75f, 0.0f});
    writeWavF32(extra, {0.0f, 0.125f, -0.125f, 0.0f});
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({input, extra}, QStringLiteral("Renamed"),
        {{QStringLiteral("Renamed/Keep.wav"), input}, {QStringLiteral("Renamed/Unsaved.wav"), extra}})
        ->successCount(), 2);
    ASSERT_TRUE(manager.closeProject());
    EXPECT_TRUE(QFile::exists(QDir(audioRoot).filePath(QStringLiteral("Keep.wav"))));
    EXPECT_EQ(AssetImporter::computeContentHash(QDir(audioRoot).filePath(QStringLiteral("Keep.wav"))), savedHash);
    EXPECT_FALSE(QDir(QDir(audioRoot).filePath(QStringLiteral("New"))).exists());
    EXPECT_FALSE(QDir(QDir(audioRoot).filePath(QStringLiteral("Renamed"))).exists());
    EXPECT_TRUE(QFile::exists(extra));
    ASSERT_TRUE(manager.loadProject(projectPath));
    ASSERT_NE(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(guid)->sourcePath, QStringLiteral("Keep.wav"));
    ASSERT_NE(AssetRegistry::instance().getAsset(id), nullptr);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    EXPECT_EQ(AssetRegistry::instance().getAsset(id)->relativePath, QStringLiteral("Keep.wav"));
}

TEST_F(ProjectExplorerSourcesTabTest, FolderEditsRejectTraversalCyclesAndCollisionsWithoutSideEffects)
{
    auto& undo = UndoRedoManager::instance();
    undo.clear();
    EXPECT_FALSE(SourceFileSystem::createFolder({}, QStringLiteral("../Escape")));
    EXPECT_FALSE(SourceFileSystem::createFolder({}, QStringLiteral("..")));
    EXPECT_FALSE(SourceFileSystem::createFolder(QStringLiteral("../"), QStringLiteral("Escape")));
    EXPECT_FALSE(undo.canUndo());
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("A")));
    ASSERT_TRUE(SourceFileSystem::createFolder(QStringLiteral("A"), QStringLiteral("Child")));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("B")));
    ASSERT_TRUE(SourceFileSystem::createFolder(QStringLiteral("B"), QStringLiteral("A")));
    const int count = undo.undoStack()->count();
    EXPECT_FALSE(SourceFileSystem::move({QStringLiteral("A")}, QStringLiteral("A/Child")));
    EXPECT_FALSE(SourceFileSystem::move({QStringLiteral("A")}, QStringLiteral("B")));
    EXPECT_FALSE(SourceFileSystem::rename(QStringLiteral("A"), QStringLiteral("../B")));
    EXPECT_FALSE(SourceFileSystem::removeEmptyFolder(QStringLiteral("A")));
    EXPECT_EQ(undo.undoStack()->count(), count);
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("A/Child"))).exists());
    ASSERT_TRUE(SourceFileSystem::removeEmptyFolder(QStringLiteral("A/Child")));
    EXPECT_EQ(findRow(explorer_->sourcesTree(), QStringLiteral("Child")), nullptr);
    undo.undo();
    EXPECT_NE(findRow(explorer_->sourcesTree(), QStringLiteral("Child")), nullptr);
}

TEST_F(ProjectExplorerSourcesTabTest, SourceCreationAndRenameAvoidCollisionsAndKeepExtensions)
{
    QString actualName;
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Folder")));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Folder"), nullptr, &actualName));
    EXPECT_EQ(actualName, QStringLiteral("Folder_01"));
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Other")));
    ASSERT_TRUE(SourceFileSystem::rename(QStringLiteral("Other"), QStringLiteral("Folder"), nullptr, &actualName));
    EXPECT_EQ(actualName, QStringLiteral("Folder_02"));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_TRUE(QFileInfo::exists(SourceFileSystem::absolutePath(QStringLiteral("Other"))));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_TRUE(QFileInfo::exists(SourceFileSystem::absolutePath(QStringLiteral("Folder_02"))));

    for (const QString& name : {QStringLiteral("First.wav"), QStringLiteral("Second.wav")}) {
        QFile file(SourceFileSystem::absolutePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(name.toUtf8()), name.size());
    }
    ASSERT_TRUE(SourceFileSystem::rename(QStringLiteral("Second.wav"), QStringLiteral("First.wav"),
                                         nullptr, &actualName));
    EXPECT_EQ(actualName, QStringLiteral("First_01.wav"));
    QFile original(SourceFileSystem::absolutePath(QStringLiteral("First.wav")));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("First.wav"));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_TRUE(QFileInfo::exists(SourceFileSystem::absolutePath(QStringLiteral("Second.wav"))));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_TRUE(QFileInfo::exists(SourceFileSystem::absolutePath(QStringLiteral("First_01.wav"))));
}
