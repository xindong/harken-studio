#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <memory>

class QCheckBox;
class QLineEdit;
class QPushButton;

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
    bool apply() override;
    bool isDirty() const override;
    bool hasExternalChanges() const;
    bool validate();

private:
    void rememberBaseline();
    bool applyPackBuildOutputDir();
    bool applyWaveformSettings();
    void onPackBuildOutputDirBrowseClicked();
    void onPackBuildOutputDirEditingFinished();
    void onWaveformOutputDirBrowseClicked();

    std::weak_ptr<ProjectData> projectLease_;
    QLineEdit* packBuildOutputDirEdit_ = nullptr;
    QPushButton* packBuildOutputDirBrowseBtn_ = nullptr;
    QCheckBox* buildExportWaveformsCheckBox_ = nullptr;
    QWidget* waveformOutputRow_ = nullptr;
    QLineEdit* waveformOutputDirEdit_ = nullptr;
    QPushButton* waveformOutputDirBrowseBtn_ = nullptr;

    QString baselinePackBuildOutputDir_;
    bool baselineExportWaveforms_ = false;
    QString baselineWaveformOutputDir_;
};

}
