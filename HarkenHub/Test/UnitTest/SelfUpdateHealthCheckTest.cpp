#include "Update/SelfUpdateHealthCheck.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>

#ifdef Q_OS_WIN
#include "Platform/UserSession.h"
#include "Platform/Windows/UserSessionReady.h"
#include <QDir>
#include <QScopeGuard>
#include <aclapi.h>
#include <thread>
#endif

using VNH::SelfUpdateHealthCheck;

namespace {

std::function<bool(const QString&)> reporterWriting(QLocalSocket* socket,
                                                    const QByteArray& payload)
{
    return [socket, payload](const QString& channelName) {
        QObject::connect(socket, &QLocalSocket::connected,
                         [socket, payload]() {
                             socket->write(payload);
                             socket->flush();
                             socket->disconnectFromServer();
                         });
        socket->connectToServer(channelName);
        return true;
    };
}

QByteArray reportJson(const QString& product, bool healthy)
{
    QJsonObject report;
    report[QStringLiteral("product")] = product;
    report[QStringLiteral("healthy")] = healthy;
    report[QStringLiteral("version")] = QStringLiteral("9.9.9");
    report[QStringLiteral("buildId")] = QStringLiteral("run9.abcdef12");
    return QJsonDocument(report).toJson(QJsonDocument::Compact) +
           QByteArrayLiteral("\n");
}

}

TEST(SelfUpdateHealthCheckTest, AcceptsHealthyReport)
{
    QLocalSocket socket;
    const auto report = SelfUpdateHealthCheck::awaitReport(
        SelfUpdateHealthCheck::newChannelName(),
        reporterWriting(&socket,
                        reportJson(QStringLiteral("HarkenHub"), true)),
        5000);
    EXPECT_TRUE(report.ok) << report.error.toStdString();
    EXPECT_EQ(report.version, QStringLiteral("9.9.9"));
    EXPECT_EQ(report.buildId, QStringLiteral("run9.abcdef12"));
}

TEST(SelfUpdateHealthCheckTest, RejectsUnhealthyReport)
{
    QLocalSocket socket;
    const auto report = SelfUpdateHealthCheck::awaitReport(
        SelfUpdateHealthCheck::newChannelName(),
        reporterWriting(&socket,
                        reportJson(QStringLiteral("HarkenHub"), false)),
        5000);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(report.error.contains(QStringLiteral("unhealthy")))
        << report.error.toStdString();
}

TEST(SelfUpdateHealthCheckTest, RejectsForeignProduct)
{
    QLocalSocket socket;
    const auto report = SelfUpdateHealthCheck::awaitReport(
        SelfUpdateHealthCheck::newChannelName(),
        reporterWriting(&socket,
                        reportJson(QStringLiteral("HarkenStudio"), true)),
        5000);
    EXPECT_FALSE(report.ok);
}

TEST(SelfUpdateHealthCheckTest, RejectsMalformedReport)
{
    QLocalSocket socket;
    const auto report = SelfUpdateHealthCheck::awaitReport(
        SelfUpdateHealthCheck::newChannelName(),
        reporterWriting(&socket, QByteArrayLiteral("not-json\n")), 5000);
    EXPECT_FALSE(report.ok);
}

TEST(SelfUpdateHealthCheckTest, TimesOutWithoutReporter)
{
    const auto report = SelfUpdateHealthCheck::awaitReport(
        SelfUpdateHealthCheck::newChannelName(),
        [](const QString&) { return true; }, 200);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(report.error.contains(QStringLiteral("Timed out")))
        << report.error.toStdString();
}

TEST(SelfUpdateHealthCheckTest, FailsWhenReporterCannotStart)
{
    const auto report = SelfUpdateHealthCheck::awaitReport(
        SelfUpdateHealthCheck::newChannelName(),
        [](const QString&) { return false; }, 5000);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(report.error.contains(QStringLiteral("Cannot start")))
        << report.error.toStdString();
}

TEST(SelfUpdateHealthCheckTest, ChannelNamesAreUnique)
{
    EXPECT_NE(SelfUpdateHealthCheck::newChannelName(),
              SelfUpdateHealthCheck::newChannelName());
}

#ifdef Q_OS_WIN

TEST(UserSessionReadyTest, RequiresConfirmationFromTheStartedHub)
{
    VNH::UserSessionReady ready;
    QString error;
    ASSERT_TRUE(ready.create(VNH::currentUserId(), &error)) << error.toStdString();
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    ASSERT_NE(process, nullptr);
    const auto cleanup = qScopeGuard([process]() { CloseHandle(process); });
    EXPECT_FALSE(ready.wait(process, 10, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("Timed out"))) << error.toStdString();
    ASSERT_TRUE(VNH::UserSessionReady::signal(ready.name(), &error)) << error.toStdString();
    EXPECT_TRUE(ready.wait(process, 1000, &error)) << error.toStdString();
}

TEST(UserSessionReadyTest, RejectsInvalidUserAndForeignEventNames)
{
    VNH::UserSessionReady ready;
    QString error;
    EXPECT_FALSE(ready.create(QStringLiteral("not-a-sid"), &error));
    EXPECT_FALSE(VNH::UserSessionReady::signal(QStringLiteral("Global\\OtherEvent"), &error));
}

TEST(UserSessionReadyTest, EachRestartRequiresItsOwnConfirmation)
{
    VNH::UserSessionReady first;
    VNH::UserSessionReady second;
    QString error;
    ASSERT_TRUE(first.create(VNH::currentUserId(), &error));
    ASSERT_TRUE(second.create(VNH::currentUserId(), &error));
    EXPECT_NE(first.name(), second.name());
    ASSERT_TRUE(VNH::UserSessionReady::signal(first.name(), &error));
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    ASSERT_NE(process, nullptr);
    const auto cleanup = qScopeGuard([process]() { CloseHandle(process); });
    EXPECT_FALSE(second.wait(process, 10, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("Timed out"))) << error.toStdString();
}

TEST(UserSessionReadyTest, AllowsMediumIntegrityConfirmationFromTheUserSession)
{
    VNH::UserSessionReady ready;
    QString error;
    ASSERT_TRUE(ready.create(VNH::currentUserId(), &error)) << error.toStdString();
    const HANDLE event = OpenEventW(READ_CONTROL, FALSE, ready.name().toStdWString().c_str());
    ASSERT_NE(event, nullptr);
    const auto closeEvent = qScopeGuard([event]() { CloseHandle(event); });
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_EQ(GetSecurityInfo(event, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                              nullptr, nullptr, nullptr, &sacl, &descriptor), ERROR_SUCCESS);
    const auto freeDescriptor = qScopeGuard([descriptor]() { LocalFree(descriptor); });
    ASSERT_NE(sacl, nullptr);
    ASSERT_EQ(sacl->AceCount, 1);
    void* ace = nullptr;
    ASSERT_TRUE(GetAce(sacl, 0, &ace));
    const auto* label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
    ASSERT_EQ(label->Header.AceType, SYSTEM_MANDATORY_LABEL_ACE_TYPE);
    EXPECT_EQ(label->Mask, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
    PSID sid = const_cast<DWORD*>(&label->SidStart);
    EXPECT_EQ(*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1),
              SECURITY_MANDATORY_MEDIUM_RID);
}

namespace {

void verifyExitedHub(int exitCode, bool primaryConfirms)
{
    VNH::UserSessionReady ready;
    QString error;
    ASSERT_TRUE(ready.create(VNH::currentUserId(), &error)) << error.toStdString();
    std::wstring command = QStringLiteral("\"%1\" /d /c exit %2")
                               .arg(QDir::toNativeSeparators(qEnvironmentVariable("ComSpec")))
                               .arg(exitCode).toStdWString();
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    ASSERT_TRUE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    const auto cleanup = qScopeGuard([&]() {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    });
    ASSERT_EQ(WaitForSingleObject(process.hProcess, 5000), WAIT_OBJECT_0);
    if (primaryConfirms) {
        bool signaled = false;
        std::thread primary([&]() {
            Sleep(30);
            QString signalError;
            signaled = VNH::UserSessionReady::signal(ready.name(), &signalError);
        });
        const bool confirmed = ready.wait(process.hProcess, 5000, &error);
        primary.join();
        EXPECT_TRUE(signaled);
        EXPECT_TRUE(confirmed) << error.toStdString();
    } else {
        EXPECT_FALSE(ready.wait(process.hProcess, 20, &error));
        EXPECT_TRUE(error.contains(exitCode == 0 ? QStringLiteral("Timed out")
                                                 : QStringLiteral("exit 7")))
            << error.toStdString();
    }
}

}

TEST(UserSessionReadyTest, ReportsEarlyProcessFailure)
{
    verifyExitedHub(7, false);
}

TEST(UserSessionReadyTest, CleanProcessExitDoesNotCountAsReady)
{
    verifyExitedHub(0, false);
}

TEST(UserSessionReadyTest, AcceptsPrimaryConfirmationAfterActivationProxyExits)
{
    verifyExitedHub(0, true);
}

#endif
