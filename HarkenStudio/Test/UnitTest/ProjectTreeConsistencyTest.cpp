#include "ItemTypeCatalog.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VSound.h"
#include "Core/LanguageContextService.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/ProjectExplorer/ExplorerSection.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/Shared/TreeRowStyle.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <memory>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

// Row text carries a " *" dirty suffix; both trees must agree on it, so
// snapshots keep the raw text and only row lookups strip it.
QString rowLabel(const QTreeWidgetItem* row)
{
    QString text = row->text(0);
    if (text.endsWith(QStringLiteral(" *")))
        text.chop(2);
    return text;
}

void appendRows(QTreeWidgetItem* row, int depth, bool visibleOnly, QStringList& out)
{
    if (visibleOnly && row->isHidden()) return;
    QString line(depth * 2, QChar(' '));
    line += row->text(0);
    if (row->data(0, TreeRowStyle::kBucketRole).toBool())
        line += QStringLiteral(" [bucket]");
    line += row->isExpanded() ? QStringLiteral(" [open]") : QStringLiteral(" [closed]");
    out.append(line);
    for (int i = 0; i < row->childCount(); ++i)
        appendRows(row->child(i), depth + 1, visibleOnly, out);
}

QString snapshot(QTreeWidget* tree, bool visibleOnly = false)
{
    QStringList out;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        appendRows(tree->topLevelItem(i), 0, visibleOnly, out);
    return out.join(QChar('\n'));
}

QTreeWidgetItem* findRowIn(QTreeWidgetItem* row, const QString& label)
{
    if (rowLabel(row) == label) return row;
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

QTreeWidgetItem* bucketRow(QTreeWidget* tree, const QString& folderName)
{
    const QString label = ProjectManager::bucketDisplayName(folderName);
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = tree->topLevelItem(i);
        if (rowLabel(row) == label) return row;
    }
    return nullptr;
}

class ProjectTreeConsistencyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        explorer_ = std::make_unique<ProjectExplorer>();
        hierarchy_ = std::make_unique<Hierarchy>();

        ProjectConfig config;
        config.name = QStringLiteral("TreeConsistencyProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        auto& pm = ProjectManager::instance();
        layerRoot_ = pm.createItem(nullptr, QStringLiteral("Amb_Root"), ProjectItem::Type::Layer);
        ASSERT_NE(layerRoot_, nullptr);
        subLayer_ = pm.createItem(layerRoot_, QStringLiteral("Amb_Sub"), ProjectItem::Type::Layer);
        ASSERT_NE(subLayer_, nullptr);
        sound_ = pm.createItem(subLayer_, QStringLiteral("Amb_Rain"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        layerAlt_ = pm.createItem(nullptr, QStringLiteral("Amb_Alt"), ProjectItem::Type::Layer);
        ASSERT_NE(layerAlt_, nullptr);

        container_ = pm.createItem(layerRoot_, QStringLiteral("Grp_Footsteps"), ProjectItem::Type::Container);
        ASSERT_NE(container_, nullptr);
        containerSound_ = pm.createItem(container_, QStringLiteral("Grp_Footsteps_Grass"), ProjectItem::Type::Sound);
        ASSERT_NE(containerSound_, nullptr);
        track_ = pm.createItem(layerRoot_, QStringLiteral("Trk_Theme"), ProjectItem::Type::Track);
        ASSERT_NE(track_, nullptr);
        piece_ = pm.createItem(track_, QStringLiteral("Pc_Intro"), ProjectItem::Type::Piece);
        ASSERT_NE(piece_, nullptr);
        pieceTrack_ = pm.createItem(piece_, QStringLiteral("Trk_Intro_Sub"), ProjectItem::Type::Track);
        ASSERT_NE(pieceTrack_, nullptr);

        soundRoot_ = pm.createItem(nullptr, QStringLiteral("Root_Shot"), ProjectItem::Type::Sound);
        ASSERT_NE(soundRoot_, nullptr);
        containerRoot_ = pm.createItem(nullptr, QStringLiteral("Root_Grp"), ProjectItem::Type::Container);
        ASSERT_NE(containerRoot_, nullptr);
        trackRoot_ = pm.createItem(nullptr, QStringLiteral("Root_Trk"), ProjectItem::Type::Track);
        ASSERT_NE(trackRoot_, nullptr);
        pieceRoot_ = pm.createItem(nullptr, QStringLiteral("Root_Pc"), ProjectItem::Type::Piece);
        ASSERT_NE(pieceRoot_, nullptr);

        variable_ = pm.createItem(nullptr, QStringLiteral("Var_Distance"), ProjectItem::Type::Variable);
        ASSERT_NE(variable_, nullptr);

        actionFolder_ = pm.createFolder(nullptr, QStringLiteral("Combat"), ItemTypeCatalog::kActionsFolderName);
        ASSERT_NE(actionFolder_, nullptr);
        actionSubFolder_ = pm.createFolder(actionFolder_, QStringLiteral("Melee"), ItemTypeCatalog::kActionsFolderName);
        ASSERT_NE(actionSubFolder_, nullptr);
        action_ = pm.createItem(nullptr, QStringLiteral("Play_Swing"), ProjectItem::Type::Action,
                                QStringLiteral("Combat/Melee"));
        ASSERT_NE(action_, nullptr);

        attenuation_ = pm.createItem(nullptr, QStringLiteral("Att_Default"), ProjectItem::Type::Attenuation);
        ASSERT_NE(attenuation_, nullptr);

        ProjectItem* master = pm.getMasterBusItem();
        ASSERT_NE(master, nullptr);
        busMusic_ = pm.createItem(master, QStringLiteral("Mix_Music"), ProjectItem::Type::Bus);
        ASSERT_NE(busMusic_, nullptr);
        busMusicSub_ = pm.createItem(busMusic_, QStringLiteral("Mix_Music_Sub"), ProjectItem::Type::Bus);
        ASSERT_NE(busMusicSub_, nullptr);
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    void expectTreesEqual(const char* stage)
    {
        EXPECT_EQ(snapshot(explorer_->treeWidget()), snapshot(hierarchy_->treeWidget()))
            << "trees diverged: " << stage;
    }

    QString rowPresentation(const QTreeWidgetItem* row) const
    {
        return row->text(0) + (row->font(0).italic() ? QStringLiteral("\nitalic") : QString());
    }

    void expectFolderPresentation(const QString& name, bool dirty)
    {
        for (ProjectTreePanel* panel : {static_cast<ProjectTreePanel*>(explorer_.get()),
                                       static_cast<ProjectTreePanel*>(hierarchy_.get())}) {
            auto* row = findRow(panel->treeWidget(), name);
            ASSERT_NE(row, nullptr);
            EXPECT_EQ(rowPresentation(row) != name, dirty);
        }
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<ProjectExplorer> explorer_;
    std::unique_ptr<Hierarchy> hierarchy_;

    ProjectItem* layerRoot_ = nullptr;
    ProjectItem* subLayer_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* layerAlt_ = nullptr;
    ProjectItem* container_ = nullptr;
    ProjectItem* containerSound_ = nullptr;
    ProjectItem* track_ = nullptr;
    ProjectItem* piece_ = nullptr;
    ProjectItem* pieceTrack_ = nullptr;
    ProjectItem* soundRoot_ = nullptr;
    ProjectItem* containerRoot_ = nullptr;
    ProjectItem* trackRoot_ = nullptr;
    ProjectItem* pieceRoot_ = nullptr;
    ProjectItem* variable_ = nullptr;
    PhysicalFolder* actionFolder_ = nullptr;
    PhysicalFolder* actionSubFolder_ = nullptr;
    ProjectItem* action_ = nullptr;
    ProjectItem* attenuation_ = nullptr;
    ProjectItem* busMusic_ = nullptr;
    ProjectItem* busMusicSub_ = nullptr;
};

}

TEST_F(ProjectTreeConsistencyTest, LanguageChangesRefreshSourcesInBothTrees)
{
    auto& pm = ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese"),
                                   QStringLiteral("Korean")});
    auto& context = LanguageContextService::instance();
    context.setLanguage(QStringLiteral("English"));
    auto* voice = sound_->comp<VSound>();
    voice->distinguishLanguage = true;
    for (const QString& language : {QStringLiteral("English"), QStringLiteral("Japanese")}) {
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = language == QStringLiteral("English") ? 1001 : 1002;
        AssetInfo asset;
        asset.sourceID = source.sourceId;
        asset.relativePath = language + QStringLiteral(".wav");
        ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
        voice->sources.localized[language].entries.push_back(source);
        voice->sources.localized[language].active = source.entryId;
    }
    emit pm.itemPropertyChanged(sound_, QStringLiteral("sources"), nullptr);

    SoundSourceDef effect;
    effect.entryId = QUuid::createUuid();
    effect.sourceId = 1003;
    AssetInfo effectAsset;
    effectAsset.sourceID = effect.sourceId;
    effectAsset.relativePath = QStringLiteral("Effect.wav");
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(effectAsset));
    auto* unlocalized = soundRoot_->comp<VSound>();
    unlocalized->sources.plain.entries = {effect};
    unlocalized->sources.plain.active = effect.entryId;
    emit pm.itemPropertyChanged(soundRoot_, QStringLiteral("sources"), nullptr);

    const QString effectTooltip = findRow(explorer_->treeWidget(), soundRoot_->name)->toolTip(0);
    for (auto* tree : {explorer_->treeWidget(), hierarchy_->treeWidget()}) {
        ASSERT_NE(findRow(tree, sound_->name), nullptr);
        EXPECT_TRUE(findRow(tree, sound_->name)->toolTip(0).contains(QStringLiteral("English.wav")));
    }

    context.setLanguage(QStringLiteral("Japanese"));
    for (auto* tree : {explorer_->treeWidget(), hierarchy_->treeWidget()}) {
        auto* row = findRow(tree, sound_->name);
        EXPECT_TRUE(row->toolTip(0).contains(QStringLiteral("Japanese.wav")));
        EXPECT_FALSE(row->toolTip(0).contains(QStringLiteral("English.wav")));
        row->setForeground(0, QBrush(Qt::red));
    }

    context.setLanguage(QStringLiteral("Korean"));
    for (auto* tree : {explorer_->treeWidget(), hierarchy_->treeWidget()}) {
        auto* row = findRow(tree, sound_->name);
        EXPECT_FALSE(row->toolTip(0).contains(QStringLiteral(".wav")));
        EXPECT_FALSE(row->data(0, Qt::ForegroundRole).isValid());
        EXPECT_EQ(findRow(tree, soundRoot_->name)->toolTip(0), effectTooltip);
    }
}

TEST_F(ProjectTreeConsistencyTest, OpenAndRebuildProduceIdenticalTrees)
{
    const QString incremental = snapshot(explorer_->treeWidget());
    ASSERT_FALSE(incremental.isEmpty());
    EXPECT_TRUE(incremental.contains(QStringLiteral("Amb_Rain")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Play_Swing")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Att_Default")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Mix_Music_Sub")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Grp_Footsteps_Grass")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Trk_Intro_Sub")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Var_Distance")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Root_Shot")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Root_Grp")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Root_Trk")));
    EXPECT_TRUE(incremental.contains(QStringLiteral("Root_Pc")));
    expectTreesEqual("signal-driven build");

    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    explorer_->onProjectOpened(project);
    hierarchy_->onProjectOpened(project);
    expectTreesEqual("full rebuild via onProjectOpened");
}

TEST_F(ProjectTreeConsistencyTest, LeafLifecycleKeepsTreesIdentical)
{
    auto& pm = ProjectManager::instance();

    ProjectItem* wind = pm.createItem(subLayer_, QStringLiteral("Amb_Wind"), ProjectItem::Type::Sound);
    ASSERT_NE(wind, nullptr);
    expectTreesEqual("create leaf");

    ASSERT_TRUE(pm.renameItem(wind, QStringLiteral("Amb_Storm")));
    expectTreesEqual("rename leaf");

    ASSERT_TRUE(pm.moveItem(wind, layerAlt_));
    expectTreesEqual("move leaf");

    ASSERT_TRUE(pm.deleteItem(wind));
    EXPECT_EQ(findRow(explorer_->treeWidget(), QStringLiteral("Amb_Storm")), nullptr);
    expectTreesEqual("delete leaf");

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_NE(findRow(explorer_->treeWidget(), QStringLiteral("Amb_Storm")), nullptr);
    EXPECT_NE(findRow(hierarchy_->treeWidget(), QStringLiteral("Amb_Storm")), nullptr);
    expectTreesEqual("undo delete");

    ASSERT_TRUE(undo->canRedo());
    undo->redo();
    EXPECT_EQ(findRow(explorer_->treeWidget(), QStringLiteral("Amb_Storm")), nullptr);
    EXPECT_EQ(findRow(hierarchy_->treeWidget(), QStringLiteral("Amb_Storm")), nullptr);
    expectTreesEqual("redo delete");
}

TEST_F(ProjectTreeConsistencyTest, ChildOrderSignalReordersExistingRows)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* second = pm.createItem(
        container_, QStringLiteral("Grp_Footsteps_Stone"), ProjectItem::Type::Sound);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(container_->children.size(), 2u);

    std::swap(container_->children[0], container_->children[1]);
    emit pm.itemPropertyChanged(container_, QStringLiteral("childOrder"));

    QTreeWidgetItem* explorerContainer = findRow(
        explorer_->treeWidget(), QStringLiteral("Grp_Footsteps"));
    QTreeWidgetItem* hierarchyContainer = findRow(
        hierarchy_->treeWidget(), QStringLiteral("Grp_Footsteps"));
    ASSERT_NE(explorerContainer, nullptr);
    ASSERT_NE(hierarchyContainer, nullptr);
    ASSERT_EQ(explorerContainer->childCount(), 2);
    ASSERT_EQ(hierarchyContainer->childCount(), 2);
    EXPECT_EQ(rowLabel(explorerContainer->child(0)), QStringLiteral("Grp_Footsteps_Stone"));
    EXPECT_EQ(rowLabel(explorerContainer->child(1)), QStringLiteral("Grp_Footsteps_Grass"));
    EXPECT_EQ(rowLabel(hierarchyContainer->child(0)), QStringLiteral("Grp_Footsteps_Stone"));
    EXPECT_EQ(rowLabel(hierarchyContainer->child(1)), QStringLiteral("Grp_Footsteps_Grass"));
    expectTreesEqual("child order signal");
}

TEST_F(ProjectTreeConsistencyTest, SearchVisibleSetsMatchWithFilterAll)
{
    auto* peSearch = explorer_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    auto* hSearch = hierarchy_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(peSearch, nullptr);
    ASSERT_NE(hSearch, nullptr);
    ASSERT_EQ(hierarchy_->currentFilter(), ItemTypeCatalog::Bucket::None);

    peSearch->setText(QStringLiteral("Amb"));
    hSearch->setText(QStringLiteral("Amb"));
    EXPECT_EQ(snapshot(explorer_->treeWidget(), true), snapshot(hierarchy_->treeWidget(), true))
        << "visible sets diverged: search Amb";
    QTreeWidgetItem* peRain = findRow(explorer_->treeWidget(), QStringLiteral("Amb_Rain"));
    QTreeWidgetItem* peSwing = findRow(explorer_->treeWidget(), QStringLiteral("Play_Swing"));
    ASSERT_NE(peRain, nullptr);
    ASSERT_NE(peSwing, nullptr);
    EXPECT_FALSE(peRain->isHidden());
    EXPECT_TRUE(peSwing->isHidden());

    peSearch->setText(QStringLiteral("NoSuchRow"));
    hSearch->setText(QStringLiteral("NoSuchRow"));
    EXPECT_EQ(snapshot(explorer_->treeWidget(), true), snapshot(hierarchy_->treeWidget(), true))
        << "visible sets diverged: search with no matches";

    peSearch->clear();
    hSearch->clear();
    expectTreesEqual("search cleared");
}

TEST_F(ProjectTreeConsistencyTest, RebuildBusTreeKeepsTreesIdentical)
{
    explorer_->rebuildBusTree();
    hierarchy_->rebuildBusTree();
    expectTreesEqual("explicit rebuildBusTree");

    QTreeWidgetItem* peBus = findRow(explorer_->treeWidget(), QStringLiteral("Mix_Music_Sub"));
    QTreeWidgetItem* hBus = findRow(hierarchy_->treeWidget(), QStringLiteral("Mix_Music_Sub"));
    ASSERT_NE(peBus, nullptr);
    ASSERT_NE(hBus, nullptr);
}

// Batch D unified itemCreated with a populated subtree to the ProjectExplorer
// semantics (sprint3 report item 4): both trees build descendant rows
// immediately, no full rebuild needed.
TEST_F(ProjectTreeConsistencyTest, SubtreeItemCreatedBuildsDescendantsOnBothSides)
{
    auto& pm = ProjectManager::instance();
    for (auto* source : {subLayer_, containerRoot_, container_}) {
        auto* branch = pm.createItem(source, QStringLiteral("CopiedBranch"), ProjectItem::Type::Container);
        ASSERT_NE(branch, nullptr);
        ASSERT_NE(pm.createItem(branch, QStringLiteral("CopiedLeaf"), ProjectItem::Type::Sound), nullptr);
        ASSERT_NE(pm.createItem(source, QStringLiteral("CopiedLeaf"), ProjectItem::Type::Sound), nullptr);
    }
    const auto verify = [&](auto&& self, ProjectItem* source, ProjectItem* clone, bool present) -> void {
        ASSERT_NE(clone, nullptr);
        EXPECT_EQ(clone->type, source->type);
        EXPECT_NE(clone->guid, source->guid);
        EXPECT_NE(clone->objectID, source->objectID);
        EXPECT_EQ(pm.findItemByGuid(clone->guid), present ? clone : nullptr);
        ASSERT_EQ(clone->children.size(), source->children.size());
        for (ProjectTreePanel* panel : {static_cast<ProjectTreePanel*>(explorer_.get()),
                                      static_cast<ProjectTreePanel*>(hierarchy_.get())}) {
            ASSERT_EQ(panel->selectProjectItem(clone), present);
            if (!present) continue;
            auto* row = panel->treeWidget()->currentItem();
            ASSERT_NE(row, nullptr);
            EXPECT_EQ(panel->selectedItem(), clone);
            ASSERT_EQ(row->childCount(), static_cast<int>(clone->children.size()));
            for (int i = 0; i < row->childCount(); ++i) {
                panel->treeWidget()->setCurrentItem(row->child(i));
                EXPECT_EQ(panel->selectedItem(), clone->children[static_cast<size_t>(i)].get());
            }
        }
        for (size_t i = 0; i < clone->children.size(); ++i) {
            EXPECT_EQ(clone->children[i]->parent, clone);
            EXPECT_EQ(clone->children[i]->name, source->children[i]->name);
            ASSERT_NO_FATAL_FAILURE(self(self, source->children[i].get(), clone->children[i].get(), present));
        }
    };
    for (auto* source : {layerRoot_, subLayer_, containerRoot_, container_}) {
        for (int operation = 0; operation < 3; ++operation) {
            SCOPED_TRACE(::testing::Message() << source->name.toStdString() << " operation=" << operation);
            ProjectItem* clone = nullptr;
            if (operation == 0) {
                clone = pm.duplicateItem(source);
            } else {
                ASSERT_EQ(pm.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
                const auto created = operation == 1
                    ? pm.pasteClipboardAtBucketRoot(ItemTypeCatalog::kLayersFolderName)
                    : pm.pasteClipboard(layerAlt_);
                ASSERT_EQ(created.size(), 1u);
                clone = created.front();
            }
            ASSERT_NE(clone, nullptr);
            EXPECT_EQ(clone->parent, operation == 0 ? source->parent : operation == 1 ? nullptr : layerAlt_);
            ASSERT_NO_FATAL_FAILURE(verify(verify, source, clone, true));
            expectTreesEqual("subtree created");
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            ASSERT_NO_FATAL_FAILURE(verify(verify, source, clone, false));
            expectTreesEqual("subtree undone");
            ASSERT_TRUE(UndoRedoManager::instance().redo());
            ASSERT_NO_FATAL_FAILURE(verify(verify, source, clone, true));
            expectTreesEqual("subtree redone");
        }
    }
}

TEST_F(ProjectTreeConsistencyTest, HierarchyFilterDimensionPinnedOnHierarchyOnly)
{
    QTreeWidget* h = hierarchy_->treeWidget();
    const auto& buckets = ItemTypeCatalog::allBuckets();

    auto* combo = hierarchy_->findChild<QComboBox*>(QStringLiteral("projectFilterCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 1 + static_cast<int>(buckets.size()));
    EXPECT_EQ(combo->itemText(0), QStringLiteral("All"));
    for (size_t i = 0; i < buckets.size(); ++i) {
        EXPECT_EQ(combo->itemText(1 + static_cast<int>(i)),
                  ProjectManager::bucketDisplayName(ItemTypeCatalog::folderNameOf(buckets[i])));
    }

    for (ItemTypeCatalog::Bucket active : buckets) {
        hierarchy_->setFilter(active);
        ASSERT_EQ(hierarchy_->currentFilter(), active);
        for (ItemTypeCatalog::Bucket b : buckets) {
            QTreeWidgetItem* row = bucketRow(h, ItemTypeCatalog::folderNameOf(b));
            ASSERT_NE(row, nullptr);
            EXPECT_EQ(row->isHidden(), b != active)
                << "bucket " << ItemTypeCatalog::folderNameOf(b).toStdString()
                << " under filter " << ItemTypeCatalog::folderNameOf(active).toStdString();
        }
    }

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Layers);
    EXPECT_FALSE(findRow(h, QStringLiteral("Amb_Rain"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Grp_Footsteps"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Grp_Footsteps_Grass"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Trk_Theme"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Pc_Intro"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Trk_Intro_Sub"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Root_Shot"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Root_Grp"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Root_Trk"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Root_Pc"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Play_Swing"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Mix_Music"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Var_Distance"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Att_Default"))->isHidden());

    auto* hSearch = hierarchy_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(hSearch, nullptr);
    hSearch->setText(QStringLiteral("Play_Swing"));
    EXPECT_TRUE(findRow(h, QStringLiteral("Play_Swing"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Amb_Rain"))->isHidden());
    hSearch->clear();

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Busses);
    EXPECT_FALSE(findRow(h, QStringLiteral("Mix_Music"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Mix_Music_Sub"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Amb_Root"))->isHidden());

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Actions);
    EXPECT_FALSE(findRow(h, QStringLiteral("Play_Swing"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Combat"))->isHidden());
    EXPECT_FALSE(findRow(h, QStringLiteral("Melee"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Amb_Root"))->isHidden());

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Variables);
    EXPECT_FALSE(findRow(h, QStringLiteral("Var_Distance"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Att_Default"))->isHidden());

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Attenuations);
    EXPECT_FALSE(findRow(h, QStringLiteral("Att_Default"))->isHidden());
    EXPECT_TRUE(findRow(h, QStringLiteral("Var_Distance"))->isHidden());

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::None);
    expectTreesEqual("filter restored to All");
}

TEST_F(ProjectTreeConsistencyTest, DuplicateResultSelectionMatchesInBothTrees)
{
    auto& pm = ProjectManager::instance();
    const std::vector<ProjectItem*> created = pm.duplicateItems({soundRoot_, containerRoot_});
    ASSERT_EQ(created.size(), 2u);
    QList<ProjectItem*> results;
    for (ProjectItem* item : created)
        results.append(item);
    explorer_->setSelectedProjectItems(results);
    hierarchy_->setSelectedProjectItems(results);
    EXPECT_EQ(explorer_->selectedItems().size(), 2);
    EXPECT_EQ(hierarchy_->selectedItems().size(), 2);
    expectTreesEqual("duplicate result selection");
}

TEST_F(ProjectTreeConsistencyTest, FolderPresentationTracksSavedLocationsAcrossUndoAndReload)
{
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    expectFolderPresentation(QStringLiteral("Combat"), true);
    expectFolderPresentation(QStringLiteral("Melee"), true);
    for (ProjectTreePanel* panel : {static_cast<ProjectTreePanel*>(explorer_.get()),
                                   static_cast<ProjectTreePanel*>(hierarchy_.get())}) {
        EXPECT_TRUE(findRow(panel->treeWidget(), QStringLiteral("Combat"))->isExpanded());
        EXPECT_TRUE(findRow(panel->treeWidget(), QStringLiteral("Melee"))->isExpanded());
    }

    ASSERT_NE(manager.createFolder(nullptr, QStringLiteral("NewFolder"), QStringLiteral("Layers")), nullptr);
    expectFolderPresentation(QStringLiteral("NewFolder"), true);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(findRow(explorer_->treeWidget(), QStringLiteral("NewFolder")), nullptr);
    EXPECT_EQ(findRow(hierarchy_->treeWidget(), QStringLiteral("NewFolder")), nullptr);
    ASSERT_TRUE(undo.redo());
    expectFolderPresentation(QStringLiteral("NewFolder"), true);
    ASSERT_TRUE(manager.saveProject());
    expectFolderPresentation(QStringLiteral("Combat"), false);
    expectFolderPresentation(QStringLiteral("Melee"), false);
    expectFolderPresentation(QStringLiteral("NewFolder"), false);
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(undo.redo());
    expectFolderPresentation(QStringLiteral("NewFolder"), false);

    ASSERT_TRUE(manager.renameFolder(actionFolder_, QStringLiteral("Battle")));
    expectFolderPresentation(QStringLiteral("Battle"), true);
    expectFolderPresentation(QStringLiteral("Melee"), true);
    ASSERT_TRUE(undo.undo());
    expectFolderPresentation(QStringLiteral("Combat"), false);
    expectFolderPresentation(QStringLiteral("Melee"), false);
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(manager.saveProject());
    expectFolderPresentation(QStringLiteral("Battle"), false);
    expectFolderPresentation(QStringLiteral("Melee"), false);

    ASSERT_TRUE(manager.moveFolder(actionSubFolder_, nullptr, ItemTypeCatalog::kActionsFolderName));
    expectFolderPresentation(QStringLiteral("Melee"), true);
    ASSERT_TRUE(undo.undo());
    expectFolderPresentation(QStringLiteral("Melee"), false);
    ASSERT_TRUE(undo.redo());
    expectFolderPresentation(QStringLiteral("Melee"), true);
    ASSERT_TRUE(manager.saveProject());
    expectFolderPresentation(QStringLiteral("Melee"), false);
    ASSERT_TRUE(undo.undo());
    expectFolderPresentation(QStringLiteral("Melee"), true);
    ASSERT_TRUE(undo.redo());
    expectFolderPresentation(QStringLiteral("Melee"), false);

    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.loadProject(projectPath));
    explorer_ = std::make_unique<ProjectExplorer>();
    expectFolderPresentation(QStringLiteral("Battle"), false);
    expectFolderPresentation(QStringLiteral("Melee"), false);
    expectFolderPresentation(QStringLiteral("NewFolder"), false);
}

TEST_F(ProjectTreeConsistencyTest, FolderExpansionPreferencesFollowDirectoryPaths)
{
    auto& manager = ProjectManager::instance();
    auto* nested = findRow(explorer_->treeWidget(), QStringLiteral("Melee"));
    ASSERT_NE(nested, nullptr);
    nested->setExpanded(false);
    ASSERT_TRUE(manager.renameFolder(actionFolder_, QStringLiteral("Battle")));
    ASSERT_TRUE(manager.moveFolder(actionSubFolder_, nullptr, ItemTypeCatalog::kActionsFolderName));
    nested = findRow(explorer_->treeWidget(), QStringLiteral("Melee"));
    ASSERT_NE(nested, nullptr);
    EXPECT_FALSE(nested->isExpanded());

    explorer_.reset();
    explorer_ = std::make_unique<ProjectExplorer>();
    nested = findRow(explorer_->treeWidget(), QStringLiteral("Melee"));
    ASSERT_NE(nested, nullptr);
    EXPECT_FALSE(nested->isExpanded());
    EXPECT_TRUE(findRow(hierarchy_->treeWidget(), QStringLiteral("Melee"))->isExpanded());

    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    nested = findRow(explorer_->treeWidget(), QStringLiteral("Melee"));
    ASSERT_NE(nested, nullptr);
    EXPECT_FALSE(nested->isExpanded());
    auto* root = findRow(explorer_->treeWidget(), QStringLiteral("Battle"));
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->isExpanded());
}

TEST_F(ProjectTreeConsistencyTest, PingRevealsNestedTargetsWithoutChangingSelection)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* parent = layerAlt_;
    for (int depth = 0; depth < 16; ++depth) {
        parent = manager.createItem(parent, QStringLiteral("Nested_%1").arg(depth),
                                    ProjectItem::Type::Layer);
        ASSERT_NE(parent, nullptr);
    }
    for (int index = 0; index < 40; ++index) {
        ASSERT_NE(manager.createItem(parent, QStringLiteral("Earlier_%1").arg(index),
                                     ProjectItem::Type::Sound), nullptr);
    }
    auto* target = manager.createItem(parent, QStringLiteral("LocateTarget"),
                                      ProjectItem::Type::Sound);
    ASSERT_NE(target, nullptr);

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Busses);
    explorer_->assetsSection()->setExpanded(false);
    for (ProjectTreePanel* panel : {static_cast<ProjectTreePanel*>(explorer_.get()),
                                   static_cast<ProjectTreePanel*>(hierarchy_.get())}) {
        SCOPED_TRACE(panel->objectName().toStdString());
        panel->resize(280, 320);
        panel->show();
        panel->setSelectedProjectItems({soundRoot_});
        auto* tree = panel->treeWidget();
        auto* row = findRow(tree, target->name);
        ASSERT_NE(row, nullptr);
        auto* search = panel->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
        ASSERT_NE(search, nullptr);
        search->setText(QStringLiteral("NoMatchingObject"));
        tree->collapseAll();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        ASSERT_TRUE(row->isHidden());

        int selectionChanges = 0;
        QObject connections;
        QObject::connect(panel, &ProjectTreePanel::objectSelectionChanged, &connections,
                         [&](const QList<ProjectItem*>&) { ++selectionChanges; });
        ASSERT_TRUE(panel->pingProjectItem(target));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        EXPECT_TRUE(search->text().isEmpty());
        for (auto* ancestor = row; ancestor; ancestor = ancestor->parent()) {
            EXPECT_FALSE(ancestor->isHidden());
            if (ancestor != row) EXPECT_TRUE(ancestor->isExpanded());
        }
        const QRect targetRect = tree->visualItemRect(row);
        const QRect viewportRect = tree->viewport()->rect();
        EXPECT_FALSE(targetRect.isEmpty());
        EXPECT_GE(targetRect.left(), viewportRect.left());
        EXPECT_GE(targetRect.top(), viewportRect.top());
        EXPECT_LE(targetRect.bottom(), viewportRect.bottom());
        EXPECT_GE(targetRect.intersected(viewportRect).width(),
                  tree->iconSize().width() + tree->fontMetrics().horizontalAdvance(row->text(0)));
        EXPECT_EQ(panel->selectedItems(), QList<ProjectItem*>{soundRoot_});
        EXPECT_EQ(selectionChanges, 0);
    }
    EXPECT_TRUE(explorer_->assetsSection()->isExpanded());
    EXPECT_EQ(hierarchy_->currentFilter(), ItemTypeCatalog::Bucket::None);

    hierarchy_->setFilter(ItemTypeCatalog::Bucket::Layers);
    for (ProjectTreePanel* panel : {static_cast<ProjectTreePanel*>(explorer_.get()),
                                   static_cast<ProjectTreePanel*>(hierarchy_.get())}) {
        auto* search = panel->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
        search->setText(target->name);
        ASSERT_TRUE(panel->pingProjectItem(target));
        EXPECT_EQ(search->text(), target->name);
    }
    EXPECT_EQ(hierarchy_->currentFilter(), ItemTypeCatalog::Bucket::Layers);
}
