
#pragma once

#include "Dialogs/Settings/SettingsPanel.h"

namespace VNX {

class ProjectConfigPage;
class ProjectPublishPage;

class ProjectSettingsPanel : public SettingsPanel
{
    Q_OBJECT

public:
    explicit ProjectSettingsPanel(QWidget* parent = nullptr);
    void finishEditing();

private:
    ProjectConfigPage* configPage_ = nullptr;
    ProjectPublishPage* publishPage_ = nullptr;
};

}
