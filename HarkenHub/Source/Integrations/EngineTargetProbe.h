#pragma once

#include "Integrations/EngineTargetStore.h"

#include <QString>

namespace VNH {

enum class EngineTargetHealth
{
    Missing,
    NotInstalled,
    Managed,
    Unmanaged,
    Incomplete,
    Unreadable
};

enum class IntegrationContentState
{
    NotChecked,
    MatchesInstallation,
    OptionalFilesMissing,
    Modified,
    Missing,
    ModifiedAndMissing,
    Unverified
};

struct EngineTargetObservation
{
    bool validTarget = false;
    EngineTargetHealth health = EngineTargetHealth::Unreadable;
    QString engineVersion;
    QString integrationVersion;
    QString integrationInstallPath;
    QString error;
    QString status;
    QString remediation;
    IntegrationContentState integrationContentState = IntegrationContentState::NotChecked;
    QString integrationContentDetails;
    bool dependencyRestricted = false;
    bool canChangeIntegration = false;
    bool canMoveIntegration = false;
    bool canRemoveIntegration = false;
};

class EngineTargetProbe
{
public:
    static bool resolve(const QString& input, EngineTargetRecord* record,
                        QString* error = nullptr);
    static bool resolve(EngineKind kind, const QString& input,
                        EngineTargetRecord* record, QString* error = nullptr);
    static EngineTargetObservation observe(const EngineTargetRecord& record, bool verifyContent = false);
};

}
