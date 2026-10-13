#include "Transactions/InstallIndex.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using VNH::InstalledVersionRecord;
using VNH::InstallIndex;

namespace {

class MachineRootGuard
{
public:
    explicit MachineRootGuard(const QString& root)
    {
        qputenv("HARKEN_HUB_MACHINE_ROOT", root.toUtf8());
    }
    ~MachineRootGuard() { qunsetenv("HARKEN_HUB_MACHINE_ROOT"); }
};

InstalledVersionRecord makeRecord(const QString& buildId)
{
    InstalledVersionRecord record;
    record.buildId = buildId;
    record.semanticVersion = QStringLiteral("1.4.0");
    record.channel = QStringLiteral("stable");
    record.installPath = QStringLiteral("C:/somewhere/1.4.0-") + buildId;
    record.packageHash = QString(64, QLatin1Char('a'));
    record.installedAt = QStringLiteral("2026-07-23T12:00:00.000Z");
    record.healthState = QStringLiteral("Installed");
    return record;
}

}

TEST(InstallIndex, CommitAndReloadRoundTrip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const auto fresh = InstallIndex::snapshot();
    ASSERT_TRUE(fresh.ok);
    EXPECT_EQ(fresh.lastReleaseSequence, 0);
    QString error;
    ASSERT_TRUE(InstallIndex::commitInstallation(
        makeRecord(QStringLiteral("run12.abcd1234")), 7, &error))
        << error.toStdString();

    const auto loaded = InstallIndex::snapshot();
    ASSERT_TRUE(loaded.ok);
    EXPECT_EQ(loaded.lastReleaseSequence, 7);
    ASSERT_EQ(loaded.records.size(), 1u);
    EXPECT_EQ(loaded.records[0].buildId, QStringLiteral("run12.abcd1234"));
    EXPECT_EQ(loaded.records[0].healthState, QStringLiteral("Installed"));
    EXPECT_EQ(loaded.records[0].semanticVersion, QStringLiteral("1.4.0"));
}

TEST(InstallIndex, SequenceNeverRegressesAndRecordsUpsert)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    QString error;
    ASSERT_TRUE(InstallIndex::commitInstallation(
        makeRecord(QStringLiteral("run12.abcd1234")), 10, &error));
    ASSERT_TRUE(InstallIndex::commitInstallation(
        makeRecord(QStringLiteral("run12.abcd1234")), 5, &error));

    const auto loaded = InstallIndex::snapshot();
    ASSERT_TRUE(loaded.ok);
    EXPECT_EQ(loaded.lastReleaseSequence, 10);
    EXPECT_EQ(loaded.records.size(), 1u);
}

TEST(InstallIndex, SetHealthStateUpdatesOnlyKnownBuilds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    QString error;
    ASSERT_TRUE(InstallIndex::commitInstallation(
        makeRecord(QStringLiteral("run12.abcd1234")), 7, &error));

    ASSERT_TRUE(InstallIndex::setHealthState(QStringLiteral("run12.abcd1234"),
                                             QStringLiteral("NeedsRepair"),
                                             &error));
    EXPECT_EQ(InstallIndex::snapshot().records[0].healthState,
              QStringLiteral("NeedsRepair"));

    EXPECT_FALSE(InstallIndex::setHealthState(QStringLiteral("unknown"),
                                              QStringLiteral("Installed"),
                                              &error));
}

TEST(InstallIndex, CorruptIndexFileFailsClosed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    QFile file(InstallIndex::filePath());
    ASSERT_TRUE(QDir().mkpath(dir.path()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("not json"));
    file.close();

    const auto snapshot = InstallIndex::snapshot();
    EXPECT_FALSE(snapshot.ok);
    EXPECT_TRUE(snapshot.error.contains(QStringLiteral("corrupt")));

    QString error;
    EXPECT_FALSE(InstallIndex::commitInstallation(
        makeRecord(QStringLiteral("run12.abcd1234")), 7, &error));
    EXPECT_FALSE(InstallIndex::setHealthState(QStringLiteral("run12.abcd1234"),
                                              QStringLiteral("Installed"),
                                              &error));
}
