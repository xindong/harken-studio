// In-callback Vn_Term contract (deferred teardown). Terminating the engine
// from inside a user callback must never leave the outer Vn_Update running
// over freed engine state. Pinned semantics: Vn_Term inside a callback
// returns VN_OK and detaches the global engine immediately, so every
// subsequent API call from the same callback returns VN_NOT_INITIALIZED;
// the outer Vn_Update dispatches no further user callbacks, tears the
// engine down exactly once after the callback returns, and itself returns
// VN_OK; a nested Vn_Update inside a callback is a benign no-op (VN_OK,
// no recursive dispatch), and after an in-callback Term it reports
// VN_NOT_INITIALIZED like any other call; a fresh Vn_Init succeeds after
// the teardown. Every user-callback entry inside Vn_Update is covered:
// the audio-origin notification drain, the pre-audibility logic
// notification drain, the post-audibility logic notification drain, the
// audio log-record drain, and the logic log-record drain.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::Vec3;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

// Scriptable callback sink: each engine callback runs the test-provided
// body with its 1-based invocation ordinal. After the script terminated
// the engine, any further engine callback of either kind only increments
// callsAfterTerm — the contract says that counter must stay zero.
struct CallbackScript {
    std::function<void(const VnNotification&, int)> onNotification;
    std::function<void(std::uint32_t code, int)> onLog;
    int notificationCalls = 0;
    int logCalls = 0;
    int callsAfterTerm = 0;
    bool termed = false;

    // The in-callback termination under test: VN_OK, and the engine is
    // detached from this point on.
    void Term() {
        EXPECT_EQ(VN_OK, Vn_Term());
        termed = true;
    }
};

void ScriptedNotification(const VnNotification* notification, void* userData) {
    CallbackScript* script = static_cast<CallbackScript*>(userData);
    ++script->notificationCalls;
    if (script->termed) {
        ++script->callsAfterTerm;
        return;
    }
    if (script->onNotification) {
        script->onNotification(*notification, script->notificationCalls);
    }
}

void ScriptedLog(void* userData, std::uint32_t /*level*/, std::uint32_t code,
                 const char* /*text*/) {
    CallbackScript* script = static_cast<CallbackScript*>(userData);
    ++script->logCalls;
    if (script->termed) {
        ++script->callsAfterTerm;
        return;
    }
    if (script->onLog) {
        script->onLog(code, script->logCalls);
    }
}

VnConfig ScriptedConfig(CallbackScript* script, std::uint32_t maxVoices,
                        bool notifications = true, bool logs = true) {
    VnConfig config = MakeNullConfig(maxVoices, kBlock, kRate);
    if (notifications) {
        config.onNotification = &ScriptedNotification;
        config.notificationUserData = script;
    }
    if (logs) {
        config.logCallback = &ScriptedLog;
        config.logUserData = script;
    }
    return config;
}

// A source shorter than one block ends inside the first pumped block, which
// queues an audio-origin VOICE_ENDED for the next Vn_Update.
void RegisterShort(VnSoundId id) {
    const std::vector<float> dc = MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

// A one-second source never ends within the few blocks these drills pump.
void RegisterLong(VnSoundId id) {
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

// --- in-callback Term: repeat calls and follow-up API calls ---

TEST(CallbackTerm, SecondTermInSameCallbackReturnsNotInitialized) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    script.onNotification = [&script](const VnNotification&, int) {
        script.Term();
        // The engine detached at the first Term: a repeat call inside the
        // same callback reports the not-initialized state, not a crash and
        // not a second teardown.
        EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Term());
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.notificationCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

TEST(CallbackTerm, ApiCallsAfterInCallbackTermReturnNotInitialized) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    script.onNotification = [&script, &play, handle](const VnNotification&,
                                                     int) {
        script.Term();
        // Query surface after the in-callback Term.
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(VnSnapshot);
        EXPECT_EQ(VN_NOT_INITIALIZED, Vn_CaptureSnapshot(&snapshot));
        // Playback-control surface after the in-callback Term.
        VnVoiceHandle fresh = 0;
        EXPECT_EQ(VN_NOT_INITIALIZED, harkentest::PlayRaw(&play, &fresh));
        EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Stop(handle, 0));
        EXPECT_EQ(VN_NOT_INITIALIZED,
                  Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.notificationCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

// --- entry coverage: Term inside each of the five user-callback drains ---

TEST(CallbackTerm, TermInAudioNotificationDrainStopsDispatchAndUpdateOk) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    // Both 64-frame voices end inside the first block, so the audio-origin
    // queue holds two VOICE_ENDED records for this update.
    session.Engine()->nullBackend.Pump(1);

    script.onNotification = [&script](const VnNotification& notification,
                                      int call) {
        if (call != 1) {
            return;
        }
        EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED),
                  notification.type);
        script.Term();
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    // The second queued audio notification and every later drain stay
    // silent once the callback terminated the engine.
    EXPECT_EQ(1, script.notificationCalls);
    EXPECT_EQ(0, script.logCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

TEST(CallbackTerm, TermInLogicNotificationDrainStopsDispatchAndUpdateOk) {
    CallbackScript script;
    VnConfig config = ScriptedConfig(&script, 2);
    config.voiceStealPolicy = 2; // steal-oldest
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLong(harkentest::ObjectId(1));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.priority = 128;

    // Fill the pool, then two more plays steal and go pending (the second
    // steal picks the remaining live voice; a prior steal's victim is not
    // eligible again).
    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &a));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &b));
    VnVoiceHandle pendingFirst = 0;
    VnVoiceHandle pendingSecond = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &pendingFirst));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &pendingSecond));
    ASSERT_EQ(2u, engine->ledger.PendingStartCount());

    // Stopping a pending start emits its ended completion as a
    // logic-origin notification at stop time. Two cancels queue two
    // records; nothing was pumped, so the audio-origin queue is empty and
    // the first callback of this update is the logic-notification drain.
    ASSERT_EQ(VN_OK, Vn_Stop(pendingFirst, 0));
    ASSERT_EQ(VN_OK, Vn_Stop(pendingSecond, 0));
    ASSERT_EQ(0u, engine->ledger.PendingStartCount());

    script.onNotification = [&script, pendingFirst](
                                const VnNotification& notification, int call) {
        if (call != 1) {
            return;
        }
        EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED),
                  notification.type);
        EXPECT_EQ(pendingFirst, notification.voiceHandle);
        script.Term();
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.notificationCalls);
    EXPECT_EQ(0, script.logCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

TEST(CallbackTerm, TermInParkNotificationDrainStopsDispatchAndUpdateOk) {
    CallbackScript script;
    VnConfig config = ScriptedConfig(&script, 4);
    config.maxParkedVoices = 4;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterLong(harkentest::ObjectId(1));
    const VnAttenuationDesc attenuation = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &attenuation));

    // Two spatialized voices at distance 100 under a linear [1,100]
    // profile sit at the attenuation floor: both park in this update's
    // audibility pass, and their PARKED notifications are delivered by the
    // post-audibility drain of the SAME update. Nothing reaches the audio
    // or pre-audibility drains first (no voice ended, nothing cancelled).
    const VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(1), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(first, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(second, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                                    Vec3(0.0f, 0.0f, -1.0f),
                                    Vec3(0.0f, 1.0f, 0.0f)));
    engine->nullBackend.Pump(1);

    script.onNotification = [&script](const VnNotification& notification,
                                      int call) {
        if (call != 1) {
            return;
        }
        EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_PARKED),
                  notification.type);
        script.Term();
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.notificationCalls);
    EXPECT_EQ(0, script.logCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

TEST(CallbackTerm, TermInAudioLogDrainStopsDispatchAndUpdateOk) {
    CallbackScript script;
    // Log sink only: the degradation notifications the overflow also queues
    // must not open an earlier callback entry, so the first user callback
    // of the update is the audio log-record drain.
    VnConfig config =
        ScriptedConfig(&script, 2, /*notifications=*/false, /*logs=*/true);
    // Room for the master bus accumulators (2 planes x 256 frames x 4
    // bytes) but not for one mono voice scratch plane: every pumped block
    // overflows the arena and pushes one audio-side log record.
    config.arenaBytes = 2560;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterLong(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    // Two blocks -> two arena-overflow records queued for this update.
    session.Engine()->nullBackend.Pump(2);

    script.onLog = [&script](std::uint32_t code, int call) {
        if (call != 1) {
            return;
        }
        EXPECT_EQ(harken::kLogCodeArenaOverflow, code);
        script.Term();
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.logCalls);
    EXPECT_EQ(0, script.notificationCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

TEST(CallbackTerm, TermInLogicLogDrainStopsDispatchAndUpdateOk) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 4));
    ASSERT_EQ(VN_OK, session.InitResult());
    for (VnBusId bus : {harkentest::ObjectId(100u), harkentest::ObjectId(101u), harkentest::ObjectId(102u)}) {
        ASSERT_EQ(VN_OK, Vn_CreateBus(bus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(bus, -6.0f, 0));
    }
    // Three active bus target rows against a capacity-1 buffer: each
    // truncating capture books one logic-side log record. Two calls queue
    // two records; nothing was pumped, so the audio log ring is empty and
    // no notification exists — the update's first (and under the contract
    // only) user callback is the logic log-record drain.
    VnPropertyTargetInfo row{};
    row.structSize = sizeof(VnPropertyTargetInfo);
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(&row, 1, &required));
    ASSERT_EQ(3u, required);
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(&row, 1, &required));

    script.onLog = [&script](std::uint32_t code, int call) {
        if (call != 1) {
            return;
        }
        EXPECT_EQ(harken::kLogCodeCaptureTruncated, code);
        script.Term();
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.logCalls);
    EXPECT_EQ(0, script.notificationCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

// --- nested Vn_Update inside a callback ---

TEST(CallbackTerm, NestedUpdateInsideCallbackIsBenignNoOp) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    session.Engine()->nullBackend.Pump(1);

    VnResult nestedResult = VN_INVALID_ARG;
    int callsSeenAfterNested = -1;
    script.onNotification = [&](const VnNotification&, int call) {
        if (call != 1) {
            return;
        }
        // Two audio notifications are queued; a recursive dispatch would
        // deliver the second one inside this nested call.
        nestedResult = Vn_Update();
        callsSeenAfterNested = script.notificationCalls;
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, nestedResult);
    // Benign no-op: the nested update dispatched nothing...
    EXPECT_EQ(1, callsSeenAfterNested);
    // ...and the outer drain still delivered both records exactly once.
    EXPECT_EQ(2, script.notificationCalls);
}

TEST(CallbackTerm, CurrentWorldLoadInsideCallbackRefusesBeforeFileAccess) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const std::string path =
        std::string(HARKEN_TEMP_DIR "/callback-current-world-missing.vnworld");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    VnResult loadResult = VN_OK;
    VnCurrentWorldLoadReport report{};
    script.onNotification = [&](const VnNotification&, int call) {
        if (call != 1) {
            return;
        }
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = path.c_str();
        report.structSize = sizeof(report);
        loadResult = Vn_LoadCurrentWorld(&desc, &report);
    };

    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_BACKPRESSURE, loadResult);
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_BACKPRESSURE, report.status);
    EXPECT_EQ(VN_BACKPRESSURE, report.result);
    EXPECT_EQ(0u, report.bodyBytes);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
}

TEST(CallbackTerm, NestedUpdateAfterInCallbackTermReturnsNotInitialized) {
    CallbackScript script;
    EngineSession session(ScriptedConfig(&script, 2));
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    script.onNotification = [&script](const VnNotification&, int) {
        script.Term();
        // The engine detached at Term, so the nested update reports the
        // not-initialized state rather than acting as the benign no-op of
        // a live nested update.
        EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Update());
    };
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1, script.notificationCalls);
    EXPECT_EQ(0, script.callsAfterTerm);
}

// --- teardown completeness: a fresh engine after an in-callback Term ---

TEST(CallbackTerm, InitDuringInCallbackTermPendingReturnsAlreadyInitialized) {
    CallbackScript script;
    const VnConfig fresh = MakeNullConfig(2, kBlock, kRate);
    {
        EngineSession session(ScriptedConfig(&script, 2));
        ASSERT_EQ(VN_OK, session.InitResult());
        RegisterShort(harkentest::ObjectId(1));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        session.Engine()->nullBackend.Pump(1);

        script.onNotification = [&script, &fresh](const VnNotification&, int) {
            script.Term();
            // The Term above only detached the engine; the object is still
            // alive and owned by the outer Vn_Update until its deferred
            // teardown runs. The session is therefore not over yet, and a
            // re-init inside this window must be refused as ALREADY (not
            // NOT) initialized — if it succeeded, the deferred teardown
            // would null the fresh engine's global pointer and destroy it a
            // second time.
            EXPECT_EQ(VN_ALREADY_INITIALIZED, Vn_Init(&fresh));
        };
        EXPECT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(1, script.notificationCalls);
        EXPECT_EQ(0, script.callsAfterTerm);
    }

    // The outer update's epilogue completed the deferred teardown, so the
    // same config that was just refused now initializes cleanly.
    EXPECT_EQ(VN_OK, Vn_Init(&fresh));
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(CallbackTerm, ReinitAfterInCallbackTermSucceeds) {
    CallbackScript script;
    {
        EngineSession session(ScriptedConfig(&script, 2));
        ASSERT_EQ(VN_OK, session.InitResult());
        RegisterShort(harkentest::ObjectId(1));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        session.Engine()->nullBackend.Pump(1);

        script.onNotification = [&script](const VnNotification&, int) {
            script.Term();
        };
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(1, script.notificationCalls);
    }

    // The in-callback Term completed the teardown, so a fresh session
    // initializes and runs a full play/pump/update round trip.
    const VnConfig fresh = MakeNullConfig(2, kBlock, kRate);
    ASSERT_EQ(VN_OK, Vn_Init(&fresh));
    RegisterShort(harkentest::ObjectId(1));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harken::detail::ActiveEngine()->nullBackend.Pump(1);
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_Term());
}

} // namespace
