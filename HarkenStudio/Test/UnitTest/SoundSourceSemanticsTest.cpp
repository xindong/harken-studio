#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/Components/VSound.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/SoundSourceEditing.h"
#include "Core/SoundSourceSemantics.h"

#include <QJsonDocument>

#include <gtest/gtest.h>
#include "Support/ProjectPlatformTestUtils.h"

using namespace VNX;

namespace {

SoundSourceDef source(uint32_t sourceId)
{
    SoundSourceDef result;
    result.entryId = QUuid::createUuid();
    result.sourceId = sourceId;
    return result;
}

constexpr uint32_t kRate = 48000;

using Issue = SoundSourceSemantics::SourceIntegrityIssue;
using Issues = SoundSourceSemantics::SourceIntegrityIssues;

AssetInfo asset(uint32_t sourceId, uint64_t totalFrames, uint32_t sampleRate = kRate)
{
    AssetInfo result;
    result.sourceID = sourceId;
    result.absolutePath = QStringLiteral("audio.wav");
    result.metadata.sampleRate = sampleRate;
    result.metadata.totalSamples = totalFrames;
    result.metadata.durationSec = sampleRate == 0
        ? 0.0 : static_cast<double>(totalFrames) / sampleRate;
    return result;
}

AssetInfo loopingAsset(uint32_t sourceId, uint64_t totalFrames,
                       uint64_t loopStart, uint64_t loopEndInclusive)
{
    AssetInfo result = asset(sourceId, totalFrames);
    result.metadata.loopInfo.enabled = true;
    result.metadata.loopInfo.loopStart = loopStart;
    result.metadata.loopInfo.loopEnd = loopEndInclusive;
    return result;
}

}

TEST(SoundSourceSemanticsTest, SoundIsTheOnlyOwnerOfVSound)
{
    for (const ProjectItem::Type type : {ProjectItem::Type::Container,
                                         ProjectItem::Type::Track,
                                         ProjectItem::Type::Piece,
                                         ProjectItem::Type::Layer}) {
        ProjectItem item(QStringLiteral("Other"), type);
        item.attachDefaultComponents();
        EXPECT_EQ(item.comp<VSound>(), nullptr);
    }
    ProjectItem sound(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    sound.attachDefaultComponents();
    EXPECT_NE(sound.comp<VSound>(), nullptr);
}

TEST(SoundSourceSemanticsTest, PlatformsShareCandidatesAndResolveIndependentActiveOverrides)
{
    VSound sound;
    auto& set = sound.sources.plain;
    set.entries = {source(1), source(2)};
    set.active = set.entries[0].entryId;
    const auto windows = HarkenTest::platformIdString("Windows");
    SoundSourceSemantics::assignActive(set, set.entries[1].entryId, windows);
    EXPECT_EQ(SoundSourceSemantics::resolveSourceSelection(sound, windows, "English").source, &set.entries[1]);
    EXPECT_EQ(SoundSourceSemantics::resolveSourceSelection(sound, HarkenTest::platformIdString("Android"), {}).source,
              &set.entries[0]);
    EXPECT_EQ(set.entries.size(), 2u);
    SoundSourceSemantics::assignActive(set, set.entries[0].entryId);
    EXPECT_TRUE(set.platforms.isEmpty());
    EXPECT_EQ(set.active, set.entries[0].entryId);
}

TEST(SoundSourceSemanticsTest, LanguageModesAndEachLanguageResolveIndependently)
{
    VSound sound;
    sound.sources.plain.entries = {source(1)};
    sound.sources.plain.active = sound.sources.plain.entries[0].entryId;
    auto& english = sound.sources.localized["English"];
    english.entries = {source(2), source(3)};
    english.active = english.entries[0].entryId;
    english.platforms[HarkenTest::platformIdString("Windows")].active = english.entries[1].entryId;
    auto& japanese = sound.sources.localized["Japanese"];
    japanese.entries = {source(4)};
    japanese.active = japanese.entries[0].entryId;
    EXPECT_EQ(SoundSourceSemantics::resolveSourceSelection(sound, {}, "Japanese").source->sourceId, 1u);
    sound.distinguishLanguage = true;
    EXPECT_EQ(SoundSourceSemantics::resolveSourceSelection(sound, {}, "Japanese").source->sourceId, 4u);
    EXPECT_EQ(SoundSourceSemantics::resolveSourceSelection(sound, {}, "English").source->sourceId, 2u);
    EXPECT_EQ(SoundSourceSemantics::resolveSourceSelection(
        sound, HarkenTest::platformIdString("Windows"), "English").source->sourceId, 3u);
    EXPECT_FALSE(SoundSourceSemantics::resolveSourceSelection(sound, {}, {}));
    EXPECT_FALSE(SoundSourceSemantics::resolveSourceSelection(sound, {}, "French"));
}

TEST(SoundSourceSemanticsTest, ActiveCanBeEmptyAndNeverDefaultsToFirstCandidate)
{
    VSound sound;
    sound.sources.plain.entries = {source(1)};
    EXPECT_FALSE(SoundSourceSemantics::resolveSourceSelection(sound, {}, {}));
    sound.sources.plain.active = sound.sources.plain.entries[0].entryId;
    const auto platform = HarkenTest::platformIdString("Windows");
    sound.sources.plain.platforms[platform].active = QUuid{};
    EXPECT_FALSE(SoundSourceSemantics::resolveSourceSelection(sound, platform, {}));
    EXPECT_TRUE(SoundSourceSemantics::resolveSourceSelection(sound, {}, {}));
    VSound reloaded;
    ASSERT_TRUE(reloaded.fromJson(sound.toJson()));
    EXPECT_EQ(reloaded.sources, sound.sources);
}

TEST(SoundSourceSemanticsTest, AggregateRejectsCrossSetActiveAndDuplicateEntryIdentities)
{
    SoundSources sources;
    sources.plain.entries = {source(1)};
    sources.plain.active = sources.plain.entries[0].entryId;
    auto& localized = sources.localized["English"];
    localized.entries = {source(2)};
    localized.active = sources.plain.active;
    QString error;
    EXPECT_FALSE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources), &error));
    EXPECT_FALSE(error.isEmpty());
    localized.active = localized.entries[0].entryId;
    EXPECT_TRUE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources)));
    localized.entries = sources.plain.entries;
    localized.active = sources.plain.active;
    EXPECT_FALSE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources)));
}

TEST(SoundSourceSemanticsTest, AggregateRejectsInvalidPlatformsAndLanguageKeys)
{
    SoundSources sources;
    sources.plain.platforms["Windows"].active = QUuid{};
    EXPECT_FALSE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources)));
    sources.plain.platforms.clear();
    for (const QString& language : {QString(), QStringLiteral("SFX"), QStringLiteral(" English ")}) {
        sources.localized.clear();
        sources.localized[language] = {};
        EXPECT_FALSE(SoundSourceSemantics::fromJson(SoundSourceSemantics::toJson(sources)));
    }
}

TEST(SoundSourceSemanticsTest, RemovalClearsOnlyReferencesInTheOwningSet)
{
    SoundSources sources;
    auto& plain = sources.plain;
    plain.entries = {source(1), source(2)};
    plain.active = plain.entries[0].entryId;
    plain.platforms[HarkenTest::platformIdString("Windows")].active = plain.entries[1].entryId;
    auto& localized = sources.localized["English"];
    localized.entries = {source(3)};
    localized.active = localized.entries[0].entryId;
    const auto dormant = localized;
    const auto removed = plain.entries[1].entryId;
    SoundSourceSemantics::removeEntry(sources, removed);
    ASSERT_EQ(plain.entries.size(), 1u);
    EXPECT_TRUE(plain.platforms.isEmpty());
    EXPECT_EQ(localized, dormant);
}

TEST(SoundSourceSemanticsTest, MovingLanguagePreservesEntryRangesAndRepairsActiveConflicts)
{
    SoundSources sources;
    auto& english = sources.localized["English"];
    english.entries = {source(1)};
    english.entries[0].trimStartFrame = 19;
    english.active = english.entries[0].entryId;
    const auto moved = english.entries[0];
    auto& japanese = sources.localized["Japanese"];
    japanese.entries = {source(2)};
    japanese.active = japanese.entries[0].entryId;
    const auto retainedActive = japanese.active;
    sources.localized["German"].platforms[HarkenTest::platformIdString("Windows")].active = QUuid{};
    const auto emptyGerman = sources.localized["German"];
    SoundSourceSemantics::moveToLanguage(sources, {moved.entryId}, "Japanese");
    EXPECT_FALSE(sources.localized.contains("English"));
    ASSERT_EQ(japanese.entries.size(), 2u);
    EXPECT_EQ(japanese.entries.back(), moved);
    EXPECT_EQ(japanese.active, retainedActive);
    EXPECT_EQ(sources.localized["German"], emptyGerman);
}

TEST(SoundSourceSemanticsTest, CanonicalRangeClampsTrimAndLoop)
{
    SoundSourceDef value = source(1);
    value.trimEndFrame = 20 * kRate;
    value.loopRegion = SoundLoopRegion{0, 30 * kRate};
    const SoundSourceDef canonical = SoundSourceSemantics::canonicalized(value, 10 * kRate);
    EXPECT_EQ(canonical.trimStartFrame, 0u);
    ASSERT_TRUE(canonical.trimEndFrame);
    EXPECT_EQ(*canonical.trimEndFrame, 10u * kRate);
    ASSERT_TRUE(canonical.loopRegion);
    EXPECT_EQ(canonical.loopRegion->startFrame, 0u);
    EXPECT_EQ(canonical.loopRegion->endFrame, 10u * kRate);
}

TEST(SoundSourceSemanticsTest, LoopRegionClampsIntoTheTrimWindow)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 8 * kRate;

    value.loopRegion = SoundLoopRegion{kRate, 30 * kRate};
    SoundSourceDef canonical = SoundSourceSemantics::canonicalized(value, 10 * kRate);
    ASSERT_TRUE(canonical.loopRegion);
    EXPECT_EQ(canonical.loopRegion->startFrame, 2u * kRate);
    EXPECT_EQ(canonical.loopRegion->endFrame, 8u * kRate);

    value.loopRegion = SoundLoopRegion{9 * kRate, 10 * kRate};
    canonical = SoundSourceSemantics::canonicalized(value, 10 * kRate);
    EXPECT_FALSE(canonical.loopRegion);

    value.trimStartFrame = 12 * kRate;
    value.trimEndFrame.reset();
    value.loopRegion = SoundLoopRegion{kRate, 2 * kRate};
    canonical = SoundSourceSemantics::canonicalized(value, 10 * kRate);
    EXPECT_EQ(canonical.trimStartFrame, 10u * kRate);
    ASSERT_TRUE(canonical.trimEndFrame);
    EXPECT_EQ(*canonical.trimEndFrame, 10u * kRate);
    EXPECT_FALSE(canonical.loopRegion);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsNothingWhenTheAuthoringFits)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 8 * kRate;
    value.loopRegion = SoundLoopRegion{3 * kRate, 5 * kRate};
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_TRUE(range->intact());
    EXPECT_EQ(range->issues.toInt(), Issues{}.toInt());
    EXPECT_EQ(range->trimStartFrame, 2u * kRate);
    EXPECT_EQ(range->trimEndFrame, 8u * kRate);
    ASSERT_TRUE(range->loopRegion);
    EXPECT_EQ(*range->loopRegion, (SoundLoopRegion{3 * kRate, 5 * kRate}));
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsTrimEndPastTheFileEnd)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 20 * kRate;
    const SoundSourceDef before = value;
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(), Issues(Issue::TrimEndPastFileEnd).toInt());
    EXPECT_EQ(range->trimStartFrame, 2u * kRate);
    EXPECT_EQ(range->trimEndFrame, 10u * kRate);
    EXPECT_EQ(value, before);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsTrimStartPastTheFileEnd)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 12 * kRate;
    const SoundSourceDef before = value;
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(),
              (Issue::TrimStartPastFileEnd | Issue::TrimWindowEmpty).toInt());
    EXPECT_EQ(range->trimStartFrame, 10u * kRate);
    EXPECT_EQ(range->trimEndFrame, 10u * kRate);
    EXPECT_EQ(value, before);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsAnEmptyTrimWindow)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 6 * kRate;
    value.trimEndFrame = 3 * kRate;
    const SoundSourceDef before = value;
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(), Issues(Issue::TrimWindowEmpty).toInt());
    EXPECT_EQ(range->trimStartFrame, 6u * kRate);
    EXPECT_EQ(range->trimEndFrame, 6u * kRate);
    EXPECT_EQ(value, before);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsALoopClampedIntoTheTrimWindow)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 8 * kRate;
    value.loopRegion = SoundLoopRegion{kRate, 30 * kRate};
    const SoundSourceDef before = value;
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(), Issues(Issue::LoopClampedIntoTrimWindow).toInt());
    ASSERT_TRUE(range->loopRegion);
    EXPECT_EQ(*range->loopRegion, (SoundLoopRegion{2 * kRate, 8 * kRate}));
    EXPECT_EQ(value, before);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsALoopDroppedByClamping)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 8 * kRate;
    value.loopRegion = SoundLoopRegion{9 * kRate, 10 * kRate};
    const SoundSourceDef before = value;
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(), Issues(Issue::LoopDroppedAsDegenerate).toInt());
    EXPECT_FALSE(range->loopRegion);
    EXPECT_EQ(range->trimStartFrame, 2u * kRate);
    EXPECT_EQ(range->trimEndFrame, 8u * kRate);
    EXPECT_EQ(value, before);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsTrimAndLoopBreakageTogether)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 20 * kRate;
    value.loopRegion = SoundLoopRegion{kRate, 30 * kRate};
    const SoundSourceDef before = value;
    const AssetInfo info = asset(1, 10 * kRate);

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_FALSE(range->intact());
    EXPECT_EQ(range->issues.toInt(),
              (Issue::TrimEndPastFileEnd | Issue::LoopClampedIntoTrimWindow).toInt());
    EXPECT_EQ(range->trimEndFrame, 10u * kRate);
    ASSERT_TRUE(range->loopRegion);
    EXPECT_EQ(*range->loopRegion, (SoundLoopRegion{2 * kRate, 10 * kRate}));
}

TEST(SoundSourceSemanticsTest, IntegrityTextReportsTrimEndPastFileEnd)
{
    SoundSourceDef value = source(1);
    value.trimEndFrame = 20 * kRate;
    const AssetInfo info = asset(1, 10 * kRate);
    const QString text = SoundSourceSemantics::integrityText(value, &info);
    EXPECT_TRUE(text.contains(QStringLiteral("Trim end is past the end of the source file.")));
    EXPECT_TRUE(text.startsWith(QStringLiteral("Source integrity:")));
}

TEST(SoundSourceSemanticsTest, FullDurationTrimIsNotADeepRuntimeSemantic)
{
    VSound sound;
    sound.sources.plain.entries = {source(7)};
    sound.sources.plain.entries.front().trimEndFrame = 10 * kRate;
    const AssetInfo info = asset(7, 10 * kRate);
    const auto resolve = [&info](uint32_t) { return &info; };

    EXPECT_FALSE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
    sound.sources.plain.entries.front().trimEndFrame = 10 * kRate - 1;
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound));
}

TEST(SoundSourceSemanticsTest, LedgerRejectsDuplicateIdentityAndBadNumbers)
{
    const QUuid id = QUuid::createUuid();
    const QJsonObject row{
        {QStringLiteral("entryId"), id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("sourceId"), 1}};
    EXPECT_FALSE(SoundSourceSemantics::entriesFromJson(QJsonArray{row, row}));

    QJsonObject bad = row;
    bad[QStringLiteral("trimStartFrame")] = -1.0;
    EXPECT_FALSE(SoundSourceSemantics::entriesFromJson(QJsonArray{bad}));
}

TEST(SoundSourceSemanticsTest, LedgerRejectsFrameValuesThatAreNotWholeSamples)
{
    const QJsonObject row{
        {QStringLiteral("entryId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("sourceId"), 1}};
    const auto rejects = [&row](const QString& key, const QJsonValue& value) {
        QJsonObject bad = row;
        bad[key] = value;
        return !SoundSourceSemantics::entriesFromJson(QJsonArray{bad}).has_value();
    };

    EXPECT_TRUE(rejects(QStringLiteral("trimStartFrame"), 12000.5));
    EXPECT_TRUE(rejects(QStringLiteral("trimStartFrame"), -1));
    EXPECT_TRUE(rejects(QStringLiteral("trimStartFrame"),
                        QStringLiteral("12000")));
    EXPECT_TRUE(rejects(QStringLiteral("trimStartFrame"), 1.0e18));
    EXPECT_TRUE(rejects(QStringLiteral("trimEndFrame"), 4.25));
    EXPECT_TRUE(rejects(QStringLiteral("trimEndFrame"), -2));
    EXPECT_TRUE(rejects(QStringLiteral("loopRegion"), QJsonObject{
        {QStringLiteral("startFrame"), 10.5}, {QStringLiteral("endFrame"), 20}}));
    EXPECT_TRUE(rejects(QStringLiteral("loopRegion"), QJsonObject{
        {QStringLiteral("startFrame"), -5}, {QStringLiteral("endFrame"), 20}}));
    EXPECT_TRUE(rejects(QStringLiteral("loopRegion"), QJsonObject{
        {QStringLiteral("startFrame"), 20}, {QStringLiteral("endFrame"), 20}}));
    EXPECT_TRUE(rejects(QStringLiteral("loopRegion"), QJsonObject{
        {QStringLiteral("startFrame"), 20}}));

    QJsonObject good = row;
    good[QStringLiteral("trimStartFrame")] = 12000;
    good[QStringLiteral("trimEndFrame")] = 36000;
    good[QStringLiteral("loopRegion")] = QJsonObject{
        {QStringLiteral("startFrame"), 18000}, {QStringLiteral("endFrame"), 30000}};
    const auto parsed = SoundSourceSemantics::entriesFromJson(QJsonArray{good});
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_EQ(parsed->front().trimStartFrame, 12000u);
    ASSERT_TRUE(parsed->front().trimEndFrame);
    EXPECT_EQ(*parsed->front().trimEndFrame, 36000u);
    ASSERT_TRUE(parsed->front().loopRegion);
    EXPECT_EQ(parsed->front().loopRegion->startFrame, 18000u);
    EXPECT_EQ(parsed->front().loopRegion->endFrame, 30000u);
}

TEST(SoundSourceSemanticsTest, FrameLedgerRoundTripsSampleExact)
{
    std::vector<SoundSourceDef> sources{source(1), source(2)};
    sources[0].trimStartFrame = 1;
    sources[0].trimEndFrame = 4294967297ull;
    sources[0].loopRegion = SoundLoopRegion{2147483649ull, 4294967296ull};
    sources[1].trimStartFrame = 4409;
    sources[1].trimEndFrame = 4410;

    const auto reloaded = SoundSourceSemantics::entriesFromJson(
        SoundSourceSemantics::entriesToJson(sources));
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->size(), sources.size());
    for (size_t index = 0; index < sources.size(); ++index) {
        EXPECT_EQ((*reloaded)[index].trimStartFrame, sources[index].trimStartFrame);
        ASSERT_TRUE((*reloaded)[index].trimEndFrame);
        EXPECT_EQ(*(*reloaded)[index].trimEndFrame, *sources[index].trimEndFrame);
        EXPECT_EQ((*reloaded)[index].loopRegion, sources[index].loopRegion);
    }
    EXPECT_EQ(*reloaded, sources);
}

TEST(SoundSourceSemanticsTest, FrameLedgerSurvivesTheSerializedDocument)
{
    std::vector<SoundSourceDef> sources{source(1)};
    sources[0].trimStartFrame = 123456789;
    sources[0].trimEndFrame = 987654321;
    sources[0].loopRegion = SoundLoopRegion{234567890, 876543210};

    const QJsonDocument document(QJsonObject{
        {QStringLiteral("sources"), SoundSourceSemantics::entriesToJson(sources)}});
    const QJsonArray parsed = QJsonDocument::fromJson(document.toJson(QJsonDocument::Compact))
                                  .object()
                                  .value(QStringLiteral("sources"))
                                  .toArray();
    const auto reloaded = SoundSourceSemantics::entriesFromJson(parsed);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(*reloaded, sources);
}

TEST(SoundSourceSemanticsTest, BothModesRoundTripWithoutDependingOnLanguageSwitch)
{
    VSound sound;
    sound.sources.plain.entries = {source(11), source(12)};
    sound.sources.plain.entries[0].trimStartFrame = 12001;
    sound.sources.plain.entries[0].trimEndFrame = 480001;
    sound.sources.plain.active = sound.sources.plain.entries[0].entryId;
    sound.sources.plain.platforms[HarkenTest::platformIdString("Windows")].active =
        sound.sources.plain.entries[1].entryId;
    auto& localized = sound.sources.localized["English"];
    localized.entries = {source(13)};
    localized.entries[0].loopRegion = SoundLoopRegion{7, 480000};
    localized.active = localized.entries[0].entryId;
    sound.scheduleOffsetSec = 0.125;
    for (bool language : {false, true}) {
        sound.distinguishLanguage = language;
        VSound reloaded;
        ASSERT_TRUE(reloaded.fromJson(QJsonDocument::fromJson(
            QJsonDocument(sound.toJson()).toJson()).object()));
        EXPECT_EQ(reloaded.sources, sound.sources);
        EXPECT_EQ(reloaded.distinguishLanguage, language);
        EXPECT_EQ(reloaded.scheduleOffsetSec, sound.scheduleOffsetSec);
        EXPECT_FALSE(reloaded.toJson().contains("sourceSelections"));
    }
}

TEST(SoundSourceSemanticsTest, SerializerDoesNotPersistOrReviveActiveProjections)
{
    ProjectItem item("Sound", ProjectItem::Type::Sound);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    item.sourceID = 42;
    item.sourcePath = "stale.wav";
    ProjectSerializer serializer;
    const auto json = serializer.projectItemToJson(item);
    EXPECT_FALSE(json.contains("sourceID"));
    EXPECT_FALSE(json.contains("path"));
    EXPECT_FALSE(json.contains("sourcePath"));
    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    EXPECT_EQ(restored.sourceID, 0u);
    EXPECT_TRUE(restored.sourcePath.isEmpty());
    EXPECT_TRUE(restored.comp<VSound>()->sources.plain.entries.empty());
    EXPECT_EQ(SoundSourceEditing::activeSourceId(restored, {}, {}), 0u);
}

TEST(SoundSourceSemanticsTest, CloneRemintsBothBanksAndTheirActiveReferences)
{
    VSound sound;
    sound.sources.plain.entries = {source(1), source(2)};
    sound.sources.plain.active = sound.sources.plain.entries[0].entryId;
    sound.sources.plain.platforms[HarkenTest::platformIdString("Windows")].active =
        sound.sources.plain.entries[1].entryId;
    auto& localized = sound.sources.localized["English"];
    localized.entries = {source(3)};
    localized.active = localized.entries[0].entryId;
    const auto previous = sound.sources;
    SoundSourceSemantics::remintEntries(sound);
    EXPECT_NE(sound.sources.plain.active, previous.plain.active);
    EXPECT_EQ(sound.sources.plain.active, sound.sources.plain.entries[0].entryId);
    EXPECT_EQ(sound.sources.plain.platforms.first().active, sound.sources.plain.entries[1].entryId);
    const auto& reminted = sound.sources.localized["English"];
    EXPECT_NE(reminted.active, previous.localized["English"].active);
    EXPECT_EQ(reminted.active, reminted.entries[0].entryId);
    EXPECT_EQ(reminted.entries[0].sourceId, 3u);
}

TEST(SoundSourceSemanticsTest, AuthoringAnchorRoundTripsAndIsOmittedWhenUnmarked)
{
    std::vector<SoundSourceDef> sources{source(1), source(2)};
    sources[0].trimStartFrame = 4409;
    sources[0].rangeContentHash = QStringLiteral("6f1ed002ab5595859014ebf0951522d9");

    const QJsonArray json = SoundSourceSemantics::entriesToJson(sources);
    ASSERT_EQ(json.size(), 2);
    EXPECT_EQ(json.at(0).toObject().value(QStringLiteral("rangeContentHash")).toString(),
              sources[0].rangeContentHash);
    EXPECT_FALSE(json.at(1).toObject().contains(QStringLiteral("rangeContentHash")));

    const auto reloaded = SoundSourceSemantics::entriesFromJson(json);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(*reloaded, sources);
    EXPECT_TRUE((*reloaded)[1].rangeContentHash.isEmpty());
}

TEST(SoundSourceSemanticsTest, LedgerRejectsAMalformedAuthoringAnchor)
{
    const QJsonObject row{
        {QStringLiteral("entryId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("sourceId"), 1}};
    const auto rejects = [&row](const QJsonValue& value) {
        QJsonObject bad = row;
        bad[QStringLiteral("rangeContentHash")] = value;
        return !SoundSourceSemantics::entriesFromJson(QJsonArray{bad}).has_value();
    };

    EXPECT_TRUE(rejects(QJsonValue{}));
    EXPECT_TRUE(rejects(QStringLiteral("")));
    EXPECT_TRUE(rejects(12345));
    EXPECT_TRUE(rejects(QJsonObject{}));
    EXPECT_FALSE(rejects(QStringLiteral("abc123")));
}

TEST(SoundSourceSemanticsTest, PcmSliceUsesTrimWindowWhenLoopIsOff)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 100;
    value.trimEndFrame = 400;
    AssetInfo info = asset(1, 1000);
    info.metadata.channels = 2;

    const auto slice = SoundSourceSemantics::pcmSlice(value, &info);
    ASSERT_TRUE(slice);
    EXPECT_EQ(slice->startFrame, 100u);
    EXPECT_EQ(slice->frameCount, 300u);
    EXPECT_EQ(slice->windowStartFrame, 100u);
    EXPECT_EQ(slice->windowFrameCount, 300u);
    EXPECT_FALSE(slice->loops);
    EXPECT_EQ(slice->channels, 2u);
    EXPECT_EQ(slice->sampleRate, kRate);
    EXPECT_EQ(slice->sampleOffset(), 200u);
}

TEST(SoundSourceSemanticsTest, PcmSliceUsesLoopRegionAndCanStartLater)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 50;
    value.trimEndFrame = 500;
    value.loopRegion = SoundLoopRegion{120, 360};
    AssetInfo info = asset(1, 1000);
    info.metadata.channels = 1;

    const auto looped = SoundSourceSemantics::pcmSlice(value, &info);
    ASSERT_TRUE(looped);
    EXPECT_EQ(looped->startFrame, 120u);
    EXPECT_EQ(looped->frameCount, 240u);
    EXPECT_EQ(looped->windowStartFrame, 120u);
    EXPECT_EQ(looped->windowFrameCount, 240u);
    EXPECT_TRUE(looped->loops);

    const auto fromLater = SoundSourceSemantics::pcmSlice(value, &info, 200);
    ASSERT_TRUE(fromLater);
    EXPECT_EQ(fromLater->startFrame, 200u);
    EXPECT_EQ(fromLater->frameCount, 240u);
    EXPECT_EQ(fromLater->windowStartFrame, 120u);
    EXPECT_EQ(fromLater->windowFrameCount, 240u);
    EXPECT_TRUE(fromLater->loops);
    EXPECT_EQ(fromLater->sampleOffset(), 200u);

    EXPECT_FALSE(SoundSourceSemantics::pcmSlice(value, &info, 360));
}

TEST(SoundSourceSemanticsTest, PlayheadExtrapolationAdvancesFromStartAndStopsAtWindowEnd)
{
    SoundSourceSemantics::PlayheadExtrapolation state;
    state.startFrame = 100;
    state.windowStartFrame = 100;
    state.windowFrameCount = 300;
    state.sampleRate = 1000;
    state.playing = true;

    EXPECT_EQ(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 0), 100u);
    EXPECT_EQ(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 150'000'000), 250u);
    EXPECT_FALSE(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 300'000'000));
    EXPECT_FALSE(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 400'000'000));

    state.playing = false;
    EXPECT_FALSE(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 0));
}

TEST(SoundSourceSemanticsTest, PlayheadExtrapolationWrapsInsideALoopWindow)
{
    SoundSourceSemantics::PlayheadExtrapolation state;
    state.startFrame = 200;
    state.windowStartFrame = 120;
    state.windowFrameCount = 240;
    state.sampleRate = 1000;
    state.loops = true;
    state.playing = true;

    EXPECT_EQ(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 0), 200u);
    EXPECT_EQ(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 160'000'000), 120u);
    EXPECT_EQ(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 240'000'000), 200u);
    EXPECT_FALSE(SoundSourceSemantics::extrapolatedPlayheadFrame(state, -1));

    state.startFrame = 360;
    EXPECT_FALSE(SoundSourceSemantics::extrapolatedPlayheadFrame(state, 0));
}

TEST(SoundSourceSemanticsTest, PcmSliceRejectsEmptyOrUnidentifiedAudio)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 10;
    value.trimEndFrame = 10;
    AssetInfo info = asset(1, 100);
    EXPECT_FALSE(SoundSourceSemantics::pcmSlice(value, &info));

    value.trimEndFrame = 40;
    info.metadata.channels = 0;
    EXPECT_FALSE(SoundSourceSemantics::pcmSlice(value, &info));
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsAReplacedFileWithoutAnyClamping)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 8 * kRate;
    value.loopRegion = SoundLoopRegion{3 * kRate, 5 * kRate};
    value.rangeContentHash = QStringLiteral("authored");
    const SoundSourceDef before = value;
    AssetInfo info = asset(1, 10 * kRate);
    info.contentHash = QStringLiteral("replaced");

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_FALSE(range->intact());
    EXPECT_EQ(range->issues.toInt(),
              Issues(Issue::SourceFileChangedSinceAuthoring).toInt());
    EXPECT_EQ(range->trimStartFrame, 2u * kRate);
    EXPECT_EQ(range->trimEndFrame, 8u * kRate);
    ASSERT_TRUE(range->loopRegion);
    EXPECT_EQ(*range->loopRegion, (SoundLoopRegion{3 * kRate, 5 * kRate}));
    EXPECT_EQ(value, before);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeReportsAReplacedFileAlongsideItsClamping)
{
    SoundSourceDef value = source(1);
    value.trimStartFrame = 2 * kRate;
    value.trimEndFrame = 20 * kRate;
    value.rangeContentHash = QStringLiteral("authored");
    AssetInfo info = asset(1, 10 * kRate);
    info.contentHash = QStringLiteral("replaced");

    const auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(),
              (Issue::SourceFileChangedSinceAuthoring | Issue::TrimEndPastFileEnd).toInt());
    EXPECT_EQ(range->trimEndFrame, 10u * kRate);
}

TEST(SoundSourceSemanticsTest, EffectiveRangeTrustsAMatchingOrUnmarkedSource)
{
    SoundSourceDef value = source(1);
    value.trimEndFrame = 20 * kRate;
    AssetInfo info = asset(1, 10 * kRate);
    info.contentHash = QStringLiteral("current");

    auto range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(), Issues(Issue::TrimEndPastFileEnd).toInt());

    value.rangeContentHash = QStringLiteral("current");
    range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->issues.toInt(), Issues(Issue::TrimEndPastFileEnd).toInt());

    value.trimEndFrame = 4 * kRate;
    range = SoundSourceSemantics::effectiveRange(value, &info);
    ASSERT_TRUE(range);
    EXPECT_TRUE(range->intact());
}

TEST(SoundSourceSemanticsTest, RangeContentHashAnchorsAtBirthUntilTheRangeIsReauthored)
{
    AssetInfo first = asset(1, 10 * kRate);
    first.contentHash = QStringLiteral("hash-one");
    AssetInfo second = asset(2, 10 * kRate);
    second.contentHash = QStringLiteral("hash-two");
    const auto resolve = [&first, &second](uint32_t sourceId) -> const AssetInfo* {
        return sourceId == first.sourceID ? &first : &second;
    };

    std::vector<SoundSourceDef> rows{source(1)};
    SoundSourceSemantics::stampAuthoringAnchors(rows, {}, resolve);
    EXPECT_EQ(rows[0].rangeContentHash, QStringLiteral("hash-one"));

    const std::vector<SoundSourceDef> previous = rows;
    rows[0].sourceId = second.sourceID;
    SoundSourceSemantics::stampAuthoringAnchors(rows, previous, resolve);
    EXPECT_EQ(rows[0].rangeContentHash, QStringLiteral("hash-one"));

    second.contentHash = QStringLiteral("hash-two-replaced");
    SoundSourceSemantics::stampAuthoringAnchors(rows, rows, resolve);
    EXPECT_EQ(rows[0].rangeContentHash, QStringLiteral("hash-one"));
}
TEST(SoundSourceSemanticsTest, AuthoringAnchorsFollowEditedRangesOnly)
{
    AssetInfo first = asset(1, 10 * kRate);
    first.contentHash = QStringLiteral("hash-one");
    AssetInfo second = asset(2, 10 * kRate);
    second.contentHash = QStringLiteral("hash-two");
    const auto resolve = [&first, &second](uint32_t sourceId) -> const AssetInfo* {
        return sourceId == first.sourceID ? &first : &second;
    };

    std::vector<SoundSourceDef> previous{source(1), source(2)};
    std::vector<SoundSourceDef> edited = previous;
    edited[0].trimEndFrame = 4 * kRate;
    SoundSourceSemantics::stampAuthoringAnchors(edited, previous, resolve);
    EXPECT_EQ(edited[0].rangeContentHash, QStringLiteral("hash-one"));
    EXPECT_TRUE(edited[1].rangeContentHash.isEmpty());

    previous = edited;
    std::vector<SoundSourceDef> untouched{previous[1], previous[0]};
    SoundSourceSemantics::stampAuthoringAnchors(untouched, previous, resolve);
    EXPECT_TRUE(untouched[0].rangeContentHash.isEmpty());
    EXPECT_EQ(untouched[1].rangeContentHash, QStringLiteral("hash-one"));

    std::vector<SoundSourceDef> looped = previous;
    looped[1].loopRegion = SoundLoopRegion{kRate, 2 * kRate};
    SoundSourceSemantics::stampAuthoringAnchors(looped, previous, resolve);
    EXPECT_EQ(looped[1].rangeContentHash, QStringLiteral("hash-two"));

    std::vector<SoundSourceDef> grown = previous;
    grown.push_back(source(2));
    grown.push_back(source(2));
    grown.back().trimStartFrame = kRate;
    SoundSourceSemantics::stampAuthoringAnchors(grown, previous, resolve);
    EXPECT_EQ(grown[2].rangeContentHash, QStringLiteral("hash-two"));
    EXPECT_EQ(grown[3].rangeContentHash, QStringLiteral("hash-two"));
}

TEST(SoundSourceSemanticsTest, FileLoopRegionConvertsTheInclusiveSmplEnd)
{
    EXPECT_FALSE(SoundSourceSemantics::fileLoopRegion(nullptr));

    AssetInfo info = asset(1, 10 * kRate);
    EXPECT_FALSE(SoundSourceSemantics::fileLoopRegion(&info));

    info = loopingAsset(1, 10 * kRate, 2 * kRate, 5 * kRate - 1);
    auto region = SoundSourceSemantics::fileLoopRegion(&info);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->startFrame, 2u * kRate);
    EXPECT_EQ(region->endFrame, 5u * kRate);

    info = loopingAsset(1, 10 * kRate, 4409, 4409);
    region = SoundSourceSemantics::fileLoopRegion(&info);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->startFrame, 4409u);
    EXPECT_EQ(region->endFrame, 4410u);

    info = loopingAsset(1, 10 * kRate, 0, 10 * kRate - 1);
    region = SoundSourceSemantics::fileLoopRegion(&info);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->endFrame, 10u * kRate);

    info = loopingAsset(1, 10 * kRate, 0, 10 * kRate);
    EXPECT_FALSE(SoundSourceSemantics::fileLoopRegion(&info));

    info = loopingAsset(1, 10 * kRate, 5 * kRate, 2 * kRate);
    EXPECT_FALSE(SoundSourceSemantics::fileLoopRegion(&info));
}

TEST(SoundSourceSemanticsTest, LoopProvenanceRoundTripsAndIsOmittedWhenAuthored)
{
    std::vector<SoundSourceDef> sources{source(1), source(2), source(3)};
    sources[0].loopRegion = SoundLoopRegion{100, 200};
    sources[0].loopProvenance = SoundLoopProvenance::File;
    sources[1].loopRegion = SoundLoopRegion{300, 400};
    sources[2].loopProvenance = SoundLoopProvenance::File;

    const QJsonArray json = SoundSourceSemantics::entriesToJson(sources);
    ASSERT_EQ(json.size(), 3);
    EXPECT_EQ(json.at(0).toObject().value(QStringLiteral("loopProvenance")).toString(),
              QStringLiteral("file"));
    EXPECT_FALSE(json.at(1).toObject().contains(QStringLiteral("loopProvenance")));
    EXPECT_FALSE(json.at(2).toObject().contains(QStringLiteral("loopProvenance")));

    const auto reloaded = SoundSourceSemantics::entriesFromJson(json);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ((*reloaded)[0].loopProvenance, SoundLoopProvenance::File);
    EXPECT_EQ((*reloaded)[1].loopProvenance, SoundLoopProvenance::Authored);
    EXPECT_EQ((*reloaded)[2].loopProvenance, SoundLoopProvenance::Authored);
}

TEST(SoundSourceSemanticsTest, LedgerRejectsAMalformedLoopProvenance)
{
    QJsonObject row{
        {QStringLiteral("entryId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("sourceId"), 1}};
    QJsonObject looped = row;
    looped[QStringLiteral("loopRegion")] = QJsonObject{
        {QStringLiteral("startFrame"), 100}, {QStringLiteral("endFrame"), 200}};
    const auto rejects = [&looped](const QJsonValue& value) {
        QJsonObject bad = looped;
        bad[QStringLiteral("loopProvenance")] = value;
        return !SoundSourceSemantics::entriesFromJson(QJsonArray{bad}).has_value();
    };

    EXPECT_TRUE(rejects(QStringLiteral("seeded")));
    EXPECT_TRUE(rejects(QStringLiteral("")));
    EXPECT_TRUE(rejects(1));
    EXPECT_TRUE(rejects(QJsonObject{}));
    EXPECT_FALSE(rejects(QStringLiteral("file")));
    EXPECT_FALSE(rejects(QStringLiteral("authored")));

    QJsonObject unlooped = row;
    unlooped[QStringLiteral("loopProvenance")] = QStringLiteral("file");
    EXPECT_FALSE(SoundSourceSemantics::entriesFromJson(QJsonArray{unlooped}));
}

TEST(SoundSourceSemanticsTest, AFileSeededLoopIsNotADeepRuntimeSemantic)
{
    const AssetInfo info = loopingAsset(7, 10 * kRate, 2 * kRate, 5 * kRate - 1);
    const auto resolve = [&info](uint32_t) { return &info; };

    VSound sound;
    sound.sources.plain.entries = {source(7)};
    SoundSourceDef& row = sound.sources.plain.entries.front();
    row.loopRegion = SoundLoopRegion{2 * kRate, 5 * kRate};
    row.loopProvenance = SoundLoopProvenance::File;
    EXPECT_FALSE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound));

    row.loopProvenance = SoundLoopProvenance::Authored;
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));

    row.loopProvenance = SoundLoopProvenance::File;
    row.loopRegion = SoundLoopRegion{2 * kRate, 6 * kRate};
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));

    row.loopRegion = SoundLoopRegion{2 * kRate, 5 * kRate};
    row.trimEndFrame = 10 * kRate;
    EXPECT_FALSE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
    row.trimEndFrame = 9 * kRate;
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
    row.trimEndFrame.reset();
    row.trimStartFrame = kRate;
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(sound, resolve));
}

TEST(SoundSourceSemanticsTest, LoopProvenanceKeepsOnlyTheClaimsTheFileBacks)
{
    const AssetInfo info = loopingAsset(1, 10 * kRate, 2 * kRate, 5 * kRate - 1);
    const auto resolve = [&info](uint32_t) { return &info; };
    const SoundLoopRegion seed{2 * kRate, 5 * kRate};

    std::vector<SoundSourceDef> sources{source(1), source(1), source(1), source(1)};
    sources[0].loopRegion = seed;
    sources[0].loopProvenance = SoundLoopProvenance::File;
    sources[1].loopRegion = SoundLoopRegion{2 * kRate, 6 * kRate};
    sources[1].loopProvenance = SoundLoopProvenance::File;
    sources[2].loopProvenance = SoundLoopProvenance::File;
    sources[3].loopRegion = seed;

    SoundSourceSemantics::settleLoopProvenance(sources, resolve);
    EXPECT_EQ(sources[0].loopProvenance, SoundLoopProvenance::File);
    EXPECT_EQ(sources[1].loopProvenance, SoundLoopProvenance::Authored);
    EXPECT_EQ(sources[2].loopProvenance, SoundLoopProvenance::Authored);
    EXPECT_EQ(sources[3].loopProvenance, SoundLoopProvenance::Authored);

    EXPECT_FALSE(SoundSourceSemantics::authoredView(sources[0], &info).loopRegion);
    EXPECT_EQ(SoundSourceSemantics::authoredView(sources[0], &info).loopProvenance,
              SoundLoopProvenance::Authored);
    EXPECT_TRUE(SoundSourceSemantics::authoredView(sources[1], &info).loopRegion);
    EXPECT_TRUE(SoundSourceSemantics::authoredView(sources[3], &info).loopRegion);
    EXPECT_TRUE(SoundSourceSemantics::authoredView(sources[0], nullptr).loopRegion);

    std::vector<SoundSourceDef> unresolvable{sources[0]};
    SoundSourceSemantics::settleLoopProvenance(
        unresolvable, [](uint32_t) -> const AssetInfo* { return nullptr; });
    EXPECT_EQ(unresolvable[0].loopProvenance, SoundLoopProvenance::File);
}

TEST(SoundSourceSemanticsTest, AuthoringAnchorsIgnoreAFileSeededLoop)
{
    AssetInfo info = loopingAsset(1, 10 * kRate, 2 * kRate, 5 * kRate - 1);
    info.contentHash = QStringLiteral("hash-one");
    const auto resolve = [&info](uint32_t) { return &info; };

    std::vector<SoundSourceDef> seeded{source(1)};
    seeded[0].loopRegion = SoundLoopRegion{2 * kRate, 5 * kRate};
    seeded[0].loopProvenance = SoundLoopProvenance::File;
    SoundSourceSemantics::stampAuthoringAnchors(seeded, {}, resolve);
    EXPECT_EQ(seeded[0].rangeContentHash, QStringLiteral("hash-one"));

    std::vector<SoundSourceDef> edited = seeded;
    edited[0].loopRegion = SoundLoopRegion{3 * kRate, 5 * kRate};
    SoundSourceSemantics::settleLoopProvenance(edited, resolve);
    EXPECT_EQ(edited[0].loopProvenance, SoundLoopProvenance::Authored);
    SoundSourceSemantics::stampAuthoringAnchors(edited, seeded, resolve);
    EXPECT_EQ(edited[0].rangeContentHash, QStringLiteral("hash-one"));

    std::vector<SoundSourceDef> relanguaged = seeded;
    SoundSourceSemantics::stampAuthoringAnchors(relanguaged, seeded, resolve);
    EXPECT_EQ(relanguaged[0].rangeContentHash, QStringLiteral("hash-one"));
}
