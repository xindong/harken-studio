#include "Dialogs/Settings/Pages/ProjectBuildPage.h"

#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ContextInfo.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"

#include "Dialogs/Settings/ProjectPlatformDialog.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include <QTreeWidget>
#include <QDir>
#include <QFileDialog>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace VNX {

namespace {


void refreshStyle(QWidget* widget)
{
    if (!widget) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

class ClickableWidget : public QWidget
{
public:
    using QWidget::QWidget;

    std::function<void()> onMousePress;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        QWidget::mousePressEvent(event);
        if (onMousePress) onMousePress();
    }
};

class FocusAwareLineEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

    std::function<void()> onFocusIn;

protected:
    void focusInEvent(QFocusEvent* event) override
    {
        QLineEdit::focusInEvent(event);
        if (onFocusIn && (event->reason() == Qt::MouseFocusReason
            || event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason))
            onFocusIn();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        QLineEdit::mousePressEvent(event);
        if (event->button() == Qt::LeftButton && onFocusIn) onFocusIn();
    }
};

}

ProjectBuildPage::ProjectBuildPage(QWidget* parent)
    : SettingsPage(parent)
{
    auto* body = makePageBody();

    auto* platformsGroup = makeSection(tr("Target Platforms"));
    ContextInfo::tag(platformsGroup, ContextTopic::forPanel(QStringLiteral("settings.build.platforms")));
    auto* platformsBox = new TableWidget(platformsGroup);
    platformList_ = platformsBox->createItemView(QStringLiteral("projectPlatformList"),
        {tr("Name"), tr("Base Platform")}, {600, 400});
    platformList_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* platformFooter = platformsBox->footer();
    auto* add = platformFooter->addButton();
    add->setObjectName(QStringLiteral("projectPlatformAdd"));
    platformRemoveButton_ = platformFooter->removeButton();
    platformRemoveButton_->setObjectName(QStringLiteral("projectPlatformDelete"));
    platformsGroup->layout()->addWidget(platformsBox);
    connect(add, &QToolButton::clicked, this, [this]() { editPlatform(true); });
    connect(platformList_, &QTreeWidget::itemDoubleClicked, this, [this]() { editPlatform(false); });
    connect(platformRemoveButton_, &QToolButton::clicked, this, &ProjectBuildPage::removePlatform);
    connect(platformList_, &QTreeWidget::itemSelectionChanged, this, [this]() {
        const bool selected = !platformList_->selectedItems().isEmpty();
        platformRemoveButton_->setEnabled(selected && draftPlatforms_.size() > 1);
    });
    body->addWidget(platformsGroup);

    auto* languagesGroup = makeSection(tr("Target Languages"));
    ContextInfo::tag(languagesGroup, ContextTopic::forPanel(QStringLiteral("settings.build.languages")));
    targetLanguagesBox_ = new TableWidget(languagesGroup);
    targetLanguagesBox_->setSelectionClearHandler([this] { selectTargetLanguageRow(-1); });
    targetLanguagesBox_->setRowSelectionHandler([this](QWidget* row) {
        selectTargetLanguageRow(targetLanguageRows_.indexOf(row));
    });
    targetLanguagesBox_->setColumns(QStringLiteral("TargetLanguages"), {
        {QStringLiteral("Language"), tr("Language"), 1000}
    });
    targetLanguagesBoxLayout_ = targetLanguagesBox_->rowsLayout();
    auto* targetLanguagesFoot = targetLanguagesBox_->footer();
    targetLanguageAddBtn_ = targetLanguagesFoot->addButton();
    targetLanguageAddBtn_->setObjectName(QStringLiteral("targetLanguageAddButton"));
    targetLanguageRemoveBtn_ = targetLanguagesFoot->removeButton();
    targetLanguageRemoveBtn_->setObjectName(QStringLiteral("targetLanguageRemoveButton"));
    connect(targetLanguageAddBtn_, &QToolButton::clicked,
            this, &ProjectBuildPage::onAddTargetLanguageClicked);
    connect(targetLanguageRemoveBtn_, &QToolButton::clicked,
            this, &ProjectBuildPage::onRemoveTargetLanguageClicked);
    languagesGroup->layout()->addWidget(targetLanguagesBox_);

    body->addWidget(languagesGroup);

    auto* packGroup = makeSection(tr("Pack Build Output"));
    ContextInfo::tag(packGroup, ContextTopic::forPanel(QStringLiteral("settings.build.output")));
    auto* form = makeForm();
    auto* dirLabel = new QLabel(tr("Relative Directory:"));

    auto* rowWidget = new QWidget();
    auto* row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    packBuildOutputDirEdit_ = new QLineEdit();
    packBuildOutputDirEdit_->setObjectName("packBuildOutputDirEdit");
    packBuildOutputDirEdit_->setPlaceholderText(PathUtils::buildPacksDirName() + '/');
    packBuildOutputDirEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[^*?<>|]*")), packBuildOutputDirEdit_));
    packBuildOutputDirBrowseBtn_ = new QPushButton(tr("Browse..."));
    packBuildOutputDirBrowseBtn_->setObjectName("packBuildOutputDirBrowseBtn");
    row->addWidget(packBuildOutputDirEdit_, 1);
    row->addWidget(packBuildOutputDirBrowseBtn_, 0);
    form->addRow(dirLabel, rowWidget);
    static_cast<QVBoxLayout*>(packGroup->layout())->addLayout(form);

    body->addWidget(packGroup);

    connect(packBuildOutputDirEdit_, &QLineEdit::textChanged, this, [this]() {
        emit dirtyChanged();
    });
    connect(packBuildOutputDirEdit_, &QLineEdit::editingFinished,
            this, &ProjectBuildPage::onPackBuildOutputDirEditingFinished);
    connect(packBuildOutputDirBrowseBtn_, &QPushButton::clicked,
            this, &ProjectBuildPage::onPackBuildOutputDirBrowseClicked);

    body->addStretch();
}

QString ProjectBuildPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageBuild);
}

QString ProjectBuildPage::categoryLabel() const
{
    return tr("Build");
}

QJsonArray ProjectBuildPage::platformDraft() const
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

void ProjectBuildPage::refreshPlatforms()
{
    const auto* current = platformList_->currentItem();
    const bool selected = current && current->isSelected();
    const QUuid selectedId = selected ? current->data(0, Qt::UserRole).toUuid() : QUuid{};
    const QString selectedName = selected ? current->text(0) : QString{};
    platformList_->clear();
    for (const auto& platform : draftPlatforms_) {
        auto* row = new QTreeWidgetItem(platformList_, {platform.name,
            ProjectPlatformSemantics::basePlatformName(platform.basePlatform)});
        row->setData(0, Qt::UserRole, platform.id);
        row->setToolTip(0, platform.name);
        if (selected && (selectedId.isNull() ? platform.name == selectedName : platform.id == selectedId))
            platformList_->setCurrentItem(row);
    }
    platformRemoveButton_->setEnabled(!platformList_->selectedItems().isEmpty() && draftPlatforms_.size() > 1);
}

void ProjectBuildPage::editPlatform(bool create)
{
    const int index = platformList_->indexOfTopLevelItem(platformList_->currentItem());
    if (!create && index < 0) return;
    ProjectPlatform definition = create ? ProjectPlatform{} : draftPlatforms_.at(index);
    ProjectPlatformDialog dialog(definition, create, this);
    if (dialog.exec() != QDialog::Accepted) return;
    definition = dialog.platform();
    auto next = draftPlatforms_;
    if (create) next.append(definition);
    else next[index] = definition;
    auto validation = next;
    for (auto& platform : validation)
        if (platform.id.isNull()) platform.id = QUuid::createUuid();
    QString error;
    if (!ProjectPlatformSemantics::validatePlatforms(validation, &error)) {
        QMessageBox::warning(this, tr("Project Platforms"), error);
        return;
    }
    draftPlatforms_ = next;
    refreshPlatforms();
    platformList_->setCurrentItem(platformList_->topLevelItem(create ? draftPlatforms_.size() - 1 : index));
    emit dirtyChanged();
}

void ProjectBuildPage::removePlatform()
{
    const int index = platformList_->indexOfTopLevelItem(platformList_->currentItem());
    if (index < 0 || draftPlatforms_.size() <= 1) return;
    static_cast<TableWidget*>(platformList_->parentWidget())->removeSelectedRow([this, index] {
        draftPlatforms_.removeAt(index);
        refreshPlatforms();
        emit dirtyChanged();
        return true;
    });
}

QStringList ProjectBuildPage::targetLanguages() const
{
    QStringList languages;
    for (QLineEdit* edit : targetLanguageEdits_)
        languages << edit->text();
    return languages;
}

void ProjectBuildPage::rememberBaseline()
{
    baselinePlatforms_ = platformDraft();
    baselineProjectLanguages_ = targetLanguages();
    baselinePackBuildOutputDir_ = packBuildOutputDirEdit_->text();
}

void ProjectBuildPage::clearTargetLanguageRows()
{
    selectedTargetLanguageRow_ = -1;
    while (!targetLanguageRows_.isEmpty())
    {
        auto* row = targetLanguageRows_.takeLast();
        targetLanguagesBoxLayout_->removeWidget(row);
        delete row;
    }
    targetLanguageEdits_.clear();
    updateTargetLanguageRowState();
}

void ProjectBuildPage::appendTargetLanguageRow(const QString& value)
{
    auto* row = new ClickableWidget(targetLanguagesBox_->rowsWidget());
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setProperty("listRow", true);
    row->setProperty("selected", false);
    row->setProperty("lastRow", false);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* cell = targetLanguagesBox_->createCell(row);
    auto* edit = new FocusAwareLineEdit(cell);
    edit->setText(value);
    edit->setPlaceholderText(tr("Language"));
    edit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9_]*")), edit));

    row->onMousePress = [this, row]() {
        selectTargetLanguageRow(targetLanguageRows_.indexOf(row));
    };
    edit->onFocusIn = [this, row]() {
        selectTargetLanguageRow(targetLanguageRows_.indexOf(row));
    };
    connect(edit, &QLineEdit::textChanged, this, [this]() {
        if (!suppressTargetLanguageDirty_)
            emit dirtyChanged();
    });

    cell->addWidget(edit);
    layout->addWidget(cell);
    targetLanguagesBox_->registerRow(row, {cell});
    targetLanguagesBoxLayout_->addWidget(row);
    targetLanguageRows_.append(row);
    targetLanguageEdits_.append(edit);
    updateTargetLanguageRowState();
}

void ProjectBuildPage::setTargetLanguages(const QStringList& value)
{
    targetLanguagesBox_->clearSelection();
    const bool previous = suppressTargetLanguageDirty_;
    suppressTargetLanguageDirty_ = true;
    clearTargetLanguageRows();
    for (const QString& language : value)
        appendTargetLanguageRow(language);
    selectTargetLanguageRow(-1);
    suppressTargetLanguageDirty_ = previous;
}

void ProjectBuildPage::selectTargetLanguageRow(int index)
{
    if (index < 0 || index >= targetLanguageRows_.size())
        selectedTargetLanguageRow_ = -1;
    else
        selectedTargetLanguageRow_ = index;
    updateTargetLanguageRowState();
}

void ProjectBuildPage::updateTargetLanguageRowState()
{
    for (int i = 0; i < targetLanguageRows_.size(); ++i)
    {
        auto* row = targetLanguageRows_.at(i);
        auto* edit = targetLanguageEdits_.at(i);
        row->setObjectName(QStringLiteral("targetLanguageRow_%1").arg(i));
        row->setProperty("languageIndex", i);
        row->setProperty("selected", i == selectedTargetLanguageRow_);
        row->setProperty("lastRow", i == targetLanguageRows_.size() - 1);
        edit->setObjectName(QStringLiteral("targetLanguageEdit_%1").arg(i));
        edit->setProperty("languageIndex", i);
        refreshStyle(row);
    }

    if (targetLanguageRemoveBtn_)
        targetLanguageRemoveBtn_->setEnabled(selectedTargetLanguageRow_ >= 0 && targetLanguageRows_.size() > 1);
}

void ProjectBuildPage::onAddTargetLanguageClicked()
{
    appendTargetLanguageRow(QString());
    selectTargetLanguageRow(targetLanguageRows_.size() - 1);
    if (!targetLanguageEdits_.isEmpty())
        targetLanguageEdits_.last()->setFocus();
    if (!suppressTargetLanguageDirty_)
        emit dirtyChanged();
}

void ProjectBuildPage::onRemoveTargetLanguageClicked()
{
    if (selectedTargetLanguageRow_ < 0 || selectedTargetLanguageRow_ >= targetLanguageRows_.size()
        || targetLanguageRows_.size() <= 1)
        return;

    targetLanguagesBox_->removeSelectedRow([this] {
        auto* row = targetLanguageRows_.takeAt(selectedTargetLanguageRow_);
        targetLanguageEdits_.removeAt(selectedTargetLanguageRow_);
        targetLanguagesBoxLayout_->removeWidget(row);
        delete row;

        selectedTargetLanguageRow_ = -1;

        updateTargetLanguageRowState();
        if (!suppressTargetLanguageDirty_)
            emit dirtyChanged();
        return true;
    });
}

void ProjectBuildPage::load()
{
    auto& pm = ProjectManager::instance();
    if (projectLease_.lock() != pm.currentProjectLease())
        static_cast<TableWidget*>(platformList_->parentWidget())->clearSelection();
    draftPlatforms_.clear();
    projectLease_ = pm.currentProjectLease();
    QStringList projectLanguages;
    QString packVal;
    if (pm.isProjectOpen())
    {
        draftPlatforms_ = pm.currentProject()->platforms;
        projectLanguages = ProjectLanguageSemantics::buildLanguages(*pm.currentProject());
        packVal = pm.packBuildOutputDir();
    }

    refreshPlatforms();

    {
        const QSignalBlocker blocker(packBuildOutputDirEdit_);
        packBuildOutputDirEdit_->setText(packVal);
    }

    setTargetLanguages(projectLanguages);
    rememberBaseline();
}

bool ProjectBuildPage::applyTargetLanguages()
{
    auto& pm = ProjectManager::instance();
    if (!pm.isProjectOpen())
        return true;

    const auto normalized = ProjectLanguageSemantics::normalize(targetLanguages());
    if (!normalized.ok())
    {
        QMessageBox::warning(this, tr("Target Languages"),
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
        QMessageBox::warning(this, tr("Target Languages"),
                             result.errorMessage.isEmpty()
                                 ? tr("Failed to update target languages.")
                                 : result.errorMessage);
        return false;
    }

    setTargetLanguages(normalized.value);
    return true;
}

bool ProjectBuildPage::applyTargetPlatforms()
{
    if (platformDraft() == baselinePlatforms_) return true;
    const auto result = ActionManager::instance().execute(QStringLiteral("apply_project_platforms"),
        {{QStringLiteral("platforms"), platformDraft()}, {QStringLiteral("expected_platforms"), baselinePlatforms_}},
        ActionSource::GUI);
    if (!result.success) {
        QMessageBox::warning(this, tr("Project Platforms"), result.errorMessage);
        return false;
    }
    draftPlatforms_ = ProjectManager::instance().currentProject()->platforms;
    refreshPlatforms();
    return true;
}

bool ProjectBuildPage::applyPackBuildOutputDir()
{
    auto& pm = ProjectManager::instance();
    if (!pm.isProjectOpen())
        return true;

    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_pack_build_output_dir"),
        QJsonObject{{QStringLiteral("path"), packBuildOutputDirEdit_->text()}},
        ActionSource::GUI);
    if (!result.success)
    {
        QMessageBox::warning(this, tr("Pack Build Output"), result.errorMessage);
        return false;
    }
    packBuildOutputDirEdit_->setText(pm.packBuildOutputDir());
    return true;
}

bool ProjectBuildPage::apply()
{
    const auto project = projectLease_.lock();
    if (!project || ProjectManager::instance().currentProject() != project.get()
        || ProjectPlatformSemantics::platformsToJson(project->platforms) != baselinePlatforms_) {
        QMessageBox conflict(QMessageBox::Warning, tr("Build Settings Changed"),
            tr("The project or its platforms changed. Reload build settings and discard your unapplied changes?"),
            QMessageBox::Cancel, this);
        auto* reload = conflict.addButton(tr("Reload"), QMessageBox::AcceptRole);
        conflict.setDefaultButton(QMessageBox::Cancel);
        conflict.exec();
        if (conflict.clickedButton() == reload) {
            load();
            emit dirtyChanged();
        }
        return false;
    }
    const auto languages = ProjectLanguageSemantics::normalize(targetLanguages());
    if (!languages.ok() || !PathUtils::isValidPackBuildOutputDir(packBuildOutputDirEdit_->text(), project->projectPath)) {
        QMessageBox::warning(this, tr("Build Settings"), languages.ok()
            ? tr("Pack output must use a relative directory path. ")
                + PathUtils::packBuildOutputLocationError() : languages.error);
        return false;
    }
    if (!applyTargetPlatforms())
        return false;
    if (!applyTargetLanguages())
        return false;
    if (!applyPackBuildOutputDir())
        return false;
    rememberBaseline();
    emit dirtyChanged();
    return true;
}

bool ProjectBuildPage::isDirty() const
{
    return platformDraft() != baselinePlatforms_
        || targetLanguages() != baselineProjectLanguages_
        || packBuildOutputDirEdit_->text() != baselinePackBuildOutputDir_;
}

void ProjectBuildPage::onPackBuildOutputDirEditingFinished()
{
    const QString normalized = PathUtils::normalizePackBuildOutputDir(packBuildOutputDirEdit_->text());
    if (normalized != packBuildOutputDirEdit_->text())
        packBuildOutputDirEdit_->setText(normalized);
}

void ProjectBuildPage::onPackBuildOutputDirBrowseClicked()
{
    auto& pm = ProjectManager::instance();
    if (!pm.isProjectOpen())
        return;
    const QString projectRoot = QFileInfo(pm.currentProject()->projectPath).absolutePath();
    const QString relative = PathUtils::normalizePackBuildOutputDir(packBuildOutputDirEdit_->text());
    const QString startDir = PathUtils::isValidPackBuildOutputDir(relative, pm.currentProject()->projectPath)
        ? PathUtils::toAbsolutePath(relative.isEmpty() ? PathUtils::buildPacksDirName() : relative,
                                    projectRoot)
        : projectRoot;

    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Pack Build Output Directory"), startDir);
    if (dir.isEmpty())
        return;

    const QString rel = QDir(projectRoot).relativeFilePath(dir);

    if (!PathUtils::isValidPackBuildOutputDir(rel, pm.currentProject()->projectPath))
    {
        QMessageBox::warning(this, tr("Pack Build Output"),
                             tr("Pack output must use a relative directory path. ")
                                 + PathUtils::packBuildOutputLocationError());
        return;
    }

    packBuildOutputDirEdit_->setText(PathUtils::normalizePackBuildOutputDir(rel));
}

}
