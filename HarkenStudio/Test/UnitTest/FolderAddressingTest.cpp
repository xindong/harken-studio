#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "AttenuationStore.h"
#include "IPC/IPCServer.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"
#include "Components/VSpatial.h"
#include "Components/VAction.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"
#include "Query/VQLEngine.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <algorithm>
#include <memory>
#include <string>

using namespace VNX;

namespace VNX {
ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);
QString buildItemFullPath(ProjectData* project, ProjectItem* item);
}

namespace {

using TestSupport::ensureApp;

QString rowLabel(const QTreeWidgetItem* row)
{
    QString text = row->text(0);
    if (text.endsWith(QStringLiteral(" *"))) text.chop(2);
    return text;
}

QTreeWidgetItem* findRowIn(QTreeWidgetItem* row, const QString& label)
{
    if (rowLabel(row) == label) return row;
    for (int i = 0; i < row->childCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(row->child(i), label)) return hit;
    }
    return nullptr;
}

QTreeWidgetItem* findRow(QTreeWidget* tree, const QString& label)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(tree->topLevelItem(i), label)) return hit;
    }
    return nullptr;
}

class FolderAddressProbe : public Hierarchy
{
public:
    using Hierarchy::deriveFolderPathForBucket;
};

class FolderAddressingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        panel_ = std::make_unique<FolderAddressProbe>();

        ProjectConfig config;
        config.name = QStringLiteral("FolderAddressProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        ProjectData* project = ProjectManager::instance().currentProject();
        ASSERT_NE(project, nullptr);
        projectPath_ = project->projectPath;
        projectRoot_ = QFileInfo(projectPath_).absolutePath();
    }

    void TearDown() override
    {
        panel_.reset();
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    PhysicalFolder* createFolder(const QString& name)
    {
        return ProjectManager::instance().createFolder(nullptr, name, QStringLiteral("Actions"));
    }

    void dropItemsOnRow(const QList<ProjectItem*>& sources, QTreeWidgetItem* targetRow)
    {
        ProjectTreeWidget* tree = panel_->treeWidget();
        tree->clearSelection();
        for (ProjectItem* source : sources) {
            QTreeWidgetItem* sourceRow = findRow(tree, source->name);
            ASSERT_NE(sourceRow, nullptr);
            sourceRow->setSelected(true);
        }
        ASSERT_TRUE(tree->canDropSelectionIntoFolderRow(targetRow));

        const auto address = panel_->folderAddressForRow(targetRow);
        ASSERT_TRUE(address.valid);
        for (ProjectItem* source : sources) {
            emit tree->internalMoveRequested(source, nullptr,
                                             address.bucketFolderName,
                                             address.relPath, true);
        }
    }

    void dropOnRow(ProjectItem* source, QTreeWidgetItem* targetRow)
    {
        dropItemsOnRow({ source }, targetRow);
    }

    QString actionFile(const QString& relPath) const
    {
        return QDir(projectRoot_).filePath(QStringLiteral("Actions/") + relPath);
    }

    QString actionFile(const ProjectItem* item, const QString& folder = {}) const
    {
        return actionFile((folder.isEmpty() ? QString() : folder + QLatin1Char('/'))
                          + NameValidator::objectFileName(item->name, item->guid));
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
    std::unique_ptr<FolderAddressProbe> panel_;
};

class FolderAutomationTest : public FolderAddressingTest
{
protected:
    void SetUp() override
    {
        FolderAddressingTest::SetUp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &ProjectManager::instance(),
                               nullptr, [this]() { return panel_.get(); });
    }

    void TearDown() override
    {
        ActionManager::instance().clearAllForTesting();
        FolderAddressingTest::TearDown();
    }
};

struct FolderBucketCase
{
    const char* label;
    const char* bucket;
    const char* itemName;
    ProjectItem::Type type;
};

class FolderBucketAddressingTest
    : public FolderAddressingTest
    , public ::testing::WithParamInterface<FolderBucketCase>
{
protected:
    QString bucket() const
    {
        return QString::fromLatin1(GetParam().bucket);
    }

    QString itemName() const
    {
        return QString::fromLatin1(GetParam().itemName);
    }

    PhysicalFolder* createBucketFolder(const QString& name)
    {
        return ProjectManager::instance().createFolder(nullptr, name, bucket());
    }

    ProjectItem* createPackRoot(const QString& path = QString())
    {
        return ProjectManager::instance().createItem(
            nullptr, itemName(), GetParam().type, path);
    }

    QString packFile(const QString& relPath) const
    {
        return QDir(projectRoot_).filePath(bucket() + QChar('/') + relPath);
    }

    QString packFile(const ProjectItem* item, const QString& folder = {}) const
    {
        return packFile((folder.isEmpty() ? QString() : folder + QLatin1Char('/'))
                        + NameValidator::objectFileName(item->name, item->guid));
    }
};

}

TEST_P(FolderBucketAddressingTest, PackRootCreatedOnFolderRowLandsAndReloadsThere)
{
    auto* folder = createBucketFolder(QStringLiteral("Target"));
    ASSERT_NE(folder, nullptr);

    ProjectTreeWidget* tree = panel_->treeWidget();
    QTreeWidgetItem* folderRow = findRow(tree, QStringLiteral("Target"));
    ASSERT_NE(folderRow, nullptr);
    tree->setCurrentItem(folderRow);
    const QString path = panel_->deriveFolderPathForBucket(bucket());
    EXPECT_EQ(path, QStringLiteral("Target"));

    ProjectItem* item = createPackRoot(path);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(item), QStringLiteral("Target"));
    EXPECT_EQ(item->parent, nullptr);

    QTreeWidgetItem* itemRow = findRow(tree, itemName());
    ASSERT_NE(itemRow, nullptr);
    EXPECT_EQ(itemRow->parent(), folderRow);

    const QUuid guid = item->guid;
    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(pm.saveProject());
    EXPECT_TRUE(QFileInfo::exists(
        packFile(item, QStringLiteral("Target"))));

    ASSERT_TRUE(pm.loadProject(projectPath_));
    ProjectItem* reloaded = pm.findItemByGuid(guid);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(reloaded), QStringLiteral("Target"));
    EXPECT_EQ(reloaded->parent, nullptr);
}

TEST_P(FolderBucketAddressingTest, SavedPackRootDraggedIntoFolderMovesOnDiskIntact)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* item = createPackRoot();
    ASSERT_NE(item, nullptr);
    struct AudioNode {
        QUuid guid;
        QUuid parentGuid;
        DefinitionFieldGroups groups;
    };
    std::vector<AudioNode> audioNodes;
    if (bucket() == QStringLiteral("Layers")) {
        auto groups = item->fieldGroups;
        ProjectItem* master = pm.getMasterBusItem();
        ASSERT_NE(master, nullptr);
        groups.mainOutput.shared = {
            FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(master->guid), -7.0f}};
        groups.spatialRating.shared = {
            FieldGroupMode::Local,
            SpatialRatingFieldGroup{0.25f, false, {}, 1.0f}};
        ASSERT_TRUE(pm.restoreFieldGroupsInternal(item, groups));
        audioNodes.push_back({item->guid, {}, item->fieldGroups});

        std::vector<ProjectItem::Type> childTypes;
        switch (item->type) {
        case ProjectItem::Type::Layer:
            childTypes = {ProjectItem::Type::Layer, ProjectItem::Type::Sound};
            break;
        case ProjectItem::Type::Container:
        case ProjectItem::Type::Track:
            childTypes = {ProjectItem::Type::Sound};
            break;
        case ProjectItem::Type::Piece:
            childTypes = {ProjectItem::Type::Track};
            break;
        default:
            break;
        }
        ProjectItem* parent = item;
        for (const auto type : childTypes) {
            ProjectItem* child = pm.createItem(
                parent, itemName() + QStringLiteral("_Child%1").arg(audioNodes.size()), type);
            ASSERT_NE(child, nullptr);
            audioNodes.push_back({child->guid, parent->guid, child->fieldGroups});
            parent = child;
        }
    }
    const auto originalGroups = buildFieldGroupEffectiveSnapshot(*pm.currentProject(), {});
    ASSERT_TRUE(originalGroups.ok());
    const auto expectAudioTree = [&](const QString& folder) {
        const auto currentGroups = buildFieldGroupEffectiveSnapshot(*pm.currentProject(), {});
        ASSERT_TRUE(currentGroups.ok());
        for (const auto& node : audioNodes) {
            ProjectItem* current = pm.findItemByGuid(node.guid);
            ASSERT_NE(current, nullptr);
            EXPECT_EQ(current->parent, pm.findItemByGuid(node.parentGuid));
            EXPECT_EQ(current->parentRef.kind, node.parentGuid.isNull()
                          ? ParentRef::Kind::Root : ParentRef::Kind::Item);
            EXPECT_EQ(current->parentRef.guid, node.parentGuid);
            EXPECT_EQ(current->fieldGroups, node.groups);
            const auto key = toFieldGroupGuid(node.guid);
            const auto& expected = originalGroups.definitions.at(key);
            const auto& actual = currentGroups.definitions.at(key);
            EXPECT_EQ(actual.mainOutput, expected.mainOutput);
            EXPECT_EQ(actual.send, expected.send);
            EXPECT_EQ(actual.spatialRating, expected.spatialRating);

            QFile file(packFile(current, folder));
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            const QJsonObject content = QJsonDocument::fromJson(file.readAll())
                                            .object().value(QStringLiteral("content")).toObject();
            const QJsonObject parentRef = content.value(QStringLiteral("parentRef")).toObject();
            EXPECT_EQ(parentRef.value(QStringLiteral("kind")).toString(),
                      node.parentGuid.isNull()
                          ? QStringLiteral("root")
                          : QStringLiteral("item"));
            if (!node.parentGuid.isNull()) {
                EXPECT_EQ(parentRef.value(QStringLiteral("id")).toString(),
                          node.parentGuid.toString(QUuid::WithoutBraces));
            }
            EXPECT_FALSE(QFileInfo::exists(packFile(
                current, folder.isEmpty() ? QStringLiteral("Target") : QString())));
        }
    };
    ASSERT_TRUE(pm.saveProject());
    ASSERT_NO_FATAL_FAILURE(expectAudioTree(QString()));

    QFile before(packFile(item));
    ASSERT_TRUE(before.open(QIODevice::ReadOnly));
    const QByteArray payload = before.readAll();
    before.close();

    ASSERT_NE(createBucketFolder(QStringLiteral("Target")), nullptr);
    QTreeWidgetItem* folderRow = findRow(panel_->treeWidget(), QStringLiteral("Target"));
    ASSERT_NE(folderRow, nullptr);
    ASSERT_NO_FATAL_FAILURE(dropOnRow(item, folderRow));

    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(item), QStringLiteral("Target"));
    EXPECT_EQ(item->parent, nullptr);
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_NE(findFileBindingForItem(*project, item), nullptr);

    const QUuid guid = item->guid;
    ASSERT_TRUE(pm.saveProject());
    ASSERT_NO_FATAL_FAILURE(expectAudioTree(QStringLiteral("Target")));
    EXPECT_FALSE(QFileInfo::exists(packFile(item)));

    const QString movedFilePath = packFile(item, QStringLiteral("Target"));
    ProjectFileBinding* movedBinding = findFileBindingForItem(*project, item);
    ASSERT_NE(movedBinding, nullptr);
    EXPECT_EQ(QDir::cleanPath(project->objectFilePath(item)), QDir::cleanPath(movedFilePath));

    QFile after(movedFilePath);
    ASSERT_TRUE(after.open(QIODevice::ReadOnly));
    const QByteArray movedPayload = after.readAll();
    after.close();

    QJsonParseError beforeError;
    QJsonParseError afterError;
    QJsonDocument beforeDocument = QJsonDocument::fromJson(payload, &beforeError);
    QJsonDocument afterDocument = QJsonDocument::fromJson(movedPayload, &afterError);
    ASSERT_EQ(beforeError.error, QJsonParseError::NoError);
    ASSERT_EQ(afterError.error, QJsonParseError::NoError);
    ASSERT_TRUE(beforeDocument.isObject());
    ASSERT_TRUE(afterDocument.isObject());
    QJsonObject beforeContent =
        beforeDocument.object().value(QStringLiteral("content")).toObject();
    QJsonObject afterContent =
        afterDocument.object().value(QStringLiteral("content")).toObject();
    EXPECT_FALSE(beforeContent.contains(QStringLiteral("path")));
    EXPECT_FALSE(afterContent.contains(QStringLiteral("path")));
    beforeContent.remove(QStringLiteral("parentRef"));
    afterContent.remove(QStringLiteral("parentRef"));
    EXPECT_EQ(beforeContent, afterContent);

    EXPECT_TRUE(UndoRedoManager::instance().canUndo());
    EXPECT_TRUE(QFileInfo(movedFilePath).isFile());

    ASSERT_TRUE(pm.loadProject(projectPath_));
    ProjectItem* reloaded = pm.findItemByGuid(guid);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(reloaded), QStringLiteral("Target"));
    ASSERT_NO_FATAL_FAILURE(expectAudioTree(QStringLiteral("Target")));
}

TEST_P(FolderBucketAddressingTest, FailedRelocationPreservesLastGoodPack)
{
    auto& pm = ProjectManager::instance();
    auto* folder = createBucketFolder(QStringLiteral("Original"));
    ProjectItem* item = createPackRoot(QStringLiteral("Original"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(item, nullptr);
    ASSERT_TRUE(pm.saveProject());

    const QString originalFile = packFile(item, QStringLiteral("Original"));
    ASSERT_TRUE(QFileInfo(originalFile).isFile());
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    ProjectFileBinding* binding = findFileBindingForItem(*project, item);
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(QDir::cleanPath(project->objectFilePath(item)), QDir::cleanPath(originalFile));

    const QString blockedPath = packFile(QStringLiteral("Blocked"));
    QFile blocker(blockedPath);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const QUuid itemGuid = item->guid;
    ASSERT_TRUE(pm.loadProject(projectPath_));
    project = pm.currentProject();
    folder = project->findFolderByPath(bucket(), QStringLiteral("Original"));
    ASSERT_NE(folder, nullptr);
    item = pm.findItemByGuid(itemGuid);
    ASSERT_NE(item, nullptr);
    const int index = UndoRedoManager::instance().undoStack()->index();
    ASSERT_TRUE(pm.renameFolder(folder, QStringLiteral("Blocked")));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), index + 1);
    EXPECT_FALSE(pm.saveProject());
    EXPECT_TRUE(QFileInfo(originalFile).isFile());
    EXPECT_TRUE(QFileInfo(blockedPath).isFile());
    EXPECT_EQ(folder->name, QStringLiteral("Blocked"));
    EXPECT_EQ(QDir::cleanPath(project->objectFilePath(item)), QDir::cleanPath(originalFile));
    EXPECT_FALSE(UndoRedoManager::instance().isClean());
}

TEST_P(FolderBucketAddressingTest, EmptyNestedFolderTreeRoundTrips)
{
    auto& pm = ProjectManager::instance();
    auto* root = createBucketFolder(QStringLiteral("Root"));
    ASSERT_NE(root, nullptr);
    auto* levelOne = pm.createFolder(root, QStringLiteral("LevelOne"), pm.currentProject()->folderBucket(root));
    ASSERT_NE(levelOne, nullptr);
    ASSERT_NE(pm.createFolder(levelOne, QStringLiteral("LevelTwo"), pm.currentProject()->folderBucket(levelOne)),
              nullptr);

    ASSERT_TRUE(pm.saveProject());
    EXPECT_TRUE(QFileInfo(packFile(QStringLiteral("Root/LevelOne/LevelTwo"))).isDir());
    ASSERT_TRUE(pm.loadProject(projectPath_));

    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    auto* reloadedRoot = project->findFolderByPath(
        bucket(), QStringLiteral("Root"));
    auto* reloadedLevelOne = project->findFolderByPath(
        bucket(), QStringLiteral("Root/LevelOne"));
    auto* reloadedLevelTwo = project->findFolderByPath(
        bucket(), QStringLiteral("Root/LevelOne/LevelTwo"));
    ASSERT_NE(reloadedRoot, nullptr);
    ASSERT_NE(reloadedLevelOne, nullptr);
    ASSERT_NE(reloadedLevelTwo, nullptr);
    EXPECT_EQ(reloadedLevelOne->parent, reloadedRoot);
    EXPECT_EQ(reloadedLevelTwo->parent, reloadedLevelOne);
    EXPECT_TRUE(reloadedLevelTwo->children.empty());
}

TEST_F(FolderAddressingTest, FailedSaveKeepsCommittedProjectFile)
{
    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(pm.saveProject());
    auto* folder = createFolder(QStringLiteral("Uncommitted"));
    ASSERT_NE(folder, nullptr);

    QFile rootFile(projectPath_);
    ASSERT_TRUE(rootFile.open(QIODevice::ReadOnly));
    const QByteArray committedProject = rootFile.readAll();
    rootFile.close();

    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Blocked"), ProjectItem::Type::Action,
        QStringLiteral("Uncommitted"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(pm.createFolder(folder, NameValidator::objectFileName(action->name, action->guid),
                               QStringLiteral("Actions")), nullptr);

    EXPECT_FALSE(pm.saveProject());
    ASSERT_TRUE(rootFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(rootFile.readAll(), committedProject);
}

TEST_F(FolderAddressingTest, FailedSaveRestoresEarlierPackWrites)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(
        nullptr, QStringLiteral("Play_First"), ProjectItem::Type::Action);
    ASSERT_NE(first, nullptr);
    ProjectItem* blocked = pm.createItem(
        nullptr, QStringLiteral("Play_Blocked"), ProjectItem::Type::Action,
        QStringLiteral("Blocked"));
    ASSERT_NE(blocked, nullptr);
    ASSERT_TRUE(pm.saveProject());

    const QString firstPath = actionFile(first);
    QFile firstFile(firstPath);
    ASSERT_TRUE(firstFile.open(QIODevice::ReadOnly));
    const QByteArray committedPack = firstFile.readAll();
    firstFile.close();
    const QString committedBindingPath = pm.currentProject()->objectFilePath(first);

    first->name = QStringLiteral("Play_First_Changed");
    first->markDirty();
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Blocked"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(pm.createFolder(folder, NameValidator::objectFileName(blocked->name, blocked->guid),
                               QStringLiteral("Actions")), nullptr);

    EXPECT_FALSE(pm.saveProject());
    EXPECT_TRUE(QFileInfo(firstPath).isFile());
    EXPECT_FALSE(QFileInfo::exists(actionFile(first)));
    EXPECT_EQ(pm.currentProject()->objectFilePath(first), committedBindingPath);
    ASSERT_TRUE(firstFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(firstFile.readAll(), committedPack);
}

TEST_F(FolderAddressingTest, FailedSaveRestoresAssetRegistry)
{
    auto& pm = ProjectManager::instance();
    ASSERT_NE(createFolder(QStringLiteral("Blocked")), nullptr);
    ASSERT_TRUE(pm.saveProject());

    const QString registryPath = QDir(PathUtils::getAssetRegistryCacheDir(projectRoot_))
                                     .filePath(QStringLiteral("assets.json"));
    QFile registryFile(registryPath);
    ASSERT_TRUE(registryFile.open(QIODevice::ReadOnly));
    const QByteArray committedRegistry = registryFile.readAll();
    registryFile.close();

    AssetInfo asset;
    asset.sourceID = AssetRegistry::instance().allocateStableID();
    asset.relativePath = QStringLiteral("uncommitted.wav");
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    ProjectItem* blocked = pm.createItem(
        nullptr, QStringLiteral("Play_Blocked"), ProjectItem::Type::Action,
        QStringLiteral("Blocked"));
    ASSERT_NE(blocked, nullptr);
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Blocked"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(pm.createFolder(folder, NameValidator::objectFileName(blocked->name, blocked->guid),
                               QStringLiteral("Actions")), nullptr);

    EXPECT_FALSE(pm.saveProject());
    ASSERT_TRUE(registryFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(registryFile.readAll(), committedRegistry);
    EXPECT_TRUE(AssetRegistry::instance().isUncommitted(asset.sourceID));
}

TEST_F(FolderAddressingTest, MultipleActionsMoveToFolderWithoutTreeRowReuse)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(nullptr, QStringLiteral("Play_Alarm"),
                                       ProjectItem::Type::Action);
    ProjectItem* second = pm.createItem(nullptr, QStringLiteral("Play_Siren"),
                                        ProjectItem::Type::Action);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(createFolder(QStringLiteral("Combat")), nullptr);

    QTreeWidgetItem* folderRow = findRow(panel_->treeWidget(), QStringLiteral("Combat"));
    ASSERT_NE(folderRow, nullptr);
    ASSERT_NO_FATAL_FAILURE(dropItemsOnRow({ first, second }, folderRow));

    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(first), QStringLiteral("Combat"));
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(second), QStringLiteral("Combat"));
    EXPECT_NE(findRow(panel_->treeWidget(), QStringLiteral("Play_Alarm")), nullptr);
    EXPECT_NE(findRow(panel_->treeWidget(), QStringLiteral("Play_Siren")), nullptr);
}

TEST_F(FolderAddressingTest, FolderAddressChangesPreserveAuthoringHistory)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* action = pm.createItem(nullptr, QStringLiteral("Play_Alarm"),
                                        ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(createFolder(QStringLiteral("Combat")), nullptr);

    ProjectTreeWidget* tree = panel_->treeWidget();
    QTreeWidgetItem* folderRow = findRow(tree, QStringLiteral("Combat"));
    ASSERT_NE(folderRow, nullptr);
    ASSERT_NO_FATAL_FAILURE(dropOnRow(action, folderRow));
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(action), QStringLiteral("Combat"));

    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(pm.currentProject()->itemFolderPath(action), QString());
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(pm.currentProject()->itemFolderPath(action), QStringLiteral("Combat"));

    QTreeWidgetItem* bucketRow = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const auto address = panel_->folderAddressForRow(tree->topLevelItem(i));
        if (address.isBucketRoot
            && address.bucketFolderName == QLatin1String(ItemTypeCatalog::kActionsFolderName)) {
            bucketRow = tree->topLevelItem(i);
            break;
        }
    }
    ASSERT_NE(bucketRow, nullptr);
    ASSERT_NO_FATAL_FAILURE(dropOnRow(action, bucketRow));
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(action), QString());
}

TEST_F(FolderAddressingTest, FolderMembershipLeavesAudioParentageIndependent)
{
    auto& pm = ProjectManager::instance();
    auto* folder = pm.createFolder(nullptr, QStringLiteral("Organization"), QStringLiteral("Layers"));
    ASSERT_NE(folder, nullptr);
    auto* layer = pm.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer,
                                QStringLiteral("Organization"));
    ASSERT_NE(layer, nullptr);
    auto* sound = pm.createItem(layer, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(layer->parent, nullptr);
    EXPECT_EQ(sound->parent, layer);
    EXPECT_TRUE(folder->children.empty());
    EXPECT_EQ(pm.currentProject()->folderForItem(layer), folder);
    EXPECT_EQ(pm.currentProject()->folderForItem(sound), folder);
}

TEST_F(FolderAddressingTest, CrossFolderParentMoveSharesOneUndoAndSaveBoundary)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* first = pm.createItem(nullptr, QStringLiteral("First"), ProjectItem::Type::Layer, QStringLiteral("Source"));
    auto* second = pm.createItem(nullptr, QStringLiteral("Second"), ProjectItem::Type::Layer, QStringLiteral("Destination"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* child = pm.createItem(first, QStringLiteral("Child"), ProjectItem::Type::Layer);
    ASSERT_NE(child, nullptr);
    auto* sound = pm.createItem(child, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(pm.saveProject());
    undo.clear();
    const QString originalChildPath = pm.currentProject()->objectFilePath(child);
    const QString originalSoundPath = pm.currentProject()->objectFilePath(sound);
    const QUuid childGuid = child->guid;
    const QUuid soundGuid = sound->guid;
    const QUuid parentGuid = second->guid;
    ASSERT_TRUE(pm.renameItem(child, QStringLiteral("PendingName")));
    const int beforeMove = undo.undoStack()->index();
    ASSERT_TRUE(pm.moveItems({child}, second));
    EXPECT_EQ(undo.undoStack()->index(), beforeMove + 1);
    EXPECT_EQ(child->parent, second);
    EXPECT_EQ(sound->parent, child);
    EXPECT_EQ(pm.currentProject()->itemFolderPath(child), QStringLiteral("Destination"));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(sound), QStringLiteral("Destination"));
    EXPECT_TRUE(QFileInfo::exists(originalChildPath));
    EXPECT_TRUE(QFileInfo::exists(originalSoundPath));
    EXPECT_EQ(pm.currentProject()->objectFilePath(child), originalChildPath);
    EXPECT_TRUE(child->isDirtyObject());
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(child->parent, first);
    EXPECT_EQ(child->name, QStringLiteral("PendingName"));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(child), QStringLiteral("Source"));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(sound), QStringLiteral("Source"));
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(pm.saveProject());
    const QString savedChildPath = pm.currentProject()->objectFilePath(child);
    EXPECT_FALSE(QFileInfo::exists(originalChildPath));
    EXPECT_FALSE(QFileInfo::exists(originalSoundPath));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(child->parent, first);
    EXPECT_EQ(pm.currentProject()->itemFolderPath(child), QStringLiteral("Source"));
    EXPECT_TRUE(QFileInfo::exists(savedChildPath));
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    child = pm.findItemByGuid(childGuid);
    sound = pm.findItemByGuid(soundGuid);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(child->name, QStringLiteral("PendingName"));
    EXPECT_EQ(child->parent, pm.findItemByGuid(parentGuid));
    EXPECT_EQ(sound->parent, child);
    EXPECT_EQ(pm.currentProject()->itemFolderPath(child), QStringLiteral("Destination"));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(sound), QStringLiteral("Destination"));
}

TEST_F(FolderAddressingTest, DeletingFormerFolderPreservesObjectsMovedOutBeforeSave)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(nullptr, QStringLiteral("First"), ProjectItem::Type::Layer,
                                QStringLiteral("Source"));
    auto* second = pm.createItem(nullptr, QStringLiteral("Second"), ProjectItem::Type::Layer,
                                 QStringLiteral("Destination"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* sound = pm.createItem(first, QStringLiteral("Moved"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(pm.saveProject());
    const QUuid firstGuid = first->guid;
    const QUuid secondGuid = second->guid;
    const QUuid soundGuid = sound->guid;
    const QString originalPath = pm.currentProject()->objectFilePath(sound);
    auto* source = pm.currentProject()->findFolderByPath(QStringLiteral("Layers"), QStringLiteral("Source"));
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(pm.moveItems({sound}, second));
    ASSERT_TRUE(pm.deleteEntries({}, {source}));
    EXPECT_EQ(pm.findItemByGuid(firstGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(soundGuid), sound);
    EXPECT_EQ(sound->parent, second);
    EXPECT_TRUE(QFileInfo(originalPath).isFile());
    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(originalPath));
    EXPECT_TRUE(QFileInfo(pm.currentProject()->objectFilePath(sound)).isFile());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    EXPECT_EQ(pm.findItemByGuid(firstGuid), nullptr);
    sound = pm.findItemByGuid(soundGuid);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->parent, pm.findItemByGuid(secondGuid));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(sound), QStringLiteral("Destination"));
}

TEST_F(FolderAddressingTest, DeletingDestinationRemovesObjectsMovedInBeforeSave)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(nullptr, QStringLiteral("First"), ProjectItem::Type::Layer,
                                QStringLiteral("Source"));
    auto* second = pm.createItem(nullptr, QStringLiteral("Second"), ProjectItem::Type::Layer,
                                 QStringLiteral("Destination"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* sound = pm.createItem(first, QStringLiteral("Moved"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(pm.saveProject());
    const QUuid firstGuid = first->guid;
    const QUuid secondGuid = second->guid;
    const QUuid soundGuid = sound->guid;
    const QString originalPath = pm.currentProject()->objectFilePath(sound);
    auto* destination = pm.currentProject()->findFolderByPath(QStringLiteral("Layers"), QStringLiteral("Destination"));
    ASSERT_NE(destination, nullptr);
    ASSERT_TRUE(pm.moveItems({sound}, second));
    ASSERT_TRUE(pm.deleteEntries({}, {destination}));
    EXPECT_EQ(pm.findItemByGuid(soundGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(firstGuid), first);
    EXPECT_TRUE(QFileInfo(originalPath).isFile());
    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(originalPath));
    ASSERT_TRUE(pm.loadProject(projectPath_));
    EXPECT_NE(pm.findItemByGuid(firstGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(secondGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(soundGuid), nullptr);
}

TEST_F(FolderAddressingTest, SameFolderParentMoveKeepsOrdinaryUndoAndSaveBoundary)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_NE(pm.createFolder(nullptr, QStringLiteral("Shared"), QStringLiteral("Layers")), nullptr);
    auto* first = pm.createItem(nullptr, QStringLiteral("First"), ProjectItem::Type::Layer, QStringLiteral("Shared"));
    auto* second = pm.createItem(nullptr, QStringLiteral("Second"), ProjectItem::Type::Layer, QStringLiteral("Shared"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* child = pm.createItem(first, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(pm.saveProject());
    undo.clear();
    const QString originalPath = pm.currentProject()->objectFilePath(child);
    QFile saved(originalPath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = saved.readAll();
    saved.close();
    ASSERT_TRUE(pm.moveItems({child}, second));
    EXPECT_EQ(child->parent, second);
    EXPECT_EQ(pm.currentProject()->objectFilePath(child), originalPath);
    EXPECT_TRUE(child->isDirtyObject());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.readAll(), originalBytes);
    saved.close();
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(child->parent, first);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(child->parent, second);
}

TEST_F(FolderAddressingTest, CreatedFoldersUseFolderBindingsInEverySupportedBucket)
{
    auto& pm = ProjectManager::instance();
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const size_t actionCount = project->actions.size();
    const size_t variableCount = project->variables.size();
    const size_t attenuationCount = project->attenuationItems.size();

    auto* folder = createFolder(QStringLiteral("Combat"));
    ASSERT_NE(folder, nullptr);
    auto* variableFolder = pm.createFolder(nullptr, QStringLiteral("Gameplay"), QStringLiteral("Variables"));
    ASSERT_NE(variableFolder, nullptr);
    auto* attenuationFolder = pm.createFolder(nullptr, QStringLiteral("World"), QStringLiteral("Attenuations"));
    ASSERT_NE(attenuationFolder, nullptr);

    EXPECT_EQ(project->actions.size(), actionCount);
    EXPECT_EQ(project->variables.size(), variableCount);
    EXPECT_EQ(project->attenuationItems.size(), attenuationCount);
    ASSERT_EQ(project->folderRootsForBucket(QStringLiteral("Actions")).size(), 1u);
    EXPECT_EQ(project->folderRootsForBucket(QStringLiteral("Actions")).front(), folder);
    ASSERT_EQ(project->folderRootsForBucket(QStringLiteral("Variables")).size(), 1u);
    EXPECT_EQ(project->folderRootsForBucket(QStringLiteral("Variables")).front(),
              variableFolder);
    ASSERT_EQ(project->folderRootsForBucket(QStringLiteral("Attenuations")).size(), 1u);
    EXPECT_EQ(project->folderRootsForBucket(QStringLiteral("Attenuations")).front(),
              attenuationFolder);

    auto* nested = pm.createFolder(folder, QStringLiteral("Boss"), pm.currentProject()->folderBucket(folder));
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->parent, folder);
    EXPECT_EQ(project->folderRelPath(nested), QStringLiteral("Combat/Boss"));
}

TEST_F(FolderAddressingTest, RebuildKeepsEmptyFolderRowsBackedByTheModel)
{
    auto* folder = createFolder(QStringLiteral("Empty"));
    ASSERT_NE(folder, nullptr);

    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    panel_->onProjectOpened(project);

    QTreeWidgetItem* folderRow = findRow(panel_->treeWidget(), QStringLiteral("Empty"));
    ASSERT_NE(folderRow, nullptr);
    EXPECT_EQ(folderRow->childCount(), 0);
    EXPECT_TRUE(panel_->selectFolder(folder));
    EXPECT_EQ(panel_->selectedFolder(), folder);
}

TEST_F(FolderAddressingTest, RebuildUsesFolderMembershipEstablishedAtCreation)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Legacy"), ProjectItem::Type::Action,
        QStringLiteral("Legacy/Nested"));
    ASSERT_NE(action, nullptr);

    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    QTreeWidgetItem* createdRow = findRow(panel_->treeWidget(), QStringLiteral("Play_Legacy"));
    QTreeWidgetItem* createdFolderRow = findRow(panel_->treeWidget(), QStringLiteral("Nested"));
    ASSERT_NE(createdRow, nullptr);
    ASSERT_NE(createdFolderRow, nullptr);
    EXPECT_EQ(createdRow->parent(), createdFolderRow);
    panel_->onProjectOpened(project);

    auto* legacy = project->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("Legacy"));
    auto* nested = project->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("Legacy/Nested"));
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->parent, legacy);
    EXPECT_TRUE(panel_->selectFolder(legacy));
    EXPECT_TRUE(panel_->selectFolder(nested));

    QTreeWidgetItem* actionRow = findRow(panel_->treeWidget(), QStringLiteral("Play_Legacy"));
    QTreeWidgetItem* nestedRow = findRow(panel_->treeWidget(), QStringLiteral("Nested"));
    ASSERT_NE(actionRow, nullptr);
    ASSERT_NE(nestedRow, nullptr);
    EXPECT_EQ(actionRow->parent(), nestedRow);
}

TEST_F(FolderAddressingTest, SaveRejectsMissingFolderMembershipWithoutMovingCommittedObject)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Headless"), ProjectItem::Type::Action,
        QStringLiteral("Recovered/Nested"));
    ASSERT_NE(action, nullptr);

    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    auto* folder = project->folderForItem(action);
    ASSERT_NE(folder, nullptr);
    const QUuid actionGuid = action->guid;
    ASSERT_TRUE(pm.saveProject());
    const QString savedPath = actionFile(action, QStringLiteral("Recovered/Nested"));
    QFile saved(savedPath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray before = saved.readAll();
    saved.close();
    panel_.reset();
    const auto detachedFolders = std::move(project->folders);
    ASSERT_FALSE(detachedFolders.empty());

    EXPECT_FALSE(pm.saveProject());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.readAll(), before);
    saved.close();
    EXPECT_FALSE(QFileInfo::exists(actionFile(action)));

    ASSERT_TRUE(pm.loadProject(projectPath_));
    auto* recovered = pm.currentProject()->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("Recovered/Nested"));
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(pm.currentProject()->folderForItem(pm.findItemByGuid(actionGuid)), recovered);
}

TEST_F(FolderAddressingTest, FolderEditsKeepPendingAuthoringUntilSave)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* folder = createFolder(QStringLiteral("Physical"));
    ASSERT_NE(folder, nullptr);
    auto* item = pm.createItem(nullptr, QStringLiteral("Pending"), ProjectItem::Type::Action);
    ASSERT_NE(item, nullptr);
    ASSERT_TRUE(pm.saveProject());
    item->extensions = {{QStringLiteral("test"), QStringLiteral("Pending authoring")}};
    item->markDirty();
    const int historyCount = undo.undoStack()->count();
    ASSERT_TRUE(pm.renameFolder(folder, QStringLiteral("Renamed")));
    EXPECT_FALSE(undo.isClean());
    EXPECT_TRUE(item->isDirtyObject());
    EXPECT_EQ(undo.undoStack()->count(), historyCount + 1);
    EXPECT_TRUE(QFileInfo(actionFile(QStringLiteral("Physical"))).isDir());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Renamed"))));
    QFile saved(actionFile(item));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    auto content = QJsonDocument::fromJson(saved.readAll()).object().value(QStringLiteral("content")).toObject();
    saved.close();
    EXPECT_FALSE(content.value(QStringLiteral("extensions")).toObject().contains(QStringLiteral("test")));
    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Physical"))));
    EXPECT_TRUE(QFileInfo(actionFile(QStringLiteral("Renamed"))).isDir());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    content = QJsonDocument::fromJson(saved.readAll()).object().value(QStringLiteral("content")).toObject();
    EXPECT_EQ(content.value(QStringLiteral("extensions")).toObject().value(QStringLiteral("test")).toString(),
              QStringLiteral("Pending authoring"));
}

#ifdef HARKEN_HAS_LMDB
TEST_F(FolderAddressingTest, SearchIndexFollowsFolderEditsAndUndoWithoutSaving)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* folder = createFolder(QStringLiteral("Combat"));
    auto* target = createFolder(QStringLiteral("Target"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(target, nullptr);
    auto* direct = pm.createItem(nullptr, QStringLiteral("Direct"),
        ProjectItem::Type::Action, QStringLiteral("Combat"));
    auto* nested = pm.createItem(nullptr, QStringLiteral("NestedEntry"),
        ProjectItem::Type::Action, QStringLiteral("Combat/Nested"));
    auto* unrelated = pm.createItem(nullptr, QStringLiteral("Untouched"),
        ProjectItem::Type::Action, QStringLiteral("Elsewhere"));
    ASSERT_NE(direct, nullptr);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(unrelated, nullptr);
    ASSERT_TRUE(QDir().mkpath(PathUtils::getSearchIndexCacheDir(projectRoot_)));

    IPCServer server(nullptr);
    server.setProjectManager(&pm);
    auto* engine = server.vqlEngine();
    engine->setProject(pm.currentProject());
    ASSERT_NE(engine->lmdbService(), nullptr);
    ASSERT_TRUE(engine->lmdbService()->isOpen());
    const auto search = [&](const QString& query) {
        LMDBQueryPipeline pipeline(engine->lmdbService());
        pipeline.setQuery(query);
        while (!pipeline.isFinished())
            EXPECT_NE(pipeline.step(), PipelineStatus::Error);
        QSet<uint32_t> ids;
        for (const auto& result : pipeline.results()) ids.insert(result.objectID);
        return ids;
    };
    const QSet<uint32_t> affected{direct->objectID, nested->objectID};
    EXPECT_EQ(search(QStringLiteral("combat")), affected);
    EXPECT_EQ(search(QStringLiteral("omb")), affected);

    ASSERT_TRUE(pm.renameFolder(folder, QStringLiteral("Harbor")));
    EXPECT_EQ(search(QStringLiteral("harbor")), affected);
    EXPECT_TRUE(search(QStringLiteral("combat")).isEmpty());
    EXPECT_TRUE(search(QStringLiteral("omb")).isEmpty());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(search(QStringLiteral("combat")), affected);
    EXPECT_TRUE(search(QStringLiteral("harbor")).isEmpty());
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(search(QStringLiteral("harbor")), affected);
    EXPECT_TRUE(search(QStringLiteral("combat")).isEmpty());

    ASSERT_TRUE(pm.moveFolder(folder, target, QStringLiteral("Actions")));
    EXPECT_EQ(search(QStringLiteral("target")), affected);
    ASSERT_TRUE(undo.undo());
    EXPECT_TRUE(search(QStringLiteral("target")).isEmpty());
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(search(QStringLiteral("target")), affected);

    ASSERT_TRUE(pm.moveEntriesToFolder({direct}, {}, QStringLiteral("Actions"), QStringLiteral("Elsewhere")));
    EXPECT_EQ(search(QStringLiteral("harbor")), QSet<uint32_t>{nested->objectID});
    EXPECT_EQ(search(QStringLiteral("elsewhere")), (QSet<uint32_t>{direct->objectID, unrelated->objectID}));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(search(QStringLiteral("harbor")), affected);
    EXPECT_EQ(search(QStringLiteral("elsewhere")), QSet<uint32_t>{unrelated->objectID});
}
#endif

TEST_F(FolderAddressingTest, FailedSaveRetainsPendingDirectoryEditsAndCommittedDisk)
{
    auto& pm = ProjectManager::instance();
    auto* source = createFolder(QStringLiteral("Source"));
    auto* target = createFolder(QStringLiteral("Target"));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    auto* blocked = pm.createItem(nullptr, QStringLiteral("Blocked"), ProjectItem::Type::Action);
    ASSERT_NE(blocked, nullptr);
    ASSERT_TRUE(pm.saveProject());
    ASSERT_NE(createFolder(NameValidator::objectFileName(blocked->name, blocked->guid)), nullptr);
    auto* created = createFolder(QStringLiteral("NewFolder"));
    ASSERT_NE(created, nullptr);
    ASSERT_TRUE(pm.renameFolder(source, QStringLiteral("Renamed")));
    ASSERT_TRUE(pm.moveFolder(source, target, QStringLiteral("Actions")));
    const int historyIndex = UndoRedoManager::instance().undoStack()->index();
    EXPECT_FALSE(pm.saveProject());
    EXPECT_TRUE(QFileInfo(actionFile(QStringLiteral("Source"))).isDir());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Target/Renamed"))));
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("NewFolder"))));
    EXPECT_EQ(source->parent, target);
    EXPECT_EQ(source->name, QStringLiteral("Renamed"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), historyIndex);
    EXPECT_FALSE(UndoRedoManager::instance().isClean());
}

TEST_F(FolderAddressingTest, PhysicalDiskFolderLoadsAndRemainsARealDirectory)
{
    const QString legacyPath = actionFile(QStringLiteral("LegacyDisk"));
    ASSERT_TRUE(QDir().mkpath(legacyPath));

    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(pm.loadProject(projectPath_));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    auto* legacy = project->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("LegacyDisk"));
    ASSERT_NE(legacy, nullptr);

    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    auto* reloaded = pm.currentProject()->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("LegacyDisk"));
    ASSERT_NE(reloaded, nullptr);
}

TEST_F(FolderAddressingTest, UnavailableDiskDoesNotPreventUndoableDirectoryCreation)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    const QString actionsPath = QDir(projectRoot_).filePath(QStringLiteral("Actions"));
    ASSERT_TRUE(QDir(actionsPath).removeRecursively());
    QFile blocker(actionsPath);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const int index = undo.undoStack()->index();
    auto* folder = createFolder(QStringLiteral("Blocked"));
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(undo.undoStack()->index(), index + 1);
    EXPECT_FALSE(pm.saveProject());
    EXPECT_EQ(pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Blocked")), folder);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Blocked")), nullptr);
    EXPECT_TRUE(QFileInfo(actionsPath).isFile());
}

TEST_F(FolderAddressingTest, FolderPathsOrganizeObjectsWithoutEnteringObjectLookup)
{
    auto& pm = ProjectManager::instance();
    auto* root = createFolder(QStringLiteral("LookupRoot"));
    ASSERT_NE(root, nullptr);
    auto* nested = pm.createFolder(root, QStringLiteral("LookupNested"), pm.currentProject()->folderBucket(root));
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Lookup"), ProjectItem::Type::Action,
        QStringLiteral("LookupRoot/LookupNested"));
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(action, nullptr);

    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(findItemByPathInProject(
                  project, QStringLiteral("Actions/LookupRoot/LookupNested")),
              nullptr);
    EXPECT_EQ(findItemByPathInProject(
                  project,
                  QStringLiteral("Actions/LookupRoot/LookupNested/Play_Lookup")),
              action);
    EXPECT_EQ(project->folderRelPath(nested), QStringLiteral("LookupRoot/LookupNested"));
    EXPECT_EQ(buildItemFullPath(project, action),
              QStringLiteral("Actions/LookupRoot/LookupNested/Play_Lookup"));
}

TEST_F(FolderAddressingTest, TopLevelListReturnsResolvablePackPaths)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Listed"), ProjectItem::Type::Action,
        QStringLiteral("ListRoot/ListNested"));
    ASSERT_NE(action, nullptr);

    IPCServer server(nullptr);
    server.setProjectManager(&pm);
    const QJsonObject response = server.dispatchCommand(QStringLiteral("list_items"), {});
    ASSERT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    const QJsonArray items = response.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("items")).toArray();
    auto listed = std::find_if(items.begin(), items.end(), [](const QJsonValue& value) {
        return value.toObject().value(QStringLiteral("name")).toString()
            == QStringLiteral("Play_Listed");
    });
    ASSERT_NE(listed, items.end());

    const QString path = listed->toObject().value(QStringLiteral("path")).toString();
    EXPECT_EQ(path, QStringLiteral("Actions/ListRoot/ListNested/Play_Listed"));
    EXPECT_EQ(findItemByPathInProject(pm.currentProject(), path), action);

    const QJsonObject info = server.dispatchCommand(
        QStringLiteral("get_item_info"), QJsonObject{{QStringLiteral("path"), path}});
    EXPECT_EQ(info.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
}

TEST_F(FolderAddressingTest, ListAndInfoEmitRuntimeStableIdObjectIdString)
{
    auto& pm = ProjectManager::instance();
    auto* folder = createFolder(QStringLiteral("StableRoot"));
    auto* nested = pm.createFolder(folder, QStringLiteral("StableNested"), pm.currentProject()->folderBucket(folder));
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Stable"), ProjectItem::Type::Action,
        QStringLiteral("StableRoot/StableNested"));
    ProjectItem* variable = pm.createItem(
        nullptr, QStringLiteral("StepVar"), ProjectItem::Type::Variable);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(variable, nullptr);
    EXPECT_FALSE(harkenstudio::IsNull(action->runtimeId()));
    EXPECT_FALSE(harkenstudio::IsNull(variable->runtimeId()));
    const QString actionId = objectIdString(action->runtimeId());
    const QString variableId = objectIdString(variable->runtimeId());
    EXPECT_NE(actionId, QStringLiteral("00000000-0000-0000-0000-000000000000"));
    EXPECT_NE(variableId, QStringLiteral("00000000-0000-0000-0000-000000000000"));

    IPCServer server(nullptr);
    server.setProjectManager(&pm);
    const QJsonObject listed = server.dispatchCommand(QStringLiteral("list_items"), {});
    ASSERT_EQ(listed.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonArray roots = listed.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("items")).toArray();

    auto findNamed = [](const QJsonArray& items, const QString& name) {
        return std::find_if(items.begin(), items.end(), [&](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("name")).toString() == name;
        });
    };

    const auto listedAction = findNamed(roots, QStringLiteral("Play_Stable"));
    const auto listedVariable = findNamed(roots, QStringLiteral("StepVar"));
    ASSERT_NE(listedAction, roots.end());
    ASSERT_NE(listedVariable, roots.end());

    const QJsonObject actionObject = listedAction->toObject();
    const QJsonObject variableObject = listedVariable->toObject();
    EXPECT_TRUE(actionObject.value(QStringLiteral("runtime_stable_id")).isString());
    EXPECT_EQ(actionObject.value(QStringLiteral("runtime_stable_id")).toString(), actionId);
    EXPECT_TRUE(actionObject.value(QStringLiteral("object_id")).isDouble());
    EXPECT_TRUE(actionObject.value(QStringLiteral("authoring_handle")).isDouble());
    EXPECT_NE(actionObject.value(QStringLiteral("runtime_stable_id")).type(),
              actionObject.value(QStringLiteral("object_id")).type());
    EXPECT_NE(actionObject.value(QStringLiteral("runtime_stable_id")).type(),
              actionObject.value(QStringLiteral("authoring_handle")).type());
    EXPECT_EQ(variableObject.value(QStringLiteral("runtime_stable_id")).toString(), variableId);

    const QString actionPath =
        actionObject.value(QStringLiteral("path")).toString();
    const QJsonObject info = server.dispatchCommand(
        QStringLiteral("get_item_info"),
        QJsonObject{{QStringLiteral("path"), actionPath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject infoResult = info.value(QStringLiteral("result")).toObject();
    EXPECT_TRUE(infoResult.value(QStringLiteral("runtime_stable_id")).isString());
    EXPECT_EQ(infoResult.value(QStringLiteral("runtime_stable_id")).toString(), actionId);
    EXPECT_EQ(infoResult.value(QStringLiteral("runtime_stable_id")).toString(),
              actionObject.value(QStringLiteral("runtime_stable_id")).toString());

}

TEST_F(FolderAddressingTest, ObjectPathLookupDoesNotTreatPhysicalFolderAsAnObject)
{
    auto& pm = ProjectManager::instance();
    auto* folder = createFolder(QStringLiteral("Collision"));
    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Collision"), ProjectItem::Type::Action);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(action, nullptr);

    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    EXPECT_EQ(project->folderRelPath(folder), QStringLiteral("Collision"));
    EXPECT_EQ(buildItemFullPath(project, action), QStringLiteral("Actions/Collision"));
    EXPECT_EQ(findItemByPathInProject(project, QStringLiteral("Actions/Collision")),
              action);
}

TEST_F(FolderAddressingTest, InvalidFolderAddressIsRejectedAtCreationBoundary)
{
    auto& pm = ProjectManager::instance();
    EXPECT_EQ(pm.createItem(nullptr, QStringLiteral("Play_Traversal"),
                            ProjectItem::Type::Action, QStringLiteral("../Outside")),
              nullptr);
    EXPECT_EQ(pm.createItem(nullptr, QStringLiteral("Play_Absolute"),
                            ProjectItem::Type::Action, QStringLiteral("C:/Outside")),
              nullptr);
    EXPECT_EQ(pm.createItem(nullptr, QStringLiteral("Play_Backslash"),
                            ProjectItem::Type::Action, QStringLiteral("Combat\\Nested")),
              nullptr);
}

TEST_F(FolderAddressingTest, InvalidFolderNamesAreRejectedAtCreationBoundary)
{
    for (const QString& name : {QStringLiteral("Bad/Folder"), QStringLiteral(".."),
                                QStringLiteral("CON"), QStringLiteral("Trailing."),
                                QStringLiteral("Trailing ")}) {
        EXPECT_EQ(createFolder(name), nullptr) << name.toStdString();
    }
}

TEST_F(FolderAddressingTest, FolderNamesAreUniqueOnlyAmongSiblings)
{
    auto& pm = ProjectManager::instance();
    auto* left = createFolder(QStringLiteral("Left"));
    auto* right = createFolder(QStringLiteral("Right"));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    auto* suffixedRoot = createFolder(QStringLiteral("left"));
    ASSERT_NE(suffixedRoot, nullptr);
    EXPECT_EQ(suffixedRoot->name, QStringLiteral("left_01"));

    ASSERT_NE(pm.createFolder(left, QStringLiteral("Shared"), pm.currentProject()->folderBucket(left)),
              nullptr);
    auto* suffixedChild = pm.createFolder(left, QStringLiteral("shared"), pm.currentProject()->folderBucket(left));
    ASSERT_NE(suffixedChild, nullptr);
    EXPECT_EQ(suffixedChild->name, QStringLiteral("shared_01"));
    EXPECT_EQ(pm.createFolder(left, QStringLiteral("Shared."), pm.currentProject()->folderBucket(left)),
              nullptr);
    EXPECT_NE(pm.createFolder(right, QStringLiteral("Shared"), pm.currentProject()->folderBucket(right)),
              nullptr);
    EXPECT_NE(pm.createFolder(nullptr, QStringLiteral("Left"), QStringLiteral("Variables")),
              nullptr);

    ASSERT_TRUE(pm.saveProject());
    EXPECT_TRUE(QFileInfo(actionFile(QStringLiteral("Left/Shared"))).isDir());
    EXPECT_TRUE(QFileInfo(actionFile(QStringLiteral("Right/Shared"))).isDir());
    EXPECT_TRUE(QFileInfo(QDir(projectRoot_).filePath(QStringLiteral("Variables/Left"))).isDir());

    ASSERT_TRUE(pm.loadProject(projectPath_));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    auto* reloadedLeft = project->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("Left/Shared"));
    auto* reloadedRight = project->findFolderByPath(
        QStringLiteral("Actions"), QStringLiteral("Right/Shared"));
    ASSERT_NE(reloadedLeft, nullptr);
    ASSERT_NE(reloadedRight, nullptr);
    EXPECT_NE(reloadedLeft, reloadedRight);
    EXPECT_EQ(reloadedLeft->parent->name, QStringLiteral("Left"));
    EXPECT_EQ(reloadedRight->parent->name, QStringLiteral("Right"));
}

TEST_F(FolderAddressingTest, RepeatedDefaultFolderNamesIncrementWithinTheirParent)
{
    auto& pm = ProjectManager::instance();
    auto* first = createFolder(QStringLiteral("New Folder"));
    auto* second = createFolder(QStringLiteral("New Folder"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->name, QStringLiteral("New Folder"));
    EXPECT_EQ(second->name, QStringLiteral("New Folder_01"));

    auto* parent = createFolder(QStringLiteral("Parent"));
    ASSERT_NE(parent, nullptr);
    auto* nested = pm.createFolder(parent, QStringLiteral("New Folder"), pm.currentProject()->folderBucket(parent));
    auto* nestedSecond = pm.createFolder(parent, QStringLiteral("New Folder"), pm.currentProject()->folderBucket(parent));
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(nestedSecond, nullptr);
    EXPECT_EQ(nested->name, QStringLiteral("New Folder"));
    EXPECT_EQ(nestedSecond->name, QStringLiteral("New Folder_01"));
}

TEST_F(FolderAddressingTest, FolderRenameUsesFilesystemValidationAndSiblingScope)
{
    auto& pm = ProjectManager::instance();
    auto* parent = createFolder(QStringLiteral("Parent"));
    ASSERT_NE(parent, nullptr);
    auto* first = pm.createFolder(parent, QStringLiteral("First"), pm.currentProject()->folderBucket(parent));
    auto* second = pm.createFolder(parent, QStringLiteral("Second"), pm.currentProject()->folderBucket(parent));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_TRUE(pm.renameFolder(second, QStringLiteral("first")));
    EXPECT_FALSE(pm.renameFolder(second, QStringLiteral("Renamed.")));
    EXPECT_EQ(second->name, QStringLiteral("first_01"));
}

TEST_F(FolderAddressingTest, FolderRenameUndoPreservesOrdinaryFilesAcrossSave)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* action = pm.createItem(nullptr, QStringLiteral("Existing"),
        ProjectItem::Type::Action, QStringLiteral("Original"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    ASSERT_TRUE(pm.saveProject());
    QFile ordinary(actionFile(QStringLiteral("Original/ordinary.bin")));
    ASSERT_TRUE(ordinary.open(QIODevice::WriteOnly));
    ASSERT_EQ(ordinary.write("retained"), 8);
    ordinary.close();
    ASSERT_TRUE(pm.loadProject(projectPath_));
    action = pm.findItemByGuid(guid);
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Original"));
    ASSERT_NE(folder, nullptr);
    ASSERT_TRUE(pm.renameFolder(folder, QStringLiteral("Renamed")));
    EXPECT_TRUE(QFileInfo(actionFile(action, QStringLiteral("Original"))).isFile());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Renamed"))));
    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Original"))));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(folder->name, QStringLiteral("Original"));
    EXPECT_TRUE(QFileInfo(actionFile(action, QStringLiteral("Renamed"))).isFile());
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(ordinary.open(QIODevice::ReadOnly));
    EXPECT_EQ(ordinary.readAll(), QByteArray("retained"));
    EXPECT_TRUE(QFileInfo(actionFile(action, QStringLiteral("Original"))).isFile());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Renamed"))));
}

TEST_F(FolderAddressingTest, FolderMoveUndoPreservesOrdinaryFilesAcrossSave)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_NE(createFolder(QStringLiteral("Target")), nullptr);
    auto* action = pm.createItem(nullptr, QStringLiteral("Existing"),
        ProjectItem::Type::Action, QStringLiteral("Original"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    ASSERT_TRUE(pm.saveProject());
    QFile ordinary(actionFile(QStringLiteral("Original/ordinary.bin")));
    ASSERT_TRUE(ordinary.open(QIODevice::WriteOnly));
    ASSERT_EQ(ordinary.write("retained"), 8);
    ordinary.close();
    ASSERT_TRUE(pm.loadProject(projectPath_));
    action = pm.findItemByGuid(guid);
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Original"));
    auto* target = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Target"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(pm.moveFolder(folder, target, QStringLiteral("Actions")));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(action), QStringLiteral("Target/Original"));
    EXPECT_TRUE(QFileInfo(actionFile(action, QStringLiteral("Original"))).isFile());
    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Original"))));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(folder->parent, nullptr);
    EXPECT_TRUE(QFileInfo(actionFile(action, QStringLiteral("Target/Original"))).isFile());
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(ordinary.open(QIODevice::ReadOnly));
    EXPECT_EQ(ordinary.readAll(), QByteArray("retained"));
    EXPECT_TRUE(QFileInfo(actionFile(action, QStringLiteral("Original"))).isFile());
    EXPECT_FALSE(QFileInfo::exists(actionFile(QStringLiteral("Target/Original"))));
}

TEST_F(FolderAddressingTest, FolderDeletionAndExternalReferenceCleanupShareUndoAndSave)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* folder = pm.createFolder(nullptr, QStringLiteral("Deleted"), QStringLiteral("Layers"));
    auto* sound = pm.createItem(nullptr, QStringLiteral("Target"), ProjectItem::Type::Sound, QStringLiteral("Deleted"));
    auto* owner = pm.createItem(nullptr, QStringLiteral("Owner"), ProjectItem::Type::Action);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(owner, nullptr);
    VActionStep step;
    step.targetObjectID = sound->objectID;
    step.targetName = sound->name;
    owner->comp<VAction>()->steps.push_back(step);
    ASSERT_TRUE(pm.saveProject());
    const QUuid soundGuid = sound->guid;
    const QUuid ownerGuid = owner->guid;
    const QString ownerPath = pm.currentProject()->objectFilePath(owner);
    QFile original(ownerPath);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const QByteArray bytes = original.readAll();
    original.close();
    owner->extensions = {{QStringLiteral("test"), QStringLiteral("Pending authoring")}};
    owner->markDirty();
    ASSERT_TRUE(pm.renameItem(owner, QStringLiteral("RenamedOwner")));
    ASSERT_TRUE(pm.deleteEntries({}, {folder}));
    EXPECT_EQ(owner->comp<VAction>()->steps.front().targetObjectID, 0u);
    EXPECT_TRUE(owner->isDirtyObject());
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), bytes);
    original.close();
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(pm.findItemByGuid(soundGuid), sound);
    EXPECT_EQ(owner->comp<VAction>()->steps.front().targetObjectID, sound->objectID);
    EXPECT_EQ(owner->name, QStringLiteral("RenamedOwner"));
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    EXPECT_EQ(pm.findItemByGuid(soundGuid), nullptr);
    auto* reloadedOwner = pm.findItemByGuid(ownerGuid);
    ASSERT_NE(reloadedOwner, nullptr);
    EXPECT_EQ(reloadedOwner->name, QStringLiteral("RenamedOwner"));
    EXPECT_EQ(reloadedOwner->comp<VAction>()->steps.front().targetObjectID, 0u);
    EXPECT_EQ(reloadedOwner->extensions.value(QStringLiteral("test")).toString(), QStringLiteral("Pending authoring"));
}

TEST_F(FolderAddressingTest, FailedSaveKeepsDeletionPendingAndLastGoodFilesIntact)
{
    auto& pm = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* target = pm.createItem(nullptr, QStringLiteral("Target"), ProjectItem::Type::Sound,
                                 QStringLiteral("Rollback"));
    auto* owner = pm.createItem(nullptr, QStringLiteral("Owner"), ProjectItem::Type::Action);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(owner, nullptr);
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Layers"), QStringLiteral("Rollback"));
    ASSERT_NE(folder, nullptr);
    VActionStep step;
    step.targetObjectID = target->objectID;
    step.targetName = target->name;
    owner->comp<VAction>()->steps.push_back(step);
    ASSERT_TRUE(pm.saveProject());
    const QString targetPath = pm.currentProject()->objectFilePath(target);
    const QString ownerPath = pm.currentProject()->objectFilePath(owner);
    const QUuid targetGuid = target->guid;
    QFile ownerFile(ownerPath);
    ASSERT_TRUE(ownerFile.open(QIODevice::ReadOnly));
    const QByteArray committedOwner = ownerFile.readAll();
    ownerFile.close();
    ASSERT_TRUE(pm.renameItem(owner, QStringLiteral("PendingOwner")));
    ASSERT_NE(createFolder(NameValidator::objectFileName(owner->name, owner->guid)), nullptr);
    ASSERT_TRUE(pm.deleteEntries({}, {folder}));
    const int historyIndex = undo.undoStack()->index();
    EXPECT_FALSE(pm.saveProject());
    EXPECT_EQ(pm.findItemByGuid(targetGuid), nullptr);
    EXPECT_EQ(owner->comp<VAction>()->steps.front().targetObjectID, 0u);
    EXPECT_EQ(undo.undoStack()->index(), historyIndex);
    EXPECT_FALSE(undo.isClean());
    EXPECT_TRUE(QFileInfo(targetPath).isFile());
    ASSERT_TRUE(ownerFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(ownerFile.readAll(), committedOwner);
    ownerFile.close();
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(pm.findItemByGuid(targetGuid), target);
    EXPECT_EQ(owner->comp<VAction>()->steps.front().targetObjectID, target->objectID);
    EXPECT_EQ(owner->name, QStringLiteral("PendingOwner"));
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(pm.saveProject());
}

TEST_F(FolderAddressingTest, FolderDeletionClearsAttenuationReferencesAndUndoRestoresThem)
{
    auto& pm = ProjectManager::instance();
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);

    auto* folder = pm.createFolder(nullptr, QStringLiteral("Protected"), QStringLiteral("Attenuations"));
    ProjectItem* attenuation = pm.createItem(
        nullptr, QStringLiteral("Distance_Protected"),
        ProjectItem::Type::Attenuation, QStringLiteral("Protected"));
    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("Layer_Reference"), ProjectItem::Type::Layer);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(attenuation, nullptr);
    ASSERT_NE(layer, nullptr);

    auto groups = layer->fieldGroups;
    SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = toFieldGroupGuid(attenuation->guid);
    groups.spatialRating.shared = {FieldGroupMode::Local, rating};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(layer, groups));
    ASSERT_TRUE(ProjectManager::isReferencedAttenuation(attenuation));
    ASSERT_TRUE(pm.saveProject());

    UndoRedoManager::instance().clear();
    ASSERT_TRUE(pm.deleteEntries({}, {folder}));
    EXPECT_EQ(project->findFolderByPath(QStringLiteral("Attenuations"),
                                        QStringLiteral("Protected")), nullptr);
    EXPECT_EQ(pm.findItemByGuid(attenuation->guid), nullptr);
    EXPECT_TRUE(harkenprojectschema::IsNull(layer->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef));
    EXPECT_TRUE(layer->dirtyBit);
    const auto* layerRow = findRow(panel_->treeWidget(), QStringLiteral("Layer_Reference"));
    ASSERT_NE(layerRow, nullptr);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(project->findFolderByPath(QStringLiteral("Attenuations"),
                                        QStringLiteral("Protected")), folder);
    EXPECT_EQ(pm.findItemByGuid(attenuation->guid), attenuation);
    EXPECT_EQ(layer->fieldGroups, groups);
    EXPECT_TRUE(layer->dirtyBit);
    ASSERT_TRUE(pm.saveProject());
}

TEST_F(FolderAddressingTest, MovingAttenuationIntoFolderPreservesSoundReferenceAfterReload)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* attenuation = pm.createItem(
        nullptr, QStringLiteral("Distance_Moved"), ProjectItem::Type::Attenuation);
    auto* folder = pm.createFolder(nullptr, QStringLiteral("Moved"), QStringLiteral("Attenuations"));
    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("Layer_Reference"), ProjectItem::Type::Layer);
    ASSERT_NE(attenuation, nullptr);
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createItem(
        layer, QStringLiteral("Sound_Reference"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);

    auto groups = sound->fieldGroups;
    SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = toFieldGroupGuid(attenuation->guid);
    groups.spatialRating.shared = {FieldGroupMode::Local, rating};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, groups));

    const QUuid attenuationGuid = attenuation->guid;
    const QUuid soundGuid = sound->guid;
    ASSERT_TRUE(pm.saveProject());

    QTreeWidgetItem* folderRow = findRow(panel_->treeWidget(), QStringLiteral("Moved"));
    ASSERT_NE(folderRow, nullptr);
    ASSERT_NO_FATAL_FAILURE(dropOnRow(attenuation, folderRow));
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(attenuation), QStringLiteral("Moved"));

    ASSERT_TRUE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(QDir(projectRoot_).filePath(
        QStringLiteral("Attenuations/") + NameValidator::objectFileName(attenuation->name, attenuationGuid))));
    EXPECT_TRUE(QFileInfo::exists(QDir(projectRoot_).filePath(
        QStringLiteral("Attenuations/Moved/") + NameValidator::objectFileName(attenuation->name, attenuationGuid))));
    ASSERT_TRUE(pm.loadProject(projectPath_));

    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    ProjectItem* reloadedAttenuation = pm.findItemByGuid(attenuationGuid);
    ProjectItem* reloadedSound = pm.findItemByGuid(soundGuid);
    ASSERT_NE(reloadedAttenuation, nullptr);
    ASSERT_NE(reloadedSound, nullptr);
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(reloadedAttenuation), QStringLiteral("Moved"));
    EXPECT_EQ(reloadedSound->fieldGroups, groups);
    const VSpatial* reloadedSpatial = reloadedSound->comp<VSpatial>();
    ASSERT_NE(reloadedSpatial, nullptr);
    EXPECT_EQ(reloadedSpatial->attenuationRef, reloadedAttenuation->runtimeId());
    EXPECT_NE(AttenuationStore::find(project, attenuationGuid,
                                     QStringLiteral("Distance_Moved")),
              nullptr);
    EXPECT_TRUE(ProjectManager::isReferencedAttenuation(reloadedAttenuation));
}

TEST_F(FolderAddressingTest, SaveProjectCommitsAssetRegistryFromHierarchySnapshot)
{
    auto& pm = ProjectManager::instance();
    const QString registryPath = QDir(PathUtils::getAssetRegistryCacheDir(projectRoot_))
                                     .filePath(QStringLiteral("assets.json"));
    if (QFileInfo::exists(registryPath))
        ASSERT_TRUE(QFile::remove(registryPath));

    ASSERT_TRUE(pm.saveProject());
    EXPECT_TRUE(QFileInfo::exists(registryPath));
    QFile registry(registryPath);
    ASSERT_TRUE(registry.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(registry.readAll());
    ASSERT_TRUE(document.isObject());
    EXPECT_TRUE(document.object().contains(QStringLiteral("version")));
}

TEST_F(FolderAutomationTest, PathOperationsKeepFoldersSeparateFromObjectIdentity)
{
    auto& actions = ActionManager::instance();
    auto& pm = ProjectManager::instance();
    const ActionResult root = actions.execute(QStringLiteral("create_folder"),
        {{"path", "Layers/Environment"}});
    ASSERT_TRUE(root.success) << root.errorMessage.toStdString();
    EXPECT_EQ(root.data.value("path").toString(), QStringLiteral("Layers/Environment"));
    for (const QString& key : {QStringLiteral("guid"), QStringLiteral("object_id"),
             QStringLiteral("authoring_handle"), QStringLiteral("runtime_stable_id"),
             QStringLiteral("type")})
        EXPECT_FALSE(root.data.contains(key));
    ASSERT_TRUE(actions.execute(QStringLiteral("create_folder"),
        {{"path", "Layers/Environment/Wind"}}).success);
    ASSERT_TRUE(actions.execute(QStringLiteral("create_folder"),
        {{"path", "Layers/Destination"}}).success);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Weather"),
        ProjectItem::Type::Layer, QStringLiteral("Environment/Wind"));
    ASSERT_NE(layer, nullptr);
    const QUuid layerGuid = layer->guid;

    const ActionResult listed = actions.execute(QStringLiteral("list_folders"),
        {{"path", "Layers/Environment"}});
    ASSERT_TRUE(listed.success);
    const QJsonArray children = listed.data.value("folders").toArray();
    ASSERT_EQ(children.size(), 1);
    EXPECT_EQ(children.first().toObject().value("path").toString(),
              QStringLiteral("Layers/Environment/Wind"));
    const ActionResult info = actions.execute(QStringLiteral("get_folder_info"),
        {{"path", "Layers/Environment/Wind"}});
    ASSERT_TRUE(info.success);
    const QJsonArray items = info.data.value("items").toArray();
    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items.first().toString(), pm.currentProject()->itemPath(layer));

    const ActionResult renamed = actions.execute(QStringLiteral("rename_folder"),
        {{"path", "Layers/Environment"}, {"new_name", "Outside"}});
    ASSERT_TRUE(renamed.success);
    EXPECT_EQ(pm.currentProject()->itemFolderPath(layer), QStringLiteral("Outside/Wind"));
    const ActionResult moved = actions.execute(QStringLiteral("move_folder"),
        {{"path", "Layers/Outside/Wind"}, {"new_parent", "Layers/Destination"}});
    ASSERT_TRUE(moved.success);
    EXPECT_EQ(moved.data.value("path").toString(), QStringLiteral("Layers/Destination/Wind"));
    EXPECT_EQ(pm.currentProject()->itemFolderPath(layer), QStringLiteral("Destination/Wind"));
    EXPECT_EQ(layer->parent, nullptr);
    const ActionResult selected = actions.execute(QStringLiteral("select_folder"),
        {{"path", "Layers/Destination/Wind"}});
    ASSERT_TRUE(selected.success);
    EXPECT_EQ(panel_->selectedFolder(), pm.currentProject()->folderForItem(layer));
    EXPECT_EQ(panel_->selectedItem(), nullptr);

    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    layer = pm.findItemByGuid(layerGuid);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(pm.currentProject()->itemFolderPath(layer), QStringLiteral("Destination/Wind"));
    const ActionResult deleted = actions.execute(QStringLiteral("delete_folder"),
        {{"path", "Layers/Destination"}});
    ASSERT_TRUE(deleted.success) << deleted.errorMessage.toStdString();
    EXPECT_EQ(pm.findItemByGuid(layerGuid), nullptr);
    EXPECT_EQ(pm.currentProject()->findFolderByPath(QStringLiteral("Layers"),
                                                   QStringLiteral("Destination")), nullptr);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(pm.findItemByGuid(layerGuid), layer);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.loadProject(projectPath_));
    EXPECT_EQ(pm.findItemByGuid(layerGuid), nullptr);
}

TEST_F(FolderAutomationTest, InvalidAndCrossBucketPathsDoNotChangeDirectories)
{
    auto& actions = ActionManager::instance();
    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(actions.execute(QStringLiteral("create_folder"),
        {{"path", "Layers/Root"}}).success);
    ASSERT_TRUE(actions.execute(QStringLiteral("create_folder"),
        {{"path", "Layers/Root/Child"}}).success);
    ASSERT_TRUE(actions.execute(QStringLiteral("create_folder"),
        {{"path", "Actions/Target"}}).success);
    UndoRedoManager::instance().clear();
    for (const QString& path : {QStringLiteral("Layers"), QStringLiteral("Busses/Folder"),
             QStringLiteral("Layers/../Escape"), QStringLiteral("Layers//Empty"),
             QStringLiteral("Layers/Missing/Child"), QStringLiteral("Layers/Root")}) {
        SCOPED_TRACE(path.toStdString());
        EXPECT_FALSE(actions.execute(QStringLiteral("create_folder"), {{"path", path}}).success);
    }
    EXPECT_FALSE(actions.execute(QStringLiteral("move_folder"),
        {{"path", "Layers/Root"}, {"new_parent", "Actions/Target"}}).success);
    EXPECT_FALSE(actions.execute(QStringLiteral("move_folder"),
        {{"path", "Layers/Root"}, {"new_parent", "Layers/Root/Child"}}).success);
    EXPECT_FALSE(actions.execute(QStringLiteral("rename_folder"),
        {{"path", "Layers/Root"}, {"new_name", "Bad/Name"}}).success);
    EXPECT_FALSE(actions.execute(QStringLiteral("delete_folder"), {{"path", "Layers"}}).success);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_NE(pm.currentProject()->findFolderByPath(QStringLiteral("Layers"),
                                                    QStringLiteral("Root/Child")), nullptr);
    EXPECT_EQ(pm.currentProject()->folderRootsForBucket(QStringLiteral("Layers")).size(), 1u);
}

TEST_F(FolderAutomationTest, ObjectCreationCannotCreatePhysicalDirectories)
{
    auto& actions = ActionManager::instance();
    const ActionResult result = actions.execute(QStringLiteral("create_item"),
        {{"name", "NotAnObject"}, {"type", "Folder"}});
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(ProjectManager::instance().currentProject()->folders.empty());
}

INSTANTIATE_TEST_SUITE_P(
    SupportedBuckets,
    FolderBucketAddressingTest,
    ::testing::Values(
        FolderBucketCase{"Actions", "Actions", "Play_Folder", ProjectItem::Type::Action},
        FolderBucketCase{"Variables", "Variables", "State_Folder", ProjectItem::Type::Variable},
        FolderBucketCase{"Attenuations", "Attenuations", "Distance_Folder",
                         ProjectItem::Type::Attenuation},
        FolderBucketCase{"LayersLayer", "Layers", "Layer_Folder", ProjectItem::Type::Layer},
        FolderBucketCase{"LayersSound", "Layers", "Sound_Folder", ProjectItem::Type::Sound},
        FolderBucketCase{"LayersContainer", "Layers", "Container_Folder", ProjectItem::Type::Container},
        FolderBucketCase{"LayersTrack", "Layers", "Track_Folder", ProjectItem::Type::Track},
        FolderBucketCase{"LayersPiece", "Layers", "Piece_Folder", ProjectItem::Type::Piece}),
    [](const ::testing::TestParamInfo<FolderBucketCase>& info) {
        return std::string(info.param.label);
    });
