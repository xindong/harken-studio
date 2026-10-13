#include "Transactions/HandoffArea.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

using VNH::HandoffArea;

namespace {

class MachineRootGuard
{
public:
    explicit MachineRootGuard(const QString& root)
    {
        qputenv("HARKEN_HUB_MACHINE_ROOT", root.toUtf8());
    }
    ~MachineRootGuard() { qunsetenv("HARKEN_HUB_MACHINE_ROOT"); }
};

QString makeSourcePackage(const QTemporaryDir& dir, const QByteArray& content)
{
    const QString path = dir.filePath(QStringLiteral("source.pkg"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(content);
    file.close();
    return path;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

}

TEST(HandoffArea, TransactionIdsAreWellFormedAndUnique)
{
    const QString first = HandoffArea::newTransactionId();
    const QString second = HandoffArea::newTransactionId();
    EXPECT_TRUE(HandoffArea::isValidTransactionId(first));
    EXPECT_TRUE(HandoffArea::isValidTransactionId(second));
    EXPECT_NE(first, second);

    EXPECT_FALSE(HandoffArea::isValidTransactionId(QStringLiteral("short")));
    EXPECT_FALSE(HandoffArea::isValidTransactionId(
        QStringLiteral("../../evil/aaaaaaaaaaaaaaaaaaaaaa")));
    EXPECT_FALSE(HandoffArea::isValidTransactionId(QString(32, QLatin1Char('X'))));
}

TEST(HandoffArea, PlaceCopiesPackageAndManifest)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.filePath(QStringLiteral("machine")));

    const QByteArray payload(300000, 'p');
    const QString source = makeSourcePackage(dir, payload);
    ASSERT_FALSE(source.isEmpty());

    const auto placement =
        HandoffArea::place(source, QByteArrayLiteral("{\"buildId\":\"run12\"}"));
    ASSERT_TRUE(placement.ok) << placement.error.toStdString();
    EXPECT_TRUE(HandoffArea::isValidTransactionId(placement.transactionId));
    EXPECT_EQ(readAll(placement.packagePath), payload);
    EXPECT_EQ(readAll(placement.manifestPath),
              QByteArrayLiteral("{\"buildId\":\"run12\"}"));
}

TEST(HandoffArea, ExistingTargetIsNeverOverwritten)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.filePath(QStringLiteral("machine")));

    const QString source = makeSourcePackage(dir, QByteArrayLiteral("data"));
    ASSERT_FALSE(source.isEmpty());

    const QString transactionId = HandoffArea::newTransactionId();
    const auto first =
        HandoffArea::placeWithId(transactionId, source, QByteArrayLiteral("{}"));
    ASSERT_TRUE(first.ok) << first.error.toStdString();

    const auto second =
        HandoffArea::placeWithId(transactionId, source, QByteArrayLiteral("{}"));
    EXPECT_FALSE(second.ok);
    EXPECT_EQ(readAll(first.packagePath), QByteArrayLiteral("data"));
}

TEST(HandoffArea, DiscardRemovesBothFilesAndRejectsHostileIds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.filePath(QStringLiteral("machine")));

    const QString source = makeSourcePackage(dir, QByteArrayLiteral("data"));
    ASSERT_FALSE(source.isEmpty());

    const auto placement = HandoffArea::place(source, QByteArrayLiteral("{}"));
    ASSERT_TRUE(placement.ok);

    EXPECT_TRUE(HandoffArea::discard(placement.transactionId));
    EXPECT_FALSE(QFile::exists(placement.packagePath));
    EXPECT_FALSE(QFile::exists(placement.manifestPath));

    EXPECT_TRUE(HandoffArea::discard(placement.transactionId));
    EXPECT_FALSE(HandoffArea::discard(QStringLiteral("../escape")));
}

TEST(HandoffArea, DiscardClearsPartialResidue)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.filePath(QStringLiteral("machine")));

    const QString source = makeSourcePackage(dir, QByteArrayLiteral("data"));
    ASSERT_FALSE(source.isEmpty());

    const auto placement = HandoffArea::place(source, QByteArrayLiteral("{}"));
    ASSERT_TRUE(placement.ok);
    ASSERT_TRUE(QFile::remove(placement.manifestPath));

    EXPECT_TRUE(HandoffArea::discard(placement.transactionId));
    EXPECT_FALSE(QFile::exists(placement.packagePath));
}
