#include "Dialogs/Settings/Pages/ProjectPublishPage.h"

#include "Core/ActionManager.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ContextInfo.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QVBoxLayout>

namespace VNX {

ProjectPublishPage::ProjectPublishPage(QWidget* parent)
    : SettingsPage(parent)
{
    auto* body = makePageBody();

    auto* form = makeForm();
    auto* dirLabel = new QLabel(tr("Publish Directory:"));

    auto* rowWidget = new QWidget();
    auto* row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    packBuildOutputDirEdit_ = new QLineEdit();
    packBuildOutputDirEdit_->setObjectName("packBuildOutputDirEdit");
    packBuildOutputDirEdit_->setPlaceholderText(PathUtils::buildPacksDirName() + '/');
    packBuildOutputDirBrowseBtn_ = new QPushButton(tr("Browse..."));
    packBuildOutputDirBrowseBtn_->setObjectName("packBuildOutputDirBrowseBtn");
    row->addWidget(packBuildOutputDirEdit_, 1);
    row->addWidget(packBuildOutputDirBrowseBtn_, 0);
    form->addRow(dirLabel, rowWidget);
    const auto outputTopic = ContextTopic::forPanel(QStringLiteral("settings.publish.output"));
    ContextInfo::tag(dirLabel, outputTopic);
    ContextInfo::tag(rowWidget, outputTopic);
    body->addLayout(form);

    auto* materialGroup = makeSection(tr("Additional Material"));
    ContextInfo::tag(materialGroup,
        ContextTopic::forPanel(QStringLiteral("settings.publish.additional_material")));
    auto* materialForm = makeForm();
    buildExportProjectDataCheckBox_ = new QCheckBox();
    buildExportProjectDataCheckBox_->setObjectName(QStringLiteral("buildExportProjectDataCheckBox"));
    auto* projectDataLabel = new QLabel(tr("Export Project Information"));
    materialForm->addRow(projectDataLabel, buildExportProjectDataCheckBox_);
    const auto projectDataTopic = ContextTopic::forPanel(QStringLiteral("settings.publish.project_information"));
    ContextInfo::tag(projectDataLabel, projectDataTopic);
    ContextInfo::tag(buildExportProjectDataCheckBox_, projectDataTopic);
    buildExportPackMetadataCheckBox_ = new QCheckBox();
    buildExportPackMetadataCheckBox_->setObjectName(QStringLiteral("buildExportPackMetadataCheckBox"));
    auto* packMetadataLabel = new QLabel(tr("Export Pack MetaData"));
    materialForm->addRow(packMetadataLabel, buildExportPackMetadataCheckBox_);
    const auto packMetadataTopic = ContextTopic::forPanel(QStringLiteral("settings.publish.pack_metadata"));
    ContextInfo::tag(packMetadataLabel, packMetadataTopic);
    ContextInfo::tag(buildExportPackMetadataCheckBox_, packMetadataTopic);

    connect(packBuildOutputDirEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const auto project = projectLease_.lock();
        const bool valid = project && PathUtils::isValidPackBuildOutputDir(text, project->projectPath);
        QtControlSkin::setSemanticProperty(packBuildOutputDirEdit_, "inputInvalid", !valid);
        packBuildOutputDirEdit_->setToolTip(valid ? QString() : PathUtils::packBuildOutputLocationError());
    });
    connect(packBuildOutputDirEdit_, &QLineEdit::editingFinished,
            this, &ProjectPublishPage::onPackBuildOutputDirEditingFinished);
    connect(packBuildOutputDirBrowseBtn_, &QPushButton::clicked,
            this, &ProjectPublishPage::onPackBuildOutputDirBrowseClicked);
    connect(buildExportProjectDataCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        commit(QStringLiteral("set_build_project_data_export"), {{QStringLiteral("enabled"), enabled}});
    });
    connect(buildExportPackMetadataCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        commit(QStringLiteral("set_build_pack_metadata_export"), {{QStringLiteral("enabled"), enabled}});
    });

    buildExportWaveformsCheckBox_ = new QCheckBox();
    buildExportWaveformsCheckBox_->setObjectName(QStringLiteral("buildExportWaveformsCheckBox"));
    auto* waveformsLabel = new QLabel(tr("Export Wave Buffer"));
    materialForm->addRow(waveformsLabel, buildExportWaveformsCheckBox_);
    const auto waveformsTopic = ContextTopic::forPanel(QStringLiteral("settings.publish.waveforms"));
    ContextInfo::tag(waveformsLabel, waveformsTopic);
    ContextInfo::tag(buildExportWaveformsCheckBox_, waveformsTopic);

    waveformOutputRow_ = new QWidget();
    auto* waveformRow = new QHBoxLayout(waveformOutputRow_);
    waveformRow->setContentsMargins(0, 0, 0, 0);
    waveformRow->setSpacing(8);
    waveformOutputDirEdit_ = new QLineEdit();
    waveformOutputDirEdit_->setObjectName(QStringLiteral("buildWaveformOutputDirEdit"));
    waveformOutputDirEdit_->setPlaceholderText(PathUtils::buildPacksDirName() + QStringLiteral("/Waveforms/"));
    waveformOutputDirBrowseBtn_ = new QPushButton(tr("Browse..."));
    waveformOutputDirBrowseBtn_->setObjectName(QStringLiteral("buildWaveformOutputDirBrowseBtn"));
    waveformRow->addWidget(waveformOutputDirEdit_, 1);
    waveformRow->addWidget(waveformOutputDirBrowseBtn_);
    waveformDirectoryLabel_ = new QLabel(tr("Waveform File Directory:"));
    const auto waveformDirectoryTopic = ContextTopic::forPanel(QStringLiteral("settings.publish.waveform_directory"));
    ContextInfo::tag(waveformDirectoryLabel_, waveformDirectoryTopic);
    ContextInfo::tag(waveformOutputRow_, waveformDirectoryTopic);
    waveformDirectoryLabel_->setEnabled(false);
    materialForm->addRow(waveformDirectoryLabel_, waveformOutputRow_);
    waveformOutputRow_->setEnabled(false);
    static_cast<QVBoxLayout*>(materialGroup->layout())->addLayout(materialForm);
    body->addWidget(materialGroup);

    connect(buildExportWaveformsCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        const auto project = projectLease_.lock();
        if (!hasCurrentProject()) return;
        commit(QStringLiteral("set_build_waveform_settings"),
            {{QStringLiteral("enabled"), enabled}, {QStringLiteral("path"), project->buildWaveformOutputDir}});
    });
    connect(waveformOutputDirEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const auto project = projectLease_.lock();
        const bool valid = project && PathUtils::isValidWaveformBuildOutputDir(text, project->projectPath);
        QtControlSkin::setSemanticProperty(waveformOutputDirEdit_, "inputInvalid", !valid);
        waveformOutputDirEdit_->setToolTip(valid ? QString()
            : tr("Choose a directory outside the project root and object directories."));
    });
    connect(waveformOutputDirEdit_, &QLineEdit::editingFinished, this, &ProjectPublishPage::commitWaveformPath);
    connect(waveformOutputDirBrowseBtn_, &QPushButton::clicked,
        this, &ProjectPublishPage::onWaveformOutputDirBrowseClicked);

    body->addStretch();
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::projectOpened, this, &ProjectPublishPage::load);
    connect(&manager, &ProjectManager::projectClosed, this, &ProjectPublishPage::load);
    connect(&manager, &ProjectManager::projectPublishSettingsChanged, this, &ProjectPublishPage::sync);
}

QString ProjectPublishPage::pageId() const
{
    return QString::fromLatin1(kSettingsPagePublish);
}

QString ProjectPublishPage::categoryLabel() const
{
    return tr("Publish");
}

bool ProjectPublishPage::hasCurrentProject() const
{
    const auto project = projectLease_.lock();
    return project && ProjectManager::instance().currentProject() == project.get();
}

void ProjectPublishPage::load()
{
    packBuildOutputDirEdit_->setModified(false);
    waveformOutputDirEdit_->setModified(false);
    projectLease_ = ProjectManager::instance().currentProjectLease();
    sync();
    setEnabled(hasCurrentProject());
}

void ProjectPublishPage::finishEditing()
{
    if (packBuildOutputDirEdit_->isModified()) onPackBuildOutputDirEditingFinished();
    if (waveformOutputDirEdit_->isModified()) commitWaveformPath();
}

void ProjectPublishPage::hideEvent(QHideEvent* event)
{
    finishEditing();
    SettingsPage::hideEvent(event);
}

void ProjectPublishPage::sync()
{
    if (committing_) return;
    QScopedValueRollback guard(committing_, true);
    const auto project = projectLease_.lock();
    const bool available = hasCurrentProject();
    if (!available || !project->buildExportWaveforms)
        waveformOutputDirEdit_->setModified(false);
    if (!packBuildOutputDirEdit_->isModified()) {
        const QSignalBlocker blocker(packBuildOutputDirEdit_);
        baselinePackBuildOutputDir_ = available ? project->packBuildOutputDir : QString();
        packBuildOutputDirEdit_->setText(baselinePackBuildOutputDir_);
        packBuildOutputDirEdit_->setToolTip(QString());
        QtControlSkin::setSemanticProperty(packBuildOutputDirEdit_, "inputInvalid", false);
    }
    if (!waveformOutputDirEdit_->isModified()) {
        const QSignalBlocker blocker(waveformOutputDirEdit_);
        baselineWaveformOutputDir_ = available ? project->buildWaveformOutputDir : QString();
        waveformOutputDirEdit_->setText(baselineWaveformOutputDir_);
        waveformOutputDirEdit_->setToolTip(QString());
        QtControlSkin::setSemanticProperty(waveformOutputDirEdit_, "inputInvalid", false);
    }
    const QSignalBlocker projectDataBlocker(buildExportProjectDataCheckBox_);
    const QSignalBlocker metadataBlocker(buildExportPackMetadataCheckBox_);
    const QSignalBlocker waveformBlocker(buildExportWaveformsCheckBox_);
    buildExportProjectDataCheckBox_->setChecked(available && project->buildExportProjectData);
    buildExportPackMetadataCheckBox_->setChecked(available && project->buildExportPackMetadata);
    buildExportWaveformsCheckBox_->setChecked(available && project->buildExportWaveforms);
    waveformDirectoryLabel_->setEnabled(available && project->buildExportWaveforms);
    waveformOutputRow_->setEnabled(available && project->buildExportWaveforms);
}

void ProjectPublishPage::commit(const QString& command, const QJsonObject& args)
{
    if (committing_ || !hasCurrentProject()) return;
    ActionResult result;
    {
        QScopedValueRollback guard(committing_, true);
        result = ActionManager::instance().execute(command, args, ActionSource::GUI);
    }
    sync();
    if (!result.success) showError(result.errorMessage);
}

void ProjectPublishPage::onPackBuildOutputDirEditingFinished()
{
    if (committing_) return;
    const auto project = projectLease_.lock();
    const auto text = packBuildOutputDirEdit_->text();
    packBuildOutputDirEdit_->setModified(false);
    if (!hasCurrentProject()) { sync(); return; }
    if (text == baselinePackBuildOutputDir_) { sync(); return; }
    if (project->packBuildOutputDir != baselinePackBuildOutputDir_) {
        sync();
        showError(tr("The publish directory changed while you were editing. The current setting was restored."));
        return;
    }
    if (!PathUtils::isValidPackBuildOutputDir(text, project->projectPath)) { sync(); return; }
    commit(QStringLiteral("set_pack_build_output_dir"), {{QStringLiteral("path"), text}});
}

void ProjectPublishPage::commitWaveformPath()
{
    if (committing_) return;
    const auto project = projectLease_.lock();
    const auto text = waveformOutputDirEdit_->text();
    waveformOutputDirEdit_->setModified(false);
    if (!hasCurrentProject()) { sync(); return; }
    if (text == baselineWaveformOutputDir_) { sync(); return; }
    if (project->buildWaveformOutputDir != baselineWaveformOutputDir_) {
        sync();
        showError(tr("The waveform directory changed while you were editing. The current setting was restored."));
        return;
    }
    if (!PathUtils::isValidWaveformBuildOutputDir(text, project->projectPath)) { sync(); return; }
    commit(QStringLiteral("set_build_waveform_settings"),
        {{QStringLiteral("enabled"), project->buildExportWaveforms}, {QStringLiteral("path"), text}});
}

void ProjectPublishPage::onPackBuildOutputDirBrowseClicked()
{
    const auto project = projectLease_.lock();
    if (!hasCurrentProject()) return;
    const auto directory = QFileDialog::getExistingDirectory(this, tr("Select Publish Directory"),
        PathUtils::packBuildOutputRoot(project->projectPath, project->packBuildOutputDir));
    if (directory.isEmpty() || !hasCurrentProject() || project != projectLease_.lock()) return;
    packBuildOutputDirEdit_->setText(directory);
    onPackBuildOutputDirEditingFinished();
}

void ProjectPublishPage::onWaveformOutputDirBrowseClicked()
{
    const auto project = projectLease_.lock();
    if (!hasCurrentProject()) return;
    const auto directory = QFileDialog::getExistingDirectory(this, tr("Select Waveform File Directory"),
        PathUtils::waveformBuildOutputRoot(project->projectPath, project->buildWaveformOutputDir));
    if (directory.isEmpty() || !hasCurrentProject() || project != projectLease_.lock()) return;
    waveformOutputDirEdit_->setText(directory);
    commitWaveformPath();
}

}
