#include "Maintenance/CacheManager.h"
#include "Download/DownloadLease.h"
#include "Maintenance/LogExporter.h"
#include "Transactions/TransactionDiagnostics.h"

#include "Packaging/PackageReader.h"
#include "Transactions/TransactionJournal.h"

#include <gtest/gtest.h>
#include <QJsonObject>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using VNH::CacheCleanOptions;
using VNH::CacheManager;
using VNH::LogExporter;
using VNH::PackageReader;
using VNH::TransactionDiagnostics;
using VNH::TransactionJournal;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

void writeFileAged(const QString& path, const QByteArray& content,
                   const QDateTime& modifiedAt)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
    file.close();
    QFile reopen(path);
    ASSERT_TRUE(reopen.open(QIODevice::ReadWrite));
    ASSERT_TRUE(reopen.setFileTime(modifiedAt, QFileDevice::FileModificationTime));
}

class MaintenanceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT",
                dir.filePath(QStringLiteral("machine")).toUtf8());
    }
    void TearDown() override
    {
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
    }

    QTemporaryDir dir;
};

TEST_F(MaintenanceTest, CacheCleanRemovesOnlyExpiredArtifacts)
{
    const QString downloads = dir.filePath(QStringLiteral("downloads"));
    const QString quarantine = dir.filePath(QStringLiteral("quarantine"));
    const QString logs = dir.filePath(QStringLiteral("logs"));

    writeFileAged(downloads + QStringLiteral("/old.zip.part"),
                  QByteArrayLiteral("x"), kNow.addDays(-9));
    writeFileAged(downloads + QStringLiteral("/old.zip.part.meta"),
                  QByteArrayLiteral("x"), kNow.addDays(-9));
    writeFileAged(downloads + QStringLiteral("/fresh.zip.part"),
                  QByteArrayLiteral("x"), kNow.addDays(-1));
    writeFileAged(downloads + QStringLiteral("/old.zip"),
                  QByteArrayLiteral("x"), kNow.addDays(-20));
    writeFileAged(downloads + QStringLiteral("/fresh.zip"),
                  QByteArrayLiteral("x"), kNow.addDays(-2));
    writeFileAged(quarantine + QStringLiteral("/bad.zip"),
                  QByteArrayLiteral("x"), kNow.addDays(-30));
    writeFileAged(logs + QStringLiteral("/ancient.log"),
                  QByteArrayLiteral("x"), kNow.addDays(-45));
    writeFileAged(logs + QStringLiteral("/current.log"),
                  QByteArrayLiteral("x"), kNow.addDays(-3));

    const auto report =
        CacheManager::clean(dir.path(), CacheCleanOptions{}, kNow);

    EXPECT_TRUE(report.failedFiles.isEmpty());
    EXPECT_FALSE(QFile::exists(downloads + QStringLiteral("/old.zip.part")));
    EXPECT_FALSE(
        QFile::exists(downloads + QStringLiteral("/old.zip.part.meta")));
    EXPECT_TRUE(QFile::exists(downloads + QStringLiteral("/fresh.zip.part")));
    EXPECT_FALSE(QFile::exists(downloads + QStringLiteral("/old.zip")));
    EXPECT_TRUE(QFile::exists(downloads + QStringLiteral("/fresh.zip")));
    EXPECT_FALSE(QFile::exists(quarantine + QStringLiteral("/bad.zip")));
    EXPECT_FALSE(QFile::exists(logs + QStringLiteral("/ancient.log")));
    EXPECT_TRUE(QFile::exists(logs + QStringLiteral("/current.log")));
    EXPECT_EQ(report.removedFiles.size(), 5);
}

TEST_F(MaintenanceTest, CacheCleanPreservesFilesReferencedByUpdates)
{
    CacheCleanOptions options;
    options.protectedFiles = {
        dir.filePath("downloads/pending.zip"),
        dir.filePath("downloads/pending.manifest"),
        dir.filePath("downloads/active.zip.part"),
        dir.filePath("downloads/active.zip.part.meta"),
        dir.filePath("hub-updates/pending.exe"),
        dir.filePath("hub-updates/pending.manifest")};
    for (const auto& path : options.protectedFiles)
        writeFileAged(path, QByteArrayLiteral("pending"), kNow.addDays(-30));
    const QString unused = dir.filePath("downloads/unused.zip");
    writeFileAged(unused, QByteArrayLiteral("unused"), kNow.addDays(-30));

    const auto report = CacheManager::clean(dir.path(), options, kNow);

    EXPECT_TRUE(report.failedFiles.isEmpty());
    EXPECT_EQ(report.removedFiles, QStringList{unused});
    for (const auto& path : options.protectedFiles) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        EXPECT_EQ(file.readAll(), QByteArrayLiteral("pending"));
    }
}

TEST_F(MaintenanceTest, ActiveArtifactsAreProtectedThroughConsumerHandoff)
{
    const auto package = dir.filePath(QStringLiteral("downloads/manual/build.zip"));
    const auto installer = dir.filePath(QStringLiteral("hub-updates/HarkenHubSetup-run.123.pkg"));
    const QStringList paths = {installer, installer + QStringLiteral(".part"),
                              installer + QStringLiteral(".part.meta"),
                              installer + QStringLiteral(".stale"),
                              dir.filePath(QStringLiteral("hub-updates/run.123.manifest")),
                              package, package + QStringLiteral(".part"),
                              package + QStringLiteral(".part.meta"),
                              package + QStringLiteral(".stale"),
                              dir.filePath(QStringLiteral("downloads/manual/build.manifest"))};
    for (const auto& path : paths)
        writeFileAged(path, QByteArrayLiteral("active"), kNow.addDays(-30));
    auto lease = VNH::DownloadLease::acquire(package);
    ASSERT_TRUE(lease);
    auto installerLease = VNH::DownloadLease::acquire(installer);
    ASSERT_TRUE(installerLease);
    const auto protectedReport = CacheManager::clean(dir.path(), {}, kNow);
    EXPECT_TRUE(protectedReport.removedFiles.isEmpty());
    for (const auto& path : paths)
        EXPECT_TRUE(QFile::exists(path));
    lease.reset();
    installerLease.reset();
    const auto expiredReport = CacheManager::clean(dir.path(), {}, kNow);
    EXPECT_EQ(expiredReport.removedFiles.size(), paths.size());
    for (const auto& path : paths)
        EXPECT_FALSE(QFile::exists(path));
}

TEST_F(MaintenanceTest, LogExporterBundlesDiagnostics)
{
    TransactionJournal::append(QStringLiteral("t1"),
                               QStringLiteral("HarkenStudio"),
                               QStringLiteral("run1.aaaa1111"),
                               QStringLiteral("Installed"), QString(), kNow);
    writeFileAged(dir.filePath(QStringLiteral("logs/hub.log")),
                  QByteArrayLiteral("log-line"), kNow);

    const QString zipPath = dir.filePath(QStringLiteral("export/diag.zip"));
    QString error;
    ASSERT_TRUE(LogExporter::exportDiagnostics(zipPath, &error))
        << error.toStdString();

    const auto listing = PackageReader::listEntries(zipPath);
    ASSERT_TRUE(listing.ok) << listing.error.toStdString();
    QStringList names;
    for (const auto& entry : listing.entries)
        names.append(entry.path);
    EXPECT_TRUE(names.contains(QStringLiteral("machine/transactions.log")));
    EXPECT_TRUE(names.contains(QStringLiteral("user/logs/hub.log")));
}

TEST_F(MaintenanceTest, LogExporterFailsWithNothingToExport)
{
    const QString zipPath = dir.filePath(QStringLiteral("export/diag.zip"));
    QString error;
    EXPECT_FALSE(LogExporter::exportDiagnostics(zipPath, &error));
}

TEST_F(MaintenanceTest, TransactionDiagnosticsSummarizesJournal)
{
    TransactionJournal::append(QStringLiteral("t1"),
                               QStringLiteral("HarkenStudio"),
                               QStringLiteral("run1.aaaa1111"),
                               QStringLiteral("Verifying"), QString(), kNow);
    TransactionJournal::append(
        QStringLiteral("t1"), QStringLiteral("HarkenStudio"),
        QStringLiteral("run1.aaaa1111"), QStringLiteral("Installed"),
        QString(), kNow.addSecs(30));
    TransactionJournal::append(
        QStringLiteral("t2"), QStringLiteral("HarkenStudio"),
        QStringLiteral("run2.bbbb2222"), QStringLiteral("Failed"),
        QStringLiteral("PackageRejected"), kNow.addSecs(60),
        QStringLiteral("Package SHA-256 mismatch"));

    const auto summaries = TransactionDiagnostics::summarize();
    ASSERT_EQ(summaries.size(), 2u);
    EXPECT_EQ(summaries[0].transactionId, QStringLiteral("t2"));
    EXPECT_EQ(summaries[0].lastState, QStringLiteral("Failed"));
    EXPECT_EQ(summaries[0].failureCode, QStringLiteral("PackageRejected"));
    EXPECT_EQ(summaries[0].failureDetail, QStringLiteral("Package SHA-256 mismatch"));
    EXPECT_EQ(summaries[1].transactionId, QStringLiteral("t1"));
    EXPECT_EQ(summaries[1].lastState, QStringLiteral("Installed"));
    EXPECT_EQ(summaries[1].entryCount, 2);

    const auto json = TransactionDiagnostics::toJson(summaries);
    EXPECT_EQ(json.size(), 2);
    EXPECT_EQ(json[0].toObject().value(QStringLiteral("failureDetail")).toString(),
              QStringLiteral("Package SHA-256 mismatch"));
    EXPECT_TRUE(TransactionDiagnostics::failureMessage(QStringLiteral("t1")).isEmpty());
    EXPECT_TRUE(TransactionDiagnostics::failureMessage(QStringLiteral("unknown")).isEmpty());
    EXPECT_TRUE(TransactionDiagnostics::failureMessage(QStringLiteral("t2"))
                    .contains(QStringLiteral("Package SHA-256 mismatch")));
}

TEST_F(MaintenanceTest, TransactionDiagnosticsClearsFailuresOnRetry)
{
    const QString transactionId = QStringLiteral("t1");
    ASSERT_TRUE(TransactionJournal::append(
        transactionId, QStringLiteral("HarkenStudio"), {},
        QStringLiteral("Failed"), QStringLiteral("InstallBusy"), kNow,
        QStringLiteral("Another install transaction is running")));
    for (const auto& state : {QStringLiteral("Verifying"), QStringLiteral("Installed"),
                              QStringLiteral("Reclaimed")}) {
        ASSERT_TRUE(TransactionJournal::append(
            transactionId, QStringLiteral("HarkenStudio"), {}, state, {}, kNow));
        const auto summaries = TransactionDiagnostics::summarize();
        ASSERT_EQ(summaries.size(), 1u);
        EXPECT_EQ(summaries.front().lastState, state);
        EXPECT_TRUE(summaries.front().failureCode.isEmpty());
        EXPECT_TRUE(summaries.front().failureDetail.isEmpty());
        EXPECT_TRUE(TransactionDiagnostics::failureMessage(transactionId).isEmpty());
        const auto json = TransactionDiagnostics::toJson(summaries);
        EXPECT_FALSE(json[0].toObject().contains(QStringLiteral("failureCode")));
        EXPECT_FALSE(json[0].toObject().contains(QStringLiteral("failureDetail")));
    }
}

}
