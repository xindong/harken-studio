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

TEST(RefIdMimeTest, CrossDomainRejected)
{
    ensureApp();

    QMimeData project;
    RefIdMime::write(&project, RefDomain::ProjectObject, {42u});
    ASSERT_TRUE(RefIdMime::has(&project, RefDomain::ProjectObject));
    EXPECT_FALSE(RefIdMime::has(&project, RefDomain::AudioAsset));
    EXPECT_TRUE(RefIdMime::read(&project, RefDomain::AudioAsset).isEmpty());

    QMimeData audio;
    RefIdMime::write(&audio, RefDomain::AudioAsset, {42u});
    ASSERT_TRUE(RefIdMime::has(&audio, RefDomain::AudioAsset));
    EXPECT_FALSE(RefIdMime::has(&audio, RefDomain::ProjectObject));
    EXPECT_TRUE(RefIdMime::read(&audio, RefDomain::ProjectObject).isEmpty());
}
