#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QTemporaryDir>

namespace VNX {
bool claimsColorItemProperty(ProjectItem*, const QString&);
bool claimsNotesItemProperty(ProjectItem*, const QString&);
}

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

int undoStackCount()
{
    return VNX::UndoRedoManager::instance().undoStack()->count();
}

} // namespace

class NotesColorCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("NotesColorProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
    }

    QTemporaryDir tempDir;
};

TEST_F(NotesColorCommandTest, NotesClaimIsPropertyNameScoped)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("NC_ClaimLayer"),
                                            VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);

    EXPECT_TRUE(VNX::claimsNotesItemProperty(layer, QStringLiteral("notes")));
    EXPECT_FALSE(VNX::claimsNotesItemProperty(layer, QStringLiteral("Notes")));
    EXPECT_FALSE(VNX::claimsColorItemProperty(layer, QStringLiteral("notes")));
}

TEST_F(NotesColorCommandTest, NotesExecuteUndoRedoRoundtripOnTreeItem)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("NC_Layer"),
                                            VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(layer->notes.isEmpty());

    const VNX::ActionResult r = handleNotesItemProperty(
        &pm, layer, QStringLiteral("notes"),
        QJsonObject{{QStringLiteral("value"), QStringLiteral("first note")}}, nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_EQ(layer->notes, QStringLiteral("first note"));
    EXPECT_TRUE(layer->isDirtyObject());
    ASSERT_TRUE(static_cast<bool>(r.undoFn));
    ASSERT_TRUE(static_cast<bool>(r.redoFn));

    r.undoFn();
    EXPECT_TRUE(layer->notes.isEmpty());

    r.redoFn();
    EXPECT_EQ(layer->notes, QStringLiteral("first note"));
}

TEST_F(NotesColorCommandTest, NotesRoundtripResolvesBusItems)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* master = pm.getMasterBusItem();
    ASSERT_NE(master, nullptr);

    const VNX::ActionResult r = handleNotesItemProperty(
        &pm, master, QStringLiteral("notes"),
        QJsonObject{{QStringLiteral("value"), QStringLiteral("bus routing doc")}}, nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_EQ(master->notes, QStringLiteral("bus routing doc"));
    ASSERT_TRUE(static_cast<bool>(r.undoFn));

    r.undoFn();
    EXPECT_TRUE(master->notes.isEmpty());

    r.redoFn();
    EXPECT_EQ(master->notes, QStringLiteral("bus routing doc"));
}

TEST_F(NotesColorCommandTest, NotesUnchangedValueYieldsNoUndo)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("NC_SameLayer"),
                                            VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    layer->notes = QStringLiteral("kept");

    const VNX::ActionResult r = handleNotesItemProperty(
        &pm, layer, QStringLiteral("notes"),
        QJsonObject{{QStringLiteral("value"), QStringLiteral("kept")}}, nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_FALSE(static_cast<bool>(r.undoFn));
    EXPECT_FALSE(static_cast<bool>(r.redoFn));
}

TEST_F(NotesColorCommandTest, AudioRouteStillRejectsNotes)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* sound = pm.createItem(nullptr, QStringLiteral("NC_Sound"),
                                            VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);

    const VNX::ActionResult r = handleAudioItemProperty(
        &pm, sound, QStringLiteral("notes"),
        QJsonObject{{QStringLiteral("value"), QStringLiteral("ignored")}}, nullptr);

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("UNKNOWN_PROPERTY"));
}

TEST_F(NotesColorCommandTest, BatchColorUsesOneUndoAndSkipsUnchanged)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* first = pm.createItem(nullptr, QStringLiteral("NC_BatchA"),
                                            VNX::ProjectItem::Type::Layer);
    VNX::ProjectItem* second = pm.createItem(nullptr, QStringLiteral("NC_BatchB"),
                                             VNX::ProjectItem::Type::Layer);
    VNX::ProjectItem* child = pm.createItem(first, QStringLiteral("NC_BatchChild"),
                                            VNX::ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(child, nullptr);
    child->color = 0xFF112233u;

    const int before = undoStackCount();
    EXPECT_TRUE(pm.setItemsColor({first, child, second}, 0xFF445566u));
    EXPECT_EQ(first->color, 0xFF445566u);
    EXPECT_EQ(second->color, 0xFF445566u);
    EXPECT_EQ(child->color, 0xFF445566u);
    EXPECT_EQ(undoStackCount(), before + 1);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(first->color, 0u);
    EXPECT_EQ(second->color, 0u);
    EXPECT_EQ(child->color, 0xFF112233u);

    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(first->color, 0xFF445566u);
    EXPECT_EQ(second->color, 0xFF445566u);
    EXPECT_EQ(child->color, 0xFF445566u);

    EXPECT_TRUE(pm.setItemsColor({first, second, child}, 0xFF445566u));
    EXPECT_EQ(undoStackCount(), before + 1);
}
