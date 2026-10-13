#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace VNX {

enum class MonitorCursorMode
{
    FollowLive,
    LockHistory,
};

enum class MonitorCursorUncertaintyKind
{
    Unknown,
    Estimate,
    Exact,
};

enum class MonitorCursorReject
{
    None,
    HistoryUnavailable,
};

struct MonitorCursorClock
{
    std::optional<std::int64_t> graphFrame;
    std::optional<std::uint64_t> deviceEpoch;
    std::optional<std::uint64_t> clockAnchorGeneration;
    std::optional<std::int64_t> deviceUncertaintyFrames;
};

struct MonitorCursorSnapshot
{
    MonitorCursorMode mode = MonitorCursorMode::FollowLive;
    std::optional<std::int64_t> executeFrame;
    std::uint64_t generation = 0;
    MonitorCursorClock clock;
    MonitorCursorUncertaintyKind uncertaintyKind = MonitorCursorUncertaintyKind::Unknown;
    bool historyUnavailable = false;
    std::uint64_t sessionEpoch = 0;
    std::uint64_t sessionAnchorGeneration = 0;
};

class MonitorCursorModel
{
public:
    using Listener = std::function<void(const MonitorCursorSnapshot&)>;

    void setSessionIdentity(std::uint64_t sessionEpoch, std::uint64_t sessionAnchorGeneration);
    void setRetainedWindow(std::optional<std::int64_t> retainedMin,
                           std::optional<std::int64_t> retainedMax);
    void setContributorWindow(std::uint64_t contributor,
                              std::optional<std::int64_t> retainedMin,
                              std::optional<std::int64_t> retainedMax);

    // generation 只保证本模型同步发布单调递增，没有外部事件序号可丢。
    bool advanceLive(std::int64_t executeFrame, const MonitorCursorClock& clock = {});
    MonitorCursorReject lockTo(std::int64_t executeFrame, const MonitorCursorClock& clock = {});
    bool followLive();

    std::uint64_t subscribe(Listener listener);
    void unsubscribe(std::uint64_t token);

    const MonitorCursorSnapshot& snapshot() const { return snapshot_; }

    static const char* uncertaintyText(MonitorCursorUncertaintyKind kind);
    static const char* modeText(MonitorCursorMode mode);
    static const char* unreadinessText();
    static const char* historyUnavailableText();

private:
    struct ListenerSlot
    {
        std::uint64_t token = 0;
        Listener listener;
        std::uint64_t lastDeliveredGeneration = 0;
        bool receivedAny = false;
    };

    void publishIfChanged(const MonitorCursorSnapshot& next);
    void flushPublishes();
    void refreshDerived(MonitorCursorSnapshot& state) const;
    bool insideRetained(std::int64_t executeFrame) const;
    MonitorCursorUncertaintyKind classify(const MonitorCursorSnapshot& state) const;
    ListenerSlot* findSlot(std::uint64_t token);

    MonitorCursorSnapshot snapshot_{};
    void applyRetainedWindow(std::optional<std::int64_t> retainedMin,
                             std::optional<std::int64_t> retainedMax);

    std::optional<std::int64_t> liveExecuteFrame_;
    MonitorCursorClock liveClock_{};
    std::optional<std::int64_t> retainedMin_;
    std::optional<std::int64_t> retainedMax_;
    struct ContributorWindow
    {
        std::uint64_t id = 0;
        std::optional<std::int64_t> minFrame;
        std::optional<std::int64_t> maxFrame;
    };
    std::vector<ContributorWindow> contributorWindows_;
    std::uint64_t nextToken_ = 1;
    std::vector<ListenerSlot> listeners_;
    std::vector<MonitorCursorSnapshot> pendingPublishes_;
    bool publishing_ = false;
};

} // namespace VNX
