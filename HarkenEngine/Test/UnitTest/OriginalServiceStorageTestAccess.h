#pragma once

#include "Engine.h"

namespace harken::detail {
struct EngineOriginalServiceStorageTestAccess {
    static EnginePlayableServiceRow* ActualService(const Engine::OriginalPlayableServiceLifetimeHolder& original) {
        return original.service_;
    }
    static bool BorrowAfterPost(Engine& engine,
        const Engine::OriginalPlayableServiceLifetimeHolder& original,
        EnginePlayablePartitionPool::OriginalStoragePin* output) {
        const auto* row = original.service_;
        if (original.engine_ != &engine || !original.Valid() || row == nullptr ||
            row->actionPrepared || !row->InvocationValid() ||
            !engine.playablePartitions.Published(row->partition, row->identity)) return false;
        return engine.playablePartitions.BorrowOriginalStorage(original.baseline_, output);
    }
    static bool BorrowAtReserve(Engine& engine,
        const Engine::OriginalPlayableServiceLifetimeHolder& original,
        EnginePlayablePartitionPool::OriginalStoragePin* output) {
        const auto* row = original.service_;
        if (original.engine_ != &engine || !original.Valid() || row == nullptr ||
            !row->actionPrepared || !row->InvocationValid() ||
            engine.playablePartitions.Published(row->partition, row->identity)) return false;
        return engine.playablePartitions.BorrowOriginalStorage(original.baseline_, output);
    }
};
}
