#include "App/SingleInstanceGuard.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QProcess>
#include <QElapsedTimer>

#ifdef Q_OS_WIN
#include "Setup/SetupHubShutdown.h"
#endif

#include <atomic>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

using VNH::SingleInstanceGuard;

#ifdef Q_OS_WIN
namespace HarkenHubTest {

int runSetupShutdownFixture(const QStringList& arguments)
{
    SingleInstanceGuard guard(arguments.value(2));
    if (!guard.isPrimary())
        return 2;
    QObject::connect(&guard, &SingleInstanceGuard::activationRequested,
                     QCoreApplication::instance(), [&](const QStringList& received) {
        if (received == QStringList{QStringLiteral("--quit")} &&
            !arguments.contains(QStringLiteral("--ignore-quit")))
            QTimer::singleShot(150, QCoreApplication::instance(), &QCoreApplication::quit);
    });
    QTimer::singleShot(15000, QCoreApplication::instance(), &QCoreApplication::quit);
    std::fputs("ready\n", stdout);
    std::fflush(stdout);
    return QCoreApplication::exec();
}

}
#endif

namespace {

QString uniqueKey(const char* suffix)
{
    return QStringLiteral("HarkenHubTest.%1.%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QLatin1String(suffix));
}

bool waitForSignal(SingleInstanceGuard& guard, QStringList& received, int timeoutMs)
{
    QEventLoop loop;
    bool fired = false;
    QObject::connect(&guard, &SingleInstanceGuard::activationRequested, &loop,
                     [&](const QStringList& arguments) {
                         received = arguments;
                         fired = true;
                         loop.quit();
                     });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return fired;
}

}

TEST(SingleInstanceGuard, SecondInstanceIsNotPrimary)
{
    const QString key = uniqueKey("arbitration");
    SingleInstanceGuard first(key);
    ASSERT_TRUE(first.isPrimary());

    SingleInstanceGuard second(key);
    EXPECT_FALSE(second.isPrimary());
}

#ifdef Q_OS_WIN
TEST(SingleInstanceGuard, SetupShutdownWaitsForExitAndRejectsAnUnresponsiveInstance)
{
    for (const bool ignoreQuit : {false, true}) {
        const QString key = uniqueKey(ignoreQuit ? "setup-stalled" : "setup-exit");
        const std::wstring pipe = (QStringLiteral("\\\\.\\pipe\\") + key).toStdWString();
        EXPECT_TRUE(HarkenSetup::requestHubExit(pipe.c_str()));
        QProcess child;
        QStringList arguments{QStringLiteral("--setup-shutdown-fixture"), key};
        if (ignoreQuit)
            arguments.append(QStringLiteral("--ignore-quit"));
        child.start(QCoreApplication::applicationFilePath(), arguments);
        ASSERT_TRUE(child.waitForStarted(5000));
        ASSERT_TRUE(child.waitForReadyRead(5000));
        ASSERT_TRUE(child.readAllStandardOutput().contains("ready"));
        QElapsedTimer elapsed;
        elapsed.start();
        EXPECT_EQ(HarkenSetup::requestHubExit(pipe.c_str()), !ignoreQuit);
        EXPECT_LT(elapsed.elapsed(), 10000);
        if (ignoreQuit) {
            EXPECT_EQ(child.state(), QProcess::Running);
            child.kill();
            EXPECT_TRUE(child.waitForFinished(5000));
        } else {
            EXPECT_TRUE(child.waitForFinished(5000));
            EXPECT_EQ(child.exitCode(), 0);
            EXPECT_GE(elapsed.elapsed(), 100);
        }
    }
}
#endif

TEST(SingleInstanceGuard, SecondaryReportsPrimaryProcessId)
{
    const QString key = uniqueKey("primary-pid");
    SingleInstanceGuard primary(key);
    ASSERT_TRUE(primary.isPrimary());

    SingleInstanceGuard secondary(key);
    ASSERT_FALSE(secondary.isPrimary());
    EXPECT_EQ(secondary.primaryProcessId(),
              QCoreApplication::applicationPid());
}

TEST(SingleInstanceGuard, NotifyDeliversLargeArgumentIntact)
{
    const QString key = uniqueKey("framing");
    SingleInstanceGuard primary(key);
    ASSERT_TRUE(primary.isPrimary());

    SingleInstanceGuard secondary(key);
    ASSERT_FALSE(secondary.isPrimary());

    const QString largeArgument =
        QStringLiteral("C:/projects/") + QString(300000, QLatin1Char('x')) +
        QStringLiteral("/Demo.hsp");

    std::atomic<bool> notified(false);
    std::thread notifier([&]() {
        notified = secondary.notifyPrimary(
            {QStringLiteral("HarkenHub.exe"), largeArgument});
    });

    QStringList received;
    const bool fired = waitForSignal(primary, received, 5000);
    notifier.join();

    ASSERT_TRUE(fired);
    EXPECT_TRUE(notified.load());
    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(received.at(1), largeArgument);
}

TEST(SingleInstanceGuard, OversizedMessageIsDroppedWithoutActivation)
{
    const QString key = uniqueKey("oversize");
    SingleInstanceGuard primary(key);
    ASSERT_TRUE(primary.isPrimary());

    SingleInstanceGuard secondary(key);
    ASSERT_FALSE(secondary.isPrimary());

    const QString oversized(2 * 1024 * 1024, QLatin1Char('x'));
    std::thread notifier([&]() { secondary.notifyPrimary({oversized}); });

    QStringList received;
    const bool fired = waitForSignal(primary, received, 1500);
    notifier.join();

    EXPECT_FALSE(fired);
}

TEST(SingleInstanceGuard, RetryPrimaryTakesOverAfterPrimaryIsGone)
{
    const QString key = uniqueKey("takeover");

    auto first = std::make_unique<SingleInstanceGuard>(key);
    ASSERT_TRUE(first->isPrimary());

    SingleInstanceGuard second(key);
    ASSERT_FALSE(second.isPrimary());

    first.reset();
    EXPECT_FALSE(second.notifyPrimary({QStringLiteral("x")}));
    EXPECT_TRUE(second.retryPrimary());
    EXPECT_TRUE(second.isPrimary());
}

TEST(SingleInstanceGuard, DeliversBurstNotifications)
{
    const QString key = uniqueKey("burst");
    SingleInstanceGuard primary(key);
    ASSERT_TRUE(primary.isPrimary());
    SingleInstanceGuard secondary(key);
    ASSERT_FALSE(secondary.isPrimary());

    constexpr int kNotificationCount = 24;
    std::atomic<int> ready(0);
    std::atomic<bool> start(false);
    std::vector<int> results(kNotificationCount, 0);
    std::vector<std::thread> notifiers;
    notifiers.reserve(kNotificationCount);
    for (int index = 0; index < kNotificationCount; ++index) {
        notifiers.emplace_back([&, index]() {
            ++ready;
            while (!start.load())
                std::this_thread::yield();
            results[static_cast<size_t>(index)] =
                secondary.notifyPrimary({QString::number(index)})
                    ? 1
                    : 0;
        });
    }
    while (ready.load() != kNotificationCount)
        std::this_thread::yield();

    std::set<QString> received;
    QEventLoop loop;
    QObject::connect(&primary, &SingleInstanceGuard::activationRequested,
                     &loop, [&](const QStringList& arguments) {
                         if (!arguments.isEmpty())
                             received.insert(arguments.first());
                         if (received.size() == kNotificationCount)
                             loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    start = true;
    loop.exec();

    for (auto& notifier : notifiers)
        notifier.join();
    for (int result : results)
        EXPECT_EQ(result, 1);
    EXPECT_EQ(received.size(), kNotificationCount);
}
