#include "AIDurableMemoryService.h"

#include <QDateTime>
#include <QUuid>

#include <utility>

namespace VNX {
namespace {

constexpr auto kInvalidCode = "AI_DURABLE_MEMORY_INVALID";
constexpr auto kNotFoundCode = "AI_DURABLE_MEMORY_NOT_FOUND";
constexpr auto kConflictCode = "AI_DURABLE_MEMORY_CONFLICT";
constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;

QString canonicalKey(const QString& key)
{
    return key.trimmed().toCaseFolded();
}

bool scopeMatchesKind(const QString& scope, AIDurableMemoryKind kind)
{
    if (kind == AIDurableMemoryKind::UserPreference)
        return scope.startsWith(QStringLiteral("user:"))
            && scope.size() > QStringLiteral("user:").size();
    return scope.startsWith(QStringLiteral("project:"))
        && scope.size() > QStringLiteral("project:").size();
}

AIDurableMemoryService::UpsertResult upsertFailure(
    const QString& code,
    const QString& message)
{
    AIDurableMemoryService::UpsertResult result;
    result.code = code;
    result.message = message;
    return result;
}

AIDurableMemoryOperationResult operationSuccess()
{
    AIDurableMemoryOperationResult result;
    result.success = true;
    return result;
}

}

AIDurableMemoryService::AIDurableMemoryService(
    std::shared_ptr<IAIDurableMemoryStore> store,
    Clock clock,
    IdGenerator idGenerator)
    : store_(std::move(store)),
      clock_(clock ? std::move(clock)
                   : Clock([]() {
                       return QDateTime::currentMSecsSinceEpoch();
                   })),
      idGenerator_(idGenerator ? std::move(idGenerator)
                               : IdGenerator([]() {
                                   return QUuid::createUuid()
                                       .toString(QUuid::WithoutBraces)
                                       .toLower();
                               }))
{
}

AIDurableMemoryLoadResult AIDurableMemoryService::list(
    const QString& scope) const
{
    if (!store_)
    {
        AIDurableMemoryLoadResult result;
        result.status = AIDurableMemoryLoadResult::Status::IoError;
        result.code = QStringLiteral("AI_DURABLE_MEMORY_UNAVAILABLE");
        result.message = QStringLiteral("Durable memory storage is unavailable.");
        return result;
    }
    return store_->load(scope.trimmed());
}

bool AIDurableMemoryService::normalizeRequest(
    const QString& scope,
    const WriteRequest& input,
    WriteRequest* output,
    QString* errorMessage)
{
    auto fail = [errorMessage](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    if (!output)
        return fail(QStringLiteral("Durable memory request output is unavailable."));

    if ((input.kind != AIDurableMemoryKind::UserPreference
         && input.kind != AIDurableMemoryKind::ProjectFact)
        || (input.source != AIDurableMemorySource::User
            && input.source != AIDurableMemorySource::Model))
    {
        return fail(QStringLiteral("Durable memory request type is invalid."));
    }

    const QString normalizedScope = scope.trimmed();
    if (normalizedScope.isEmpty()
        || normalizedScope.toUtf8().size()
            > AIDurableMemoryLimits::kMaximumScopeBytes
        || !scopeMatchesKind(normalizedScope, input.kind))
    {
        return fail(QStringLiteral("Durable memory request scope is invalid."));
    }

    WriteRequest normalized = input;
    normalized.key = canonicalKey(input.key);
    normalized.value = input.value.trimmed();
    normalized.sourceReference = input.sourceReference.trimmed();
    if (normalized.key.isEmpty()
        || normalized.key.toUtf8().size()
            > AIDurableMemoryLimits::kMaximumKeyBytes)
    {
        return fail(QStringLiteral("Durable memory request key is invalid."));
    }
    if (normalized.value.isEmpty()
        || normalized.value.toUtf8().size()
            > AIDurableMemoryLimits::kMaximumValueBytes)
    {
        return fail(QStringLiteral("Durable memory request value is invalid."));
    }
    if (normalized.sourceReference.isEmpty()
        || normalized.sourceReference.toUtf8().size()
            > AIDurableMemoryLimits::kMaximumSourceReferenceBytes)
    {
        return fail(QStringLiteral("Durable memory request provenance is invalid."));
    }

    *output = normalized;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

bool AIDurableMemoryService::isValidId(const QString& id)
{
    const QString normalized = id.trimmed().toLower();
    const QUuid uuid(normalized);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).toLower() == normalized;
}

AIDurableMemoryService::UpsertResult AIDurableMemoryService::upsert(
    const QString& scope,
    const WriteRequest& request) const
{
    if (!store_)
    {
        return upsertFailure(
            QStringLiteral("AI_DURABLE_MEMORY_UNAVAILABLE"),
            QStringLiteral("Durable memory storage is unavailable."));
    }

    WriteRequest normalizedRequest;
    QString validationError;
    if (!normalizeRequest(scope, request, &normalizedRequest,
                          &validationError))
    {
        return upsertFailure(QString::fromLatin1(kInvalidCode),
                             validationError);
    }

    const QString normalizedScope = scope.trimmed();
    UpsertResult result;
    const AIDurableMemoryOperationResult transaction = store_->mutate(
        normalizedScope,
        [&](const AIDurableMemoryLoadResult& loaded,
            QList<AIDurableMemoryEntry>* replacement,
            bool* shouldPersist) {
            if (loaded.status == AIDurableMemoryLoadResult::Status::Invalid
                || loaded.status == AIDurableMemoryLoadResult::Status::IoError)
            {
                return failure(loaded.code, loaded.message);
            }

            QList<AIDurableMemoryEntry> entries =
                loaded.status == AIDurableMemoryLoadResult::Status::Loaded
                    ? loaded.entries
                    : QList<AIDurableMemoryEntry>{};
            for (AIDurableMemoryEntry& entry : entries)
            {
                if (entry.kind != normalizedRequest.kind
                    || entry.key != normalizedRequest.key)
                {
                    continue;
                }

                if (entry.value == normalizedRequest.value)
                {
                    result.success = true;
                    result.entry = entry;
                    return operationSuccess();
                }

                const qint64 now = clock_();
                if (now <= 0 || now > kMaximumExactJsonInteger
                    || now < entry.updatedAtMs
                    || entry.revision >= kMaximumExactJsonInteger)
                {
                    return failure(
                        QString::fromLatin1(kConflictCode),
                        QStringLiteral(
                            "Durable memory revision metadata is invalid."));
                }

                entry.value = normalizedRequest.value;
                entry.source = normalizedRequest.source;
                entry.sourceReference =
                    normalizedRequest.sourceReference;
                entry.updatedAtMs = now;
                ++entry.revision;
                *replacement = entries;
                *shouldPersist = true;
                result.success = true;
                result.changed = true;
                result.entry = entry;
                return operationSuccess();
            }

            if (entries.size() >= AIDurableMemoryLimits::kMaximumEntries)
            {
                return failure(
                    QString::fromLatin1(kConflictCode),
                    QStringLiteral(
                        "Durable memory scope has reached its entry limit."));
            }

            const qint64 now = clock_();
            const QString id = idGenerator_().trimmed().toLower();
            if (now <= 0 || now > kMaximumExactJsonInteger
                || !isValidId(id))
            {
                return failure(
                    QString::fromLatin1(kConflictCode),
                    QStringLiteral(
                        "Durable memory identity metadata is invalid."));
            }
            for (const AIDurableMemoryEntry& existing : entries)
            {
                if (existing.id == id)
                {
                    return failure(
                        QString::fromLatin1(kConflictCode),
                        QStringLiteral(
                            "Durable memory entry ID already exists."));
                }
            }

            AIDurableMemoryEntry entry;
            entry.id = id;
            entry.kind = normalizedRequest.kind;
            entry.key = normalizedRequest.key;
            entry.value = normalizedRequest.value;
            entry.source = normalizedRequest.source;
            entry.sourceReference = normalizedRequest.sourceReference;
            entry.createdAtMs = now;
            entry.updatedAtMs = now;
            entry.revision = 1;
            entries.append(entry);
            *replacement = entries;
            *shouldPersist = true;
            result.success = true;
            result.changed = true;
            result.entry = entry;
            return operationSuccess();
        });
    if (!transaction.success)
        return upsertFailure(transaction.code, transaction.message);
    return result;
}

AIDurableMemoryOperationResult AIDurableMemoryService::remove(
    const QString& scope,
    const QString& id) const
{
    if (!store_)
    {
        return failure(
            QStringLiteral("AI_DURABLE_MEMORY_UNAVAILABLE"),
            QStringLiteral("Durable memory storage is unavailable."));
    }

    const QString normalizedScope = scope.trimmed();
    const QString normalizedId = id.trimmed().toLower();
    if (!isValidId(normalizedId))
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Durable memory entry ID is invalid."));
    }

    return store_->mutate(
        normalizedScope,
        [&](const AIDurableMemoryLoadResult& loaded,
            QList<AIDurableMemoryEntry>* replacement,
            bool* shouldPersist) {
            if (loaded.status == AIDurableMemoryLoadResult::Status::Invalid
                || loaded.status == AIDurableMemoryLoadResult::Status::IoError)
            {
                return failure(loaded.code, loaded.message);
            }
            if (loaded.status == AIDurableMemoryLoadResult::Status::Missing)
            {
                return failure(
                    QString::fromLatin1(kNotFoundCode),
                    QStringLiteral("Durable memory entry was not found."));
            }

            QList<AIDurableMemoryEntry> entries = loaded.entries;
            qsizetype index = -1;
            for (qsizetype candidate = 0;
                 candidate < entries.size();
                 ++candidate)
            {
                if (entries[candidate].id == normalizedId)
                {
                    index = candidate;
                    break;
                }
            }
            if (index < 0)
            {
                return failure(
                    QString::fromLatin1(kNotFoundCode),
                    QStringLiteral("Durable memory entry was not found."));
            }

            entries.removeAt(index);
            *replacement = entries;
            *shouldPersist = true;
            return operationSuccess();
        });
}

AIDurableMemoryOperationResult AIDurableMemoryService::clear(
    const QString& scope) const
{
    if (!store_)
    {
        return failure(
            QStringLiteral("AI_DURABLE_MEMORY_UNAVAILABLE"),
            QStringLiteral("Durable memory storage is unavailable."));
    }
    return store_->clear(scope.trimmed());
}

AIDurableMemoryOperationResult AIDurableMemoryService::failure(
    const QString& code,
    const QString& message)
{
    AIDurableMemoryOperationResult result;
    result.code = code;
    result.message = message;
    return result;
}

}
