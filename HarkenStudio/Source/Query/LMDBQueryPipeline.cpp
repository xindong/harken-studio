
#include "LMDBQueryPipeline.h"
#include "LMDBService.h"
#include "LMDBIndexer.h"
#include "../Core/ItemTypeCatalog.h"

#include <QRegularExpression>
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace VNX {

uint64_t LMDBQueryPipeline::fnv1aHash(const QString& str)
{
    return LMDBIndexer::fnv1aHashPublic(str);
}


std::vector<uint32_t> LMDBQueryPipeline::unpackIDList(const void* data, size_t size)
{
    size_t count = size / sizeof(uint32_t);
    std::vector<uint32_t> ids(count);
    if (count > 0)
        std::memcpy(ids.data(), data, count * sizeof(uint32_t));
    return ids;
}


QString LMDBQueryPipeline::typeNameFromValue(uint16_t typeVal)
{
    return ItemTypeCatalog::canonicalName(static_cast<ProjectItem::Type>(typeVal));
}


LMDBQueryPipeline::LMDBQueryPipeline(LMDBService* service)
    : service_(service)
{
}


void LMDBQueryPipeline::setQuery(const QString& searchText)
{
    rawQuery_ = searchText;
    stage_    = 0;
    parsed_   = ParsedQuery();
    candidateIDs_.clear();
    filteredIDs_.clear();
    finalResults_.clear();
}


QString LMDBQueryPipeline::stageName() const
{
    switch (stage_) {
    case 0:  return QStringLiteral("ParseQuery");
    case 1:  return QStringLiteral("IndexSearch");
    case 2:  return QStringLiteral("PhraseSearch");
    case 3:  return QStringLiteral("ExclusionSearch");
    case 4:  return QStringLiteral("FilterMetadata");
    case 5:  return QStringLiteral("RankResults");
    default: return QStringLiteral("Finished");
    }
}


PipelineStatus LMDBQueryPipeline::step(int maxItems)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(maxItems);
    stage_ = 6;
    return PipelineStatus::Finished;
#else
    if (stage_ >= 6)
        return PipelineStatus::Finished;

    if (!service_ || !service_->isOpen()) {
        stage_ = 6;
        return PipelineStatus::Error;
    }

    Q_UNUSED(maxItems);

    PipelineStatus status = PipelineStatus::Error;

    switch (stage_) {
    case 0: status = processSearchString(); break;
    case 1: status = indexSearch();          break;
    case 2: status = phraseSearch();         break;
    case 3: status = exclusionSearch();      break;
    case 4: status = filterMetadata();       break;
    case 5: status = rankResults();          break;
    }

    if (status == PipelineStatus::Error) {
        stage_ = 6;
        return PipelineStatus::Error;
    }

    ++stage_;
    return (stage_ >= 6) ? PipelineStatus::Finished : PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::processSearchString()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    parsed_ = ParsedQuery();

    if (rawQuery_.trimmed().isEmpty())
        return PipelineStatus::Error;

    static const QRegularExpression rx(
        QStringLiteral("([-!]?)\"([^\"]+)\"|([-!]?)(\\S+)"));

    auto it = rx.globalMatch(rawQuery_);
    while (it.hasNext()) {
        auto match = it.next();

        if (match.capturedLength(2) > 0) {
            bool    exclude = !match.captured(1).isEmpty();
            QString phrase  = match.captured(2).toLower().trimmed();

            if (exclude) {
                for (const QString& w : phrase.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    QStringList subTokens = LMDBIndexer::tokenize(w);
                    for (const QString& t : subTokens)
                        parsed_.excludeTokens.append(t);
                }
            } else {
                parsed_.phraseTokens.append(phrase);
                for (const QString& w : phrase.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    QStringList subTokens = LMDBIndexer::tokenize(w);
                    for (const QString& t : subTokens) {
                        if (!parsed_.normalTokens.contains(t))
                            parsed_.normalTokens.append(t);
                    }
                }
            }
        } else if (match.capturedLength(4) > 0) {
            bool    exclude = !match.captured(3).isEmpty();
            QString word    = match.captured(4).toLower();

            QStringList subTokens = LMDBIndexer::tokenize(word);
            if (exclude) {
                for (const QString& t : subTokens)
                    parsed_.excludeTokens.append(t);
            } else {
                for (const QString& t : subTokens) {
                    if (!parsed_.normalTokens.contains(t))
                        parsed_.normalTokens.append(t);
                }
            }
        }
    }

    if (parsed_.normalTokens.isEmpty() && parsed_.phraseTokens.isEmpty()
        && parsed_.excludeTokens.isEmpty())
        return PipelineStatus::Error;

    return PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::indexSearch()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    candidateIDs_.clear();

    bool firstToken = true;
    for (const QString& token : parsed_.normalTokens) {
        auto ids = lookupInvertedIndex(token);
        if (firstToken) {
            candidateIDs_ = std::move(ids);
            firstToken = false;
        } else {
            candidateIDs_ = intersectSorted(candidateIDs_, ids);
        }
    }

    if (candidateIDs_.empty() && rawQuery_.trimmed().size() >= 3) {
        parsed_.useTrigram = true;
        trigramSearch();
    }

    return PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::phraseSearch()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    if (parsed_.phraseTokens.isEmpty() || candidateIDs_.empty())
        return PipelineStatus::Continue;

    auto txn = service_->beginRead();
    if (!txn.valid())
        return PipelineStatus::Error;

    std::vector<uint32_t> matched;
    matched.reserve(candidateIDs_.size());

    for (uint32_t oid : candidateIDs_) {
        void*  data = nullptr;
        size_t size = 0;
        int rc = txn.get(LMDBService::Object, &oid, sizeof(oid), &data, &size);
        if (rc != 0 || !data || size < 8)
            continue;

        const char* p = static_cast<const char*>(data);
        p += 4;
        p += 2;
        uint16_t nameLen = 0;
        std::memcpy(&nameLen, p, 2);
        p += 2;

        if (nameLen == 0 || static_cast<size_t>(8 + nameLen) > size)
            continue;

        QString nameLower = QString::fromUtf8(p, nameLen).toLower();

        bool allMatch = true;
        for (const QString& phrase : parsed_.phraseTokens) {
            if (!nameLower.contains(phrase)) {
                allMatch = false;
                break;
            }
        }

        if (allMatch)
            matched.push_back(oid);
    }

    candidateIDs_ = std::move(matched);
    return PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::exclusionSearch()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    if (parsed_.excludeTokens.isEmpty() || candidateIDs_.empty())
        return PipelineStatus::Continue;

    for (const QString& token : parsed_.excludeTokens) {
        auto excludeIDs = lookupInvertedIndex(token);
        candidateIDs_ = differenceSorted(candidateIDs_, excludeIDs);
    }

    return PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::filterMetadata()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    filteredIDs_ = candidateIDs_;
    finalResults_.clear();
    finalResults_.reserve(filteredIDs_.size());

    if (filteredIDs_.empty())
        return PipelineStatus::Continue;

    auto txn = service_->beginRead();
    if (!txn.valid())
        return PipelineStatus::Error;

    for (uint32_t oid : filteredIDs_) {
        void*  data = nullptr;
        size_t size = 0;
        int rc = txn.get(LMDBService::Object, &oid, sizeof(oid), &data, &size);
        if (rc != 0 || !data || size < 8)
            continue;

        const char* p = static_cast<const char*>(data);
        p += 4;

        uint16_t typeVal = 0;
        std::memcpy(&typeVal, p, 2);
        p += 2;

        uint16_t nameLen = 0;
        std::memcpy(&nameLen, p, 2);
        p += 2;

        SearchResult result;
        result.objectID = oid;
        result.type     = typeNameFromValue(typeVal);

        if (nameLen > 0 && static_cast<size_t>(8 + nameLen) <= size)
            result.name = QString::fromUtf8(p, nameLen);

        finalResults_.push_back(std::move(result));
    }

    return PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::rankResults()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    if (finalResults_.empty())
        return PipelineStatus::Continue;

    QString queryLower  = rawQuery_.toLower().trimmed();
    int     totalTokens = parsed_.normalTokens.size();

    for (auto& result : finalResults_) {
        float   score     = 0.0f;
        QString nameLower = result.name.toLower();

        if (nameLower == queryLower)
            score += 10.0f;

        if (nameLower.startsWith(queryLower))
            score += 5.0f;

        if (totalTokens > 0) {
            int matched = 0;
            for (const QString& token : parsed_.normalTokens) {
                if (nameLower.contains(token))
                    ++matched;
            }
            score += 3.0f * static_cast<float>(matched)
                          / static_cast<float>(totalTokens);
        }

        score += 1.0f / static_cast<float>(result.name.size() + 1);

        result.score = score;
    }

    std::sort(finalResults_.begin(), finalResults_.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  return a.score > b.score;
              });

    if (static_cast<int>(finalResults_.size()) > maxResults_)
        finalResults_.resize(maxResults_);

    return PipelineStatus::Continue;
#endif
}


PipelineStatus LMDBQueryPipeline::trigramSearch()
{
#ifndef HARKEN_HAS_LMDB
    return PipelineStatus::Error;
#else
    auto txn = service_->beginRead();
    if (!txn.valid())
        return PipelineStatus::Error;

    QStringList trigrams;
    for (const QString& token : parsed_.normalTokens) {
        QStringList tokenTrigrams = LMDBIndexer::generateTrigrams(token);
        for (const QString& tri : tokenTrigrams) {
            if (!trigrams.contains(tri))
                trigrams.append(tri);
        }
    }

    if (trigrams.isEmpty())
        return PipelineStatus::Continue;

    std::unordered_map<uint32_t, int> hitCount;

    for (const QString& tri : trigrams) {
        uint64_t h    = fnv1aHash(tri);
        void*    data = nullptr;
        size_t   size = 0;
        int rc = txn.get(LMDBService::Trigram, &h, sizeof(h), &data, &size);
        if (rc != 0 || !data)
            continue;

        auto ids = unpackIDList(data, size);
        for (uint32_t id : ids)
            ++hitCount[id];
    }

    float threshold = 0.6f * static_cast<float>(trigrams.size());
    std::vector<uint32_t> trigramIDs;
    trigramIDs.reserve(hitCount.size());

    for (const auto& [id, count] : hitCount) {
        if (static_cast<float>(count) >= threshold)
            trigramIDs.push_back(id);
    }

    std::sort(trigramIDs.begin(), trigramIDs.end());
    candidateIDs_ = unionSorted(candidateIDs_, trigramIDs);

    return PipelineStatus::Continue;
#endif
}


std::vector<uint32_t> LMDBQueryPipeline::lookupInvertedIndex(const QString& token)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(token);
    return {};
#else
    auto txn = service_->beginRead();
    if (!txn.valid())
        return {};

    uint64_t h    = fnv1aHash(token);
    void*    data = nullptr;
    size_t   size = 0;
    int rc = txn.get(LMDBService::Inverted, &h, sizeof(h), &data, &size);
    if (rc != 0 || !data)
        return {};

    return unpackIDList(data, size);
#endif
}


std::vector<uint32_t> LMDBQueryPipeline::intersectSorted(
    const std::vector<uint32_t>& a,
    const std::vector<uint32_t>& b)
{
    std::vector<uint32_t> result;
    result.reserve(std::min(a.size(), b.size()));
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            result.push_back(a[i]);
            ++i; ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}


std::vector<uint32_t> LMDBQueryPipeline::unionSorted(
    const std::vector<uint32_t>& a,
    const std::vector<uint32_t>& b)
{
    std::vector<uint32_t> result;
    result.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            result.push_back(a[i]);
            ++i; ++j;
        } else if (a[i] < b[j]) {
            result.push_back(a[i++]);
        } else {
            result.push_back(b[j++]);
        }
    }
    while (i < a.size()) result.push_back(a[i++]);
    while (j < b.size()) result.push_back(b[j++]);
    return result;
}


std::vector<uint32_t> LMDBQueryPipeline::differenceSorted(
    const std::vector<uint32_t>& base,
    const std::vector<uint32_t>& exclude)
{
    std::vector<uint32_t> result;
    result.reserve(base.size());
    size_t i = 0, j = 0;
    while (i < base.size()) {
        if (j < exclude.size() && base[i] == exclude[j]) {
            ++i; ++j;
        } else if (j < exclude.size() && base[i] > exclude[j]) {
            ++j;
        } else {
            result.push_back(base[i++]);
        }
    }
    return result;
}

}
