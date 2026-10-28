#include "ActionManager.h"
#include "Dialogs/Settings/ProjectSettingsPanel.h"
#include "ProjectManager.h"
#include "ProjectPlatformActivity.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QToolButton>

using namespace VNX;

namespace {

void editText(QLineEdit* edit, const QString& text)
{
    edit->setText(text);
    edit->setModified(true);
}

class ProjectSettingsPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &ProjectManager::instance(), nullptr);
        ASSERT_TRUE(tempDir_.isValid());
        createProject(QStringLiteral("SettingsProject"));
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    void createProject(const QString& name)
    {
        ProjectConfig config;
        config.name = name;
        config.location = tempDir_.path();
        config.sampleRate = 48000;
        config.channelConfig = AudioChannelConfig::Stereo_2_0;
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("macOS")});
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        UndoRedoManager::instance().clear();
    }

    QTemporaryDir tempDir_;
};

}

TEST_F(ProjectSettingsPanelTest, CompletedEditsUseProjectHistoryAndExternalChangesRefreshWithoutWritingBack)
{
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* output = panel.findChild<QLineEdit*>("packBuildOutputDirEdit");
    auto* exported = panel.findChild<QCheckBox*>("buildExportProjectDataCheckBox");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(exported, nullptr);
    const QString originalName = ProjectManager::instance().currentProject()->platforms.first().name;

    editText(name, QStringLiteral("Desktop"));
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.first().name, originalName);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    panel.finishEditing();
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.first().name, QStringLiteral("Desktop"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    UndoRedoManager::instance().undo();
    EXPECT_EQ(name->text(), originalName);
    UndoRedoManager::instance().redo();
    EXPECT_EQ(name->text(), QStringLiteral("Desktop"));

    ASSERT_TRUE(ActionManager::instance().execute("set_pack_build_output_dir", {{"path", "Published"}}, ActionSource::IPC).success);
    EXPECT_EQ(output->text(), QStringLiteral("Published"));
    ASSERT_TRUE(ActionManager::instance().execute("set_build_project_data_export", {{"enabled", true}}, ActionSource::IPC).success);
    EXPECT_TRUE(exported->isChecked());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 3);
    UndoRedoManager::instance().undo();
    EXPECT_FALSE(exported->isChecked());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 3);
}

TEST_F(ProjectSettingsPanelTest, ConcurrentChangesCannotBeOverwrittenByUnfinishedFields)
{
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* output = panel.findChild<QLineEdit*>("packBuildOutputDirEdit");
    editText(name, QStringLiteral("Local"));
    editText(output, QStringLiteral("LocalPacks"));
    const auto id = ProjectManager::instance().currentProject()->platforms.first().id;
    ASSERT_TRUE(ActionManager::instance().execute("rename_project_platform",
        {{"platform_id", ProjectPlatformSemantics::idString(id)}, {"name", "External"}}, ActionSource::IPC).success);
    ASSERT_TRUE(ActionManager::instance().execute("set_pack_build_output_dir", {{"path", "ExternalPacks"}}, ActionSource::IPC).success);
    EXPECT_EQ(name->text(), QStringLiteral("Local"));
    EXPECT_EQ(output->text(), QStringLiteral("LocalPacks"));
    panel.finishEditing();
    EXPECT_EQ(name->text(), QStringLiteral("External"));
    EXPECT_EQ(output->text(), QStringLiteral("ExternalPacks"));
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.first().name, QStringLiteral("External"));
    EXPECT_EQ(ProjectManager::instance().packBuildOutputDir(), QStringLiteral("ExternalPacks"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
}

TEST_F(ProjectSettingsPanelTest, InvalidInputIsMarkedAndRestoredWithoutCreatingHistory)
{
    ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* language = panel.findChild<QLineEdit*>("targetLanguageEdit");
    auto* output = panel.findChild<QLineEdit*>("packBuildOutputDirEdit");
    const QString originalName = name->text();
    editText(name, QString());
    editText(language, QStringLiteral("Japanese"));
    editText(output, QStringLiteral("Invalid*Path"));
    EXPECT_TRUE(name->property("inputInvalid").toBool());
    EXPECT_TRUE(language->property("inputInvalid").toBool());
    EXPECT_TRUE(output->property("inputInvalid").toBool());
    panel.finishEditing();
    EXPECT_EQ(name->text(), originalName);
    EXPECT_EQ(language->text(), QStringLiteral("English"));
    EXPECT_TRUE(output->text().isEmpty());
    EXPECT_FALSE(name->property("inputInvalid").toBool());
    EXPECT_FALSE(language->property("inputInvalid").toBool());
    EXPECT_FALSE(output->property("inputInvalid").toBool());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectSettingsPanelTest, SelectionChangesFinishTheOriginalTarget)
{
    ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* platforms = panel.findChild<QListWidget*>("projectPlatformList");
    auto* language = panel.findChild<QLineEdit*>("targetLanguageEdit");
    auto* languages = panel.findChild<QListWidget*>("targetLanguagesList");
    editText(name, QStringLiteral("Desktop"));
    platforms->setCurrentRow(1);
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.first().name, QStringLiteral("Desktop"));
    EXPECT_EQ(name->text(), ProjectManager::instance().currentProject()->platforms.at(1).name);
    EXPECT_EQ(platforms->currentRow(), 1);
    editText(language, QStringLiteral("French"));
    languages->setCurrentRow(1);
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), (QStringList{"French", "Japanese"}));
    EXPECT_EQ(language->text(), QStringLiteral("Japanese"));
    EXPECT_EQ(languages->currentRow(), 1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
}

TEST_F(ProjectSettingsPanelTest, ProjectChangesDiscardUnfinishedInputAndRebindControls)
{
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* output = panel.findChild<QLineEdit*>("packBuildOutputDirEdit");
    editText(name, QStringLiteral("OldProjectDraft"));
    editText(output, QStringLiteral("OldOutput"));
    ASSERT_TRUE(ProjectManager::instance().closeProject());
    createProject(QStringLiteral("OtherProject"));
    panel.finishEditing();
    EXPECT_NE(name->text(), QStringLiteral("OldProjectDraft"));
    EXPECT_TRUE(output->text().isEmpty());
    EXPECT_TRUE(ProjectManager::instance().packBuildOutputDir().isEmpty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectSettingsPanelTest, RemovedSelectionTargetsCannotSurviveAnUnfinishedEdit)
{
    ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* platforms = panel.findChild<QListWidget*>("projectPlatformList");
    auto* language = panel.findChild<QLineEdit*>("targetLanguageEdit");
    auto* languages = panel.findChild<QListWidget*>("targetLanguagesList");
    editText(name, QStringLiteral("Desktop"));
    const auto removedId = ProjectManager::instance().currentProject()->platforms.at(1).id;
    ASSERT_TRUE(ActionManager::instance().execute("delete_project_platform",
        {{"platform_id", ProjectPlatformSemantics::idString(removedId)}}, ActionSource::IPC).success);
    platforms->setCurrentRow(1);
    EXPECT_EQ(platforms->count(), 1);
    EXPECT_EQ(name->text(), ProjectManager::instance().currentProject()->platforms.first().name);
    EXPECT_EQ(platforms->currentRow(), 0);
    editText(language, QStringLiteral("French"));
    ASSERT_TRUE(ActionManager::instance().execute("set_project_languages",
        {{"languages", QJsonArray{QStringLiteral("English")}}}, ActionSource::IPC).success);
    languages->setCurrentRow(1);
    EXPECT_EQ(languages->count(), 1);
    EXPECT_EQ(language->text(), ProjectManager::instance().getProjectLanguages().first());
    EXPECT_EQ(languages->currentRow(), 0);
}

TEST_F(ProjectSettingsPanelTest, PlatformWorkDisablesEditsUntilTheActivityCompletes)
{
    ProjectSettingsPanel panel;
    auto* name = panel.findChild<QLineEdit*>("projectPlatformName");
    auto* add = panel.findChild<QToolButton*>("projectPlatformAdd");
    auto activity = ProjectPlatformActivity::acquire();
    EXPECT_TRUE(name->isReadOnly());
    EXPECT_FALSE(add->isEnabled());
    activity.reset();
    EXPECT_FALSE(name->isReadOnly());
    EXPECT_TRUE(add->isEnabled());
}

TEST_F(ProjectSettingsPanelTest, ExternalDisableDoesNotCommitTheFieldItDisables)
{
    ASSERT_TRUE(ActionManager::instance().execute("set_build_waveform_settings",
        {{"enabled", true}, {"path", "OriginalWaveforms"}}, ActionSource::IPC).success);
    UndoRedoManager::instance().clear();
    ProjectSettingsPanel panel;
    panel.showPage(QStringLiteral("publish"));
    panel.show();
    auto* path = panel.findChild<QLineEdit*>("buildWaveformOutputDirEdit");
    path->setFocus();
    QApplication::processEvents();
    editText(path, QStringLiteral("UnfinishedWaveforms"));
    ASSERT_TRUE(ActionManager::instance().execute("set_build_waveform_settings",
        {{"enabled", false}, {"path", "OriginalWaveforms"}}, ActionSource::IPC).success);
    EXPECT_FALSE(path->isEnabled());
    EXPECT_EQ(path->text(), QStringLiteral("OriginalWaveforms"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    UndoRedoManager::instance().undo();
    EXPECT_TRUE(path->isEnabled());
    EXPECT_EQ(path->text(), QStringLiteral("OriginalWaveforms"));
}
