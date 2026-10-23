
#include "Dialogs/Settings/ProjectSettingsDialog.h"

#include "Core/DialogManager.h"
#include "Dialogs/Settings/Pages/ProjectConfigPage.h"
#include "Dialogs/Settings/Pages/ProjectPublishPage.h"

#include <QMessageBox>

namespace VNX {

ProjectSettingsDialog::ProjectSettingsDialog(QWidget* parent)
    : SettingsShellDialog(parent)
{
    setWindowTitle(tr("Project Settings"));
    configPage_ = new ProjectConfigPage(this);
    publishPage_ = new ProjectPublishPage(this);
    addPage(configPage_);
    addPage(publishPage_);
    loadPages();
    DialogManager::instance().registerDialog(QStringLiteral("project_settings"), this);
}

bool ProjectSettingsDialog::validateBeforeApply()
{
    if (configPage_->hasExternalChanges() || publishPage_->hasExternalChanges()) {
        QMessageBox conflict(QMessageBox::Warning, tr("Project Settings Changed"),
            tr("The project or its settings changed. Reload project settings and discard your unapplied changes?"),
            QMessageBox::Cancel, this);
        auto* reload = conflict.addButton(tr("Reload"), QMessageBox::AcceptRole);
        conflict.setDefaultButton(QMessageBox::Cancel);
        conflict.exec();
        if (conflict.clickedButton() == reload) loadPages();
        return false;
    }
    if (!configPage_->validate()) {
        showPage(QString::fromLatin1(kSettingsPageConfig));
        return false;
    }
    if (!publishPage_->validate()) {
        showPage(QString::fromLatin1(kSettingsPagePublish));
        return false;
    }
    return true;
}

}
