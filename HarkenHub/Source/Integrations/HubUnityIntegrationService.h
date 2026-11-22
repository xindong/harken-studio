#pragma once

#include "Integrations/Unity/UnityIntegrationService.h"

namespace VNH {

using UnityIntegrationStepStatus = VNX::UnityIntegrationService::StepStatus;
using UnityIntegrationStep = VNX::UnityIntegrationService::StepResult;

struct UnityIntegrationRequest
{
    QString unityProjectPath;
    QString integrationSourcePath;
    QString integrationInstallPath;
    bool checkOnly = false;
    bool inspectOnly = false;
    bool removeIntegration = false;
    bool restorePrevious = false;
};

struct UnityIntegrationResult : VNX::UnityIntegrationService::Result
{
    bool cancelled = false;
};

class HubUnityIntegrationService
{
public:
    static UnityIntegrationResult run(const UnityIntegrationRequest& request);
    static QString localizeText(const QString& text);
};

}
