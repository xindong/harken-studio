
#pragma once

namespace VNX {

class ReentrancyGuard
{
public:
    explicit ReentrancyGuard(bool& flag) noexcept
        : flag_(flag), wasActive_(flag)
    {
        flag_ = true;
    }

    ~ReentrancyGuard() noexcept
    {
        if (!wasActive_)
            flag_ = false;
    }

    bool wasAlreadyActive() const noexcept { return wasActive_; }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;

private:
    bool& flag_;
    bool  wasActive_;
};

}
