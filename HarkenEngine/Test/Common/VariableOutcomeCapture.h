#ifndef HARKEN_TEST_VARIABLE_OUTCOME_CAPTURE_H
#define HARKEN_TEST_VARIABLE_OUTCOME_CAPTURE_H

#include "Engine.h"

#include <cassert>
#include <cstddef>
#include <vector>

namespace harkentest {

class VariableOutcomeCapture {
public:
    explicit VariableOutcomeCapture(harken::Engine& engine, std::size_t capacity = 0)
        : engine_(&engine), active_(harken::detail::ActiveEngine() == &engine),
          records_(capacity == 0 ? engine.variables.RevisionStorageCapacity() : capacity) {
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
        if (out == nullptr || count_ == 0) return false;
        *out = records_[begin_];
        begin_ = (begin_ + 1) % records_.size();
        --count_;
        return true;
    }

    bool Overflowed() const noexcept { return overflowed_; }
    std::size_t Count() const noexcept { return count_; }

private:
    static void Capture(void* context, const harken::VariableOutcomeRecord& record) noexcept {
        auto& capture = *static_cast<VariableOutcomeCapture*>(context);
        if (capture.count_ == capture.records_.size()) {
            capture.overflowed_ = true;
            return;
        }
        capture.records_[(capture.begin_ + capture.count_) % capture.records_.size()] = record;
        ++capture.count_;
    }

    harken::Engine* engine_;
    bool active_;
    std::vector<harken::VariableOutcomeRecord> records_;
    std::size_t begin_ = 0;
    std::size_t count_ = 0;
    bool overflowed_ = false;
};

} // namespace harkentest

#endif
