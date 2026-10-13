#include "AIPermissionModel.h"

namespace VNX {

QString toId(AIPermissionMode mode)
{
    switch (mode) {
    case AIPermissionMode::ReadOnly: return QStringLiteral("read_only");
    case AIPermissionMode::Auto: return QStringLiteral("auto");
    case AIPermissionMode::Ask: default: return QStringLiteral("ask");
    }
}

QString toId(AIToolProfile profile)
{
    switch (profile) {
    case AIToolProfile::ReadOnly: return QStringLiteral("read_only");
    case AIToolProfile::Pack: return QStringLiteral("pack");
    case AIToolProfile::Live: return QStringLiteral("live");
    case AIToolProfile::Authoring: default: return QStringLiteral("authoring");
    }
}

AIPermissionMode parsePermissionMode(const QString& value)
{
    const QString id = value.trimmed().toLower();
    if (id == QLatin1String("read_only")) return AIPermissionMode::ReadOnly;
    if (id == QLatin1String("auto")) return AIPermissionMode::Auto;
    return AIPermissionMode::Ask;
}

std::optional<AIToolProfile> parseToolProfile(const QString& value)
{
    const QString id = value.trimmed().toLower();
    if (id == QLatin1String("read_only")) return AIToolProfile::ReadOnly;
    if (id == QLatin1String("authoring")) return AIToolProfile::Authoring;
    if (id == QLatin1String("pack")) return AIToolProfile::Pack;
    if (id == QLatin1String("live")) return AIToolProfile::Live;
    return std::nullopt;
}

bool allowsWrites(AIPermissionMode mode)
{
    return mode != AIPermissionMode::ReadOnly;
}

const QList<AIPermissionMetadata>& permissionMetadata()
{
    static const QList<AIPermissionMetadata> metadata = {
        {"read_only", "Read-only (deny all writes)", "Read-only denies every write outright; the agent can only inspect the project."},
        {"ask", "Ask before writes (interactive approval)", "Ask presents Approve and Reject controls before a write that requires confirmation can execute."},
        {"auto", "Auto-approve eligible writes", "Auto skips confirmation only for commands classified auto-approvable and reversible. No currently enabled command is eligible, so every current write still asks; high-risk or irreversible writes always ask."},
        {"bypass", "Legacy Bypass (treated as Ask)", "Legacy Bypass is retained for compatibility and behaves like Ask."}
    };
    return metadata;
}

const QList<AIToolProfileMetadata>& toolProfileMetadata()
{
    static const QList<AIToolProfileMetadata> metadata = {
        {AIToolProfile::ReadOnly, "Read-only project assistant"},
        {AIToolProfile::Authoring, "Authoring assistant"},
        {AIToolProfile::Pack, "Pack / export assistant"},
        {AIToolProfile::Live, "Live testing assistant"}
    };
    return metadata;
}

AIToolProfileSet::AIToolProfileSet(const QStringList& ids)
{
    for (const QString& id : ids) {
        const auto profile = parseToolProfile(id);
        if (profile) add(*profile);
    }
}

bool AIToolProfileSet::contains(const QString& id) const
{
    const auto profile = parseToolProfile(id);
    return profile && contains(*profile);
}

bool requiresConfirmation(AIPermissionMode mode, AIConfirmationPolicy confirmation)
{
    if (confirmation == AIConfirmationPolicy::Always) return true;
    if (confirmation == AIConfirmationPolicy::WhenPolicyRequires)
        return mode != AIPermissionMode::Auto;
    return false;
}

}
