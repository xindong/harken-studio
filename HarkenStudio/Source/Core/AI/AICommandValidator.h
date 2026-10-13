#pragma once

#include "AICommandCatalog.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace VNX {

class AICommandValidator
{
public:
    struct Policy
    {
        AIPermissionMode permission = AIPermissionMode::Ask;
        bool confirmationGranted = false;

        Policy() = default;
        Policy(AIPermissionMode mode, bool granted)
            : permission(mode), confirmationGranted(granted) {}
    };

    AICommandValidator();
    explicit AICommandValidator(const QList<AICommandSpec>& catalog);

    static QString normalizePermissionProfile(const QString& mode);
    static bool validateArguments(const QJsonObject& arguments,
                                  const QJsonObject& schema,
                                  QString* errorMessage = nullptr);

    std::optional<QJsonObject> normalizeAndValidate(
        const QJsonObject& modelCommand,
        const Policy& policy,
        QString* errorCode,
        QString* errorMessage) const;

private:
    QHash<QString, AICommandSpec> catalog_;
};

}
