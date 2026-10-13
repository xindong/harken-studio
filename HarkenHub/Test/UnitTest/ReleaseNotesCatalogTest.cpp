#include "ReleaseNotes/ReleaseNotesCatalog.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QScopeGuard>
#include <QTranslator>

using VNH::ReleaseNotesCatalog;

TEST(ReleaseNotesCatalogTest, EveryPublishedChangeHasAChineseTranslation)
{
    QTranslator translator;
    ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
    QCoreApplication::installTranslator(&translator);
    const auto guard = qScopeGuard([&translator] {
        QCoreApplication::removeTranslator(&translator);
    });
    QFile catalog(QStringLiteral(":/Resources/release-notes.json"));
    ASSERT_TRUE(catalog.open(QIODevice::ReadOnly));
    QString error;
    const auto entries = ReleaseNotesCatalog::fromJson(catalog.readAll(), &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_FALSE(entries.isEmpty());
    for (const auto& entry : entries) {
        for (const auto& change : entry.changes) {
            EXPECT_NE(ReleaseNotesCatalog::localizedChange(change.text), change.text)
                << entry.version.toStdString() << ": " << change.text.toStdString();
        }
    }
}

TEST(ReleaseNotesCatalogTest, RejectsUnknownChangeCategories)
{
    const QByteArray json = R"({
        "schemaVersion": 1,
        "releases": [{
            "version": "0.1.0-dev",
            "date": "2026-08-14",
            "pullRequests": [],
            "contributors": ["contributor"],
            "changes": [{"category": "internal", "text": "Details"}]
        }]
    })";
    QString error;
    EXPECT_TRUE(ReleaseNotesCatalog::fromJson(json, &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ReleaseNotesCatalogTest, RejectsIncompleteEntries)
{
    const QByteArray json = R"({
        "schemaVersion": 1,
        "releases": [{
            "version": "0.1.0-dev",
            "date": "",
            "pullRequests": [],
            "contributors": ["contributor"],
            "changes": []
        }]
    })";
    QString error;
    EXPECT_TRUE(ReleaseNotesCatalog::fromJson(json, &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
}
