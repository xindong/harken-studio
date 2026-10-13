#include "MonitorSessionCompareUi.h"
#include "Monitor/MonitorTypes.h"
#include "RuntimeBridge/MonitorBridge.h"

namespace VNX {
namespace Monitor {

const char* compareResultText(uint32_t result)
{
    switch (result)
    {
    case kMonitorCompareComparable:
        return "Comparable";
    case kMonitorComparePartiallyComparable:
        return "PartiallyComparable";
    case kMonitorCompareIncomparable:
        return "Incomparable";
    case kMonitorCompareMissingData:
        return "MissingData";
    default:
        return "MissingData";
    }
}

MonitorSessionCompareView projectCompareView(const MonitorSessionFileResult& file)
{
    MonitorSessionCompareView view {};

    view.status = static_cast<uint32_t>(file.status);
    view.droppedRequired = file.droppedRequired;
    view.requiredUnknown = file.status == kMonitorIncompatible ? 1u : 0u;
    view.hasCompleteFooter =
        file.status == kOk && file.commitsWritten != 0 && file.droppedRequired == 0
            ? 1u
            : 0u;
    view.capabilityFingerprint = file.capabilityFingerprint;
    view.lastCompleteChunkSequence = 0;
    for (const auto& commit : file.commits)
    {
        if (commit.kind == kMonitorChunkTraceCorrelation)
            view.hasTraceCorrelation = 1;
        if (commit.kind == kMonitorChunkOptionalSymbols)
            view.hasOptionalSymbols = 1;
        if (commit.chunkSequence > view.lastCompleteChunkSequence)
            view.lastCompleteChunkSequence = commit.chunkSequence;
    }
    view.bytesConsumed = file.bytesConsumed;
    return view;
}

MonitorCompareUiState projectCompareUi(const MonitorCompareUiInput& input)
{
    MonitorCompareUiState state;
    state.pickerWired = input.pickerWired;
    state.summary.identity = input.identity;
    state.summary.timestamp = input.timestamp;
    if (!input.pickerWired)
    {
        state.picker = MonitorComparePicker::NotWired;
        state.compareResult = kMonitorCompareMissingData;
        state.summary.result = state.compareResult;
        state.summary.resultText = compareResultText(state.compareResult);
        state.summary.reason = "File picker not wired";
        return state;
    }
    if (!input.leftPresent || !input.rightPresent || input.readInterrupted)
    {
        state.picker = MonitorComparePicker::MissingSecondFile;
        state.compareResult = kMonitorCompareMissingData;
        state.summary.result = state.compareResult;
        state.summary.resultText = compareResultText(state.compareResult);
        state.summary.reason = input.readInterrupted ? "Read interrupted"
                                                     : "Second file missing";
        return state;
    }

    state.picker = MonitorComparePicker::Loaded;
    MonitorSessionCompareDesc desc {};

    desc.left = &input.left;
    desc.right = &input.right;
    MonitorCompareResult out {};

    state.callResult = HarkenEditor::Bridge::Monitoring::compareSessions(&desc, &out);
    if (state.callResult != kOk)
    {
        state.compareResult = kMonitorCompareMissingData;
        state.summary.reason = "Compare call failed";
    }
    else
    {
        state.compareResult = out.result;
        state.summary.capability = "Vn_MonitorCompareSessions";
    }
    state.summary.result = state.compareResult;
    state.summary.resultText = compareResultText(state.compareResult);
    if (state.summary.reason.empty())
        state.summary.reason = state.summary.resultText;
    return state;
}

std::string compareExportText(const MonitorCompareSummary& summary)
{
    std::string text;
    text += "identity=";
    text += summary.identity.empty() ? "unknown" : summary.identity;
    text += "\ntimestamp=";
    text += summary.timestamp.empty() ? "unknown" : summary.timestamp;
    text += "\nreason=";
    text += summary.reason.empty() ? "unknown" : summary.reason;
    text += "\ncapability=";
    text += summary.capability.empty() ? "unknown" : summary.capability;
    text += "\n";
    return text;
}

} // namespace Monitor
} // namespace VNX
