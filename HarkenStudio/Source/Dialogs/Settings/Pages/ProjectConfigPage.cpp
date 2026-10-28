#include "Dialogs/Settings/Pages/ProjectConfigPage.h"

#include "Core/ActionManager.h"
#include "Core/BaseDialog.h"
#include "Core/ContextInfo.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectPlatformActivity.h"
#include "Core/QtControlSkin.h"

#include "Dialogs/Settings/ProjectPlatformDialog.h"
#include "Dialogs/Settings/ProjectPlatformEditor.h"
#include "Widgets/Common/DialogFormLayout.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include <QDialogButtonBox>
#include <QListWidget>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QScopedValueRollback>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

ProjectConfigPage::ProjectConfigPage(QWidget* parent)
    : SettingsPage(parent)
{
    auto* body = makePageBody();

    auto* platformsGroup = makeSection(tr("Platform Configuration"));
    ContextInfo::tag(platformsGroup, ContextTopic::forPanel(QStringLiteral("settings.config.platforms")));
    auto* platformsLayout = new QHBoxLayout();
    platformsLayout->setContentsMargins(0, 0, 0, 0);
    platformsLayout->setSpacing(0);
    auto* platformTabs = new TableWidget(
        {TableKind::Config, TableBodySizing::Expanding}, platformsGroup);
    platformTabs->setFixedWidth(150);
    platformTabs->setMinimumHeight(200);
    platformList_ = platformTabs->createNavigationView(QStringLiteral("projectPlatformList"));
    auto* platformFooter = platformTabs->footer();
    platformAddButton_ = platformFooter->addButton();
    platformAddButton_->setObjectName(QStringLiteral("projectPlatformAdd"));
    platformRemoveButton_ = platformFooter->removeButton();
    platformRemoveButton_->setObjectName(QStringLiteral("projectPlatformDelete"));
    platformsLayout->addWidget(platformTabs);
    platformEditor_ = new ProjectPlatformEditor(platformsGroup);
    QtControlSkin::setSemanticProperty(platformEditor_, "joinedEdge", QStringLiteral("left"));
    platformsLayout->addWidget(platformEditor_, 1);
    static_cast<QVBoxLayout*>(platformsGroup->layout())->addLayout(platformsLayout);
    connect(platformAddButton_, &QToolButton::clicked, this, &ProjectConfigPage::addPlatform);
    connect(platformList_, &QListWidget::currentRowChanged, this, &ProjectConfigPage::selectPlatform);
    connect(platformRemoveButton_, &QToolButton::clicked, this, &ProjectConfigPage::removePlatform);
    connect(platformEditor_, &ProjectPlatformEditor::platformChanged,
            this, &ProjectConfigPage::commitPlatform);
    platformBusyLabel_ = new QLabel(tr("Platform settings are unavailable while audio processing or export is running."), platformsGroup);
    platformBusyLabel_->setWordWrap(true);
    QtControlSkin::setSemanticProperty(platformBusyLabel_, "textRole", QStringLiteral("secondary"));
    static_cast<QVBoxLayout*>(platformsGroup->layout())->addWidget(platformBusyLabel_);
    body->addWidget(platformsGroup);

    auto* languagesGroup = makeSection(tr("Language Configuration"));
    ContextInfo::tag(languagesGroup, ContextTopic::forPanel(QStringLiteral("settings.config.languages")));
    auto* languagesLayout = new QHBoxLayout();
    languagesLayout->setContentsMargins(0, 0, 0, 0);
    languagesLayout->setSpacing(0);
    auto* languageTabs = new TableWidget(
        {TableKind::Config, TableBodySizing::Expanding}, languagesGroup);
    languageTabs->setFixedWidth(150);
    targetLanguagesList_ = languageTabs->createNavigationView(QStringLiteral("targetLanguagesList"));
    targetLanguagesList_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    targetLanguagesList_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    auto* targetLanguagesFoot = languageTabs->footer();
    auto* targetLanguageAddBtn = targetLanguagesFoot->addButton();
    targetLanguageAddBtn->setObjectName(QStringLiteral("targetLanguageAddButton"));
    targetLanguageRemoveBtn_ = targetLanguagesFoot->removeButton();
    targetLanguageRemoveBtn_->setObjectName(QStringLiteral("targetLanguageRemoveButton"));
    languagesLayout->addWidget(languageTabs);
    auto* languageEditor = new QWidget(languagesGroup);
    languageEditor->setObjectName(QStringLiteral("targetLanguageEditor"));
    languageEditor->setAttribute(Qt::WA_StyledBackground, true);
    QtControlSkin::setSemanticProperty(languageEditor, "surfaceRole", QStringLiteral("panel"));
    QtControlSkin::setSemanticProperty(languageEditor, "joinedEdge", QStringLiteral("left"));
    auto* languageEditorLayout = new QVBoxLayout(languageEditor);
    languageEditorLayout->setContentsMargins(12, 12, 12, 12);
    targetLanguageFields_ = new QWidget(languageEditor);
    auto languageFieldsPolicy = targetLanguageFields_->sizePolicy();
    languageFieldsPolicy.setRetainSizeWhenHidden(true);
    targetLanguageFields_->setSizePolicy(languageFieldsPolicy);
    auto* languageForm = makeForm();
    targetLanguageFields_->setLayout(languageForm);
    targetLanguageEdit_ = new QLineEdit(targetLanguageFields_);
    targetLanguageEdit_->setObjectName(QStringLiteral("targetLanguageEdit"));
    languageForm->addRow(tr("Language:"), targetLanguageEdit_);
    languageEditorLayout->addWidget(targetLanguageFields_);
    languageEditorLayout->addStretch();
    languagesLayout->addWidget(languageEditor, 1);
    static_cast<QVBoxLayout*>(languagesGroup->layout())->addLayout(languagesLayout);
    connect(targetLanguageAddBtn, &QToolButton::clicked, this, &ProjectConfigPage::addTargetLanguage);
    connect(targetLanguagesList_, &QListWidget::currentRowChanged,
            this, &ProjectConfigPage::selectTargetLanguage);
    connect(targetLanguageEdit_, &QLineEdit::textChanged, this, [this](const QString& language) {
        auto languages = ProjectManager::instance().getProjectLanguages();
        const int index = languages.indexOf(selectedLanguage_);
        if (index >= 0) languages[index] = language;
        const auto normalized = ProjectLanguageSemantics::normalize(languages);
        const QString error = language.trimmed().isEmpty() ? tr("Language name cannot be empty.")
            : normalized.error;
        targetLanguageEdit_->setToolTip(error);
        QtControlSkin::setSemanticProperty(targetLanguageEdit_, "inputInvalid", !error.isEmpty());
    });
    connect(targetLanguageEdit_, &QLineEdit::editingFinished, this, &ProjectConfigPage::commitLanguageName);
    connect(targetLanguageRemoveBtn_, &QToolButton::clicked,
            this, &ProjectConfigPage::onRemoveTargetLanguageClicked);

    body->addWidget(languagesGroup);

    body->addStretch();
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::projectOpened, this, &ProjectConfigPage::load);
    connect(&manager, &ProjectManager::projectClosed, this, &ProjectConfigPage::load);
    connect(&manager, &ProjectManager::targetPlatformsChanged, this, [this] { syncPlatforms(); });
    connect(&manager, &ProjectManager::projectLanguagesChanged, this, [this] { syncLanguages(); });
    connect(&ProjectPlatformActivity::instance(), &ProjectPlatformActivity::busyChanged,
            this, &ProjectConfigPage::updatePlatformAvailability);
}

QString ProjectConfigPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageConfig);
}

QString ProjectConfigPage::categoryLabel() const
{
    return tr("General");
}

bool ProjectConfigPage::hasCurrentProject() const
{
    const auto project = projectLease_.lock();
    return project && ProjectManager::instance().currentProject() == project.get();
}

void ProjectConfigPage::load()
{
    platformEditor_->discardNameEditing();
    targetLanguageEdit_->setModified(false);
    projectLease_ = ProjectManager::instance().currentProjectLease();
    selectedLanguage_.clear();
    syncPlatforms(true, 0);
    syncLanguages(true, 0);
    setEnabled(hasCurrentProject());
}

void ProjectConfigPage::finishEditing()
{
    platformEditor_->finishNameEditing();
    commitLanguageName();
}

void ProjectConfigPage::hideEvent(QHideEvent* event)
{
    finishEditing();
    SettingsPage::hideEvent(event);
}

void ProjectConfigPage::syncPlatforms(bool finishEditing, int selectedIndex)
{
    if (committing_ || (!finishEditing && platformEditor_->hasPendingName())) return;
    platformEditor_->discardNameEditing();
    const auto project = projectLease_.lock();
    platforms_ = hasCurrentProject() ? project->platforms : QVector<ProjectPlatform>{};
    refreshPlatforms(selectedIndex);
}

void ProjectConfigPage::refreshPlatforms(int selectedIndex)
{
    if (selectedIndex < 0) {
        selectedIndex = platformList_->currentRow();
        if (const auto* current = platformList_->currentItem()) {
            const auto id = current->data(Qt::UserRole).toUuid();
            for (int i = 0; i < platforms_.size(); ++i)
                if (platforms_.at(i).id == id) selectedIndex = i;
        }
    }
    const int index = platforms_.isEmpty() ? -1
        : std::clamp(selectedIndex, 0, int(platforms_.size()) - 1);
    {
        const QSignalBlocker blocker(platformList_);
        platformList_->clear();
        for (const auto& platform : platforms_) {
            auto* row = new QListWidgetItem(platform.name, platformList_);
            row->setData(Qt::UserRole, platform.id);
            row->setToolTip(platform.name);
        }
        platformList_->setCurrentRow(index);
    }
    selectPlatform(index);
}

void ProjectConfigPage::selectPlatform(int index)
{
    auto id = platformList_->item(index)
        ? platformList_->item(index)->data(Qt::UserRole).toUuid() : QUuid{};
    platformEditor_->finishNameEditing();
    if (!ProjectPlatformSemantics::find(platforms_, id) && platformList_->currentItem())
        id = platformList_->currentItem()->data(Qt::UserRole).toUuid();
    const auto* selected = ProjectPlatformSemantics::find(platforms_, id);
    for (int row = 0; row < platformList_->count(); ++row) {
        if (platformList_->item(row)->data(Qt::UserRole).toUuid() != id) continue;
        const QSignalBlocker blocker(platformList_);
        platformList_->setCurrentRow(row);
        break;
    }
    platformEditor_->setVisible(selected != nullptr);
    if (selected) {
        QStringList otherNames;
        for (const auto& platform : platforms_)
            if (platform.id != id) otherNames.append(platform.name);
        platformEditor_->setPlatform(*selected, otherNames);
    }
    updatePlatformAvailability();
}

void ProjectConfigPage::updatePlatformAvailability()
{
    const bool busy = ProjectPlatformActivity::instance().isBusy();
    platformBusyLabel_->setVisible(busy);
    platformAddButton_->setEnabled(hasCurrentProject() && !busy);
    platformRemoveButton_->setEnabled(hasCurrentProject() && !busy
        && platformList_->currentRow() >= 0 && platforms_.size() > 1);
    platformEditor_->setReadOnly(busy);
}

void ProjectConfigPage::commitPlatform(const ProjectPlatform& before)
{
    if (committing_) return;
    if (!hasCurrentProject() || ProjectPlatformActivity::instance().isBusy()) {
        syncPlatforms(true);
        return;
    }
    const auto next = platformEditor_->platform();
    if (next.toJson() == before.toJson()) { syncPlatforms(true); return; }
    const auto* current = ProjectPlatformSemantics::find(ProjectManager::instance().currentProject()->platforms, before.id);
    if (!current || current->toJson() != before.toJson()) {
        syncPlatforms(true);
        showError(tr("This platform changed while you were editing. The current settings were restored."));
        return;
    }
    QString command;
    QJsonObject args{{QStringLiteral("platform_id"), ProjectPlatformSemantics::idString(before.id)}};
    if (next.name != before.name) {
        command = QStringLiteral("rename_project_platform");
        args.insert(QStringLiteral("name"), next.name);
    } else if (next.basePlatform != before.basePlatform) {
        command = QStringLiteral("set_project_platform_base");
        args.insert(QStringLiteral("base_platform"), ProjectPlatformSemantics::basePlatformName(next.basePlatform));
    } else if (next.profile.toJson() != before.profile.toJson()) {
        command = QStringLiteral("set_project_platform_profile");
        args.insert(QStringLiteral("profile"), next.profile.toJson());
    }
    if (command.isEmpty()) return;
    ActionResult result;
    {
        QScopedValueRollback guard(committing_, true);
        result = ActionManager::instance().execute(command, args, ActionSource::GUI);
    }
    syncPlatforms(true);
    if (!result.success) showError(result.errorMessage);
}

void ProjectConfigPage::addPlatform()
{
    const auto project = projectLease_.lock();
    if (!hasCurrentProject() || ProjectPlatformActivity::instance().isBusy()) return;
    ProjectPlatformDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || !hasCurrentProject()
        || project != projectLease_.lock() || ProjectPlatformActivity::instance().isBusy()) return;
    auto definition = dialog.platform();
    const QString baseName = definition.name;
    int suffix = 2;
    while (std::any_of(project->platforms.cbegin(), project->platforms.cend(),
                      [&](const auto& platform) {
                          return platform.name.compare(definition.name, Qt::CaseInsensitive) == 0;
                      }))
        definition.name = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
    ActionResult result;
    {
        QScopedValueRollback guard(committing_, true);
        result = ActionManager::instance().execute(QStringLiteral("create_project_platform"),
            {{QStringLiteral("name"), definition.name},
             {QStringLiteral("base_platform"), ProjectPlatformSemantics::basePlatformName(definition.basePlatform)},
             {QStringLiteral("profile"), definition.profile.toJson()}}, ActionSource::GUI);
    }
    syncPlatforms(true, project->platforms.size() - 1);
    if (!result.success) showError(result.errorMessage);
}

void ProjectConfigPage::removePlatform()
{
    if (!hasCurrentProject() || ProjectPlatformActivity::instance().isBusy()) return;
    const auto* item = platformList_->currentItem();
    if (!item) return;
    const auto id = item->data(Qt::UserRole).toUuid();
    ActionResult result;
    {
        QScopedValueRollback guard(committing_, true);
        result = ActionManager::instance().execute(QStringLiteral("delete_project_platform"),
            {{QStringLiteral("platform_id"), ProjectPlatformSemantics::idString(id)}}, ActionSource::GUI);
    }
    syncPlatforms(true);
    if (!result.success) showError(result.errorMessage);
}

void ProjectConfigPage::syncLanguages(bool finishEditing, int selectedIndex)
{
    if (committing_ || (!finishEditing && targetLanguageEdit_->isModified())) return;
    targetLanguageEdit_->setModified(false);
    const auto project = projectLease_.lock();
    const auto languages = hasCurrentProject()
        ? ProjectLanguageSemantics::buildLanguages(*project) : QStringList{};
    if (selectedIndex < 0) selectedIndex = languages.indexOf(selectedLanguage_);
    setTargetLanguages(languages, selectedIndex);
}

void ProjectConfigPage::setTargetLanguages(const QStringList& value, int selectedIndex)
{
    if (selectedIndex < 0) selectedIndex = targetLanguagesList_->currentRow();
    const int index = value.isEmpty() ? -1 : std::clamp(selectedIndex, 0, int(value.size()) - 1);
    {
        const QSignalBlocker blocker(targetLanguagesList_);
        targetLanguagesList_->clear();
        for (const QString& language : value) {
            auto* item = new QListWidgetItem(language, targetLanguagesList_);
            item->setToolTip(language);
        }
        targetLanguagesList_->setCurrentRow(index);
    }
    selectTargetLanguage(index);
}

void ProjectConfigPage::selectTargetLanguage(int index)
{
    QString selected = targetLanguagesList_->item(index)
        ? targetLanguagesList_->item(index)->text() : QString();
    commitLanguageName();
    if (targetLanguagesList_->findItems(selected, Qt::MatchExactly).isEmpty())
        selected = targetLanguagesList_->currentItem()
            ? targetLanguagesList_->currentItem()->text() : QString();
    selectedLanguage_ = selected;
    for (int row = 0; row < targetLanguagesList_->count(); ++row) {
        if (targetLanguagesList_->item(row)->text() != selected) continue;
        const QSignalBlocker blocker(targetLanguagesList_);
        targetLanguagesList_->setCurrentRow(row);
        break;
    }
    targetLanguageFields_->setVisible(!selected.isEmpty());
    targetLanguageRemoveBtn_->setEnabled(!selected.isEmpty());
    const QSignalBlocker blocker(targetLanguageEdit_);
    targetLanguageEdit_->setText(selected);
    targetLanguageEdit_->setToolTip(QString());
    QtControlSkin::setSemanticProperty(targetLanguageEdit_, "inputInvalid", false);
}

bool ProjectConfigPage::commitLanguages(const QStringList& languages, int selectedIndex)
{
    if (!hasCurrentProject()) return false;
    ActionResult result;
    {
        QScopedValueRollback guard(committing_, true);
        result = ActionManager::instance().execute(QStringLiteral("set_project_languages"),
            {{QStringLiteral("languages"), QJsonArray::fromStringList(languages)}}, ActionSource::GUI);
    }
    syncLanguages(true, selectedIndex);
    if (!result.success) showError(result.errorMessage);
    return result.success;
}

void ProjectConfigPage::commitLanguageName()
{
    if (committing_ || !targetLanguageEdit_->isModified()) return;
    const auto text = targetLanguageEdit_->text();
    targetLanguageEdit_->setModified(false);
    if (!hasCurrentProject()) { syncLanguages(true); return; }
    auto languages = ProjectManager::instance().getProjectLanguages();
    const int index = languages.indexOf(selectedLanguage_);
    if (index < 0) {
        syncLanguages(true);
        showError(tr("This language changed while you were editing. The current settings were restored."));
        return;
    }
    languages[index] = text;
    const auto normalized = ProjectLanguageSemantics::normalize(languages);
    if (text.trimmed().isEmpty() || !normalized.ok()) { syncLanguages(true); return; }
    commitLanguages(normalized.value, index);
}

void ProjectConfigPage::addTargetLanguage()
{
    const auto project = projectLease_.lock();
    if (!hasCurrentProject()) return;
    BaseDialog dialog(this);
    dialog.setWindowTitle(tr("Add Language"));
    dialog.setObjectName(QStringLiteral("targetLanguageDialog"));
    dialog.resize(340, 120);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new DialogFormLayout();
    auto* edit = new QLineEdit(&dialog);
    edit->setObjectName(QStringLiteral("targetLanguageEdit"));
    form->addRow(tr("Language:"), edit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    connect(edit, &QLineEdit::textChanged, &dialog, [edit, ok, project](const QString& text) {
        auto languages = ProjectLanguageSemantics::buildLanguages(*project);
        languages.append(text);
        const auto normalized = ProjectLanguageSemantics::normalize(languages);
        const QString error = text.trimmed().isEmpty() ? tr("Language name cannot be empty.") : normalized.error;
        edit->setToolTip(error);
        QtControlSkin::setSemanticProperty(edit, "inputInvalid", !error.isEmpty());
        ok->setEnabled(error.isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted || !hasCurrentProject() || project != projectLease_.lock()) return;
    auto languages = ProjectLanguageSemantics::buildLanguages(*project);
    languages.append(edit->text());
    commitLanguages(languages, languages.size() - 1);
}

void ProjectConfigPage::onRemoveTargetLanguageClicked()
{
    if (!hasCurrentProject()) return;
    auto languages = ProjectManager::instance().getProjectLanguages();
    const int index = languages.indexOf(selectedLanguage_);
    if (index < 0) return;
    languages.removeAt(index);
    commitLanguages(languages, index);
}

}
