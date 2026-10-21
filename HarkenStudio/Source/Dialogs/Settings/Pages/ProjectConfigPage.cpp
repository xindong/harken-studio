#include "Dialogs/Settings/Pages/ProjectConfigPage.h"

#include "Core/ActionManager.h"
#include "Core/ContextInfo.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"

#include "Dialogs/Settings/ProjectPlatformDialog.h"
#include "Dialogs/Settings/ProjectPlatformEditor.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include <QListWidget>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <algorithm>

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
            this, &ProjectConfigPage::onAddTargetLanguageClicked);
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
    for (QLineEdit* edit : targetLanguageEdits_)
        languages << edit->text();
    return languages;
}

void ProjectConfigPage::rememberBaseline()
{
    baselinePlatforms_ = platformDraft();
    baselineProjectLanguages_ = targetLanguages();
}

void ProjectConfigPage::clearTargetLanguageRows()
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

void ProjectConfigPage::appendTargetLanguageRow(const QString& value)
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

void ProjectConfigPage::setTargetLanguages(const QStringList& value)
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

void ProjectConfigPage::selectTargetLanguageRow(int index)
{
    if (index < 0 || index >= targetLanguageRows_.size())
        selectedTargetLanguageRow_ = -1;
    else
        selectedTargetLanguageRow_ = index;
    updateTargetLanguageRowState();
}

void ProjectConfigPage::updateTargetLanguageRowState()
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
        targetLanguageRemoveBtn_->setEnabled(selectedTargetLanguageRow_ >= 0);
}

void ProjectConfigPage::onAddTargetLanguageClicked()
{
    appendTargetLanguageRow(QString());
    selectTargetLanguageRow(targetLanguageRows_.size() - 1);
    if (!targetLanguageEdits_.isEmpty())
        targetLanguageEdits_.last()->setFocus();
    if (!suppressTargetLanguageDirty_)
        emit dirtyChanged();
}

void ProjectConfigPage::onRemoveTargetLanguageClicked()
{
    if (selectedTargetLanguageRow_ < 0 || selectedTargetLanguageRow_ >= targetLanguageRows_.size())
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
