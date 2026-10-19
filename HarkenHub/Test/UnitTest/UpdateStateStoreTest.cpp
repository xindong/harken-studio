#include "Update/UpdateStateStore.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>

#include <QCoreApplication>
#include <QProcess>
#include <QTextStream>
#include <QThread>

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

TEST_F(UpdateStateStoreTest, ConcurrentProcessesPreserveIndependentProducts)
{
    QProcess studio;
    QProcess hub;
    const auto readReply = [](QProcess& process) {
        while (!process.canReadLine()) {
            if (!process.waitForReadyRead(5000))
                return QByteArray();
        }
        return process.readLine().trimmed();
    };
    for (auto* process : {&studio, &hub}) {
        process->start(QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--update-state-writer"), dir.path(),
                        process == &studio ? QStringLiteral("HarkenStudio")
                                           : QStringLiteral("HarkenHub")});
        ASSERT_EQ(readReply(*process), QByteArrayLiteral("ready"));
    }
    for (int round = 1; round <= 20; ++round) {
        const QByteArray command = QByteArray::number(round) + " " +
            QByteArray::number(QDateTime::currentMSecsSinceEpoch() + 15) + "\n";
        studio.write(command);
        hub.write(command);
        studio.waitForBytesWritten(5000);
        hub.waitForBytesWritten(5000);
        ASSERT_EQ(readReply(studio), QByteArrayLiteral("ok"));
        ASSERT_EQ(readReply(hub), QByteArrayLiteral("ok"));
        const auto studioState = UpdateStateStore::load(QStringLiteral("HarkenStudio"));
        const auto hubState = UpdateStateStore::load(QStringLiteral("HarkenHub"));
        EXPECT_EQ(studioState.releaseSequence, round);
        if (round % 2 == 0)
            EXPECT_FALSE(hubState.found);
        else
            EXPECT_EQ(hubState.releaseSequence, round);
    }
    for (auto* process : {&studio, &hub}) {
        process->write("quit\n");
        ASSERT_TRUE(process->waitForFinished(5000));
        EXPECT_EQ(process->exitCode(), 0);
    }
}

TEST_F(UpdateStateStoreTest, StaleSnapshotsCannotReplaceOrClearNewerState)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenStudio");
    record.state = QStringLiteral("Downloading");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));
    const auto stale = UpdateStateStore::load(record.product);
    record.state = QStringLiteral("AwaitingBridge");
    record.transactionId = QStringLiteral("new-transaction");
    ASSERT_EQ(UpdateStateStore::replace(stale, record, kNow),
              UpdateStateStore::WriteResult::Saved);
    const auto current = UpdateStateStore::load(record.product);
    EXPECT_EQ(UpdateStateStore::replace(stale, stale, kNow),
              UpdateStateStore::WriteResult::Conflict);
    EXPECT_EQ(UpdateStateStore::clear(stale), UpdateStateStore::WriteResult::Conflict);
    EXPECT_EQ(UpdateStateStore::load(record.product), current);
    ASSERT_EQ(UpdateStateStore::clear(current), UpdateStateStore::WriteResult::Saved);
    EXPECT_EQ(UpdateStateStore::replace(current, current, kNow),
              UpdateStateStore::WriteResult::Conflict);
    EXPECT_FALSE(UpdateStateStore::load(record.product).found);
}

TEST_F(UpdateStateStoreTest, UnreadableStateCannotBeReplacedWithAnEmptySnapshot)
{
    QFile file(UpdateStateStore::filePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray contents("{broken");
    ASSERT_EQ(file.write(contents), contents.size());
    file.close();
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenStudio");
    record.state = QStringLiteral("Downloading");
    EXPECT_FALSE(UpdateStateStore::save(record, kNow));
    EXPECT_FALSE(UpdateStateStore::clear(record.product));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), contents);
}

#if defined(Q_OS_WIN)
TEST_F(UpdateStateStoreTest, FailedReplacementPreservesAllRecords)
{
    UpdateStateRecord studio;
    studio.product = QStringLiteral("HarkenStudio");
    studio.state = QStringLiteral("Downloaded");
    ASSERT_TRUE(UpdateStateStore::save(studio, kNow));
    UpdateStateRecord hub;
    hub.product = QStringLiteral("HarkenHub");
    hub.state = QStringLiteral("AwaitingUserApproval");
    ASSERT_TRUE(UpdateStateStore::save(hub, kNow));

    QFile reader(UpdateStateStore::filePath());
    ASSERT_TRUE(reader.open(QIODevice::ReadOnly));
    const QByteArray original = reader.readAll();
    studio.state = QStringLiteral("Verified");
    EXPECT_FALSE(UpdateStateStore::save(studio, kNow));
    EXPECT_FALSE(UpdateStateStore::clear(hub.product));
    ASSERT_TRUE(reader.seek(0));
    EXPECT_EQ(reader.readAll(), original);
    EXPECT_EQ(UpdateStateStore::load(studio.product).state, "Downloaded");
    EXPECT_EQ(UpdateStateStore::load(hub.product).state, hub.state);

    reader.close();
    ASSERT_TRUE(UpdateStateStore::save(studio, kNow));
    EXPECT_EQ(UpdateStateStore::load(studio.product).state, studio.state);
    EXPECT_EQ(UpdateStateStore::load(hub.product).state, hub.state);
}
#endif

}

namespace HarkenHubTest {

int runStateWriter(const QStringList& arguments)
{
    qputenv("HARKEN_HUB_USER_ROOT", arguments.value(2).toUtf8());
    UpdateStateRecord record;
    record.product = arguments.value(3);
    record.state = QStringLiteral("Downloading");
    QTextStream input(stdin);
    QTextStream output(stdout);
    output << "ready" << Qt::endl;
    for (;;) {
        const QString line = input.readLine();
        if (line.isNull() || line == QStringLiteral("quit"))
            return 0;
        const auto parts = line.split(QLatin1Char(' '));
        record.releaseSequence = parts.value(0).toLongLong();
        const qint64 startAt = parts.value(1).toLongLong();
        while (QDateTime::currentMSecsSinceEpoch() < startAt)
            QThread::yieldCurrentThread();
        const bool ok = record.product == QStringLiteral("HarkenHub") &&
                                record.releaseSequence % 2 == 0
                            ? UpdateStateStore::clear(record.product)
                            : UpdateStateStore::save(record, QDateTime::currentDateTimeUtc());
        output << (ok ? "ok" : "failed") << Qt::endl;
    }
}

}
