#pragma once

#include "ProjectIssue.h"

#include <QString>

namespace VNX {

enum class ProjectUpgradePhase;
struct ProjectUpgradeProgress;

}

namespace VNX::ProjectUpgradeMessages {

struct Presentation
{
    QString reason;
    QString nextStep;
    QString technicalDetails;

    QString primaryText() const;
};

Presentation presentation(ProjectIssue issue, const QString& detail = {});
Presentation presentation(const QString& internalError,
                          bool upgradeContext = false);
QString userMessage(ProjectIssue issue, const QString& detail = {});
QString userMessage(const QString& internalError, bool upgradeContext = false);
QString progressText(ProjectUpgradePhase phase);
int progressPercent(const ProjectUpgradeProgress& progress);

}
