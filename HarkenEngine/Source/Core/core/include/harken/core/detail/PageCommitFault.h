#pragma once

namespace harken::detail {

// A test page limit normally models a fixed capacity. Scope this around a
// capped allocation to model an OS commitment failure at that specific page.
class ScopedPageCommitFaultForTests {
public:
    ScopedPageCommitFaultForTests() noexcept : previous_(active_) { active_ = true; }
    ~ScopedPageCommitFaultForTests() { active_ = previous_; }
    ScopedPageCommitFaultForTests(const ScopedPageCommitFaultForTests&) = delete;
    ScopedPageCommitFaultForTests& operator=(const ScopedPageCommitFaultForTests&) = delete;
    static bool Active() noexcept { return active_; }
private:
    static inline thread_local bool active_ = false;
    bool previous_;
};

} // namespace harken::detail
