
#include "VQLObserver.h"
#include "VQLEngine.h"

namespace VNX {

VQLObserver::VQLObserver(VQLEngine* engine, QObject* parent)
    : QObject(parent)
    , engine_(engine)
{
    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(DEBOUNCE_MS);
    connect(&debounceTimer_, &QTimer::timeout, this, &VQLObserver::evaluatePending);
}

VQLObserver::~VQLObserver()
{
    debounceTimer_.stop();
}

ObserverId VQLObserver::observe(const QString& vql, ObserverCallback callback)
{
    if (!engine_ || !callback) return 0;

    VQLError err;
    auto compiled = engine_->compile(vql, &err);
    if (!compiled) return 0;

    ObserverId id = nextId_++;

    ObserverEntry entry;
    entry.vql      = vql;
    entry.compiled = std::move(compiled);
    entry.callback = std::move(callback);

    auto result = engine_->execute(entry.compiled, nullptr);
    entry.lastHash = computeResultHash(result);
    entry.callback(result);

    entries_[id] = std::move(entry);
    return id;
}

void VQLObserver::unobserve(ObserverId id)
{
    entries_.erase(id);
}

void VQLObserver::unobserveAll()
{
    entries_.clear();
}

void VQLObserver::onProjectChanged()
{
    if (entries_.empty()) return;

    dirty_ = true;
    if (!debounceTimer_.isActive())
        debounceTimer_.start();
}

void VQLObserver::evaluatePending()
{
    if (!dirty_ || !engine_) return;
    dirty_ = false;

    for (auto& [id, entry] : entries_) {
        VQLError err;
        auto result = engine_->execute(entry.compiled, &err);
        uint32_t newHash = computeResultHash(result);

        if (newHash != entry.lastHash) {
            entry.lastHash = newHash;
            if (entry.callback)
                entry.callback(result);
        }
    }
}

uint32_t VQLObserver::computeResultHash(const VQLResultSet& result) const
{
    uint32_t hash = 0x811c9dc5u;
    for (const auto* item : result.items()) {
        auto ptr = reinterpret_cast<uintptr_t>(item);
        for (size_t i = 0; i < sizeof(ptr); ++i) {
            hash ^= static_cast<uint8_t>(ptr >> (i * 8));
            hash *= 0x01000193u;
        }
    }
    uint32_t count = static_cast<uint32_t>(result.count());
    for (size_t i = 0; i < sizeof(count); ++i) {
        hash ^= static_cast<uint8_t>(count >> (i * 8));
        hash *= 0x01000193u;
    }
    return hash;
}

}
