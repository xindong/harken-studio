#ifndef HARKEN_CORE_CURRENT_SLICE_BACKING_H
#define HARKEN_CORE_CURRENT_SLICE_BACKING_H

namespace harken {

class ScheduledTransactionPool;

class CurrentSliceBacking {
public:
    CurrentSliceBacking() = default;
    ~CurrentSliceBacking() { Release(); }
    CurrentSliceBacking(const CurrentSliceBacking&) = delete;
    CurrentSliceBacking& operator=(const CurrentSliceBacking&) = delete;
    CurrentSliceBacking(CurrentSliceBacking&& other) noexcept;
    CurrentSliceBacking& operator=(CurrentSliceBacking&& other) noexcept;
    bool Valid() const noexcept { return owner_ != nullptr; }
    void Release() noexcept;

private:
    friend class ScheduledTransactionPool;
    ScheduledTransactionPool* owner_ = nullptr;
};

} // namespace harken

#endif
