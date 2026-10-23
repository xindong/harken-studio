#include "Dialogs/Settings/Pages/ProjectConfigPage.h"

#include "Core/ActionManager.h"
#include "Core/BaseDialog.h"
#include "Core/ContextInfo.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"

#include "Dialogs/Settings/ProjectPlatformDialog.h"
#include "Dialogs/Settings/ProjectPlatformEditor.h"
#include "Widgets/Common/DialogFormLayout.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"
#include <QListWidget>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
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
    auto* add = platformFooter->addButton();
    add->setObjectName(QStringLiteral("projectPlatformAdd"));
    platformRemoveButton_ = platformFooter->removeButton();
    platformRemoveButton_->setObjectName(QStringLiteral("projectPlatformDelete"));
    platformsLayout->addWidget(platformTabs);
    platformEditor_ = new ProjectPlatformEditor(platformsGroup);
    QtControlSkin::setSemanticProperty(platformEditor_, "joinedEdge", QStringLiteral("left"));
    platformsLayout->addWidget(platformEditor_, 1);
    static_cast<QVBoxLayout*>(platformsGroup->layout())->addLayout(platformsLayout);
    connect(add, &QToolButton::clicked, this, &ProjectConfigPage::addPlatform);
    connect(platformList_, &QListWidget::currentRowChanged, this, &ProjectConfigPage::selectPlatform);
    connect(platformRemoveButton_, &QToolButton::clicked, this, &ProjectConfigPage::removePlatform);
    connect(platformEditor_, &ProjectPlatformEditor::platformChanged, this, [this]() {
        const int index = platformList_->currentRow();
        if (index < 0 || index >= draftPlatforms_.size()) return;
        draftPlatforms_[index] = platformEditor_->platform();
        platformList_->item(index)->setText(draftPlatforms_.at(index).name);
        platformList_->item(index)->setToolTip(draftPlatforms_.at(index).name);
        emit dirtyChanged();
    });
    body->addWidget(platformsGroup);

    auto* languagesGroup = makeSection(tr("Language Configuration"));
    ContextInfo::tag(languagesGroup, ContextTopic::forPanel(QStringLiteral("settings.config.languages")));
    targetLanguagesBox_ = new TableWidget(languagesGroup);
    targetLanguagesList_ = targetLanguagesBox_->createItemView(
        QStringLiteral("targetLanguagesList"), {tr("Language")}, {1000});
    targetLanguagesList_->setHeaderHidden(true);
    targetLanguagesList_->setSelectionMode(QAbstractItemView::SingleSelection);
    targetLanguagesList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* targetLanguagesFoot = targetLanguagesBox_->footer();
    auto* targetLanguageAddBtn = targetLanguagesFoot->addButton();
    targetLanguageAddBtn->setObjectName(QStringLiteral("targetLanguageAddButton"));
    targetLanguageRemoveBtn_ = targetLanguagesFoot->removeButton();
    targetLanguageRemoveBtn_->setObjectName(QStringLiteral("targetLanguageRemoveButton"));
    targetLanguageRemoveBtn_->setEnabled(false);
    connect(targetLanguageAddBtn, &QToolButton::clicked, this, [this] { editTargetLanguage(); });
    connect(targetLanguagesList_, &QTreeWidget::itemDoubleClicked,
            this, &ProjectConfigPage::editTargetLanguage);
    connect(targetLanguagesList_, &QTreeWidget::itemSelectionChanged, this, [this] {
        targetLanguageRemoveBtn_->setEnabled(!targetLanguagesList_->selectedItems().isEmpty());
    });
    connect(targetLanguageRemoveBtn_, &QToolButton::clicked,
            this, &ProjectConfigPage::onRemoveTargetLanguageClicked);
    languagesGroup->layout()->addWidget(targetLanguagesBox_);

    body->addWidget(languagesGroup);

    body->addStretch();
}

QString ProjectConfigPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageConfig);
}

QString ProjectConfigPage::categoryLabel() const
{
    return tr("General");
}

QJsonArray ProjectConfigPage::platformDraft() const
{
    auto rows = ProjectPlatformSemantics::platformsToJson(draftPlatforms_);
    for (int i = 0; i < rows.size(); ++i) {
        if (!draftPlatforms_.at(i).id.isNull()) continue;
        auto row = rows.at(i).toObject();
        row.remove(QStringLiteral("id"));
        rows.replace(i, row);
    }
    return rows;
}

void ProjectConfigPage::refreshPlatforms(int selectedIndex)
{
    if (selectedIndex < 0) {
        selectedIndex = platformList_->currentRow();
        if (const auto* current = platformList_->currentItem()) {
            const auto id = current->data(Qt::UserRole).toUuid();
            for (int i = 0; i < draftPlatforms_.size(); ++i) {
                if (id.isNull() ? draftPlatforms_.at(i).name == current->text()
                                : draftPlatforms_.at(i).id == id) {
                    selectedIndex = i;
                    break;
                }
            }
        }
    }
    const int index = draftPlatforms_.isEmpty()
        ? -1 : std::clamp(selectedIndex, 0, int(draftPlatforms_.size()) - 1);
    {
        const QSignalBlocker blocker(platformList_);
        platformList_->clear();
        for (const auto& platform : draftPlatforms_) {
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
    const bool selected = index >= 0 && index < draftPlatforms_.size();
    platformEditor_->setVisible(selected);
    platformRemoveButton_->setEnabled(selected && draftPlatforms_.size() > 1);
    if (selected) platformEditor_->setPlatform(draftPlatforms_.at(index));
}

void ProjectConfigPage::addPlatform()
{
    ProjectPlatformDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    auto definition = dialog.platform();
    const QString baseName = definition.name;
    int suffix = 2;
    while (std::any_of(draftPlatforms_.cbegin(), draftPlatforms_.cend(),
                      [&](const auto& platform) {
                          return platform.name.compare(definition.name, Qt::CaseInsensitive) == 0;
                      }))
        definition.name = QStringLiteral("%1 %2").arg(baseName).arg(suffix++);
    draftPlatforms_.append(definition);
    refreshPlatforms(draftPlatforms_.size() - 1);
    emit dirtyChanged();
}

void ProjectConfigPage::removePlatform()
{
    const int index = platformList_->currentRow();
    if (index < 0 || draftPlatforms_.size() <= 1) return;
    draftPlatforms_.removeAt(index);
    refreshPlatforms(index);
    emit dirtyChanged();
}

QStringList ProjectConfigPage::targetLanguages() const
{
    QStringList languages;
    for (int i = 0; i < targetLanguagesList_->topLevelItemCount(); ++i)
        languages << targetLanguagesList_->topLevelItem(i)->text(0);
    return languages;
}

void ProjectConfigPage::rememberBaseline()
{
    baselinePlatforms_ = platformDraft();
    baselineProjectLanguages_ = targetLanguages();
}

void ProjectConfigPage::setTargetLanguages(const QStringList& value)
{
    targetLanguagesBox_->clearSelection();
    targetLanguagesList_->clear();
    for (const QString& language : value)
        new QTreeWidgetItem(targetLanguagesList_, {language});
}

void ProjectConfigPage::editTargetLanguage(QTreeWidgetItem* item)
{
    BaseDialog dialog(this);
    dialog.setWindowTitle(item ? tr("Edit Language") : tr("Add Language"));
    dialog.setObjectName(QStringLiteral("targetLanguageDialog"));
    dialog.resize(340, 120);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new DialogFormLayout();
    auto* edit = new QLineEdit(item ? item->text(0) : QString(), &dialog);
    edit->setObjectName(QStringLiteral("targetLanguageEdit"));
    edit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9_]*")), edit));
    form->addRow(tr("Language:"), edit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(!edit->text().isEmpty());
    connect(edit, &QLineEdit::textChanged, &dialog, [ok](const QString& text) {
        ok->setEnabled(!text.isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    edit->setFocus();
    edit->selectAll();
    if (dialog.exec() != QDialog::Accepted) return;
    const QString language = edit->text();
    if (item) {
        if (item->text(0) == language) return;
        item->setText(0, language);
    } else {
        item = new QTreeWidgetItem(targetLanguagesList_, {language});
    }
    targetLanguagesList_->setCurrentItem(item);
    emit dirtyChanged();
}

void ProjectConfigPage::onRemoveTargetLanguageClicked()
{
    auto* item = targetLanguagesList_->currentItem();
    if (!item || !item->isSelected()) return;
    targetLanguagesBox_->removeSelectedRow([this, item] {
        delete item;
        emit dirtyChanged();
        return true;
    });
}

void ProjectConfigPage::load()
{
    auto& pm = ProjectManager::instance();
    const bool projectChanged = projectLease_.lock() != pm.currentProjectLease();
    draftPlatforms_.clear();
    projectLease_ = pm.currentProjectLease();
    QStringList projectLanguages;
    if (pm.isProjectOpen()) {
        draftPlatforms_ = pm.currentProject()->platforms;
        projectLanguages = ProjectLanguageSemantics::buildLanguages(*pm.currentProject());
    }
    refreshPlatforms(projectChanged ? 0 : -1);
    setTargetLanguages(projectLanguages);
    rememberBaseline();
}

bool ProjectConfigPage::hasExternalChanges() const
{
    const auto project = projectLease_.lock();
    return !project || ProjectManager::instance().currentProject() != project.get()
        || ProjectPlatformSemantics::platformsToJson(project->platforms) != baselinePlatforms_
        || ProjectLanguageSemantics::buildLanguages(*project) != baselineProjectLanguages_;
}

bool ProjectConfigPage::validate()
{
    auto platforms = draftPlatforms_;
    for (auto& platform : platforms)
        if (platform.id.isNull()) platform.id = QUuid::createUuid();
    QString error;
    if (!ProjectPlatformSemantics::validatePlatforms(platforms, &error)) {
        QMessageBox::warning(this, tr("Platform Configuration"), error);
        return false;
    }
    const auto languages = ProjectLanguageSemantics::normalize(targetLanguages());
    if (!languages.ok()) {
        QMessageBox::warning(this, tr("Language Configuration"), languages.error);
        return false;
    }
    return true;
}

bool ProjectConfigPage::applyTargetLanguages()
{
    auto& pm = ProjectManager::instance();
    if (!pm.isProjectOpen())
        return true;

    const auto normalized = ProjectLanguageSemantics::normalize(targetLanguages());
    if (!normalized.ok())
    {
        QMessageBox::warning(this, tr("Language Configuration"),
                             normalized.error.isEmpty()
                                 ? tr("Failed to update target languages.")
                                 : normalized.error);
        return false;
    }

    const QStringList current = ProjectLanguageSemantics::buildLanguages(*pm.currentProject());
    if (normalized.value == current)
    {
        setTargetLanguages(normalized.value);
        return true;
    }

    QJsonArray arr;
    for (const QString& language : normalized.value)
        arr.append(language);
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_project_languages"),
        QJsonObject{{QStringLiteral("languages"), arr}},
        ActionSource::GUI);
    if (!result.success)
    {
        QMessageBox::warning(this, tr("Language Configuration"),
                             result.errorMessage.isEmpty()
                                 ? tr("Failed to update target languages.")
                                 : result.errorMessage);
        return false;
    }

    setTargetLanguages(normalized.value);
    return true;
}

bool ProjectConfigPage::applyTargetPlatforms()
{
    if (platformDraft() == baselinePlatforms_) return true;
    const auto result = ActionManager::instance().execute(QStringLiteral("apply_project_platforms"),
        {{QStringLiteral("platforms"), platformDraft()}, {QStringLiteral("expected_platforms"), baselinePlatforms_}},
        ActionSource::GUI);
    if (!result.success) {
        QMessageBox::warning(this, tr("Platform Configuration"), result.errorMessage);
        return false;
    }
    draftPlatforms_ = ProjectManager::instance().currentProject()->platforms;
    refreshPlatforms();
    return true;
}

bool ProjectConfigPage::apply()
{
    if (!applyTargetPlatforms() || !applyTargetLanguages()) return false;
    rememberBaseline();
    emit dirtyChanged();
    return true;
}

bool ProjectConfigPage::isDirty() const
{
    return platformDraft() != baselinePlatforms_
        || targetLanguages() != baselineProjectLanguages_;
}

}
