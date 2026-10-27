#include "ActionManager.h"
#include "Dialogs/Settings/ProjectSettingsDialog.h"
#include "Dialogs/Settings/ProjectPlatformEditor.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include "Support/ProjectPlatformTestUtils.h"

using namespace VNX;

namespace {

QPushButton* applyButton(ProjectSettingsDialog& dialog)
{
    return dialog.findChild<QPushButton*>(QStringLiteral("settingsApplyButton"));
}

QToolButton* addLanguageButton(ProjectSettingsDialog& dialog)
{
    return dialog.findChild<QToolButton*>(QStringLiteral("targetLanguageAddButton"));
}

QListWidget* targetLanguageList(ProjectSettingsDialog& dialog)
{
    return dialog.findChild<QListWidget*>(QStringLiteral("targetLanguagesList"));
}

QStringList targetLanguageValues(ProjectSettingsDialog& dialog)
{
    QStringList values;
    auto* list = targetLanguageList(dialog);
    for (int i = 0; i < list->count(); ++i)
        values.append(list->item(i)->text());
    return values;
}

void enterLanguage(ProjectSettingsDialog& dialog, const QString& value)
{
    QTimer::singleShot(0, &dialog, [value] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        auto* edit = modal->findChild<QLineEdit*>(QStringLiteral("targetLanguageEdit"));
        ASSERT_NE(edit, nullptr);
        edit->setText(value);
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    addLanguageButton(dialog)->click();
}

class ProjectSettingsDialogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(),
                               &ProjectManager::instance(), nullptr);

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("SettingsProject");
        config.location = tempDir_.path();
        config.author = QStringLiteral("Author");
        config.description = QString();
        config.sampleRate = 48000;
        config.channelConfig = AudioChannelConfig::Stereo_2_0;
        config.enableLFE = true;
        config.platforms = HarkenTest::platforms(QStringList{QStringLiteral("Windows"),
                                             QStringLiteral("macOS")});
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    void seedProjectLanguages(const QStringList& value)
    {
        ProjectManager::instance().setProjectLanguagesInternal(value);
        UndoRedoManager::instance().clear();
    }

    QTemporaryDir tempDir_;
};

}

namespace {
void enterPlatform(ProjectSettingsDialog& settings, const QString& name, const QString& base)
{
    QTimer::singleShot(0, [base]() {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        auto* supported = modal->findChild<QListWidget*>("supportedPlatformList");
        ASSERT_NE(supported, nullptr);
        const auto choices = supported->findItems(base, Qt::MatchExactly);
        ASSERT_EQ(choices.size(), 1);
        supported->setCurrentItem(choices.front());
        modal->findChild<QPushButton*>("addProjectPlatformButton")->click();
    });
    settings.findChild<QToolButton*>("projectPlatformAdd")->click();
    auto* editor = settings.findChild<ProjectPlatformEditor*>();
    ASSERT_NE(editor, nullptr);
    editor->findChild<QLineEdit*>("projectPlatformName")->setText(name);
}
}

TEST_F(ProjectSettingsDialogTest, ConcurrentPlatformEditRejectsApplyBeforeOtherSettingsChange)
{
    seedProjectLanguages({QStringLiteral("English")});
    ProjectSettingsDialog dialog;
    dialog.show();
    enterPlatform(dialog, "Draft", "Android");
    enterLanguage(dialog, "Japanese");
    const auto id = ProjectManager::instance().currentProject()->platforms.first().id;
    ASSERT_TRUE(ActionManager::instance().execute("rename_project_platform",
        {{"platform_id", ProjectPlatformSemantics::idString(id)}, {"name", "External"}}, ActionSource::IPC).success);
    QTimer::singleShot(0, []() {
        auto* conflict = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(conflict, nullptr);
        conflict->button(QMessageBox::Cancel)->click();
    });
    applyButton(dialog)->click();
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.size(), 2);
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.first().name, QStringLiteral("External"));
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), QStringList{"English"});
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(dialog.findChild<QListWidget*>("projectPlatformList")->count(), 3);
    EXPECT_EQ(targetLanguageValues(dialog), (QStringList{"English", "Japanese"}));
    EXPECT_TRUE(applyButton(dialog)->isEnabled());
}

TEST_F(ProjectSettingsDialogTest, ConcurrentPlatformEditCanReloadAllBuildDraftsWithoutApplyingThem)
{
    seedProjectLanguages({QStringLiteral("English")});
    ProjectSettingsDialog dialog;
    dialog.show();
    enterPlatform(dialog, "Draft", "Android");
    enterLanguage(dialog, "Japanese");
    dialog.findChild<QLineEdit*>("packBuildOutputDirEdit")->setText("DraftPacks");
    dialog.findChild<QCheckBox*>("buildExportProjectDataCheckBox")->setChecked(true);
    dialog.findChild<QCheckBox*>("buildExportPackMetadataCheckBox")->setChecked(true);
    dialog.findChild<QCheckBox*>("buildExportWaveformsCheckBox")->setChecked(true);
    dialog.findChild<QLineEdit*>("buildWaveformOutputDirEdit")->setText("DraftWaveforms");
    auto& manager = ProjectManager::instance();
    const auto id = manager.currentProject()->platforms.first().id;
    ASSERT_TRUE(ActionManager::instance().execute("rename_project_platform",
        {{"platform_id", ProjectPlatformSemantics::idString(id)}, {"name", "External"}}, ActionSource::IPC).success);
    QTimer::singleShot(0, []() {
        auto* conflict = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(conflict, nullptr);
        for (auto* button : conflict->buttons()) {
            if (conflict->buttonRole(button) != QMessageBox::AcceptRole) continue;
            button->click();
            return;
        }
        conflict->reject();
        FAIL() << "The conflict must offer reloading the build draft";
    });
    applyButton(dialog)->click();
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_FALSE(applyButton(dialog)->isEnabled());
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    ASSERT_EQ(list->count(), 2);
    EXPECT_EQ(list->item(0)->text(), QStringLiteral("External"));
    EXPECT_EQ(targetLanguageValues(dialog), QStringList{"English"});
    EXPECT_TRUE(dialog.findChild<QLineEdit*>("packBuildOutputDirEdit")->text().isEmpty());
    EXPECT_FALSE(dialog.findChild<QCheckBox*>("buildExportProjectDataCheckBox")->isChecked());
    EXPECT_FALSE(dialog.findChild<QCheckBox*>("buildExportPackMetadataCheckBox")->isChecked());
    EXPECT_FALSE(dialog.findChild<QCheckBox*>("buildExportWaveformsCheckBox")->isChecked());
    EXPECT_TRUE(dialog.findChild<QLineEdit*>("buildWaveformOutputDirEdit")->text().isEmpty());
    EXPECT_EQ(manager.getProjectLanguages(), QStringList{"English"});
    EXPECT_TRUE(manager.packBuildOutputDir().isEmpty());
    EXPECT_FALSE(manager.currentProject()->buildExportProjectData);
    EXPECT_FALSE(manager.currentProject()->buildExportPackMetadata);
    EXPECT_FALSE(manager.currentProject()->buildExportWaveforms);
    EXPECT_TRUE(manager.currentProject()->buildWaveformOutputDir.isEmpty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}
