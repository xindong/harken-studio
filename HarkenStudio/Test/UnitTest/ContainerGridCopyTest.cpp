#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "GridSemantics.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
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
        ASSERT_NE(layerR_, nullptr);

        containerA_ = manager.createItem(layerR_, QStringLiteral("Container_A"),
                                         ProjectItem::Type::Container);
        ASSERT_NE(containerA_, nullptr);

        soundA_ = manager.createItem(containerA_, QStringLiteral("Sound_A"),
                                     ProjectItem::Type::Sound);
        ASSERT_NE(soundA_, nullptr);

        variable_ = manager.createItem(nullptr, QStringLiteral("Variable"),
                                       ProjectItem::Type::Variable);
        ASSERT_NE(variable_, nullptr);

        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult setProperty(ProjectItem* owner, const QString& name,
                             const QJsonValue& value)
    {
        if (name == QStringLiteral("matches")) {
            auto selections = owner->comp<VSelection>()->selections;
            if (!selections.shared.setMatchGrid(DiscreteGridDef::fromJson(value.toObject()))) return {};
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

    const VSelection* selectionOf(const ProjectItem* owner) const
    {
        return owner ? owner->comp<VSelection>() : nullptr;
    }

    QTemporaryDir tempDir_;
    ProjectItem* layerR_ = nullptr;
    ProjectItem* containerA_ = nullptr;
    ProjectItem* soundA_ = nullptr;
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
