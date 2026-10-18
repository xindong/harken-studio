#include "Versions/LocalBuildRegistry.h"
#include "Versions/VersionScanner.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <atomic>
#include <thread>

using VNH::LocalBuildRecord;
using VNH::LocalBuildRegistry;
using VNH::VersionScanner;

namespace {

class LocalBuildRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
    }

    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    LocalBuildRecord record(const QString& executablePath) const
    {
        LocalBuildRecord value;
        value.executablePath = executablePath;
        value.reportedBuildId = QStringLiteral("local.aaaa1111");
        value.semanticVersion = QStringLiteral("0.1.21-dev");
        value.channel = QStringLiteral("dev");
        value.buildType = QStringLiteral("Debug");
        value.capabilitiesVersion = 2;
        return value;
    }

    LocalBuildRecord recordForExistingFile(const QString& executablePath) const
    {
        LocalBuildRecord value = record(executablePath);
        value.executableFingerprint = LocalBuildRegistry::fileFingerprint(executablePath);
        const QFileInfo executable(executablePath);
        value.executableSize = executable.size();
        value.executableModifiedMs =
            executable.lastModified().toMSecsSinceEpoch();
        return value;
    }

    QTemporaryDir dir;
};

TEST_F(LocalBuildRegistryTest, SavesUpdatesAndRemovesLocalBuild)
{
    const QString executable = dir.filePath(QStringLiteral("HarkenStudio.exe"));
    QString error;
    ASSERT_TRUE(LocalBuildRegistry::upsert(record(executable), &error))
        << error.toStdString();

    auto records = LocalBuildRegistry::load(&error);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().reportedBuildId,
              QStringLiteral("local.aaaa1111"));

    LocalBuildRecord updated = record(executable);
    updated.reportedBuildId = QStringLiteral("local.bbbb2222");
    ASSERT_TRUE(LocalBuildRegistry::upsert(updated, &error))
        << error.toStdString();
    records = LocalBuildRegistry::load(&error);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().reportedBuildId,
              QStringLiteral("local.bbbb2222"));

    ASSERT_TRUE(LocalBuildRegistry::remove(executable, &error))
        << error.toStdString();
    EXPECT_TRUE(LocalBuildRegistry::load(&error).empty());
}

TEST_F(LocalBuildRegistryTest, ConcurrentReadsAndMutationsPreserveAllRecords)
{
    const auto first = record(dir.filePath("FirstStudio.exe"));
    const auto second = record(dir.filePath("SecondStudio.exe"));
    QString error;
    ASSERT_TRUE(LocalBuildRegistry::upsert(first, &error)) << error.toStdString();
    ASSERT_TRUE(LocalBuildRegistry::upsert(second, &error)) << error.toStdString();
    std::atomic<bool> reading{false};
    std::atomic<bool> done{false};
    std::thread reader([&]() {
        do {
            QString readError;
            const auto records = LocalBuildRegistry::load(&readError);
            EXPECT_TRUE(readError.isEmpty()) << readError.toStdString();
            EXPECT_EQ(records.size(), 2u);
            reading = true;
        } while (!done);
    });
    while (!reading)
        std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
        auto updated = first;
        updated.reportedBuildId = QString::number(i);
        EXPECT_TRUE(LocalBuildRegistry::updateExisting(updated, &error))
            << error.toStdString();
    }
    done = true;
    reader.join();
    const auto records = LocalBuildRegistry::load(&error);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records.front().reportedBuildId, QStringLiteral("99"));
    EXPECT_EQ(records.back().reportedBuildId, second.reportedBuildId);
}

TEST_F(LocalBuildRegistryTest, StableIdDependsOnNormalizedPath)
{
    const QString path = dir.filePath(QStringLiteral("build/HarkenStudio.exe"));
    const QString equivalent =
        QDir::cleanPath(dir.filePath(QStringLiteral("build/../build/HarkenStudio.exe")));
    EXPECT_EQ(LocalBuildRegistry::stableIdForPath(path),
              LocalBuildRegistry::stableIdForPath(equivalent));
    EXPECT_TRUE(LocalBuildRegistry::stableIdForPath(path)
                    .startsWith(QStringLiteral("local-user-")));
}

TEST_F(LocalBuildRegistryTest, ContainsMatchesTheSameExecutablePath)
{
    const QString executable =
        dir.filePath(QStringLiteral("build/HarkenStudio.exe"));
    QString error;
    ASSERT_TRUE(LocalBuildRegistry::upsert(record(executable), &error))
        << error.toStdString();

    const QString equivalent = QDir::cleanPath(
        dir.filePath(QStringLiteral("build/../build/HarkenStudio.exe")));
    EXPECT_TRUE(LocalBuildRegistry::contains(equivalent, &error));
    EXPECT_FALSE(LocalBuildRegistry::contains(
        dir.filePath(QStringLiteral("other/HarkenStudio.exe")), &error));
}

TEST_F(LocalBuildRegistryTest, ScanKeepsTheExactExecutablePathWithoutRunningIt)
{
    const QString executable =
        dir.filePath(QStringLiteral("custom/output/StudioUnderTest.exe"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(executable).absolutePath()));
    QFile binary(executable);
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    binary.write("not an executable");
    binary.close();

    QString error;
    ASSERT_TRUE(
        LocalBuildRegistry::upsert(recordForExistingFile(executable), &error))
        << error.toStdString();

    const auto versions = VersionScanner::scanLocalBuilds();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions.front().executablePath,
              QFileInfo(executable).absoluteFilePath());
    EXPECT_EQ(versions.front().installPath,
              QFileInfo(executable).absolutePath());
    EXPECT_TRUE(versions.front().available);
    EXPECT_EQ(versions.front().expectedBuildId(),
              recordForExistingFile(executable).reportedBuildId);
}

TEST_F(LocalBuildRegistryTest, ScanBlocksAChangedExecutableUntilItIsRechecked)
{
    const QString executable =
        dir.filePath(QStringLiteral("custom/output/HarkenStudio.exe"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(executable).absolutePath()));
    QFile binary(executable);
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    binary.write("first build");
    binary.close();

    QString error;
    ASSERT_TRUE(
        LocalBuildRegistry::upsert(recordForExistingFile(executable), &error))
        << error.toStdString();
    ASSERT_TRUE(binary.open(QIODevice::Append));
    binary.write(" changed");
    binary.close();

    const auto versions = VersionScanner::scanLocalBuilds();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_FALSE(versions.front().available);
    EXPECT_TRUE(versions.front().availabilityError.contains(
        QStringLiteral("being checked again")));
}

TEST_F(LocalBuildRegistryTest, VerifiedHistoricalProtocolRemainsAvailableUntilFileChanges)
{
    const auto executable = dir.filePath(QStringLiteral("HarkenStudio.exe"));
    QFile binary(executable);
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    binary.write("unchanged old binary");
    binary.close();
    auto record = recordForExistingFile(executable);
    record.capabilitiesVersion = 1;
    EXPECT_TRUE(LocalBuildRegistry::metadataMatchesFile(record));
    QString error;
    ASSERT_TRUE(LocalBuildRegistry::upsert(record, &error));
    const auto versions = VersionScanner::scanLocalBuilds();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_TRUE(versions.front().available);
    ASSERT_TRUE(binary.open(QIODevice::Append));
    binary.write(" changed");
    binary.close();
    EXPECT_FALSE(LocalBuildRegistry::metadataMatchesFile(record));
    EXPECT_FALSE(VersionScanner::scanLocalBuilds().front().available);
}

TEST_F(LocalBuildRegistryTest, CacheWithoutContentFingerprintRequiresRecheck)
{
    const auto executable = dir.filePath("HarkenStudio.exe");
    QFile binary(executable);
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    binary.write("unchanged binary");
    binary.close();
    auto cached = recordForExistingFile(executable);
    ASSERT_TRUE(LocalBuildRegistry::metadataMatchesFile(cached));
    cached.executableFingerprint.clear();
    EXPECT_FALSE(LocalBuildRegistry::metadataMatchesFile(cached));
}

TEST_F(LocalBuildRegistryTest, StaleRecheckRequestCannotInvalidateANewerVerifiedBuild)
{
    auto latest = record(dir.filePath("HarkenStudio.exe"));
    latest.executableFingerprint = QString(64, QLatin1Char('b'));
    QString error;
    ASSERT_TRUE(LocalBuildRegistry::upsert(latest, &error));
    ASSERT_TRUE(LocalBuildRegistry::requestRecheck(latest.executablePath,
        QString(64, QLatin1Char('a')), &error));
    const auto records = LocalBuildRegistry::load();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().executableFingerprint, latest.executableFingerprint);
    EXPECT_TRUE(records.front().lastProbeError.isEmpty());
}

TEST_F(LocalBuildRegistryTest, BackgroundRefreshCannotRestoreARemovedBuild)
{
    const QString executable = dir.filePath(QStringLiteral("HarkenStudio.exe"));
    QString error;
    ASSERT_TRUE(LocalBuildRegistry::upsert(record(executable), &error))
        << error.toStdString();
    ASSERT_TRUE(LocalBuildRegistry::remove(executable, &error))
        << error.toStdString();

    LocalBuildRecord refreshed = record(executable);
    refreshed.reportedBuildId = QStringLiteral("local.refreshed");
    EXPECT_FALSE(LocalBuildRegistry::updateExisting(refreshed, &error));
    EXPECT_TRUE(LocalBuildRegistry::load().empty());
}

TEST_F(LocalBuildRegistryTest, InvalidRegistryCannotBeOverwritten)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(LocalBuildRegistry::filePath())
                                 .absolutePath()));
    const QByteArray invalid = QByteArrayLiteral("{invalid registry");
    QFile file(LocalBuildRegistry::filePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(invalid);
    file.close();

    QString error;
    EXPECT_FALSE(LocalBuildRegistry::upsert(
        record(dir.filePath(QStringLiteral("HarkenStudio.exe"))), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(LocalBuildRegistry::remove(
        dir.filePath(QStringLiteral("HarkenStudio.exe")), &error));

    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), invalid);
}

}
