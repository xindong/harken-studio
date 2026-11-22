#include <harken/Harken.h>
#include "Monitor/MonitorSessionFile.h"
#include "Monitor/support/MonitorFixtureLoader.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

using harken::studio::Monitor::MonitorSessionFile;
using harken::studio::Monitor::MonitorSessionWriter;
using harken::studio::Monitor::TestSupport::loadEditorFixture;

TEST(MonitorSessionFile, WrittenRecordsRestoreInTheSameStudioVersion)
{
    const auto fixture = loadEditorFixture("optional_unknown.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = QDir(directory.path()).filePath("session.vnmon").toStdString();

    MonitorSessionWriter writer;
    ASSERT_TRUE(writer.open(path, {"1.2.3", 3, 7, 11}, 0));
    ASSERT_TRUE(writer.append(fixture.packed.data(), fixture.packed.size()));
    ASSERT_TRUE(writer.append(fixture.packed.data(), fixture.packed.size()));
    writer.close();

    const auto restored = MonitorSessionFile::restore(path);
    EXPECT_EQ(restored.status, VN_OK);
    EXPECT_EQ(restored.commitsWritten, 2u);
    EXPECT_EQ(restored.engineVersion, "1.2.3");
    EXPECT_EQ(restored.remoteProtocolMajor, 3u);
    EXPECT_EQ(restored.records.size(), 2u * fixture.expectedRecords);
}

TEST(MonitorSessionFile, ByteBudgetStopsTheRecordingInsteadOfTruncating)
{
    const auto fixture = loadEditorFixture("optional_unknown.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    QTemporaryDir directory;
    const auto path = QDir(directory.path()).filePath("budget.vnmon").toStdString();
    MonitorSessionWriter writer;
    ASSERT_TRUE(writer.open(path, {}, 128 + fixture.packed.size()));
    EXPECT_FALSE(writer.append(fixture.packed.data(), fixture.packed.size()));
    EXPECT_FALSE(writer.isOpen());
    EXPECT_EQ(MonitorSessionFile::restore(path).commitsWritten, 0u);
}
