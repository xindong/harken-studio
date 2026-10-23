#include "Common/ToastPresenter.h"

#include <QApplication>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>

#include <gtest/gtest.h>

#include <memory>

namespace {

void waitForEvents(int durationMs)
{
    QEventLoop loop;
    QTimer::singleShot(durationMs, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();
}

}

TEST(ToastPresenterTest, OverlayStaysWithinHostAndDoesNotInterceptInput)
{
    QWidget host;
    host.resize(800, 600);
    VNX::ToastPresenter toast(&host);
    toast.setViewportMargins(QMargins(180, 40, 0, 0));
    host.show();
    toast.showMessage(QStringLiteral("This connection is unavailable."));

    EXPECT_TRUE(toast.isVisible());
    EXPECT_FALSE(toast.isWindow());
    EXPECT_EQ(toast.focusPolicy(), Qt::NoFocus);
    EXPECT_TRUE(toast.testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_TRUE(host.rect().marginsRemoved(QMargins(180, 40, 0, 0)).contains(toast.geometry()));
    EXPECT_EQ(host.childAt(toast.geometry().center()), nullptr);

    const auto previousCenter = toast.geometry().center();
    host.resize(450, 300);
    QApplication::processEvents();
    EXPECT_TRUE(host.rect().marginsRemoved(QMargins(180, 40, 0, 0)).contains(toast.geometry()));
    EXPECT_LT(toast.geometry().center().x(), previousCenter.x());
}

TEST(ToastPresenterTest, HiddenHostDiscardsFeedbackAndNeverReplaysIt)
{
    QWidget host;
    host.resize(800, 600);
    VNX::ToastPresenter toast(&host);
    host.show();
    toast.showMessage(QStringLiteral("Before hiding"));
    ASSERT_TRUE(toast.isVisible());

    host.hide();
    host.show();
    EXPECT_FALSE(toast.isVisible());

    host.hide();
    toast.showMessage(QStringLiteral("While hidden"));
    host.show();
    EXPECT_FALSE(toast.isVisible());

    toast.showMessage(QStringLiteral("After reopening"));
    EXPECT_TRUE(toast.isVisible());
}

TEST(ToastPresenterTest, HostsOwnIndependentFeedbackLifetimes)
{
    auto firstHost = std::make_unique<QWidget>();
    firstHost->resize(800, 600);
    firstHost->show();
    QPointer<VNX::ToastPresenter> firstToast = new VNX::ToastPresenter(firstHost.get());
    firstToast->showMessage(QStringLiteral("First view"));

    QWidget secondHost;
    secondHost.resize(800, 600);
    secondHost.show();
    VNX::ToastPresenter secondToast(&secondHost);
    secondToast.showMessage(QStringLiteral("Second view"));

    firstHost.reset();
    EXPECT_TRUE(firstToast.isNull());
    EXPECT_TRUE(secondToast.isVisible());
    secondToast.dismiss();
    EXPECT_FALSE(secondToast.isVisible());
}

TEST(ToastPresenterTest, ReplacementReceivesItsOwnLifetimeAndThenExpires)
{
    QWidget host;
    host.resize(800, 600);
    host.show();
    VNX::ToastPresenter toast(&host);
    toast.showMessage(QStringLiteral("First feedback"));
    waitForEvents(2100);
    ASSERT_TRUE(toast.isVisible());

    toast.showMessage(QStringLiteral("Latest feedback"));
    waitForEvents(1100);
    EXPECT_TRUE(toast.isVisible());
    EXPECT_EQ(toast.accessibleName(), QStringLiteral("Latest feedback"));

    waitForEvents(2100);
    EXPECT_FALSE(toast.isVisible());
}
