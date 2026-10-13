#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

namespace VNX {

enum class AIDurableMemoryKind
{
    UserPreference,
    ProjectFact
};

enum class AIDurableMemorySource
{
    User,
    Model
};

struct AIDurableMemoryEntry
{
    QString id;
    AIDurableMemoryKind kind = AIDurableMemoryKind::UserPreference;
    QString key;
    QString value;
    AIDurableMemorySource source = AIDurableMemorySource::User;
    QString sourceReference;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    qint64 revision = 0;
};

struct AIDurableMemoryLimits
{
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMaximumEntries = 512;
    static constexpr int kMaximumScopeBytes = 4096;
    static constexpr int kMaximumKeyBytes = 256;
    static constexpr int kMaximumValueBytes = 8192;
    static constexpr int kMaximumSourceReferenceBytes = 1024;
    static constexpr int kMaximumEntriesBytes = 196608;
    static constexpr int kMaximumDocumentBytes = 262144;
};

struct AIDurableMemoryOperationResult
{
    bool success = false;
    QString code;
    QString message;
};

struct AIDurableMemoryLoadResult
{
    enum class Status
    {
        Missing,
        Loaded,
        Invalid,
        IoError
    };

    Status status = Status::Missing;
    QList<AIDurableMemoryEntry> entries;
    QString code;
    QString message;
};

}
