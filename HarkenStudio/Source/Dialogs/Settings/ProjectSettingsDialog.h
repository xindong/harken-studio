
#pragma once

#include "Dialogs/Settings/SettingsShellDialog.h"

namespace VNX {

class ProjectSettingsDialog : public SettingsShellDialog
{
    Q_OBJECT

public:
    explicit ProjectSettingsDialog(QWidget* parent = nullptr);
};

}
