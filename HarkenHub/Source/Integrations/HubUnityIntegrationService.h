#pragma once

#include "Integrations/Unity/UnityIntegrationService.h"

namespace harken::hub {

using UnityIntegrationStepStatus = harken::hub::unity::UnityIntegrationService::StepStatus;
using UnityIntegrationStep = harken::hub::unity::UnityIntegrationService::StepResult;

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

struct UnityIntegrationResult : harken::hub::unity::UnityIntegrationService::Result
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
