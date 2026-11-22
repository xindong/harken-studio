#include "Common/ToastPresenter.h"

#include <QPointer>

#include <gtest/gtest.h>

#include <memory>

TEST(ToastPresenterTest, HostsOwnIndependentFeedbackLifetimes)
{
    auto firstHost = std::make_unique<QWidget>();
    firstHost->resize(800, 600);
    firstHost->show();
    QPointer<harken::studio::ToastPresenter> firstToast = new harken::studio::ToastPresenter(firstHost.get());
    firstToast->showMessage(QStringLiteral("First view"));

    QWidget secondHost;
    secondHost.resize(800, 600);
    secondHost.show();
    harken::studio::ToastPresenter secondToast(&secondHost);
    secondToast.showMessage(QStringLiteral("Second view"));

    firstHost.reset();
    EXPECT_TRUE(firstToast.isNull());
    EXPECT_TRUE(secondToast.isVisible());
}
