#ifndef HARKEN_HOST_PACK_PLAY_INIT_FAILURE_H
#define HARKEN_HOST_PACK_PLAY_INIT_FAILURE_H

#include <cstdio>
#include <cstring>

#include "DevicePlaybackOpenPlan.h"
#include "RealDeviceInitStage.h"

namespace harkenhost {

struct PackPlayInitFailure {
    int exitCode = 3;
    bool skipped = false;
};

// Failed Vn_Init never skips. Null and real-device errors share this path.
inline PackPlayInitFailure ClassifyPackPlayInitFailure(bool nullMode, int initResult) {
    (void)nullMode;
    (void)initResult;
    return {};
}

inline bool FormatPackPlayInitFailure(
    int initResult, const char* stage, char* buffer, std::size_t size,
    bool hasDeviceInitCode = false, int deviceInitCode = 0,
    bool hasOpenPlan = false, bool enumerateFailed = false,
    int triedEnumeratedCount = 0, int openedEnumeratedIndex = -1) {
    if (buffer == nullptr || size == 0) {
        return false;
    }
    const bool hasStage = stage != nullptr && stage[0] != '\0';
    const bool isDeviceInit = stage != nullptr &&
                              std::strcmp(stage, "device-init") == 0;
    const bool appendMa = hasDeviceInitCode && isDeviceInit;
    const bool appendPlan = hasOpenPlan && isDeviceInit;
    int written = 0;
    if (appendMa && appendPlan) {
        written = std::snprintf(
            buffer, size,
            "error: Vn_Init failed (%d) %s ma=%d enumerateFailed=%d "
            "triedEnumeratedCount=%d openedEnumeratedIndex=%d\n",
            initResult, stage, deviceInitCode, enumerateFailed ? 1 : 0,
            triedEnumeratedCount, openedEnumeratedIndex);
    } else if (appendMa) {
        written = std::snprintf(
            buffer, size, "error: Vn_Init failed (%d) %s ma=%d\n",
            initResult, stage, deviceInitCode);
    } else if (hasStage) {
        written = std::snprintf(
            buffer, size, "error: Vn_Init failed (%d) %s\n", initResult, stage);
    } else {
        written = std::snprintf(
            buffer, size, "error: Vn_Init failed (%d)\n", initResult);
    }
    return written > 0 && static_cast<std::size_t>(written) < size;
}

inline int RunPackPlayInitFailureSelfTest() {
    const int kDeviceError = -400;
    const int kOther = -7;
    const bool modes[] = {false, true};
    const int results[] = {kDeviceError, kOther};
    for (bool nullMode : modes) {
        for (int result : results) {
            const PackPlayInitFailure failure =
                ClassifyPackPlayInitFailure(nullMode, result);
            if (failure.exitCode != 3 || failure.skipped || failure.exitCode == 0) {
                std::fprintf(stderr, "error: init failure exit was not 3\n");
                return 1;
            }
            char stderrText[80];
            char expected[80];
            if (!FormatPackPlayInitFailure(
                    result, nullptr, stderrText, sizeof(stderrText)) ||
                std::snprintf(
                    expected, sizeof(expected), "error: Vn_Init failed (%d)\n",
                    result) < 0 ||
                std::strcmp(stderrText, expected) != 0) {
                std::fprintf(stderr, "error: unstaged init failure text changed\n");
                return 2;
            }
            if (std::strstr(stderrText, "SKIPPED") != nullptr) {
                std::fprintf(stderr, "error: init failure text skipped\n");
                return 3;
            }
        }
    }
    char contextText[80];
    char deviceInitText[80];
    char deviceStartText[80];
    char emptyStage[80];
    if (!FormatPackPlayInitFailure(-400, "context", contextText, 80) ||
        !FormatPackPlayInitFailure(-400, "device-init", deviceInitText, 80) ||
        !FormatPackPlayInitFailure(-400, "device-start", deviceStartText, 80) ||
        !FormatPackPlayInitFailure(-400, "", emptyStage, 80)) {
        std::fprintf(stderr, "error: staged init failure text did not fit\n");
        return 4;
    }
    if (std::strcmp(contextText, deviceInitText) == 0 ||
        std::strcmp(deviceInitText, deviceStartText) == 0 ||
        std::strstr(contextText, "context") == nullptr ||
        std::strstr(deviceInitText, "device-init") == nullptr ||
        std::strstr(deviceStartText, "device-start") == nullptr ||
        std::strstr(contextText, "(-400)") == nullptr ||
        std::strstr(contextText, "SKIPPED") != nullptr ||
        std::strcmp(emptyStage, "error: Vn_Init failed (-400)\n") != 0) {
        std::fprintf(stderr, "error: staged init failure text mismatch\n");
        return 5;
    }
    char withCode[80];
    char contextWithCode[80];
    char startWithCode[80];
    char emptyWithCode[80];
    char deviceInitPlain[80];
    if (!FormatPackPlayInitFailure(
            -400, "device-init", withCode, 80, true, -17) ||
        !FormatPackPlayInitFailure(
            -400, "context", contextWithCode, 80, true, -17) ||
        !FormatPackPlayInitFailure(
            -400, "device-start", startWithCode, 80, true, -17) ||
        !FormatPackPlayInitFailure(
            -400, nullptr, emptyWithCode, 80, true, -17) ||
        !FormatPackPlayInitFailure(
            -400, "device-init", deviceInitPlain, 80, false, -17)) {
        std::fprintf(stderr, "error: ma code text did not fit\n");
        return 6;
    }
    if (std::strcmp(withCode, "error: Vn_Init failed (-400) device-init ma=-17\n") != 0 ||
        std::strstr(contextWithCode, "ma=") != nullptr ||
        std::strstr(startWithCode, "ma=") != nullptr ||
        std::strstr(emptyWithCode, "ma=") != nullptr ||
        std::strcmp(emptyWithCode, "error: Vn_Init failed (-400)\n") != 0 ||
        std::strstr(deviceInitPlain, "ma=") != nullptr ||
        std::strstr(withCode, "SKIPPED") != nullptr) {
        std::fprintf(stderr, "error: ma code leaked onto the wrong stage\n");
        return 7;
    }
    char enumerateFail[160];
    char allTried[160];
    char planOnContext[160];
    char planWithoutMa[160];
    if (!FormatPackPlayInitFailure(
            -400, "device-init", enumerateFail, sizeof(enumerateFail), true, -1,
            true, true, 0, -1) ||
        !FormatPackPlayInitFailure(
            -400, "device-init", allTried, sizeof(allTried), true, -1, true,
            false, 2, -1) ||
        !FormatPackPlayInitFailure(
            -400, "context", planOnContext, sizeof(planOnContext), true, -1,
            true, true, 0, -1) ||
        !FormatPackPlayInitFailure(
            -400, "device-init", planWithoutMa, sizeof(planWithoutMa), false, -1,
            true, true, 0, -1)) {
        std::fprintf(stderr, "error: open-plan text did not fit\n");
        return 8;
    }
    if (std::strcmp(
            enumerateFail,
            "error: Vn_Init failed (-400) device-init ma=-1 enumerateFailed=1 "
            "triedEnumeratedCount=0 openedEnumeratedIndex=-1\n") != 0 ||
        std::strcmp(
            allTried,
            "error: Vn_Init failed (-400) device-init ma=-1 enumerateFailed=0 "
            "triedEnumeratedCount=2 openedEnumeratedIndex=-1\n") != 0 ||
        std::strstr(planOnContext, "enumerateFailed=") != nullptr ||
        std::strstr(planWithoutMa, "enumerateFailed=") != nullptr ||
        std::strstr(planWithoutMa, "ma=") != nullptr) {
        std::fprintf(stderr, "error: open-plan leaked onto the wrong stage\n");
        return 9;
    }
    return 0;
}

inline int RunRealDeviceInitStageLifetimeSelfTest() {
    harken::ClearRealDeviceInitStage();
    harken::SetRealDeviceInitStage("device-start");
    struct EngineStandIn {
        int id;
    };
    EngineStandIn* engine = new EngineStandIn{7};
    delete engine;
    engine = nullptr;
    const char* stage = harken::RealDeviceInitStage();
    char text[80];
    if (stage == nullptr || std::strcmp(stage, "device-start") != 0) {
        std::fprintf(stderr, "error: stage did not survive engine reset\n");
        return 20;
    }
    if (!FormatPackPlayInitFailure(-400, stage, text, sizeof(text)) ||
        std::strcmp(text, "error: Vn_Init failed (-400) device-start\n") != 0) {
        std::fprintf(stderr, "error: host did not format the surviving stage\n");
        return 21;
    }
    harken::ClearRealDeviceInitStage();
    if (harken::RealDeviceInitStage() != nullptr ||
        harken::RealDeviceInitStageHasCode() ||
        !FormatPackPlayInitFailure(
            -400, harken::RealDeviceInitStage(), text, sizeof(text),
            harken::RealDeviceInitStageHasCode(), harken::RealDeviceInitStageCode()) ||
        std::strcmp(text, "error: Vn_Init failed (-400)\n") != 0) {
        std::fprintf(stderr, "error: cleared stage still changed the host text\n");
        return 22;
    }
    harken::SetRealDeviceInitStage("device-init");
    harken::SetRealDeviceInitMaCode(-17);
    harken::SetRealDeviceInitOpenPlan(true, 0, -1);
    EngineStandIn* again = new EngineStandIn{8};
    delete again;
    again = nullptr;
    if (!harken::RealDeviceInitStageHasCode() ||
        harken::RealDeviceInitStageCode() != -17 ||
        std::strcmp(harken::RealDeviceInitStage(), "device-init") != 0) {
        std::fprintf(stderr, "error: ma code did not survive engine reset\n");
        return 23;
    }
    char maOnly[80];
    if (!FormatPackPlayInitFailure(
            -400, harken::RealDeviceInitStage(), maOnly, sizeof(maOnly),
            harken::RealDeviceInitStageHasCode(), harken::RealDeviceInitStageCode()) ||
        std::strcmp(maOnly, "error: Vn_Init failed (-400) device-init ma=-17\n") != 0) {
        std::fprintf(stderr, "error: host missed the surviving ma code\n");
        return 24;
    }
    if (!harken::RealDeviceInitStageHasOpenPlan() ||
        !harken::RealDeviceInitEnumerateFailed() ||
        harken::RealDeviceInitTriedEnumeratedCount() != 0 ||
        harken::RealDeviceInitOpenedEnumeratedIndex() != -1) {
        std::fprintf(stderr, "error: open-plan did not survive engine reset\n");
        return 25;
    }
    char planText[160];
    if (!FormatPackPlayInitFailure(
            -400, harken::RealDeviceInitStage(), planText, sizeof(planText),
            harken::RealDeviceInitStageHasCode(), harken::RealDeviceInitStageCode(),
            harken::RealDeviceInitStageHasOpenPlan(),
            harken::RealDeviceInitEnumerateFailed(),
            harken::RealDeviceInitTriedEnumeratedCount(),
            harken::RealDeviceInitOpenedEnumeratedIndex()) ||
        std::strcmp(
            planText,
            "error: Vn_Init failed (-400) device-init ma=-17 enumerateFailed=1 "
            "triedEnumeratedCount=0 openedEnumeratedIndex=-1\n") != 0) {
        std::fprintf(stderr, "error: host missed the surviving open-plan\n");
        return 26;
    }
    harken::ClearRealDeviceInitStage();
    if (harken::RealDeviceInitStage() != nullptr ||
        harken::RealDeviceInitStageHasCode() ||
        harken::RealDeviceInitStageHasOpenPlan() ||
        harken::RealDeviceInitEnumerateFailed() ||
        harken::RealDeviceInitTriedEnumeratedCount() != 0 ||
        harken::RealDeviceInitOpenedEnumeratedIndex() != -1) {
        std::fprintf(stderr, "error: clear left open-plan fields set\n");
        return 27;
    }
    char afterClear[80];
    if (!FormatPackPlayInitFailure(
            -400, harken::RealDeviceInitStage(), afterClear, sizeof(afterClear),
            harken::RealDeviceInitStageHasCode(), harken::RealDeviceInitStageCode(),
            harken::RealDeviceInitStageHasOpenPlan(),
            harken::RealDeviceInitEnumerateFailed(),
            harken::RealDeviceInitTriedEnumeratedCount(),
            harken::RealDeviceInitOpenedEnumeratedIndex()) ||
        std::strcmp(afterClear, "error: Vn_Init failed (-400)\n") != 0 ||
        std::strstr(afterClear, "enumerateFailed=") != nullptr ||
        std::strstr(afterClear, "ma=") != nullptr) {
        std::fprintf(stderr, "error: cleared open-plan still changed the host text\n");
        return 28;
    }
    return 0;
}

inline int RunPlaybackOpenPlanSelfTest() {
    const harken::PlaybackOpenPlan first =
        harken::AfterDefaultPlaybackAttempt(0);
    if (!first.defaultAttempted || !first.defaultSucceeded ||
        first.defaultFailed || first.enumerateAttempted || first.resetImpl ||
        first.writeDeviceInitStage || first.abandonFailedDevice) {
        std::fprintf(stderr, "error: default success walked the fallback\n");
        return 30;
    }

    harken::PlaybackOpenPlan failedDefault =
        harken::AfterDefaultPlaybackAttempt(-1);
    if (!failedDefault.defaultFailed || !failedDefault.keepContext ||
        failedDefault.resetImpl || failedDefault.writeDeviceInitStage ||
        failedDefault.maCode != -1 || !failedDefault.abandonFailedDevice) {
        std::fprintf(stderr, "error: default failure reset the context\n");
        return 31;
    }

    const harken::PlaybackOpenPlan enumFailed =
        harken::AfterPlaybackEnumerateFailure(failedDefault);
    if (!enumFailed.enumerateFailed || !enumFailed.resetImpl ||
        !enumFailed.writeDeviceInitStage || enumFailed.maCode != -1 ||
        enumFailed.keepContext || !enumFailed.abandonFailedDevice) {
        std::fprintf(stderr, "error: enumerate failure lost the first ma code\n");
        return 32;
    }
    const harken::PlaybackOpenPlan enumFailedAfterAbandon =
        harken::AfterPlaybackEnumerateFailure(failedDefault);
    if (!enumFailedAfterAbandon.abandonFailedDevice) {
        std::fprintf(stderr, "error: enumerate failure cleared abandon\n");
        return 38;
    }

    harken::PlaybackOpenPlan firstId =
        harken::AfterEnumeratedPlaybackAttempt(failedDefault, 0, -4, 3);
    if (firstId.openedEnumeratedIndex != -1 || firstId.resetImpl ||
        firstId.writeDeviceInitStage || !firstId.keepContext ||
        firstId.triedEnumeratedCount != 1 || firstId.maCode != -4 ||
        !firstId.abandonFailedDevice) {
        std::fprintf(stderr, "error: first enumerated miss wrote the stage\n");
        return 33;
    }
    const harken::PlaybackOpenPlan secondId =
        harken::AfterEnumeratedPlaybackAttempt(firstId, 1, 0, 3);
    if (secondId.openedEnumeratedIndex != 1 || secondId.resetImpl ||
        secondId.writeDeviceInitStage || secondId.triedEnumeratedCount != 2 ||
        secondId.abandonFailedDevice) {
        std::fprintf(stderr, "error: first success was not the stop\n");
        return 34;
    }

    harken::PlaybackOpenPlan lastFail =
        harken::AfterEnumeratedPlaybackAttempt(failedDefault, 0, -5, 2);
    lastFail = harken::AfterEnumeratedPlaybackAttempt(lastFail, 1, -9, 2);
    if (!lastFail.resetImpl || !lastFail.writeDeviceInitStage ||
        lastFail.maCode != -9 || lastFail.openedEnumeratedIndex != -1 ||
        !lastFail.abandonFailedDevice) {
        std::fprintf(stderr, "error: last enumerated miss kept the first code\n");
        return 35;
    }
    const harken::PlaybackOpenPlan emptyList =
        harken::AfterEnumeratedPlaybackAttempt(failedDefault, 0, -1, 0);
    if (!emptyList.resetImpl || emptyList.maCode != -1 ||
        !emptyList.abandonFailedDevice) {
        std::fprintf(stderr, "error: empty playback list lost the first ma code\n");
        return 36;
    }

    int abandonCalls = 0;
    auto recordAbandon = [&](const harken::PlaybackOpenPlan& plan) {
        if (plan.abandonFailedDevice) {
            ++abandonCalls;
        }
    };
    harken::PlaybackOpenPlan flow = harken::AfterDefaultPlaybackAttempt(-1);
    recordAbandon(flow);
    flow = harken::AfterEnumeratedPlaybackAttempt(flow, 0, -2, 2);
    recordAbandon(flow);
    flow = harken::AfterEnumeratedPlaybackAttempt(flow, 1, 0, 2);
    recordAbandon(flow);
    if (abandonCalls != 2 || flow.openedEnumeratedIndex != 1 ||
        flow.abandonFailedDevice) {
        std::fprintf(stderr, "error: retry control flow missed abandon\n");
        return 37;
    }
    return 0;
}

}  // namespace harkenhost


#endif
