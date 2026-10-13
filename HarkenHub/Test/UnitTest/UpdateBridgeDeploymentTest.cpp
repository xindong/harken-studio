#include "Platform/UpdateBridgeDeployment.h"

#include "Platform/AclHardening.h"
#include "Platform/ProcessScan.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <string>

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(data), data.size());
}

void makePayload(const QString& sourceDir)
{
    writeFile(sourceDir + QStringLiteral("/HarkenHubUpdateBridge.exe"),
              QByteArrayLiteral("bridge"));
    writeFile(sourceDir + QStringLiteral("/Qt6Core.dll"),
              QByteArrayLiteral("core"));
    writeFile(sourceDir + QStringLiteral("/tls/qschannelbackend.dll"),
              QByteArrayLiteral("tls"));
}

QString permissiveSddl()
{
    return QStringLiteral(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;BU)");
}

bool aceSidIs(ACCESS_ALLOWED_ACE* ace, WELL_KNOWN_SID_TYPE type)
{
    BYTE wellKnown[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(wellKnown);
    if (!CreateWellKnownSid(type, nullptr, wellKnown, &size))
        return false;
    return EqualSid(&ace->SidStart, wellKnown) != FALSE;
}

}

TEST(UpdateBridgeDeploymentTest, DeploysPayloadAndPrunesStaleBuilds)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);

    const QString root = temp.filePath(QStringLiteral("machine/Updater"));
    writeFile(root + QStringLiteral("/run1.aaaa1111/HarkenHubUpdateBridge.exe"),
              QByteArrayLiteral("old"));

    QString error;
    const QString deployed = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("run2.bbbb2222"),
        VNH::updaterAclSddl(), &error);
    ASSERT_FALSE(deployed.isEmpty()) << error.toStdString();
    const QString deployedDir = QFileInfo(deployed).absolutePath();
    EXPECT_TRUE(QFileInfo(deployedDir).fileName().startsWith(
        QStringLiteral("run2.bbbb2222-")));
    EXPECT_TRUE(QFile::exists(deployed));
    EXPECT_TRUE(
        QFile::exists(deployedDir + QStringLiteral("/Qt6Core.dll")));
    EXPECT_TRUE(QFile::exists(
        deployedDir + QStringLiteral("/tls/qschannelbackend.dll")));
    EXPECT_TRUE(QDir(root + QStringLiteral("/run1.aaaa1111")).exists());

    VNH::applyDirectoryAcl(root, permissiveSddl(), &error);
    VNH::applyDirectoryAcl(deployedDir, permissiveSddl(), &error);
    VNH::pruneUpdateBridgePayloads(root, deployed);
    EXPECT_FALSE(QDir(root + QStringLiteral("/run1.aaaa1111")).exists());

    VNH::applyDirectoryAcl(root, permissiveSddl(), &error);
}

TEST(UpdateBridgeDeploymentTest, RedeployOfTheSameBuildUsesANewDirectory)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);
    const QString root = temp.filePath(QStringLiteral("machine/Updater"));

    QString error;
    const QString original = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("run3.cccc3333"), permissiveSddl(), &error);
    ASSERT_FALSE(original.isEmpty()) << error.toStdString();

    QFile bridge(source + QStringLiteral("/HarkenHubUpdateBridge.exe"));
    ASSERT_TRUE(bridge.open(QIODevice::WriteOnly));
    ASSERT_GT(bridge.write(QByteArrayLiteral("bridge-v2")), 0);
    bridge.close();

    const QString deployed = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("run3.cccc3333"), permissiveSddl(),
        &error);
    ASSERT_FALSE(deployed.isEmpty()) << error.toStdString();
    EXPECT_NE(deployed, original);

    QFile previous(original);
    ASSERT_TRUE(previous.open(QIODevice::ReadOnly));
    EXPECT_EQ(previous.readAll(), QByteArrayLiteral("bridge"));
    previous.close();

    QFile copied(deployed);
    ASSERT_TRUE(copied.open(QIODevice::ReadOnly));
    EXPECT_EQ(copied.readAll(), QByteArrayLiteral("bridge-v2"));
}

TEST(UpdateBridgeDeploymentTest, RedeployAndPrunePreserveTheRunningBridge)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);
    const QString sourceExe = source + QStringLiteral("/HarkenHubUpdateBridge.exe");
    ASSERT_TRUE(QFile::remove(sourceExe));
    ASSERT_TRUE(QFile::copy(qEnvironmentVariable("ComSpec"), sourceExe));
    const QString root = temp.filePath(QStringLiteral("Updater"));
    QString error;
    const QString original = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("same-build"), permissiveSddl(), &error);
    ASSERT_FALSE(original.isEmpty()) << error.toStdString();

    QProcess bridge;
    bridge.start(original, {QStringLiteral("/d"), QStringLiteral("/c"),
                            QStringLiteral("pause")});
    const auto stopBridge = qScopeGuard([&bridge]() {
        if (bridge.state() != QProcess::NotRunning) {
            bridge.kill();
            bridge.waitForFinished();
        }
    });
    ASSERT_TRUE(bridge.waitForStarted());
    ASSERT_TRUE(bridge.waitForReadyRead());
    EXPECT_FALSE(DeleteFileW(reinterpret_cast<LPCWSTR>(original.utf16())));
    const QString deployed = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("same-build"), permissiveSddl(), &error);
    ASSERT_FALSE(deployed.isEmpty()) << error.toStdString();
    EXPECT_NE(deployed, original);
    VNH::pruneUpdateBridgePayloads(root, deployed);
    const QDir originalDir = QFileInfo(original).dir();
    EXPECT_TRUE(QFile::exists(original));
    EXPECT_TRUE(QFile::exists(originalDir.filePath(QStringLiteral("Qt6Core.dll"))));
    EXPECT_TRUE(QFile::exists(originalDir.filePath(QStringLiteral("tls/qschannelbackend.dll"))));

    bridge.write("\n");
    ASSERT_TRUE(bridge.waitForFinished());
    VNH::pruneUpdateBridgePayloads(root, deployed);
    EXPECT_FALSE(originalDir.exists());
    EXPECT_TRUE(QFile::exists(deployed));
}

TEST(UpdateBridgeDeploymentTest, PrunePreservesPayloadWhenBridgeBinaryCannotBeDeleted)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);
    const QString root = temp.filePath(QStringLiteral("Updater"));
    QString error;
    const QString original = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("old-build"), permissiveSddl(), &error);
    ASSERT_FALSE(original.isEmpty()) << error.toStdString();
    const QString deployed = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("new-build"), permissiveSddl(), &error);
    ASSERT_FALSE(deployed.isEmpty()) << error.toStdString();
    const QDir originalDir = QFileInfo(original).dir();
    ASSERT_TRUE(VNH::runningExecutablesUnder(originalDir.path()).isEmpty());

    {
        const HANDLE locked = CreateFileW(
            reinterpret_cast<LPCWSTR>(original.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
            nullptr);
        ASSERT_NE(locked, INVALID_HANDLE_VALUE);
        const auto closeLock = qScopeGuard([locked]() { CloseHandle(locked); });
        VNH::pruneUpdateBridgePayloads(root, deployed);
        EXPECT_TRUE(QFile::exists(original));
        EXPECT_TRUE(QFile::exists(originalDir.filePath(QStringLiteral("Qt6Core.dll"))));
        EXPECT_TRUE(QFile::exists(originalDir.filePath(QStringLiteral("tls/qschannelbackend.dll"))));
    }

    VNH::pruneUpdateBridgePayloads(root, deployed);
    EXPECT_FALSE(originalDir.exists());
    EXPECT_TRUE(QFile::exists(deployed));
}

TEST(UpdateBridgeDeploymentTest, FailedDeploymentPreservesThePreviousPayload)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);
    const QString root = temp.filePath(QStringLiteral("Updater"));
    QString error;
    const QString original = VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("same-build"), permissiveSddl(), &error);
    ASSERT_FALSE(original.isEmpty()) << error.toStdString();
    EXPECT_TRUE(VNH::deployUpdateBridgePayload(
        source, root, QStringLiteral("same-build"), QStringLiteral("invalid"),
        &error).isEmpty());
    EXPECT_TRUE(QFile::exists(original));
    EXPECT_TRUE(QFile::exists(QFileInfo(original).dir().filePath(QStringLiteral("Qt6Core.dll"))));
    EXPECT_EQ(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
}

TEST(UpdateBridgeDeploymentTest, RejectsMissingBridgeBinary)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    writeFile(source + QStringLiteral("/Qt6Core.dll"),
              QByteArrayLiteral("core"));

    QString error;
    EXPECT_TRUE(VNH::deployUpdateBridgePayload(
                    source, temp.filePath(QStringLiteral("machine/Updater")),
                    QStringLiteral("run4.dddd4444"), permissiveSddl(), &error)
                    .isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(UpdateBridgeDeploymentTest, RejectsUnsafeBuildId)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);

    QString error;
    EXPECT_TRUE(VNH::deployUpdateBridgePayload(
                    source, temp.filePath(QStringLiteral("machine/Updater")),
                    QStringLiteral("../evil"), permissiveSddl(), &error)
                    .isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(UpdateBridgeDeploymentTest, HardensUpdaterRootAcl)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString source = temp.filePath(QStringLiteral("payload"));
    makePayload(source);
    const QString root = temp.filePath(QStringLiteral("machine/Updater"));

    QString error;
    ASSERT_FALSE(VNH::deployUpdateBridgePayload(
                     source, root, QStringLiteral("run5.eeee5555"),
                     VNH::updaterAclSddl(), &error)
                     .isEmpty())
        << error.toStdString();

    const std::wstring nativePath =
        QDir::toNativeSeparators(root).toStdWString();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_EQ(GetNamedSecurityInfoW(nativePath.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr,
                                    nullptr, &dacl, nullptr, &descriptor),
              static_cast<DWORD>(ERROR_SUCCESS));
    ASSERT_NE(dacl, nullptr);

    bool sawUsers = false;
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER* header = nullptr;
        ASSERT_TRUE(GetAce(dacl, i, reinterpret_cast<LPVOID*>(&header)));
        ASSERT_EQ(header->AceType, ACCESS_ALLOWED_ACE_TYPE);
        auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(header);
        if (aceSidIs(ace, WinBuiltinUsersSid)) {
            sawUsers = true;
            EXPECT_FALSE(ace->Mask & FILE_ADD_FILE);
            EXPECT_FALSE(ace->Mask & FILE_ADD_SUBDIRECTORY);
            EXPECT_FALSE(ace->Mask & WRITE_DAC);
            EXPECT_FALSE(ace->Mask & WRITE_OWNER);
        }
    }
    EXPECT_TRUE(sawUsers);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ASSERT_TRUE(GetSecurityDescriptorControl(descriptor, &control, &revision));
    EXPECT_TRUE(control & SE_DACL_PROTECTED);
    LocalFree(descriptor);

    VNH::applyDirectoryAcl(root, permissiveSddl(), &error);
    for (const QFileInfo& child : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        VNH::applyDirectoryAcl(child.absoluteFilePath(), permissiveSddl(), &error);
}
