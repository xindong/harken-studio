#include "ReleaseNotes/StudioReleaseNotes.h"
#include "Settings/HubSettings.h"
#include "Support/RangeHttpServer.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {

using HarkenHubTest::RangeHttpServer;
using VNH::HubSettings;
using VNH::StudioReleaseNotes;

class StudioReleaseNotesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(settings_.isValid());
        ASSERT_TRUE(server_.listen());
        qputenv("HARKEN_CONTENT_BASE_URL", server_.baseUrl().toEncoded());
        qputenv("HARKEN_HUB_USER_ROOT", settings_.path().toUtf8());
        ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("en")));
        setDocument(document_);
    }

    void TearDown() override
    {
        if (previousBase_.isNull())
            qunsetenv("HARKEN_CONTENT_BASE_URL");
        else
            qputenv("HARKEN_CONTENT_BASE_URL", previousBase_);
        if (previousRoot_.isNull())
            qunsetenv("HARKEN_HUB_USER_ROOT");
        else
            qputenv("HARKEN_HUB_USER_ROOT", previousRoot_);
    }

    void setDocument(const QJsonObject& document)
    {
        server_.setPayload(QJsonDocument(document).toJson(), QByteArrayLiteral("notes"));
    }

    const QByteArray previousBase_ = qgetenv("HARKEN_CONTENT_BASE_URL");
    const QByteArray previousRoot_ = qgetenv("HARKEN_HUB_USER_ROOT");
    QTemporaryDir settings_;
    RangeHttpServer server_;
    const QString version_ = QStringLiteral("1.2.3");
    const QJsonObject document_ = QJsonDocument::fromJson(R"({
        "schemaVersion": 1, "product": "harken-studio", "version": "1.2.3",
        "releaseNotesPageId": "release",
        "pages": [
            {"id": "other", "locale": "en", "path": "document/other.html"},
            {"id": "release", "locale": "en", "path": "document/harken-studio/1.2.3/en/notes.html"},
            {"id": "release", "locale": "zh", "path": "document/harken-studio/1.2.3/zh/\u53d1\u5e03\u65e5\u5fd7.html"}
        ]
    })").object();
};

TEST_F(StudioReleaseNotesTest, RejectsUnboundOrMalformedDocuments)
{
    QList<QByteArray> responses{QByteArrayLiteral("not JSON"), QByteArrayLiteral("[]")};
    const QJsonObject invalidValues{
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("product"), QStringLiteral("another-product")},
        {QStringLiteral("version"), QStringLiteral("9.9.9")},
        {QStringLiteral("releaseNotesPageId"), QStringLiteral("missing-page")},
        {QStringLiteral("pages"), QJsonObject()}};
    for (auto it = invalidValues.begin(); it != invalidValues.end(); ++it) {
        QJsonObject modified = document_;
        modified[it.key()] = it.value();
        responses.append(QJsonDocument(modified).toJson());
        modified.remove(it.key());
        responses.append(QJsonDocument(modified).toJson());
    }
    for (const QByteArray& response : responses) {
        SCOPED_TRACE(response.toStdString());
        server_.setPayload(response, QByteArrayLiteral("invalid"));
        StudioReleaseNotes notes;
        QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_).isEmpty());
    }
}

TEST_F(StudioReleaseNotesTest, UntrustedPathsCannotRedirectOrTraverse)
{
    const QStringList paths{
        QString(), QStringLiteral("https://other.example/notes"),
        QStringLiteral("//other.example/notes"), QStringLiteral("javascript:alert(1)"),
        QStringLiteral("/absolute.html"), QStringLiteral("../notes.html"),
        QStringLiteral("document/../notes.html"), QStringLiteral("%2e%2e/notes.html"),
        QStringLiteral("document/%2e%2e/notes.html"), QStringLiteral("document\\notes.html"),
        QStringLiteral("document/notes.html?redirect=elsewhere"),
        QStringLiteral("document/notes.html#fragment")};
    for (const QString& path : paths) {
        SCOPED_TRACE(path.toStdString());
        QJsonObject modified = document_;
        modified[QStringLiteral("pages")] = QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("release")},
            {QStringLiteral("locale"), QStringLiteral("en")},
            {QStringLiteral("path"), path}}};
        setDocument(modified);
        StudioReleaseNotes notes;
        QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_).isEmpty());
    }
}

TEST_F(StudioReleaseNotesTest, SharedRequestsPreserveLocalizedTargets)
{
    StudioReleaseNotes notes;
    QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
    notes.fetch(version_);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_EQ(server_.requestCount(), 1);
    EXPECT_EQ(notes.url(version_), QUrl(QStringLiteral(
        "https://harkenstudio.xd.com/document/harken-studio/1.2.3/en/notes.html")));
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("zh_CN")));
    const QUrl chinese(QStringLiteral(
        "https://harkenstudio.xd.com/document/harken-studio/1.2.3/zh/\u53d1\u5e03\u65e5\u5fd7.html"));
    EXPECT_EQ(notes.url(version_), chinese);
    EXPECT_EQ(QUrl::fromEncoded(notes.url(version_).toEncoded()), chinese);
    notes.fetch(version_);
    EXPECT_FALSE(finished.wait(50));
    EXPECT_EQ(server_.requestCount(), 1);
}

TEST_F(StudioReleaseNotesTest, MissingLocaleDoesNotSelectAnotherLanguage)
{
    QJsonObject modified = document_;
    QJsonArray pages = modified.value(QStringLiteral("pages")).toArray();
    pages.removeLast();
    modified[QStringLiteral("pages")] = pages;
    setDocument(modified);
    StudioReleaseNotes notes;
    QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    const QUrl english = notes.url(version_);
    ASSERT_FALSE(english.isEmpty());
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("zh_CN")));
    EXPECT_TRUE(notes.url(version_).isEmpty());
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("en")));
    EXPECT_EQ(notes.url(version_), english);
}

TEST_F(StudioReleaseNotesTest, FailedResponsesCanRecoverWithoutPublishingInvalidLinks)
{
    StudioReleaseNotes notes;
    QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
    for (const int status : {404, 503, 200}) {
        RangeHttpServer::Script failure;
        failure.mode = RangeHttpServer::Script::Mode::Status;
        failure.status = status;
        failure.responseBody = QByteArrayLiteral("not JSON");
        server_.enqueue(failure);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_).isEmpty());
    }
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_FALSE(notes.url(version_).isEmpty());
    EXPECT_EQ(server_.requestCount(), 4);
}

TEST_F(StudioReleaseNotesTest, RedirectsCannotSendRequestsToAnotherServer)
{
    RangeHttpServer destination;
    ASSERT_TRUE(destination.listen());
    RangeHttpServer::Script redirect;
    redirect.mode = RangeHttpServer::Script::Mode::Redirect;
    redirect.redirectLocation = destination.packageUrl().toEncoded();
    server_.enqueue(redirect);
    StudioReleaseNotes notes;
    QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_TRUE(notes.url(version_).isEmpty());
    EXPECT_EQ(destination.requestCount(), 0);
}

TEST_F(StudioReleaseNotesTest, OversizedResponsesCannotPublishLinks)
{
    server_.setPayload(QByteArray(8 * 1024 * 1024, ' ') + QJsonDocument(document_).toJson(),
                       QByteArrayLiteral("oversized"));
    StudioReleaseNotes notes;
    QSignalSpy finished(&notes, &StudioReleaseNotes::finished);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_TRUE(notes.url(version_).isEmpty());
}

}
