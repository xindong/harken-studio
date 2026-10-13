#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Shared/RefIdMime.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMimeData>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

TEST(RefIdMimeTest, ProjectObjectRoundTrip)
{
    ensureApp();

    QMimeData mime;
    RefIdMime::write(&mime, RefDomain::ProjectObject, {3u, 0u, 7u});
    EXPECT_TRUE(RefIdMime::has(&mime, RefDomain::ProjectObject));
    EXPECT_EQ(RefIdMime::read(&mime, RefDomain::ProjectObject), (QList<uint32_t>{3u, 7u}));
    EXPECT_EQ(RefIdMime::readSingle(&mime, RefDomain::ProjectObject), 0u);
}

TEST(RefIdMimeTest, AudioAssetRoundTrip)
{
    ensureApp();

    QMimeData mime;
    RefIdMime::write(&mime, RefDomain::AudioAsset, {3u, 0u, 7u});
    EXPECT_TRUE(RefIdMime::has(&mime, RefDomain::AudioAsset));
    EXPECT_EQ(RefIdMime::read(&mime, RefDomain::AudioAsset), (QList<uint32_t>{3u, 7u}));
    EXPECT_EQ(RefIdMime::readSingle(&mime, RefDomain::AudioAsset), 0u);
}

TEST(RefIdMimeTest, CrossDomainRejected)
{
    ensureApp();

    QMimeData project;
    RefIdMime::write(&project, RefDomain::ProjectObject, {42u});
    EXPECT_TRUE(project.hasFormat(QLatin1String("application/x-harken-project-item")));
    EXPECT_TRUE(project.hasFormat(QLatin1String("application/x-harken-project-items")));
    EXPECT_FALSE(project.hasFormat(QLatin1String("application/x-harken-audio-asset")));
    EXPECT_FALSE(project.hasFormat(QLatin1String("application/x-harken-audio-assets")));
    EXPECT_FALSE(RefIdMime::has(&project, RefDomain::AudioAsset));
    EXPECT_TRUE(RefIdMime::read(&project, RefDomain::AudioAsset).isEmpty());
    EXPECT_EQ(RefIdMime::readSingle(&project, RefDomain::AudioAsset), 0u);

    QMimeData audio;
    RefIdMime::write(&audio, RefDomain::AudioAsset, {42u});
    EXPECT_TRUE(audio.hasFormat(QLatin1String("application/x-harken-audio-asset")));
    EXPECT_TRUE(audio.hasFormat(QLatin1String("application/x-harken-audio-assets")));
    EXPECT_FALSE(audio.hasFormat(QLatin1String("application/x-harken-project-item")));
    EXPECT_FALSE(audio.hasFormat(QLatin1String("application/x-harken-project-items")));
    EXPECT_FALSE(RefIdMime::has(&audio, RefDomain::ProjectObject));
    EXPECT_TRUE(RefIdMime::read(&audio, RefDomain::ProjectObject).isEmpty());
    EXPECT_EQ(RefIdMime::readSingle(&audio, RefDomain::ProjectObject), 0u);
}

TEST(RefIdMimeTest, ProjectItemMimeSharesProjectObjectBytes)
{
    ensureApp();

    QMimeData mime;
    ProjectItemMime::writeObjectIDs(&mime, {3u, 0u, 7u});
    EXPECT_TRUE(RefIdMime::has(&mime, RefDomain::ProjectObject));
    EXPECT_EQ(RefIdMime::read(&mime, RefDomain::ProjectObject), (QList<uint32_t>{3u, 7u}));
    EXPECT_EQ(RefIdMime::readSingle(&mime, RefDomain::ProjectObject), 0u);

    QMimeData single;
    ProjectItemMime::writeObjectIDs(&single, {42u});
    EXPECT_EQ(RefIdMime::readSingle(&single, RefDomain::ProjectObject), 42u);
}

TEST(RefIdMimeTest, BoundaryBehaviors)
{
    ensureApp();

    for (auto domain : {RefDomain::ProjectObject, RefDomain::AudioAsset}) {
        QMimeData empty;
        RefIdMime::write(&empty, domain, {0u});
        EXPECT_FALSE(RefIdMime::has(&empty, domain));
        EXPECT_TRUE(RefIdMime::read(&empty, domain).isEmpty());
        EXPECT_EQ(RefIdMime::readSingle(&empty, domain), 0u);
        EXPECT_EQ(RefIdMime::readSingle(nullptr, domain), 0u);

        QMimeData single;
        RefIdMime::write(&single, domain, {42u});
        EXPECT_EQ(RefIdMime::readSingle(&single, domain), 42u);
    }
}
