#pragma once

#include "ProjectUpgradeManager.h"

class QWidget;

namespace harken::studio {

struct ProjectOpenDecision
{
    bool proceed = false;
    bool cancelled = false;
    ProjectIssue issue = ProjectIssue::None;
    ProjectUpgradeApprovalCallback upgradeApproval;
};

ProjectOpenDecision confirmProjectOpen(
    const ProjectUpgradeAnalysis& analysis,
    QWidget* parent);

}
