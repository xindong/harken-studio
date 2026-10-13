#pragma once

#include <QString>

namespace VNX {

enum class ProjectIssue
{
    None,
    ProjectNotFound,
    ProjectAmbiguous,
    ProjectHasNoFormalBaseline,
    ProjectFileDamaged,
    ProjectUpgradeSelectionRequired,
    ProjectChangedDuringUpgrade,
    ProjectSchemaStateMismatch,
    SchemaArchiveDamaged,
    ProjectVolumeUnsupported,
    ProjectRecoveryVolumeUnavailable,
    ProjectUpgradeSpaceInsufficient,
    ProjectUpgradeRecoveryRequired,
    ProjectUpgradeIncompleteElsewhere,
    ProjectInUse,
    MigrationMissing,
    ProjectRequiresNewerStudio,
    ProjectSchemaUnsupported,
    InvalidPlaybackReference,
    InternalFailure,
};

QString projectIssueCode(ProjectIssue issue);
ProjectIssue projectIssueFromCode(const QString& code);

}
