#include "harken/core/ActionSeekTable.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <utility>

TEST(ActionSeekTable, ConcurrentQueryObservesWholeCompletionRecords) {
    harken::ActionSeekTable table;
    ASSERT_TRUE(table.Reset(1));
    std::atomic<VnActionSeekTicket> requested{0};
    std::atomic<VnActionSeekTicket> finished{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> writerOk{true};
    std::thread writer([&] {
        VnActionSeekTicket previous = 0;
        std::uint32_t iteration = 0;
        while (!stopping.load(std::memory_order_acquire)) {
            const auto ticket = requested.load(std::memory_order_acquire);
            if (ticket == 0 || ticket == previous) {
                std::this_thread::yield();
                continue;
            }
            const bool ok = (iteration++ & 1u) == 0
                ? table.TryBeginApply(ticket) && table.FinishApplied(ticket, 99, 123, 456)
                : table.FinishRequestedCancellation(ticket);
            if (!ok) writerOk.store(false, std::memory_order_relaxed);
            previous = ticket;
            finished.store(ticket, std::memory_order_release);
        }
    });
    bool controlOk = true;
    std::uint32_t mixedRecords = 0;
    for (std::uint32_t iteration = 0; iteration < 20000; ++iteration) {
        VnActionSeekTicket ticket = 0;
        if (!table.Prepare(101, 2, iteration + 1u, &ticket) || !table.Commit(ticket)) {
            controlOk = false;
            break;
        }
        const bool cancel = (iteration & 1u) != 0;
        if (cancel && !table.RequestCancel(ticket, VN_STALE_HANDLE)) {
            controlOk = false;
            break;
        }
        requested.store(ticket, std::memory_order_release);
        const auto observe = [&]() -> VnActionSeekState {
            VnActionSeekInfoV1 info{};
            if (table.Query(ticket, &info) != VN_OK) {
                controlOk = false;
                return VN_ACTION_SEEK_ACCEPTED;
            }
            controlOk &= info.firing == 101 && info.targetCount == 2 &&
                info.acceptedLogicSequence == iteration + 1u;
            if (info.state == VN_ACTION_SEEK_ACCEPTED) {
                if (info.result != VN_OK || info.appliedCount != 0 ||
                    info.cancelledCount != 0 || info.appliedAudioBlockSequence != 0 ||
                    info.minimumAppliedSourceFrame != 0 || info.maximumAppliedSourceFrame != 0)
                    ++mixedRecords;
            } else if (cancel) {
                controlOk &= info.state == VN_ACTION_SEEK_CANCELLED &&
                    info.result == VN_STALE_HANDLE && info.cancelledCount == 2 &&
                    info.appliedCount == 0 && info.appliedAudioBlockSequence == 0 &&
                    info.minimumAppliedSourceFrame == 0 && info.maximumAppliedSourceFrame == 0;
            } else {
                controlOk &= info.state == VN_ACTION_SEEK_APPLIED && info.result == VN_OK &&
                    info.appliedCount == 2 && info.cancelledCount == 0 &&
                    info.appliedAudioBlockSequence == 99 &&
                    info.minimumAppliedSourceFrame == 123 && info.maximumAppliedSourceFrame == 456;
            }
            return info.state;
        };
        do {
            observe();
        } while (finished.load(std::memory_order_acquire) != ticket);
        controlOk &= observe() == (cancel ? VN_ACTION_SEEK_CANCELLED : VN_ACTION_SEEK_APPLIED);
        controlOk &= table.Release(ticket) == VN_OK;
        if (!controlOk) break;
    }
    stopping.store(true, std::memory_order_release);
    writer.join();
    EXPECT_TRUE(writerOk.load(std::memory_order_relaxed));
    EXPECT_TRUE(controlOk);
    EXPECT_EQ(0u, mixedRecords);
}

TEST(ActionSeekTable, CancellationAndApplyUseOneAtomicWinner) {
    harken::ActionSeekTable table;
    ASSERT_TRUE(table.Reset(2));

    VnActionSeekTicket cancelled = 0;
    ASSERT_TRUE(table.Prepare(101, 2, 7, &cancelled));
    ASSERT_TRUE(table.Commit(cancelled));
    ASSERT_TRUE(table.RequestCancel(cancelled, VN_STALE_HANDLE));
    EXPECT_FALSE(table.TryBeginApply(cancelled));
    ASSERT_TRUE(table.FinishCancelled(cancelled, VN_STALE_HANDLE));

    VnActionSeekInfoV1 info{};
    ASSERT_EQ(VN_OK, table.Query(cancelled, &info));
    EXPECT_EQ(VN_ACTION_SEEK_CANCELLED, info.state);
    EXPECT_EQ(VN_STALE_HANDLE, info.result);
    EXPECT_EQ(0u, info.appliedCount);
    EXPECT_EQ(2u, info.cancelledCount);

    VnActionSeekTicket applied = 0;
    ASSERT_TRUE(table.Prepare(202, 1, 8, &applied));
    ASSERT_TRUE(table.Commit(applied));
    ASSERT_TRUE(table.TryBeginApply(applied));
    EXPECT_FALSE(table.RequestCancel(applied, VN_STALE_HANDLE));
    ASSERT_TRUE(table.FinishApplied(applied, 99, 123, 123));
    ASSERT_EQ(VN_OK, table.Query(applied, &info));
    EXPECT_EQ(VN_ACTION_SEEK_APPLIED, info.state);
    EXPECT_EQ(VN_OK, info.result);
    EXPECT_EQ(1u, info.appliedCount);
    EXPECT_EQ(99u, info.appliedAudioBlockSequence);
    EXPECT_EQ(123u, info.minimumAppliedSourceFrame);
    EXPECT_EQ(123u, info.maximumAppliedSourceFrame);

    EXPECT_FALSE(table.RequestCancel(applied, VN_STALE_HANDLE));
    ASSERT_EQ(VN_OK, table.Query(applied, &info));
    EXPECT_EQ(VN_ACTION_SEEK_APPLIED, info.state);
    EXPECT_EQ(VN_OK, info.result);
}

TEST(ActionSeekTable, ResetDoesNotReviveAnOldTicket) {
    harken::ActionSeekTable table;
    ASSERT_TRUE(table.Reset(1));
    VnActionSeekTicket oldTicket = 0;
    ASSERT_TRUE(table.Prepare(101, 1, 1, &oldTicket));
    ASSERT_TRUE(table.Commit(oldTicket));

    ASSERT_TRUE(table.Reset(1));
    VnActionSeekTicket newTicket = 0;
    ASSERT_TRUE(table.Prepare(202, 1, 2, &newTicket));
    ASSERT_TRUE(table.Commit(newTicket));

    EXPECT_NE(oldTicket, newTicket);
    VnActionSeekInfoV1 info{};
    EXPECT_EQ(VN_STALE_HANDLE, table.Query(oldTicket, &info));
    EXPECT_EQ(VN_OK, table.Query(newTicket, &info));
    EXPECT_EQ(202u, info.firing);
}

TEST(ActionSeekTable, ShutdownCancelsEveryNonTerminalOperation) {
    harken::ActionSeekTable table;
    ASSERT_TRUE(table.Reset(3));

    VnActionSeekTicket accepted = 0;
    VnActionSeekTicket cancelRequested = 0;
    VnActionSeekTicket applying = 0;
    ASSERT_TRUE(table.Prepare(101, 1, 1, &accepted));
    ASSERT_TRUE(table.Commit(accepted));
    ASSERT_TRUE(table.Prepare(202, 2, 2, &cancelRequested));
    ASSERT_TRUE(table.Commit(cancelRequested));
    ASSERT_TRUE(table.RequestCancel(cancelRequested, VN_STALE_HANDLE));
    ASSERT_TRUE(table.Prepare(303, 3, 3, &applying));
    ASSERT_TRUE(table.Commit(applying));
    ASSERT_TRUE(table.TryBeginApply(applying));

    table.CancelAllAfterAudioJoin(VN_NOT_INITIALIZED);

    for (const auto [ticket, count] : {
             std::pair{accepted, 1u},
             std::pair{cancelRequested, 2u},
             std::pair{applying, 3u},
         }) {
        VnActionSeekInfoV1 info{};
        ASSERT_EQ(VN_OK, table.Query(ticket, &info));
        EXPECT_EQ(VN_ACTION_SEEK_CANCELLED, info.state);
        EXPECT_EQ(VN_NOT_INITIALIZED, info.result);
        EXPECT_EQ(0u, info.appliedCount);
        EXPECT_EQ(count, info.cancelledCount);
        EXPECT_EQ(VN_OK, table.Release(ticket));
    }
}
