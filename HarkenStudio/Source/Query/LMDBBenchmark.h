
#pragma once

#include <QJsonObject>

namespace VNX {

class LMDBService;
class LMDBIndexer;
struct ProjectData;

class LMDBBenchmark {
public:
    struct BenchmarkResult {
        int    objectCount        = 0;
        double rebuildMs          = 0.0;
        double wordSearchMs       = 0.0;
        double phraseSearchMs     = 0.0;
        double trigramSearchMs    = 0.0;
        double incrementalIndexMs = 0.0;
        double boundedBatchMs     = 0.0;
        int    wordResultCount    = 0;
        int    phraseResultCount  = 0;
        int    trigramResultCount = 0;
        int    boundedBatchCount  = 0;
        bool   complete           = false;

        QJsonObject toJson() const;
    };

    static BenchmarkResult run(LMDBService* service, LMDBIndexer* indexer,
                               ProjectData* project);

    static BenchmarkResult runSynthetic(int objectCount);
};

}
