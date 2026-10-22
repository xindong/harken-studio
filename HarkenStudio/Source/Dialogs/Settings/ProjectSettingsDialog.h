
#pragma once

#include "Dialogs/Settings/SettingsShellDialog.h"

namespace VNX {

class ProjectConfigPage;
class ProjectPublishPage;

class ProjectSettingsDialog : public SettingsShellDialog
{
    Q_OBJECT

public:
    explicit ProjectSettingsDialog(QWidget* parent = nullptr);

protected:
    bool validateBeforeApply() override;

private:
    ProjectConfigPage* configPage_ = nullptr;
    ProjectPublishPage* publishPage_ = nullptr;
};

}
