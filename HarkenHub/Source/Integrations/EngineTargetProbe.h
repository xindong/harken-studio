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

struct EngineTargetObservation
{
    bool validTarget = false;
    EngineTargetHealth health = EngineTargetHealth::Unreadable;
    QString engineVersion;
    QString integrationVersion;
    QString sdkInstallPath;
    QString error;
    QString status;
    QString remediation;
    bool canChangeSdk = false;
    bool canMoveSdk = false;
    bool canRemoveSdk = false;
};

class EngineTargetProbe
{
public:
    static bool resolve(const QString& input, EngineTargetRecord* record,
                        QString* error = nullptr);
    static bool resolve(EngineKind kind, const QString& input,
                        EngineTargetRecord* record, QString* error = nullptr);
    static EngineTargetObservation observe(const EngineTargetRecord& record);
};

}
