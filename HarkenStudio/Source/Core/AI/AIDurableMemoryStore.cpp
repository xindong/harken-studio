#include "AIDurableMemoryStore.h"

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

constexpr auto kInvalidCode = "AI_DURABLE_MEMORY_INVALID";
constexpr auto kIoCode = "AI_DURABLE_MEMORY_IO_ERROR";
constexpr auto kBusyCode = "AI_DURABLE_MEMORY_BUSY";
constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr int kLockTimeoutMs = 5000;

QString canonicalKey(const QString& key)
{
    return key.trimmed().toCaseFolded();
}

QString kindName(AIDurableMemoryKind kind)
{
    switch (kind)
    {
    case AIDurableMemoryKind::UserPreference:
        return QStringLiteral("user_preference");
    case AIDurableMemoryKind::ProjectFact:
        return QStringLiteral("project_fact");
    }
    return {};
}

bool parseKind(const QString& name, AIDurableMemoryKind* kind)
{
    if (!kind)
        return false;
    if (name == QLatin1String("user_preference"))
    {
        *kind = AIDurableMemoryKind::UserPreference;
        return true;
    }
    if (name == QLatin1String("project_fact"))
    {
        *kind = AIDurableMemoryKind::ProjectFact;
        return true;
    }
    return false;
}

QString sourceName(AIDurableMemorySource source)
{
    switch (source)
    {
    case AIDurableMemorySource::User:
        return QStringLiteral("user");
    case AIDurableMemorySource::Model:
        return QStringLiteral("model");
    }
    return {};
}

bool parseSource(const QString& name, AIDurableMemorySource* source)
{
    if (!source)
        return false;
    if (name == QLatin1String("user"))
    {
        *source = AIDurableMemorySource::User;
        return true;
    }
    if (name == QLatin1String("model"))
    {
        *source = AIDurableMemorySource::Model;
        return true;
    }
    return false;
}

bool scopeMatchesKind(const QString& scope, AIDurableMemoryKind kind)
{
    if (kind == AIDurableMemoryKind::UserPreference)
        return scope.startsWith(QStringLiteral("user:"))
            && scope.size() > QStringLiteral("user:").size();
    return scope.startsWith(QStringLiteral("project:"))
        && scope.size() > QStringLiteral("project:").size();
}

bool supportedScope(const QString& scope)
{
    return (scope.startsWith(QStringLiteral("user:"))
            && scope.size() > QStringLiteral("user:").size())
        || (scope.startsWith(QStringLiteral("project:"))
            && scope.size() > QStringLiteral("project:").size());
}

bool validInteger(const QJsonValue& value, qint64 minimum, qint64* output)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(minimum)
        || number > static_cast<double>(kMaximumExactJsonInteger))
    {
        return false;
    }
    if (output)
        *output = static_cast<qint64>(number);
    return true;
}

bool validId(const QString& id)
{
    const QString normalized = id.trimmed().toLower();
    const QUuid uuid(normalized);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == normalized;
}

QJsonObject serializeEntry(const AIDurableMemoryEntry& entry)
{
    return QJsonObject{
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("kind"), kindName(entry.kind)},
        {QStringLiteral("key"), entry.key},
        {QStringLiteral("value"), entry.value},
        {QStringLiteral("source"), sourceName(entry.source)},
        {QStringLiteral("source_reference"), entry.sourceReference},
        {QStringLiteral("created_at_ms"),
         static_cast<double>(entry.createdAtMs)},
        {QStringLiteral("updated_at_ms"),
         static_cast<double>(entry.updatedAtMs)},
        {QStringLiteral("revision"),
         static_cast<double>(entry.revision)}
    };
}

AIDurableMemoryLoadResult invalidLoad(const QString& message)
{
    AIDurableMemoryLoadResult result;
    result.status = AIDurableMemoryLoadResult::Status::Invalid;
    result.code = QString::fromLatin1(kInvalidCode);
    result.message = message;
    return result;
}

AIDurableMemoryLoadResult ioLoad(const QString& message)
{
    AIDurableMemoryLoadResult result;
    result.status = AIDurableMemoryLoadResult::Status::IoError;
    result.code = QString::fromLatin1(kIoCode);
    result.message = message;
    return result;
}

AIDurableMemoryOperationResult success()
{
    AIDurableMemoryOperationResult result;
    result.success = true;
    return result;
}

AIDurableMemoryOperationResult failure(const QString& code,
                                       const QString& message)
{
    AIDurableMemoryOperationResult result;
    result.code = code;
    result.message = message;
    return result;
}

}

LocalAIDurableMemoryStore::LocalAIDurableMemoryStore()
    : LocalAIDurableMemoryStore(defaultRootPath())
{
}

LocalAIDurableMemoryStore::LocalAIDurableMemoryStore(QString rootPath)
    : rootPath_(rootPath.trimmed().isEmpty()
                    ? QString()
                    : QDir::cleanPath(std::move(rootPath)))
{
}

QString LocalAIDurableMemoryStore::defaultRootPath()
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (appData.isEmpty())
        return {};
    return QDir(appData).filePath(QStringLiteral("AI/Memories"));
}

QString LocalAIDurableMemoryStore::scopeHash(const QString& scope)
{
    const QByteArray digest = QCryptographicHash::hash(
        scope.trimmed().toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}

QString LocalAIDurableMemoryStore::storagePathForScope(
    const QString& scope) const
{
    const QString normalizedScope = scope.trimmed();
    if (rootPath_.isEmpty() || normalizedScope.isEmpty()
        || normalizedScope.toUtf8().size()
            > AIDurableMemoryLimits::kMaximumScopeBytes
        || !supportedScope(normalizedScope))
    {
        return {};
    }
    return QDir(rootPath_).filePath(
        scopeHash(normalizedScope) + QStringLiteral(".json"));
}

bool LocalAIDurableMemoryStore::normalizeEntries(
    const QString& scope,
    const QList<AIDurableMemoryEntry>& input,
    QList<AIDurableMemoryEntry>* output,
    QString* errorMessage)
{
    auto fail = [errorMessage](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    if (!output)
        return fail(QStringLiteral("Durable memory output is unavailable."));

    const QString normalizedScope = scope.trimmed();
    if (normalizedScope.isEmpty()
        || normalizedScope.toUtf8().size()
            > AIDurableMemoryLimits::kMaximumScopeBytes
        || !supportedScope(normalizedScope))
    {
        return fail(QStringLiteral("Durable memory scope is invalid."));
    }
    if (input.size() > AIDurableMemoryLimits::kMaximumEntries)
        return fail(QStringLiteral("Durable memory contains too many entries."));

    QList<AIDurableMemoryEntry> normalized;
    QSet<QString> ids;
    QSet<QString> identities;
    QJsonArray serialized;
    for (const AIDurableMemoryEntry& sourceEntry : input)
    {
        AIDurableMemoryEntry entry = sourceEntry;
        entry.id = entry.id.trimmed().toLower();
        entry.key = canonicalKey(entry.key);
        entry.value = entry.value.trimmed();
        entry.sourceReference = entry.sourceReference.trimmed();

        if (kindName(entry.kind).isEmpty()
            || sourceName(entry.source).isEmpty())
        {
            return fail(QStringLiteral("Durable memory entry type is invalid."));
        }
        if (!validId(entry.id))
            return fail(QStringLiteral("Durable memory entry ID is invalid."));
        if (!scopeMatchesKind(normalizedScope, entry.kind))
            return fail(QStringLiteral("Durable memory entry scope is invalid."));
        if (entry.key.isEmpty()
            || entry.key.toUtf8().size()
                > AIDurableMemoryLimits::kMaximumKeyBytes)
        {
            return fail(QStringLiteral("Durable memory entry key is invalid."));
        }
        if (entry.value.isEmpty()
            || entry.value.toUtf8().size()
                > AIDurableMemoryLimits::kMaximumValueBytes)
        {
            return fail(QStringLiteral("Durable memory entry value is invalid."));
        }
        if (entry.sourceReference.isEmpty()
            || entry.sourceReference.toUtf8().size()
                > AIDurableMemoryLimits::kMaximumSourceReferenceBytes)
        {
            return fail(
                QStringLiteral("Durable memory provenance is invalid."));
        }
        if (entry.createdAtMs <= 0
            || entry.createdAtMs > kMaximumExactJsonInteger
            || entry.updatedAtMs < entry.createdAtMs
            || entry.updatedAtMs > kMaximumExactJsonInteger
            || entry.revision <= 0
            || entry.revision > kMaximumExactJsonInteger)
        {
            return fail(
                QStringLiteral("Durable memory entry metadata is invalid."));
        }

        const QString identity =
            kindName(entry.kind) + QLatin1Char(':') + entry.key;
        if (ids.contains(entry.id))
            return fail(QStringLiteral("Durable memory entry IDs are duplicated."));
        if (identities.contains(identity))
        {
            return fail(
                QStringLiteral("Durable memory entry identities are duplicated."));
        }
        ids.insert(entry.id);
        identities.insert(identity);
        normalized.append(entry);
        serialized.append(serializeEntry(entry));
    }

    if (QJsonDocument(serialized).toJson(QJsonDocument::Compact).size()
        > AIDurableMemoryLimits::kMaximumEntriesBytes)
    {
        return fail(QStringLiteral("Durable memory entries exceed the storage limit."));
    }

    *output = normalized;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

AIDurableMemoryLoadResult LocalAIDurableMemoryStore::load(
    const QString& scope) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
        return invalidLoad(QStringLiteral("Durable memory scope is invalid."));

    QFile file(path);
    if (!file.exists())
        return {};
    if (file.size() > AIDurableMemoryLimits::kMaximumDocumentBytes)
        return invalidLoad(QStringLiteral("Stored durable memory exceeds the file limit."));
    if (!file.open(QIODevice::ReadOnly))
        return ioLoad(QStringLiteral("Stored durable memory could not be opened."));

    const QByteArray bytes =
        file.read(AIDurableMemoryLimits::kMaximumDocumentBytes + 1);
    if (bytes.size() > AIDurableMemoryLimits::kMaximumDocumentBytes)
        return invalidLoad(QStringLiteral("Stored durable memory exceeds the file limit."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalidLoad(QStringLiteral("Stored durable memory is not valid JSON."));

    const QJsonObject root = document.object();
    const QJsonValue schemaVersion =
        root.value(QStringLiteral("schema_version"));
    if (!schemaVersion.isDouble()
        || schemaVersion.toDouble()
            != static_cast<double>(AIDurableMemoryLimits::kSchemaVersion))
    {
        return invalidLoad(
            QStringLiteral("Stored durable memory schema is unsupported."));
    }
    if (root.value(QStringLiteral("scope_id")).toString()
        != scopeHash(scope))
    {
        return invalidLoad(QStringLiteral("Stored durable memory scope is invalid."));
    }
    if (!root.value(QStringLiteral("entries")).isArray())
        return invalidLoad(QStringLiteral("Stored durable memory entries are invalid."));

    QList<AIDurableMemoryEntry> entries;
    const QJsonArray serialized = root.value(QStringLiteral("entries")).toArray();
    if (serialized.size() > AIDurableMemoryLimits::kMaximumEntries)
        return invalidLoad(QStringLiteral("Durable memory contains too many entries."));
    for (const QJsonValue& value : serialized)
    {
        if (!value.isObject())
            return invalidLoad(QStringLiteral("Durable memory contains a malformed entry."));
        const QJsonObject object = value.toObject();
        const QJsonValue id = object.value(QStringLiteral("id"));
        const QJsonValue kind = object.value(QStringLiteral("kind"));
        const QJsonValue key = object.value(QStringLiteral("key"));
        const QJsonValue content = object.value(QStringLiteral("value"));
        const QJsonValue source = object.value(QStringLiteral("source"));
        const QJsonValue sourceReference =
            object.value(QStringLiteral("source_reference"));
        if (!id.isString() || !kind.isString() || !key.isString()
            || !content.isString() || !source.isString()
            || !sourceReference.isString())
        {
            return invalidLoad(
                QStringLiteral("Durable memory entry fields are invalid."));
        }

        AIDurableMemoryEntry entry;
        entry.id = id.toString();
        entry.key = key.toString();
        entry.value = content.toString();
        entry.sourceReference = sourceReference.toString();
        if (!parseKind(kind.toString(), &entry.kind)
            || !parseSource(source.toString(), &entry.source)
            || !validInteger(object.value(QStringLiteral("created_at_ms")),
                             1, &entry.createdAtMs)
            || !validInteger(object.value(QStringLiteral("updated_at_ms")),
                             1, &entry.updatedAtMs)
            || !validInteger(object.value(QStringLiteral("revision")),
                             1, &entry.revision))
        {
            return invalidLoad(
                QStringLiteral("Durable memory entry metadata is invalid."));
        }
        entries.append(entry);
    }

    QList<AIDurableMemoryEntry> normalized;
    QString validationError;
    if (!normalizeEntries(scope, entries, &normalized, &validationError))
        return invalidLoad(validationError);

    AIDurableMemoryLoadResult result;
    result.status = AIDurableMemoryLoadResult::Status::Loaded;
    result.entries = normalized;
    return result;
}

AIDurableMemoryOperationResult LocalAIDurableMemoryStore::mutate(
    const QString& scope,
    const Mutation& mutation) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Durable memory scope is invalid."));
    }
    if (!mutation)
    {
        return failure(
            QString::fromLatin1(kInvalidCode),
            QStringLiteral("Durable memory mutation is unavailable."));
    }
    if (!QDir().mkpath(rootPath_))
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral("Durable memory storage directory could not be created."));
    }

    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(kLockTimeoutMs))
    {
        return failure(
            QString::fromLatin1(kBusyCode),
            QStringLiteral("Durable memory scope is busy."));
    }

    const AIDurableMemoryLoadResult current = load(scope);
    QList<AIDurableMemoryEntry> replacement;
    bool shouldPersist = false;
    const AIDurableMemoryOperationResult mutationResult =
        mutation(current, &replacement, &shouldPersist);
    if (!mutationResult.success || !shouldPersist)
        return mutationResult;

    const AIDurableMemoryOperationResult persisted =
        replacement.isEmpty()
            ? clearUnlocked(path)
            : saveUnlocked(scope, path, replacement);
    return persisted.success ? mutationResult : persisted;
}

AIDurableMemoryOperationResult LocalAIDurableMemoryStore::saveUnlocked(
    const QString& scope,
    const QString& path,
    const QList<AIDurableMemoryEntry>& entries) const
{
    QList<AIDurableMemoryEntry> normalized;
    QString validationError;
    if (!normalizeEntries(scope, entries, &normalized, &validationError))
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       validationError);
    }

    QJsonArray serialized;
    for (const AIDurableMemoryEntry& entry : normalized)
        serialized.append(serializeEntry(entry));
    const QJsonObject root{
        {QStringLiteral("schema_version"),
         AIDurableMemoryLimits::kSchemaVersion},
        {QStringLiteral("scope_id"), scopeHash(scope)},
        {QStringLiteral("entries"), serialized}
    };
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > AIDurableMemoryLimits::kMaximumDocumentBytes)
    {
        return failure(
            QString::fromLatin1(kInvalidCode),
            QStringLiteral("Durable memory document exceeds the file limit."));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral("Durable memory storage could not be opened."));
    }
    if (file.write(bytes) != bytes.size() || !file.commit())
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral("Durable memory storage could not be committed."));
    }
    return success();
}

AIDurableMemoryOperationResult LocalAIDurableMemoryStore::save(
    const QString& scope,
    const QList<AIDurableMemoryEntry>& entries) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Durable memory scope is invalid."));
    }
    if (!QDir().mkpath(rootPath_))
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral("Durable memory storage directory could not be created."));
    }

    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(kLockTimeoutMs))
    {
        return failure(
            QString::fromLatin1(kBusyCode),
            QStringLiteral("Durable memory scope is busy."));
    }
    return saveUnlocked(scope, path, entries);
}

AIDurableMemoryOperationResult LocalAIDurableMemoryStore::clearUnlocked(
    const QString& path) const
{
    if (!QFile::exists(path) || QFile::remove(path))
        return success();
    return failure(QString::fromLatin1(kIoCode),
                   QStringLiteral("Stored durable memory could not be removed."));
}

AIDurableMemoryOperationResult LocalAIDurableMemoryStore::clear(
    const QString& scope) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Durable memory scope is invalid."));
    }
    if (!QDir().mkpath(rootPath_))
    {
        return failure(
            QString::fromLatin1(kIoCode),
            QStringLiteral("Durable memory storage directory could not be created."));
    }

    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(kLockTimeoutMs))
    {
        return failure(
            QString::fromLatin1(kBusyCode),
            QStringLiteral("Durable memory scope is busy."));
    }
    return clearUnlocked(path);
}

}
