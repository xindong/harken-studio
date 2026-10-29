#include "ReleaseNotes/VersionReleaseNotes.h"
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
using VNH::VersionReleaseNotes;

class VersionReleaseNotesTest : public ::testing::TestWithParam<VersionReleaseNotes::Product>
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

    QString productPath() const
    {
        return GetParam() == VersionReleaseNotes::Product::Studio
            ? QStringLiteral("studio") : QStringLiteral("unity");
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
        "schemaVersion": 2, "id": "harken-studio-1.2.3", "version": "1.2.3",
        "studioReleaseNotesPageId": "studio",
        "unityIntegrationReleaseNotesPageId": "unity",
        "pages": [
            {"id": "other", "locale": "en", "path": "document/other.html"},
            {"id": "studio", "locale": "en", "path": "document/1.2.3/en/studio/notes.html"},
            {"id": "studio", "locale": "zh", "path": "document/1.2.3/zh/studio/\u53d1\u5e03\u65e5\u5fd7.html"},
            {"id": "unity", "locale": "en", "path": "document/1.2.3/en/unity/notes.html"},
            {"id": "unity", "locale": "zh", "path": "document/1.2.3/zh/unity/\u53d1\u5e03\u65e5\u5fd7.html"}
        ]
    })").object();
};

TEST_P(VersionReleaseNotesTest, RejectsUnboundOrMalformedDocuments)
{
    QList<QByteArray> responses{QByteArrayLiteral("not JSON"), QByteArrayLiteral("[]")};
    const QJsonObject invalidValues{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), QStringLiteral("harken-studio-9.9.9")},
        {QStringLiteral("version"), QStringLiteral("9.9.9")},
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
        VersionReleaseNotes notes;
        QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
    }
}

TEST_P(VersionReleaseNotesTest, UntrustedPathsCannotRedirectOrTraverse)
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
            {QStringLiteral("id"), productPath()},
            {QStringLiteral("locale"), QStringLiteral("en")},
            {QStringLiteral("path"), path}}};
        setDocument(modified);
        VersionReleaseNotes notes;
        QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
    }
}

TEST_P(VersionReleaseNotesTest, SharedRequestsPreserveLocalizedTargets)
{
    VersionReleaseNotes notes;
    QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
    notes.fetch(version_);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_EQ(server_.requestCount(), 1);
    EXPECT_EQ(notes.url(version_, GetParam()), QUrl(QStringLiteral(
        "https://harkenstudio.xd.com/document/1.2.3/en/%1/notes.html").arg(productPath())));
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("zh_CN")));
    const QUrl chinese(QStringLiteral(
        "https://harkenstudio.xd.com/document/1.2.3/zh/%1/\u53d1\u5e03\u65e5\u5fd7.html").arg(productPath()));
    EXPECT_EQ(notes.url(version_, GetParam()), chinese);
    EXPECT_EQ(QUrl::fromEncoded(notes.url(version_, GetParam()).toEncoded()), chinese);
    notes.fetch(version_);
    EXPECT_FALSE(finished.wait(50));
    EXPECT_EQ(server_.requestCount(), 1);
}

TEST_P(VersionReleaseNotesTest, MissingLocaleDoesNotSelectAnotherLanguage)
{
    QJsonObject modified = document_;
    QJsonArray pages = modified.value(QStringLiteral("pages")).toArray();
    for (qsizetype i = pages.size() - 1; i >= 0; --i) {
        const QJsonObject page = pages[i].toObject();
        if (page.value(QStringLiteral("id")).toString() == productPath() &&
            page.value(QStringLiteral("locale")).toString() == QStringLiteral("zh"))
            pages.removeAt(i);
    }
    modified[QStringLiteral("pages")] = pages;
    setDocument(modified);
    VersionReleaseNotes notes;
    QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    const QUrl english = notes.url(version_, GetParam());
    ASSERT_FALSE(english.isEmpty());
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("zh_CN")));
    EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
    ASSERT_TRUE(HubSettings::setLanguagePreference(QStringLiteral("en")));
    EXPECT_EQ(notes.url(version_, GetParam()), english);
}

TEST_P(VersionReleaseNotesTest, FailedResponsesCanRecoverWithoutPublishingInvalidLinks)
{
    VersionReleaseNotes notes;
    QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
    for (const int status : {404, 503, 200}) {
        RangeHttpServer::Script failure;
        failure.mode = RangeHttpServer::Script::Mode::Status;
        failure.status = status;
        failure.responseBody = QByteArrayLiteral("not JSON");
        server_.enqueue(failure);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
    }
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_FALSE(notes.url(version_, GetParam()).isEmpty());
    EXPECT_EQ(server_.requestCount(), 4);
}

TEST_P(VersionReleaseNotesTest, RedirectsCannotSendRequestsToAnotherServer)
{
    RangeHttpServer destination;
    ASSERT_TRUE(destination.listen());
    RangeHttpServer::Script redirect;
    redirect.mode = RangeHttpServer::Script::Mode::Redirect;
    redirect.redirectLocation = destination.packageUrl().toEncoded();
    server_.enqueue(redirect);
    VersionReleaseNotes notes;
    QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
    EXPECT_EQ(destination.requestCount(), 0);
}

TEST_P(VersionReleaseNotesTest, OversizedResponsesCannotPublishLinks)
{
    server_.setPayload(QByteArray(8 * 1024 * 1024, ' ') + QJsonDocument(document_).toJson(),
                       QByteArrayLiteral("oversized"));
    VersionReleaseNotes notes;
    QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
    notes.fetch(version_);
    ASSERT_TRUE(finished.wait(3000));
    EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
}

TEST_P(VersionReleaseNotesTest, MissingTargetDoesNotDisableOrBorrowAnotherProduct)
{
    const auto otherProduct = GetParam() == VersionReleaseNotes::Product::Studio
        ? VersionReleaseNotes::Product::UnityIntegration : VersionReleaseNotes::Product::Studio;
    const QString field = GetParam() == VersionReleaseNotes::Product::Studio
        ? QStringLiteral("studioReleaseNotesPageId") : QStringLiteral("unityIntegrationReleaseNotesPageId");
    for (const QJsonValue invalid : {QJsonValue(), QJsonValue(42), QJsonValue(QStringLiteral("missing"))}) {
        QJsonObject modified = document_;
        modified[field] = invalid;
        setDocument(modified);
        VersionReleaseNotes notes;
        QSignalSpy finished(&notes, &VersionReleaseNotes::finished);
        notes.fetch(version_);
        ASSERT_TRUE(finished.wait(3000));
        EXPECT_TRUE(notes.url(version_, GetParam()).isEmpty());
        EXPECT_FALSE(notes.url(version_, otherProduct).isEmpty());
    }
}

INSTANTIATE_TEST_SUITE_P(Products, VersionReleaseNotesTest,
    ::testing::Values(VersionReleaseNotes::Product::Studio,
                      VersionReleaseNotes::Product::UnityIntegration));

}
