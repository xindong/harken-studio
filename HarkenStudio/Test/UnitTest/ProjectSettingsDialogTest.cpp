#include "ActionManager.h"
#include "AssetPipeline/PathUtils.h"
#include "Core/LanguageContextService.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Dialogs/Settings/Pages/ProjectConfigPage.h"
#include "Dialogs/Settings/Pages/ProjectPublishPage.h"
#include "Dialogs/Settings/ProjectSettingsDialog.h"
#include "Dialogs/Settings/ProjectPlatformDialog.h"
#include "Dialogs/Settings/ProjectPlatformEditor.h"
#include "ProjectManager.h"
#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Publisher/PublisherPanel.h"
#include "Widgets/PlatformBar/PlatformBar.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QTreeWidget>
#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QScrollBar>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <memory>
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

QToolButton* removeLanguageButton(ProjectSettingsDialog& dialog)
{
    return dialog.findChild<QToolButton*>(QStringLiteral("targetLanguageRemoveButton"));
}

QLineEdit* targetLanguageEdit(ProjectSettingsDialog& dialog, int index)
{
    return dialog.findChild<QLineEdit*>(QStringLiteral("targetLanguageEdit_%1").arg(index));
}

QList<QLineEdit*> targetLanguageEdits(ProjectSettingsDialog& dialog)
{
    QList<QLineEdit*> edits;
    for (QLineEdit* edit : dialog.findChildren<QLineEdit*>())
    {
        if (!edit->objectName().startsWith(QStringLiteral("targetLanguageEdit_")))
            continue;
        edits.append(edit);
    }
    std::sort(edits.begin(), edits.end(), [](QLineEdit* lhs, QLineEdit* rhs) {
        return lhs->property("languageIndex").toInt() < rhs->property("languageIndex").toInt();
    });
    return edits;
}

QStringList targetLanguageValues(ProjectSettingsDialog& dialog)
{
    QStringList values;
    for (QLineEdit* edit : targetLanguageEdits(dialog))
        values.append(edit->text());
    return values;
}

void acceptNextModalDialog()
{
    QTimer::singleShot(0, [] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal)
        {
            acceptNextModalDialog();
            return;
        }
        if (auto* dialog = qobject_cast<QDialog*>(modal))
            dialog->accept();
    });
}

void doubleClickCurrentRow(QTreeWidget* table)
{
    ASSERT_NE(table, nullptr);
    ASSERT_NE(table->currentItem(), nullptr);
    table->scrollToItem(table->currentItem());
    table->doItemsLayout();
    auto* viewport = table->viewport();
    const QPoint point = table->visualItemRect(table->currentItem()).center();
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                      QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, point, viewport->mapToGlobal(point), Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(viewport, &event);
    }
}

void clickLanguageEditor(QLineEdit* edit)
{
    QApplication::processEvents();
    const auto point = edit->rect().center();
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        const auto buttons = type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton;
        QMouseEvent event(type, point, edit->mapToGlobal(point), Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(edit, &event);
    }
}

class ProjectSettingsDialogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(),
                               &ProjectManager::instance(), nullptr, {});

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

TEST_F(ProjectSettingsDialogTest, ConfigPageShowsInstanceNamesBaseTypesAndStableIds)
{
    ProjectSettingsDialog dialog;
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    auto* editor = dialog.findChild<ProjectPlatformEditor*>();
    ASSERT_NE(list, nullptr);
    ASSERT_NE(editor, nullptr);
    ASSERT_EQ(list->count(), 2);
    const auto& platforms = ProjectManager::instance().currentProject()->platforms;
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(list->item(i)->text(), platforms.at(i).name);
        EXPECT_EQ(list->item(i)->data(Qt::UserRole).toUuid(), platforms.at(i).id);
        list->setCurrentRow(i);
        EXPECT_EQ(editor->platform().id, platforms.at(i).id);
        EXPECT_EQ(editor->findChild<QComboBox*>("projectPlatformBase")->currentText(),
                  ProjectPlatformSemantics::basePlatformName(platforms.at(i).basePlatform));
    }
    EXPECT_TRUE(list->findChildren<QCheckBox*>().isEmpty());
}

TEST_F(ProjectSettingsDialogTest, LastPlatformCannotBeDeletedFromTheDraft)
{
    ProjectSettingsDialog dialog;
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    auto* remove = dialog.findChild<QToolButton*>("projectPlatformDelete");
    ASSERT_NE(list, nullptr);
    ASSERT_NE(remove, nullptr);
    list->setCurrentItem(list->item(1));
    remove->click();
    EXPECT_EQ(list->count(), 1);
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.size(), 2);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectSettingsDialogTest, PlatformSelectionPreservesDraftsAndDeletionSelectsAdjacentPlatform)
{
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    auto* remove = dialog.findChild<QToolButton*>("projectPlatformDelete");
    auto* editor = dialog.findChild<ProjectPlatformEditor*>();
    auto* page = dialog.findChild<ProjectConfigPage*>();
    ASSERT_NE(list, nullptr);
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(remove, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_EQ(list->currentRow(), 0);
    const auto initialId = editor->platform().id;
    auto* name = editor->findChild<QLineEdit*>("projectPlatformName");
    name->setText("Windows Draft");
    editor->findChild<QCheckBox*>("platformResample")->setChecked(true);
    list->setCurrentRow(1);
    EXPECT_EQ(name->text(), QStringLiteral("macOS"));
    EXPECT_FALSE(editor->findChild<QCheckBox*>("platformResample")->isChecked());
    name->setText("macOS Draft");
    list->setCurrentRow(0);
    EXPECT_EQ(editor->platform().id, initialId);
    EXPECT_EQ(name->text(), QStringLiteral("Windows Draft"));
    EXPECT_TRUE(editor->findChild<QCheckBox*>("platformResample")->isChecked());
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.first().name, QStringLiteral("Windows"));
    remove->click();
    EXPECT_EQ(list->count(), 1);
    EXPECT_EQ(list->currentRow(), 0);
    EXPECT_EQ(name->text(), QStringLiteral("macOS Draft"));
    EXPECT_FALSE(remove->isEnabled());
    const auto selectedId = list->currentItem()->data(Qt::UserRole);
    page->load();
    ASSERT_NE(list->currentItem(), nullptr);
    EXPECT_EQ(list->currentItem()->data(Qt::UserRole), selectedId);
    EXPECT_EQ(name->text(), QStringLiteral("macOS"));
}

TEST_F(ProjectSettingsDialogTest, LanguageFocusDoesNotSelectUntilUserInteractionAndCanBeCleared)
{
    seedProjectLanguages({"English", "Japanese"});
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* edit = targetLanguageEdit(dialog, 0);
    edit->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    EXPECT_FALSE(removeLanguageButton(dialog)->isEnabled());
    clickLanguageEditor(edit);
    EXPECT_TRUE(removeLanguageButton(dialog)->isEnabled());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(edit, &escape);
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_FALSE(removeLanguageButton(dialog)->isEnabled());
    addLanguageButton(dialog)->click();
    EXPECT_TRUE(removeLanguageButton(dialog)->isEnabled());
    EXPECT_TRUE(dialog.findChild<QWidget*>("targetLanguageRow_2")->property("selected").toBool());
}

TEST_F(ProjectSettingsDialogTest, PolicyDeletionSelectsNextRowWhileResetClearsSelection)
{
    auto platform = ProjectManager::instance().currentProject()->platforms.first();
    platform.profile.performResample = true;
    platform.profile.sizePolicies = {{64, {}}, {128, {}}, {std::nullopt, {}}};
    ProjectPlatformEditor dialog;
    dialog.setPlatform(platform);
    dialog.show();
    auto* list = dialog.findChild<QTreeWidget*>("platformSizePolicies");
    auto* remove = dialog.findChild<QToolButton*>("platformPolicyDelete");
    list->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    EXPECT_EQ(list->currentItem(), nullptr);
    EXPECT_FALSE(remove->isEnabled());
    list->setCurrentItem(list->topLevelItem(0));
    EXPECT_TRUE(remove->isEnabled());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(list, &escape);
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_EQ(list->currentItem(), nullptr);
    EXPECT_FALSE(remove->isEnabled());
    list->setCurrentItem(list->topLevelItem(0));
    remove->click();
    QApplication::processEvents();
    EXPECT_EQ(list->topLevelItemCount(), 2);
    EXPECT_EQ(list->currentItem(), list->topLevelItem(0));
    EXPECT_TRUE(remove->isEnabled());
    list->setCurrentItem(list->topLevelItem(0));
    dialog.findChild<QPushButton*>("projectPlatformReset")->click();
    EXPECT_EQ(list->currentItem(), nullptr);
    EXPECT_TRUE(list->selectedItems().isEmpty());
    EXPECT_FALSE(remove->isEnabled());
}

TEST_F(ProjectSettingsDialogTest, LanguageListStartsEmptyWithoutLockedSfxRow)
{
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* add = addLanguageButton(dialog);
    auto* remove = removeLanguageButton(dialog);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);
    EXPECT_EQ(dialog.findChild<QWidget*>(QStringLiteral("targetLanguageLockedRow")), nullptr);
    EXPECT_TRUE(targetLanguageValues(dialog).isEmpty());
    EXPECT_FALSE(remove->isEnabled());

    add->click();
    QApplication::processEvents();
    auto* edit = targetLanguageEdit(dialog, 0);
    ASSERT_NE(edit, nullptr);
    EXPECT_TRUE(edit->hasFocus());
}

TEST_F(ProjectSettingsDialogTest, PlatformDeletionAppliesWithLanguagesAndOneUndo)
{
    const QStringList languages{"Japanese", "English"};
    seedProjectLanguages(languages);
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    ASSERT_NE(list, nullptr);
    list->setCurrentItem(list->item(1));
    dialog.findChild<QToolButton*>("projectPlatformDelete")->click();
    EXPECT_EQ(targetLanguageValues(dialog), languages);
    EXPECT_TRUE(applyButton(dialog)->isEnabled());
    applyButton(dialog)->click();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(ProjectManager::instance().getTargetPlatforms(), HarkenTest::platformIdStrings({"Windows"}));
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), languages);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(ProjectManager::instance().currentProject()->platforms.size(), 2);
}

TEST_F(ProjectSettingsDialogTest, AddingLanguageOnlyDirtiesUntilApply)
{
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* add = addLanguageButton(dialog);
    auto* apply = applyButton(dialog);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(apply, nullptr);

    add->click();
    auto* edit = targetLanguageEdit(dialog, 0);
    ASSERT_NE(edit, nullptr);
    edit->setText(QStringLiteral("Japanese"));
    EXPECT_TRUE(apply->isEnabled());
    EXPECT_TRUE(ProjectManager::instance().getProjectLanguages().isEmpty());

    apply->click();
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(),
              QStringList{QStringLiteral("Japanese")});
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST_F(ProjectSettingsDialogTest, ApplyPersistsLanguagesAndClearsDirty)
{
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* add = addLanguageButton(dialog);
    auto* apply = applyButton(dialog);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(apply, nullptr);

    add->click();
    auto* first = targetLanguageEdit(dialog, 0);
    ASSERT_NE(first, nullptr);
    first->setText(QStringLiteral("English"));
    add->click();
    auto* second = targetLanguageEdit(dialog, 1);
    ASSERT_NE(second, nullptr);
    second->setText(QStringLiteral("Japanese"));
    apply->click();
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(),
              QStringList({QStringLiteral("English"), QStringLiteral("Japanese")}));
    EXPECT_FALSE(dialog.isVisible());

    auto& pm = ProjectManager::instance();
    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(projectPath));

    ProjectSettingsDialog reopen;
    reopen.show();
    EXPECT_EQ(targetLanguageValues(reopen),
              QStringList({QStringLiteral("English"), QStringLiteral("Japanese")}));
    auto* reopenApply = applyButton(reopen);
    ASSERT_NE(reopenApply, nullptr);
    EXPECT_FALSE(reopenApply->isEnabled());
}

TEST_F(ProjectSettingsDialogTest, RenameLanguageInPlaceApplies)
{
    seedProjectLanguages({QStringLiteral("English"), QStringLiteral("Japanese")});

    ProjectSettingsDialog dialog;
    dialog.show();
    auto* apply = applyButton(dialog);
    auto* edit = targetLanguageEdit(dialog, 1);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(edit, nullptr);

    edit->setText(QStringLiteral("Korean"));
    EXPECT_TRUE(apply->isEnabled());
    apply->click();

    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(),
              QStringList({QStringLiteral("English"), QStringLiteral("Korean")}));
    EXPECT_FALSE(dialog.isVisible());
}

TEST_F(ProjectSettingsDialogTest, RemoveSelectedLanguageApplies)
{
    seedProjectLanguages({QStringLiteral("English"), QStringLiteral("Japanese")});

    ProjectSettingsDialog dialog;
    dialog.show();
    auto* apply = applyButton(dialog);
    auto* remove = removeLanguageButton(dialog);
    auto* edit = targetLanguageEdit(dialog, 1);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(remove, nullptr);
    ASSERT_NE(edit, nullptr);
    EXPECT_FALSE(remove->isEnabled());

    clickLanguageEditor(edit);
    EXPECT_TRUE(remove->isEnabled());
    remove->click();
    EXPECT_EQ(targetLanguageValues(dialog), QStringList{QStringLiteral("English")});
    EXPECT_TRUE(apply->isEnabled());

    apply->click();
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(),
              QStringList{QStringLiteral("English")});
    EXPECT_FALSE(dialog.isVisible());
}

TEST_F(ProjectSettingsDialogTest, LastLanguageDeletionAppliesAndCanBeUndone)
{
    const QStringList languages{QStringLiteral("Japanese")};
    seedProjectLanguages(languages);
    LanguageContextService service(std::make_unique<MemorySettingsStorage>());
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));

    ProjectSettingsDialog dialog;
    dialog.show();
    auto* apply = applyButton(dialog);
    auto* remove = removeLanguageButton(dialog);
    auto* edit = targetLanguageEdit(dialog, 0);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(remove, nullptr);
    ASSERT_NE(edit, nullptr);

    clickLanguageEditor(edit);
    EXPECT_TRUE(remove->isEnabled());
    remove->click();
    EXPECT_TRUE(targetLanguageValues(dialog).isEmpty());
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_TRUE(apply->isEnabled());
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), languages);
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    apply->click();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_TRUE(ProjectManager::instance().getProjectLanguages().isEmpty());
    EXPECT_TRUE(service.language().isEmpty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);

    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), languages);
    EXPECT_EQ(service.language(), QStringLiteral("Japanese"));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_TRUE(ProjectManager::instance().getProjectLanguages().isEmpty());
    EXPECT_TRUE(service.language().isEmpty());
}

TEST_F(ProjectSettingsDialogTest, IllegalLanguageCharactersCannotBeTyped)
{
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* add = addLanguageButton(dialog);
    ASSERT_NE(add, nullptr);

    add->click();
    auto* edit = targetLanguageEdit(dialog, 0);
    ASSERT_NE(edit, nullptr);
    edit->insert(QStringLiteral("English"));
    EXPECT_EQ(edit->text(), QStringLiteral("English"));
    edit->insert(QStringLiteral(" "));
    EXPECT_EQ(edit->text(), QStringLiteral("English"));
    edit->insert(QStringLiteral("-"));
    EXPECT_EQ(edit->text(), QStringLiteral("English"));
    edit->insert(QStringLiteral("中"));
    EXPECT_EQ(edit->text(), QStringLiteral("English"));
}

TEST_F(ProjectSettingsDialogTest, DuplicateLanguageRejectedOnApply)
{
    seedProjectLanguages({QStringLiteral("English")});
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* add = addLanguageButton(dialog);
    auto* apply = applyButton(dialog);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(apply, nullptr);

    auto* first = targetLanguageEdit(dialog, 0);
    ASSERT_NE(first, nullptr);
    first->setText(QStringLiteral("English"));
    add->click();
    auto* second = targetLanguageEdit(dialog, 1);
    ASSERT_NE(second, nullptr);
    second->setText(QStringLiteral("english"));
    acceptNextModalDialog();
    apply->click();

    EXPECT_TRUE(dialog.isVisible());
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), QStringList{QStringLiteral("English")});
    EXPECT_EQ(targetLanguageValues(dialog),
              QStringList({QStringLiteral("English"), QStringLiteral("english")}));
    EXPECT_TRUE(apply->isEnabled());
}

TEST_F(ProjectSettingsDialogTest, InvalidConfigurationRejectsApplyBeforeAnySettingsChange)
{
    seedProjectLanguages({QStringLiteral("English")});
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* add = addLanguageButton(dialog);
    auto* apply = applyButton(dialog);
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    ASSERT_NE(add, nullptr);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(list, nullptr);

    add->click();
    auto* edit = targetLanguageEdit(dialog, 1);
    ASSERT_NE(edit, nullptr);
    edit->setText(QStringLiteral("SFX"));
    list->setCurrentItem(list->item(1));
    dialog.findChild<QToolButton*>("projectPlatformDelete")->click();
    dialog.findChild<QLineEdit*>("packBuildOutputDirEdit")->setText("DraftPacks");
    acceptNextModalDialog();
    apply->click();

    EXPECT_TRUE(dialog.isVisible());
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), QStringList{QStringLiteral("English")});
    EXPECT_EQ(ProjectManager::instance().getTargetPlatforms(),
              HarkenTest::platformIdStrings({"Windows", "macOS"}));
    EXPECT_TRUE(ProjectManager::instance().packBuildOutputDir().isEmpty());

    edit->setText(QStringLiteral("German"));
    auto* platformName = dialog.findChild<QLineEdit*>("projectPlatformName");
    ASSERT_NE(platformName, nullptr);
    platformName->clear();
    acceptNextModalDialog();
    apply->click();

    EXPECT_TRUE(dialog.isVisible());
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), QStringList{QStringLiteral("English")});
    EXPECT_EQ(ProjectManager::instance().getTargetPlatforms(),
              HarkenTest::platformIdStrings({"Windows", "macOS"}));
    EXPECT_TRUE(ProjectManager::instance().packBuildOutputDir().isEmpty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectSettingsDialogTest, IllegalCharactersCannotBeTyped)
{
    ProjectSettingsDialog dialog;
    ASSERT_TRUE(dialog.showPage(QString::fromLatin1(kSettingsPagePublish)));
    dialog.show();
    auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
    ASSERT_NE(edit, nullptr);

    edit->setText(QStringLiteral("Assets/Audio"));
    edit->insert(QStringLiteral("*"));
    EXPECT_EQ(edit->text(), QStringLiteral("Assets/Audio"));
    EXPECT_FALSE(edit->text().contains(QLatin1Char('*')));
    EXPECT_FALSE(edit->text().contains(QLatin1Char('?')));
    EXPECT_FALSE(edit->text().contains(QLatin1Char('<')));
    EXPECT_FALSE(edit->text().contains(QLatin1Char('>')));
    EXPECT_FALSE(edit->text().contains(QLatin1Char('|')));
}

TEST_F(ProjectSettingsDialogTest, PublishSettingsRoundTripAndUndoPreserveDefaultPaths)
{
    ProjectSettingsDialog dialog;
    ASSERT_TRUE(dialog.showPage(QString::fromLatin1(kSettingsPagePublish)));
    dialog.show();
    auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
    auto* exportProjectData = dialog.findChild<QCheckBox*>(QStringLiteral("buildExportProjectDataCheckBox"));
    auto* exportPackMetadata = dialog.findChild<QCheckBox*>(QStringLiteral("buildExportPackMetadataCheckBox"));
    auto* apply = applyButton(dialog);
    ASSERT_NE(edit, nullptr);
    ASSERT_NE(exportProjectData, nullptr);
    ASSERT_NE(exportPackMetadata, nullptr);
    ASSERT_NE(apply, nullptr);

    EXPECT_FALSE(exportProjectData->isChecked());
    exportProjectData->setChecked(true);
    EXPECT_FALSE(ProjectManager::instance().currentProject()->buildExportProjectData);
    EXPECT_FALSE(exportPackMetadata->isChecked());
    exportPackMetadata->setChecked(true);
    EXPECT_FALSE(ProjectManager::instance().currentProject()->buildExportPackMetadata);
    edit->setText(QStringLiteral("  Audio\\Packs//  "));
    edit->editingFinished();
    EXPECT_EQ(edit->text(), QStringLiteral("Audio/Packs"));
    EXPECT_TRUE(apply->isEnabled());
    apply->click();
    EXPECT_EQ(ProjectManager::instance().packBuildOutputDir(),
              QStringLiteral("Audio/Packs"));
    EXPECT_TRUE(ProjectManager::instance().currentProject()->buildExportProjectData);
    EXPECT_TRUE(ProjectManager::instance().currentProject()->buildExportPackMetadata);
    EXPECT_FALSE(dialog.isVisible());

    auto& pm = ProjectManager::instance();
    const QString projectPath = pm.currentProject()->projectPath;
    const QDir projectRoot(QFileInfo(projectPath).absolutePath());
    EXPECT_EQ(PathUtils::packBuildOutputRoot(projectPath, pm.packBuildOutputDir()),
              projectRoot.filePath(QStringLiteral("Audio/Packs")));
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(pm.currentProject()->buildExportPackMetadata);
    EXPECT_TRUE(pm.currentProject()->buildExportProjectData);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(pm.currentProject()->buildExportProjectData);
    EXPECT_EQ(pm.packBuildOutputDir(), QStringLiteral("Audio/Packs"));
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(PathUtils::packBuildOutputRoot(projectPath, pm.packBuildOutputDir()),
              projectRoot.filePath(QStringLiteral("BuildPacks")));
    UndoRedoManager::instance().undoStack()->redo();
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_TRUE(pm.currentProject()->buildExportProjectData);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_TRUE(pm.currentProject()->buildExportPackMetadata);
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.saveProject(projectPath, *pm.currentProject()));
    ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(projectPath, loaded)) << serializer.lastError().toStdString();
    EXPECT_EQ(loaded.packBuildOutputDir, QStringLiteral("Audio/Packs"));
    EXPECT_TRUE(loaded.buildExportProjectData);
    EXPECT_TRUE(loaded.buildExportPackMetadata);
    EXPECT_EQ(PathUtils::packBuildOutputRoot(loaded.projectPath, loaded.packBuildOutputDir),
              projectRoot.filePath(QStringLiteral("Audio/Packs")));

    ProjectSettingsDialog reopen;
    ASSERT_TRUE(reopen.showPage(QString::fromLatin1(kSettingsPagePublish)));
    reopen.show();
    EXPECT_TRUE(reopen.findChild<QCheckBox*>(QStringLiteral("buildExportProjectDataCheckBox"))->isChecked());
    EXPECT_TRUE(reopen.findChild<QCheckBox*>(QStringLiteral("buildExportPackMetadataCheckBox"))->isChecked());
    auto* emptyEdit = reopen.findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
    auto* reopenApply = applyButton(reopen);
    ASSERT_NE(emptyEdit, nullptr);
    ASSERT_NE(reopenApply, nullptr);
    emptyEdit->setText(QString());
    emptyEdit->editingFinished();
    EXPECT_TRUE(emptyEdit->text().isEmpty());
    EXPECT_TRUE(reopenApply->isEnabled());
    reopenApply->click();
    EXPECT_TRUE(ProjectManager::instance().packBuildOutputDir().isEmpty());
    EXPECT_FALSE(reopen.isVisible());
    EXPECT_EQ(PathUtils::packBuildOutputRoot(projectPath, pm.packBuildOutputDir()),
              projectRoot.filePath(QStringLiteral("BuildPacks")));

    ProjectSettingsDialog prefixDialog;
    ASSERT_TRUE(prefixDialog.showPage(QString::fromLatin1(kSettingsPagePublish)));
    prefixDialog.show();
    auto* prefixEdit = prefixDialog.findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
    ASSERT_NE(prefixEdit, nullptr);
    prefixEdit->setText(QStringLiteral("Assets/Audio"));
    prefixEdit->editingFinished();
    EXPECT_EQ(prefixEdit->text(), QStringLiteral("Assets/Audio"));
}

TEST_F(ProjectSettingsDialogTest, ProjectRootAndObjectBucketPackBuildDirectoriesCannotBeApplied)
{
    auto& manager = ProjectManager::instance();
    const QString projectRoot = QFileInfo(manager.currentProject()->projectPath).absolutePath();
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    for (const auto& path : {QStringLiteral("."), QStringLiteral("../") + QDir(projectRoot).dirName(),
                            QStringLiteral("Actions"), QStringLiteral("Layers/BuildPacks"),
                            QStringLiteral("Busses/Packs"), QStringLiteral("Variables/Packs"),
                            QStringLiteral("Attenuations/Packs")}) {
        ProjectSettingsDialog dialog;
        ASSERT_TRUE(dialog.showPage(QString::fromLatin1(kSettingsPagePublish)));
        dialog.show();
        auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
        ASSERT_NE(edit, nullptr);
        edit->setText(path);
        acceptNextModalDialog();
        applyButton(dialog)->click();
        EXPECT_TRUE(dialog.isVisible());
        EXPECT_TRUE(manager.packBuildOutputDir().isEmpty());
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
    }
}

TEST_F(ProjectSettingsDialogTest, AbsolutePackBuildDirectoryCannotBeApplied)
{
    seedProjectLanguages({QStringLiteral("English")});
    ProjectSettingsDialog dialog;
    ASSERT_TRUE(dialog.showPage(QString::fromLatin1(kSettingsPagePublish)));
    dialog.show();
    addLanguageButton(dialog)->click();
    targetLanguageEdit(dialog, 1)->setText("Japanese");
    auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("packBuildOutputDirEdit"));
    ASSERT_NE(edit, nullptr);
    edit->setText(tempDir_.filePath(QStringLiteral("Packs")));
    acceptNextModalDialog();
    applyButton(dialog)->click();
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_TRUE(ProjectManager::instance().packBuildOutputDir().isEmpty());
    EXPECT_EQ(ProjectManager::instance().getProjectLanguages(), QStringList{"English"});
}

namespace {
void enterPlatform(ProjectSettingsDialog& settings, const QString& name, const QString& base,
                   double rate = 0, bool create = true)
{
    if (create) {
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
    } else {
        settings.findChild<QListWidget*>("projectPlatformList")->setCurrentRow(0);
    }
    auto* editor = settings.findChild<ProjectPlatformEditor*>();
    ASSERT_NE(editor, nullptr);
    editor->findChild<QLineEdit*>("projectPlatformName")->setText(name);
    auto* baseChoice = editor->findChild<QComboBox*>("projectPlatformBase");
    if (baseChoice->currentText() != base) {
        QTimer::singleShot(0, []() {
            auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            ASSERT_NE(confirmation, nullptr);
            confirmation->button(QMessageBox::Yes)->click();
        });
        baseChoice->setCurrentText(base);
    }
    if (rate > 0) {
        editor->findChild<QCheckBox*>("platformResample")->setChecked(true);
        auto* policies = editor->findChild<QTreeWidget*>("platformSizePolicies");
        policies->setCurrentItem(policies->topLevelItem(0));
        QTimer::singleShot(0, [rate]() {
            auto* range = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            ASSERT_NE(range, nullptr);
            EXPECT_EQ(range->findChild<QCheckBox*>("platformResample"), nullptr);
            range->findChild<QDoubleSpinBox*>("platformSampleRate")->setValue(rate);
            range->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        doubleClickCurrentRow(policies);
    }
}
}

TEST_F(ProjectSettingsDialogTest, TwoAndroidDraftsApplyTogetherAndPersistTheirProfiles)
{
    auto& manager = ProjectManager::instance();
    manager.closeProject();
    ProjectConfig config;
    config.name = "SinglePlatform";
    config.location = tempDir_.path();
    config.platforms = HarkenTest::platforms({"Windows"});
    ASSERT_TRUE(manager.createProject(config));
    PlatformBar bar(PlatformContextService::instance());
    PublisherPanel packs;
    ProjectSettingsDialog dialog;
    dialog.show();
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    ASSERT_EQ(list->count(), 1);
    EXPECT_EQ(list->item(0)->text(), QStringLiteral("Windows"));
    EXPECT_FALSE(dialog.findChild<QToolButton*>("projectPlatformDelete")->isEnabled());
    EXPECT_FALSE(bar.findChild<QToolButton*>("platformBarAll")->isHidden());
    enterPlatform(dialog, "Android_Low", "Android", 22050);
    enterPlatform(dialog, "Android_High", "Android", 44100);
    EXPECT_EQ(manager.currentProject()->platforms.size(), 1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    applyButton(dialog)->click();
    const auto* project = manager.currentProject();
    ASSERT_EQ(project->platforms.size(), 3);
    const auto low = project->platforms[1];
    const auto high = project->platforms[2];
    EXPECT_NE(low.id, high.id);
    EXPECT_EQ(low.basePlatform, BasePlatform::Android);
    EXPECT_EQ(high.basePlatform, BasePlatform::Android);
    EXPECT_EQ(low.profile.sizePolicies.front().config.targetSampleRate, 22050u);
    EXPECT_EQ(high.profile.sizePolicies.front().config.targetSampleRate, 44100u);
    AssetInfo asset;
    asset.metadata.fileSize = 1024;
    EXPECT_EQ(PlatformProfileManager::effectiveSettingsForAsset(asset, low).targetSampleRate, 22050u);
    EXPECT_EQ(PlatformProfileManager::effectiveSettingsForAsset(asset, high).targetSampleRate, 44100u);
    auto* platforms = bar.findChild<QComboBox*>("platformBarSelector");
    ASSERT_NE(platforms, nullptr);
    ASSERT_EQ(platforms->count(), 3);
    EXPECT_EQ(platforms->itemText(1), low.name);
    EXPECT_EQ(platforms->itemText(2), high.name);
    EXPECT_EQ(platforms->itemData(1).toUuid(), low.id);
    EXPECT_EQ(platforms->itemData(2).toUuid(), high.id);
    EXPECT_EQ(packs.generationPlatforms(), project->platformIds());
    auto* exportPlatforms = packs.findChild<QComboBox*>("packPlatformCombo");
    ASSERT_NE(exportPlatforms, nullptr);
    EXPECT_EQ(exportPlatforms->itemText(exportPlatforms->findData(
        ProjectPlatformSemantics::idString(high.id))), high.name);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    const auto expected = ProjectPlatformSemantics::platformsToJson(project->platforms);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(project->platforms.size(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(project->platforms), expected);
    const auto path = project->projectPath;
    ASSERT_TRUE(ProjectManager::instance().saveProject());
    ProjectManager::instance().closeProject();
    ASSERT_TRUE(ProjectManager::instance().loadProject(path));
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(ProjectManager::instance().currentProject()->platforms), expected);
}

TEST_F(ProjectSettingsDialogTest, DisablingResampleAppliesWithoutLosingRatesAndSurvivesReopen)
{
    auto& manager = ProjectManager::instance();
    {
        ProjectSettingsDialog initial;
        initial.show();
        enterPlatform(initial, "Configured", "Windows", 32000, false);
        applyButton(initial)->click();
    }
    const auto before = ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms);
    ASSERT_TRUE(manager.currentProject()->platforms.first().profile.performResample);
    UndoRedoManager::instance().clear();
    ProjectSettingsDialog settings;
    settings.show();
    auto* list = settings.findChild<QListWidget*>("projectPlatformList");
    list->setCurrentItem(list->item(0));
    settings.findChild<QCheckBox*>("platformResample")->setChecked(false);
    EXPECT_TRUE(manager.currentProject()->platforms.first().profile.performResample);
    applyButton(settings)->click();
    const auto after = ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms);
    EXPECT_FALSE(manager.currentProject()->platforms.first().profile.performResample);
    EXPECT_EQ(manager.currentProject()->platforms.first().profile.sizePolicies.front().config.targetSampleRate, 32000u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), before);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), after);
    const auto path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();
    ASSERT_TRUE(manager.loadProject(path));
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), after);
}

TEST_F(ProjectSettingsDialogTest, CancelDiscardsPlatformCreationRenameAndDeletionWithoutDirtyingProject)
{
    const auto expected = ProjectPlatformSemantics::platformsToJson(ProjectManager::instance().currentProject()->platforms);
    ProjectSettingsDialog dialog;
    dialog.show();
    enterPlatform(dialog, "Draft", "Android");
    auto* list = dialog.findChild<QListWidget*>("projectPlatformList");
    list->setCurrentItem(list->item(0));
    enterPlatform(dialog, "Renamed", "Windows", 0, false);
    list->setCurrentItem(list->item(1));
    dialog.findChild<QToolButton*>("projectPlatformDelete")->click();
    dialog.findChild<QPushButton*>("settingsCancelButton")->click();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(ProjectManager::instance().currentProject()->platforms), expected);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_TRUE(UndoRedoManager::instance().isClean());
}

TEST_F(ProjectSettingsDialogTest, ConcurrentPlatformEditRejectsApplyBeforeOtherSettingsChange)
{
    seedProjectLanguages({QStringLiteral("English")});
    ProjectSettingsDialog dialog;
    dialog.show();
    enterPlatform(dialog, "Draft", "Android");
    addLanguageButton(dialog)->click();
    targetLanguageEdit(dialog, 1)->setText("Japanese");
    const auto id = ProjectManager::instance().currentProject()->platforms.first().id;
    ASSERT_TRUE(ActionManager::instance().execute("rename_project_platform",
        {{"platform_id", ProjectPlatformSemantics::idString(id)}, {"name", "External"}}, ActionSource::IPC).success);
    QTimer::singleShot(0, []() {
        auto* conflict = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(conflict, nullptr);
        EXPECT_EQ(conflict->defaultButton(), conflict->button(QMessageBox::Cancel));
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
    EXPECT_EQ(dialog.findChild<QAbstractButton*>("projectPlatformsReload"), nullptr);
    enterPlatform(dialog, "Draft", "Android");
    addLanguageButton(dialog)->click();
    targetLanguageEdit(dialog, 1)->setText("Japanese");
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
    list->setCurrentItem(list->item(0));
    enterPlatform(dialog, "After Reload", "Windows", 0, false);
    applyButton(dialog)->click();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(manager.currentProject()->platforms.first().name, QStringLiteral("After Reload"));
    EXPECT_EQ(manager.currentProject()->platforms.size(), 2);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
}

TEST_F(ProjectSettingsDialogTest, ProfileSizePolicyEditingPreservesCodecFieldsAndResetCopiesBaseDefaults)
{
    auto definition = ProjectManager::instance().currentProject()->platforms.first();
    ProjectPlatformProfile profile;
    profile.performResample = true;
    profile.sizePolicies.front().config.format = ConversionSettings::Codec::Vorbis;
    profile.sizePolicies.front().config.vorbisQuality = 0.4f;
    profile.sizePolicies.front().maxBytes = 256;
    SizePolicy infinite{std::nullopt, profile.sizePolicies.front().config};
    infinite.config.format = ConversionSettings::Codec::HarkenPackOpus;
    profile.sizePolicies.push_back(infinite);
    definition.profile = profile;
    ProjectPlatformEditor editor;
    editor.setPlatform(definition);
    editor.show();
    auto* policies = editor.findChild<QTreeWidget*>("platformSizePolicies");
    policies->setCurrentItem(policies->topLevelItem(0));
    QTimer::singleShot(0, []() {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        EXPECT_EQ(modal->findChild<QCheckBox*>(), nullptr);
        modal->findChild<QLineEdit*>("platformPolicyLimit")->setText("128");
        modal->findChild<QDoubleSpinBox*>("platformSampleRate")->setValue(32000);
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    doubleClickCurrentRow(editor.findChild<QTreeWidget*>("platformSizePolicies"));
    const auto changed = editor.platform();
    EXPECT_TRUE(changed.profile.performResample);
    ASSERT_EQ(changed.profile.sizePolicies.size(), 2u);
    EXPECT_EQ(changed.profile.sizePolicies.front().config.format, ConversionSettings::Codec::Vorbis);
    EXPECT_FLOAT_EQ(changed.profile.sizePolicies.front().config.vorbisQuality, 0.4f);
    EXPECT_EQ(changed.profile.sizePolicies.back().toJson(), infinite.toJson());
    EXPECT_EQ(changed.profile.sizePolicies.front().maxBytes, 128u);
    AssetInfo source;
    source.metadata.fileSize = 128;
    EXPECT_EQ(PlatformProfileManager::effectiveSettingsForAsset(source, changed).targetSampleRate, 32000u);
    ProjectPlatformEditor reset;
    reset.setPlatform(changed);
    reset.findChild<QPushButton*>("projectPlatformReset")->click();
    EXPECT_EQ(reset.platform().profile.toJson(), ProjectPlatformProfile::fromBasePlatform(definition.basePlatform).toJson());
    EXPECT_EQ(reset.platform().id, definition.id);
}

TEST_F(ProjectSettingsDialogTest, UpperLimitsSortBySizeAndTheMasterSwitchPreservesTheirValues)
{
    const auto definition = ProjectManager::instance().currentProject()->platforms.first();
    ProjectPlatformEditor editor;
    editor.setPlatform(definition);
    editor.show();
    auto* list = editor.findChild<QTreeWidget*>("platformSizePolicies");
    auto* enabled = editor.findChild<QCheckBox*>("platformResample");
    ASSERT_NE(list, nullptr);
    ASSERT_NE(enabled, nullptr);
    EXPECT_FALSE(enabled->isChecked());
    EXPECT_FALSE(list->isEnabled());
    ASSERT_EQ(list->topLevelItemCount(), 1);
    EXPECT_EQ(list->topLevelItem(0)->text(0), QStringLiteral("< Infinite"));
    EXPECT_EQ(list->topLevelItem(0)->text(1), QStringLiteral("48000 Hz"));
    for (const auto* name : {"platformPolicyAdd", "platformPolicyDelete"})
        EXPECT_FALSE(editor.findChild<QToolButton*>(name)->isEnabled());
    enabled->setChecked(true);
    EXPECT_TRUE(list->isEnabled());
    EXPECT_FALSE(editor.findChild<QToolButton*>("platformPolicyDelete")->isEnabled());
    const auto add = [&](const QString& value, const QString& unit, int rate) {
        QTimer::singleShot(0, [=]() {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            ASSERT_NE(modal, nullptr);
            EXPECT_EQ(modal->findChild<QCheckBox*>(), nullptr);
            modal->findChild<QComboBox*>("platformPolicyUnit")->setCurrentText(unit);
            modal->findChild<QLineEdit*>("platformPolicyLimit")->setText(value);
            modal->findChild<QDoubleSpinBox*>("platformSampleRate")->setValue(rate);
            modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        editor.findChild<QToolButton*>("platformPolicyAdd")->click();
    };
    add("12", "MB", 44100);
    add("64", "B", 22050);
    ASSERT_EQ(list->topLevelItemCount(), 3);
    list->setCurrentItem(list->topLevelItem(1));
    QTimer::singleShot(0, []() {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        auto* value = modal->findChild<QLineEdit*>("platformPolicyLimit");
        auto* unit = modal->findChild<QComboBox*>("platformPolicyUnit");
        EXPECT_EQ(value->text(), QStringLiteral("12"));
        unit->setCurrentText("KB");
        EXPECT_EQ(value->text(), QStringLiteral("12288"));
        unit->setCurrentText("B");
        EXPECT_EQ(value->text(), QStringLiteral("12582912"));
        unit->setCurrentText("GB");
        EXPECT_EQ(value->text(), QStringLiteral("0.01171875"));
        unit->setCurrentText("MB");
        EXPECT_EQ(value->text(), QStringLiteral("12"));
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    doubleClickCurrentRow(editor.findChild<QTreeWidget*>("platformSizePolicies"));
    enabled->setChecked(false);
    EXPECT_FALSE(list->isEnabled());
    QCoreApplication::processEvents();
    EXPECT_EQ(list->horizontalScrollBar()->maximum(), 0);
    for (const auto* widget : {static_cast<QWidget*>(list), static_cast<QWidget*>(enabled),
             static_cast<QWidget*>(editor.findChild<QPushButton*>("projectPlatformReset"))}) {
        const QRect bounds(widget->mapTo(&editor, QPoint(0, 0)), widget->size());
        EXPECT_TRUE(editor.rect().contains(bounds));
    }
    const auto result = editor.platform();
    EXPECT_FALSE(result.profile.performResample);
    ASSERT_EQ(result.profile.sizePolicies.size(), 3u);
    EXPECT_EQ(result.profile.sizePolicies.front().maxBytes, 64u);
    EXPECT_EQ(result.profile.sizePolicies[1].maxBytes, 12u * 1024 * 1024);
    EXPECT_EQ(result.profile.sizePolicies[1].config.targetSampleRate, 44100u);
    EXPECT_FALSE(result.profile.sizePolicies.back().maxBytes);
    ProjectPlatformEditor reopened;
    reopened.setPlatform(result);
    reopened.findChild<QCheckBox*>("platformResample")->setChecked(true);
    EXPECT_TRUE(reopened.findChild<QTreeWidget*>("platformSizePolicies")->isEnabled());
    reopened.findChild<QPushButton*>("projectPlatformReset")->click();
    EXPECT_EQ(reopened.platform().profile.toJson(),
              ProjectPlatformProfile::fromBasePlatform(result.basePlatform).toJson());
    EXPECT_EQ(reopened.platform().id, result.id);
    EXPECT_EQ(editor.platform().toJson(), result.toJson());
}

TEST_F(ProjectSettingsDialogTest, SupportedPlatformSelectionPreviewsDefaultsWithoutChangingProject)
{
    const auto before = ProjectPlatformSemantics::platformsToJson(ProjectManager::instance().currentProject()->platforms);
    ProjectPlatformDialog dialog;
    dialog.show();
    auto* list = dialog.findChild<QListWidget*>("supportedPlatformList");
    auto* preview = dialog.findChild<ProjectPlatformEditor*>();
    ASSERT_NE(list, nullptr);
    ASSERT_NE(preview, nullptr);
    const QStringList names{"Windows", "Android", "Linux", "macOS", "iOS"};
    ASSERT_EQ(list->count(), names.size());
    for (int i = 0; i < list->count(); ++i) {
        EXPECT_EQ(list->item(i)->text(), names[i]);
        list->setCurrentRow(i);
        const auto platform = preview->platform();
        EXPECT_EQ(ProjectPlatformSemantics::basePlatformName(platform.basePlatform), names[i]);
        EXPECT_EQ(platform.profile.toJson(), ProjectPlatformProfile::fromBasePlatform(platform.basePlatform).toJson());
        auto* name = preview->findChild<QLineEdit*>("projectPlatformName");
        EXPECT_TRUE(name->isReadOnly() || !name->isEnabled());
        EXPECT_FALSE(preview->findChild<QComboBox*>("projectPlatformBase")->isEnabled());
        EXPECT_FALSE(preview->findChild<QCheckBox*>("platformResample")->isEnabled());
        EXPECT_FALSE(preview->findChild<QToolButton*>("platformPolicyAdd")->isEnabled());
        EXPECT_FALSE(preview->findChild<QToolButton*>("platformPolicyDelete")->isEnabled());
        EXPECT_FALSE(preview->findChild<QPushButton*>("projectPlatformReset")->isVisible());
    }
    dialog.findChild<QPushButton*>("addProjectPlatformButton")->click();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(dialog.platform().basePlatform, BasePlatform::iOS);
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(ProjectManager::instance().currentProject()->platforms), before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectSettingsDialogTest, SizeLimitEditorRejectsEquivalentUnitsAndProtectsInfinite)
{
    auto definition = ProjectManager::instance().currentProject()->platforms.first();
    definition.profile.performResample = true;
    definition.profile.sizePolicies = {{64, {}}, {128, {}}, {std::nullopt, {}}};
    ProjectPlatformEditor editor;
    editor.setPlatform(definition);
    editor.show();
    auto* list = editor.findChild<QTreeWidget*>("platformSizePolicies");
    QTimer::singleShot(0, []() {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        auto* value = modal->findChild<QLineEdit*>("platformPolicyLimit");
        auto* unit = modal->findChild<QComboBox*>("platformPolicyUnit");
        auto* ok = modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        unit->setCurrentText("KB");
        value->setText("0.0625");
        ok->click();
        EXPECT_TRUE(modal->isVisible());
        unit->setCurrentText("B");
        value->setText("0.1");
        ok->click();
        EXPECT_TRUE(modal->isVisible());
        unit->setCurrentText("GB");
        value->setText("8589934592");
        ok->click();
        EXPECT_TRUE(modal->isVisible());
        unit->setCurrentText("B");
        value->setText("9223372036854775807");
        unit->setCurrentText("GB");
        unit->setCurrentText("B");
        EXPECT_EQ(value->text(), QStringLiteral("9223372036854775807"));
        ok->click();
    });
    editor.findChild<QToolButton*>("platformPolicyAdd")->click();
    ASSERT_EQ(list->topLevelItemCount(), 4);
    list->setCurrentItem(list->topLevelItem(1));
    QTimer::singleShot(0, []() {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        modal->findChild<QLineEdit*>("platformPolicyLimit")->setText("32");
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    doubleClickCurrentRow(editor.findChild<QTreeWidget*>("platformSizePolicies"));
    EXPECT_EQ(list->topLevelItem(0)->text(0), QStringLiteral("\u2264 32 B"));
    editor.findChild<QToolButton*>("platformPolicyDelete")->click();
    ASSERT_EQ(list->topLevelItemCount(), 3);
    list->setCurrentItem(list->topLevelItem(2));
    EXPECT_FALSE(editor.findChild<QToolButton*>("platformPolicyDelete")->isEnabled());
    QTimer::singleShot(0, []() {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(modal, nullptr);
        EXPECT_EQ(modal->findChild<QLineEdit*>("platformPolicyLimit"), nullptr);
        EXPECT_EQ(modal->findChild<QComboBox*>("platformPolicyUnit"), nullptr);
        EXPECT_NE(modal->findChild<QLabel*>("platformPolicyInfinite"), nullptr);
        modal->findChild<QDoubleSpinBox*>("platformSampleRate")->setValue(32000);
        modal->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    doubleClickCurrentRow(editor.findChild<QTreeWidget*>("platformSizePolicies"));
    const auto result = editor.platform();
    EXPECT_EQ(result.profile.sizePolicies.back().config.targetSampleRate, 32000u);
    EXPECT_TRUE(ProjectPlatformSemantics::validatePlatforms({result}));
}

TEST_F(ProjectSettingsDialogTest, ExistingBaseChangeRequiresConfirmationAndReplacesAllProcessingSettings)
{
    auto definition = ProjectManager::instance().currentProject()->platforms.first();
    definition.profile.performResample = true;
    definition.profile.sizePolicies.front().config.targetSampleRate = 32000;
    ProjectPlatformEditor dialog;
    dialog.setPlatform(definition);
    dialog.show();
    EXPECT_EQ(dialog.findChild<QCheckBox*>("projectPlatformCustomProfile"), nullptr);
    EXPECT_EQ(dialog.findChild<QCheckBox*>("platformNormalize"), nullptr);
    EXPECT_EQ(dialog.findChild<QDoubleSpinBox*>("platformNormalizeTarget"), nullptr);
    EXPECT_EQ(dialog.findChild<QCheckBox*>("platformForceMono"), nullptr);
    EXPECT_EQ(dialog.findChild<QCheckBox*>("platformTrimSilence"), nullptr);
    EXPECT_EQ(dialog.findChild<QDoubleSpinBox*>("platformTrimThreshold"), nullptr);
    ASSERT_NE(dialog.findChild<QCheckBox*>("platformResample"), nullptr);
    EXPECT_TRUE(dialog.findChild<QCheckBox*>("platformResample")->isChecked());
    EXPECT_EQ(dialog.findChild<QDoubleSpinBox*>("platformSampleRate"), nullptr);
    auto* policies = dialog.findChild<QTreeWidget*>("platformSizePolicies");
    auto* base = dialog.findChild<QComboBox*>("projectPlatformBase");
    ASSERT_NE(base, nullptr);
    EXPECT_TRUE(base->isVisible());
    QTimer::singleShot(0, []() {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(confirmation, nullptr);
        confirmation->button(QMessageBox::Cancel)->click();
    });
    base->setCurrentText("Android");
    EXPECT_EQ(base->currentText(), QStringLiteral("Windows"));
    EXPECT_EQ(policies->topLevelItem(0)->text(1), QStringLiteral("32000 Hz"));
    EXPECT_EQ(policies->topLevelItemCount(), 1);
    QTimer::singleShot(0, []() {
        auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(confirmation, nullptr);
        confirmation->button(QMessageBox::Yes)->click();
    });
    base->setCurrentText("Android");
    EXPECT_EQ(policies->topLevelItem(0)->text(1), QStringLiteral("48000 Hz"));
    EXPECT_FALSE(dialog.findChild<QCheckBox*>("platformResample")->isChecked());
    EXPECT_EQ(policies->topLevelItemCount(), 1);
    EXPECT_EQ(dialog.platform().basePlatform, BasePlatform::Android);
    EXPECT_EQ(dialog.platform().profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
    EXPECT_EQ(dialog.platform().id, definition.id);
    EXPECT_EQ(dialog.platform().name, definition.name);
}

TEST_F(ProjectSettingsDialogTest, ExistingBaseChangeStaysInDraftUntilApplyAndHasOneUndo)
{
    auto& manager = ProjectManager::instance();
    const auto before = ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms);
    {
        ProjectSettingsDialog cancelled;
        cancelled.show();
        enterPlatform(cancelled, "Windows", "Android", 0, false);
        EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), before);
        QTimer::singleShot(0, []() {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            for (auto* button : box->buttons())
                if (box->buttonRole(button) == QMessageBox::DestructiveRole
                    || box->buttonRole(button) == QMessageBox::AcceptRole) { button->click(); break; }
        });
        cancelled.findChild<QPushButton*>("settingsCancelButton")->click();
        EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), before);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    }
    ProjectSettingsDialog applied;
    applied.show();
    enterPlatform(applied, "Windows", "Android", 0, false);
    applyButton(applied)->click();
    const auto after = ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms);
    EXPECT_EQ(manager.currentProject()->platforms.first().basePlatform, BasePlatform::Android);
    EXPECT_EQ(after.first().toObject().value("id"), before.first().toObject().value("id"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), before);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(ProjectPlatformSemantics::platformsToJson(manager.currentProject()->platforms), after);
}
