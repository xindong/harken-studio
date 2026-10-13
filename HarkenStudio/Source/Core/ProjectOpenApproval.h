#pragma once

#include "ProjectUpgradeManager.h"

#include <optional>

class QWidget;

namespace VNX {

struct ProjectOpenApprovalBinding
{
    QString requestId;
    QString digest;
    QString expectedBuildId;
};

struct ProjectOpenDecision
{
    bool proceed = false;
    bool cancelled = false;
    ProjectIssue issue = ProjectIssue::None;
    ProjectUpgradeApprovalCallback upgradeApproval;
};

ProjectOpenDecision confirmProjectOpen(
    const ProjectUpgradeAnalysis& analysis,
    QWidget* parent,
    const std::optional<ProjectOpenApprovalBinding>& suppliedApproval = std::nullopt);

}
