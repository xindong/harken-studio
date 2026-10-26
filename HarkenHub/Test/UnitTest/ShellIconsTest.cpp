#include "Platform/Windows/ShellIcons.h"
#include "Platform/Windows/TaskbarIcon.h"
#include "UI/HubIcons.h"

#include <QDir>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <future>
#include <thread>
#include <gtest/gtest.h>

TEST(ShellIconsTest, ShellReadableImagesPreserveEverySourceSizeAndPixelAlpha)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const QColor color : {QColor(Qt::black), QColor(Qt::white)}) {
        const QString path = VNH::saveShellIcon(VNH::HubIcons::applicationIcon(color), directory.path());
        ASSERT_FALSE(path.isEmpty());
        const auto nativePath = QDir::toNativeSeparators(path).toStdWString();
        QImageReader original(QStringLiteral(":/Resources/icons/HubIcon.ico"));
        QImageReader persisted(path);
        ASSERT_GT(original.imageCount(), 0);
        ASSERT_EQ(persisted.imageCount(), original.imageCount());
        for (int frame = 0; frame < original.imageCount(); ++frame) {
            ASSERT_TRUE(original.jumpToImage(frame));
            ASSERT_TRUE(persisted.jumpToImage(frame));
            const QImage source = original.read().convertToFormat(QImage::Format_ARGB32);
            const QImage stored = persisted.read().convertToFormat(QImage::Format_ARGB32);
            ASSERT_FALSE(source.isNull());
            ASSERT_EQ(stored.size(), source.size());
            HICON handle = nullptr;
            UINT iconId = 0;
            ASSERT_EQ(PrivateExtractIconsW(nativePath.c_str(), 0, source.width(), source.height(),
                                          &handle, &iconId, 1, 0), 1u);
            ASSERT_NE(handle, nullptr);
            const auto destroy = qScopeGuard([handle] { DestroyIcon(handle); });
            const QImage shell = QImage::fromHICON(handle).convertToFormat(QImage::Format_ARGB32);
            ASSERT_EQ(shell.size(), source.size());
            for (int y = 0; y < source.height(); ++y) {
                for (int x = 0; x < source.width(); ++x) {
                    const int alpha = qAlpha(source.pixel(x, y));
                    ASSERT_EQ(qAlpha(stored.pixel(x, y)), alpha);
                    ASSERT_EQ(qAlpha(shell.pixel(x, y)), alpha);
                    if (alpha) {
                        ASSERT_EQ(stored.pixelColor(x, y).rgb(), color.rgb());
                        ASSERT_EQ(shell.pixelColor(x, y).rgb(), color.rgb());
                    }
                }
            }
        }
    }
}

namespace {

class ShellShortcutTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        previousRoot = qgetenv("HARKEN_HUB_SHELL_ROOT");
        previousUser = qgetenv("HARKEN_HUB_USER_ROOT");
        qputenv("HARKEN_HUB_SHELL_ROOT", directory.path().toUtf8());
        qputenv("HARKEN_HUB_USER_ROOT", directory.path().toUtf8());
        initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE);
        pinnedRoot = directory.filePath(QStringLiteral("Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"));
        ASSERT_TRUE(QDir().mkpath(pinnedRoot));
        ASSERT_TRUE(QDir().mkpath(directory.filePath(QStringLiteral("Desktop"))));
    }

    void TearDown() override
    {
        if (SUCCEEDED(initialized))
            CoUninitialize();
        if (previousRoot.isNull())
            qunsetenv("HARKEN_HUB_SHELL_ROOT");
        else
            qputenv("HARKEN_HUB_SHELL_ROOT", previousRoot);
        if (previousUser.isNull())
            qunsetenv("HARKEN_HUB_USER_ROOT");
        else
            qputenv("HARKEN_HUB_USER_ROOT", previousUser);
    }

    void save(const QString& path, const QString& target, const QString& icon, const QString& arguments)
    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(link.GetAddressOf())));
        ASSERT_HRESULT_SUCCEEDED(link->SetPath(QDir::toNativeSeparators(target).toStdWString().c_str()));
        ASSERT_HRESULT_SUCCEEDED(link->SetIconLocation(QDir::toNativeSeparators(icon).toStdWString().c_str(), 0));
        ASSERT_HRESULT_SUCCEEDED(link->SetArguments(arguments.toStdWString().c_str()));
        Microsoft::WRL::ComPtr<IPersistFile> file;
        ASSERT_HRESULT_SUCCEEDED(link.As(&file));
        ASSERT_HRESULT_SUCCEEDED(file->Save(QDir::toNativeSeparators(path).toStdWString().c_str(), TRUE));
    }

    QStringList read(const QString& path)
    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(link.GetAddressOf()))))
            return {};
        Microsoft::WRL::ComPtr<IPersistFile> file;
        if (FAILED(link.As(&file)) || FAILED(file->Load(QDir::toNativeSeparators(path).toStdWString().c_str(), STGM_READ)))
            return {};
        wchar_t icon[MAX_PATH]{}, arguments[2048]{};
        int index = 0;
        if (FAILED(link->GetIconLocation(icon, MAX_PATH, &index)) || FAILED(link->GetArguments(arguments, 2048)))
            return {};
        return {QFileInfo(path).symLinkTarget(), QDir::fromNativeSeparators(QString::fromWCharArray(icon)),
                QString::fromWCharArray(arguments)};
    }

    QTemporaryDir directory;
    QString pinnedRoot;
    QByteArray previousRoot, previousUser;
    HRESULT initialized = E_FAIL;
};

}

TEST_F(ShellShortcutTest, MigrationPreservesUserChoicesAndIsIdempotent)
{
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/HarkenHub.exe");
    const QString legacy = QCoreApplication::applicationDirPath() + QStringLiteral("/HarkenStudioHub.exe");
    const QString arguments = QStringLiteral("--edit-integration --harken-project=\"C:/Project With Spaces/Game.hsp\"");
    const QString pinned = pinnedRoot + QStringLiteral("/Harken Studio Hub.lnk");
    const QString custom = pinnedRoot + QStringLiteral("/Custom.lnk");
    const QString other = pinnedRoot + QStringLiteral("/Other.lnk");
    const QString desktop = directory.filePath(QStringLiteral("Desktop/Harken Hub.lnk"));
    const QString customIcon = directory.filePath(QStringLiteral("custom.ico"));
    const QString otherExecutable = directory.filePath(QStringLiteral("HarkenHub.exe"));
    QFile otherFile(otherExecutable);
    ASSERT_TRUE(otherFile.open(QIODevice::WriteOnly));
    otherFile.close();
    save(pinned, legacy, legacy, arguments);
    save(custom, executable, customIcon, arguments);
    save(desktop, executable, executable, arguments);
    save(other, otherExecutable, customIcon, arguments);
    QFile otherLink(other);
    ASSERT_TRUE(otherLink.open(QIODevice::ReadOnly));
    const auto otherBytes = otherLink.readAll();
    otherLink.close();
    QFile desktopLink(desktop);
    ASSERT_TRUE(desktopLink.open(QIODevice::ReadOnly));
    const auto desktopBytes = desktopLink.readAll();
    desktopLink.close();
    for (const QColor color : {QColor(Qt::black), QColor(Qt::white), QColor(Qt::black)}) {
        const QString path = VNH::saveShellIcon(VNH::HubIcons::applicationIcon(color), directory.filePath(QStringLiteral("shell-icons")));
        ASSERT_FALSE(path.isEmpty());
        ASSERT_EQ(VNH::migrateShellShortcuts(path), VNH::ShortcutMigrationResult::Complete);
        EXPECT_EQ(read(pinned), (QStringList{executable, path, arguments}));
        EXPECT_EQ(read(custom), (QStringList{executable, customIcon, arguments}));
        EXPECT_EQ(read(desktop), (QStringList{executable, executable, arguments}));
        ASSERT_TRUE(desktopLink.open(QIODevice::ReadOnly));
        EXPECT_EQ(desktopLink.readAll(), desktopBytes);
        desktopLink.close();
        ASSERT_TRUE(otherLink.open(QIODevice::ReadOnly));
        EXPECT_EQ(otherLink.readAll(), otherBytes);
        otherLink.close();
        QFile migrated(pinned);
        ASSERT_TRUE(migrated.open(QIODevice::ReadOnly));
        const auto bytes = migrated.readAll();
        migrated.close();
        ASSERT_EQ(VNH::migrateShellShortcuts(path), VNH::ShortcutMigrationResult::Complete);
        ASSERT_TRUE(migrated.open(QIODevice::ReadOnly));
        EXPECT_EQ(migrated.readAll(), bytes);
    }
}

TEST_F(ShellShortcutTest, ThemeChangeCompletesAfterConcurrentMigrationReleasesLock)
{
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/HarkenHub.exe");
    const QString pinned = pinnedRoot + QStringLiteral("/Harken Hub.lnk");
    save(pinned, executable, executable, QStringLiteral("--show-versions"));
    const auto hash = QCryptographicHash::hash(QCoreApplication::applicationDirPath().toCaseFolded().toUtf8(),
                                             QCryptographicHash::Sha256).toHex();
    const auto name = (QStringLiteral("Local\\HarkenHubShortcutMigration-") + QString::fromLatin1(hash)).toStdWString();
    std::promise<bool> acquired;
    std::promise<void> release;
    auto released = release.get_future();
    std::thread owner([&] {
        const HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        const DWORD result = mutex ? WaitForSingleObject(mutex, 2000) : WAIT_FAILED;
        const bool owns = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        acquired.set_value(owns);
        released.wait();
        if (owns)
            ReleaseMutex(mutex);
        if (mutex)
            CloseHandle(mutex);
    });
    bool unlocked = false;
    const auto finish = qScopeGuard([&] {
        if (!unlocked)
            release.set_value();
        owner.join();
    });
    ASSERT_TRUE(acquired.get_future().get());
    VNH::TaskbarIcon taskbar;
    taskbar.setIcon(VNH::HubIcons::applicationIcon(Qt::white));
    taskbar.setIcon(VNH::HubIcons::applicationIcon(Qt::black));
    ASSERT_EQ(read(pinned).value(1), executable);
    release.set_value();
    unlocked = true;
    const QString expected = VNH::saveShellIcon(VNH::HubIcons::applicationIcon(Qt::black), directory.filePath(QStringLiteral("shell-icons")));
    for (int attempt = 0; attempt < 40 && read(pinned).value(1) != expected; ++attempt)
        QTest::qWait(50);
    EXPECT_EQ(read(pinned), (QStringList{executable, expected, QStringLiteral("--show-versions")}));
}
