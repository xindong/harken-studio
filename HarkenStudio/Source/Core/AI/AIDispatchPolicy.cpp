#include "AIDispatchPolicy.h"

#include "AICommandValidator.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace VNX {
namespace {

QJsonValue canonicalValue(const QJsonValue& value)
{
    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        QJsonObject canonical;
        for (const QString& key : keys)
            canonical.insert(key, canonicalValue(object.value(key)));
        return canonical;
    }
    if (value.isArray())
    {
        QJsonArray canonical;
        for (const QJsonValue& item : value.toArray())
            canonical.append(canonicalValue(item));
        return canonical;
    }
    return value;
}

QString normalizeToolProfile(const QString& profile)
{
    return profile.trimmed().toLower();
}

AIDispatchPolicy::Decision denied(const QString& code, const QString& message)
{
    return {false, code, message};
}

}

AIDispatchPolicy::AIDispatchPolicy()
    : clock_([]() { return QDateTime::currentMSecsSinceEpoch(); })
{
    for (const auto& entry : AICommandCatalog::all())
        catalog_.insert(entry.cmd, entry);
}

AIDispatchPolicy::AIDispatchPolicy(const QList<AICommandSpec>& catalog)
    : AIDispatchPolicy()
{
    catalog_.clear();
    for (const auto& entry : catalog)
        catalog_.insert(entry.cmd, entry);
}

void AIDispatchPolicy::setClockForTesting(Clock clock)
{
    clock_ = std::move(clock);
}

qint64 AIDispatchPolicy::nowMs() const
{
    return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch();
}

void AIDispatchPolicy::pruneExpired(qint64 now)
{
    for (auto it = consumed_.begin(); it != consumed_.end();)
    {
        if (it->expiresAtMs <= now)
            it = consumed_.erase(it);
        else
            ++it;
    }
}

QByteArray AIDispatchPolicy::bindingFor(const Request& request) const
{
    const auto it = catalog_.constFind(request.cmd);
    const AICommandSpec* spec = it == catalog_.constEnd() ? nullptr : &it.value();
    const int contractVersion = spec ? spec->contractVersion : 0;
    const QJsonObject binding{
        {QStringLiteral("args"), canonicalValue(request.args)},
        {QStringLiteral("cmd"), request.cmd},
        {QStringLiteral("contract_version"), contractVersion},
        {QStringLiteral("permission"),
         AICommandValidator::normalizePermissionProfile(request.permissionProfile)},
        {QStringLiteral("tool_profile"), normalizeToolProfile(request.toolProfile)}
    };
    return QCryptographicHash::hash(
        QJsonDocument(canonicalValue(binding).toObject()).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
}

AIDispatchPolicy::Decision AIDispatchPolicy::evaluate(const Request& request,
                                                       bool consumeGrant)
{
    const qint64 now = nowMs();
    pruneExpired(now);
    const auto it = catalog_.constFind(request.cmd);
    const AICommandSpec* spec = it == catalog_.constEnd() ? nullptr : &it.value();
    if (!spec || spec->availability != AICommandSpec::Availability::Enabled)
        return denied(QStringLiteral("AI_COMMAND_UNAVAILABLE"),
                      QStringLiteral("The requested AI command is unavailable."));

    const QString profile = normalizeToolProfile(request.toolProfile);
    if (!spec->toolProfiles.contains(profile))
        return denied(QStringLiteral("AI_PROFILE_DENIED"),
                      QStringLiteral("The active AI tool profile does not allow this command."));

    const AIPermissionMode permission = parsePermissionMode(request.permissionProfile);
    if (spec->effect != AICommandSpec::Effect::Query && !allowsWrites(permission))
    {
        return denied(QStringLiteral("AI_PROFILE_DENIED"),
                      QStringLiteral("The active AI permission does not allow this command."));
    }

    QString schemaError;
    if (!AICommandValidator::validateArguments(request.args, spec->argumentSchema, &schemaError))
        return denied(QStringLiteral("INVALID_ARGUMENTS"), schemaError);

    if (!requiresConfirmation(permission, spec->confirmation))
        return {true, {}, {}};
    if (request.grantId.isEmpty())
        return denied(QStringLiteral("AI_CONFIRMATION_REQUIRED"),
                      QStringLiteral("The requested AI command requires confirmation."));

    const auto consumedIt = consumed_.find(request.grantId);
    if (consumedIt != consumed_.end())
        return denied(QStringLiteral("AI_GRANT_REPLAYED"),
                      QStringLiteral("The AI confirmation grant was already consumed."));
    auto grantIt = grants_.find(request.grantId);
    if (grantIt == grants_.end())
        return denied(QStringLiteral("AI_GRANT_INVALID"),
                      QStringLiteral("The AI confirmation grant is invalid."));
    if (grantIt->expiresAtMs <= now)
    {
        grants_.erase(grantIt);
        return denied(QStringLiteral("AI_GRANT_EXPIRED"),
                      QStringLiteral("The AI confirmation grant expired."));
    }
    const QByteArray binding = bindingFor(request);
    if (grantIt->binding != binding || grantIt->requestId != request.requestId)
        return denied(QStringLiteral("AI_GRANT_INVALID"),
                      QStringLiteral("The AI confirmation grant does not match this command."));
    if (consumeGrant)
    {
        consumed_.insert(request.grantId, ConsumedGrant{grantIt->binding, grantIt->expiresAtMs});
        grants_.erase(grantIt);
    }
    return {true, {}, {}};
}

QString AIDispatchPolicy::issueGrant(const Request& request, int ttlMs)
{
    const qint64 now = nowMs();
    pruneExpired(now);
    const QString grantId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    grants_.insert(grantId, Grant{
        bindingFor(request), request.requestId,
        now + qMax(0, ttlMs)
    });
    return grantId;
}

void AIDispatchPolicy::revokeGrant(const QString& grantId)
{
    pruneExpired(nowMs());
    grants_.remove(grantId);
}

void AIDispatchPolicy::revokeRequest(const QString& requestId)
{
    pruneExpired(nowMs());
    for (auto it = grants_.begin(); it != grants_.end();)
    {
        if (it->requestId == requestId)
            it = grants_.erase(it);
        else
            ++it;
    }
}

}
