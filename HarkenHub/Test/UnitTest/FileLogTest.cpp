#include "App/FileLog.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

using VNH::FileLog;

namespace {

QString readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

class FileLogTest : public ::testing::Test
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

}

TEST_F(FileLogTest, AppendWritesTimestampedLevelLine)
{
    const QString path = FileLog::filePathFor(QStringLiteral("gui"));
    ASSERT_TRUE(FileLog::append(path, QStringLiteral("INFO"),
                                QStringLiteral("first message")));
    ASSERT_TRUE(FileLog::append(path, QStringLiteral("WARN"),
                                QStringLiteral("second message")));

    const QString content = readAll(path);
    const QStringList lines =
        content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 2);
    static const QRegularExpression pattern(QStringLiteral(
        "^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z "
        "\\[(INFO|WARN)\\] .+$"));
    EXPECT_TRUE(pattern.match(lines[0]).hasMatch()) << lines[0].toStdString();
    EXPECT_TRUE(lines[0].endsWith(QStringLiteral("first message")));
    EXPECT_TRUE(lines[1].contains(QStringLiteral("[WARN]")));
}

TEST_F(FileLogTest, RotationMovesOversizedFileAside)
{
    const QString path = FileLog::filePathFor(QStringLiteral("gui"));
    ASSERT_TRUE(FileLog::append(path, QStringLiteral("INFO"),
                                QString(128, QLatin1Char('x'))));

    FileLog::rotateIfOversized(path, 8);
    EXPECT_FALSE(QFile::exists(path));
    EXPECT_TRUE(QFile::exists(path + QStringLiteral(".1")));

    ASSERT_TRUE(FileLog::append(path, QStringLiteral("INFO"),
                                QStringLiteral("fresh")));
    FileLog::rotateIfOversized(path, 1024 * 1024);
    EXPECT_TRUE(QFile::exists(path));
}

TEST_F(FileLogTest, IgnoresKnownWindowsFrameworkNoise)
{
    EXPECT_TRUE(FileLog::isIgnorableFrameworkMessage(QStringLiteral(
        "QWindowsWindow::setGeometry: Unable to set geometry 1280x720")));
    EXPECT_TRUE(FileLog::isIgnorableFrameworkMessage(QStringLiteral(
        "QWindowsPipeWriter: write failed. (The pipe has been ended.)")));
    EXPECT_FALSE(FileLog::isIgnorableFrameworkMessage(
        QStringLiteral("Hub update bridge dispatch failed")));
}
