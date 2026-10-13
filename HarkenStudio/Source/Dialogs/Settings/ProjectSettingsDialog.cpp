
#include "Dialogs/Settings/ProjectSettingsDialog.h"

#include "Core/DialogManager.h"
#include "Dialogs/Settings/Pages/ProjectBuildPage.h"

namespace VNX {

ProjectSettingsDialog::ProjectSettingsDialog(QWidget* parent)
    : SettingsShellDialog(parent)
{
    setWindowTitle(tr("Project Settings"));
    addPage(new ProjectBuildPage(this));
    loadPages();
    DialogManager::instance().registerDialog(QStringLiteral("project_settings"), this);
}

}
