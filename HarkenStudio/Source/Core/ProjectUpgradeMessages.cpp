#include "ProjectUpgradeMessages.h"

#include "ProjectUpgradeManager.h"

#include <QCoreApplication>
#include <QtGlobal>

namespace VNX::ProjectUpgradeMessages {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

}

QString Presentation::primaryText() const
{
    if (reason.isEmpty())
        return nextStep;
    if (nextStep.isEmpty())
        return reason;
    return reason + QLatin1Char(' ') + nextStep;
}

Presentation presentation(ProjectIssue issue, const QString& detail)
{
    Presentation result;
    switch (issue) {
    case ProjectIssue::None:
        result.reason = detail;
        break;
    case ProjectIssue::ProjectNotFound:
        result.reason = hubText("Harken Studio could not find a .hsp project here.");
        result.nextStep = hubText("Choose the project file or its folder. No project was changed.");
        break;
    case ProjectIssue::ProjectAmbiguous:
        result.reason = hubText("This folder contains more than one .hsp project.");
        result.nextStep = hubText("Choose the project file you want to open. No project was changed.");
        break;
    case ProjectIssue::ProjectHasNoFormalBaseline:
        result.reason = hubText("This project was created before the first supported Harken project format.");
        result.nextStep = hubText("Open a project created with a supported Studio version. No project was changed.");
        break;
    case ProjectIssue::ProjectFileDamaged:
        result.reason = hubText("This project is incomplete or damaged.");
        result.nextStep = hubText("Restore a known good copy, then try again. No project was changed.");
        break;
    case ProjectIssue::ProjectUpgradeSelectionRequired:
        result.reason = hubText("This project needs an update before it can open.");
        result.nextStep = hubText("Open the project in Harken Studio to review and confirm the update. No project was changed.");
        break;
    case ProjectIssue::ProjectChangedDuringUpgrade:
        result.reason = hubText("The project changed while Harken Studio was preparing the update.");
        result.nextStep = hubText("Close other programs using it, then try again.");
        break;
    case ProjectIssue::ProjectSchemaStateMismatch:
        result.reason = hubText("The version written in this project does not match its files.");
        result.nextStep = hubText("Restore a backup or contact support. Do not edit the files by hand. No project was changed.");
        break;
    case ProjectIssue::SchemaArchiveDamaged:
        result.reason = hubText("This Harken Studio installation is incomplete.");
        result.nextStep = hubText("Reinstall Harken Studio, then try again. The project is not damaged and was not changed.");
        break;
    case ProjectIssue::ProjectVolumeUnsupported:
        result.reason = hubText("This project is stored on a network or cloud-synced location, where an interrupted update cannot be recovered safely.");
        result.nextStep = hubText("Move it to a local drive, USB drive, or external drive, then try again. No project was changed.");
        break;
    case ProjectIssue::ProjectRecoveryVolumeUnavailable:
        result.reason = hubText("Harken Studio cannot find a safe place on this computer for the recovery files needed to update this removable-drive project.");
        result.nextStep = hubText("Free space on the computer's internal drive or move the project to an internal drive, then try again. No project was changed.");
        break;
    case ProjectIssue::ProjectUpgradeSpaceInsufficient:
        result.reason = hubText("There is not enough free space to update this project safely.");
        result.nextStep = hubText("Free some space, then check again. No project was changed.");
        break;
    case ProjectIssue::ProjectUpgradeRecoveryRequired:
        result.reason = hubText("A previous project update did not finish.");
        result.nextStep = hubText("Keep the storage device connected while Harken Studio checks and restores the project.");
        break;
    case ProjectIssue::ProjectUpgradeIncompleteElsewhere:
        result.reason = hubText("This project update started on another computer and did not finish.");
        result.nextStep = hubText("Connect the drive to that computer to continue or restore it. This computer did not change the project.");
        break;
    case ProjectIssue::ProjectInUse:
        result.reason = hubText("Another program is using this project.");
        result.nextStep = hubText("Close the other Studio window or program, then try again. No project was changed.");
        break;
    case ProjectIssue::MigrationMissing:
        result.reason = hubText("This Harken Studio installation is missing a required project update step.");
        result.nextStep = hubText("Reinstall the latest version, then try again. No project was changed.");
        break;
    case ProjectIssue::ProjectRequiresNewerStudio:
        result.reason = hubText("This project uses a newer project format.");
        result.nextStep = hubText("Open it with a newer Harken Studio version. No project was changed.");
        break;
    case ProjectIssue::ProjectSchemaUnsupported:
        result.reason = hubText("This project uses an older project format that this Harken Studio no longer supports.");
        result.nextStep = hubText("Open it with the Studio version that created it, or create a new project. No project was changed.");
        break;
    case ProjectIssue::InvalidPlaybackReference:
        result.reason = hubText("This project contains an invalid playback reference.");
        if (!detail.isEmpty()) result.reason += QLatin1Char('\n') + detail;
        result.nextStep = hubText("Restore a valid version of the project and try again. No project was changed.");
        break;
    case ProjectIssue::ProjectUpgradeFilesNotWritable:
        result.reason = hubText("Some files needed for the project upgrade are read-only or not writable.");
        result.nextStep = hubText("Check file write protection and access permissions, then try again. If your version control workflow requires checkout or unlocking, do that first. No project data was changed.");
        if (!detail.isEmpty())
            result.nextStep += QStringLiteral("\n\n") + hubText("Affected files:\n%1").arg(detail);
        break;
    case ProjectIssue::InternalFailure:
        result.reason = hubText("Harken Studio could not finish checking this project.");
        result.nextStep = hubText("Try again. If the problem continues, contact support. The project was not changed by this attempt.");
        break;
    }
    const QString code = projectIssueCode(issue);
    result.technicalDetails = code;
    const QString cleanDetail = detail.trimmed();
    if (!cleanDetail.isEmpty() && cleanDetail != code) {
        if (!result.technicalDetails.isEmpty())
            result.technicalDetails.append(QLatin1Char('\n'));
        result.technicalDetails.append(cleanDetail);
    }
    return result;
}

Presentation presentation(const QString& internalError, bool upgradeContext)
{
    const QString value = internalError.trimmed();
    const int separator = value.indexOf(QChar(':'));
    const QString code = separator < 0 ? value : value.left(separator);
    const ProjectIssue issue = projectIssueFromCode(code);
    if (projectIssueCode(issue) == code)
        return presentation(issue,
                            separator < 0 ? QString() : value.mid(separator + 1));
    if (upgradeContext) {
        Presentation result = presentation(ProjectIssue::InternalFailure);
        if (!value.isEmpty())
            result.technicalDetails.append(QLatin1Char('\n') + value);
        return result;
    }
    return {value, {}, {}};
}

QString userMessage(ProjectIssue issue, const QString& detail)
{
    return presentation(issue, detail).primaryText();
}

QString userMessage(const QString& internalError, bool upgradeContext)
{
    return presentation(internalError, upgradeContext).primaryText();
}

QString progressText(ProjectUpgradePhase phase)
{
    switch (phase) {
    case ProjectUpgradePhase::Analyzing:
        return hubText("Checking the project");
    case ProjectUpgradePhase::CopyingStaging:
        return hubText("Preparing the files that need to be updated");
    case ProjectUpgradePhase::Migrating:
        return hubText("Updating the project data");
    case ProjectUpgradePhase::ValidatingStaging:
        return hubText("Checking the updated files");
    case ProjectUpgradePhase::BackingUp:
        return hubText("Protecting the original project data");
    case ProjectUpgradePhase::Replacing:
        return hubText("Replacing the project files");
    case ProjectUpgradePhase::ValidatingProject:
        return hubText("Checking the updated project");
    case ProjectUpgradePhase::Committing:
        return hubText("Finishing the project update");
    case ProjectUpgradePhase::RollingBack:
        return hubText("Restoring the original project data");
    case ProjectUpgradePhase::Completed:
        return hubText("Project update complete");
    }
    return hubText("Updating the project");
}

int progressPercent(const ProjectUpgradeProgress& progress)
{
    int start = 0;
    int end = 0;
    switch (progress.phase) {
    case ProjectUpgradePhase::Analyzing: start = 0; end = 8; break;
    case ProjectUpgradePhase::CopyingStaging: start = 8; end = 34; break;
    case ProjectUpgradePhase::Migrating: start = 34; end = 56; break;
    case ProjectUpgradePhase::ValidatingStaging: start = 56; end = 64; break;
    case ProjectUpgradePhase::BackingUp: start = 64; end = 76; break;
    case ProjectUpgradePhase::Replacing: start = 76; end = 90; break;
    case ProjectUpgradePhase::ValidatingProject: start = 90; end = 96; break;
    case ProjectUpgradePhase::Committing: start = 96; end = 99; break;
    case ProjectUpgradePhase::RollingBack: start = 0; end = 99; break;
    case ProjectUpgradePhase::Completed: return 100;
    }
    qreal fraction = 0.0;
    if (progress.totalBytes > 0) {
        fraction = static_cast<qreal>(progress.completedBytes)
            / static_cast<qreal>(progress.totalBytes);
    } else if (progress.totalItems > 0) {
        fraction = static_cast<qreal>(progress.completedItems)
            / static_cast<qreal>(progress.totalItems);
    }
    fraction = qBound<qreal>(0.0, fraction, 1.0);
    return start + qRound((end - start) * fraction);
}

}
