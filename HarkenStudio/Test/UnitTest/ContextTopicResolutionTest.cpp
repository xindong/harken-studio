#include "Core/ContextInfo.h"
#include "Core/ContextInfoService.h"
#include "Core/ProjectManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEventLoop>
#include <QHoverEvent>
#include <QPointF>
#include <QUuid>
#include <QWidget>

#include <memory>
#include <vector>

using VNX::ContextInfoService;
using VNX::ContextTopic;
using VNX::IContextInfoSource;
using VNX::ProjectItem;
using VNX::ProjectManager;
namespace ContextInfo = VNX::ContextInfo;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

constexpr int kSettleMs = 400;

void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void sendEnter(QWidget* widget)
{
    QEnterEvent event(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QCoreApplication::sendEvent(widget, &event);
}

void sendHoverMove(QWidget* widget)
{
    QHoverEvent event(QEvent::HoverMove, QPointF(1, 1), QPointF(1, 1), QPointF(0, 0));
    QCoreApplication::sendEvent(widget, &event);
}

ContextTopic objectTopic(const QString& name)
{
    return ContextTopic::forObject(QUuid::createUuid(), ProjectItem::Type::Sound, name);
}

class SourceWidget : public QWidget, public IContextInfoSource
{
public:
    explicit SourceWidget(QWidget* parent = nullptr) : QWidget(parent) {}

    ContextTopic answer;

    ContextTopic contextTopicAt(const QPoint&) const override { return answer; }
};

class ContextTopicResolution : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        service.setActive(true);
        QObject::connect(&service, &ContextInfoService::topicChanged,
                         [this](const ContextTopic& topic) { seen.push_back(topic); });
    }

    std::vector<ContextTopic> seen;
    ContextInfoService service;
};

}

TEST(ContextInfoTag, RoundTripsThroughAWidgetProperty)
{
    ensureApp();

    QWidget widget;
    EXPECT_FALSE(ContextInfo::tagOf(&widget).has_value());
    EXPECT_FALSE(ContextInfo::tagOf(nullptr).has_value());

    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(&widget, topic);

    const auto read = ContextInfo::tagOf(&widget);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, topic);

    ContextInfo::clearTag(&widget);
    EXPECT_FALSE(ContextInfo::tagOf(&widget).has_value());
}

TEST_F(ContextTopicResolution, HoverOverATaggedWidgetRaisesItsTopic)
{
    QWidget widget;
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(&widget, topic);

    sendEnter(&widget);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), topic);
    EXPECT_EQ(service.currentTopic(), topic);
}

TEST_F(ContextTopicResolution, TopicComesFromTheNearestTaggedAncestor)
{
    QWidget page;
    auto* row = new QWidget(&page);
    auto* control = new QWidget(row);

    const ContextTopic pageTopic = objectTopic(QStringLiteral("Rain_Loop"));
    const ContextTopic rowTopic = ContextTopic::forField(
        VNX::ComponentType::Audio, QStringLiteral("volume"), QStringLiteral("Volume"),
        QUuid::createUuid(), ProjectItem::Type::Sound, QStringLiteral("Rain_Loop"));
    ContextInfo::tag(&page, pageTopic);
    ContextInfo::tag(row, rowTopic);

    sendEnter(control);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), rowTopic);
}

TEST_F(ContextTopicResolution, ASourceAnswersBeforeAStaticTagOnTheSameWidget)
{
    SourceWidget source;
    source.answer = objectTopic(QStringLiteral("FromSource"));
    ContextInfo::tag(&source, objectTopic(QStringLiteral("FromTag")));

    sendEnter(&source);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front().objectName, QStringLiteral("FromSource"));
}

TEST_F(ContextTopicResolution, ASourceWithNoAnswerFallsThroughToAnAncestorTag)
{
    QWidget page;
    auto* source = new SourceWidget(&page);
    const ContextTopic pageTopic = objectTopic(QStringLiteral("Layers"));
    ContextInfo::tag(&page, pageTopic);

    sendEnter(source);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), pageTopic);
}

TEST_F(ContextTopicResolution, ASourceAnswersHoverMovesToo)
{
    SourceWidget source;
    source.answer = objectTopic(QStringLiteral("Rain_Loop"));

    sendHoverMove(&source);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), source.answer);
}

TEST_F(ContextTopicResolution, EnteringASourceArmsHoverTracking)
{
    QWidget plain;
    sendEnter(&plain);
    EXPECT_FALSE(plain.testAttribute(Qt::WA_Hover));

    SourceWidget source;
    EXPECT_FALSE(source.testAttribute(Qt::WA_Hover));
    sendEnter(&source);
    EXPECT_TRUE(source.testAttribute(Qt::WA_Hover));
}

TEST_F(ContextTopicResolution, HoverOverUnmarkedGroundKeepsTheLastTopic)
{
    QWidget tagged;
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(&tagged, topic);
    sendEnter(&tagged);
    spin(kSettleMs);

    QWidget bare;
    sendEnter(&bare);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(service.currentTopic(), topic);
}

TEST_F(ContextTopicResolution, TheSameTopicIsPublishedOnlyOnce)
{
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    QWidget first;
    QWidget second;
    ContextInfo::tag(&first, topic);
    ContextInfo::tag(&second, topic);

    sendEnter(&first);
    spin(kSettleMs);
    sendEnter(&second);
    spin(kSettleMs);

    EXPECT_EQ(seen.size(), 1u);
}

TEST_F(ContextTopicResolution, TheDebouncePublishesOnlyTheTopicHoveredLast)
{
    QWidget first;
    QWidget second;
    const ContextTopic firstTopic = objectTopic(QStringLiteral("First"));
    const ContextTopic secondTopic = objectTopic(QStringLiteral("Second"));
    ContextInfo::tag(&first, firstTopic);
    ContextInfo::tag(&second, secondTopic);

    sendEnter(&first);
    sendEnter(&second);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), secondTopic);
}

TEST_F(ContextTopicResolution, MovingToUnmarkedGroundLeavesThePendingTopicAlone)
{
    QWidget tagged;
    QWidget bare;
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(&tagged, topic);

    sendEnter(&tagged);
    sendEnter(&bare);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(service.currentTopic(), topic);
}

// Qt delivers HoverMove to every WA_Hover widget up the parent chain, so an
// ancestor is asked about the same pointer position as the row under it.
TEST_F(ContextTopicResolution, AnAncestorAnsweringNothingDoesNotCancelTheRowBelowIt)
{
    QWidget host;
    auto* row = new QWidget(&host);
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(row, topic);

    sendHoverMove(row);
    sendHoverMove(&host);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), topic);
}

TEST_F(ContextTopicResolution, AnAncestorReportIsResolvedAgainstThePointerNotTheReceiver)
{
    QWidget host;
    host.resize(200, 200);
    auto* row = new QWidget(&host);
    row->setGeometry(0, 0, 100, 40);
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(row, topic);

    sendHoverMove(&host);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), topic);
}

TEST_F(ContextTopicResolution, ATopicOnAnAncestorNeverOutranksTheRowUnderThePointer)
{
    QWidget host;
    host.resize(200, 200);
    ContextInfo::tag(&host, objectTopic(QStringLiteral("Enclosing_Panel")));

    auto* row = new QWidget(&host);
    row->setGeometry(0, 0, 100, 40);
    const ContextTopic rowTopic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(row, rowTopic);

    sendHoverMove(row);
    sendHoverMove(&host);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), rowTopic);
}

TEST_F(ContextTopicResolution, ADriftingPointerOnOneRowStillPublishesIt)
{
    QWidget tagged;
    const ContextTopic topic = objectTopic(QStringLiteral("Rain_Loop"));
    ContextInfo::tag(&tagged, topic);

    for (int i = 0; i < 12; ++i)
    {
        sendHoverMove(&tagged);
        spin(40);
    }

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), topic);
}

TEST_F(ContextTopicResolution, AnInactiveServiceIgnoresHover)
{
    service.setActive(false);

    QWidget widget;
    ContextInfo::tag(&widget, objectTopic(QStringLiteral("Rain_Loop")));
    sendEnter(&widget);
    spin(kSettleMs);

    EXPECT_TRUE(seen.empty());
    EXPECT_TRUE(service.currentTopic().isNone());
}

TEST_F(ContextTopicResolution, TaggedModalAllowsItsControlsToPublishHelp)
{
    QDialog modal;
    modal.setModal(true);
    ContextInfo::tag(&modal, ContextTopic::forPanel(QStringLiteral("modal")));
    QWidget control(&modal);
    const auto topic = ContextTopic::forPanel(QStringLiteral("control"));
    ContextInfo::tag(&control, topic);
    modal.show();
    QCoreApplication::processEvents();
    ASSERT_EQ(QApplication::activeModalWidget(), &modal);
    service.clearTopic();

    sendEnter(&control);
    spin(kSettleMs);
    EXPECT_EQ(service.currentTopic(), topic);

    QDialog nested(&modal);
    nested.setModal(true);
    QWidget nestedControl(&nested);
    const auto nestedTopic = ContextTopic::forPanel(QStringLiteral("nested.control"));
    ContextInfo::tag(&nestedControl, nestedTopic);
    nested.show();
    QCoreApplication::processEvents();
    ASSERT_EQ(QApplication::activeModalWidget(), &nested);
    const auto heldTopic = service.currentTopic();

    sendEnter(&nestedControl);
    spin(kSettleMs);
    EXPECT_EQ(service.currentTopic(), heldTopic);

    nested.hide();
    service.clearTopic();
    sendHoverMove(&control);
    spin(kSettleMs);
    EXPECT_EQ(service.currentTopic(), topic);
}

TEST_F(ContextTopicResolution, TaggedModalBlocksPendingAndNewBackgroundHover)
{
    QWidget background;
    const auto topic = objectTopic(QStringLiteral("Background"));
    ContextInfo::tag(&background, topic);
    sendEnter(&background);

    QDialog modal(nullptr, Qt::Dialog | Qt::WindowDoesNotAcceptFocus);
    modal.setModal(true);
    ContextInfo::tag(&modal, ContextTopic::forPanel(QStringLiteral("modal")));
    modal.move(200, 200);
    modal.show();
    ASSERT_EQ(QApplication::activeModalWidget(), &modal);
    spin(kSettleMs);
    EXPECT_NE(service.currentTopic(), topic);

    sendHoverMove(&background);
    spin(kSettleMs);
    EXPECT_NE(service.currentTopic(), topic);
}

TEST_F(ContextTopicResolution, DeletingTheHoveredControlDropsItsPendingHelp)
{
    auto widget = std::make_unique<QWidget>();
    ContextInfo::tag(widget.get(), objectTopic(QStringLiteral("Pending")));
    sendEnter(widget.get());
    widget.reset();
    spin(kSettleMs);
    EXPECT_TRUE(seen.empty());
}

TEST_F(ContextTopicResolution, DeactivatingDropsThePendingTopic)
{
    QWidget widget;
    ContextInfo::tag(&widget, objectTopic(QStringLiteral("Rain_Loop")));

    sendEnter(&widget);
    service.setActive(false);
    spin(kSettleMs);

    EXPECT_TRUE(seen.empty());
}

TEST_F(ContextTopicResolution, LockingPinsTheCurrentTopic)
{
    QWidget first;
    QWidget second;
    const ContextTopic firstTopic = objectTopic(QStringLiteral("First"));
    const ContextTopic secondTopic = objectTopic(QStringLiteral("Second"));
    ContextInfo::tag(&first, firstTopic);
    ContextInfo::tag(&second, secondTopic);

    sendEnter(&first);
    spin(kSettleMs);

    service.setLocked(true);
    EXPECT_FALSE(service.isActive());
    sendEnter(&second);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(service.currentTopic(), firstTopic);

    service.setLocked(false);
    EXPECT_TRUE(service.isActive());
    sendEnter(&second);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(service.currentTopic(), secondTopic);
}

TEST_F(ContextTopicResolution, TheFreezeRootStopsTheWalkUpTheParentChain)
{
    QWidget window;
    const ContextTopic windowTopic = objectTopic(QStringLiteral("Workspace"));
    ContextInfo::tag(&window, windowTopic);
    auto* panel = new QWidget(&window);
    auto* body = new QWidget(panel);

    service.setFreezeRoot(panel);
    sendEnter(body);
    spin(kSettleMs);

    EXPECT_TRUE(seen.empty());

    service.setFreezeRoot(nullptr);
    sendEnter(body);
    spin(kSettleMs);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), windowTopic);
}

TEST_F(ContextTopicResolution, ClearTopicFallsBackToTheEmptyTopic)
{
    QWidget widget;
    ContextInfo::tag(&widget, objectTopic(QStringLiteral("Rain_Loop")));
    sendEnter(&widget);
    spin(kSettleMs);
    ASSERT_EQ(seen.size(), 1u);

    service.clearTopic();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.back().isNone());
    EXPECT_TRUE(service.currentTopic().isNone());

    service.clearTopic();
    EXPECT_EQ(seen.size(), 2u);
}

TEST_F(ContextTopicResolution, ProjectCloseClearsTheCurrentTopic)
{
    QWidget widget;
    ContextInfo::tag(&widget, objectTopic(QStringLiteral("Rain_Loop")));
    sendEnter(&widget);
    spin(kSettleMs);

    ProjectManager::instance().projectClosed();

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.back().isNone());
    EXPECT_TRUE(service.currentTopic().isNone());
}

TEST_F(ContextTopicResolution, ProjectCloseDropsThePendingTopic)
{
    QWidget widget;
    ContextInfo::tag(&widget, objectTopic(QStringLiteral("Rain_Loop")));
    sendEnter(&widget);

    ProjectManager::instance().projectClosed();
    spin(kSettleMs);

    EXPECT_TRUE(seen.empty());
    EXPECT_TRUE(service.currentTopic().isNone());
}

TEST_F(ContextTopicResolution, DeletingTheCurrentObjectsSubtreeClearsTheTopic)
{
    ProjectItem root(QStringLiteral("Root"), ProjectItem::Type::Layer);
    auto child = std::make_shared<ProjectItem>(QStringLiteral("Rain_Loop"),
                                               ProjectItem::Type::Sound);
    child->guid = QUuid::createUuid();
    root.children.push_back(child);

    QWidget widget;
    ContextInfo::tag(&widget, ContextTopic::forObject(
        child->guid, child->type, child->name));
    sendEnter(&widget);
    spin(kSettleMs);

    ProjectManager::instance().itemDeleted(&root);

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.back().isNone());
    EXPECT_TRUE(service.currentTopic().isNone());
}

TEST_F(ContextTopicResolution, DeletingThePendingObjectsSubtreeDropsTheTopic)
{
    ProjectItem root(QStringLiteral("Root"), ProjectItem::Type::Layer);
    auto child = std::make_shared<ProjectItem>(QStringLiteral("Rain_Loop"),
                                               ProjectItem::Type::Sound);
    child->guid = QUuid::createUuid();
    root.children.push_back(child);

    QWidget widget;
    ContextInfo::tag(&widget, ContextTopic::forObject(
        child->guid, child->type, child->name));
    sendEnter(&widget);

    ProjectManager::instance().itemDeleted(&root);
    spin(kSettleMs);

    EXPECT_TRUE(seen.empty());
    EXPECT_TRUE(service.currentTopic().isNone());
}
