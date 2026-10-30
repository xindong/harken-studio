#include "Core/FileWatcher.h"
#include "Utils/FileOperationRetry.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>

namespace {

::testing::AssertionResult replaceFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return ::testing::AssertionFailure() << "Cannot create directory for " << path.toStdString();
    auto failure = ::testing::AssertionFailure();
    const bool replaced = VNX::retryFileOperation([&] {
        QSaveFile file(path);
        const auto fail = [&](const char* operation) {
            failure = ::testing::AssertionFailure() << operation << " " << path.toStdString()
                << ": " << file.errorString().toStdString() << " (" << file.error() << ")";
            return false;
        };
        if (!file.open(QIODevice::WriteOnly)) return fail("open");
        if (file.write(bytes) != bytes.size()) return fail("write");
        if (!file.commit()) return fail("commit");
        return true;
    });
    return replaced ? ::testing::AssertionSuccess() : failure;
}

bool waitFor(const std::function<bool()>& ready)
{
    if (ready()) return true;
    QEventLoop loop;
    QTimer poll;
    QTimer deadline;
    poll.setInterval(10);
    deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready()) loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    deadline.start(5000);
    loop.exec();
    return ready();
}

void settleEvents()
{
    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();
}

class FileWatcherContract : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory_.isValid());
        changed_ = directory_.filePath(QStringLiteral("Layers/Changed.vp"));
        retained_ = directory_.filePath(QStringLiteral("Layers/Retained.vp"));
        source_ = directory_.filePath(QStringLiteral("AudioSources/Source.wav"));
        ASSERT_TRUE(replaceFile(changed_, "changed before"));
        ASSERT_TRUE(replaceFile(retained_, "retained before"));
        ASSERT_TRUE(replaceFile(source_, "source before"));
        watcher_.setDebounceInterval(50);
        QObject::connect(&watcher_, &VNX::FileWatcher::packExternallyModified, &watcher_,
                         [&](const QString& path) { projectChanges_.insert(path); });
        QObject::connect(&watcher_, &VNX::FileWatcher::filesAdded, &watcher_,
                         [&](const QStringList& paths) {
            for (const auto& path : paths) addedSources_.insert(path);
        });
        QObject::connect(&watcher_, &VNX::FileWatcher::filesRemoved, &watcher_,
                         [&](const QStringList& paths) {
            for (const auto& path : paths) removedSources_.insert(path);
        });
        QObject::connect(&watcher_, &VNX::FileWatcher::filesModified, &watcher_,
                         [&](const QStringList& paths) {
            for (const auto& path : paths) modifiedSources_.insert(path);
        });
        watcher_.startWatching(directory_.path());
    }

    void TearDown() override
    {
        watcher_.stopWatching();
    }

    QTemporaryDir directory_;
    VNX::FileWatcher watcher_;
    QString changed_;
    QString retained_;
    QString source_;
    QSet<QString> projectChanges_;
    QSet<QString> addedSources_;
    QSet<QString> removedSources_;
    QSet<QString> modifiedSources_;
};

class FileWatcherWriteContract : public FileWatcherContract,
                                public ::testing::WithParamInterface<bool>
{
protected:
    bool beginWrite(const QStringList& paths)
    {
        if (GetParam()) return watcher_.beginProjectWrite(directory_.path(), paths);
        if (!watcher_.isWatching()) return false;
        watcher_.stopWatching();
        return true;
    }

    void endWrite()
    {
        if (GetParam())
            watcher_.endProjectWrite();
        else
            watcher_.startWatching(directory_.path());
    }
};

}

TEST_P(FileWatcherWriteContract, CompletedWritesStaySilentAndImmediatelyFollowingExternalChangesAreObserved)
{
    const QString created = directory_.filePath(QStringLiteral("Layers/Created.vp"));
    ASSERT_TRUE(beginWrite({changed_, created}));
    EXPECT_FALSE(watcher_.isWatching());
    EXPECT_TRUE(watcher_.sourceFiles().isEmpty());
    ASSERT_TRUE(replaceFile(changed_, "changed by save"));
    ASSERT_TRUE(replaceFile(created, "created by save"));
    endWrite();
    EXPECT_TRUE(watcher_.isWatching());
    EXPECT_TRUE(watcher_.sourceFiles().contains(source_));
    settleEvents();
    EXPECT_TRUE(projectChanges_.isEmpty());
    EXPECT_TRUE(modifiedSources_.isEmpty());

    ASSERT_TRUE(beginWrite({changed_}));
    ASSERT_TRUE(replaceFile(changed_, "changed by another save"));
    endWrite();
    ASSERT_TRUE(replaceFile(changed_, "changed externally after save completion"));
    ASSERT_TRUE(replaceFile(retained_, "retained externally changed"));
    ASSERT_TRUE(replaceFile(created, "created externally changed"));
    ASSERT_TRUE(replaceFile(source_, "source externally changed"));
    ASSERT_TRUE(waitFor([&] {
        return projectChanges_.contains(changed_) && projectChanges_.contains(retained_)
            && projectChanges_.contains(created) && modifiedSources_.contains(source_);
    }));
}

INSTANTIATE_TEST_SUITE_P(WriteLifecycle, FileWatcherWriteContract, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "Incremental" : "OriginalRestart";
    });

TEST_F(FileWatcherContract, RollbackAndConsecutiveWritesPreserveLaterNotifications)
{
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {changed_}));
    ASSERT_TRUE(replaceFile(changed_, "temporary transaction content"));
    ASSERT_TRUE(replaceFile(changed_, "changed before"));
    watcher_.endProjectWrite();
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {changed_}));
    ASSERT_TRUE(replaceFile(changed_, "next committed content"));
    watcher_.endProjectWrite();
    settleEvents();
    EXPECT_TRUE(projectChanges_.isEmpty());

    ASSERT_TRUE(QFile::remove(changed_));
    ASSERT_TRUE(waitFor([&] { return projectChanges_.contains(changed_); }));
    projectChanges_.clear();
    ASSERT_TRUE(replaceFile(changed_, "externally recreated content"));
    ASSERT_TRUE(waitFor([&] { return projectChanges_.contains(changed_); }));
    projectChanges_.clear();
    ASSERT_TRUE(replaceFile(changed_, "externally edited after recreation"));
    ASSERT_TRUE(waitFor([&] { return projectChanges_.contains(changed_); }));
}

TEST_F(FileWatcherContract, EmptyDirectoryChangesDoNotPreventWritesOrLeaveReplacementDirectoriesUnobserved)
{
    const QString removed = directory_.filePath(QStringLiteral("Layers/Removed"));
    const QString renamed = directory_.filePath(QStringLiteral("Layers/Renamed"));
    const QString restored = directory_.filePath(QStringLiteral("Layers/Restored"));
    ASSERT_TRUE(QDir().mkpath(removed));
    ASSERT_TRUE(QDir().mkpath(restored));
    watcher_.startWatching(directory_.path());
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {}));
    ASSERT_TRUE(QDir().rename(removed, renamed));
    ASSERT_TRUE(QDir().rmdir(restored));
    ASSERT_TRUE(QDir().mkpath(restored));
    watcher_.endProjectWrite();
    settleEvents();
    EXPECT_TRUE(projectChanges_.isEmpty());

    const QString renamedFile = QDir(renamed).filePath(QStringLiteral("External.vp"));
    const QString restoredFile = QDir(restored).filePath(QStringLiteral("External.vp"));
    ASSERT_TRUE(replaceFile(renamedFile, "external in renamed folder"));
    ASSERT_TRUE(replaceFile(restoredFile, "external in restored folder"));
    ASSERT_TRUE(waitFor([&] {
        return projectChanges_.contains(renamedFile) && projectChanges_.contains(restoredFile);
    }));
}

TEST_F(FileWatcherContract, SourceCreationDeletionAndProjectClosureKeepTheirNotificationBoundaries)
{
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {changed_}));
    ASSERT_TRUE(replaceFile(changed_, "saved"));
    watcher_.endProjectWrite();
    const QString added = directory_.filePath(QStringLiteral("AudioSources/Nested/Added.wav"));
    ASSERT_TRUE(replaceFile(added, "external audio source"));
    ASSERT_TRUE(QFile::remove(source_));
    ASSERT_TRUE(waitFor([&] {
        return addedSources_.contains(added) && removedSources_.contains(source_);
    }));
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {changed_}));
    watcher_.stopWatching();
    watcher_.endProjectWrite();
    EXPECT_FALSE(watcher_.isWatching());
    EXPECT_TRUE(watcher_.sourceFiles().isEmpty());
    EXPECT_FALSE(watcher_.beginProjectWrite(directory_.path(), {changed_}));

    QTemporaryDir reopened;
    ASSERT_TRUE(reopened.isValid());
    const QString reopenedFile = reopened.filePath(QStringLiteral("Layers/Reopened.vp"));
    const QString reopenedSource = reopened.filePath(QStringLiteral("AudioSources/Reopened.wav"));
    ASSERT_TRUE(replaceFile(reopenedFile, "opened content"));
    ASSERT_TRUE(replaceFile(reopenedSource, "opened source"));
    watcher_.startWatching(reopened.path());
    projectChanges_.clear();
    modifiedSources_.clear();
    ASSERT_TRUE(replaceFile(changed_, "closed project external content"));
    ASSERT_TRUE(replaceFile(reopenedFile, "reopened project external content"));
    ASSERT_TRUE(replaceFile(reopenedSource, "reopened source external content"));
    ASSERT_TRUE(waitFor([&] {
        return projectChanges_.contains(reopenedFile) && modifiedSources_.contains(reopenedSource);
    }));
    EXPECT_FALSE(projectChanges_.contains(changed_));
    watcher_.stopWatching();
}

TEST_F(FileWatcherContract, RestoredSizeAndTimestampDoNotHideNativeExternalChangeNotifications)
{
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {changed_}));
    ASSERT_TRUE(replaceFile(changed_, "saved content"));
    watcher_.endProjectWrite();
    const QFileInfo committed(changed_);
    const auto permissions = committed.permissions();
    const auto size = committed.size();
    const auto modified = committed.lastModified();
    const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(changed_, permissions); });
    QFile external(changed_);
    ASSERT_TRUE(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(external.write("other content"), size);
    ASSERT_TRUE(external.flush());
    ASSERT_TRUE(external.setFileTime(modified, QFileDevice::FileModificationTime));
    external.close();
    ASSERT_TRUE(QFile::setPermissions(changed_, permissions
        & ~QFileDevice::WriteOwner & ~QFileDevice::WriteUser
        & ~QFileDevice::WriteGroup & ~QFileDevice::WriteOther));
    EXPECT_EQ(QFileInfo(changed_).size(), size);
    EXPECT_EQ(QFileInfo(changed_).lastModified(), modified);
    ASSERT_TRUE(waitFor([&] { return projectChanges_.contains(changed_); }));
}

TEST_F(FileWatcherContract, CaseOnlyReplacementDoesNotReportSelfWritesAndKeepsTheNewSpellingObserved)
{
    const QString renamed = directory_.filePath(QStringLiteral("Layers/changed.VP"));
    ASSERT_TRUE(watcher_.beginProjectWrite(directory_.path(), {renamed}));
    ASSERT_TRUE(QFile::remove(changed_));
    ASSERT_TRUE(replaceFile(renamed, "case renamed content"));
    watcher_.endProjectWrite();
    settleEvents();
    EXPECT_TRUE(projectChanges_.isEmpty());
    ASSERT_TRUE(replaceFile(renamed, "case renamed external content"));
    ASSERT_TRUE(waitFor([&] { return projectChanges_.contains(renamed); }));
}
