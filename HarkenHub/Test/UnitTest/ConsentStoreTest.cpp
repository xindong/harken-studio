#include "Legal/ConsentStore.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using VNH::ConsentStore;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

class ConsentStoreTest : public ::testing::Test
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

TEST_F(ConsentStoreTest, RecordsAndChecksConsent)
{
    EXPECT_FALSE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                          QStringLiteral("placeholder-1"),
                                          QStringLiteral("abc123")));
    ASSERT_TRUE(ConsentStore::recordConsent(QStringLiteral("hub-eula"),
                                            QStringLiteral("placeholder-1"),
                                            QStringLiteral("abc123"),
                                            QStringLiteral("0.1.0"), kNow));
    EXPECT_TRUE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                         QStringLiteral("placeholder-1"),
                                         QStringLiteral("abc123")));
    EXPECT_FALSE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                          QStringLiteral("placeholder-2"),
                                          QStringLiteral("abc123")));
    EXPECT_FALSE(ConsentStore::hasConsent(QStringLiteral("studio-eula"),
                                          QStringLiteral("placeholder-1"),
                                          QStringLiteral("abc123")));
}

TEST_F(ConsentStoreTest, RecordingTwiceIsIdempotent)
{
    ASSERT_TRUE(ConsentStore::recordConsent(QStringLiteral("studio-eula"),
                                            QStringLiteral("placeholder-1"),
                                            QStringLiteral("abc123"),
                                            QStringLiteral("0.1.0"), kNow));
    ASSERT_TRUE(ConsentStore::recordConsent(QStringLiteral("studio-eula"),
                                            QStringLiteral("placeholder-1"),
                                            QStringLiteral("abc123"),
                                            QStringLiteral("0.1.0"), kNow));
    EXPECT_TRUE(ConsentStore::hasConsent(QStringLiteral("studio-eula"),
                                         QStringLiteral("placeholder-1"),
                                         QStringLiteral("abc123")));
    QFile file(ConsentStore::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QJsonDocument::fromJson(file.readAll()).array().size(), 1);
}

TEST_F(ConsentStoreTest, SameVersionDifferentHashRequiresNewConsent)
{
    ASSERT_TRUE(ConsentStore::recordConsent(QStringLiteral("hub-eula"),
                                            QStringLiteral("placeholder-1"),
                                            QStringLiteral("hash-a"),
                                            QStringLiteral("0.1.0"), kNow));
    EXPECT_FALSE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                          QStringLiteral("placeholder-1"),
                                          QStringLiteral("hash-b")));
    ASSERT_TRUE(ConsentStore::recordConsent(QStringLiteral("hub-eula"),
                                            QStringLiteral("placeholder-1"),
                                            QStringLiteral("hash-b"),
                                            QStringLiteral("0.1.0"), kNow));
    EXPECT_TRUE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                         QStringLiteral("placeholder-1"),
                                         QStringLiteral("hash-b")));
    EXPECT_TRUE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                         QStringLiteral("placeholder-1"),
                                         QStringLiteral("hash-a")));
    QFile file(ConsentStore::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QJsonDocument::fromJson(file.readAll()).array().size(), 2);
}

TEST_F(ConsentStoreTest, LegacyRecordWithoutHashStaysValid)
{
    QJsonObject entry;
    entry[QStringLiteral("documentId")] = QStringLiteral("hub-eula");
    entry[QStringLiteral("documentVersion")] = QStringLiteral("draft-2");
    entry[QStringLiteral("hubVersion")] = QStringLiteral("0.1.0");
    entry[QStringLiteral("acceptedAt")] =
        kNow.toUTC().toString(Qt::ISODateWithMs);
    QJsonArray consents;
    consents.append(entry);
    const QString path = ConsentStore::filePath();
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).dir().path()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(consents).toJson(QJsonDocument::Indented));
    file.close();

    EXPECT_TRUE(ConsentStore::hasConsent(QStringLiteral("hub-eula"),
                                         QStringLiteral("draft-2"),
                                         QStringLiteral("any-hash")));
}

TEST_F(ConsentStoreTest, RejectsEmptyIdentity)
{
    EXPECT_FALSE(ConsentStore::recordConsent(QString(), QStringLiteral("v1"),
                                             QStringLiteral("abc123"),
                                             QStringLiteral("0.1.0"), kNow));
    EXPECT_FALSE(ConsentStore::recordConsent(QStringLiteral("doc"), QString(),
                                             QStringLiteral("abc123"),
                                             QStringLiteral("0.1.0"), kNow));
}

TEST_F(ConsentStoreTest, PersistsDocumentHash)
{
    ASSERT_TRUE(ConsentStore::recordConsent(QStringLiteral("hub-eula"),
                                            QStringLiteral("placeholder-1"),
                                            QStringLiteral("deadbeef"),
                                            QStringLiteral("0.1.0"), kNow));
    QFile file(ConsentStore::filePath());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonArray consents =
        QJsonDocument::fromJson(file.readAll()).array();
    ASSERT_EQ(consents.size(), 1);
    EXPECT_EQ(consents.first()
                  .toObject()[QStringLiteral("documentSha256")]
                  .toString(),
              QStringLiteral("deadbeef"));
}

}
