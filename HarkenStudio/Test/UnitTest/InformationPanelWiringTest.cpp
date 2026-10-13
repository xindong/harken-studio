#include "Core/ContextInfo.h"
#include "Core/ContextInfoService.h"
#include "Core/WorkspaceWindowManager.h"
#include "Core/WorkspaceWindowTypes.h"
#include "Widgets/Information/InformationPanel.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"

#include <gtest/gtest.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEventLoop>
#include <QLabel>
#include <QMainWindow>
#include <QPointF>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

using VNX::ContextInfoService;
using VNX::ContextTopic;
using VNX::InformationPanel;
using VNX::ProjectItem;
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

QString headerText(InformationPanel* panel)
{
    auto* label = panel->findChild<QLabel*>(QStringLiteral("informationHeaderLabel"));
    return label ? label->text() : QString();
}

QString bodyText(InformationPanel* panel)
{
    auto* label = panel->findChild<QLabel*>(QStringLiteral("informationBodyLabel"));
    return label ? label->text() : QString();
}

class PanelWiring : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ContextInfoService::instance().setLocked(false);
        ContextInfoService::instance().clearTopic();
    }

    void TearDown() override
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ContextInfoService::instance().setFreezeRoot(nullptr);
        ContextInfoService::instance().setActive(false);
        ContextInfoService::instance().setLocked(false);
        ContextInfoService::instance().clearTopic();
    }
};

}

TEST_F(PanelWiring, ShowingThePanelActivatesTheSharedService)
{
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* panel = new InformationPanel(&host);
    layout->addWidget(panel);

    EXPECT_FALSE(ContextInfoService::instance().isActive());

    host.show();
    spin(100);

    EXPECT_TRUE(ContextInfoService::instance().isActive());

    host.hide();
    spin(50);

    EXPECT_FALSE(ContextInfoService::instance().isActive());
}

TEST_F(PanelWiring, HoveringATaggedWidgetRendersItsTopicInThePanel)
{
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* panel = new InformationPanel(&host);
    layout->addWidget(panel);
    host.show();
    spin(100);

    ASSERT_TRUE(ContextInfoService::instance().isActive());

    QWidget page;
    auto* row = new QWidget(&page);
    ContextInfo::tag(row, ContextTopic::forObject(QUuid::createUuid(),
                                                  ProjectItem::Type::Sound,
                                                  QStringLiteral("Rain_Loop")));
    page.show();
    spin(50);

    sendEnter(row);
    spin(400);

    EXPECT_TRUE(headerText(panel).contains(QStringLiteral("Rain_Loop")))
        << "header was: " << headerText(panel).toStdString();
    EXPECT_FALSE(bodyText(panel).isEmpty());
}

TEST_F(PanelWiring, ThePanelActivatesTheServiceFromInsideAnAdsDock)
{
    QMainWindow window;
    window.resize(1200, 800);
    auto* manager = new ads::CDockManager(&window);

    auto* explorerDock = new ads::CDockWidget(QStringLiteral("ExplorerDock"));
    explorerDock->setObjectName(QStringLiteral("ExplorerDock"));
    explorerDock->setWidget(new QLabel(QStringLiteral("explorer")));
    manager->addDockWidget(ads::LeftDockWidgetArea, explorerDock);

    auto* informationDock = new ads::CDockWidget(QStringLiteral("InformationDock"));
    informationDock->setObjectName(QStringLiteral("InformationDock"));
    manager->addDockWidget(ads::BottomDockWidgetArea, informationDock,
                           explorerDock->dockAreaWidget());

    auto* panel = new InformationPanel();
    informationDock->setWidget(panel);

    window.show();
    spin(200);

    ASSERT_FALSE(informationDock->isClosed());
    EXPECT_TRUE(panel->isVisible());
    EXPECT_TRUE(ContextInfoService::instance().isActive());

    QWidget page;
    auto* row = new QWidget(&page);
    ContextInfo::tag(row, ContextTopic::forObject(QUuid::createUuid(),
                                                  ProjectItem::Type::Sound,
                                                  QStringLiteral("Rain_Loop")));
    page.show();
    spin(50);

    sendEnter(row);
    spin(400);

    EXPECT_TRUE(headerText(panel).contains(QStringLiteral("Rain_Loop")))
        << "header was: " << headerText(panel).toStdString();

    window.hide();
    spin(50);
}

TEST_F(PanelWiring, TheProjectTreeResolvesAsAContextInfoSource)
{
    VNX::ProjectTreeWidget tree;
    QWidget* asWidget = &tree;
    EXPECT_NE(dynamic_cast<VNX::IContextInfoSource*>(asWidget), nullptr);
    EXPECT_EQ(tree.viewport()->parentWidget(), asWidget);
}

TEST_F(PanelWiring, TheWorkspaceManagerMaterializesTheInformationPanelOnStartup)
{
    QMainWindow window;
    window.resize(1400, 900);
    auto* manager = new ads::CDockManager(&window);

    VNX::WorkspaceWindowManager wm(VNX::defaultWorkspaceWindowCatalog(),
                                   VNX::QtAdsWindowAdapter(manager, {}));

    auto* explorerDock = new ads::CDockWidget(QStringLiteral("Explorer"));
    explorerDock->setObjectName(QStringLiteral("ExplorerDock"));
    explorerDock->setWidget(new QLabel(QStringLiteral("explorer")));
    manager->addDockWidget(ads::LeftDockWidgetArea, explorerDock);
    wm.registerSingletons({{QStringLiteral("Explorer"), explorerDock}});

    wm.registerSingletonFactory(QStringLiteral("Information"),
                                {[]() -> QWidget* { return new InformationPanel(); }, {}, {}});
    wm.ensureSingletons({QStringLiteral("Information")});

    ads::CDockWidget* dock = wm.singletonDock(QStringLiteral("Information"));
    ASSERT_NE(dock, nullptr);
    EXPECT_FALSE(dock->isClosed());
    EXPECT_EQ(dock->widget(), nullptr);

    EXPECT_GT(wm.materializeVisibleSingletons(), 0);
    ASSERT_NE(dock->widget(), nullptr);

    window.show();
    spin(200);

    EXPECT_TRUE(dock->widget()->isVisible());
    EXPECT_TRUE(ContextInfoService::instance().isActive());

    window.hide();
    spin(50);
}
