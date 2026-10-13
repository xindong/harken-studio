#pragma once

#include "AIDurableMemory.h"

#include <QList>
#include <QString>

namespace VNX {

struct AIDurableMemoryCandidate
{
    QString scope;
    AIDurableMemoryEntry entry;
};

struct AIDurableMemoryRetrievalQuery
{
    QString userText;
    int maximumContextBytes = 0;
    int maximumEntries = 0;
};

struct AIDurableMemoryRetrievalItem
{
    QString scope;
    AIDurableMemoryEntry entry;
    int relevanceScore = 0;
};

struct AIDurableMemoryRetrievalResult
{
    QList<AIDurableMemoryRetrievalItem> items;
    QString contextBlock;
    int omittedCount = 0;
};

class IAIDurableMemoryRetriever
{
public:
    virtual ~IAIDurableMemoryRetriever() = default;
    virtual AIDurableMemoryRetrievalResult retrieve(
        const AIDurableMemoryRetrievalQuery& query,
        const QList<AIDurableMemoryCandidate>& candidates) const = 0;
};

class DeterministicAIDurableMemoryRetriever final
    : public IAIDurableMemoryRetriever
{
public:
    AIDurableMemoryRetrievalResult retrieve(
        const AIDurableMemoryRetrievalQuery& query,
        const QList<AIDurableMemoryCandidate>& candidates) const override;
};

QString buildDurableMemoryContextBlock(
    const QList<AIDurableMemoryRetrievalItem>& items);

}
