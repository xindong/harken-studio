#include "ProjectIssue.h"

namespace VNX {

QString projectIssueCode(ProjectIssue issue)
{
    switch (issue) {
    case ProjectIssue::None: return {};
    case ProjectIssue::ProjectNotFound: return QStringLiteral("PROJECT_NOT_FOUND");
    case ProjectIssue::ProjectAmbiguous: return QStringLiteral("PROJECT_AMBIGUOUS");
    case ProjectIssue::ProjectHasNoFormalBaseline:
        return QStringLiteral("PROJECT_HAS_NO_FORMAL_BASELINE");
    case ProjectIssue::ProjectFileDamaged: return QStringLiteral("PROJECT_FILE_DAMAGED");
    case ProjectIssue::ProjectUpgradeSelectionRequired:
        return QStringLiteral("PROJECT_UPGRADE_SELECTION_REQUIRED");
    case ProjectIssue::ProjectChangedDuringUpgrade:
        return QStringLiteral("PROJECT_CHANGED_DURING_UPGRADE");
    case ProjectIssue::ProjectSchemaStateMismatch:
        return QStringLiteral("PROJECT_SCHEMA_STATE_MISMATCH");
    case ProjectIssue::SchemaArchiveDamaged:
        return QStringLiteral("SCHEMA_ARCHIVE_DAMAGED");
    case ProjectIssue::ProjectVolumeUnsupported:
        return QStringLiteral("PROJECT_VOLUME_UNSUPPORTED");
    case ProjectIssue::ProjectRecoveryVolumeUnavailable:
        return QStringLiteral("PROJECT_RECOVERY_VOLUME_UNAVAILABLE");
    case ProjectIssue::ProjectUpgradeSpaceInsufficient:
        return QStringLiteral("PROJECT_UPGRADE_SPACE_INSUFFICIENT");
    case ProjectIssue::ProjectUpgradeRecoveryRequired:
        return QStringLiteral("PROJECT_UPGRADE_RECOVERY_REQUIRED");
    case ProjectIssue::ProjectUpgradeIncompleteElsewhere:
        return QStringLiteral("PROJECT_UPGRADE_INCOMPLETE_ELSEWHERE");
    case ProjectIssue::ProjectInUse: return QStringLiteral("PROJECT_IN_USE");
    case ProjectIssue::MigrationMissing: return QStringLiteral("PROJECT_MIGRATION_MISSING");
    case ProjectIssue::ProjectRequiresNewerStudio:
        return QStringLiteral("PROJECT_REQUIRES_NEWER_STUDIO");
    case ProjectIssue::ProjectSchemaUnsupported:
        return QStringLiteral("PROJECT_SCHEMA_UNSUPPORTED");
    case ProjectIssue::InvalidPlaybackReference:
        return QStringLiteral("PROJECT_INVALID_PLAYBACK_REFERENCE");
    case ProjectIssue::ProjectUpgradeFilesNotWritable:
        return QStringLiteral("PROJECT_UPGRADE_FILES_NOT_WRITABLE");
    case ProjectIssue::InternalFailure: return QStringLiteral("PROJECT_INTERNAL_FAILURE");
    }
    return QStringLiteral("PROJECT_INTERNAL_FAILURE");
}

ProjectIssue projectIssueFromCode(const QString& code)
{
    for (int value = static_cast<int>(ProjectIssue::None);
         value <= static_cast<int>(ProjectIssue::InternalFailure); ++value) {
        const auto issue = static_cast<ProjectIssue>(value);
        if (projectIssueCode(issue) == code)
            return issue;
    }
    return ProjectIssue::InternalFailure;
}

}
