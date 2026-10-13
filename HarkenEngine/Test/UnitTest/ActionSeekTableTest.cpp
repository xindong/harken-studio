#include "harken/core/ActionSeekTable.h"

#include <gtest/gtest.h>

#include <utility>

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
