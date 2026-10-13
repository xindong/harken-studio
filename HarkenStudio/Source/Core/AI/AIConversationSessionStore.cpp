#include "AIConversationSessionStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>
#include <utility>

namespace VNX {
namespace {

constexpr auto kInvalidCode = "AI_CONVERSATION_SESSIONS_INVALID";
constexpr auto kIoCode = "AI_CONVERSATION_SESSIONS_IO_ERROR";
constexpr auto kBusyCode = "AI_CONVERSATION_SESSIONS_BUSY";
constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr int kLockTimeoutMs = 5000;

bool validId(const QString& id)
{
    const QString normalized = id.trimmed().toLower();
    const QUuid uuid(normalized);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == normalized;
}

bool validInteger(const QJsonValue& value, qint64* output)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number <= 0.0
        || number > static_cast<double>(kMaximumExactJsonInteger))
    {
        return false;
    }
    if (output)
        *output = static_cast<qint64>(number);
    return true;
}

QJsonObject serializeSession(
    const AIConversationSessionRecord& session)
{
    return QJsonObject{
        {QStringLiteral("id"), session.id},
        {QStringLiteral("title"), session.title},
        {QStringLiteral("created_at_ms"),
         static_cast<double>(session.createdAtMs)},
        {QStringLiteral("updated_at_ms"),
         static_cast<double>(session.updatedAtMs)}
    };
}

AIConversationSessionLoadResult invalidLoad(const QString& message)
{
    AIConversationSessionLoadResult result;
    result.status = AIConversationSessionLoadResult::Status::Invalid;
    result.code = QString::fromLatin1(kInvalidCode);
    result.message = message;
    return result;
}

AIConversationSessionLoadResult ioLoad(const QString& message)
{
    AIConversationSessionLoadResult result;
    result.status = AIConversationSessionLoadResult::Status::IoError;
    result.code = QString::fromLatin1(kIoCode);
    result.message = message;
    return result;
}

AIConversationSessionOperationResult success()
{
    AIConversationSessionOperationResult result;
    result.success = true;
    return result;
}

AIConversationSessionOperationResult failure(
    const QString& code,
    const QString& message)
{
    AIConversationSessionOperationResult result;
    result.code = code;
    result.message = message;
    return result;
}

}

LocalAIConversationSessionStore::LocalAIConversationSessionStore()
    : LocalAIConversationSessionStore(defaultRootPath())
{
}

LocalAIConversationSessionStore::LocalAIConversationSessionStore(
    QString rootPath)
    : rootPath_(rootPath.trimmed().isEmpty()
                    ? QString()
                    : QDir::cleanPath(std::move(rootPath)))
{
}

QString LocalAIConversationSessionStore::defaultRootPath()
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (appData.isEmpty())
        return {};
    return QDir(appData).filePath(QStringLiteral("AI/Conversations"));
}

QString LocalAIConversationSessionStore::scopeHash(
    const QString& scope)
{
    const QByteArray digest = QCryptographicHash::hash(
        scope.trimmed().toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}

QString LocalAIConversationSessionStore::storagePathForScope(
    const QString& scope) const
{
    if (rootPath_.isEmpty() || scope.trimmed().isEmpty())
        return {};
    return QDir(rootPath_).filePath(
        scopeHash(scope) + QStringLiteral(".sessions.json"));
}

bool LocalAIConversationSessionStore::normalizeCatalog(
    const AIConversationSessionCatalog& input,
    AIConversationSessionCatalog* output,
    QString* errorMessage)
{
    auto fail = [errorMessage](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    if (!output)
        return fail(QStringLiteral("Conversation session output is unavailable."));
    if (input.sessions.isEmpty()
        || input.sessions.size()
            > AIConversationSessionLimits::kMaximumSessions)
    {
        return fail(QStringLiteral(
            "Conversation session count is invalid."));
    }

    AIConversationSessionCatalog normalized;
    normalized.activeSessionId =
        input.activeSessionId.trimmed().toLower();
    QSet<QString> ids;
    for (const AIConversationSessionRecord& source : input.sessions)
    {
        AIConversationSessionRecord session = source;
        session.id = session.id.trimmed().toLower();
        session.title = session.title.trimmed();
        if (!validId(session.id)
            || session.title.isEmpty()
            || session.title.toUtf8().size()
                > AIConversationSessionLimits::kMaximumTitleBytes
            || session.createdAtMs <= 0
            || session.createdAtMs > kMaximumExactJsonInteger
            || session.updatedAtMs < session.createdAtMs
            || session.updatedAtMs > kMaximumExactJsonInteger
            || ids.contains(session.id))
        {
            return fail(QStringLiteral(
                "Conversation session metadata is invalid."));
        }
        ids.insert(session.id);
        normalized.sessions.append(session);
    }
    if (!ids.contains(normalized.activeSessionId))
    {
        return fail(QStringLiteral(
            "Active conversation session is invalid."));
    }

    *output = normalized;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

AIConversationSessionLoadResult
LocalAIConversationSessionStore::load(
    const QString& scope) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
        return invalidLoad(QStringLiteral(
            "Conversation session scope is invalid."));

    QFile file(path);
    if (!file.exists())
        return {};
    if (file.size()
        > AIConversationSessionLimits::kMaximumDocumentBytes)
    {
        return invalidLoad(QStringLiteral(
            "Stored conversation sessions exceed the file limit."));
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        return ioLoad(QStringLiteral(
            "Stored conversation sessions could not be opened."));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject())
    {
        return invalidLoad(QStringLiteral(
            "Stored conversation sessions are not valid JSON."));
    }
    const QJsonObject root = document.object();
    const QJsonValue schemaVersion =
        root.value(QStringLiteral("schema_version"));
    if (!schemaVersion.isDouble()
        || schemaVersion.toDouble()
            != static_cast<double>(
                AIConversationSessionLimits::kSchemaVersion)
        || root.value(QStringLiteral("scope_id")).toString()
            != scopeHash(scope)
        || !root.value(QStringLiteral("active_session_id")).isString()
        || !root.value(QStringLiteral("sessions")).isArray())
    {
        return invalidLoad(QStringLiteral(
            "Stored conversation session document is invalid."));
    }

    AIConversationSessionCatalog catalog;
    catalog.activeSessionId =
        root.value(QStringLiteral("active_session_id")).toString();
    const QJsonArray serialized =
        root.value(QStringLiteral("sessions")).toArray();
    if (serialized.size()
        > AIConversationSessionLimits::kMaximumSessions)
    {
        return invalidLoad(QStringLiteral(
            "Stored conversation session count is invalid."));
    }
    for (const QJsonValue& value : serialized)
    {
        if (!value.isObject())
        {
            return invalidLoad(QStringLiteral(
                "Stored conversation session is malformed."));
        }
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("id")).isString()
            || !object.value(QStringLiteral("title")).isString())
        {
            return invalidLoad(QStringLiteral(
                "Stored conversation session fields are invalid."));
        }
        AIConversationSessionRecord session;
        session.id = object.value(QStringLiteral("id")).toString();
        session.title = object.value(QStringLiteral("title")).toString();
        if (!validInteger(
                object.value(QStringLiteral("created_at_ms")),
                &session.createdAtMs)
            || !validInteger(
                object.value(QStringLiteral("updated_at_ms")),
                &session.updatedAtMs))
        {
            return invalidLoad(QStringLiteral(
                "Stored conversation session timestamps are invalid."));
        }
        catalog.sessions.append(session);
    }

    AIConversationSessionCatalog normalized;
    QString validationError;
    if (!normalizeCatalog(catalog, &normalized, &validationError))
        return invalidLoad(validationError);

    AIConversationSessionLoadResult result;
    result.status = AIConversationSessionLoadResult::Status::Loaded;
    result.catalog = normalized;
    return result;
}

AIConversationSessionOperationResult
LocalAIConversationSessionStore::mutate(
    const QString& scope,
    const Mutation& mutation) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
    {
        return failure(
            QString::fromLatin1(kInvalidCode),
            QStringLiteral("Conversation session scope is invalid."));
    }
    if (!mutation)
    {
        return failure(
            QString::fromLatin1(kInvalidCode),
            QStringLiteral("Conversation session mutation is unavailable."));
    }
    if (!QDir().mkpath(rootPath_))
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral(
                "Conversation session directory could not be created."));
    }

    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(kLockTimeoutMs))
    {
        return failure(
            QString::fromLatin1(kBusyCode),
            QStringLiteral("Conversation session scope is busy."));
    }

    const AIConversationSessionLoadResult current = load(scope);
    AIConversationSessionCatalog replacement;
    bool shouldPersist = false;
    const AIConversationSessionOperationResult mutationResult =
        mutation(current, &replacement, &shouldPersist);
    if (!mutationResult.success || !shouldPersist)
        return mutationResult;

    const AIConversationSessionOperationResult persisted =
        saveUnlocked(scope, path, replacement);
    return persisted.success ? mutationResult : persisted;
}

AIConversationSessionOperationResult
LocalAIConversationSessionStore::saveUnlocked(
    const QString& scope,
    const QString& path,
    const AIConversationSessionCatalog& catalog) const
{
    AIConversationSessionCatalog normalized;
    QString validationError;
    if (!normalizeCatalog(catalog, &normalized, &validationError))
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       validationError);
    }

    QJsonArray sessions;
    for (const AIConversationSessionRecord& session
         : normalized.sessions)
    {
        sessions.append(serializeSession(session));
    }
    const QJsonObject root{
        {QStringLiteral("schema_version"),
         AIConversationSessionLimits::kSchemaVersion},
        {QStringLiteral("scope_id"), scopeHash(scope)},
        {QStringLiteral("active_session_id"),
         normalized.activeSessionId},
        {QStringLiteral("sessions"), sessions}
    };
    const QByteArray bytes =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size()
        > AIConversationSessionLimits::kMaximumDocumentBytes)
    {
        return failure(
            QString::fromLatin1(kInvalidCode),
            QStringLiteral(
                "Conversation session document exceeds the file limit."));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size()
        || !file.commit())
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral(
                "Conversation session document could not be committed."));
    }
    return success();
}

AIConversationSessionOperationResult
LocalAIConversationSessionStore::save(
    const QString& scope,
    const AIConversationSessionCatalog& catalog) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
    {
        return failure(
            QString::fromLatin1(kInvalidCode),
            QStringLiteral("Conversation session scope is invalid."));
    }
    if (!QDir().mkpath(rootPath_))
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral(
                "Conversation session directory could not be created."));
    }

    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(kLockTimeoutMs))
    {
        return failure(
            QString::fromLatin1(kBusyCode),
            QStringLiteral("Conversation session scope is busy."));
    }
    return saveUnlocked(scope, path, catalog);
}

}
