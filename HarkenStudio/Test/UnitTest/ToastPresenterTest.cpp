#include "Common/ToastPresenter.h"

#include <QPointer>

#include <gtest/gtest.h>

#include <memory>

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
}
