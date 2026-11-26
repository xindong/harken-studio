#pragma once

#include "ProjectIssue.h"

#include <QString>

namespace harken::studio {

enum class ProjectUpgradePhase;
struct ProjectUpgradeProgress;

}

namespace harken::studio::ProjectUpgradeMessages {

struct Presentation
{
    QString reason;
    QString nextStep;
    QString technicalDetails;
    QString userDetails;

    QString primaryText() const;
    QString detailedText() const;
    QString userText() const;
};

Presentation presentation(ProjectIssue issue, const QString& detail = {});
Presentation presentation(const QString& internalError,
                          bool upgradeContext = false);
QString userMessage(ProjectIssue issue, const QString& detail = {});
QString userMessage(const QString& internalError, bool upgradeContext = false);
QString progressText(ProjectUpgradePhase phase);
int progressPercent(const ProjectUpgradeProgress& progress);

}
