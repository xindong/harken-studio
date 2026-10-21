#include "UI/VersionsPage.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectStore.h"
#include "Support/StudioProtocolTestUtils.h"
#include "Transactions/UninstallExecutor.h"
#include "Transactions/InstallExecutor.h"
#include "Update/UpdateStateStore.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QLockFile>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QScopeGuard>
#include <QThreadPool>

#include <gtest/gtest.h>

using VNH::VersionsPage;

TEST(VersionsPageWatchTest, ReturnsRootWhenItExists)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    EXPECT_EQ(VersionsPage::watchDirectoryFor(temp.path()),
              QDir(temp.path()).absolutePath());
}

TEST(VersionsPageWatchTest, FallsBackToNearestExistingAncestor)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString missing =
        temp.path() + QStringLiteral("/HarkenStudio/Versions");
    EXPECT_EQ(VersionsPage::watchDirectoryFor(missing),
              QDir(temp.path()).absolutePath());
}

TEST(VersionsPageWatchTest, PromotesToRootAfterCreation)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.path() + QStringLiteral("/Versions");
    EXPECT_EQ(VersionsPage::watchDirectoryFor(root),
              QDir(temp.path()).absolutePath());
    ASSERT_TRUE(QDir().mkpath(root));
    EXPECT_EQ(VersionsPage::watchDirectoryFor(root),
              QDir(root).absolutePath());
}

namespace {

VNH::InstalledVersion version(const QString& buildId)
{
    VNH::InstalledVersion result;
    result.buildId = buildId;
    result.semanticVersion = buildId;
    return result;
}

}

TEST(VersionsPageSortTest, SortsNewestSemanticVersionFirst)
{
    std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("0.1.6-dev")),
        version(QStringLiteral("0.1.10-dev")),
        version(QStringLiteral("0.1.8-dev")),
        version(QStringLiteral("0.1.9-dev"))};

    VersionsPage::sortForDisplay(versions);

    ASSERT_EQ(versions.size(), 4u);
    EXPECT_EQ(versions[0].semanticVersion, QStringLiteral("0.1.10-dev"));
    EXPECT_EQ(versions[1].semanticVersion, QStringLiteral("0.1.9-dev"));
    EXPECT_EQ(versions[2].semanticVersion, QStringLiteral("0.1.8-dev"));
    EXPECT_EQ(versions[3].semanticVersion, QStringLiteral("0.1.6-dev"));
}

TEST(VersionsPageSortTest, UsesBuildIdForEqualVersions)
{
    auto first = version(QStringLiteral("build-a"));
    first.semanticVersion = QStringLiteral("1.2.3-dev");
    auto second = version(QStringLiteral("build-b"));
    second.semanticVersion = QStringLiteral("1.2.3-dev");
    std::vector<VNH::InstalledVersion> versions = {first, second};

    VersionsPage::sortForDisplay(versions);

    ASSERT_EQ(versions.size(), 2u);
    EXPECT_EQ(versions[0].buildId, QStringLiteral("build-b"));
    EXPECT_EQ(versions[1].buildId, QStringLiteral("build-a"));
}

namespace {

class VersionsPageIssuesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        qputenv("HARKEN_HUB_USER_ROOT", temp.filePath("user").toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT", temp.filePath("machine").toUtf8());
        qputenv("HARKEN_HUB_VERSIONS_ROOT", temp.filePath("versions").toUtf8());
        ASSERT_TRUE(store.open(temp.filePath("projects.db")));
    }
    void TearDown() override
    {
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_VERSIONS_ROOT");
    }
    void write(const QString& path, const QByteArray& bytes)
    {
        ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }
    VNH::InstalledVersionRecord install(const QString& id, const QString& health = "Installed")
    {
        VNH::InstalledVersionRecord record;
        record.buildId = id;
        record.semanticVersion = "0.1.20-dev";
        record.installPath = QDir(VNH::studioVersionsRoot()).filePath(id);
        record.healthState = health;
        write(QDir(record.installPath).filePath(VNH::studioBinaryRelativePath()), "stub");
        write(record.installPath + "/package-manifest.json", QJsonDocument(QJsonObject{
            {"product", "HarkenStudio"}, {"buildId", id}, {"version", record.semanticVersion}}).toJson());
        auto capabilities = HarkenHubTest::studioCapabilities(id, record.semanticVersion);
        capabilities["capabilitiesVersion"] = 1;
        write(record.installPath + "/capabilities.json", QJsonDocument(capabilities).toJson());
        QString error;
        EXPECT_TRUE(VNH::InstallIndex::commitInstallation(record, 1, &error));
        return record;
    }
    QTemporaryDir temp;
    VNH::ProjectStore store;
};

TEST_F(VersionsPageIssuesTest, LegacyVersionIsVisibleAndDoesNotNeedRepair)
{
    install("legacy-build");
    VersionsPage page(&store);
    page.refresh();
    ASSERT_EQ(page.installedVersions().size(), 1u);
    EXPECT_EQ(page.installedVersions().front().capabilitiesVersion, 1);
    EXPECT_TRUE(page.findChild<QFrame*>("installationIssueBanner")->isHidden());
}

TEST_F(VersionsPageIssuesTest, AsyncInstallationOwnsAllCacheInputsAfterViewCloses)
{
    VNH::UpdateStateRecord record;
    record.product = QStringLiteral("HarkenStudio");
    record.state = QStringLiteral("AwaitingUserInstall");
    record.buildId = QStringLiteral("build");
    record.downloadPath = VNH::userDataDir() + QStringLiteral("/downloads/build.zip");
    record.manifestEnvelopePath = VNH::userDataDir() + QStringLiteral("/downloads/manual/build.manifest");
    write(record.downloadPath, "package");
    write(record.manifestEnvelopePath, "manifest");
    ASSERT_TRUE(VNH::UpdateStateStore::save(record, QDateTime::currentDateTimeUtc()));

    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int maxThreads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started;
    QSemaphore release;
    pool->start([&]() { started.release(); release.acquire(); });
    const auto guard = qScopeGuard([&]() {
        release.release();
        pool->waitForDone();
        pool->setMaxThreadCount(maxThreads);
    });
    ASSERT_TRUE(started.tryAcquire(1, 5000));
    {
        VersionsPage page(&store);
        page.refresh();
        QPushButton* installButton = nullptr;
        for (auto* button : page.findChildren<QPushButton*>())
            if (button->text() == QCoreApplication::translate("HarkenHub", "Install"))
                installButton = button;
        ASSERT_NE(installButton, nullptr);
        ASSERT_TRUE(installButton->isEnabled());
        installButton->click();
        EXPECT_FALSE(VNH::DownloadLease::acquire(record.downloadPath));
        EXPECT_FALSE(VNH::DownloadLease::acquire(record.manifestEnvelopePath));
    }
    EXPECT_FALSE(VNH::DownloadLease::acquire(record.downloadPath));
    EXPECT_FALSE(VNH::DownloadLease::acquire(record.manifestEnvelopePath));
    release.release();
    ASSERT_TRUE(pool->waitForDone(5000));
    EXPECT_TRUE(VNH::DownloadLease::acquire(record.downloadPath));
    EXPECT_TRUE(VNH::DownloadLease::acquire(record.manifestEnvelopePath));
}

TEST_F(VersionsPageIssuesTest, NewerProtocolRemainsInstalledWithSpecificHubLimitations)
{
    const auto record = install("newer-protocol");
    auto capabilities = HarkenHubTest::studioCapabilities(record.buildId, record.semanticVersion);
    capabilities["capabilitiesVersion"] = 3;
    write(record.installPath + "/capabilities.json", QJsonDocument(capabilities).toJson());
    VersionsPage page(&store);
    page.refresh();
    ASSERT_EQ(page.installedVersions().size(), 1u);
    EXPECT_TRUE(page.installedVersions().front().available);
    EXPECT_TRUE(page.findChild<QFrame*>("installationIssueBanner")->isHidden());
    auto* table = page.findChild<QTableWidget*>();
    ASSERT_NE(table, nullptr);
    ASSERT_NE(table->item(0, 0), nullptr);
}

TEST_F(VersionsPageIssuesTest, MultipleIssuesExposeTheSelectedCauseAndPreserveSelection)
{
    const auto first = install("missing-executable");
    const auto second = install("missing-capabilities");
    ASSERT_TRUE(QFile::remove(QDir(first.installPath).filePath(VNH::studioBinaryRelativePath())));
    ASSERT_TRUE(QFile::remove(second.installPath + "/capabilities.json"));
    VersionsPage page(&store);
    page.refresh();
    auto* selector = page.findChild<QComboBox*>("installationIssueSelector");
    ASSERT_EQ(selector->count(), 2);
    selector->setCurrentIndex(1);
    EXPECT_FALSE(page.findChild<QPushButton*>("installationRepairButton")->isEnabled());
    page.refresh();
    EXPECT_EQ(selector->currentData().toString(), second.buildId);
}

TEST_F(VersionsPageIssuesTest, ActiveInstallDoesNotAppearAsDamagedButInterruptedInstallDoes)
{
    install("pending", "Installing");
    QLockFile lock(VNH::InstallExecutor::lockFilePath());
    ASSERT_TRUE(lock.tryLock());
    VersionsPage page(&store);
    page.refresh();
    EXPECT_TRUE(page.installedVersions().empty());
    EXPECT_TRUE(page.findChild<QFrame*>("installationIssueBanner")->isHidden());
    lock.unlock();
    page.refresh();
    EXPECT_FALSE(page.findChild<QFrame*>("installationIssueBanner")->isHidden());
    EXPECT_TRUE(page.findChild<QPushButton*>("installationRepairButton")->isEnabled());
}

TEST_F(VersionsPageIssuesTest, RepairMarkerCanBeCheckedWithoutACachedPackage)
{
    install("intact-legacy", "NeedsRepair");
    VersionsPage page(&store);
    page.refresh();
    EXPECT_TRUE(page.findChild<QPushButton*>("installationRepairButton")->isEnabled());
}

TEST_F(VersionsPageIssuesTest, IdentityMismatchDisablesFileOperationsEvenWhenTheExecutableIsMissing)
{
    const auto record = install("registered-build");
    write(record.installPath + "/package-manifest.json", QJsonDocument(QJsonObject{
        {"product", "HarkenStudio"}, {"buildId", "other-build"}, {"version", record.semanticVersion}}).toJson());
    ASSERT_TRUE(QFile::remove(QDir(record.installPath).filePath(VNH::studioBinaryRelativePath())));
    VersionsPage page(&store);
    page.refresh();
    EXPECT_FALSE(page.findChild<QPushButton*>("installationRepairButton")->isEnabled());
    EXPECT_FALSE(page.findChild<QPushButton*>("installationUninstallButton")->isEnabled());
}

TEST_F(VersionsPageIssuesTest, CapabilityIdentityMismatchPreventsFileOperationsAndUninstall)
{
    for (const auto* field : {"buildId", "studioVersion"}) {
        for (const auto* health : {"Installed", "NeedsRepair"}) {
            const auto record = install("registered-build", health);
            auto capabilities = HarkenHubTest::studioCapabilities(record.buildId, record.semanticVersion);
            capabilities[field] = "other-identity";
            write(record.installPath + "/capabilities.json", QJsonDocument(capabilities).toJson());
            ASSERT_TRUE(QFile::remove(QDir(record.installPath).filePath(VNH::studioBinaryRelativePath())));
            VersionsPage page(&store);
            page.refresh();
            EXPECT_FALSE(page.findChild<QPushButton*>("installationRepairButton")->isEnabled());
            EXPECT_FALSE(page.findChild<QPushButton*>("installationUninstallButton")->isEnabled());
            const auto outcome = VNH::UninstallExecutor::run(record.buildId, QDateTime::currentDateTimeUtc(),
                [](const QString&) { return QStringList(); });
            EXPECT_FALSE(outcome.ok);
            EXPECT_EQ(outcome.failureCode, QStringLiteral("IdentityMismatch"));
            EXPECT_TRUE(QDir(record.installPath).exists());
            EXPECT_EQ(VNH::InstallIndex::snapshot().records.size(), 1u);
        }
    }
}

TEST_F(VersionsPageIssuesTest, CorruptIndexShowsAnErrorAndNoVersionActions)
{
    write(VNH::InstallIndex::filePath(), "invalid-json");
    VersionsPage page(&store);
    page.refresh();
    EXPECT_FALSE(page.findChild<QFrame*>("installationIssueBanner")->isHidden());
    EXPECT_TRUE(page.findChild<QPushButton*>("installationRepairButton")->isHidden());
}

}
