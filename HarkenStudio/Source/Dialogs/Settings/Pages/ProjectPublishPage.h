#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <memory>
#include <QJsonObject>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QLabel;

namespace VNX {

struct ProjectData;

inline constexpr const char* kSettingsPagePublish = "publish";

class ProjectPublishPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ProjectPublishPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    void finishEditing();

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void sync();
    bool hasCurrentProject() const;
    void commit(const QString& command, const QJsonObject& args);
    void commitWaveformPath();
    void onPackBuildOutputDirBrowseClicked();
    void onPackBuildOutputDirEditingFinished();
    void onWaveformOutputDirBrowseClicked();

    std::weak_ptr<ProjectData> projectLease_;
    QLineEdit* packBuildOutputDirEdit_ = nullptr;
    QPushButton* packBuildOutputDirBrowseBtn_ = nullptr;
    QCheckBox* buildExportProjectDataCheckBox_ = nullptr;
    QCheckBox* buildExportPackMetadataCheckBox_ = nullptr;
    QCheckBox* buildExportWaveformsCheckBox_ = nullptr;
    QWidget* waveformOutputRow_ = nullptr;
    QLabel* waveformDirectoryLabel_ = nullptr;
    QLineEdit* waveformOutputDirEdit_ = nullptr;
    QPushButton* waveformOutputDirBrowseBtn_ = nullptr;

    QString baselinePackBuildOutputDir_;
    QString baselineWaveformOutputDir_;
    bool committing_ = false;
};

}
