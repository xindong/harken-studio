#pragma once

#include "ProjectUpgradeManager.h"

#include <QJsonObject>
#include <QString>

namespace VNX {

inline constexpr int kProjectOpenAnalysisProtocolVersion = 1;

QString currentStudioBuildId();
QString currentStudioVersion();
QString projectOpenApprovalDigest(const ProjectUpgradeAnalysis& analysis,
                                  const QString& requestId);
QJsonObject projectOpenAnalysisReport(const ProjectUpgradeAnalysis& analysis,
                                      const QString& requestId);

}
