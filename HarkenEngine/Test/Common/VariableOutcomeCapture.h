#ifndef HARKEN_TEST_VARIABLE_OUTCOME_CAPTURE_H
#define HARKEN_TEST_VARIABLE_OUTCOME_CAPTURE_H

#include "Engine.h"

#include <cassert>
#include <cstddef>
#include <deque>

namespace harkentest {

class VariableOutcomeCapture {
public:
    explicit VariableOutcomeCapture(harken::Engine& engine, std::size_t capacity = 0)
        : engine_(&engine), active_(harken::detail::ActiveEngine() == &engine),
          limit_(capacity) {
        assert(engine.variableOutcomeSink == nullptr);
        engine.variableOutcomeSinkContext = this;
        engine.variableOutcomeSink = &Capture;
    }

    ~VariableOutcomeCapture() {
        if (!active_ || harken::detail::ActiveEngine() == engine_) {
            engine_->variableOutcomeSink = nullptr;
            engine_->variableOutcomeSinkContext = nullptr;
        }
    }

    VariableOutcomeCapture(const VariableOutcomeCapture&) = delete;
    VariableOutcomeCapture& operator=(const VariableOutcomeCapture&) = delete;

    bool Pop(harken::VariableOutcomeRecord* out) {
        engine_->CompleteVariableOperation();
        if (out == nullptr || records_.empty()) return false;
        *out = records_.front();
        records_.pop_front();
        return true;
    }

    bool Overflowed() const noexcept { return overflowed_; }
    std::size_t Count() const noexcept { return records_.size(); }

private:
    static void Capture(void* context, const harken::VariableOutcomeRecord& record) noexcept {
        auto& capture = *static_cast<VariableOutcomeCapture*>(context);
        if (capture.limit_ != 0 && capture.limit_ == capture.records_.size()) {
            capture.overflowed_ = true;
            return;
        }
        try { capture.records_.push_back(record); }
        catch (const std::bad_alloc&) { capture.overflowed_ = true; }
    }

    harken::Engine* engine_;
    bool active_;
    std::size_t limit_;
    std::deque<harken::VariableOutcomeRecord> records_;
    bool overflowed_ = false;
};

} // namespace harkentest

#endif
