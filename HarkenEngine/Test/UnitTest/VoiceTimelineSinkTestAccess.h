#pragma once

#include "harken/render/VoiceTimelineSink.h"

#include <cstddef>

namespace harken::detail {

struct VoiceTimelineSinkTestAccess {
    static void SetVariableControlSliceClaimHook(
        VoiceTimelineSink& sink, void (*hook)(void*), void* context) {
        sink.variableControlSliceClaimHook_ = hook;
        sink.variableControlSliceClaimHookContext_ = context;
        sink.frontier_.SetVariableControlSliceClaimHookForTest(hook, context);
    }

    static void SetQueueSliceClaimHook(
        VoiceTimelineSink& sink, void (*hook)(void*), void* context) {
        sink.queueSliceClaimHook_ = hook;
        sink.queueSliceClaimHookContext_ = context;
    }

    static std::size_t SetPageGroupScratchCount(
        VoiceTimelineSink& sink, std::size_t count) {
        const auto previous = sink.pageGroups_.size();
        sink.pageGroups_.resize(count);
        return previous;
    }
};

} // namespace harken::detail
