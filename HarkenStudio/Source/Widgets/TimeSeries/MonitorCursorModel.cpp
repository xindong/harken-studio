#include "MonitorCursorModel.h"

#include <algorithm>
#include <utility>

namespace VNX {

void MonitorCursorModel::setSessionIdentity(
    std::uint64_t sessionEpoch,
    std::uint64_t sessionAnchorGeneration)
{
    auto next = snapshot_;
    const bool epochChanged = next.sessionEpoch != sessionEpoch;
    next.sessionEpoch = sessionEpoch;
    next.sessionAnchorGeneration = sessionAnchorGeneration;
    if (epochChanged)
    {
        next.executeFrame.reset();
        next.mode = MonitorCursorMode::FollowLive;
        next.historyUnavailable = false;
        next.clock = {};
        liveExecuteFrame_.reset();
        liveClock_ = {};
    }
    refreshDerived(next);
    publishIfChanged(next);
}

void MonitorCursorModel::applyRetainedWindow(
    std::optional<std::int64_t> retainedMin,
    std::optional<std::int64_t> retainedMax)
{
    retainedMin_ = retainedMin;
    retainedMax_ = retainedMax;
    auto next = snapshot_;
    if (next.mode == MonitorCursorMode::LockHistory && next.executeFrame
        && !insideRetained(*next.executeFrame))
    {
        next.historyUnavailable = true;
    }
    else if (next.mode == MonitorCursorMode::LockHistory && next.executeFrame
             && insideRetained(*next.executeFrame))
    {
        next.historyUnavailable = false;
    }
    refreshDerived(next);
    publishIfChanged(next);
}

void MonitorCursorModel::setRetainedWindow(
    std::optional<std::int64_t> retainedMin,
    std::optional<std::int64_t> retainedMax)
{
    contributorWindows_.clear();
    applyRetainedWindow(retainedMin, retainedMax);
}

void MonitorCursorModel::setContributorWindow(
    std::uint64_t contributor,
    std::optional<std::int64_t> retainedMin,
    std::optional<std::int64_t> retainedMax)
{
    auto existing = std::find_if(contributorWindows_.begin(), contributorWindows_.end(),
                                 [contributor](const ContributorWindow& entry) {
                                     return entry.id == contributor;
                                 });
    if (existing == contributorWindows_.end())
        contributorWindows_.push_back({contributor, retainedMin, retainedMax});
    else
    {
        existing->minFrame = retainedMin;
        existing->maxFrame = retainedMax;
    }

    std::optional<std::int64_t> mergedMin;
    std::optional<std::int64_t> mergedMax;
    for (const auto& entry : contributorWindows_)
    {
        if (!entry.minFrame || !entry.maxFrame)
            continue;
        mergedMin = mergedMin ? std::min(*mergedMin, *entry.minFrame) : entry.minFrame;
        mergedMax = mergedMax ? std::max(*mergedMax, *entry.maxFrame) : entry.maxFrame;
    }
    applyRetainedWindow(mergedMin, mergedMax);
}

bool MonitorCursorModel::advanceLive(std::int64_t executeFrame, const MonitorCursorClock& clock)
{
    liveExecuteFrame_ = executeFrame;
    liveClock_ = clock;
    if (snapshot_.mode != MonitorCursorMode::FollowLive)
        return true;

    auto next = snapshot_;
    next.executeFrame = executeFrame;
    next.clock = clock;
    next.historyUnavailable = false;
    refreshDerived(next);
    publishIfChanged(next);
    return true;
}

MonitorCursorReject MonitorCursorModel::lockTo(
    std::int64_t executeFrame,
    const MonitorCursorClock& clock)
{
    if (!insideRetained(executeFrame))
        return MonitorCursorReject::HistoryUnavailable;

    auto next = snapshot_;
    next.mode = MonitorCursorMode::LockHistory;
    next.executeFrame = executeFrame;
    next.clock = clock;
    next.historyUnavailable = false;
    refreshDerived(next);
    publishIfChanged(next);
    return MonitorCursorReject::None;
}

bool MonitorCursorModel::followLive()
{
    auto next = snapshot_;
    next.mode = MonitorCursorMode::FollowLive;
    next.historyUnavailable = false;
    if (liveExecuteFrame_)
    {
        next.executeFrame = liveExecuteFrame_;
        next.clock = liveClock_;
    }
    refreshDerived(next);
    publishIfChanged(next);
    return true;
}

std::uint64_t MonitorCursorModel::subscribe(Listener listener)
{
    const auto token = nextToken_++;
    listeners_.push_back({token, std::move(listener), snapshot_.generation, true});
    const auto initialListener = listeners_.back().listener;
    initialListener(snapshot_);
    return token;
}

void MonitorCursorModel::unsubscribe(std::uint64_t token)
{
    listeners_.erase(
        std::remove_if(
            listeners_.begin(),
            listeners_.end(),
            [token](const auto& entry) { return entry.token == token; }),
        listeners_.end());
}

const char* MonitorCursorModel::uncertaintyText(MonitorCursorUncertaintyKind kind)
{
    switch (kind)
    {
    case MonitorCursorUncertaintyKind::Exact:
        return "精确";
    case MonitorCursorUncertaintyKind::Estimate:
        return "估算";
    case MonitorCursorUncertaintyKind::Unknown:
        return "未知";
    }
    return "未知";
}

const char* MonitorCursorModel::modeText(MonitorCursorMode mode)
{
    return mode == MonitorCursorMode::LockHistory ? "锁定历史帧" : "跟随实时";
}

const char* MonitorCursorModel::unreadinessText()
{
    return "未就绪";
}

const char* MonitorCursorModel::historyUnavailableText()
{
    return "历史不可用";
}

void MonitorCursorModel::publishIfChanged(const MonitorCursorSnapshot& next)
{
    const auto same =
        next.mode == snapshot_.mode
        && next.executeFrame == snapshot_.executeFrame
        && next.clock.graphFrame == snapshot_.clock.graphFrame
        && next.clock.deviceEpoch == snapshot_.clock.deviceEpoch
        && next.clock.clockAnchorGeneration == snapshot_.clock.clockAnchorGeneration
        && next.clock.deviceUncertaintyFrames == snapshot_.clock.deviceUncertaintyFrames
        && next.uncertaintyKind == snapshot_.uncertaintyKind
        && next.historyUnavailable == snapshot_.historyUnavailable
        && next.sessionEpoch == snapshot_.sessionEpoch
        && next.sessionAnchorGeneration == snapshot_.sessionAnchorGeneration;
    if (same)
        return;

    auto published = next;
    published.generation = snapshot_.generation + 1;
    snapshot_ = published;
    pendingPublishes_.push_back(published);
    flushPublishes();
}

void MonitorCursorModel::flushPublishes()
{
    if (publishing_)
        return;

    publishing_ = true;
    while (!pendingPublishes_.empty())
    {
        const auto published = pendingPublishes_.front();
        pendingPublishes_.erase(pendingPublishes_.begin());

        std::vector<std::uint64_t> tokens;
        tokens.reserve(listeners_.size());
        for (const auto& entry : listeners_)
            tokens.push_back(entry.token);

        for (const auto token : tokens)
        {
            auto* slot = findSlot(token);
            if (!slot)
                continue;
            if (slot->receivedAny && slot->lastDeliveredGeneration >= published.generation)
                continue;

            slot->receivedAny = true;
            slot->lastDeliveredGeneration = published.generation;
            const auto listener = slot->listener;
            listener(published);
        }
    }
    publishing_ = false;
}

MonitorCursorModel::ListenerSlot* MonitorCursorModel::findSlot(std::uint64_t token)
{
    for (auto& entry : listeners_)
    {
        if (entry.token == token)
            return &entry;
    }
    return nullptr;
}

void MonitorCursorModel::refreshDerived(MonitorCursorSnapshot& state) const
{
    state.uncertaintyKind = classify(state);
}

bool MonitorCursorModel::insideRetained(std::int64_t executeFrame) const
{
    if (!retainedMin_ || !retainedMax_)
        return false;
    if (*retainedMax_ <= *retainedMin_)
        return false;
    return executeFrame >= *retainedMin_ && executeFrame < *retainedMax_;
}

MonitorCursorUncertaintyKind MonitorCursorModel::classify(
    const MonitorCursorSnapshot& state) const
{
    if (!state.executeFrame)
        return MonitorCursorUncertaintyKind::Unknown;
    if (!state.clock.deviceEpoch || !state.clock.clockAnchorGeneration)
        return MonitorCursorUncertaintyKind::Unknown;
    if (*state.clock.deviceEpoch != state.sessionEpoch
        || *state.clock.clockAnchorGeneration != state.sessionAnchorGeneration)
        return MonitorCursorUncertaintyKind::Unknown;
    if (!state.clock.deviceUncertaintyFrames)
        return MonitorCursorUncertaintyKind::Unknown;
    if (*state.clock.deviceUncertaintyFrames > 0)
        return MonitorCursorUncertaintyKind::Estimate;
    return MonitorCursorUncertaintyKind::Exact;
}

} // namespace VNX
