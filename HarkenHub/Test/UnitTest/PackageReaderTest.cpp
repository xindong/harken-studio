#include "Packaging/PackageReader.h"

#include "ZipTestUtils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <vector>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

using VNH::PackageReader;
using HarkenHubTest::TestEntry;
using HarkenHubTest::writeArchive;

namespace {

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

}

TEST(PackageReader, RoundtripExtractsFilesIntact)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString archive = dir.filePath(QStringLiteral("pkg.zip"));

    QByteArray large(200000, 'v');
    ASSERT_TRUE(writeArchive(archive, {
        {QByteArrayLiteral("HarkenStudio.exe"), QByteArrayLiteral("binary-stub")},
        {QByteArrayLiteral("data/"), QByteArray()},
        {QByteArrayLiteral("data/large.bin"), large},
    }));

    const auto listing = PackageReader::listEntries(archive);
    ASSERT_TRUE(listing.ok) << listing.error.toStdString();
    ASSERT_EQ(listing.entries.size(), 3u);
    EXPECT_EQ(listing.entries[2].uncompressedSize, large.size());

    const QString out = dir.filePath(QStringLiteral("out"));
    QString error;
    ASSERT_TRUE(PackageReader::extractAll(archive, out, &error))
        << error.toStdString();
    EXPECT_EQ(readFile(out + QStringLiteral("/HarkenStudio.exe")),
              QByteArrayLiteral("binary-stub"));
    EXPECT_EQ(readFile(out + QStringLiteral("/data/large.bin")), large);
}

#ifndef Q_OS_WIN

TEST(PackageReader, RestoresUnixPermissionsOnExtract)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString archive = dir.filePath(QStringLiteral("perm.zip"));

    TestEntry executable;
    executable.name = QByteArrayLiteral("bin/tool");
    executable.data = QByteArrayLiteral("stub");
    executable.versionMadeby = 3 << 8;
    executable.externalFa = 0100755u << 16;
    TestEntry document;
    document.name = QByteArrayLiteral("readme.txt");
    document.data = QByteArrayLiteral("text");
    document.versionMadeby = 3 << 8;
    document.externalFa = 0100644u << 16;
    TestEntry setuidTool;
    setuidTool.name = QByteArrayLiteral("bin/suid-tool");
    setuidTool.data = QByteArrayLiteral("stub");
    setuidTool.versionMadeby = 3 << 8;
    setuidTool.externalFa = 0104755u << 16;
    TestEntry ownerOnly;
    ownerOnly.name = QByteArrayLiteral("private.json");
    ownerOnly.data = QByteArrayLiteral("{}");
    ownerOnly.versionMadeby = 3 << 8;
    ownerOnly.externalFa = 0100600u << 16;
    ASSERT_TRUE(writeArchive(archive,
                             {executable, document, setuidTool, ownerOnly}));

    const QString out = dir.filePath(QStringLiteral("out"));
    QString error;
    ASSERT_TRUE(PackageReader::extractAll(archive, out, &error))
        << error.toStdString();

    struct stat toolInfo = {};
    ASSERT_EQ(stat(QFile::encodeName(out + QStringLiteral("/bin/tool"))
                       .constData(),
                   &toolInfo),
              0);
    EXPECT_EQ(toolInfo.st_mode & 07777, static_cast<mode_t>(0755));

    struct stat documentInfo = {};
    ASSERT_EQ(stat(QFile::encodeName(out + QStringLiteral("/readme.txt"))
                       .constData(),
                   &documentInfo),
              0);
    EXPECT_EQ(documentInfo.st_mode & 07777, static_cast<mode_t>(0644));

    struct stat setuidInfo = {};
    ASSERT_EQ(stat(QFile::encodeName(out + QStringLiteral("/bin/suid-tool"))
                       .constData(),
                   &setuidInfo),
              0);
    EXPECT_EQ(setuidInfo.st_mode & 07777, static_cast<mode_t>(0755));

    struct stat ownerOnlyInfo = {};
    ASSERT_EQ(stat(QFile::encodeName(out + QStringLiteral("/private.json"))
                       .constData(),
                   &ownerOnlyInfo),
              0);
    EXPECT_EQ(ownerOnlyInfo.st_mode & 07777, static_cast<mode_t>(0644));
}

#endif

TEST(PackageReader, RejectsPathTraversal)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString archive = dir.filePath(QStringLiteral("evil.zip"));
    ASSERT_TRUE(writeArchive(archive, {
        {QByteArrayLiteral("../evil.txt"), QByteArrayLiteral("x")},
    }));

    const auto listing = PackageReader::listEntries(archive);
    EXPECT_FALSE(listing.ok);

    const QString out = dir.filePath(QStringLiteral("out"));
    QString error;
    EXPECT_FALSE(PackageReader::extractAll(archive, out, &error));
    EXPECT_FALSE(QFile::exists(dir.filePath(QStringLiteral("evil.txt"))));
}

TEST(PackageReader, RejectsAbsoluteAndDrivePaths)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString absolute = dir.filePath(QStringLiteral("abs.zip"));
    ASSERT_TRUE(writeArchive(absolute, {
        {QByteArrayLiteral("/abs.txt"), QByteArrayLiteral("x")},
    }));
    EXPECT_FALSE(PackageReader::listEntries(absolute).ok);

    const QString drive = dir.filePath(QStringLiteral("drive.zip"));
    ASSERT_TRUE(writeArchive(drive, {
        {QByteArrayLiteral("C:/evil.txt"), QByteArrayLiteral("x")},
    }));
    EXPECT_FALSE(PackageReader::listEntries(drive).ok);

}

TEST(PackageReader, EntryPathValidatorRejectsHostileNames)
{
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QStringLiteral("a\\b.txt")));
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QStringLiteral("C:\\evil.txt")));
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QStringLiteral("//server/share/x")));
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QStringLiteral("a/../b.txt")));
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QStringLiteral("./a.txt")));
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QStringLiteral("a//b.txt")));
    EXPECT_FALSE(PackageReader::isSafeEntryPath(QString()));
    EXPECT_TRUE(PackageReader::isSafeEntryPath(QStringLiteral("a/b.txt")));
    EXPECT_TRUE(PackageReader::isSafeEntryPath(QStringLiteral("dir/")));
}

TEST(PackageReader, RejectsDuplicateEntriesCaseInsensitive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString archive = dir.filePath(QStringLiteral("dup.zip"));
    ASSERT_TRUE(writeArchive(archive, {
        {QByteArrayLiteral("Dup.txt"), QByteArrayLiteral("a")},
        {QByteArrayLiteral("dup.txt"), QByteArrayLiteral("b")},
    }));

    EXPECT_FALSE(PackageReader::listEntries(archive).ok);
}

TEST(PackageReader, RejectsSymlinkEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString archive = dir.filePath(QStringLiteral("link.zip"));

    TestEntry link;
    link.name = QByteArrayLiteral("link.txt");
    link.data = QByteArrayLiteral("/etc/target");
    link.versionMadeby = static_cast<uint16_t>((3 << 8) | 20);
    link.externalFa = 0xA1FF0000u;
    ASSERT_TRUE(writeArchive(archive, {link}));

    EXPECT_FALSE(PackageReader::listEntries(archive).ok);
}

TEST(PackageReader, Sha256MatchesKnownVector)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("abc.bin"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("abc");
    file.close();

    EXPECT_EQ(PackageReader::fileSha256Hex(path),
              QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}
