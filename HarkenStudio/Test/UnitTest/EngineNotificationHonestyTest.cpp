#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/AudioEngine.h"
#include "RuntimeBridge/NativeDiagnostics.h"

#include <harken/Harken.h>

#include <QCoreApplication>
#include <QThread>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>

namespace Bridge = HarkenEditor::Bridge;

namespace {

void pumpUi()
{
    if (QCoreApplication::instance())
        QCoreApplication::processEvents();
}

bool hasKind(const std::vector<Bridge::EngineNotification>& items,
             Bridge::EngineNotification::Kind kind)
{
    return std::any_of(items.begin(), items.end(), [kind](const auto& item) {
        return item.kind == kind;
    });
}

}

TEST(EngineNotificationHonesty, InjectedSevenKindsKeepZeroHandleAndTimestamp)
{
    std::vector<Bridge::EngineNotification> leftover;
    Bridge::pollEngineNotifications(leftover);

    const struct Sample
    {
        uint32_t type;
        uint64_t voice;
        uint64_t stamp;
        Bridge::EngineNotification::Kind kind;
        const char* errorType;
    } samples[] = {
        {VN_NOTIFY_DEGRADATION_OCCURRED, 0, 11,
         Bridge::EngineNotification::Kind::ExecError, "DEGRADATION_OCCURRED"},
        {VN_NOTIFY_VOICE_STARVED, 0, 12,
         Bridge::EngineNotification::Kind::ExecError, "VOICE_STARVED"},
        {VN_NOTIFY_VOICE_STOLEN, 7, 13,
         Bridge::EngineNotification::Kind::VoiceStolen, nullptr},
        {VN_NOTIFY_VOICE_PARKED, 8, 14,
         Bridge::EngineNotification::Kind::VoiceParked, nullptr},
        {VN_NOTIFY_VOICE_RETURNED, 9, 15,
         Bridge::EngineNotification::Kind::VoiceReturned, nullptr},
        {VN_NOTIFY_VOICE_ENDED, 3, 16,
         Bridge::EngineNotification::Kind::PlaybackEnded, nullptr},
        {VN_NOTIFY_VOICE_STOPPED_BY_FADE, 4, 17,
         Bridge::EngineNotification::Kind::PlaybackEnded, nullptr},
    };

    for (const auto& sample : samples)
        Bridge::injectRuntimeNotificationForTest(sample.type, sample.voice, sample.stamp);
    pumpUi();

    std::vector<Bridge::EngineNotification> items;
    Bridge::pollEngineNotifications(items);

    EXPECT_FALSE(hasKind(items, Bridge::EngineNotification::Kind::PlaybackEnded));

    int queued = 0;
    for (const auto& sample : samples)
    {
        if (sample.type == VN_NOTIFY_VOICE_ENDED
            || sample.type == VN_NOTIFY_VOICE_STOPPED_BY_FADE)
        {
            continue;
        }
        ++queued;
        const auto it = std::find_if(items.begin(), items.end(),
                                     [&](const Bridge::EngineNotification& item) {
                                         return item.kind == sample.kind
                                             && item.voiceHandle == sample.voice
                                             && item.blockTimestamp == sample.stamp;
                                     });
        ASSERT_NE(it, items.end());
        if (sample.errorType)
        {
            EXPECT_EQ(it->errorType, QString::fromUtf8(sample.errorType));
            EXPECT_EQ(it->domain, Bridge::kExecErrorDomainUnclassified);
            EXPECT_TRUE(it->message.isEmpty());
        }
        EXPECT_FALSE(it->blockTimestamp == 0);
    }
    EXPECT_EQ(queued, 5);
}

TEST(EngineNotificationHonesty, DegradationAndStarvedEnterErrorsThroughMonitorPath)
{
    VNX::AudioEngine engine;
    engine.clearExecErrors(-1);
    const int before = engine.recentExecErrors(-1, -1).size();

    Bridge::injectRuntimeNotificationForTest(VN_NOTIFY_DEGRADATION_OCCURRED, 0, 21);
    Bridge::injectRuntimeNotificationForTest(VN_NOTIFY_VOICE_STARVED, 0, 22);
    Bridge::injectRuntimeNotificationForTest(VN_NOTIFY_VOICE_STOLEN, 5, 23);
    Bridge::injectRuntimeNotificationForTest(VN_NOTIFY_VOICE_PARKED, 6, 24);
    Bridge::injectRuntimeNotificationForTest(VN_NOTIFY_VOICE_RETURNED, 7, 25);
    pumpUi();
    engine.consumeRuntimeNotificationsForTest();

    const auto errors = engine.recentExecErrors(-1, -1);
    ASSERT_GE(errors.size(), before + 2);
    int addedDegrade = 0;
    int addedStarve = 0;
    for (int i = before; i < errors.size(); ++i)
    {
        EXPECT_EQ(errors[i].domain, VNX::AudioEngine::ExecErrorDomain::Unclassified);
        EXPECT_TRUE(errors[i].message.isEmpty());
        if (errors[i].errorType == QStringLiteral("DEGRADATION_OCCURRED"))
            ++addedDegrade;
        if (errors[i].errorType == QStringLiteral("VOICE_STARVED"))
            ++addedStarve;
        EXPECT_NE(errors[i].errorType, QStringLiteral("VOICE_STOLEN"));
        EXPECT_NE(errors[i].errorType, QStringLiteral("VOICE_PARKED"));
        EXPECT_NE(errors[i].errorType, QStringLiteral("VOICE_RETURNED"));
    }
    EXPECT_EQ(addedDegrade, 1);
    EXPECT_EQ(addedStarve, 1);
}

TEST(EngineNotificationHonesty, ShutdownDropsQueuedCrossThreadDelivery)
{
    std::vector<Bridge::EngineNotification> leftover;
    Bridge::pollEngineNotifications(leftover);

    std::atomic<bool> enqueued { false };
    QThread worker;
    QObject workerObject;
    workerObject.moveToThread(&worker);
    QObject::connect(&worker, &QThread::started, &workerObject, [&]() {
        Bridge::injectRuntimeNotificationForTest(VN_NOTIFY_DEGRADATION_OCCURRED, 0, 31);
        enqueued.store(true);
    });
    worker.start();
    while (!enqueued.load())
        QThread::yieldCurrentThread();
    worker.quit();
    worker.wait();

    Bridge::invalidateRuntimeNotificationsForTest();
    pumpUi();

    std::vector<Bridge::EngineNotification> items;
    Bridge::pollEngineNotifications(items);
    EXPECT_FALSE(hasKind(items, Bridge::EngineNotification::Kind::ExecError));
}

TEST(EngineNotificationHonesty, RemovedKindsAreGone)
{
    using Kind = Bridge::EngineNotification::Kind;
    static_assert(static_cast<uint8_t>(Kind::ExecError) == 0);
    static_assert(static_cast<uint8_t>(Kind::PlaybackEnded) == 1);
    static_assert(static_cast<uint8_t>(Kind::VoiceStolen) == 2);
    EXPECT_NE(static_cast<int>(Kind::VoiceStolen), 3);
}
