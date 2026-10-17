#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

using namespace VNX;
using namespace VNX::ProjectLanguageSemantics;

namespace {

constexpr int kNameLengthLimit = 32;

ProjectData projectWith(const QStringList& languages)
{
    ProjectData project;
    project.projectLanguages = languages;
    return project;
}

QString overlongName()
{
    return QString(kNameLengthLimit + 1, QLatin1Char('a'));
}

}

TEST(ProjectLanguageSemanticsTest, EmptyProjectBuildsEmpty)
{
    const ProjectData project;
    EXPECT_EQ(buildLanguages(project), QStringList());
}

TEST(ProjectLanguageSemanticsTest, BlankEntriesStillBuildEmpty)
{
    const auto project = projectWith({QStringLiteral("   "), QString()});
    EXPECT_EQ(buildLanguages(project), QStringList());
}

TEST(ProjectLanguageSemanticsTest, AuthoredLanguagesAreReturnedInOrder)
{
    const auto project = projectWith({QStringLiteral("English"), QStringLiteral("Japanese")});
    EXPECT_EQ(buildLanguages(project),
              QStringList({QStringLiteral("English"), QStringLiteral("Japanese")}));
}

TEST(ProjectLanguageSemanticsTest, AuthoredOrderIsPreserved)
{
    const auto project = projectWith({QStringLiteral("Japanese"), QStringLiteral("English")});
    EXPECT_EQ(buildLanguages(project),
              QStringList({QStringLiteral("Japanese"), QStringLiteral("English")}));
}

TEST(ProjectLanguageSemanticsTest, DiskSfxEntryIsDropped)
{
    const auto project = projectWith({QStringLiteral("English"), QStringLiteral("SFX")});
    EXPECT_EQ(buildLanguages(project), QStringList{QStringLiteral("English")});
}

TEST(ProjectLanguageSemanticsTest, DropSfxNamesLeavesOnlyRealLocales)
{
    EXPECT_EQ(dropSfxNames({QStringLiteral("SFX"), QStringLiteral("English"),
                            QStringLiteral("sfx"), QStringLiteral(" Japanese ")}),
              QStringList({QStringLiteral("English"), QStringLiteral("Japanese")}));
    EXPECT_TRUE(dropSfxNames({QStringLiteral("SFX"), QStringLiteral("sfx")}).isEmpty());
}

TEST(ProjectLanguageSemanticsTest, CaseVariantSfxIsDropped)
{
    const auto project = projectWith({QStringLiteral("sfx"), QStringLiteral("English")});
    EXPECT_EQ(buildLanguages(project), QStringList{QStringLiteral("English")});
}

TEST(ProjectLanguageSemanticsTest, OnlySfxBuildsEmpty)
{
    const auto project = projectWith({QStringLiteral("sfx")});
    EXPECT_EQ(buildLanguages(project), QStringList());
}

TEST(ProjectLanguageSemanticsTest, ExactDuplicatesAreDroppedOnTheBuildSide)
{
    const auto project = projectWith({QStringLiteral("English"), QStringLiteral("English")});
    EXPECT_EQ(buildLanguages(project), QStringList{QStringLiteral("English")});
}

TEST(ProjectLanguageSemanticsTest, BuildNamesAreTrimmed)
{
    const auto project = projectWith({QStringLiteral("  English  ")});
    EXPECT_EQ(buildLanguages(project), QStringList{QStringLiteral("English")});
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsEmptyList)
{
    for (const QStringList& names : {QStringList{}, QStringList{QStringLiteral("  "), QString()}}) {
        const auto result = normalize(names);
        EXPECT_FALSE(result.ok());
        EXPECT_TRUE(result.value.isEmpty());
    }
}

TEST(ProjectLanguageSemanticsTest, NormalizeTrimsSurroundingWhitespace)
{
    const auto result = normalize({QStringLiteral("  English  ")});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value, QStringList{QStringLiteral("English")});
}

TEST(ProjectLanguageSemanticsTest, NormalizeDropsBlankEntriesInsteadOfRejecting)
{
    const auto result = normalize({QStringLiteral("English"), QString(), QStringLiteral("  ")});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value, QStringList{QStringLiteral("English")});
}

TEST(ProjectLanguageSemanticsTest, NormalizeKeepsAuthoredOrder)
{
    const auto result = normalize({QStringLiteral("Japanese"), QStringLiteral("English")});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value,
              QStringList({QStringLiteral("Japanese"), QStringLiteral("English")}));
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsWhitespaceInsideNames)
{
    const auto result = normalize({QStringLiteral("Simplified Chinese")});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("Simplified Chinese")));
    EXPECT_TRUE(result.value.isEmpty());
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsPunctuation)
{
    const auto result = normalize({QStringLiteral("zh-CN")});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("zh-CN")));
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsNonAsciiNames)
{
    const auto result = normalize({QStringLiteral("Español")});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.value.isEmpty());
}

TEST(ProjectLanguageSemanticsTest, NormalizeAcceptsDigitsAndUnderscores)
{
    const auto result = normalize({QStringLiteral("en_US"), QStringLiteral("Voice2")});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value, QStringList({QStringLiteral("en_US"), QStringLiteral("Voice2")}));
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsOverlongName)
{
    const auto result = normalize({overlongName()});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.value.isEmpty());
}

TEST(ProjectLanguageSemanticsTest, NormalizeAcceptsNameAtTheLengthLimit)
{
    const QString name(kNameLengthLimit, QLatin1Char('a'));
    const auto result = normalize({name});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value, QStringList{name});
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsCaseInsensitiveDuplicate)
{
    const auto result = normalize({QStringLiteral("English"), QStringLiteral("english")});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("English")));
    EXPECT_TRUE(result.error.contains(QStringLiteral("english")));
    EXPECT_TRUE(result.value.isEmpty());
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsExactDuplicate)
{
    const auto result = normalize({QStringLiteral("English"), QStringLiteral("English")});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error.contains(QStringLiteral("English")));
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectsSfx)
{
    const auto sfx = normalize({QStringLiteral("SFX")});
    EXPECT_FALSE(sfx.ok());
    EXPECT_TRUE(sfx.error.contains(QStringLiteral("not a language")));
    EXPECT_TRUE(sfx.value.isEmpty());

    const auto lower = normalize({QStringLiteral("sfx")});
    EXPECT_FALSE(lower.ok());
    EXPECT_TRUE(lower.error.contains(QStringLiteral("not a language")));

    EXPECT_FALSE(normalize({QStringLiteral("English"), QStringLiteral("Sfx")}).ok());
}

TEST(ProjectLanguageSemanticsTest, NormalizeRejectionCarriesNoPartialValue)
{
    const auto result = normalize({QStringLiteral("English"), QStringLiteral("zh-CN"),
                                   QStringLiteral("Japanese")});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.value.isEmpty());
}
