#include "Transactions/UninstallExecutor.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/TransactionJournal.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

#include <memory>

using VNH::InstallIndex;
using VNH::InstalledVersionRecord;
using VNH::UninstallExecutor;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

class EnvGuard
{
public:
    EnvGuard(const char* name, const QString& value)
        : name_(name)
    {
        qputenv(name, value.toUtf8());
    }
    ~EnvGuard() { qunsetenv(name_); }

private:
    const char* name_;
};

class UninstallExecutorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        machineGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_MACHINE_ROOT",
            dir.filePath(QStringLiteral("machine")));
        versionsGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_VERSIONS_ROOT",
            dir.filePath(QStringLiteral("versions")));
    }

    QString installVersion(const QString& buildId, const QString& version)
    {
        const QString installPath = VNH::studioVersionsRoot() +
                                    QStringLiteral("/") + version +
                                    QStringLiteral("-") + buildId;
        QDir().mkpath(installPath + QStringLiteral("/data"));

        QJsonObject manifest;
        manifest[QStringLiteral("buildId")] = buildId;
        manifest[QStringLiteral("version")] = version;
        QFile manifestFile(installPath +
                           QStringLiteral("/package-manifest.json"));
        manifestFile.open(QIODevice::WriteOnly);
        manifestFile.write(QJsonDocument(manifest).toJson());
        manifestFile.close();

        QFile payload(installPath + QStringLiteral("/data/payload.bin"));
        payload.open(QIODevice::WriteOnly);
        payload.write(QByteArrayLiteral("payload"));
        payload.close();

        InstalledVersionRecord record;
        record.buildId = buildId;
        record.semanticVersion = version;
        record.installPath = installPath;
        record.healthState = QStringLiteral("Installed");
        QString error;
        EXPECT_TRUE(InstallIndex::commitInstallation(record, 1, &error))
            << error.toStdString();
        return installPath;
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
    std::unique_ptr<EnvGuard> versionsGuard;
};

TEST_F(UninstallExecutorTest, RemovesDirectoryAndIndexRecord)
{
    const QString installPath =
        installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });

    EXPECT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_FALSE(QDir(installPath).exists());
    const auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    EXPECT_TRUE(snapshot.records.empty());
    const auto states = VNH::TransactionJournal::lastStateByTransaction();
    ASSERT_EQ(states.size(), 1);
    EXPECT_EQ(states.constBegin().value(), QStringLiteral("Uninstalled"));
}

TEST_F(UninstallExecutorTest, RejectsInvalidBuildId)
{
    const auto outcome = UninstallExecutor::run(
        QStringLiteral("../escape"), kNow,
        [](const QString&) { return QStringList(); });
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("InvalidBuildId"));
}

TEST_F(UninstallExecutorTest, RejectsUnknownBuildId)
{
    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run9.ffff9999"), kNow,
        [](const QString&) { return QStringList(); });
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("UnknownBuild"));
}

TEST_F(UninstallExecutorTest, BlocksWhenVersionIsRunning)
{
    const QString installPath =
        installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow, [&](const QString& path) {
            EXPECT_EQ(path, installPath);
            return QStringList{path + QStringLiteral("/HarkenStudio.exe")};
        });

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("VersionInUse"));
    EXPECT_TRUE(QDir(installPath).exists());
    const auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.records.size(), 1u);
}

TEST_F(UninstallExecutorTest, BlocksWhenIdentityDisagrees)
{
    const QString installPath =
        installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));

    QJsonObject manifest;
    manifest[QStringLiteral("buildId")] = QStringLiteral("run2.bbbb2222");
    QFile manifestFile(installPath + QStringLiteral("/package-manifest.json"));
    manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    manifestFile.write(QJsonDocument(manifest).toJson());
    manifestFile.close();

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("IdentityMismatch"));
    EXPECT_TRUE(QDir(installPath).exists());

    QString error;
    ASSERT_TRUE(InstallIndex::setHealthState(QStringLiteral("run1.aaaa1111"),
                                             QStringLiteral("NeedsRepair"), &error));
    const auto marked = UninstallExecutor::run(QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });
    EXPECT_FALSE(marked.ok);
    EXPECT_EQ(marked.failureCode, QStringLiteral("IdentityMismatch"));
    EXPECT_TRUE(QDir(installPath).exists());
}

TEST_F(UninstallExecutorTest, RemovesInvalidMetadataWithoutRequiringARepairMarker)
{
    for (const QByteArray& metadata : {QByteArrayLiteral("invalid-json"), QByteArrayLiteral("{}")}) {
        const QString installPath =
            installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));
        QFile manifest(installPath + QStringLiteral("/package-manifest.json"));
        ASSERT_TRUE(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(metadata);
        manifest.close();
        const auto outcome = UninstallExecutor::run(QStringLiteral("run1.aaaa1111"), kNow,
            [](const QString&) { return QStringList(); });
        EXPECT_TRUE(outcome.ok) << outcome.error.toStdString();
        EXPECT_FALSE(QDir(installPath).exists());
        EXPECT_TRUE(InstallIndex::snapshot().records.empty());
    }
}

TEST_F(UninstallExecutorTest, AllowsRemovingNeedsRepairVersionWithBrokenManifest)
{
    const QString installPath =
        installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));
    QString error;
    ASSERT_TRUE(InstallIndex::setHealthState(QStringLiteral("run1.aaaa1111"),
                                             QStringLiteral("NeedsRepair"),
                                             &error));
    ASSERT_TRUE(
        QFile::remove(installPath + QStringLiteral("/package-manifest.json")));

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });

    EXPECT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_FALSE(QDir(installPath).exists());
    const auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    EXPECT_TRUE(snapshot.records.empty());
}

TEST_F(UninstallExecutorTest, RemovesStaleRecordWhenDirectoryIsGone)
{
    const QString installPath =
        installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));
    ASSERT_TRUE(QDir(installPath).removeRecursively());

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });

    EXPECT_TRUE(outcome.ok) << outcome.error.toStdString();
    const auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    EXPECT_TRUE(snapshot.records.empty());
}

TEST_F(UninstallExecutorTest, RefusesWhileAnotherTransactionHoldsTheLock)
{
    installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));

    QLockFile lock(VNH::InstallExecutor::lockFilePath());
    ASSERT_TRUE(lock.tryLock(0));

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("InstallBusy"));
}

#ifdef Q_OS_WIN
TEST_F(UninstallExecutorTest, RestoresDirectoryWhenDeleteFails)
{
    const QString installPath =
        installVersion(QStringLiteral("run1.aaaa1111"), QStringLiteral("1.4.0"));

    QFile held(installPath + QStringLiteral("/data/payload.bin"));
    ASSERT_TRUE(held.open(QIODevice::ReadOnly));

    const auto outcome = UninstallExecutor::run(
        QStringLiteral("run1.aaaa1111"), kNow,
        [](const QString&) { return QStringList(); });
    held.close();

    EXPECT_FALSE(outcome.ok);
    EXPECT_TRUE(outcome.failureCode == QStringLiteral("CommitFailed") ||
                outcome.failureCode == QStringLiteral("DeleteFailed"))
        << outcome.failureCode.toStdString();
    if (outcome.failureCode == QStringLiteral("DeleteFailed")) {
        EXPECT_TRUE(QDir(installPath).exists());
        const auto snapshot = InstallIndex::snapshot();
        ASSERT_TRUE(snapshot.ok);
        ASSERT_EQ(snapshot.records.size(), 1u);
        EXPECT_EQ(snapshot.records.front().healthState,
                  QStringLiteral("NeedsRepair"));
    }
}
#endif

}
