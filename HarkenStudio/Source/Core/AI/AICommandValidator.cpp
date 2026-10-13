#include "AICommandValidator.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <cmath>

namespace VNX {
namespace {

void setError(QString* codeOut, QString* messageOut, const QString& code,
              const QString& message)
{
    if (codeOut)
        *codeOut = code;
    if (messageOut)
        *messageOut = message;
}

bool isCredentialKey(const QString& key)
{
    static const QStringList keys = {
        QStringLiteral("api_key"), QStringLiteral("token"), QStringLiteral("authorization"),
        QStringLiteral("password"), QStringLiteral("secret"), QStringLiteral("credential")
    };
    return keys.contains(key.toLower());
}

bool containsCredentialKey(const QJsonValue& value)
{
    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            if (isCredentialKey(it.key()) || containsCredentialKey(it.value()))
                return true;
        }
    }
    else if (value.isArray())
    {
        for (const auto& item : value.toArray())
        {
            if (containsCredentialKey(item))
                return true;
        }
    }
    return false;
}

bool valueMatchesType(const QJsonValue& value, const QString& type)
{
    if (type == QLatin1String("object")) return value.isObject();
    if (type == QLatin1String("array")) return value.isArray();
    if (type == QLatin1String("string")) return value.isString();
    if (type == QLatin1String("boolean")) return value.isBool();
    if (type == QLatin1String("number")) return value.isDouble();
    if (type == QLatin1String("integer"))
        return value.isDouble() && std::floor(value.toDouble()) == value.toDouble();
    return false;
}

bool hasRequiredFields(const QJsonObject& value, const QJsonArray& required)
{
    for (const auto& field : required)
    {
        if (!field.isString() || !value.contains(field.toString()))
            return false;
    }
    return true;
}

bool validateObjectSchema(const QJsonObject& value, const QJsonObject& schema,
                          QString* message)
{
    if (!hasRequiredFields(value, schema.value(QStringLiteral("required")).toArray()))
    {
        if (message) *message = QStringLiteral("Command arguments are missing a required property.");
        return false;
    }
    const QJsonArray anyOf = schema.value(QStringLiteral("anyOf")).toArray();
    if (!anyOf.isEmpty())
    {
        bool matched = false;
        for (const auto& option : anyOf)
        {
            if (option.isObject() && hasRequiredFields(value, option.toObject().value(QStringLiteral("required")).toArray()))
            {
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            if (message) *message = QStringLiteral("Command arguments do not satisfy any allowed shape.");
            return false;
        }
    }
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    if (schema.value(QStringLiteral("additionalProperties")).isBool()
        && !schema.value(QStringLiteral("additionalProperties")).toBool())
    {
        for (auto it = value.constBegin(); it != value.constEnd(); ++it)
        {
            if (!properties.contains(it.key()))
            {
                if (message) *message = QStringLiteral("Command arguments contain an unsupported property.");
                return false;
            }
        }
    }
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
    {
        if (!value.contains(it.key())) continue;
        const QJsonObject propertySchema = it.value().toObject();
        const QJsonValue propertyValue = value.value(it.key());
        const QString type = propertySchema.value(QStringLiteral("type")).toString();
        if (!type.isEmpty() && !valueMatchesType(propertyValue, type))
        {
            if (message) *message = QStringLiteral("Command argument '%1' has the wrong type.").arg(it.key());
            return false;
        }
        if (propertySchema.contains(QStringLiteral("enum"))
            && !propertySchema.value(QStringLiteral("enum")).toArray().contains(propertyValue))
        {
            if (message) *message = QStringLiteral("Command argument '%1' is not an allowed value.").arg(it.key());
            return false;
        }
        if (propertySchema.contains(QStringLiteral("minimum"))
            && propertyValue.toDouble() < propertySchema.value(QStringLiteral("minimum")).toDouble())
        {
            if (message) *message = QStringLiteral("Command argument '%1' is below its minimum.").arg(it.key());
            return false;
        }
        if (propertySchema.contains(QStringLiteral("minLength"))
            && propertyValue.toString().size() < propertySchema.value(QStringLiteral("minLength")).toInt())
        {
            if (message) *message = QStringLiteral("Command argument '%1' is too short.").arg(it.key());
            return false;
        }
    }
    return true;
}

}

AICommandValidator::AICommandValidator()
    : AICommandValidator(AICommandCatalog::all())
{
}

AICommandValidator::AICommandValidator(const QList<AICommandSpec>& catalog)
{
    catalog_.reserve(catalog.size());
    for (const auto& entry : catalog)
        catalog_.insert(entry.cmd, entry);
}

QString AICommandValidator::normalizePermissionProfile(const QString& mode)
{
    return toId(parsePermissionMode(mode));
}

bool AICommandValidator::validateArguments(const QJsonObject& arguments,
                                           const QJsonObject& schema,
                                           QString* errorMessage)
{
    return validateObjectSchema(arguments, schema, errorMessage);
}

std::optional<QJsonObject> AICommandValidator::normalizeAndValidate(
    const QJsonObject& modelCommand, const Policy& policy,
    QString* errorCode, QString* errorMessage) const
{
    if (errorCode) errorCode->clear();
    if (errorMessage) errorMessage->clear();
    const QJsonValue cmdValue = modelCommand.value(QStringLiteral("cmd"));
    if (!cmdValue.isString() || cmdValue.toString().isEmpty())
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("A non-empty cmd field is required."));
        return std::nullopt;
    }
    const auto it = catalog_.constFind(cmdValue.toString());
    if (it == catalog_.constEnd())
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("The requested command is not in the AI command catalog."));
        return std::nullopt;
    }
    const AICommandSpec& spec = it.value();
    if (spec.availability != AICommandSpec::Availability::Enabled)
    {
        setError(errorCode, errorMessage, QStringLiteral("AI_COMMAND_DISABLED"), QStringLiteral("The requested AI command is disabled."));
        return std::nullopt;
    }
    if (containsCredentialKey(modelCommand))
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Credential-shaped fields are not allowed in AI commands."));
        return std::nullopt;
    }
    if (spec.effect != AICommandSpec::Effect::Query && !allowsWrites(policy.permission))
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("The active permission profile does not allow writes."));
        return std::nullopt;
    }
    static const QStringList canonicalKeys = {QStringLiteral("cmd"), QStringLiteral("args"), QStringLiteral("request_id")};
    if (modelCommand.size() != canonicalKeys.size() || modelCommand.contains(QStringLiteral("command")))
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("The command envelope must contain exactly cmd, args, and request_id."));
        return std::nullopt;
    }
    for (const auto& key : canonicalKeys)
    {
        if (!modelCommand.contains(key))
        {
            setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("The command envelope is missing a required field."));
            return std::nullopt;
        }
    }
    const QJsonValue argsValue = modelCommand.value(QStringLiteral("args"));
    const QJsonValue requestIdValue = modelCommand.value(QStringLiteral("request_id"));
    if (!argsValue.isObject() || !requestIdValue.isString() || requestIdValue.toString().isEmpty())
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("args must be an object and request_id must be a non-empty string."));
        return std::nullopt;
    }
    QString schemaMessage;
    if (!validateArguments(argsValue.toObject(), spec.argumentSchema, &schemaMessage))
    {
        setError(errorCode, errorMessage, QStringLiteral("INVALID_ARGUMENTS"), schemaMessage);
        return std::nullopt;
    }
    if (requiresConfirmation(policy.permission, spec.confirmation) && !policy.confirmationGranted)
    {
        setError(errorCode, errorMessage, QStringLiteral("AI_CONFIRMATION_REQUIRED"), QStringLiteral("The requested command requires explicit confirmation."));
        return std::nullopt;
    }
    return QJsonObject{{QStringLiteral("cmd"), spec.cmd}, {QStringLiteral("args"), argsValue.toObject()}, {QStringLiteral("request_id"), requestIdValue.toString()}};
}

}
