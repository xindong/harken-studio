#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "GridSemantics.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

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

QString guidString(const ProjectItem* item)
{
    return item->guid.toString(QUuid::WithoutBraces);
}

QUndoStack* undoStack()
{
    return UndoRedoManager::instance().undoStack();
}

int descendantCount(const ProjectItem* item)
{
    if (!item) return 0;
    int count = 1;
    for (const auto& child : item->children)
        count += descendantCount(child.get());
    return count;
}

ProjectItem* childAt(ProjectItem* parent, size_t index)
{
    if (!parent || index >= parent->children.size()) return nullptr;
    return parent->children[index].get();
}


} // namespace

class ContainerGridCopyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ContainerGridCopy");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        layerR_ = manager.createItem(nullptr, QStringLiteral("Layer_R"),
                                     ProjectItem::Type::Layer);
        layerX_ = manager.createItem(nullptr, QStringLiteral("Layer_X"),
                                     ProjectItem::Type::Layer);
        ASSERT_NE(layerR_, nullptr);
        ASSERT_NE(layerX_, nullptr);

        containerA_ = manager.createItem(layerR_, QStringLiteral("Container_A"),
                                         ProjectItem::Type::Container);
        containerB_ = manager.createItem(layerR_, QStringLiteral("Container_B"),
                                         ProjectItem::Type::Container);
        ASSERT_NE(containerA_, nullptr);
        ASSERT_NE(containerB_, nullptr);

        soundA_ = manager.createItem(containerA_, QStringLiteral("Sound_A"),
                                     ProjectItem::Type::Sound);
        soundB_ = manager.createItem(containerA_, QStringLiteral("Sound_B"),
                                     ProjectItem::Type::Sound);
        containerN_ = manager.createItem(containerA_, QStringLiteral("Container_N"),
                                         ProjectItem::Type::Container);
        ASSERT_NE(soundA_, nullptr);
        ASSERT_NE(soundB_, nullptr);
        ASSERT_NE(containerN_, nullptr);
        soundN_ = manager.createItem(containerN_, QStringLiteral("Sound_N"),
                                     ProjectItem::Type::Sound);
        ASSERT_NE(soundN_, nullptr);

        soundX_ = manager.createItem(layerX_, QStringLiteral("Sound_X"),
                                     ProjectItem::Type::Sound);
        variable_ = manager.createItem(nullptr, QStringLiteral("Variable"),
                                       ProjectItem::Type::Variable);
        ASSERT_NE(soundX_, nullptr);
        ASSERT_NE(variable_, nullptr);

        errorConnection_ = QObject::connect(
            &manager, &ProjectManager::errorOccurred,
            [this](const QString& error) { errors_ << error; });
        UndoRedoManager::instance().clear();
        errors_.clear();
    }

    void TearDown() override
    {
        QObject::disconnect(errorConnection_);
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult setProperty(ProjectItem* owner, const QString& name,
                             const QJsonValue& value)
    {
        if (name == QStringLiteral("matches") || name == QStringLiteral("blends")) {
            auto selections = owner->comp<VSelection>()->selections;
            if (name == QStringLiteral("matches")) {
                if (!selections.shared.setMatchGrid(DiscreteGridDef::fromJson(value.toObject()))) return {};
            } else {
                const auto mixing = MixingDef::fromJson(value.toArray());
                if (!mixing || !selections.shared.setMixing(*mixing)) return {};
            }
            return setProperty(owner, QStringLiteral("selections"), selections.toJson());
        }
        return ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"),
                 static_cast<qint64>(owner->objectID)},
                {QStringLiteral("property"), name},
                {QStringLiteral("value"), value}},
            ActionSource::GUI);
    }

    ActionResult setMode(ProjectItem* owner, SelectionMode mode = SelectionMode::Match)
    {
        return setProperty(owner, QStringLiteral("mode"),
                           static_cast<int>(mode));
    }

    QJsonObject discreteRow(const ProjectItem* target, const QJsonArray& cells) const
    {
        return QJsonObject{{QStringLiteral("target"), guidString(target)},
                           {QStringLiteral("cells"), cells}};
    }

    QJsonObject discreteGrid(const ProjectItem* target, const QJsonArray& cells) const
    {
        return QJsonObject{
            {QStringLiteral("axes"), QJsonArray{guidString(variable_)}},
            {QStringLiteral("rows"), QJsonArray{discreteRow(target, cells)}}};
    }

    QJsonArray blends(const ProjectItem* target) const
    {
        MixingChildDef child{target->guid, {{0.0f, 0.0f}, {0.5f, 0.8f}, {1.0f, 1.0f}},
            {{0.1f, 0.0f, 0.3f, 0.8f}, {0.7f, 0.8f, 0.9f, 1.0f}}};
        return MixingDef{{{variable_->guid, {child}}}}.toJson();
    }

    void authorNestedMatchBlend()
    {
        ASSERT_TRUE(setMode(containerA_).success);
        ASSERT_TRUE(setMode(containerN_, SelectionMode::Blend).success);
        ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                                discreteGrid(containerN_, QJsonArray{1})).success);
        ASSERT_TRUE(setProperty(containerN_, QStringLiteral("blends"),
                                blends(soundN_)).success);
    }

    const VSelection* selectionOf(const ProjectItem* owner) const
    {
        return owner ? owner->comp<VSelection>() : nullptr;
    }

    QTemporaryDir tempDir_;
    QMetaObject::Connection errorConnection_;
    QStringList errors_;
    ProjectItem* layerR_ = nullptr;
    ProjectItem* layerX_ = nullptr;
    ProjectItem* containerA_ = nullptr;
    ProjectItem* containerB_ = nullptr;
    ProjectItem* containerN_ = nullptr;
    ProjectItem* soundA_ = nullptr;
    ProjectItem* soundB_ = nullptr;
    ProjectItem* soundN_ = nullptr;
    ProjectItem* soundX_ = nullptr;
    ProjectItem* variable_ = nullptr;
};

TEST_F(ContainerGridCopyTest, DuplicateRemapsDiscreteGridToClonedChild)
{
    ASSERT_TRUE(setMode(containerA_).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid(soundA_, QJsonArray{1})).success);

    const auto* original = selectionOf(containerA_);
    ASSERT_EQ(original->matchGrid().rows.size(), 1u);
    EXPECT_EQ(original->matchGrid().rows[0].target, soundA_->guid);

    auto* clone = ProjectManager::instance().duplicateItem(containerA_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->children.size(), containerA_->children.size());
    auto* soundA2 = childAt(clone, 0);
    ASSERT_NE(soundA2, nullptr);
    EXPECT_NE(soundA2->guid, soundA_->guid);

    const auto* cloned = selectionOf(clone);
    ASSERT_EQ(cloned->matchGrid().rows.size(), 1u);
    EXPECT_EQ(cloned->matchGrid().rows[0].target, soundA2->guid);
    const auto value = [&](const ProjectItem* owner) {
        return GridSemantics::discreteCellValue(*selectionOf(owner)->matchGrid().cellAt(0, 0),
            variable_->comp<VVariable>());
    };
    EXPECT_EQ(value(clone), std::optional<int>(1));
    EXPECT_EQ(cloned->matchGrid().cellAt(0, 0)->labelId,
              original->matchGrid().cellAt(0, 0)->labelId);
    EXPECT_EQ(cloned->matchGrid().axes[0], variable_->guid);
    EXPECT_EQ(original->matchGrid().rows[0].target, soundA_->guid);

    QJsonObject editedClone = discreteGrid(soundA2, QJsonArray{4});
    ASSERT_TRUE(setProperty(clone, QStringLiteral("matches"), editedClone).success);
    EXPECT_EQ(value(clone), std::optional<int>(4));
    EXPECT_EQ(value(containerA_), std::optional<int>(1));
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid(soundA_, QJsonArray{7})).success);
    EXPECT_EQ(value(containerA_), std::optional<int>(7));
    EXPECT_EQ(value(clone), std::optional<int>(4));
}

TEST_F(ContainerGridCopyTest, DuplicatePreservesContinuousSpanAndCurves)
{
    ASSERT_TRUE(setMode(containerA_, SelectionMode::Blend).success);
    const auto parsedTracks = MixingDef::fromJson(blends(soundA_));
    ASSERT_TRUE(parsedTracks.has_value());
    auto originalMixing = *parsedTracks;
    originalMixing.rows.push_back({{}, {originalMixing.rows.front().children.front()}});
    originalMixing.rows.push_back({{}, {MixingChildDef::unity(soundA_->guid, 2.0f, 6.0f)}});
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("blends"),
                            originalMixing.toJson()).success);

    auto* clone = ProjectManager::instance().duplicateItem(containerA_);
    ASSERT_NE(clone, nullptr);
    auto* soundA2 = childAt(clone, 0);
    ASSERT_NE(soundA2, nullptr);

    const auto* cloned = selectionOf(clone);
    auto copiedMixing = cloned->mixing();
    ASSERT_EQ(copiedMixing.rows.size(), originalMixing.rows.size());
    for (size_t i = 0; i < copiedMixing.rows.size(); ++i) {
        auto& copied = copiedMixing.rows[i];
        ASSERT_EQ(copied.children.size(), 1u);
        EXPECT_NE(copied.guid, originalMixing.rows[i].guid);
        EXPECT_EQ(copied.children.front().target, soundA2->guid);
        copied.guid = originalMixing.rows[i].guid;
        copied.children.front().target = soundA_->guid;
    }
    EXPECT_EQ(copiedMixing.toJson(), originalMixing.toJson());
    EXPECT_EQ(selectionOf(containerA_)->mixing().toJson(), originalMixing.toJson());
}

TEST_F(ContainerGridCopyTest, NestedMatchBlendRemapsEachMemberGraph)
{
    ASSERT_NO_FATAL_FAILURE(authorNestedMatchBlend());
    auto* clone = ProjectManager::instance().duplicateItem(containerA_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->children.size(), containerA_->children.size());
    auto* nested = childAt(clone, 2);
    ASSERT_NE(nested, nullptr);
    auto* sound = childAt(nested, 0);
    ASSERT_NE(sound, nullptr);

    const auto* clonedMatch = selectionOf(clone);
    const auto* clonedBlend = selectionOf(nested);
    EXPECT_EQ(clonedMatch->mode, SelectionMode::Match);
    EXPECT_EQ(clonedBlend->mode, SelectionMode::Blend);
    ASSERT_EQ(clonedMatch->matchGrid().rows.size(), 1u);
    ASSERT_EQ(clonedBlend->mixing().rows.front().children.size(), 1u);
    EXPECT_EQ(clonedMatch->matchGrid().rows[0].target, nested->guid);
    EXPECT_EQ(clonedBlend->mixing().rows.front().children[0].target, sound->guid);
    EXPECT_TRUE(clonedMatch->mixing().empty());
    EXPECT_TRUE(clonedBlend->matchGrid().empty());
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, containerN_->guid);
    EXPECT_EQ(selectionOf(containerN_)->mixing().rows.front().children[0].target, soundN_->guid);
}

TEST_F(ContainerGridCopyTest, NestedCopyKeepsDirectChildAtEachLayer)
{
    ASSERT_TRUE(setMode(containerA_).success);
    ASSERT_TRUE(setMode(containerN_).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid(containerN_, QJsonArray{1})).success);
    ASSERT_TRUE(setProperty(containerN_, QStringLiteral("matches"),
                            discreteGrid(soundN_, QJsonArray{3})).success);

    auto* clone = ProjectManager::instance().duplicateItem(containerA_);
    ASSERT_NE(clone, nullptr);
    auto* nested = childAt(clone, 2);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->type, ProjectItem::Type::Container);
    auto* soundN2 = childAt(nested, 0);
    ASSERT_NE(soundN2, nullptr);

    EXPECT_EQ(selectionOf(clone)->matchGrid().rows[0].target, nested->guid);
    EXPECT_EQ(selectionOf(nested)->matchGrid().rows[0].target, soundN2->guid);
    EXPECT_TRUE(GridSemantics::isGridTargetCandidate(*clone, nested->guid));
    EXPECT_TRUE(GridSemantics::isGridTargetCandidate(*nested, soundN2->guid));
    EXPECT_FALSE(GridSemantics::isGridTargetCandidate(*clone, soundN2->guid));
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, containerN_->guid);
    EXPECT_EQ(selectionOf(containerN_)->matchGrid().rows[0].target, soundN_->guid);
}

TEST_F(ContainerGridCopyTest, PasteOntoAnotherLayerUsesTheSameRemap)
{
    ASSERT_NO_FATAL_FAILURE(authorNestedMatchBlend());
    const QUuid originalA = containerA_->guid;
    const QUuid originalNested = containerN_->guid;
    const QUuid originalSound = soundN_->guid;
    const int layerRCount = static_cast<int>(layerR_->children.size());
    const int layerXCount = static_cast<int>(layerX_->children.size());

    auto& manager = ProjectManager::instance();
    ASSERT_EQ(manager.setClipboard({containerA_}, ProjectManager::ClipboardOp::Copy), 1);
    const auto pasted = manager.pasteClipboard(layerX_);
    ASSERT_EQ(pasted.size(), 1u);
    auto* clone = pasted.front();
    ASSERT_EQ(clone->parent, layerX_);
    auto* nested = childAt(clone, 2);
    ASSERT_NE(nested, nullptr);
    auto* sound = childAt(nested, 0);
    ASSERT_NE(sound, nullptr);

    EXPECT_EQ(containerA_->parent, layerR_);
    EXPECT_EQ(containerA_->guid, originalA);
    EXPECT_EQ(containerN_->parent, containerA_);
    EXPECT_EQ(containerN_->guid, originalNested);
    EXPECT_EQ(soundN_->parent, containerN_);
    EXPECT_EQ(soundN_->guid, originalSound);
    EXPECT_EQ(static_cast<int>(layerR_->children.size()), layerRCount);
    EXPECT_EQ(static_cast<int>(layerX_->children.size()), layerXCount + 1);

    EXPECT_EQ(selectionOf(clone)->matchGrid().rows[0].target, nested->guid);
    EXPECT_EQ(selectionOf(nested)->mixing().rows.front().children[0].target, sound->guid);
    EXPECT_EQ(selectionOf(clone)->matchGrid().axes[0], variable_->guid);
    EXPECT_EQ(selectionOf(nested)->mixing().rows[0].variable, variable_->guid);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, originalNested);
    EXPECT_EQ(selectionOf(containerN_)->mixing().rows.front().children[0].target, originalSound);
}

TEST_F(ContainerGridCopyTest, RepeatedPasteIntoSourceKeepsTheCopiedSubtree)
{
    ASSERT_NO_FATAL_FAILURE(authorNestedMatchBlend());
    auto& manager = ProjectManager::instance();
    for (ProjectItem* source : {containerB_, containerA_}) {
        const int count = descendantCount(source);
        const auto childCount = source->children.size();
        const auto nextObject = manager.currentProject()->nextObjectID;
        const auto undoCount = undoStack()->count();
        ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
        EXPECT_EQ(manager.currentProject()->nextObjectID, nextObject);
        EXPECT_EQ(undoStack()->count(), undoCount);
        QSet<QUuid> identities{source->guid};
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(manager.canPasteClipboard(source));
            const auto pasted = manager.pasteClipboard(source);
            ASSERT_EQ(pasted.size(), 1u) << errors_.join(';').toStdString();
            auto* copy = pasted.front();
            EXPECT_EQ(descendantCount(copy), count);
            EXPECT_EQ(copy->children.size(), childCount);
            EXPECT_EQ(copy->parent, source);
            const auto collect = [&](auto&& self, const ProjectItem* item) -> void {
                EXPECT_FALSE(identities.contains(item->guid));
                identities.insert(item->guid);
                for (const auto& child : item->children)
                    self(self, child.get());
            };
            collect(collect, copy);
            if (source == containerA_) {
                auto* nested = childAt(copy, 2);
                ASSERT_NE(nested, nullptr);
                EXPECT_EQ(selectionOf(copy)->matchGrid().rows.front().target, nested->guid);
                EXPECT_EQ(selectionOf(nested)->mixing().rows.front().children.front().target,
                          childAt(nested, 0)->guid);
            }
            const auto guid = copy->guid;
            undoStack()->undo();
            EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
            undoStack()->redo();
            ASSERT_NE(manager.findItemByGuid(guid), nullptr);
            EXPECT_EQ(descendantCount(manager.findItemByGuid(guid)), count);
        }
    }
}

TEST_F(ContainerGridCopyTest, CopySurvivesSourceEditsAndDeletion)
{
    ASSERT_NO_FATAL_FAILURE(authorNestedMatchBlend());
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("volume"), -6.0).success);
    containerA_->notes = QStringLiteral("Copied notes");
    auto& manager = ProjectManager::instance();
    const int count = descendantCount(containerA_);
    ASSERT_EQ(manager.setClipboard({containerA_}, ProjectManager::ClipboardOp::Copy), 1);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("volume"), -18.0).success);
    ASSERT_TRUE(manager.renameItem(containerA_, QStringLiteral("RenamedSource")));
    containerA_->notes = QStringLiteral("Changed notes");
    ASSERT_NE(manager.createItem(containerA_, QStringLiteral("NewChild"), ProjectItem::Type::Sound), nullptr);

    const auto first = manager.pasteClipboard(layerX_);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(descendantCount(first.front()), count);
    EXPECT_EQ(first.front()->name, QStringLiteral("Container_A"));
    EXPECT_EQ(first.front()->notes, QStringLiteral("Copied notes"));
    EXPECT_FLOAT_EQ(first.front()->comp<VMixNode>()->volume, -6.0f);

    ASSERT_TRUE(manager.deleteItem(layerR_));
    UndoRedoManager::instance().clear();
    const auto second = manager.pasteClipboardAtBucketRoot(QStringLiteral("Layers"));
    ASSERT_EQ(second.size(), 1u) << errors_.join(';').toStdString();
    EXPECT_EQ(descendantCount(second.front()), count);
    EXPECT_EQ(second.front()->name, QStringLiteral("Container_A"));
    EXPECT_EQ(second.front()->notes, QStringLiteral("Copied notes"));
    EXPECT_FLOAT_EQ(second.front()->comp<VMixNode>()->volume, -6.0f);
    auto* nested = childAt(second.front(), 2);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(selectionOf(second.front())->matchGrid().rows.front().target, nested->guid);
    EXPECT_EQ(selectionOf(nested)->mixing().rows.front().children.front().target,
              childAt(nested, 0)->guid);
    EXPECT_EQ(selectionOf(second.front())->matchGrid().axes.front(), variable_->guid);

    EXPECT_TRUE(manager.pastePropertiesFromClipboard({first.front()}));
    ASSERT_TRUE(setProperty(first.front(), QStringLiteral("volume"), -24.0).success);
    ASSERT_TRUE(manager.pastePropertiesFromClipboard({first.front()}));
    EXPECT_FLOAT_EQ(first.front()->comp<VMixNode>()->volume, -6.0f);
    undoStack()->undo();
    EXPECT_FLOAT_EQ(first.front()->comp<VMixNode>()->volume, -24.0f);
    undoStack()->redo();
    EXPECT_FLOAT_EQ(first.front()->comp<VMixNode>()->volume, -6.0f);
}

TEST_F(ContainerGridCopyTest, ClosingProjectClearsCopiedObjects)
{
    auto& manager = ProjectManager::instance();
    ASSERT_EQ(manager.setClipboard({containerA_}, ProjectManager::ClipboardOp::Copy), 1);
    ASSERT_TRUE(manager.closeProject());
    EXPECT_TRUE(manager.clipboardEmpty());
    EXPECT_EQ(manager.clipboardCount(), 0);
    EXPECT_TRUE(manager.resolveClipboardSources().empty());
}

TEST_F(ContainerGridCopyTest, IndependentCopiesSurviveDeletingTheOriginal)
{
    ASSERT_TRUE(setMode(containerA_).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid(soundA_, QJsonArray{5})).success);

    auto& manager = ProjectManager::instance();
    auto* clone2 = manager.duplicateItem(containerA_);
    auto* clone3 = manager.duplicateItem(containerA_);
    ASSERT_NE(clone2, nullptr);
    ASSERT_NE(clone3, nullptr);
    const QUuid clone2Guid = clone2->guid;
    const QUuid clone3Guid = clone3->guid;
    const QUuid sound2 = childAt(clone2, 0)->guid;
    const QUuid sound3 = childAt(clone3, 0)->guid;
    EXPECT_NE(sound2, sound3);

    ASSERT_TRUE(manager.deleteItem(containerA_));
    EXPECT_EQ(manager.findItemByGuid(clone2Guid), clone2);
    EXPECT_EQ(manager.findItemByGuid(clone3Guid), clone3);
    EXPECT_EQ(selectionOf(clone2)->matchGrid().rows[0].target, sound2);
    EXPECT_EQ(selectionOf(clone3)->matchGrid().rows[0].target, sound3);
    EXPECT_TRUE(GridSemantics::isGridTargetCandidate(*clone2, sound2));
    EXPECT_TRUE(GridSemantics::isGridTargetCandidate(*clone3, sound3));
    EXPECT_FALSE(GridSemantics::isGridTargetCandidate(*clone2, sound3));
}

TEST_F(ContainerGridCopyTest, IllegalSourceGridRefusesTheCopy)
{
    ASSERT_TRUE(setMode(containerA_).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid(soundA_, QJsonArray{1})).success);
    auto* selection = containerA_->comp<VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->selections.shared.member(soundA_->guid)->childGuid = soundX_->guid;

    const int layerRCount = static_cast<int>(layerR_->children.size());
    const int layerXCount = static_cast<int>(layerX_->children.size());
    const int aCount = descendantCount(containerA_);
    const int undoCount = undoStack()->count();
    errors_.clear();

    auto* clone = ProjectManager::instance().duplicateItem(containerA_);
    EXPECT_EQ(clone, nullptr);
    ASSERT_FALSE(errors_.isEmpty());
    EXPECT_TRUE(errors_.front().contains(QStringLiteral("Container_A")));
    EXPECT_TRUE(errors_.front().contains(QStringLiteral("direct child")));
    EXPECT_TRUE(errors_.front().contains(guidString(soundX_)));
    EXPECT_EQ(static_cast<int>(layerR_->children.size()), layerRCount);
    EXPECT_EQ(static_cast<int>(layerX_->children.size()), layerXCount);
    EXPECT_EQ(descendantCount(containerA_), aCount);
    EXPECT_EQ(soundX_->parent, layerX_);
    EXPECT_EQ(undoStack()->count(), undoCount);
    EXPECT_EQ(containerB_->children.size(), 0u);
}

TEST_F(ContainerGridCopyTest, UndoRedoSaveAndReloadKeepDirectChildTargets)
{
    ASSERT_NO_FATAL_FAILURE(authorNestedMatchBlend());
    auto& manager = ProjectManager::instance();
    auto* clone = manager.duplicateItem(containerA_);
    ASSERT_NE(clone, nullptr);
    auto* nested = childAt(clone, 2);
    ASSERT_NE(nested, nullptr);
    const QUuid cloneGuid = clone->guid;
    const QUuid nestedGuid = nested->guid;
    const QUuid soundGuid = childAt(nested, 0)->guid;
    const QUuid originalGuid = containerA_->guid;
    const QUuid originalNested = containerN_->guid;
    const QUuid originalSound = soundN_->guid;
    EXPECT_EQ(selectionOf(clone)->matchGrid().rows[0].target, nestedGuid);

    undoStack()->undo();
    EXPECT_EQ(manager.findItemByGuid(cloneGuid), nullptr);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, originalNested);

    undoStack()->redo();
    clone = manager.findItemByGuid(cloneGuid);
    nested = manager.findItemByGuid(nestedGuid);
    ASSERT_NE(clone, nullptr);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(selectionOf(clone)->matchGrid().rows[0].target, nestedGuid);
    EXPECT_EQ(selectionOf(nested)->mixing().rows.front().children[0].target, soundGuid);
    EXPECT_TRUE(GridSemantics::isGridTargetCandidate(*clone, nestedGuid));
    EXPECT_TRUE(GridSemantics::isGridTargetCandidate(*nested, soundGuid));

    const QUuid variableGuid = variable_->guid;
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.loadProject(projectPath));
    const auto* reloaded = manager.findItemByGuid(cloneGuid);
    const auto* reloadedNested = manager.findItemByGuid(nestedGuid);
    const auto* reloadedSound = manager.findItemByGuid(soundGuid);
    ASSERT_NE(reloaded, nullptr);
    ASSERT_NE(reloadedNested, nullptr);
    ASSERT_NE(reloadedSound, nullptr);
    EXPECT_EQ(reloadedNested->parent, reloaded);
    EXPECT_EQ(reloadedSound->parent, reloadedNested);
    const auto* reloadedMatch = reloaded->comp<VSelection>();
    const auto* reloadedBlend = reloadedNested->comp<VSelection>();
    ASSERT_NE(reloadedMatch, nullptr);
    ASSERT_NE(reloadedBlend, nullptr);
    EXPECT_EQ(reloadedMatch->mode, SelectionMode::Match);
    EXPECT_EQ(reloadedBlend->mode, SelectionMode::Blend);
    EXPECT_EQ(reloadedMatch->matchGrid().rows[0].target, nestedGuid);
    EXPECT_EQ(reloadedBlend->mixing().rows.front().children[0].target, soundGuid);
    EXPECT_EQ(reloadedMatch->matchGrid().axes[0], variableGuid);
    EXPECT_EQ(reloadedBlend->mixing().rows[0].variable, variableGuid);
    EXPECT_TRUE(reloadedMatch->mixing().empty());
    EXPECT_TRUE(reloadedBlend->matchGrid().empty());

    const auto* reloadedOriginal = manager.findItemByGuid(originalGuid);
    const auto* reloadedOriginalNested = manager.findItemByGuid(originalNested);
    ASSERT_NE(reloadedOriginal, nullptr);
    ASSERT_NE(reloadedOriginalNested, nullptr);
    EXPECT_EQ(selectionOf(reloadedOriginal)->matchGrid().rows[0].target, originalNested);
    EXPECT_EQ(selectionOf(reloadedOriginalNested)->mixing().rows.front().children[0].target, originalSound);
}
