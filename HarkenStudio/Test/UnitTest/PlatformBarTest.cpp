#include "Support/FieldGroupTestUtils.h"
#include "Core/LanguageContextService.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Widgets/PlatformBar/PlatformBar.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QTemporaryDir>
#include <QMainWindow>
#include <QToolButton>
#include <cstdlib>
#include <memory>
#include <utility>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QStringList names(std::initializer_list<const char*> raw)
{
    QStringList out;
    for (const char* name : raw)
        out.append(QString::fromLatin1(name));
    return out;
}

QToolButton* allButtonOf(VNX::PlatformBar& bar)
{
    return bar.findChild<QToolButton*>(QStringLiteral("platformBarAll"));
}

QComboBox* platformsOf(VNX::PlatformBar& bar)
{
    return bar.findChild<QComboBox*>(QStringLiteral("platformBarSelector"));
}

TEST(PlatformBar, HiddenWhenNoProject)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    EXPECT_EQ(control->count(), 0);
    auto* all = allButtonOf(bar);
    ASSERT_NE(all, nullptr);
    EXPECT_FALSE(all->isVisible());
}

TEST(PlatformBar, DefaultWindowsKeepsAllVisibleAndAllowsIndependentEditing)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    EXPECT_FALSE(bar.isVisible());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("DefaultWindowsBar");
    config.location = directory.path();
    ASSERT_TRUE(pm.createProject(config));
    ASSERT_EQ(pm.currentProject()->platforms.size(), 1);
    const auto id = pm.currentProject()->platforms.front().id;
    auto* all = allButtonOf(bar);
    auto* control = platformsOf(bar);
    ASSERT_NE(all, nullptr);
    ASSERT_NE(control, nullptr);
    EXPECT_TRUE(bar.isVisible());
    EXPECT_TRUE(all->isVisible());
    EXPECT_TRUE(all->isEnabled());
    EXPECT_TRUE(all->isChecked());
    ASSERT_EQ(control->count(), 1);
    EXPECT_FALSE(control->isEditable());
    EXPECT_EQ(control->currentText(), QStringLiteral("Windows"));
    EXPECT_EQ(control->currentData().toUuid(), id);
    all->click();
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(service.context().selected, QVector<VNX::ProjectPlatformId>{id});
    all->click();
    EXPECT_TRUE(service.context().all);
    pm.closeProject();
    EXPECT_FALSE(bar.isVisible());
    EXPECT_FALSE(all->isChecked());
    EXPECT_EQ(service.context(), VNX::PlatformContext{});
}

TEST(PlatformBar, RebuildsDropdownFromTargetPlatforms)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarDropdown");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    bar.show();
    EXPECT_TRUE(bar.isVisible());
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(control->count(), 3);
    EXPECT_EQ(control->itemText(0), QStringLiteral("Windows"));
    EXPECT_EQ(control->itemText(1), QStringLiteral("Linux"));
    EXPECT_EQ(control->itemText(2), QStringLiteral("Android"));
    auto* all = allButtonOf(bar);
    ASSERT_NE(all, nullptr);
    EXPECT_TRUE(all->isVisible());
    EXPECT_TRUE(all->isChecked());

    pm.setProjectPlatformsInternal(HarkenTest::platforms(names({"Windows", "iOS"})));
    ASSERT_EQ(control->count(), 2);
    EXPECT_EQ(control->itemText(0), QStringLiteral("Windows"));
    EXPECT_EQ(control->itemText(1), QStringLiteral("iOS"));
    EXPECT_TRUE(all->isVisible());

    pm.closeProject();
    EXPECT_FALSE(bar.isVisible());
}

TEST(PlatformBar, ProjectOpenedRebuildsDropdownAndShowsBar)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    EXPECT_EQ(control->count(), 0);
    EXPECT_FALSE(bar.isVisible());

    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarOpened");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    ASSERT_EQ(control->count(), 3);
    EXPECT_EQ(control->itemText(0), QStringLiteral("Windows"));
    EXPECT_EQ(control->itemText(1), QStringLiteral("Linux"));
    EXPECT_EQ(control->itemText(2), QStringLiteral("Android"));
    EXPECT_TRUE(bar.isVisible());
    EXPECT_TRUE(allButtonOf(bar)->isVisible());

    pm.closeProject();
}

TEST(PlatformBar, SingleTargetKeepsAllIndependentFromPlatformSelection)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarKeepLastSingle");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    bar.show();
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(control->count(), 1);
    EXPECT_EQ(control->currentText(), QStringLiteral("Windows"));
    EXPECT_EQ(control->currentIndex(), 0);
    auto* all = allButtonOf(bar);
    ASSERT_NE(all, nullptr);
    EXPECT_TRUE(all->isVisible());
    EXPECT_EQ(bar.findChild<QToolButton*>(QStringLiteral("platformBarShared")), nullptr);
    ASSERT_EQ(service.context().selected.size(), 1);
    EXPECT_EQ(service.context().selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(service.context().all);

    int emptyEmissions = 0;
    QMetaObject::Connection watcher =
        QObject::connect(&service, &VNX::PlatformContextService::contextChanged,
                         &service, [&](const VNX::PlatformContext& context) {
                             EXPECT_FALSE(context.selected.isEmpty());
                             if (context.selected.isEmpty())
                                 ++emptyEmissions;
                         });

    all->click();
    EXPECT_FALSE(all->isChecked());
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(control->currentIndex(), 0);
    control->setCurrentIndex(0);
    EXPECT_EQ(control->currentIndex(), 0);
    EXPECT_FALSE(service.context().all);
    ASSERT_EQ(service.context().selected.size(), 1);
    EXPECT_EQ(service.context().selected.at(0), HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(emptyEmissions, 0);
    all->click();
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(control->currentIndex(), 0);

    QObject::disconnect(watcher);
    pm.closeProject();
}

TEST(PlatformBar, SelectingCurrentPlatformDoesNotLeaveEmptySelection)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarKeepLastMulti");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(control->count(), 2);
    auto* all = allButtonOf(bar);
    ASSERT_NE(all, nullptr);
    EXPECT_TRUE(all->isChecked());
    EXPECT_EQ(control->currentIndex(), 0);

    int emptyEmissions = 0;
    QMetaObject::Connection watcher =
        QObject::connect(&service, &VNX::PlatformContextService::contextChanged,
                         &service, [&](const VNX::PlatformContext& context) {
                             EXPECT_FALSE(context.selected.isEmpty());
                             if (context.selected.isEmpty())
                                 ++emptyEmissions;
                         });

    control->setCurrentIndex(0);
    EXPECT_EQ(control->currentIndex(), 0);
    EXPECT_TRUE(all->isChecked());
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    EXPECT_EQ(emptyEmissions, 0);

    QObject::disconnect(watcher);
    pm.closeProject();
}

TEST(PlatformBar, AllAndPrimarySurviveTargetRebuild)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarAllRebuild");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    service.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    service.setAll(true);
    pm.setProjectPlatformsInternal(HarkenTest::platforms(names({"Windows", "Linux", "Android"})));
    EXPECT_TRUE(allButtonOf(bar)->isChecked());
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(control->count(), 3);
    EXPECT_EQ(control->currentIndex(), 2);
    EXPECT_EQ(control->currentData().toUuid(), HarkenTest::PlatformIds::Android);
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(service.context().selected.size(), 3);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    pm.closeProject();
}

TEST(PlatformBar, SelectingPlatformWhileAllStaysOnOnlyChangesPrimary)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarClickPrimary");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(control->count(), 3);
    auto* all = allButtonOf(bar);
    ASSERT_NE(all, nullptr);
    EXPECT_TRUE(all->isChecked());
    EXPECT_EQ(control->currentIndex(), 0);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Windows);
    EXPECT_TRUE(service.context().all);

    control->setCurrentIndex(1);
    EXPECT_TRUE(all->isChecked());
    EXPECT_EQ(control->currentIndex(), 1);
    EXPECT_TRUE(service.context().all);
    ASSERT_EQ(service.context().selected.size(), 3);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Linux);

    pm.closeProject();
}

TEST(PlatformBar, TogglingAllOffEditsOnlyPrimary)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarToggleAll");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux", "Android"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    auto* control = platformsOf(bar);
    ASSERT_NE(control, nullptr);
    auto* all = allButtonOf(bar);
    ASSERT_EQ(control->count(), 3);
    ASSERT_NE(all, nullptr);

    control->setCurrentIndex(1);
    all->click();
    EXPECT_FALSE(all->isChecked());
    EXPECT_FALSE(service.context().all);
    ASSERT_EQ(service.context().selected.size(), 1);
    EXPECT_EQ(service.context().selected.at(0), HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Linux);
    EXPECT_EQ(control->currentIndex(), 1);

    control->setCurrentIndex(2);
    EXPECT_FALSE(all->isChecked());
    EXPECT_FALSE(service.context().all);
    ASSERT_EQ(service.context().selected.size(), 1);
    EXPECT_EQ(service.context().selected.at(0), HarkenTest::PlatformIds::Android);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);

    all->click();
    EXPECT_TRUE(all->isChecked());
    EXPECT_TRUE(service.context().all);
    ASSERT_EQ(service.context().selected.size(), 3);
    EXPECT_EQ(service.context().primary, HarkenTest::PlatformIds::Android);
    EXPECT_EQ(control->currentIndex(), 2);

    pm.closeProject();
}

VNX::SegmentedControl* languagesOf(VNX::PlatformBar& bar)
{
    return bar.findChild<VNX::SegmentedControl*>(QStringLiteral("platformBarLanguages"));
}

TEST(PlatformBar, LanguageSegmentsHiddenUntilProjectHasLanguages)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen())
        pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformBarLanguages");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms(names({"Windows", "Linux"}));
    ASSERT_TRUE(pm.createProject(config));

    VNX::LanguageContextService::instance().setStorageForTesting(
        std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PlatformBar bar(service);
    bar.show();
    auto* languages = languagesOf(bar);
    ASSERT_NE(languages, nullptr);
    EXPECT_EQ(languages->button(0), nullptr);
    EXPECT_FALSE(languages->isVisible());
    EXPECT_TRUE(VNX::LanguageContextService::instance().language().isEmpty());
    auto* platforms = platformsOf(bar);
    auto* all = allButtonOf(bar);
    ASSERT_NE(platforms, nullptr);
    ASSERT_NE(all, nullptr);
    EXPECT_TRUE(platforms->isVisible());
    EXPECT_TRUE(all->isVisible());
    EXPECT_TRUE(all->isChecked());

    pm.setProjectLanguagesInternal(names({"English", "Japanese"}));
    ASSERT_NE(languages->button(0), nullptr);
    ASSERT_NE(languages->button(1), nullptr);
    EXPECT_EQ(languages->button(2), nullptr);
    EXPECT_TRUE(languages->isVisible());
    EXPECT_EQ(languages->button(0)->text(), QStringLiteral("English"));
    EXPECT_EQ(languages->button(1)->text(), QStringLiteral("Japanese"));
    EXPECT_TRUE(languages->button(0)->isChecked());
    EXPECT_FALSE(languages->button(1)->isChecked());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), QStringLiteral("English"));

    languages->button(1)->click();
    EXPECT_FALSE(languages->button(0)->isChecked());
    EXPECT_TRUE(languages->button(1)->isChecked());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), QStringLiteral("Japanese"));

    languages->button(1)->click();
    EXPECT_TRUE(languages->button(1)->isChecked());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), QStringLiteral("Japanese"));

    pm.setProjectLanguagesInternal(names({"Japanese", "English"}));
    ASSERT_NE(languages->button(0), nullptr);
    ASSERT_NE(languages->button(1), nullptr);
    EXPECT_EQ(languages->button(0)->text(), QStringLiteral("Japanese"));
    EXPECT_TRUE(languages->button(0)->isChecked());
    EXPECT_FALSE(languages->button(1)->isChecked());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), QStringLiteral("Japanese"));

    pm.setProjectLanguagesInternal(names({"Korean", "English"}));
    ASSERT_NE(languages->button(0), nullptr);
    ASSERT_NE(languages->button(1), nullptr);
    EXPECT_EQ(languages->button(0)->text(), QStringLiteral("Korean"));
    EXPECT_TRUE(languages->button(0)->isChecked());
    EXPECT_FALSE(languages->button(1)->isChecked());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), QStringLiteral("Korean"));

    pm.setProjectLanguagesInternal({});
    EXPECT_EQ(languages->button(0), nullptr);
    EXPECT_FALSE(languages->isVisible());
    EXPECT_TRUE(VNX::LanguageContextService::instance().language().isEmpty());
    EXPECT_TRUE(bar.isVisible());
    EXPECT_TRUE(platforms->isVisible());
    EXPECT_TRUE(all->isVisible());
    EXPECT_TRUE(all->isChecked());

    pm.closeProject();
    VNX::LanguageContextService::instance().setStorageForTesting(
        VNX::SettingsStorageFactory::create(
            VNX::SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"),
            QStringLiteral("HarkenStudio")));
}

TEST(PlatformBar, NarrowToolbarKeepsEveryLongNamedSiblingReachableAndRetainsAllAudition)
{
    ensureApp();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir directory;
    VNX::ProjectConfig config;
    config.location = directory.path();
    config.name = "Compressed";
    config.platforms.clear();
    for (int i = 0; i < 9; ++i)
        config.platforms.append({QUuid::createUuid(), QString("Android_Quality_%1_LongName").arg(i), VNX::BasePlatform::Android});
    ASSERT_TRUE(pm.createProject(config));
    VNX::PlatformContextService service(std::make_unique<VNX::MemorySettingsStorage>());
    QMainWindow window;
    auto* bar = new VNX::PlatformBar(service, &window);
    window.addToolBar(bar);
    window.resize(360, 200);
    window.show();
    QApplication::processEvents();
    auto* control = platformsOf(*bar);
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(control->count(), 9);
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(control->itemText(i), config.platforms.at(i).name);
        EXPECT_EQ(control->itemData(i).toUuid(), config.platforms.at(i).id);
    }
    control->setCurrentIndex(8);
    EXPECT_EQ(service.context().primary, config.platforms.last().id);
    EXPECT_TRUE(service.context().all);
    EXPECT_EQ(control->currentIndex(), 8);
    allButtonOf(*bar)->click();
    EXPECT_FALSE(service.context().all);
    EXPECT_EQ(service.context().selected, QVector<VNX::ProjectPlatformId>{config.platforms.last().id});
    control->setCurrentIndex(6);
    EXPECT_EQ(service.context().primary, config.platforms.at(6).id);
    EXPECT_FALSE(service.context().all);
    pm.closeProject();
    EXPECT_FALSE(bar->isVisible());
}

class PlatformBarState : public testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(directory_.isValid());
        VNX::ProjectConfig config;
        config.location = directory_.path();
        config.name = QStringLiteral("ToolbarLayout");
        ASSERT_TRUE(manager.createProject(config));
        VNX::LanguageContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        service_ = std::make_unique<VNX::PlatformContextService>(std::make_unique<VNX::MemorySettingsStorage>());
        window_ = std::make_unique<QMainWindow>();
        bar_ = new VNX::PlatformBar(*service_, window_.get());
        window_->addToolBar(bar_);
    }

    void TearDown() override
    {
        window_.reset();
        service_.reset();
        VNX::ProjectManager::instance().closeProject();
        VNX::LanguageContextService::instance().setStorageForTesting(
            VNX::SettingsStorageFactory::create(VNX::SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    void resizeWindow(int width)
    {
        window_->resize(width, 240);
        window_->show();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
    }

    QTemporaryDir directory_;
    std::unique_ptr<VNX::PlatformContextService> service_;
    std::unique_ptr<QMainWindow> window_;
    VNX::PlatformBar* bar_ = nullptr;
};

TEST_F(PlatformBarState, PlatformRenamePreservesSelectionIdentity)
{
    auto& manager = VNX::ProjectManager::instance();
    auto platforms = QVector<VNX::ProjectPlatform>{
        {QUuid::createUuid(), QStringLiteral("Android_Production_Low_Memory"), VNX::BasePlatform::Android},
        {QUuid::createUuid(), QStringLiteral("Android_Production_High_Quality"), VNX::BasePlatform::Android}};
    manager.setProjectPlatformsInternal(platforms);
    service_->setSelected({platforms[1].id}, platforms[1].id, false);
    auto* control = platformsOf(*bar_);
    resizeWindow(1200);
    platforms[1].name += QStringLiteral("_Optimized");
    manager.setProjectPlatformsInternal(platforms);
    resizeWindow(1200);
    EXPECT_EQ(control->currentText(), platforms[1].name);
    EXPECT_EQ(control->currentData().toUuid(), platforms[1].id);
    EXPECT_EQ(service_->context().primary, platforms[1].id);
    EXPECT_FALSE(service_->context().all);
}

TEST_F(PlatformBarState, NarrowWindowPreservesDropdownReachabilityAndSelectionState)
{
    auto& manager = VNX::ProjectManager::instance();
    QVector<VNX::ProjectPlatform> platforms;
    for (int i = 0; i < 9; ++i)
        platforms.append({QUuid::createUuid(), QStringLiteral("Android_Quality_%1_LongName").arg(i), VNX::BasePlatform::Android});
    manager.setProjectPlatformsInternal(platforms);
    auto* control = platformsOf(*bar_);
    for (const bool all : {true, false}) {
        QVector<VNX::ProjectPlatformId> selected;
        for (const auto& platform : platforms)
            if (all || selected.isEmpty()) selected.append(platform.id);
        service_->setSelected(selected, platforms[0].id, all);
        resizeWindow(360);
        EXPECT_TRUE(control->isVisible());
        EXPECT_TRUE(allButtonOf(*bar_)->isVisible());
        EXPECT_EQ(control->count(), platforms.size());
        EXPECT_EQ(control->currentData().toUuid(), platforms[0].id);
        EXPECT_EQ(service_->context().primary, platforms[0].id);
        EXPECT_EQ(service_->context().all, all);
    }
}

}
