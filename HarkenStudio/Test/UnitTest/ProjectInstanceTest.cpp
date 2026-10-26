#include "Core/ProjectInstance.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <array>
#include <functional>

namespace {

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return condition();
}

class InstanceProcess
{
public:
    ~InstanceProcess() { stop(); }

    void start(const QString& path, bool unresponsive = false)
    {
        QStringList arguments{path};
        if (unresponsive)
            arguments.append(QStringLiteral("--unresponsive"));
        process.start(QString::fromUtf8(HARKEN_PROJECT_INSTANCE_PROCESS_PATH), arguments);
    }

    bool hasStatus()
    {
        output.append(process.readAllStandardOutput());
        return output.contains('\n');
    }

    QByteArray status()
    {
        hasStatus();
        return output.left(output.indexOf('\n')).trimmed();
    }

    void stop()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }

    QProcess process;
    QByteArray output;
};

class ProjectInstanceTest : public testing::Test
{
protected:
    QTemporaryDir directory{QStringLiteral(HARKEN_TEMP_DIR "/project-instance-XXXXXX")};

    QString project(const QString& relative) const
    {
        return directory.filePath(relative);
    }
};

TEST_F(ProjectInstanceTest, SharedOwnersKeepLeaseUntilLastRelease)
{
    ASSERT_TRUE(directory.isValid());
    const QString first = project(QStringLiteral("new/project.hsp"));
    auto original = VNX::ProjectInstance::acquire(first);
    ASSERT_EQ(original.status, VNX::ProjectInstance::Status::Acquired)
        << original.error.toStdString();
    auto repeated = VNX::ProjectInstance::acquire(first);
    ASSERT_EQ(repeated.status, VNX::ProjectInstance::Status::Acquired);
    EXPECT_EQ(original.instance, repeated.instance);
    auto different = VNX::ProjectInstance::acquire(
        project(QStringLiteral("other/project.hsp")));
    ASSERT_EQ(different.status, VNX::ProjectInstance::Status::Acquired);
    EXPECT_NE(original.instance, different.instance);

    int activations = 0;
    QObject::connect(original.instance.get(), &VNX::ProjectInstance::activationRequested,
                     QCoreApplication::instance(), [&activations]() { ++activations; });
    original.instance.reset();
    InstanceProcess forwarded;
    forwarded.start(first);
    ASSERT_TRUE(waitUntil([&]() { return forwarded.hasStatus(); }));
    EXPECT_EQ(forwarded.status(), QByteArray("forwarded"));
    EXPECT_EQ(activations, 1);
    repeated.instance.reset();

    InstanceProcess next;
    next.start(first);
    ASSERT_TRUE(waitUntil([&]() { return next.hasStatus(); }));
    EXPECT_EQ(next.status(), QByteArray("acquired"));
}

TEST_F(ProjectInstanceTest, SimultaneousProcessesForwardAndRecoverAfterOwnerTermination)
{
    ASSERT_TRUE(directory.isValid());
    const QString path = project(QStringLiteral("shared/project.hsp"));
    std::array<InstanceProcess, 6> processes;
    for (auto& process : processes)
        process.start(path);
    ASSERT_TRUE(waitUntil([&]() {
        bool ready = true;
        for (auto& process : processes)
            ready = process.hasStatus() && ready;
        return ready;
    }));
    InstanceProcess* owner = nullptr;
    int owners = 0;
    int forwarded = 0;
    for (auto& process : processes) {
        const QByteArray status = process.status();
        if (status == "acquired") {
            ++owners;
            owner = &process;
        } else if (status == "forwarded") {
            ++forwarded;
        } else {
            ADD_FAILURE() << status.toStdString();
        }
    }
    ASSERT_EQ(owners, 1);
    EXPECT_EQ(forwarded, 5);
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(waitUntil([&]() {
        owner->hasStatus();
        return owner->output.count("activated") == 5;
    }));

    InstanceProcess independent;
    independent.start(project(QStringLiteral("independent/project.hsp")));
    ASSERT_TRUE(waitUntil([&]() { return independent.hasStatus(); }));
    EXPECT_EQ(independent.status(), QByteArray("acquired"));

    owner->stop();
    InstanceProcess recovered;
    recovered.start(path);
    ASSERT_TRUE(waitUntil([&]() { return recovered.hasStatus(); }));
    EXPECT_EQ(recovered.status(), QByteArray("acquired"));
}

TEST_F(ProjectInstanceTest, UnresponsiveOwnerDoesNotAllowAnotherOwner)
{
    ASSERT_TRUE(directory.isValid());
    const QString path = project(QStringLiteral("project.hsp"));
    InstanceProcess owner;
    owner.start(path, true);
    ASSERT_TRUE(waitUntil([&]() { return owner.hasStatus(); }));
    ASSERT_EQ(owner.status(), QByteArray("acquired"));

    InstanceProcess contender;
    contender.start(path);
    ASSERT_TRUE(waitUntil([&]() { return contender.hasStatus(); }));
    EXPECT_TRUE(contender.status().startsWith("failed:"))
        << contender.status().toStdString();
    EXPECT_NE(owner.process.state(), QProcess::NotRunning);
    owner.stop();

    InstanceProcess recovered;
    recovered.start(path);
    ASSERT_TRUE(waitUntil([&]() { return recovered.hasStatus(); }));
    EXPECT_EQ(recovered.status(), QByteArray("acquired"));
}

}
