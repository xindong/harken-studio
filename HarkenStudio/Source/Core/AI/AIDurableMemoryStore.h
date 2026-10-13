#pragma once

#include "AIDurableMemory.h"

#include <functional>

namespace VNX {

class IAIDurableMemoryStore
{
public:
    using Mutation = std::function<AIDurableMemoryOperationResult(
        const AIDurableMemoryLoadResult& current,
        QList<AIDurableMemoryEntry>* replacement,
        bool* shouldPersist)>;

    virtual ~IAIDurableMemoryStore() = default;
    virtual AIDurableMemoryLoadResult load(const QString& scope) const = 0;
    virtual AIDurableMemoryOperationResult mutate(
        const QString& scope,
        const Mutation& mutation) const = 0;
    virtual AIDurableMemoryOperationResult save(
        const QString& scope,
        const QList<AIDurableMemoryEntry>& entries) const = 0;
    virtual AIDurableMemoryOperationResult clear(
        const QString& scope) const = 0;
};

class LocalAIDurableMemoryStore final : public IAIDurableMemoryStore
{
public:
    LocalAIDurableMemoryStore();
    explicit LocalAIDurableMemoryStore(QString rootPath);

    AIDurableMemoryLoadResult load(const QString& scope) const override;
    AIDurableMemoryOperationResult mutate(
        const QString& scope,
        const Mutation& mutation) const override;
    AIDurableMemoryOperationResult save(
        const QString& scope,
        const QList<AIDurableMemoryEntry>& entries) const override;
    AIDurableMemoryOperationResult clear(
        const QString& scope) const override;

    QString storagePathForScope(const QString& scope) const;
    static QString defaultRootPath();
    static bool normalizeEntries(
        const QString& scope,
        const QList<AIDurableMemoryEntry>& input,
        QList<AIDurableMemoryEntry>* output,
        QString* errorMessage = nullptr);

private:
    static QString scopeHash(const QString& scope);
    AIDurableMemoryOperationResult saveUnlocked(
        const QString& scope,
        const QString& path,
        const QList<AIDurableMemoryEntry>& entries) const;
    AIDurableMemoryOperationResult clearUnlocked(
        const QString& path) const;

    QString rootPath_;
};

}
