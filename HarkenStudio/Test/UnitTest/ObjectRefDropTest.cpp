#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/SelectorWindow.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QScreen>
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
        ProjectManager::instance().closeProject();
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
