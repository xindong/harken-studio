#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorSessionCompareUi.h"
#include "Widgets/Monitor/MonitorSessionComparePanel.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <fstream>
#include <iterator>
#include <string>

using VNX::Monitor::MonitorComparePicker;
using VNX::Monitor::MonitorCompareUiInput;
using VNX::Monitor::compareExportText;
using VNX::Monitor::compareResultText;
using VNX::Monitor::projectCompareUi;

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}
VNX::Monitor::MonitorSessionCompareView readyView()
{
    VNX::Monitor::MonitorSessionCompareView view {};
    view.status = static_cast<uint32_t>(VN_OK);
    view.hasCompleteFooter = 1;
    view.hasTraceCorrelation = 1;
    view.hasOptionalSymbols = 1;
    view.programSemanticFingerprint = 22;
    view.tapDescriptorId = 44;
    view.sampleRate = 48000;
    view.algorithmId = 7;
    view.algorithmVersion = 1;
    view.unitId = 2;
    view.targetStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    view.presentStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    view.rangeBegin = 0;
    view.rangeEnd = 100;
    return view;
}

} // namespace

TEST(MonitorSessionCompare, PickerNotWiredIsMissingData)
{
    MonitorCompareUiInput input;
    input.pickerWired = false;
    const auto state = projectCompareUi(input);
    EXPECT_EQ(state.picker, MonitorComparePicker::NotWired);
    EXPECT_EQ(state.compareResult, VN_MONITOR_COMPARE_MISSING_DATA);
    EXPECT_STREQ(state.summary.resultText, "MissingData");
}

TEST(MonitorSessionCompare, MissingSecondFileIsMissingData)
{
    MonitorCompareUiInput input;
    input.pickerWired = true;
    input.leftPresent = true;
    input.rightPresent = false;
    input.left = readyView();
    const auto state = projectCompareUi(input);
    EXPECT_EQ(state.picker, MonitorComparePicker::MissingSecondFile);
    EXPECT_EQ(state.compareResult, VN_MONITOR_COMPARE_MISSING_DATA);
}

TEST(MonitorSessionCompare, ReadInterruptIsMissingData)
{
    MonitorCompareUiInput input;
    input.pickerWired = true;
    input.leftPresent = true;
    input.rightPresent = true;
    input.readInterrupted = true;
    input.left = readyView();
    input.right = readyView();
    const auto state = projectCompareUi(input);
    EXPECT_EQ(state.compareResult, VN_MONITOR_COMPARE_MISSING_DATA);
}

TEST(MonitorSessionCompare, MatchingViewsAreComparable)
{
    MonitorCompareUiInput input;
    input.pickerWired = true;
    input.leftPresent = true;
    input.rightPresent = true;
    input.left = readyView();
    input.right = readyView();
    input.identity = "session-a";
    input.timestamp = "1";
    const auto state = projectCompareUi(input);
    EXPECT_EQ(state.callResult, VN_OK);
    EXPECT_EQ(state.compareResult, VN_MONITOR_COMPARE_COMPARABLE);
    EXPECT_STREQ(state.summary.resultText, "Comparable");
    EXPECT_NE(compareExportText(state.summary).find("identity=session-a"),
              std::string::npos);
    EXPECT_EQ(compareExportText(state.summary).find("result="), std::string::npos);
}

TEST(MonitorSessionCompare, FingerprintMismatchIsIncomparable)
{
    MonitorCompareUiInput input;
    input.pickerWired = true;
    input.leftPresent = true;
    input.rightPresent = true;
    input.left = readyView();
    input.right = readyView();
    input.right.programSemanticFingerprint = 99;
    const auto state = projectCompareUi(input);
    EXPECT_EQ(state.compareResult, VN_MONITOR_COMPARE_INCOMPARABLE);
}

TEST(MonitorSessionCompare, TraceOnlyGapIsPartiallyComparable)
{
    MonitorCompareUiInput input;
    input.pickerWired = true;
    input.leftPresent = true;
    input.rightPresent = true;
    input.left = readyView();
    input.right = readyView();
    input.right.hasTraceCorrelation = 0;
    const auto state = projectCompareUi(input);
    EXPECT_EQ(state.compareResult, VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE);
    EXPECT_STREQ(compareResultText(state.compareResult), "PartiallyComparable");
}

TEST(MonitorSessionCompare, ExportOmitsUnknownFields)
{
    MonitorCompareUiInput input;
    const auto state = projectCompareUi(input);
    const auto text = compareExportText(state.summary);
    EXPECT_NE(text.find("identity=unknown"), std::string::npos);
    EXPECT_EQ(text.find("result="), std::string::npos);
    EXPECT_EQ(text.find("C:\\"), std::string::npos);
    EXPECT_EQ(text.find("token"), std::string::npos);
}

TEST(MonitorSessionCompare, HostWiresComparePanelType)
{
    const char* files[] = {"Source/MainWindow.cpp", "Source/Widgets/Monitor/MonitorPanelKeys.cpp"};
    for (const char* file : files)
    {
        const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/" + file;
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;
        const std::string source((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        EXPECT_NE(source.find("MonitorSessionCompare"), std::string::npos) << path;
    }
}
