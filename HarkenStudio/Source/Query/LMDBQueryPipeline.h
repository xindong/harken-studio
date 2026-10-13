
#pragma once

#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>

namespace VNX {

class LMDBService;

struct SearchResult {
    uint32_t objectID = 0;
    QString  name;
    QString  type;
    float    score = 0.0f;
};

enum class PipelineStatus { Continue, Finished, Error };

class LMDBQueryPipeline {
public:
    explicit LMDBQueryPipeline(LMDBService* service);

    void setQuery(const QString& searchText);
    void setMaxResults(int maxResults) { maxResults_ = maxResults; }

    PipelineStatus step(int maxItems = 32);
    bool isFinished() const { return stage_ >= 6; }

    const std::vector<SearchResult>& results() const { return finalResults_; }
    int     currentStage() const { return stage_; }
    QString stageName() const;

private:
    PipelineStatus processSearchString();
    PipelineStatus indexSearch();
    PipelineStatus phraseSearch();
    PipelineStatus exclusionSearch();
    PipelineStatus filterMetadata();
    PipelineStatus rankResults();
    PipelineStatus trigramSearch();

    struct ParsedQuery {
        QStringList normalTokens;
        QStringList phraseTokens;
        QStringList excludeTokens;
        bool useTrigram = false;
    };

    ParsedQuery parsed_;
    QString     rawQuery_;

    std::vector<uint32_t>     candidateIDs_;
    std::vector<uint32_t>     filteredIDs_;
    std::vector<SearchResult> finalResults_;

    struct ScoredCandidate {
        uint32_t objectID;
        float    score;
        bool operator<(const ScoredCandidate& o) const { return score < o.score; }
    };

    LMDBService* service_;
    int stage_      = 0;
    int maxResults_ = 100;

    std::vector<uint32_t> lookupInvertedIndex(const QString& token);

    static std::vector<uint32_t> intersectSorted(const std::vector<uint32_t>& a,
                                                 const std::vector<uint32_t>& b);
    static std::vector<uint32_t> unionSorted(const std::vector<uint32_t>& a,
                                             const std::vector<uint32_t>& b);
    static std::vector<uint32_t> differenceSorted(const std::vector<uint32_t>& base,
                                                  const std::vector<uint32_t>& exclude);

    static uint64_t              fnv1aHash(const QString& str);
    static std::vector<uint32_t> unpackIDList(const void* data, size_t size);
    static QString               typeNameFromValue(uint16_t typeVal);
};

}
