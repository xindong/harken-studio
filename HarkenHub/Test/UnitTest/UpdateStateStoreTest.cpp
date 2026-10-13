#include "Update/UpdateStateStore.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QTemporaryDir>

using VNH::UpdateStateRecord;
using VNH::UpdateStateStore;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

class UpdateStateStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
    }
    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    QTemporaryDir dir;
};

TEST_F(UpdateStateStoreTest, RoundTripsRecords)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenStudio");
    record.state = QStringLiteral("Verified");
    record.version = QStringLiteral("1.5.0");
    record.buildId = QStringLiteral("run3.cccc3333");
    record.downloadPath = QStringLiteral("C:/downloads/pkg.zip");
    record.manifestEnvelopePath = QStringLiteral("C:/downloads/pkg.manifest");
    record.expectedSha256 = QString(64, QLatin1Char('a'));
    record.packageSize = 12345;
    record.downloadedBytes = 6789;
    record.releaseSequence = 7;
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    const UpdateStateRecord loaded =
        UpdateStateStore::load(QStringLiteral("HarkenStudio"));
    EXPECT_TRUE(loaded.found);
    EXPECT_EQ(loaded.state, QStringLiteral("Verified"));
    EXPECT_EQ(loaded.buildId, QStringLiteral("run3.cccc3333"));
    EXPECT_EQ(loaded.packageSize, 12345);
    EXPECT_EQ(loaded.downloadedBytes, 6789);
    EXPECT_EQ(loaded.releaseSequence, 7);
    EXPECT_EQ(loaded.updatedAt, QStringLiteral("2026-07-24T12:00:00.000Z"));

    const UpdateStateRecord other =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    EXPECT_FALSE(other.found);
}

TEST_F(UpdateStateStoreTest, KeepsProductsIndependent)
{
    UpdateStateRecord studio;
    studio.product = QStringLiteral("HarkenStudio");
    studio.state = QStringLiteral("Downloading");
    ASSERT_TRUE(UpdateStateStore::save(studio, kNow));
    UpdateStateRecord hub;
    hub.product = QStringLiteral("HarkenHub");
    hub.state = QStringLiteral("AwaitingUserApproval");
    ASSERT_TRUE(UpdateStateStore::save(hub, kNow));

    ASSERT_TRUE(UpdateStateStore::clear(QStringLiteral("HarkenStudio")));
    EXPECT_FALSE(
        UpdateStateStore::load(QStringLiteral("HarkenStudio")).found);
    EXPECT_TRUE(
        UpdateStateStore::load(QStringLiteral("HarkenHub")).found);
}

TEST_F(UpdateStateStoreTest, RejectsIncompleteRecords)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenStudio");
    EXPECT_FALSE(UpdateStateStore::save(record, kNow));
    record.product.clear();
    record.state = QStringLiteral("Verified");
    EXPECT_FALSE(UpdateStateStore::save(record, kNow));
}

}
