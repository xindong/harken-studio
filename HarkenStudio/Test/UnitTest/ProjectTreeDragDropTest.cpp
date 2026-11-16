#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/ProjectTree/ProjectTreePanel.h"
#include "Widgets/Shared/TreeExpansionWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMimeData>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <memory>

#ifdef Q_OS_WIN
#include <QHBoxLayout>
#include <QWindow>
#include <qt_windows.h>
#endif

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

void processEventsFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template<typename Predicate>
bool waitFor(Predicate&& predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 2000)
        processEventsFor(10);
    return predicate();
}

class DragNavigationTree : public TreeExpansionWidget
{
public:
    DragNavigationTree()
    {
        setAcceptDrops(true);
        setHeaderHidden(true);
        resize(300, 180);
    }

    QPersistentModelIndex dragTarget;
    QPersistentModelIndex dropTarget;
    bool targetAccepted = false;
    int wheelCount = 0;

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        ++wheelCount;
        TreeExpansionWidget::wheelEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        dragTarget = indexAt(event->position().toPoint());
        targetAccepted = dragTarget.data(Qt::UserRole).toBool();
        event->setAccepted(targetAccepted);
    }

    void dropEvent(QDropEvent* event) override
    {
        const auto index = indexAt(event->position().toPoint());
        if (index.data(Qt::UserRole).toBool()) {
            dropTarget = index;
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }
};

class ProjectTreeDragDropTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        ProjectConfig config;
        config.name = QStringLiteral("DragDropProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        auto& pm = ProjectManager::instance();
        layerRoot_ = pm.createItem(nullptr, QStringLiteral("Drag_Root"), ProjectItem::Type::Layer);
        ASSERT_NE(layerRoot_, nullptr);
        subLayer_ = pm.createItem(layerRoot_, QStringLiteral("Drag_Sub"), ProjectItem::Type::Layer);
        ASSERT_NE(subLayer_, nullptr);
        subSubLayer_ = pm.createItem(subLayer_, QStringLiteral("Drag_SubSub"), ProjectItem::Type::Layer);
        ASSERT_NE(subSubLayer_, nullptr);
        layerAlt_ = pm.createItem(nullptr, QStringLiteral("Drag_Alt"), ProjectItem::Type::Layer);
        ASSERT_NE(layerAlt_, nullptr);
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;

    ProjectItem* layerRoot_ = nullptr;
    ProjectItem* subLayer_ = nullptr;
    ProjectItem* subSubLayer_ = nullptr;
    ProjectItem* layerAlt_ = nullptr;
};

}

TEST_F(ProjectTreeDragDropTest, WouldCreateCycleWalksTargetAncestorChain)
{
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(layerRoot_, layerRoot_));
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(layerRoot_, subLayer_));
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(layerRoot_, subSubLayer_));
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(subLayer_, subSubLayer_));

    EXPECT_FALSE(ProjectTreePanel::wouldCreateCycle(subLayer_, layerRoot_));
    EXPECT_FALSE(ProjectTreePanel::wouldCreateCycle(layerRoot_, layerAlt_));
}

TEST(TreeDragNavigation, RejectedTargetsDoNotBlockNavigationOrChangeSelection)
{
    ensureApp();
    for (const auto mode : {QAbstractItemView::ScrollPerItem, QAbstractItemView::ScrollPerPixel}) {
        SCOPED_TRACE(mode);
        DragNavigationTree tree;
        tree.setVerticalScrollMode(mode);
        for (int row = 0; row < 80; ++row)
            new QTreeWidgetItem(&tree, {QString::number(row)});
        tree.setCurrentItem(tree.topLevelItem(0));
        tree.show();
        QApplication::processEvents();
        auto* selected = tree.currentItem();
        const QPoint edge(tree.viewport()->width() / 2, tree.viewport()->height() - 2);
        QMimeData mime;
        QDragEnterEvent enter(edge, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        QDragMoveEvent move(edge, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &move);
        ASSERT_FALSE(move.isAccepted());
        const auto originalTarget = tree.dragTarget;
        ASSERT_TRUE(waitFor([&] { return tree.dragTarget != originalTarget; }));
        EXPECT_GT(tree.verticalScrollBar()->value(), 0);
        EXPECT_EQ(tree.dragTarget, tree.indexAt(edge));
        EXPECT_FALSE(tree.targetAccepted);
        EXPECT_EQ(tree.currentItem(), selected);
        EXPECT_EQ(tree.selectedItems(), QList<QTreeWidgetItem*>{selected});

        QWheelEvent reverse(edge, tree.viewport()->mapToGlobal(edge), {}, QPoint(0, 120),
                            Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(tree.viewport(), &reverse);
        const int wheeledTo = tree.verticalScrollBar()->value();
        processEventsFor(150);
        EXPECT_EQ(tree.verticalScrollBar()->value(), wheeledTo);
        QDragMoveEvent unchanged(edge, Qt::MoveAction, &mime, Qt::LeftButton, Qt::ControlModifier);
        QApplication::sendEvent(tree.viewport(), &unchanged);
        processEventsFor(100);
        EXPECT_EQ(tree.verticalScrollBar()->value(), wheeledTo);
        QDragMoveEvent resume(edge - QPoint(1, 0), Qt::MoveAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &resume);
        ASSERT_TRUE(waitFor([&] { return tree.verticalScrollBar()->value() > wheeledTo; }));

        const QPoint center = tree.viewport()->rect().center();
        QDragMoveEvent middle(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &middle);
        const int stoppedAt = tree.verticalScrollBar()->value();
        processEventsFor(100);
        EXPECT_EQ(tree.verticalScrollBar()->value(), stoppedAt);

        QWheelEvent wheel(center, tree.viewport()->mapToGlobal(center), {}, QPoint(0, -120),
                          Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(tree.viewport(), &wheel);
        EXPECT_GT(tree.verticalScrollBar()->value(), stoppedAt);
        EXPECT_EQ(tree.dragTarget, tree.indexAt(center));
        auto* destination = tree.itemAt(center);
        ASSERT_NE(destination, nullptr);
        destination->setData(0, Qt::UserRole, true);
        QDragMoveEvent valid(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &valid);
        ASSERT_TRUE(valid.isAccepted());
        QDropEvent drop(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &drop);
        EXPECT_TRUE(drop.isAccepted());
        EXPECT_EQ(tree.dropTarget, tree.indexFromItem(destination));
        EXPECT_EQ(tree.selectedItems(), QList<QTreeWidgetItem*>{selected});
    }
}

#ifdef Q_OS_WIN
TEST(TreeDragNavigation, NativeWheelRoutingFollowsDragTargetAndLifetime)
{
    ensureApp();
    if (QApplication::platformName() != QStringLiteral("windows"))
        GTEST_SKIP() << "Requires the Windows message queue";

    for (int finish = 0; finish < 4; ++finish) {
        SCOPED_TRACE(finish);
        QWidget host;
        QHBoxLayout layout(&host);
        DragNavigationTree tree;
        DragNavigationTree other;
        layout.addWidget(&tree);
        layout.addWidget(&other);
        host.resize(620, 200);
        for (int row = 0; row < 80; ++row) {
            new QTreeWidgetItem(&tree, {QString::number(row)});
            new QTreeWidgetItem(&other, {QString::number(row)});
        }
        tree.setCurrentItem(tree.topLevelItem(0));
        host.show();
        QApplication::processEvents();
        const QPoint center = tree.viewport()->rect().center();
        QWheelEvent normal(center, tree.viewport()->mapToGlobal(center), {}, QPoint(0, -120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(tree.viewport(), &normal);
        const int normalDistance = tree.verticalScrollBar()->value();
        ASSERT_GT(normalDistance, 0);
        tree.verticalScrollBar()->setValue(0);
        tree.wheelCount = 0;

        const auto queuedWheel = [&](QWidget* target, int delta) {
            const QPoint logical = target->mapTo(&host, target->rect().center());
            const QPoint native = (QPointF(logical) * host.windowHandle()->devicePixelRatio()).toPoint();
            POINT screen{native.x(), native.y()};
            EXPECT_TRUE(ClientToScreen(reinterpret_cast<HWND>(host.winId()), &screen));
            EXPECT_TRUE(PostThreadMessageW(GetCurrentThreadId(), WM_MOUSEWHEEL,
                                           MAKEWPARAM(MK_LBUTTON, delta),
                                           MAKELPARAM(screen.x, screen.y)));
            MSG msg{};
            for (int peek = 0; peek < 2; ++peek) {
                EXPECT_TRUE(PeekMessageW(&msg, nullptr, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_NOREMOVE));
                EXPECT_EQ(msg.message, UINT(WM_MOUSEWHEEL));
            }
            EXPECT_TRUE(PeekMessageW(&msg, nullptr, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE));
            QApplication::processEvents();
            return msg.message;
        };

        EXPECT_EQ(queuedWheel(tree.viewport(), -120), UINT(WM_MOUSEWHEEL));
        EXPECT_EQ(tree.wheelCount, 0);
        auto mime = std::make_unique<QMimeData>();
        QDragEnterEvent enter(center, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        for (int part = 0; part < 4; ++part)
            EXPECT_EQ(queuedWheel(tree.viewport(), -30), UINT(WM_NULL));
        EXPECT_EQ(tree.wheelCount, 4);
        EXPECT_EQ(tree.verticalScrollBar()->value(), normalDistance);
        EXPECT_EQ(tree.dragTarget, tree.indexAt(center));
        EXPECT_EQ(tree.selectedItems(), QList<QTreeWidgetItem*>{tree.topLevelItem(0)});

        EXPECT_EQ(queuedWheel(other.viewport(), -120), UINT(WM_MOUSEWHEEL));
        EXPECT_EQ(other.wheelCount, 0);
        EXPECT_EQ(tree.wheelCount, 4);

        switch (finish) {
        case 0: {
            QDragLeaveEvent leave;
            QApplication::sendEvent(tree.viewport(), &leave);
            break;
        }
        case 1: {
            QDropEvent drop(center, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree.viewport(), &drop);
            break;
        }
        case 2: tree.hide(); tree.show(); break;
        case 3: mime.reset(); break;
        }
        QApplication::processEvents();
        EXPECT_EQ(queuedWheel(tree.viewport(), -120), UINT(WM_MOUSEWHEEL));
        EXPECT_EQ(tree.wheelCount, 4);
        EXPECT_EQ(tree.verticalScrollBar()->value(), normalDistance);

        const QPoint otherCenter = other.viewport()->rect().center();
        mime = std::make_unique<QMimeData>();
        QDragEnterEvent next(otherCenter, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(other.viewport(), &next);
        ASSERT_TRUE(next.isAccepted());
        EXPECT_EQ(queuedWheel(other.viewport(), -120), UINT(WM_NULL));
        EXPECT_EQ(other.wheelCount, 1);
        EXPECT_GT(other.verticalScrollBar()->value(), 0);
        EXPECT_EQ(tree.wheelCount, 4);
    }
}
#endif

TEST(TreeDragNavigation, HoverNavigationPreservesDescendantAndSelectionState)
{
    ensureApp();
    DragNavigationTree tree;
    auto* parent = new QTreeWidgetItem(&tree, {QStringLiteral("Parent")});
    auto* child = new QTreeWidgetItem(parent, {QStringLiteral("Child")});
    new QTreeWidgetItem(child, {QStringLiteral("Descendant")});
    auto* other = new QTreeWidgetItem(&tree, {QStringLiteral("Other")});
    new QTreeWidgetItem(other, {QStringLiteral("Other child")});
    tree.setCurrentItem(other);
    tree.show();
    QApplication::processEvents();
    const QPoint position = tree.visualItemRect(parent).center();
    QMimeData mime;
    QDragEnterEvent enter(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &enter);
    QDragMoveEvent move(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &move);
    ASSERT_FALSE(move.isAccepted());
    ASSERT_TRUE(waitFor([&] { return parent->isExpanded(); }));
    EXPECT_FALSE(child->isExpanded());
    EXPECT_FALSE(other->isExpanded());
    EXPECT_FALSE(tree.targetAccepted);
    EXPECT_EQ(tree.currentItem(), other);
    EXPECT_EQ(tree.selectedItems(), QList<QTreeWidgetItem*>{other});
    QDragLeaveEvent leave;
    QApplication::sendEvent(tree.viewport(), &leave);
}

TEST(TreeDragNavigation, EndedDragsCannotScrollOrExpandRows)
{
    ensureApp();
    for (int finish = 0; finish < 4; ++finish) {
        SCOPED_TRACE(finish);
        DragNavigationTree tree;
        auto* parent = new QTreeWidgetItem(&tree, {QStringLiteral("Parent")});
        new QTreeWidgetItem(parent, {QStringLiteral("Child")});
        for (int row = 0; row < 60; ++row)
            new QTreeWidgetItem(&tree, {QString::number(row)});
        tree.show();
        QApplication::processEvents();
        auto mime = std::make_unique<QMimeData>();
        const auto endDrag = [&](const QPoint& position) {
            switch (finish) {
            case 0: {
                QDragLeaveEvent leave;
                QApplication::sendEvent(tree.viewport(), &leave);
                break;
            }
            case 1: {
                QDropEvent drop(position, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree.viewport(), &drop);
                break;
            }
            case 2: tree.hide(); break;
            case 3: mime.reset(); break;
            }
        };
        const QPoint hover = tree.visualItemRect(parent).center();
        QDragEnterEvent enter(hover, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        endDrag(hover);
        processEventsFor(700);
        EXPECT_FALSE(parent->isExpanded());

        mime = std::make_unique<QMimeData>();
        tree.show();
        QApplication::processEvents();
        const QPoint edge(tree.viewport()->width() / 2, tree.viewport()->height() - 2);
        QDragEnterEvent next(edge, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree.viewport(), &next);
        ASSERT_TRUE(waitFor([&] { return tree.verticalScrollBar()->value() > 0; }));
        endDrag(edge);
        const int stoppedAt = tree.verticalScrollBar()->value();
        processEventsFor(100);
        EXPECT_EQ(tree.verticalScrollBar()->value(), stoppedAt);
    }
}
