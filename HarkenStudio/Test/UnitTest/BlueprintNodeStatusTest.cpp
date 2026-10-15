#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/Components/VSound.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <QApplication>
#include <QBrush>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QLineEdit>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QTimer>

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace {

using namespace VNX;

void drainNodeStatusEvents()
{
    QApplication::processEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class BlueprintNodeStatusTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("BlueprintNodeStatus");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        auto* layer = create("Layer", ProjectItemType::Layer);
        container = create("Container", ProjectItemType::Container, layer);
        sound = create("Sound", ProjectItemType::Sound, container);
        bus = create("Bus", ProjectItemType::Bus, manager.getMasterBusItem());
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerProjectActions(actions, &manager, nullptr, {});
        UndoRedoManager::instance().clear();
        drainNodeStatusEvents();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
        drainNodeStatusEvents();
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, ProjectItemType type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        if (!item) throw std::runtime_error("Cannot create test object");
        return item;
    }

    NodeItem* nodeFor(BlueprintCanvasWidget& canvas, const ProjectItem* item)
    {
        const auto& graph = canvas.activeSignalPathSource()->graph();
        for (size_t index = 0; index < graph.nodes.size(); ++index)
            if (graph.nodes[index].key.objectGuid == item->guid)
                return canvas.signalPathNode(index);
        return nullptr;
    }

    void expectMatchingStatus(BlueprintCanvasWidget& canvas, Hierarchy& hierarchy, ProjectItem* item)
    {
        auto* node = nodeFor(canvas, item);
        ASSERT_NE(node, nullptr);
        ASSERT_TRUE(hierarchy.selectProjectItem(item));
        auto* row = hierarchy.treeWidget()->currentItem();
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(node->title(), item->name);
        EXPECT_EQ(node->titleDirty(), item->isDirtyObject());
        EXPECT_EQ(node->displayTitle(), row->text(0));
        EXPECT_EQ(node->titleDirty(), row->font(0).italic());
    }

    void refreshMediaStatus(uint32_t sourceID, MediaStatus expected)
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool refreshed = false;
        auto& pool = MediaPool::instance();
        QObject::connect(&pool, &MediaPool::statusCacheRefreshed, &loop, [&]() {
            refreshed = true;
            loop.quit();
        });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(5000);
        emit AssetRegistry::instance().assetModified(sourceID);
        pool.refreshStatusCacheAsync();
        loop.exec();
        ASSERT_TRUE(refreshed);
        ASSERT_EQ(pool.cachedStatus(sourceID), expected);
        drainNodeStatusEvents();
    }

    QTemporaryDir directory;
    ProjectItem* container = nullptr;
    ProjectItem* sound = nullptr;
    ProjectItem* bus = nullptr;
};

TEST_F(BlueprintNodeStatusTest, CreatedObjectsMatchHierarchyAndSavingClearsDecoration)
{
    Hierarchy hierarchy;
    BlueprintCanvasWidget canvas;
    canvas.rebuildFromProject();
    drainNodeStatusEvents();
    for (auto* item : {container, sound, bus}) {
        SCOPED_TRACE(item->name.toStdString());
        ASSERT_TRUE(item->isDirtyObject());
        expectMatchingStatus(canvas, hierarchy, item);
        EXPECT_EQ(nodeFor(canvas, item)->displayTitle(), item->name + QStringLiteral(" *"));
    }
    ASSERT_TRUE(ProjectManager::instance().saveProject());
    drainNodeStatusEvents();
    for (auto* item : {container, sound, bus}) {
        SCOPED_TRACE(item->name.toStdString());
        EXPECT_FALSE(item->isDirtyObject());
        expectMatchingStatus(canvas, hierarchy, item);
        EXPECT_EQ(nodeFor(canvas, item)->displayTitle(), item->name);
    }
}

TEST_F(BlueprintNodeStatusTest, PropertyPreviewCommitCancelAndUndoKeepHierarchyStatus)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    Hierarchy hierarchy;
    BlueprintCanvasWidget canvas;
    canvas.rebuildFromProject();
    drainNodeStatusEvents();
    for (auto* item : {container, sound, bus}) {
        SCOPED_TRACE(item->name.toStdString());
        ASSERT_TRUE(manager.saveProject());
        drainNodeStatusEvents();
        {
            PropertyEditSession cancelled(item, QStringLiteral("volume"), 0.0);
            cancelled.preview(-3.0);
            drainNodeStatusEvents();
            EXPECT_FALSE(item->isDirtyObject());
            expectMatchingStatus(canvas, hierarchy, item);
            cancelled.cancel();
        }
        drainNodeStatusEvents();
        EXPECT_FALSE(item->isDirtyObject());
        expectMatchingStatus(canvas, hierarchy, item);
        PropertyEditSession committed(item, QStringLiteral("volume"), 0.0);
        committed.preview(-6.0);
        committed.commit();
        drainNodeStatusEvents();
        EXPECT_TRUE(item->isDirtyObject());
        expectMatchingStatus(canvas, hierarchy, item);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drainNodeStatusEvents();
        expectMatchingStatus(canvas, hierarchy, item);
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        drainNodeStatusEvents();
        expectMatchingStatus(canvas, hierarchy, item);
        ASSERT_TRUE(manager.saveProject());
        drainNodeStatusEvents();
        EXPECT_FALSE(item->isDirtyObject());
        expectMatchingStatus(canvas, hierarchy, item);
    }
}

TEST_F(BlueprintNodeStatusTest, DirtyTitleEditingUsesRawNameAndSynchronizesRenameUndo)
{
    Hierarchy hierarchy;
    BlueprintCanvasWidget canvas;
    canvas.rebuildFromProject();
    drainNodeStatusEvents();
    for (auto* item : {container, sound, bus}) {
        SCOPED_TRACE(item->name.toStdString());
        auto* node = nodeFor(canvas, item);
        ASSERT_NE(node, nullptr);
        ASSERT_TRUE(node->titleDirty());
        const QString original = item->name;
        const QString renamed = original + QStringLiteral(" Renamed");
        node->beginTitleEditing();
        ASSERT_TRUE(node->isTitleEditing());
        ASSERT_NE(node->titleEditor(), nullptr);
        EXPECT_EQ(node->titleEditor()->text(), original);
        EXPECT_FALSE(node->titleEditor()->font().italic());
        node->titleEditor()->setText(renamed);
        node->finishTitleEditing(true);
        drainNodeStatusEvents();
        EXPECT_EQ(item->name, renamed);
        expectMatchingStatus(canvas, hierarchy, item);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drainNodeStatusEvents();
        EXPECT_EQ(item->name, original);
        expectMatchingStatus(canvas, hierarchy, item);
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        drainNodeStatusEvents();
        EXPECT_EQ(item->name, renamed);
        expectMatchingStatus(canvas, hierarchy, item);
        node = nodeFor(canvas, item);
        node->beginTitleEditing();
        EXPECT_EQ(node->titleEditor()->text(), renamed);
        node->titleEditor()->setText(QStringLiteral("Discarded"));
        node->finishTitleEditing(false);
        drainNodeStatusEvents();
        EXPECT_EQ(item->name, renamed);
        expectMatchingStatus(canvas, hierarchy, item);
    }
}

TEST_F(BlueprintNodeStatusTest, AssetRefreshPreservesTitleEditsUntilCommitOrCancel)
{
    auto& registry = AssetRegistry::instance();
    AssetInfo asset;
    asset.sourceID = registry.allocateStableID();
    asset.relativePath = QStringLiteral("external.wav");
    asset.absolutePath = QDir(registry.projectRoot()).filePath(QStringLiteral("AudioSources/external.wav"));
    ASSERT_TRUE(registry.registerAsset(asset, false));
    for (const bool chain : {false, true}) {
        SCOPED_TRACE(chain ? "signal chain" : "signal path");
        BlueprintCanvasWidget canvas;
        if (chain) canvas.useSignalChain(container);
        else canvas.rebuildFromProject();
        drainNodeStatusEvents();
        const auto currentNode = [&]() -> NodeItem* {
            for (auto* item : canvas.scene()->items())
                if (auto* node = dynamic_cast<NodeItem*>(item); node && node->objectID() == sound->objectID)
                    return node;
            return nullptr;
        };
        int refreshes = 0;
        QObject::connect(&canvas, &BlueprintCanvasWidget::signalChainViewChanged, &canvas, [&] { ++refreshes; });
        for (const bool committed : {true, false}) {
            SCOPED_TRACE(committed ? "commit" : "cancel");
            auto* node = currentNode();
            ASSERT_NE(node, nullptr);
            const QString original = sound->name;
            const QString edited = original + QStringLiteral(" Edited");
            node->beginTitleEditing();
            ASSERT_TRUE(node->isTitleEditing());
            node->titleEditor()->setText(edited);
            refreshes = 0;
            emit registry.assetModified(asset.sourceID);
            emit registry.assetModified(asset.sourceID);
            drainNodeStatusEvents();
            ASSERT_EQ(refreshes, 0);
            ASSERT_EQ(currentNode(), node);
            ASSERT_TRUE(node->isTitleEditing());
            EXPECT_EQ(node->titleEditor()->text(), edited);
            EXPECT_EQ(sound->name, original);
            node->finishTitleEditing(committed);
            drainNodeStatusEvents();
            EXPECT_GT(refreshes, 0);
            ASSERT_NE(currentNode(), nullptr);
            EXPECT_FALSE(currentNode()->isTitleEditing());
            EXPECT_EQ(sound->name, committed ? edited : original);
            EXPECT_EQ(currentNode()->title(), sound->name);
            if (committed) {
                ASSERT_TRUE(UndoRedoManager::instance().undo());
                drainNodeStatusEvents();
                EXPECT_EQ(sound->name, original);
                EXPECT_EQ(currentNode()->title(), original);
            }
        }
    }
}

TEST_F(BlueprintNodeStatusTest, SlowTitleClickRenamesAndInvalidInputStaysInPlace)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1000, 700);
    canvas.show();
    canvas.focusItem(sound);
    drainNodeStatusEvents();
    auto* node = nodeFor(canvas, sound);
    ASSERT_NE(node, nullptr);
    canvas.centerOn(node);
    node->setSelected(false);
    const QPoint point = canvas.mapFromScene(node->mapToScene(node->titleRect().center()));
    const auto mouse = [&](QEvent::Type type, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(point), QPointF(canvas.viewport()->mapToGlobal(point)),
                          Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas.viewport(), &event);
    };
    const auto wait = [] {
        QEventLoop loop;
        QTimer::singleShot(QApplication::doubleClickInterval() + 100, &loop, &QEventLoop::quit);
        loop.exec();
    };
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_FALSE(node->isTitleEditing());
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    QLineEdit otherWindow;
    otherWindow.show();
    QApplication::setActiveWindow(&otherWindow);
    otherWindow.setFocus();
    wait();
    EXPECT_FALSE(node->isTitleEditing());
    QApplication::setActiveWindow(&canvas);
    canvas.setFocus(Qt::MouseFocusReason);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_FALSE(node->isTitleEditing());
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    ASSERT_TRUE(node->isTitleEditing());
    auto* editor = node->titleEditor();
    editor->setText(QStringLiteral("Bad/Name"));
    EXPECT_TRUE(editor->property("inputInvalid").toBool());
    node->finishTitleEditing(true);
    EXPECT_TRUE(node->isTitleEditing());
    EXPECT_EQ(sound->name, QStringLiteral("Sound"));
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escape);
    EXPECT_FALSE(node->isTitleEditing());
    int opened = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &canvas,
                     [&](uint32_t id) { if (id == sound->objectID) ++opened; });
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    mouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(opened, 1);
    EXPECT_FALSE(node->isTitleEditing());
    QKeyEvent f2(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &f2);
    EXPECT_TRUE(node->isTitleEditing());
    node->finishTitleEditing(false);
}

TEST_F(BlueprintNodeStatusTest, ActiveSourceStatusColorsRefreshAndCoexistWithDirtySelectionPlayback)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& pool = MediaPool::instance();
    const auto root = QFileInfo(manager.currentProject()->projectPath).absolutePath();
    AssetInfo asset;
    asset.sourceID = 99001;
    asset.relativePath = QStringLiteral("node-status.wav");
    asset.absolutePath = QDir(root).filePath(QStringLiteral("AudioSources/node-status.wav"));
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 480;
    asset.metadata.durationSec = 0.01;
    ASSERT_TRUE(registry.registerAsset(asset));
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    auto* component = sound->comp<VSound>();
    ASSERT_NE(component, nullptr);
    component->sources.plain.entries = {source};
    component->sources.plain.active = source.entryId;
    emit manager.itemPropertyChanged(sound, QStringLiteral("sources"));
    Hierarchy hierarchy;
    BlueprintCanvasWidget canvas;
    canvas.rebuildFromProject();
    canvas.setSelectedProjectItems({sound});
    const auto expectColor = [&](const QColor& color) {
        auto* node = nodeFor(canvas, sound);
        ASSERT_NE(node, nullptr);
        node->setPlaybackActive(true);
        EXPECT_TRUE(node->isSelected());
        EXPECT_TRUE(node->playbackActive());
        EXPECT_TRUE(node->titleDirty());
        EXPECT_EQ(node->titleColor(), color);
        hierarchy.onItemPropertyChanged(sound, QStringLiteral("sources"));
        expectMatchingStatus(canvas, hierarchy, sound);
        const auto foreground = hierarchy.treeWidget()->currentItem()->data(0, Qt::ForegroundRole);
        if (color.isValid()) {
            ASSERT_TRUE(foreground.isValid());
            EXPECT_EQ(qvariant_cast<QBrush>(foreground).color(), color);
        } else {
            EXPECT_FALSE(foreground.isValid());
        }
        EXPECT_FALSE(nodeFor(canvas, container)->titleColor().isValid());
        EXPECT_FALSE(nodeFor(canvas, bus)->titleColor().isValid());
    };
    refreshMediaStatus(asset.sourceID, MediaStatus::Missing);
    expectColor(ThemeManager::instance().colors().error);
    ASSERT_TRUE(AssetConverter::writeFloatWav(
        asset.absolutePath, std::vector<float>(480, 0.25f), 1, 48000));
    refreshMediaStatus(asset.sourceID, MediaStatus::Imported);
    expectColor({});
    registry.markAsOrphaned(asset.sourceID);
    refreshMediaStatus(asset.sourceID, MediaStatus::Orphaned);
    expectColor(ThemeManager::instance().colors().textMuted);
    registry.restoreOrphaned(asset.sourceID);
    auto* stored = registry.getAsset(asset.sourceID);
    ASSERT_NE(stored, nullptr);
    auto& cache = stored->platformCaches[HarkenTest::PlatformIds::Windows];
    cache.valid = true;
    cache.cachePath = QStringLiteral("missing-node-status-cache.wav");
    refreshMediaStatus(asset.sourceID, MediaStatus::Outdated);
    expectColor(ThemeManager::instance().colors().warning);
    stored->platformCaches.clear();
    refreshMediaStatus(asset.sourceID, MediaStatus::Imported);
    expectColor({});
    AssetInfo missing = asset;
    missing.sourceID = 99002;
    missing.relativePath = QStringLiteral("inactive-missing.wav");
    missing.absolutePath = QDir(root).filePath(QStringLiteral("AudioSources/inactive-missing.wav"));
    ASSERT_TRUE(registry.registerAsset(missing));
    SoundSourceDef inactive;
    inactive.entryId = QUuid::createUuid();
    inactive.sourceId = missing.sourceID;
    component->sources.plain.entries.push_back(inactive);
    emit manager.itemPropertyChanged(sound, QStringLiteral("sources"));
    refreshMediaStatus(missing.sourceID, MediaStatus::Missing);
    ASSERT_EQ(pool.cachedStatus(asset.sourceID), MediaStatus::Imported);
    expectColor({});
    component->sources.plain.active = inactive.entryId;
    emit manager.itemPropertyChanged(sound, QStringLiteral("sources"));
    drainNodeStatusEvents();
    expectColor(ThemeManager::instance().colors().error);
    component->sources.plain.active = QUuid{};
    emit manager.itemPropertyChanged(sound, QStringLiteral("sources"));
    drainNodeStatusEvents();
    expectColor({});
}

}
