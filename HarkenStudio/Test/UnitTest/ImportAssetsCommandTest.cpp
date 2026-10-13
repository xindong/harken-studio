#include "AssetPipeline/ImportAssetsCommand.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDataStream>
#include <QFile>
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

// The import funnel must reject a file whose stem cannot become a legal object
// name before any side effect: no copy, no registry record, no tree node, and
// an honest fail count so the entry points' summary dialog fires. Previously
// the file was copied and logged as success while the tree object silently
// failed ("invalid name" dialog yet the audio landed in AudioSources).
class ImportAssetsCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ImportNamePreflightProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
};

} // namespace

TEST_F(ImportAssetsCommandTest, SkipsFileWhoseStemIsNotAValidObjectName)
{
    ProjectItem* layer = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Imported Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);

    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ProjectFileBinding* binding = findFileBindingForItem(*project, layer);
    ASSERT_NE(binding, nullptr);

    const QString stem(129, QLatin1Char('A'));
    const QString wavPath =
        tempDir_.path() + QStringLiteral("/") + stem + QStringLiteral(".wav");
    {
        QFile wav(wavPath);
        ASSERT_TRUE(wav.open(QIODevice::WriteOnly));
        wav.write("RIFF");
    }

    auto* cmd = ImportAssetsCommand::pushImport({wavPath}, binding, layer);

    EXPECT_EQ(cmd->successCount(), 0);
    EXPECT_EQ(cmd->failCount(), 1);
    EXPECT_EQ(cmd->duplicateCount(), 0);
    EXPECT_TRUE(cmd->importedIDs().isEmpty());
    EXPECT_TRUE(layer->children.empty());
    EXPECT_TRUE(QFile::exists(wavPath));
}

// Root mode's own preflight mirrors the parent-mode one: a stem that cannot
// become a legal object name must be skipped before any side effect, with no
// root Sound created.
TEST_F(ImportAssetsCommandTest, RootModeSkipsFileWhoseStemIsNotAValidObjectName)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    const QString stem(129, QLatin1Char('A'));
    const QString wavPath =
        tempDir_.path() + QStringLiteral("/") + stem + QStringLiteral(".wav");
    {
        QFile wav(wavPath);
        ASSERT_TRUE(wav.open(QIODevice::WriteOnly));
        wav.write("RIFF");
    }

    auto* cmd = ImportAssetsCommand::pushImportAtRoot({wavPath});

    EXPECT_EQ(cmd->successCount(), 0);
    EXPECT_EQ(cmd->failCount(), 1);
    EXPECT_EQ(cmd->duplicateCount(), 0);
    EXPECT_TRUE(cmd->importedIDs().isEmpty());
    EXPECT_TRUE(project->layers.empty());
    EXPECT_TRUE(QFile::exists(wavPath));
}

TEST_F(ImportAssetsCommandTest, FailedRootImportDoesNotOccupyUndoStack)
{
    UndoRedoManager::instance().clear();
    ASSERT_FALSE(UndoRedoManager::instance().canUndo());

    const QString missing = tempDir_.path() + QStringLiteral("/missing.wav");
    ASSERT_FALSE(QFile::exists(missing));

    auto* cmd = ImportAssetsCommand::pushImportAtRoot({missing});
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->successCount(), 0);
    EXPECT_GE(cmd->failCount(), 1);
    EXPECT_FALSE(UndoRedoManager::instance().canUndo());
    EXPECT_TRUE(ProjectManager::instance().currentProject()->layers.empty());
}

TEST_F(ImportAssetsCommandTest, UnicodeImportUsesSiblingNamesAndRedoRestoresOriginalIdentity)
{
    auto& pm = ProjectManager::instance();
    const QString stem = QStringLiteral("\u97f3\u6548");
    ASSERT_NE(pm.createItem(nullptr, stem, ProjectItem::Type::Sound), nullptr);
    auto* parent = pm.createItem(nullptr, QStringLiteral("Container"), ProjectItem::Type::Container);
    ASSERT_NE(parent, nullptr);
    const QString wavPath = tempDir_.filePath(stem + QStringLiteral(".wav"));
    {
        QFile wav(wavPath);
        ASSERT_TRUE(wav.open(QIODevice::WriteOnly));
        QDataStream stream(&wav);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF", 4);
        stream << quint32(38);
        stream.writeRawData("WAVE", 4);
        stream.writeRawData("fmt ", 4);
        stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
               << quint32(96000) << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << quint32(2) << qint16(0);
        ASSERT_EQ(stream.status(), QDataStream::Ok);
    }
    auto* binding = findFileBindingForItem(*pm.currentProject(), parent);
    auto* command = ImportAssetsCommand::pushImport({wavPath, wavPath}, binding, parent);
    ASSERT_EQ(command->successCount(), 2);
    ASSERT_EQ(parent->children.size(), 2u);
    const auto firstGuid = parent->children[0]->guid;
    const auto secondGuid = parent->children[1]->guid;
    EXPECT_EQ(parent->children[0]->name, stem);
    EXPECT_EQ(parent->children[1]->name, stem + QStringLiteral("_01"));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_TRUE(parent->children.empty());
    ASSERT_NE(pm.createAudioItem(parent, stem, {}, 0), nullptr);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    ASSERT_NE(pm.findItemByGuid(firstGuid), nullptr);
    ASSERT_NE(pm.findItemByGuid(secondGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(firstGuid)->name, stem);
    EXPECT_EQ(pm.findItemByGuid(secondGuid)->name, stem + QStringLiteral("_01"));
    EXPECT_EQ(command->importedItemGuids(), (QList<QUuid>{firstGuid, secondGuid}));
}
