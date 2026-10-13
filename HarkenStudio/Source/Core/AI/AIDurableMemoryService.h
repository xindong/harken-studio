#pragma once

#include "AIDurableMemoryStore.h"

#include <functional>
#include <memory>

namespace VNX {

class AIDurableMemoryService
{
public:
    using Clock = std::function<qint64()>;
    using IdGenerator = std::function<QString()>;

    struct WriteRequest
    {
        AIDurableMemoryKind kind = AIDurableMemoryKind::UserPreference;
        QString key;
        QString value;
        AIDurableMemorySource source = AIDurableMemorySource::User;
        QString sourceReference;
    };

    struct UpsertResult : AIDurableMemoryOperationResult
    {
        bool changed = false;
        AIDurableMemoryEntry entry;
    };

    explicit AIDurableMemoryService(
        std::shared_ptr<IAIDurableMemoryStore> store,
        Clock clock = {},
        IdGenerator idGenerator = {});

    AIDurableMemoryLoadResult list(const QString& scope) const;
    UpsertResult upsert(const QString& scope,
                        const WriteRequest& request) const;
    AIDurableMemoryOperationResult remove(const QString& scope,
                                          const QString& id) const;
    AIDurableMemoryOperationResult clear(const QString& scope) const;

private:
    static bool normalizeRequest(const QString& scope,
                                 const WriteRequest& input,
                                 WriteRequest* output,
                                 QString* errorMessage);
    static bool isValidId(const QString& id);
    static AIDurableMemoryOperationResult failure(
        const QString& code,
        const QString& message);

    std::shared_ptr<IAIDurableMemoryStore> store_;
    Clock clock_;
    IdGenerator idGenerator_;
};

}
