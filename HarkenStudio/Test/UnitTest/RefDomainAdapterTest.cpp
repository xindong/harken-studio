#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/RefDomainAdapter.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/TreeRowStyle.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSet>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

#include <functional>

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

class RefDomainAdapterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("RefDomainAdapterProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        masterBus_ = ProjectManager::instance().getMasterBusItem();
        ASSERT_NE(masterBus_, nullptr);
        ASSERT_NE(masterBus_->objectID, 0u);

        childBus_ = ProjectManager::instance().createItem(
            masterBus_, QStringLiteral("ChildBus"), ProjectItem::Type::Bus);
        ASSERT_NE(childBus_, nullptr);
        ASSERT_NE(childBus_->objectID, 0u);

        ProjectItem* layer = ProjectManager::instance().createItem(
            nullptr, QStringLiteral("AdapterLayer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        sound_ = ProjectManager::instance().createItem(
            layer, QStringLiteral("AdapterSound"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    ProjectItem* masterBus_ = nullptr;
    ProjectItem* childBus_ = nullptr;
    ProjectItem* sound_ = nullptr;
};

}

TEST(RefDomainAdapterDispatch, DistinctInstancesPerDomain)
{
    ensureApp();
    EXPECT_NE(&refDomainAdapter(RefDomain::ProjectObject),
              &refDomainAdapter(RefDomain::AudioAsset));
}

TEST_F(RefDomainAdapterTest, ProjectObjectResolvesBusLikeTheWidget)
{
    const IRefDomainAdapter& adapter = refDomainAdapter(RefDomain::ProjectObject);
    const uint32_t busId = childBus_->objectID;
    const QString tooltip = ProjectManager::instance().currentProject()->itemPath(childBus_);

    EXPECT_TRUE(adapter.exists(busId));
    EXPECT_EQ(adapter.displayName(busId), childBus_->name);
    EXPECT_EQ(adapter.tooltip(busId), tooltip);
    EXPECT_NE(adapter.tooltip(busId), adapter.displayName(busId));
    EXPECT_TRUE(adapter.matchesFilter(busId, {ProjectItem::Type::Bus, {}}));
    EXPECT_FALSE(adapter.matchesFilter(sound_->objectID, {ProjectItem::Type::Bus, {}}));
    EXPECT_TRUE(adapter.matchesFilter(
        sound_->objectID, {ProjectItem::Type::Bus, {ProjectItem::Type::Sound, ProjectItem::Type::Bus}}));
    EXPECT_FALSE(adapter.exists(0));

    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);
    w.setCurrentObjectID(busId);
    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), adapter.displayName(busId));
    EXPECT_EQ(w.toolTip(), adapter.tooltip(busId));

    const IRefDomainAdapter& audio = refDomainAdapter(RefDomain::AudioAsset);
    EXPECT_FALSE(audio.exists(busId));
    EXPECT_TRUE(audio.displayName(busId).isEmpty());
    EXPECT_TRUE(audio.tooltip(busId).isEmpty());
}

TEST_F(RefDomainAdapterTest, ProjectObjectPopulateWritesRoles)
{
    const IRefDomainAdapter& adapter = refDomainAdapter(RefDomain::ProjectObject);
    QTreeWidget tree;
    SelectorRequest req;
    req.filterType = ProjectItem::Type::Bus;
    req.currentObjectID = childBus_->objectID;
    adapter.populate(&tree, req);

    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (found || !item)
            return;
        if (item->data(0, kRoleObjectID).toUInt() == childBus_->objectID) {
            found = item;
            return;
        }
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };
    for (int i = 0; i < tree.topLevelItemCount(); ++i)
        walk(tree.topLevelItem(i));

    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->data(0, kRoleSelectable).toBool());
    EXPECT_EQ(found->text(0), childBus_->name);
}

TEST_F(RefDomainAdapterTest, DirectChildScopeRetainsParentContextWhileLimitingAcceptance)
{
    auto& manager = ProjectManager::instance();
    auto* direct = manager.createItem(childBus_, QStringLiteral("DirectBus"), ProjectItem::Type::Bus);
    auto* branch = manager.createItem(childBus_, QStringLiteral("BranchBus"), ProjectItem::Type::Bus);
    ASSERT_NE(direct, nullptr);
    ASSERT_NE(branch, nullptr);
    auto* grandchild = manager.createItem(branch, QStringLiteral("GrandchildBus"), ProjectItem::Type::Bus);
    ASSERT_NE(grandchild, nullptr);

    ObjectRefWidget reference;
    reference.setFilterType(ProjectItem::Type::Bus);
    reference.setDirectChildParentGuid(childBus_->guid);
    reference.setIsSelectable([branch](uint32_t id) { return id != branch->objectID; });
    reference.setCurrentObjectID(direct->objectID);
    EXPECT_TRUE(reference.canAcceptObject(direct->objectID));
    EXPECT_FALSE(reference.canAcceptObject(branch->objectID));
    EXPECT_FALSE(reference.canAcceptObject(childBus_->objectID));
    EXPECT_FALSE(reference.canAcceptObject(masterBus_->objectID));
    EXPECT_FALSE(reference.canAcceptObject(grandchild->objectID));
    reference.openSelector();
    SelectorWindow* selector = nullptr;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* candidate = qobject_cast<SelectorWindow*>(widget); candidate && candidate->isVisible())
            selector = candidate;
    ASSERT_NE(selector, nullptr);
    auto* tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    QSet<uint32_t> displayed;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const uint32_t id = (*it)->data(0, kRoleObjectID).toUInt();
        displayed.insert(id);
        EXPECT_EQ((*it)->data(0, kRoleSelectable).toBool(), reference.canAcceptObject(id));
    }
    EXPECT_EQ(displayed, (QSet<uint32_t>{childBus_->objectID, direct->objectID, branch->objectID}));
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->data(0, kRoleObjectID).toUInt(), childBus_->objectID);
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->parent(), tree->topLevelItem(0));
    EXPECT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), direct->objectID);
    EXPECT_TRUE(tree->currentItem()->isSelected());
    int changes = 0;
    QObject::connect(&reference, &ObjectRefWidget::objectChanged, &reference, [&changes] { ++changes; });
    selector->accept();
    EXPECT_EQ(changes, 0);

    reference.setDirectChildParentGuid(direct->guid);
    reference.openSelector();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    SelectorWindow::dismiss();
}

TEST_F(RefDomainAdapterTest, ProjectObjectSearchMatchesNamesAndPathsWithoutChangingIdentity)
{
    auto& manager = ProjectManager::instance();
    auto* alternate = manager.createItem(nullptr, QStringLiteral("AlternateLayer"), ProjectItem::Type::Layer);
    ASSERT_NE(alternate, nullptr);
    auto* sameName = manager.createItem(alternate, sound_->name, ProjectItem::Type::Sound);
    auto* unique = manager.createItem(alternate, QStringLiteral("UniqueSound"), ProjectItem::Type::Sound);
    ASSERT_NE(sameName, nullptr);
    ASSERT_NE(unique, nullptr);
    ASSERT_EQ(sameName->name, sound_->name);
    QWidget anchor;
    uint32_t selected = 0;
    SelectorRequest request;
    request.filterType = ProjectItem::Type::Sound;
    request.onSelected = [&](uint32_t id) { selected = id; };
    auto* selector = SelectorWindow::open(request, &anchor);
    auto* tree = selector->findChild<QTreeWidget*>();
    auto* search = selector->findChild<QLineEdit*>();
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(search, nullptr);
    const auto expectMatches = [&](const QString& query, const QSet<uint32_t>& expected) {
        SCOPED_TRACE(query.toStdString());
        search->setText(query);
        QSet<uint32_t> actual;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->isHidden() || !(*it)->data(0, kRoleSelectable).toBool()) continue;
            const uint32_t id = (*it)->data(0, kRoleObjectID).toUInt();
            actual.insert(id);
            auto* item = manager.findItemByObjectID(id);
            ASSERT_NE(item, nullptr);
            EXPECT_EQ((*it)->text(0), item->name);
        }
        EXPECT_EQ(actual, expected);
    };
    expectMatches(sound_->name, {sound_->objectID, sameName->objectID});
    expectMatches(QStringLiteral(" adapterlayer "), {sound_->objectID});
    expectMatches(alternate->name, {sameName->objectID, unique->objectID});
    expectMatches(manager.currentProject()->itemPath(unique), {unique->objectID});
    expectMatches(QStringLiteral("AlternateLayer/AdapterSound"), {sameName->objectID});
    expectMatches(manager.currentProject()->itemPath(sound_), {sound_->objectID});
    QTreeWidgetItem* target = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, kRoleObjectID).toUInt() == sound_->objectID) target = *it;
    ASSERT_NE(target, nullptr);
    tree->setCurrentItem(target);
    selector->accept();
    EXPECT_EQ(selected, sound_->objectID);
    SelectorWindow::dismiss(&anchor);
}

TEST_F(RefDomainAdapterTest, UnselectableParentsLookDisabledWhileChildrenAndBucketNavigationRemainUsable)
{
    for (bool toggleMode : {false, true}) {
        SCOPED_TRACE(toggleMode);
        QWidget anchor;
        uint32_t selected = 0;
        bool toggled = false;
        SelectorRequest request;
        request.filterType = ProjectItem::Type::Bus;
        request.isSelectable = [&](uint32_t id) { return id != masterBus_->objectID; };
        request.onSelected = [&](uint32_t id) { selected = id; };
        if (toggleMode) {
            request.checkState = [](uint32_t) { return Qt::Unchecked; };
            request.onToggled = [&](uint32_t id, bool checked) {
                selected = id;
                toggled = checked;
            };
        }
        auto* selector = SelectorWindow::open(request, &anchor);
        auto* tree = selector->findChild<QTreeWidget*>();
        ASSERT_NE(tree, nullptr);
        QTreeWidgetItem* parent = nullptr;
        QTreeWidgetItem* child = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const uint32_t id = (*it)->data(0, kRoleObjectID).toUInt();
            if (id == masterBus_->objectID) parent = *it;
            if (id == childBus_->objectID) child = *it;
        }
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(child, nullptr);
        ASSERT_EQ(child->parent(), parent);
        EXPECT_TRUE(parent->flags() & Qt::ItemIsEnabled);
        EXPECT_FALSE(parent->flags() & Qt::ItemIsSelectable);
        if (toggleMode) EXPECT_FALSE(parent->flags() & Qt::ItemIsUserCheckable);
        EXPECT_TRUE(child->flags() & Qt::ItemIsEnabled);
        EXPECT_TRUE(child->flags() & Qt::ItemIsSelectable);
        ASSERT_FALSE(parent->icon(0).isNull());
        ASSERT_FALSE(child->icon(0).isNull());
        auto* bucket = parent->parent();
        ASSERT_NE(bucket, nullptr);
        EXPECT_TRUE(bucket->data(0, TreeRowStyle::kBucketRole).toBool());
        EXPECT_TRUE(bucket->flags() & Qt::ItemIsEnabled);
        ASSERT_TRUE(bucket->isExpanded());
        emit tree->itemClicked(bucket, 0);
        EXPECT_FALSE(bucket->isExpanded());
        emit tree->itemClicked(bucket, 0);
        EXPECT_TRUE(bucket->isExpanded());
        parent->setExpanded(false);
        EXPECT_TRUE(tree->visualItemRect(child).isEmpty());
        parent->setExpanded(true);
        EXPECT_FALSE(tree->visualItemRect(child).isEmpty());

        TreeRowStyle::ItemRowDelegate reference;
        const auto render = [&](QAbstractItemDelegate* delegate, QTreeWidgetItem* item, QStyle::State state) {
            QImage image(240, 32, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QStyleOptionViewItem option;
            option.initFrom(tree);
            option.widget = tree;
            option.rect = image.rect();
            option.decorationSize = tree->iconSize();
            option.state = state;
            QPainter painter(&image);
            delegate->paint(&painter, option, tree->indexFromItem(item));
            return image;
        };
        for (const QStyle::State state : {QStyle::State(QStyle::State_Enabled),
                                         QStyle::State_Enabled | QStyle::State_MouseOver}) {
            SCOPED_TRACE(int(state));
            const auto parentDisabled = render(&reference, parent, state & ~QStyle::State_Enabled);
            EXPECT_NE(parentDisabled, render(&reference, parent, state));
            EXPECT_EQ(render(tree->itemDelegate(), parent, state), parentDisabled);
            EXPECT_EQ(render(tree->itemDelegate(), child, state), render(&reference, child, state));
            EXPECT_EQ(render(tree->itemDelegate(), bucket, state), render(&reference, bucket, state));
        }

        tree->setCurrentItem(child);
        if (toggleMode) {
            child->setCheckState(1, Qt::Checked);
            EXPECT_TRUE(toggled);
        } else {
            selector->accept();
        }
        EXPECT_EQ(selected, childBus_->objectID);
        SelectorWindow::dismiss(&anchor);
    }
}
