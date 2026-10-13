#pragma once

#include <utility>

namespace VNX {

class PollReentryGuard
{
public:
    bool tryEnter()
    {
        if (busy_)
        {
            ++blocked_;
            return false;
        }
        busy_ = true;
        return true;
    }

    void leave()
    {
        busy_ = false;
    }

    int executedCount() const { return executed_; }
    int blockedCount() const { return blocked_; }
    bool busy() const { return busy_; }

    void markExecuted() { ++executed_; }

    template <typename Fn>
    bool run(Fn&& body)
    {
        if (!tryEnter())
            return false;
        struct Scope
        {
            PollReentryGuard* guard = nullptr;
            ~Scope()
            {
                if (guard)
                    guard->leave();
            }
        } scope{this};
        markExecuted();
        std::forward<Fn>(body)();
        return true;
    }

private:
    bool busy_ = false;
    int executed_ = 0;
    int blocked_ = 0;
};

} // namespace VNX
