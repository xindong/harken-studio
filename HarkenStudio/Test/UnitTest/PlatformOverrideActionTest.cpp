#include "ActionManager.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PropertyEditSession.h"
#include "UndoCommands.h"
#include "Components/VMixNode.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

constexpr uint8_t kVolumeId = 1;

class PlatformOverrideActionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("PlatformOverrideActionProject");
        config.location = tempDir.path();
        config.platforms = HarkenTest::platforms(QStringList{
            QStringLiteral("Windows"), QStringLiteral("Android"),
            QStringLiteral("iOS")});
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(
            VNX::ActionManager::instance(), &pm, nullptr, {});
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
    }

    VNX::ProjectItem* makeSound(const QString& name)
    {
        return VNX::ProjectManager::instance().createItem(
            nullptr, name, VNX::ProjectItem::Type::Sound);
    }

    VNX::ActionResult setVolume(VNX::ProjectItem* sound, double value,
                                const QJsonValue& platforms = QJsonValue::Undefined)
    {
        QJsonObject args{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(sound->objectID)},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("override_operation"), QStringLiteral("set_override")},
            {QStringLiteral("value"), value}};
        if (!platforms.isUndefined())
            args.insert(QStringLiteral("platform_ids"), platforms);
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"), args, VNX::ActionSource::Internal);
    }

    VNX::ActionResult clearPlatformVolume(VNX::ProjectItem* sound,
                                          const QJsonArray& platforms)
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"),
                 static_cast<qint64>(sound->objectID)},
                {QStringLiteral("property"), QStringLiteral("volume")},
                {QStringLiteral("override_operation"),
                 QStringLiteral("clear_platform_override")},
                {QStringLiteral("platform_ids"), platforms}},
            VNX::ActionSource::Internal);
    }

    VNX::ActionResult setTargets(const QStringList& platforms)
    {
        VNX::ProjectManager::instance().setProjectPlatformsInternal(HarkenTest::platforms(platforms));
        return {true, {}, {}, {}};
    }

    std::optional<VNX::PlayablePropertyLocatorV1> locatorOf(VNX::ProjectItem* sound)
    {
        return VNX::PlayablePropertyAuthoringV1::definitionLocator(*sound);
    }

    void expectVolume(VNX::ProjectItem* sound, VNX::PlatformScope scope,
                      bool present, double value)
    {
        const auto locator = locatorOf(sound);
        ASSERT_TRUE(locator.has_value());
        const auto state = VNX::PlayablePropertyAuthoringV1::readProperty(
            *sound, *locator, kVolumeId, scope);
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state->present, present);
        EXPECT_DOUBLE_EQ(state->value.toDouble(), value);
    }

    bool overlayPresent(VNX::ProjectItem* sound, VNX::ProjectPlatformId platform)
    {
        const auto* row = sound->platformOverrides.find(platform);
        return row && row->properties.presence.Has(
                   harkenplayableproperties::PlayablePropertyIdV1::Volume);
    }

    int undoCount() const
    {
        return VNX::UndoRedoManager::instance().undoStack()->count();
    }

    void undoOnce()
    {
        VNX::UndoRedoManager::instance().undoStack()->undo();
    }

    QTemporaryDir tempDir;
};

TEST_F(PlatformOverrideActionTest, RetiredSignalGraphCannotBeAuthoredByCommand)
{
    auto* sound = makeSound(QStringLiteral("RetiredGraphSound"));
    ASSERT_NE(sound, nullptr);
    const int before = undoCount();
    QJsonObject args{
        {QStringLiteral("authoring_handle"), static_cast<qint64>(sound->objectID)},
        {QStringLiteral("property"), QStringLiteral("signalGraph")},
        {QStringLiteral("override_operation"), QStringLiteral("set_override")},
        {QStringLiteral("value"), QJsonArray{}},
    };
    for (const bool platformOverride : {false, true}) {
        if (platformOverride)
            args.insert(QStringLiteral("platforms"), QJsonArray{QStringLiteral("Android")});
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"), args, VNX::ActionSource::Internal);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(sound->playablePropertyPresence.bits, 0u);
        EXPECT_TRUE(sound->platformOverrides.rows.empty());
        EXPECT_EQ(undoCount(), before);
    }
}

TEST_F(PlatformOverrideActionTest, OmittedPlatformsWritesSharedAndClearsFieldOverlays)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("OmittedSound"));
    ASSERT_NE(sound, nullptr);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    const int before = undoCount();

    ASSERT_TRUE(setVolume(sound, -3.0).success);

    expectVolume(sound, std::nullopt, true, -3.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -3.0);
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_FLOAT_EQ(sound->comp<VNX::VMixNode>()->volume, -3.0f);
    EXPECT_EQ(undoCount(), before + 1);

    undoOnce();
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    EXPECT_EQ(undoCount(), before + 1);
}

TEST_F(PlatformOverrideActionTest, SubsetWritesOnlyNamedOverlay)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("SubsetSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    VNX::UndoRedoManager::instance().clear();
    const int before = undoCount();

    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);

    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::iOS, true, -6.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::iOS));
    EXPECT_FLOAT_EQ(sound->comp<VNX::VMixNode>()->volume, -6.0f);
    EXPECT_EQ(undoCount(), before + 1);

    undoOnce();
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -6.0);
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
}

TEST_F(PlatformOverrideActionTest, ExplicitAllTargetsWriteOverlaysAndPreserveBase)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("FullSelectSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    ASSERT_TRUE(setVolume(sound, -9.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("iOS"))}).success);
    VNX::UndoRedoManager::instance().clear();
    const int before = undoCount();

    ASSERT_TRUE(setVolume(sound, -3.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Windows")),
                                     HarkenTest::platformIdString(QStringLiteral("Android")),
                                     HarkenTest::platformIdString(QStringLiteral("iOS"))}).success);

    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -3.0);
    expectVolume(sound, HarkenTest::PlatformIds::iOS, true, -3.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::iOS));
    EXPECT_EQ(undoCount(), before + 1);

    undoOnce();
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, HarkenTest::PlatformIds::iOS, true, -9.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::iOS));
}

TEST_F(PlatformOverrideActionTest, SingleTargetWritesOverlayAndPreservesBaseAndDormantPlatform)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("DormantSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    ASSERT_TRUE(setTargets({QStringLiteral("Windows")}).success);
    VNX::UndoRedoManager::instance().clear();

    ASSERT_TRUE(setVolume(sound, -3.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Windows"))}).success);

    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Windows, true, -3.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);

    ASSERT_TRUE(setTargets(
        {QStringLiteral("Windows"), QStringLiteral("Android")}).success);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Windows, true, -3.0);
}

TEST_F(PlatformOverrideActionTest, ClearPlatformOverrideClearsOnlyNamedSubset)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("ClearSubsetSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    ASSERT_TRUE(setVolume(sound, -9.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("iOS"))}).success);
    VNX::UndoRedoManager::instance().clear();
    const int before = undoCount();

    ASSERT_TRUE(clearPlatformVolume(
        sound, QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);

    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::iOS));
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::iOS, true, -9.0);
    EXPECT_EQ(undoCount(), before + 1);

    undoOnce();
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, HarkenTest::PlatformIds::iOS, true, -9.0);
}

TEST_F(PlatformOverrideActionTest, EmptyPlatformsArrayIsRejected)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("EmptyPlatformsSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    sound->clearDirty();
    const int before = undoCount();

    const auto result = setVolume(sound, -12.0, QJsonArray{});
    EXPECT_FALSE(result.success);
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_EQ(undoCount(), before);
}

TEST_F(PlatformOverrideActionTest, UniversalOrUnknownPlatformIsRejected)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("InvalidPlatformSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    const int before = undoCount();

    EXPECT_FALSE(setVolume(sound, -12.0,
                           QJsonArray{QStringLiteral("Universal")}).success);
    EXPECT_FALSE(setVolume(sound, -12.0,
                           QJsonArray{QStringLiteral("PS5")}).success);
    EXPECT_FALSE(setVolume(sound, -12.0,
                           QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android")),
                                      HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_TRUE(sound->platformOverrides.rows.empty());
    EXPECT_EQ(undoCount(), before);
}

TEST_F(PlatformOverrideActionTest, SessionSubsetPreviewCommitMatchesDirectWrite)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("SessionSubsetSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    VNX::UndoRedoManager::instance().clear();
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());

    VNX::PropertyEditSession session(
        sound, *locator, kVolumeId, nullptr,
        QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Android});
    session.preview(QVariant(-12.0));
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, std::nullopt, true, -6.0);
    session.commit();

    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_EQ(undoCount(), 1);
}

TEST_F(PlatformOverrideActionTest, SessionExplicitAllTargetsPreviewAndCommitOnlyOverlays)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("SessionFullSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    VNX::UndoRedoManager::instance().clear();
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());

    VNX::PropertyEditSession session(
        sound, *locator, kVolumeId, nullptr,
        QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android,
                               HarkenTest::PlatformIds::iOS});
    session.preview(QVariant(-3.0));
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -3.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    session.commit();

    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -3.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_EQ(undoCount(), 1);

    undoOnce();
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
}

TEST_F(PlatformOverrideActionTest, OutOfTargetPlatformIsRejected)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("OutOfTargetSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setTargets({QStringLiteral("Windows")}).success);
    VNX::UndoRedoManager::instance().clear();
    sound->clearDirty();
    const int before = undoCount();

    const auto result = setVolume(sound, -12.0,
                                  QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_ARGS"));
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_TRUE(sound->platformOverrides.rows.empty());
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_EQ(undoCount(), before);
}

TEST_F(PlatformOverrideActionTest, PartialOutOfTargetPlatformIsRejected)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("PartialOutOfTargetSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setTargets(
        {QStringLiteral("Windows"), QStringLiteral("Android")}).success);
    VNX::UndoRedoManager::instance().clear();
    sound->clearDirty();
    const int before = undoCount();

    const auto result = setVolume(
        sound, -12.0,
        QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android")), HarkenTest::platformIdString(QStringLiteral("iOS"))});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_ARGS"));
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_TRUE(sound->platformOverrides.rows.empty());
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_FALSE(overlayPresent(sound, HarkenTest::PlatformIds::iOS));
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_EQ(undoCount(), before);
}

TEST_F(PlatformOverrideActionTest, ClearPlatformOverrideOutOfTargetIsRejected)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("ClearOutOfTargetSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    ASSERT_TRUE(setTargets({QStringLiteral("Windows")}).success);
    VNX::UndoRedoManager::instance().clear();
    sound->clearDirty();
    const int before = undoCount();

    const auto result = clearPlatformVolume(
        sound, QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_ARGS"));
    expectVolume(sound, std::nullopt, true, -6.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_EQ(undoCount(), before);
}

TEST_F(PlatformOverrideActionTest, SessionPreviewCancelRestoresTrueStart)
{
    VNX::ProjectItem* sound = makeSound(QStringLiteral("SessionCancelSound"));
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(setVolume(sound, -6.0).success);
    ASSERT_TRUE(setVolume(sound, -12.0,
                          QJsonArray{HarkenTest::platformIdString(QStringLiteral("Android"))}).success);
    const auto locator = locatorOf(sound);
    ASSERT_TRUE(locator.has_value());
    const int before = undoCount();

    {
        VNX::PropertyEditSession subset(
            sound, *locator, kVolumeId, nullptr,
            QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Android});
        subset.preview(QVariant(-1.0));
        expectVolume(sound, HarkenTest::PlatformIds::Android, true, -1.0);
        subset.cancel();
    }
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    expectVolume(sound, std::nullopt, true, -6.0);

    {
        VNX::PropertyEditSession full(
            sound, *locator, kVolumeId, nullptr,
            QVector<VNX::ProjectPlatformId>{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android,
                                   HarkenTest::PlatformIds::iOS});
        full.preview(QVariant(-3.0));
        EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
        expectVolume(sound, std::nullopt, true, -6.0);
        full.cancel();
    }
    expectVolume(sound, std::nullopt, true, -6.0);
    expectVolume(sound, HarkenTest::PlatformIds::Android, true, -12.0);
    EXPECT_TRUE(overlayPresent(sound, HarkenTest::PlatformIds::Android));
    EXPECT_EQ(undoCount(), before);
}

}  // namespace
