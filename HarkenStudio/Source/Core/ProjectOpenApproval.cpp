#include "ProjectOpenApproval.h"

#include "ProjectOpenProtocol.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QUuid>

namespace VNX {

namespace {

QString text(const char* value)
{
    return QCoreApplication::translate("ProjectOpenApproval", value);
}

bool matches(const ProjectUpgradeAnalysis& analysis,
             const ProjectOpenApprovalBinding& binding)
{
    return analysis.ok && analysis.upgradeRequired && analysis.migrationPlan.ok
        && analysis.sourceSchema > 0 && analysis.sourceSchema < analysis.targetSchema
        && !binding.requestId.trimmed().isEmpty() && !binding.digest.isEmpty()
        && (binding.expectedBuildId.isEmpty() || binding.expectedBuildId == currentStudioBuildId())
        && projectOpenApprovalDigest(analysis, binding.requestId) == binding.digest;
}

bool confirmUpgrade(const ProjectUpgradeAnalysis& analysis, QWidget* parent)
{
    const QString backupRoot = analysis.recoveryRoot.isEmpty()
        ? QFileInfo(analysis.projectDirectory).absolutePath() : analysis.recoveryRoot;
    QString impact = text("%1 project file(s) will be updated.")
                         .arg(analysis.migrationPlan.fileCount);
    impact += QStringLiteral("\n\n")
        + text("Original files affected by the update will be backed up in:\n%1")
              .arg(QDir::toNativeSeparators(backupRoot));
    impact += QStringLiteral("\n\n")
        + text("The updated project may not open in older Studio versions.");
    if (analysis.protection == ProjectUpgradeProtection::RecoverableInPlace)
        impact += QStringLiteral("\n\n")
            + text("Keep the storage device connected until the update finishes.");

    QMessageBox dialog(QMessageBox::Question, text("Update Project"),
        text("Update \"%1\" to open it in Harken Studio %2?")
            .arg(QFileInfo(analysis.projectFilePath).completeBaseName(), currentStudioVersion()),
        QMessageBox::Ok | QMessageBox::Cancel, parent);
    dialog.setObjectName(QStringLiteral("projectUpgradeConfirmation"));
    dialog.setInformativeText(impact);
    dialog.button(QMessageBox::Ok)->setText(text("Update and Open"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.setEscapeButton(QMessageBox::Cancel);
    QString details = text("Project: %1\nRequired space on the project drive: %2")
        .arg(QDir::toNativeSeparators(analysis.projectFilePath),
             QLocale().formattedDataSize(analysis.requiredProjectBytes));
    details += QLatin1Char('\n') + text("Project format: %1 to %2")
        .arg(analysis.sourceSchema).arg(analysis.targetSchema);
    if (analysis.requiredRecoveryBytes > 0)
        details += QLatin1Char('\n') + text("Required recovery space: %1")
            .arg(QLocale().formattedDataSize(analysis.requiredRecoveryBytes));
    details += QStringLiteral("\n\n") + text("Files to update:")
        + QLatin1Char('\n') + analysis.migrationPlan.writeSet.join(QLatin1Char('\n'));
    dialog.setDetailedText(details);
    return dialog.exec() == QMessageBox::Ok;
}

}

ProjectOpenDecision confirmProjectOpen(
    const ProjectUpgradeAnalysis& analysis,
    QWidget* parent,
    const std::optional<ProjectOpenApprovalBinding>& suppliedApproval)
{
    ProjectOpenDecision decision;
    if (!analysis.ok) {
        decision.issue = analysis.issue == ProjectIssue::None
            ? ProjectIssue::InternalFailure : analysis.issue;
        return decision;
    }
    if (suppliedApproval && !matches(analysis, *suppliedApproval)) {
        decision.issue = ProjectIssue::ProjectChangedDuringUpgrade;
        return decision;
    }
    if (!analysis.upgradeRequired) {
        decision.proceed = true;
        return decision;
    }
    ProjectOpenApprovalBinding binding = suppliedApproval.value_or(ProjectOpenApprovalBinding{});
    if (!suppliedApproval) {
        binding.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        binding.expectedBuildId = currentStudioBuildId();
        binding.digest = projectOpenApprovalDigest(analysis, binding.requestId);
        if (!matches(analysis, binding)) {
            decision.issue = ProjectIssue::ProjectChangedDuringUpgrade;
            return decision;
        }
        if (!confirmUpgrade(analysis, parent)) {
            decision.cancelled = true;
            return decision;
        }
    }
    decision.proceed = true;
    decision.upgradeApproval = [binding](const ProjectUpgradeAnalysis& current) {
        return matches(current, binding);
    };
    return decision;
}

}
