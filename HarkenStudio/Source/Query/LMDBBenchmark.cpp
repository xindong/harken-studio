
#include "LMDBBenchmark.h"
#include "LMDBService.h"
#include "LMDBIndexer.h"
#include "LMDBQueryPipeline.h"
#include "../Core/ProjectTypes.h"

#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <random>

namespace VNX {


QJsonObject LMDBBenchmark::BenchmarkResult::toJson() const
{
    return QJsonObject{
        {"objectCount",        objectCount},
        {"rebuildMs",          rebuildMs},
        {"wordSearchMs",       wordSearchMs},
        {"phraseSearchMs",     phraseSearchMs},
        {"trigramSearchMs",    trigramSearchMs},
        {"incrementalIndexMs", incrementalIndexMs},
        {"boundedBatchMs",     boundedBatchMs},
        {"wordResultCount",    wordResultCount},
        {"phraseResultCount",  phraseResultCount},
        {"trigramResultCount", trigramResultCount},
        {"boundedBatchCount",  boundedBatchCount},
        {"complete",           complete}
    };
}


static int countItems(const ProjectItem* root)
{
    if (!root) return 0;
    int count = 1;
    for (const auto& c : root->children)
        count += countItems(c.get());
    return count;
}

static int countProjectItems(ProjectData* project)
{
    int total = 0;
    for (const auto* binding : project->allFileBindings()) {
        if (binding && binding->rootItem)
            total += countItems(binding->rootItem.get());
    }
    return total;
}

static std::vector<SearchResult> runPipelineSync(LMDBService* service,
                                                  const QString& query,
                                                  int maxResults = 100)
{
    LMDBQueryPipeline pipeline(service);
    pipeline.setQuery(query);
    pipeline.setMaxResults(maxResults);

    constexpr int kMaxIter = 10000;
    for (int i = 0; i < kMaxIter && !pipeline.isFinished(); ++i) {
        auto st = pipeline.step(256);
        if (st == PipelineStatus::Error)
            break;
    }
    return pipeline.results();
}


LMDBBenchmark::BenchmarkResult
LMDBBenchmark::run(LMDBService* service, LMDBIndexer* indexer,
                   ProjectData* project)
{
    BenchmarkResult result;
    if (!service || !service->isOpen() || !indexer || !project)
        return result;

    result.objectCount = countProjectItems(project);

    QElapsedTimer timer;

    timer.start();
    result.complete = indexer->rebuild(project);
    result.rebuildMs = timer.nsecsElapsed() / 1.0e6;

    timer.restart();
    auto wordRes = runPipelineSync(service, "sound");
    result.wordSearchMs    = timer.nsecsElapsed() / 1.0e6;
    result.wordResultCount = static_cast<int>(wordRes.size());

    timer.restart();
    auto phraseRes = runPipelineSync(service, "\"weapon shot\"");
    result.phraseSearchMs    = timer.nsecsElapsed() / 1.0e6;
    result.phraseResultCount = static_cast<int>(phraseRes.size());

    timer.restart();
    auto triRes = runPipelineSync(service, "weapn");
    result.trigramSearchMs    = timer.nsecsElapsed() / 1.0e6;
    result.trigramResultCount = static_cast<int>(triRes.size());

    qDebug() << "[LMDBBenchmark::run] N=" << result.objectCount
             << "rebuild=" << result.rebuildMs << "ms"
             << "word=" << result.wordSearchMs << "ms"
             << "phrase=" << result.phraseSearchMs << "ms"
             << "trigram=" << result.trigramSearchMs << "ms";

    return result;
}


LMDBBenchmark::BenchmarkResult
LMDBBenchmark::runSynthetic(int objectCount)
{
    BenchmarkResult result;
    result.objectCount = objectCount;

    auto service = LMDBService::Create();
    if (!service) return result;

    QString tmpPath = QDir::temp().filePath(
        QString("harken_bench_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    QDir().mkpath(tmpPath);
    if (!service->open(tmpPath, 128))
        return result;

    LMDBIndexer indexer(service.get());

    static const char* prefixes[] = {"SFX", "Amb", "Music", "Voice", "UI"};
    static const char* middles[]  = {"Weapon", "Forest", "Battle", "Menu",
                                     "Dialog", "Vehicle", "Weather"};
    static const char* suffixes[] = {"Shot", "Birds", "Rain", "Click",
                                     "Walk", "Run", "Explode"};
    constexpr int nPre = 5, nMid = 7, nSuf = 7;

    ProjectData syntheticProject;
    auto syntheticRoot = std::make_shared<ProjectItem>();
    syntheticRoot->name = "Root";
    syntheticRoot->type = ProjectItem::Type::None;
    syntheticRoot->objectID = 1;

    std::mt19937 rng(42);
    syntheticRoot->children.reserve(static_cast<size_t>(objectCount));

    for (int i = 0; i < objectCount; ++i) {
        auto child = std::make_shared<ProjectItem>();
        child->objectID = static_cast<uint32_t>(100 + i);
        child->type     = ProjectItem::Type::Sound;
        child->name     = QString("%1 %2 %3 %4")
                              .arg(prefixes[rng() % nPre])
                              .arg(middles[rng() % nMid])
                              .arg(suffixes[rng() % nSuf])
                              .arg(i);
        child->parent   = syntheticRoot.get();
        syntheticRoot->children.push_back(child);
    }
    {
        ProjectFileBinding binding;
        binding.rootItem     = syntheticRoot;
        binding.parentFolder = QStringLiteral("Layers");
        syntheticProject.layers.push_back(std::move(binding));
    }

    QElapsedTimer timer;

    timer.start();
    if (!indexer.rebuild(&syntheticProject)) {
        service->close();
        QDir(tmpPath).removeRecursively();
        return result;
    }
    result.rebuildMs = timer.nsecsElapsed() / 1.0e6;

    timer.restart();
    auto wordRes = runPipelineSync(service.get(), "weapon");
    result.wordSearchMs    = timer.nsecsElapsed() / 1.0e6;
    result.wordResultCount = static_cast<int>(wordRes.size());

    timer.restart();
    auto phraseRes = runPipelineSync(service.get(), "\"weapon shot\"");
    result.phraseSearchMs    = timer.nsecsElapsed() / 1.0e6;
    result.phraseResultCount = static_cast<int>(phraseRes.size());

    timer.restart();
    auto triRes = runPipelineSync(service.get(), "weapn");
    result.trigramSearchMs    = timer.nsecsElapsed() / 1.0e6;
    result.trigramResultCount = static_cast<int>(triRes.size());

    {
        auto newItem = std::make_shared<ProjectItem>();
        newItem->objectID = 999999;
        newItem->name     = "Benchmark_Incremental_Test";
        newItem->type     = ProjectItem::Type::Sound;

        timer.restart();
        indexer.indexObject(newItem.get());
        result.incrementalIndexMs = timer.nsecsElapsed() / 1.0e6;
    }

    constexpr int kBoundedBatchCount = 256;
    bool batchOk = true;
    timer.restart();
    for (int i = 0; i < kBoundedBatchCount; ++i) {
        ProjectItem item;
        item.objectID = static_cast<uint32_t>(1000000 + i);
        item.name = QString("Batch_Weapon_Shot_%1").arg(i);
        item.type = ProjectItem::Type::Sound;
        batchOk = indexer.indexObject(&item) && batchOk;
    }
    result.boundedBatchMs = timer.nsecsElapsed() / 1.0e6;
    result.boundedBatchCount = batchOk ? kBoundedBatchCount : 0;
    result.complete = batchOk;

    service->close();
    QDir(tmpPath).removeRecursively();

    qDebug() << "[LMDBBenchmark::runSynthetic] N=" << objectCount
             << "rebuild=" << result.rebuildMs << "ms"
             << "word=" << result.wordSearchMs << "ms (" << result.wordResultCount << "hits)"
             << "phrase=" << result.phraseSearchMs << "ms (" << result.phraseResultCount << "hits)"
              << "trigram=" << result.trigramSearchMs << "ms (" << result.trigramResultCount << "hits)"
              << "incremental=" << result.incrementalIndexMs << "ms"
              << "batch=" << result.boundedBatchMs << "ms";

    return result;
}

}
