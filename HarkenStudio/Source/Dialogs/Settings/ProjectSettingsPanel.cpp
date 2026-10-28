
#include "Dialogs/Settings/ProjectSettingsPanel.h"

#include "Dialogs/Settings/Pages/ProjectConfigPage.h"
#include "Dialogs/Settings/Pages/ProjectPublishPage.h"


namespace VNX {

ProjectSettingsPanel::ProjectSettingsPanel(QWidget* parent)
    : SettingsPanel(parent)
{
    setObjectName(QStringLiteral("ProjectSettingsPanel"));
    setWindowTitle(tr("Project Settings"));
    configPage_ = new ProjectConfigPage(this);
    publishPage_ = new ProjectPublishPage(this);
    addPage(configPage_);
    addPage(publishPage_);
    loadPages();
}

void ProjectSettingsPanel::finishEditing()
{
    configPage_->finishEditing();
    publishPage_->finishEditing();
}

}
