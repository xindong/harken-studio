#include "Maintenance/LogTailer.h"
#include "UI/LogConsoleDialog.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

using VNH::LogTailer;
using VNH::LogConsoleDialog;

namespace {

bool writeFile(const QString& path, const QByteArray& content, bool append)
{
    QFile file(path);
    const auto mode = append ? (QIODevice::WriteOnly | QIODevice::Append)
                             : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!file.open(mode))
        return false;
    return file.write(content) == content.size();
}

}

TEST(LogTailerTest, EmitsOnlyIncrementalContent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.log"));
    ASSERT_TRUE(writeFile(path, QByteArrayLiteral("line1\n"), false));

    LogTailer tailer;
    tailer.setFiles({path});
    QStringList received;
    QObject::connect(&tailer, &LogTailer::appended, &tailer,
                     [&received](const QString& text) {
                         received.append(text);
                     });

    tailer.poll();
    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(received[0], QStringLiteral("line1\n"));

    tailer.poll();
    EXPECT_EQ(received.size(), 1);

    ASSERT_TRUE(writeFile(path, QByteArrayLiteral("line2\n"), true));
    tailer.poll();
    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(received[1], QStringLiteral("line2\n"));
}

TEST(LogTailerTest, TruncatedFileRestartsFromZero)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.log"));
    ASSERT_TRUE(writeFile(
        path, QByteArrayLiteral("old-one\nold-two\nold-three\n"), false));

    LogTailer tailer;
    tailer.setFiles({path});
    QStringList received;
    QObject::connect(&tailer, &LogTailer::appended, &tailer,
                     [&received](const QString& text) {
                         received.append(text);
                     });
    tailer.poll();
    ASSERT_EQ(received.size(), 1);

    ASSERT_TRUE(writeFile(path, QByteArrayLiteral("new\n"), false));
    tailer.poll();
    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(received[1], QStringLiteral("new\n"));
}

TEST(LogTailerTest, RotatedFileGrowingPastOldOffsetIsReReadFromZero)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.log"));
    ASSERT_TRUE(writeFile(
        path, QByteArrayLiteral("2026-07-26T05:00:00.001Z old line\n"),
        false));

    LogTailer tailer;
    tailer.setFiles({path});
    QStringList received;
    QObject::connect(&tailer, &LogTailer::appended, &tailer,
                     [&received](const QString& text) {
                         received.append(text);
                     });
    tailer.poll();
    ASSERT_EQ(received.size(), 1);

    ASSERT_TRUE(QFile::rename(path, path + QStringLiteral(".1")));
    ASSERT_TRUE(writeFile(
        path,
        QByteArrayLiteral("2026-07-26T05:00:01.002Z fresh line one\n"
                          "2026-07-26T05:00:01.003Z fresh line two\n"),
        false));

    tailer.poll();
    ASSERT_EQ(received.size(), 2);
    EXPECT_TRUE(received[1].startsWith(
        QStringLiteral("2026-07-26T05:00:01.002Z fresh line one")));
    EXPECT_TRUE(received[1].contains(QStringLiteral("fresh line two")));
}

TEST(LogTailerTest, MergesLinesAcrossFilesByTimestamp)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.log"));
    const QString b = dir.filePath(QStringLiteral("b.log"));
    ASSERT_TRUE(writeFile(
        a,
        QByteArrayLiteral("2026-07-26T05:00:00.100Z alpha one\n"
                          "2026-07-26T05:00:00.300Z alpha two\n"),
        false));
    ASSERT_TRUE(writeFile(
        b, QByteArrayLiteral("2026-07-26T05:00:00.200Z beta one\n"),
        false));

    LogTailer tailer;
    tailer.setFiles({a, b});
    QStringList received;
    QObject::connect(&tailer, &LogTailer::appended, &tailer,
                     [&received](const QString& text) {
                         received.append(text);
                     });
    tailer.poll();

    ASSERT_EQ(received.size(), 1);
    const QStringList lines =
        received[0].split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 3);
    EXPECT_TRUE(lines[0].contains(QStringLiteral("alpha one")));
    EXPECT_TRUE(lines[1].contains(QStringLiteral("beta one")));
    EXPECT_TRUE(lines[2].contains(QStringLiteral("alpha two")));
}

TEST(LogTailerTest, IncompleteLineIsHeldUntilNewlineArrives)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.log"));
    ASSERT_TRUE(writeFile(
        path, QByteArrayLiteral("2026-07-26T05:00:00.001Z partia"), false));

    LogTailer tailer;
    tailer.setFiles({path});
    QStringList received;
    QObject::connect(&tailer, &LogTailer::appended, &tailer,
                     [&received](const QString& text) {
                         received.append(text);
                     });
    tailer.poll();
    EXPECT_TRUE(received.isEmpty());

    ASSERT_TRUE(writeFile(path, QByteArrayLiteral("l done\n"), true));
    tailer.poll();
    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(received[0],
              QStringLiteral("2026-07-26T05:00:00.001Z partial done\n"));
}

TEST(LogTailerTest, MissingFileIsSkippedUntilItAppears)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("late.log"));

    LogTailer tailer;
    tailer.setFiles({path});
    QStringList received;
    QObject::connect(&tailer, &LogTailer::appended, &tailer,
                     [&received](const QString& text) {
                         received.append(text);
                     });
    tailer.poll();
    EXPECT_TRUE(received.isEmpty());

    ASSERT_TRUE(writeFile(path, QByteArrayLiteral("arrived\n"), false));
    tailer.poll();
    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(received[0], QStringLiteral("arrived\n"));
}

TEST(LogConsoleDialogTest, ConvertsUtcTimestampToSystemTime)
{
    const QString source =
        QStringLiteral("2026-08-13T00:04:36.440Z [INFO] message");
    const QDateTime timestamp = QDateTime::fromString(
        QStringLiteral("2026-08-13T00:04:36.440Z"), Qt::ISODateWithMs);
    const QString expected = timestamp.toLocalTime().toString(
                                 QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) +
                             QStringLiteral(" [INFO] message");

    EXPECT_EQ(LogConsoleDialog::localizeTimestamp(source), expected);
}

TEST(LogConsoleDialogTest, LeavesUnstructuredLineUnchanged)
{
    const QString source = QStringLiteral("message without timestamp");
    EXPECT_EQ(LogConsoleDialog::localizeTimestamp(source), source);
}
