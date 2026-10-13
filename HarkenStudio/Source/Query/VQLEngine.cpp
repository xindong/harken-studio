
#include "VQLEngine.h"
#include "VQLLexer.h"
#include "VQLParser.h"
#include "VQLEvaluator.h"
#include "VQLFieldCatalog.h"
#include "../Core/ItemTypeCatalog.h"
#include <QDebug>

#ifdef HARKEN_HAS_LMDB
#include "../Core/ProjectTypes.h"
#include "../Core/AssetPipeline/PathUtils.h"
#include <QFileInfo>
#include <QThreadPool>
#endif

namespace VNX {

namespace {

bool validatePredicate(const VQLPredicatePtr& predicate, VQLError* outError)
{
    if (!predicate) return true;
    if (predicate->kind == VQLPredicate::Kind::Comparison
        && !VQLFieldCatalog::isKnownField(predicate->fieldName)) {
        if (outError)
            outError->message = QStringLiteral("Unknown VQL field '%1'")
                                    .arg(predicate->fieldName);
        return false;
    }
    return validatePredicate(predicate->left, outError)
        && validatePredicate(predicate->right, outError);
}

bool validateQuery(const VQLQueryAST& query, VQLError* outError)
{
    for (const VQLPathStep& step : query.pathSteps) {
        if (step.kind != VQLPathStep::Kind::TypeFilter) continue;
        if (!ItemTypeCatalog::parseTypeName(step.value, Qt::CaseInsensitive, false)) {
            if (outError)
                outError->message = QStringLiteral("Unknown VQL type '%1'").arg(step.value);
            return false;
        }
    }
    if (!validatePredicate(query.predicate, outError)) return false;
    for (const QString& field : query.returnFields) {
        if (!VQLFieldCatalog::isKnownField(field)) {
            if (outError)
                outError->message = QStringLiteral("Unknown VQL field '%1'").arg(field);
            return false;
        }
    }
    for (const VQLAggregate& aggregate : query.aggregates) {
        if (aggregate.fieldName != QLatin1String("*")
            && !VQLFieldCatalog::isKnownField(aggregate.fieldName)) {
            if (outError)
                outError->message = QStringLiteral("Unknown VQL field '%1'")
                                        .arg(aggregate.fieldName);
            return false;
        }
    }
    return true;
}

}

VQLEngine::VQLEngine()
    : observer_(std::make_unique<VQLObserver>(this))
{
}

void VQLEngine::setProject(ProjectData* project)
{
    project_ = project;
    memoryDataSource_.setProject(project);

#ifdef HARKEN_HAS_LMDB
    try {
        initLMDB();
        if (project && lmdbInitialized_)
            rebuildSearchIndex();
    } catch (const std::exception& e) {
        qWarning() << "[VQL] LMDB init/rebuild failed (non-fatal):" << e.what();
        lmdbInitialized_ = false;
        lmdbService_.reset();
        lmdbIndexer_.reset();
        lmdbPath_.clear();
    } catch (...) {
        qWarning() << "[VQL] LMDB init/rebuild crashed (non-fatal), disabling LMDB search";
        lmdbInitialized_ = false;
        lmdbService_.reset();
        lmdbIndexer_.reset();
        lmdbPath_.clear();
    }
#endif
}


VQLCompiledQuery VQLEngine::compile(const QString& vql, VQLError* outError)
{
    uint32_t hash = vqlFnv1a(vql);

    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        lruOrder_.erase(it->second.lruIter);
        lruOrder_.push_front(hash);
        it->second.lruIter = lruOrder_.begin();
        return it->second.query;
    }

    VQLError lexError;
    VQLLexer lexer(vql);
    auto tokens = lexer.tokenize(&lexError);
    if (lexError.isError()) {
        if (outError) *outError = lexError;
        qWarning() << "[VQL] Lex error:" << lexError.toString();
        return nullptr;
    }

    VQLError parseError;
    VQLParser parser(std::move(tokens));
    auto compiled = parser.parse(&parseError);
    if (!compiled || parseError.isError()) {
        if (outError) *outError = parseError;
        qWarning() << "[VQL] Parse error:" << parseError.toString();
        return nullptr;
    }

    VQLError validationError;
    if (!validateQuery(*compiled, &validationError)) {
        if (outError) *outError = validationError;
        qWarning() << "[VQL] Compile error:" << validationError.toString();
        return nullptr;
    }

    if (cache_.size() >= cacheCapacity_)
        evictLRU();

    lruOrder_.push_front(hash);
    cache_[hash] = { compiled, lruOrder_.begin() };

    return compiled;
}


VQLResultSet VQLEngine::execute(const VQLCompiledQuery& query, VQLError* outError)
{
    if (!query) {
        if (outError) outError->message = "Null compiled query";
        return {};
    }
    if (!project_) {
        if (outError) outError->message = "No project set on VQLEngine";
        return {};
    }

    VQLEvaluator evaluator;
    evaluator.setDataSource(&memoryDataSource_);
    return evaluator.evaluate(*query, project_, outError);
}


VQLResultSet VQLEngine::query(const QString& vql, VQLError* outError)
{
    auto compiled = compile(vql, outError);
    if (!compiled) return {};
    return execute(compiled, outError);
}

QJsonArray VQLEngine::queryJson(const QString& vql,
                                const std::vector<QString>& returnFields,
                                VQLError* outError)
{
    for (const QString& field : returnFields) {
        if (!VQLFieldCatalog::isKnownField(field)) {
            if (outError)
                outError->message = QStringLiteral("Unknown VQL field '%1'").arg(field);
            return {};
        }
    }
    auto result = query(vql, outError);
    if (outError && outError->isError()) return {};
    return result.toJson(returnFields, outError);
}


void VQLEngine::clearCache()
{
    cache_.clear();
    lruOrder_.clear();
}

void VQLEngine::evictLRU()
{
    if (lruOrder_.empty()) return;
    uint32_t oldest = lruOrder_.back();
    lruOrder_.pop_back();
    cache_.erase(oldest);
}


ObserverId VQLEngine::observe(const QString& vql, ObserverCallback callback)
{
    if (!observer_) return 0;
    return observer_->observe(vql, std::move(callback));
}

void VQLEngine::unobserve(ObserverId id)
{
    if (observer_)
        observer_->unobserve(id);
}

void VQLEngine::notifyProjectChanged()
{
    clearCache();
    if (observer_)
        observer_->onProjectChanged();
}

void VQLEngine::notifyFolderChanged(const PhysicalFolder* folder)
{
#ifdef HARKEN_HAS_LMDB
    if (lmdbIndexer_ && project_)
        lmdbIndexer_->updateFolder(folder, project_);
#else
    Q_UNUSED(folder);
#endif
    notifyProjectChanged();
}


#ifdef HARKEN_HAS_LMDB
void VQLEngine::initLMDB()
{
    QString dbPath;
    if (project_ && !project_->projectPath.isEmpty()) {
        const QString projectRoot = QFileInfo(project_->projectPath).absolutePath();
        dbPath = PathUtils::getSearchIndexCacheDir(projectRoot);
    }

    if (dbPath == lmdbPath_)
        return;

    cancelSearch();
    lmdbIndexer_.reset();
    lmdbService_.reset();
    lmdbInitialized_ = false;
    lmdbPath_.clear();

    if (dbPath.isEmpty())
        return;

    lmdbService_ = LMDBService::Create();
    if (lmdbService_) {
        if (lmdbService_->open(dbPath, 1024)) {
            lmdbIndexer_ = std::make_unique<LMDBIndexer>(lmdbService_.get());
            lmdbInitialized_ = true;
            lmdbPath_ = dbPath;
            qDebug() << "[VQL] LMDB search index opened at" << dbPath;
        } else {
            qWarning() << "[VQL] Failed to open LMDB at" << dbPath;
            lmdbService_.reset();
        }
    }
}
#endif

void VQLEngine::searchAsync(const QString& query,
                            std::function<void(const std::vector<SearchResult>&)> callback,
                            int maxResults)
{
    cancelSearch();
#ifdef HARKEN_HAS_LMDB
    if (!lmdbService_ || !lmdbService_->isOpen()) {
        if (callback) callback({});
        return;
    }
    activeSearch_ = new LMDBQueryTask(lmdbService_.get(), query, maxResults);
    activeSearch_->setAutoDelete(false);
    activeSearch_->setResultCallback(callback);
    LMDBQueryTask* task = activeSearch_;
    QObject::connect(task, &LMDBQueryTask::searchCompleted, task, [this]() {
        activeSearch_ = nullptr;
    }, Qt::QueuedConnection);
    QThreadPool::globalInstance()->start(activeSearch_);
#else
    if (callback) callback({});
#endif
}

void VQLEngine::cancelSearch()
{
#ifdef HARKEN_HAS_LMDB
    if (activeSearch_) {
        activeSearch_->cancel();
        activeSearch_ = nullptr;
    }
#endif
}

bool VQLEngine::isSearching() const
{
#ifdef HARKEN_HAS_LMDB
    return activeSearch_ != nullptr && !activeSearch_->isCancelled();
#else
    return false;
#endif
}

void VQLEngine::rebuildSearchIndex()
{
#ifdef HARKEN_HAS_LMDB
    try {
        if (lmdbIndexer_ && project_)
            lmdbIndexer_->rebuild(project_);
    } catch (...) {
        qWarning() << "[VQL] LMDB rebuildSearchIndex failed (non-fatal)";
    }
#endif
}

void VQLEngine::indexObjectChanged(const ProjectItem* item)
{
#ifdef HARKEN_HAS_LMDB
    if (lmdbIndexer_ && item)
        lmdbIndexer_->updateObject(item, project_);
#endif
}

void VQLEngine::indexObjectRemoved(uint32_t objectID)
{
#ifdef HARKEN_HAS_LMDB
    if (lmdbIndexer_)
        lmdbIndexer_->removeObject(objectID);
#endif
}

LMDBService* VQLEngine::lmdbService() const
{
#ifdef HARKEN_HAS_LMDB
    return lmdbService_.get();
#else
    return nullptr;
#endif
}

LMDBService* VQLEngine::ensureLmdbOpen()
{
#ifdef HARKEN_HAS_LMDB
    if (!lmdbInitialized_) {
        try {
            initLMDB();
        } catch (const std::exception& e) {
            qWarning() << "[VQL] ensureLmdbOpen failed (non-fatal):" << e.what();
            lmdbInitialized_ = false;
            lmdbService_.reset();
            lmdbIndexer_.reset();
        } catch (...) {
            qWarning() << "[VQL] ensureLmdbOpen crashed (non-fatal), disabling LMDB";
            lmdbInitialized_ = false;
            lmdbService_.reset();
            lmdbIndexer_.reset();
        }
    }
    return lmdbService_.get();
#else
    return nullptr;
#endif
}

LMDBIndexer* VQLEngine::lmdbIndexer() const
{
#ifdef HARKEN_HAS_LMDB
    return lmdbIndexer_.get();
#else
    return nullptr;
#endif
}

}
