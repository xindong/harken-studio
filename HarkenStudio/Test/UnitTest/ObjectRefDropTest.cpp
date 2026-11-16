#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/Components/VSound.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/RefDomainAdapter.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/AudioAssetLabels.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <memory>

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

SelectorWindow* selectPickerRow(uint32_t id)
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* selector = qobject_cast<SelectorWindow*>(widget);
        if (!selector || !selector->isVisible())
            continue;
        auto* tree = selector->findChild<QTreeWidget*>();
        if (!tree)
            continue;
        for (QTreeWidgetItemIterator row(tree); *row; ++row) {
            if ((*row)->data(0, kRoleObjectID).toUInt() == id
                && (*row)->data(0, kRoleSelectable).toBool()) {
                tree->setCurrentItem(*row);
                return selector;
            }
        }
    }
    return nullptr;
}

class ObjectRefDropTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ObjectRefDropProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        masterBus_ = ProjectManager::instance().getMasterBusItem();
        ASSERT_NE(masterBus_, nullptr);

        ProjectItem* layer = ProjectManager::instance().createItem(
            nullptr, QStringLiteral("DropLayer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        sound_ = ProjectManager::instance().createItem(
            layer, QStringLiteral("DropSound"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
    }

    void TearDown() override
    {
        SelectorWindow::dismiss();
        if (ProjectManager::instance().currentProject())
            ProjectManager::instance().closeProject();
    }

    AssetInfo registerSource(const QString& path)
    {
        auto& registry = AssetRegistry::instance();
        AssetInfo asset;
        asset.sourceID = registry.allocateStableID();
        asset.relativePath = path;
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(registry.projectRoot())).filePath(path);
        EXPECT_TRUE(QDir().mkpath(QFileInfo(asset.absolutePath).absolutePath()));
        QFile file(asset.absolutePath);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.close();
        EXPECT_TRUE(registry.registerAsset(asset));
        return asset;
    }

    QTemporaryDir tempDir_;
    ProjectItem* masterBus_ = nullptr;
    ProjectItem* sound_ = nullptr;
};

}

TEST_F(ObjectRefDropTest, CancellingAnOldWidgetDoesNotCloseAnotherWidgetsPicker)
{
    ObjectRefWidget first;
    ObjectRefWidget second;
    int firstChanges = 0;
    int secondChanges = 0;
    QObject::connect(&first, &ObjectRefWidget::objectChanged, [&] { ++firstChanges; });
    QObject::connect(&second, &ObjectRefWidget::objectChanged, [&] { ++secondChanges; });
    auto* firstPick = first.findChild<QToolButton*>(QStringLiteral("refPick"));
    auto* secondPick = second.findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(firstPick, nullptr);
    ASSERT_NE(secondPick, nullptr);
    firstPick->click();
    secondPick->click();
    auto* selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    first.cancelPendingSelection();
    EXPECT_TRUE(selector->isVisible());
    selector->accept();
    EXPECT_EQ(firstChanges, 0);
    EXPECT_EQ(secondChanges, 1);
    EXPECT_EQ(second.currentObjectID(), masterBus_->objectID);
}

TEST_F(ObjectRefDropTest, SharedPickerStaysOnScreenWithoutChangingItsOwner)
{
    SelectorRequest request;
    request.currentObjectID = masterBus_->objectID;
    for (auto* screen : QApplication::screens()) {
        SCOPED_TRACE(screen->name().toStdString());
        const QRect available = screen->availableGeometry();
        QWidget anchor;
        anchor.setScreen(screen);
        anchor.resize(24, 24);
        const QRect positions(available.topLeft(), available.size() - anchor.size() + QSize(1, 1));
        for (const QPoint& position : {positions.topLeft(), positions.topRight(),
                                       positions.bottomLeft(), positions.bottomRight()}) {
            SCOPED_TRACE(::testing::Message() << position.x() << ", " << position.y());
            anchor.move(position);
            auto* selector = SelectorWindow::open(request, &anchor);
            QApplication::processEvents();
            ASSERT_TRUE(selector->isVisible());
            EXPECT_EQ(selector->screen(), screen);
            EXPECT_TRUE(available.contains(selector->frameGeometry()));
            SelectorWindow::dismiss(&anchor);
        }

        QWidget largeOwner;
        largeOwner.setScreen(screen);
        largeOwner.setGeometry(QRect(available.topLeft(), available.size() * 4));
        const QRect clickRect(available.center(), QSize(1, 1));
        auto* selector = SelectorWindow::open(request, &largeOwner, clickRect);
        QApplication::processEvents();
        ASSERT_TRUE(selector->isVisible());
        EXPECT_EQ(selector->screen(), screen);
        EXPECT_TRUE(available.contains(selector->frameGeometry()));
        SelectorWindow::dismiss(&anchor);
        EXPECT_TRUE(selector->isVisible());

        const QSize originalSize = selector->size();
        selector->resize(available.size() * 2);
        selector = SelectorWindow::open(request, &largeOwner, clickRect);
        QApplication::processEvents();
        ASSERT_TRUE(selector->isVisible());
        EXPECT_EQ(selector->screen(), screen);
        EXPECT_TRUE(available.contains(selector->frameGeometry()));
        SelectorWindow::dismiss(&largeOwner);
        EXPECT_FALSE(selector->isVisible());
        selector->resize(originalSize);
    }
}

TEST_F(ObjectRefDropTest, DeferredNavigationDoesNotOpenAReplacedOrDestroyedReference)
{
    QWidget host;
    ObjectRefNavigation navigation(&host);
    auto widget = std::make_unique<ObjectRefWidget>(&host);
    widget->setCurrentObjectID(masterBus_->objectID);
    int navigated = 0;
    QObject::connect(&navigation, &ObjectRefNavigation::navigateRequested,
                     [&] { ++navigated; });
    const auto doubleClick = [&] {
        QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget.get(), &event);
    };
    doubleClick();
    widget->setCurrentObjectID(sound_->objectID);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(navigated, 0);
    doubleClick();
    widget.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(navigated, 0);
}

TEST_F(ObjectRefDropTest, ReferenceChangesFollowDependenciesAndCoalesce)
{
    auto& registry = AssetRegistry::instance();
    const auto first = registerSource(QStringLiteral("first.wav"));
    const auto second = registerSource(QStringLiteral("second.wav"));
    auto* sound = sound_->comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {{.entryId = QUuid::createUuid(), .sourceId = first.sourceID}};
    sound->sources.localized[QStringLiteral("en")].entries = {
        {.entryId = QUuid::createUuid(), .sourceId = second.sourceID}};

    ObjectRefWidget soundRef;
    soundRef.setCurrentObjectID(sound_->objectID);
    ObjectRefWidget sourceRef;
    sourceRef.setRefDomain(RefDomain::AudioAsset);
    sourceRef.setCurrentObjectID(first.sourceID);
    QObject observer;
    int unrelatedChanges = 0;
    int soundChanges = 0;
    const auto& adapter = refDomainAdapter(RefDomain::ProjectObject);
    auto busSubscription = adapter.connectChanged(&observer, [&] { ++unrelatedChanges; }, masterBus_->objectID);
    auto soundSubscription = adapter.connectChanged(&observer, [&] { ++soundChanges; }, sound_->objectID);
    registry.relocateSourcePaths(QStringLiteral("second.wav"), QStringLiteral("inactive.wav"));
    QApplication::processEvents();
    EXPECT_EQ(unrelatedChanges, 0);
    EXPECT_EQ(soundChanges, 0);

    const QString previousSoundTip = soundRef.toolTip();
    const QString previousSourceTip = sourceRef.toolTip();
    registry.relocateSourcePaths(QStringLiteral("first.wav"), QStringLiteral("intermediate.wav"));
    registry.relocateSourcePaths(QStringLiteral("intermediate.wav"), QStringLiteral("final.wav"));
    EXPECT_EQ(soundRef.toolTip(), previousSoundTip);
    EXPECT_EQ(sourceRef.toolTip(), previousSourceTip);
    QApplication::processEvents();
    const QString finalPath = assetDisplayPath(*registry.getAsset(first.sourceID));
    EXPECT_TRUE(soundRef.toolTip().contains(finalPath));
    EXPECT_TRUE(sourceRef.toolTip().contains(finalPath));
    EXPECT_EQ(unrelatedChanges, 0);

    sound->distinguishLanguage = true;
    emit ProjectManager::instance().itemPropertyChanged(sound_, QStringLiteral("distinguishLanguage"), nullptr);
    QApplication::processEvents();
    EXPECT_TRUE(soundRef.toolTip().contains(assetDisplayPath(*registry.getAsset(second.sourceID))));
    soundChanges = 0;
    registry.relocateSourcePaths(QStringLiteral("final.wav"), QStringLiteral("unused.wav"));
    QApplication::processEvents();
    EXPECT_EQ(soundChanges, 0);
    EXPECT_EQ(unrelatedChanges, 0);

    registry.unregisterAsset(second.sourceID);
    soundRef.setCurrentObjectID(masterBus_->objectID);
    sourceRef.setCurrentObjectID(second.sourceID);
    QApplication::processEvents();
    EXPECT_EQ(soundRef.toolTip(), adapter.tooltip(masterBus_->objectID));
    EXPECT_EQ(sourceRef.currentObjectID(), second.sourceID);
    EXPECT_FALSE(sourceRef.toolTip().contains(finalPath));
}

TEST_F(ObjectRefDropTest, PickerRefreshPreservesSelectionSearchExpansionAndScroll)
{
    const auto first = registerSource(QStringLiteral("A/Common/first.wav"));
    const auto selected = registerSource(QStringLiteral("B/Common/selected.wav"));
    for (int index = 0; index < 40; ++index)
        registerSource(QStringLiteral("B/Common/source%1.wav").arg(index));

    int selections = 0;
    SelectorRequest request;
    request.domain = RefDomain::AudioAsset;
    request.currentObjectID = selected.sourceID;
    request.onSelected = [&](uint32_t) { ++selections; };
    auto* selector = SelectorWindow::open(request);
    auto* tree = selector->findChild<QTreeWidget*>();
    auto* search = selector->findChild<QLineEdit*>();
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral(".wav"));
    QApplication::processEvents();
    tree->topLevelItem(0)->child(0)->setExpanded(false);
    tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2);
    const int scroll = tree->verticalScrollBar()->value();
    ASSERT_GT(scroll, 0);
    int rebuilds = 0;
    QObject observer;
    QObject::connect(tree->model(), &QAbstractItemModel::modelReset, &observer, [&] { ++rebuilds; });

    const QString renamedPath = QStringLiteral("A/Common/renamed.wav");
    ASSERT_TRUE(QFile::rename(first.absolutePath,
        QFileInfo(first.absolutePath).dir().filePath(QStringLiteral("renamed.wav"))));
    auto& registry = AssetRegistry::instance();
    registry.relocateSourcePaths(first.relativePath, renamedPath);
    emit registry.assetModified(selected.sourceID);
    QApplication::processEvents();
    EXPECT_EQ(rebuilds, 1);
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), selected.sourceID);
    EXPECT_EQ(search->text(), QStringLiteral(".wav"));
    EXPECT_EQ(tree->verticalScrollBar()->value(), scroll);
    EXPECT_FALSE(tree->topLevelItem(0)->child(0)->isExpanded());
    EXPECT_TRUE(tree->topLevelItem(1)->child(0)->isExpanded());
    EXPECT_TRUE(tree->topLevelItem(0)->child(0)->child(0)->toolTip(0).contains(renamedPath));
    EXPECT_EQ(selections, 0);
}

TEST_F(ObjectRefDropTest, PickerSubscriptionsFollowTheActiveRequestAndVisibility)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    const auto first = registerSource(QStringLiteral("first.wav"));
    const auto second = registerSource(QStringLiteral("second.wav"));
    auto* sound = sound_->comp<VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {{.entryId = QUuid::createUuid(), .sourceId = first.sourceID}};
    SelectorRequest request;
    request.filterType = ProjectItem::Type::Sound;
    request.currentObjectID = sound_->objectID;
    auto* selector = SelectorWindow::open(request);
    auto* tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    QApplication::processEvents();
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_TRUE(tree->currentItem()->toolTip(0).contains(first.relativePath));

    ASSERT_TRUE(manager.renameItem(sound_->parent, QStringLiteral("RenamedLayer")));
    sound->sources.plain.entries.front().sourceId = second.sourceID;
    emit manager.itemPropertyChanged(sound_, QStringLiteral("sources"), nullptr);
    QApplication::processEvents();
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), sound_->objectID);
    EXPECT_TRUE(tree->currentItem()->toolTip(0).contains(QStringLiteral("RenamedLayer")));
    EXPECT_TRUE(tree->currentItem()->toolTip(0).contains(second.relativePath));
    registry.unregisterAsset(second.sourceID);
    QApplication::processEvents();
    EXPECT_FALSE(tree->currentItem()->toolTip(0).contains(second.relativePath));

    SelectorWindow::refresh();
    request = {};
    request.domain = RefDomain::AudioAsset;
    request.currentObjectID = first.sourceID;
    SelectorWindow::open(request);
    int rebuilds = 0;
    QObject observer;
    QObject::connect(tree->model(), &QAbstractItemModel::modelReset, &observer, [&] { ++rebuilds; });
    ASSERT_TRUE(manager.renameItem(sound_->parent, QStringLiteral("AnotherLayer")));
    QApplication::processEvents();
    EXPECT_EQ(rebuilds, 0);

    emit registry.assetModified(first.sourceID);
    selector->hide();
    QApplication::processEvents();
    emit registry.assetModified(first.sourceID);
    QApplication::processEvents();
    EXPECT_EQ(rebuilds, 0);
    SelectorWindow::open(request);
    QApplication::processEvents();
    rebuilds = 0;
    emit registry.assetModified(first.sourceID);
    QApplication::processEvents();
    EXPECT_EQ(rebuilds, 1);
    manager.closeProject();
    EXPECT_FALSE(selector->isVisible());
}
