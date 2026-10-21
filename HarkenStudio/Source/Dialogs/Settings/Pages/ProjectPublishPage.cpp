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

    auto* packGroup = makeSection(tr("Pack Build Output"));
    ContextInfo::tag(packGroup, ContextTopic::forPanel(QStringLiteral("settings.publish.output")));
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
            this, &ProjectPublishPage::onPackBuildOutputDirEditingFinished);
    connect(packBuildOutputDirBrowseBtn_, &QPushButton::clicked,
            this, &ProjectPublishPage::onPackBuildOutputDirBrowseClicked);

    auto* waveformGroup = makeSection(tr("Waveform Export"));
    ContextInfo::tag(waveformGroup, ContextTopic::forPanel(QStringLiteral("settings.publish.waveforms")));
    auto* waveformForm = makeForm();
    buildExportWaveformsCheckBox_ = new QCheckBox();
    buildExportWaveformsCheckBox_->setObjectName(QStringLiteral("buildExportWaveformsCheckBox"));
    waveformForm->addRow(tr("Export waveform preview data"), buildExportWaveformsCheckBox_);

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
    auto* waveformDirectoryLabel = new QLabel(tr("Relative Directory:"));
    waveformDirectoryLabel->setEnabled(false);
    waveformForm->addRow(waveformDirectoryLabel, waveformOutputRow_);
    waveformOutputRow_->setEnabled(false);
    static_cast<QVBoxLayout*>(waveformGroup->layout())->addLayout(waveformForm);
    body->addWidget(waveformGroup);

    connect(buildExportWaveformsCheckBox_, &QCheckBox::toggled, this,
        [this, waveformDirectoryLabel](bool enabled) {
            waveformOutputRow_->setEnabled(enabled);
            waveformDirectoryLabel->setEnabled(enabled);
            emit dirtyChanged();
        });
    connect(waveformOutputDirEdit_, &QLineEdit::textChanged, this, [this] { emit dirtyChanged(); });
    connect(waveformOutputDirEdit_, &QLineEdit::editingFinished, this, [this] {
        waveformOutputDirEdit_->setText(PathUtils::normalizePackBuildOutputDir(waveformOutputDirEdit_->text()));
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
    buildExportWaveformsCheckBox_->setChecked(project && project->buildExportWaveforms);
    rememberBaseline();
}

bool ProjectPublishPage::hasExternalChanges() const
{
    const auto project = projectLease_.lock();
    return !project || ProjectManager::instance().currentProject() != project.get()
        || project->packBuildOutputDir != baselinePackBuildOutputDir_
        || project->buildExportWaveforms != baselineExportWaveforms_
        || project->buildWaveformOutputDir != baselineWaveformOutputDir_;
}

bool ProjectPublishPage::validate()
{
    const auto project = projectLease_.lock();
    if (!project) return false;
    if (!PathUtils::isValidPackBuildOutputDir(packBuildOutputDirEdit_->text(), project->projectPath)) {
        QMessageBox::warning(this, tr("Pack Build Output"),
            tr("Pack output must use a relative directory path. ") + PathUtils::packBuildOutputLocationError());
        return false;
    }
    if (!PathUtils::isValidWaveformBuildOutputDir(waveformOutputDirEdit_->text(), project->projectPath)) {
        QMessageBox::warning(this, tr("Waveform Export"),
            tr("Waveform output must use a relative directory outside the project root and object directories."));
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
        QMessageBox::warning(this, tr("Pack Build Output"), result.errorMessage);
        return false;
    }
    packBuildOutputDirEdit_->setText(pm.packBuildOutputDir());
    return true;
}

bool ProjectPublishPage::applyWaveformSettings()
{
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_build_waveform_settings"),
        QJsonObject{{QStringLiteral("enabled"), buildExportWaveformsCheckBox_->isChecked()},
            {QStringLiteral("path"), waveformOutputDirEdit_->text()}}, ActionSource::GUI);
    if (!result.success) {
        QMessageBox::warning(this, tr("Waveform Export"), result.errorMessage);
        return false;
    }
    waveformOutputDirEdit_->setText(ProjectManager::instance().currentProject()->buildWaveformOutputDir);
    return true;
}

bool ProjectPublishPage::apply()
{
    if (!applyPackBuildOutputDir() || !applyWaveformSettings()) return false;
    rememberBaseline();
    emit dirtyChanged();
    return true;
}

bool ProjectPublishPage::isDirty() const
{
    return packBuildOutputDirEdit_->text() != baselinePackBuildOutputDir_
        || buildExportWaveformsCheckBox_->isChecked() != baselineExportWaveforms_
        || waveformOutputDirEdit_->text() != baselineWaveformOutputDir_;
}

void ProjectPublishPage::onPackBuildOutputDirEditingFinished()
{
    const QString normalized = PathUtils::normalizePackBuildOutputDir(packBuildOutputDirEdit_->text());
    if (normalized != packBuildOutputDirEdit_->text())
        packBuildOutputDirEdit_->setText(normalized);
}

void ProjectPublishPage::onPackBuildOutputDirBrowseClicked()
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

void ProjectPublishPage::onWaveformOutputDirBrowseClicked()
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) return;
    const QString projectRoot = QFileInfo(project->projectPath).absolutePath();
    const QString relative = PathUtils::normalizePackBuildOutputDir(waveformOutputDirEdit_->text());
    const QString startDir = PathUtils::isValidWaveformBuildOutputDir(relative, project->projectPath)
        ? PathUtils::waveformBuildOutputRoot(project->projectPath, relative) : projectRoot;
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Select Waveform Output Directory"), startDir);
    if (directory.isEmpty()) return;
    const QString path = QDir(projectRoot).relativeFilePath(directory);
    if (!PathUtils::isValidWaveformBuildOutputDir(path, project->projectPath)) {
        QMessageBox::warning(this, tr("Waveform Export"),
            tr("Waveform output must use a relative directory outside the project root and object directories."));
        return;
    }
    waveformOutputDirEdit_->setText(PathUtils::normalizePackBuildOutputDir(path));
}

}
