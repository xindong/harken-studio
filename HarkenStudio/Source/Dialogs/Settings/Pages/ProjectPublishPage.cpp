#include "Dialogs/Settings/Pages/ProjectPublishPage.h"

#include "Core/ActionManager.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/ContextInfo.h"
#include "Core/ProjectManager.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace VNX {

ProjectPublishPage::ProjectPublishPage(QWidget* parent)
    : SettingsPage(parent)
{
    auto* body = makePageBody();

    auto* form = makeForm();
    auto* dirLabel = new QLabel(tr("Export Directory:"));

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

    connect(packBuildOutputDirEdit_, &QLineEdit::textChanged, this, [this]() {
        emit dirtyChanged();
    });
    connect(packBuildOutputDirEdit_, &QLineEdit::editingFinished,
            this, &ProjectPublishPage::onPackBuildOutputDirEditingFinished);
    connect(packBuildOutputDirBrowseBtn_, &QPushButton::clicked,
            this, &ProjectPublishPage::onPackBuildOutputDirBrowseClicked);
    connect(buildExportProjectDataCheckBox_, &QCheckBox::toggled, this,
        [this] { emit dirtyChanged(); });
    connect(buildExportPackMetadataCheckBox_, &QCheckBox::toggled, this,
        [this] { emit dirtyChanged(); });

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
    waveformOutputDirEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[^*?<>|]*")), waveformOutputDirEdit_));
    waveformOutputDirBrowseBtn_ = new QPushButton(tr("Browse..."));
    waveformOutputDirBrowseBtn_->setObjectName(QStringLiteral("buildWaveformOutputDirBrowseBtn"));
    waveformRow->addWidget(waveformOutputDirEdit_, 1);
    waveformRow->addWidget(waveformOutputDirBrowseBtn_);
    auto* waveformDirectoryLabel = new QLabel(tr("Waveform File Directory:"));
    const auto waveformDirectoryTopic = ContextTopic::forPanel(QStringLiteral("settings.publish.waveform_directory"));
    ContextInfo::tag(waveformDirectoryLabel, waveformDirectoryTopic);
    ContextInfo::tag(waveformOutputRow_, waveformDirectoryTopic);
    waveformDirectoryLabel->setEnabled(false);
    materialForm->addRow(waveformDirectoryLabel, waveformOutputRow_);
    waveformOutputRow_->setEnabled(false);
    static_cast<QVBoxLayout*>(materialGroup->layout())->addLayout(materialForm);
    body->addWidget(materialGroup);

    connect(buildExportWaveformsCheckBox_, &QCheckBox::toggled, this,
        [this, waveformDirectoryLabel](bool enabled) {
            waveformOutputRow_->setEnabled(enabled);
            waveformDirectoryLabel->setEnabled(enabled);
            emit dirtyChanged();
        });
    connect(waveformOutputDirEdit_, &QLineEdit::textChanged, this, [this] { emit dirtyChanged(); });
    connect(waveformOutputDirEdit_, &QLineEdit::editingFinished, this, [this] {
        const auto project = projectLease_.lock();
        if (!project) return;
        waveformOutputDirEdit_->setText(PathUtils::normalizePackBuildOutputDir(
            waveformOutputDirEdit_->text(), project->projectPath));
    });
    connect(waveformOutputDirBrowseBtn_, &QPushButton::clicked,
        this, &ProjectPublishPage::onWaveformOutputDirBrowseClicked);

    body->addStretch();
}

QString ProjectPublishPage::pageId() const
{
    return QString::fromLatin1(kSettingsPagePublish);
}

QString ProjectPublishPage::categoryLabel() const
{
    return tr("Publish");
}

void ProjectPublishPage::rememberBaseline()
{
    baselinePackBuildOutputDir_ = packBuildOutputDirEdit_->text();
    baselineExportProjectData_ = buildExportProjectDataCheckBox_->isChecked();
    baselineExportPackMetadata_ = buildExportPackMetadataCheckBox_->isChecked();
    baselineExportWaveforms_ = buildExportWaveformsCheckBox_->isChecked();
    baselineWaveformOutputDir_ = waveformOutputDirEdit_->text();
}

void ProjectPublishPage::load()
{
    auto& pm = ProjectManager::instance();
    projectLease_ = pm.currentProjectLease();
    const auto project = projectLease_.lock();
    {
        const QSignalBlocker blocker(packBuildOutputDirEdit_);
        packBuildOutputDirEdit_->setText(project ? project->packBuildOutputDir : QString());
    }
    {
        const QSignalBlocker blocker(waveformOutputDirEdit_);
        waveformOutputDirEdit_->setText(project ? project->buildWaveformOutputDir : QString());
    }
    buildExportProjectDataCheckBox_->setChecked(project && project->buildExportProjectData);
    buildExportPackMetadataCheckBox_->setChecked(project && project->buildExportPackMetadata);
    buildExportWaveformsCheckBox_->setChecked(project && project->buildExportWaveforms);
    rememberBaseline();
}

bool ProjectPublishPage::hasExternalChanges() const
{
    const auto project = projectLease_.lock();
    return !project || ProjectManager::instance().currentProject() != project.get()
        || project->packBuildOutputDir != baselinePackBuildOutputDir_
        || project->buildExportProjectData != baselineExportProjectData_
        || project->buildExportPackMetadata != baselineExportPackMetadata_
        || project->buildExportWaveforms != baselineExportWaveforms_
        || project->buildWaveformOutputDir != baselineWaveformOutputDir_;
}

bool ProjectPublishPage::validate()
{
    const auto project = projectLease_.lock();
    if (!project) return false;
    if (!PathUtils::isValidPackBuildOutputDir(packBuildOutputDirEdit_->text(), project->projectPath)) {
        QMessageBox::warning(this, tr("Export Directory"),
            tr("Choose a valid relative or absolute export directory. ") + PathUtils::packBuildOutputLocationError());
        return false;
    }
    if (!PathUtils::isValidWaveformBuildOutputDir(waveformOutputDirEdit_->text(), project->projectPath)) {
        QMessageBox::warning(this, tr("Waveform File Directory"),
            tr("Waveform output must use a valid directory outside the project root and object directories."));
        return false;
    }
    return true;
}

bool ProjectPublishPage::applyPackBuildOutputDir()
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
        QMessageBox::warning(this, tr("Export Directory"), result.errorMessage);
        return false;
    }
    packBuildOutputDirEdit_->setText(pm.packBuildOutputDir());
    return true;
}

bool ProjectPublishPage::applyProjectDataExport()
{
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_build_project_data_export"),
        QJsonObject{{QStringLiteral("enabled"), buildExportProjectDataCheckBox_->isChecked()}}, ActionSource::GUI);
    if (!result.success) {
        QMessageBox::warning(this, tr("Export Project Information"), result.errorMessage);
        return false;
    }
    return true;
}

bool ProjectPublishPage::applyPackMetadataExport()
{
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_build_pack_metadata_export"),
        QJsonObject{{QStringLiteral("enabled"), buildExportPackMetadataCheckBox_->isChecked()}}, ActionSource::GUI);
    if (!result.success) {
        QMessageBox::warning(this, tr("Export Pack MetaData"), result.errorMessage);
        return false;
    }
    return true;
}

bool ProjectPublishPage::applyWaveformSettings()
{
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_build_waveform_settings"),
        QJsonObject{{QStringLiteral("enabled"), buildExportWaveformsCheckBox_->isChecked()},
            {QStringLiteral("path"), waveformOutputDirEdit_->text()}}, ActionSource::GUI);
    if (!result.success) {
        QMessageBox::warning(this, tr("Export Wave Buffer"), result.errorMessage);
        return false;
    }
    waveformOutputDirEdit_->setText(ProjectManager::instance().currentProject()->buildWaveformOutputDir);
    return true;
}

bool ProjectPublishPage::apply()
{
    if (!applyPackBuildOutputDir() || !applyProjectDataExport()
        || !applyPackMetadataExport() || !applyWaveformSettings()) return false;
    rememberBaseline();
    emit dirtyChanged();
    return true;
}

bool ProjectPublishPage::isDirty() const
{
    return packBuildOutputDirEdit_->text() != baselinePackBuildOutputDir_
        || buildExportProjectDataCheckBox_->isChecked() != baselineExportProjectData_
        || buildExportPackMetadataCheckBox_->isChecked() != baselineExportPackMetadata_
        || buildExportWaveformsCheckBox_->isChecked() != baselineExportWaveforms_
        || waveformOutputDirEdit_->text() != baselineWaveformOutputDir_;
}

void ProjectPublishPage::onPackBuildOutputDirEditingFinished()
{
    const auto project = projectLease_.lock();
    if (!project) return;
    const QString normalized = PathUtils::normalizePackBuildOutputDir(
        packBuildOutputDirEdit_->text(), project->projectPath);
    if (normalized != packBuildOutputDirEdit_->text())
        packBuildOutputDirEdit_->setText(normalized);
}

void ProjectPublishPage::onPackBuildOutputDirBrowseClicked()
{
    auto& pm = ProjectManager::instance();
    if (!pm.isProjectOpen())
        return;
    const QString projectRoot = QFileInfo(pm.currentProject()->projectPath).absolutePath();
    const QString path = PathUtils::normalizePackBuildOutputDir(
        packBuildOutputDirEdit_->text(), pm.currentProject()->projectPath);
    const QString startDir = PathUtils::isValidPackBuildOutputDir(path, pm.currentProject()->projectPath)
        ? PathUtils::packBuildOutputRoot(pm.currentProject()->projectPath, path)
        : projectRoot;

    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Export Directory"), startDir);
    if (dir.isEmpty())
        return;

    const QString selectedPath = PathUtils::normalizePackBuildOutputDir(dir, pm.currentProject()->projectPath);

    if (!PathUtils::isValidPackBuildOutputDir(selectedPath, pm.currentProject()->projectPath))
    {
        QMessageBox::warning(this, tr("Export Directory"),
                             tr("Choose a valid relative or absolute export directory. ")
                                 + PathUtils::packBuildOutputLocationError());
        return;
    }

    packBuildOutputDirEdit_->setText(selectedPath);
}

void ProjectPublishPage::onWaveformOutputDirBrowseClicked()
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) return;
    const QString projectRoot = QFileInfo(project->projectPath).absolutePath();
    const QString path = PathUtils::normalizePackBuildOutputDir(waveformOutputDirEdit_->text(), project->projectPath);
    const QString startDir = PathUtils::isValidWaveformBuildOutputDir(path, project->projectPath)
        ? PathUtils::waveformBuildOutputRoot(project->projectPath, path) : projectRoot;
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Select Waveform File Directory"), startDir);
    if (directory.isEmpty()) return;
    const QString selectedPath = PathUtils::normalizePackBuildOutputDir(directory, project->projectPath);
    if (!PathUtils::isValidWaveformBuildOutputDir(selectedPath, project->projectPath)) {
        QMessageBox::warning(this, tr("Waveform File Directory"),
            tr("Waveform output must use a valid directory outside the project root and object directories."));
        return;
    }
    waveformOutputDirEdit_->setText(selectedPath);
}

}
