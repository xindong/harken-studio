#include "Update/UpdateNotifier.h"

#include <gtest/gtest.h>

#include <vector>

using VNH::UpdateNotifier;
using VNH::UpdateStateRecord;

namespace {

class RecordingNotifier : public UpdateNotifier
{
public:
    std::vector<Notification> presented;

protected:
    void present(const Notification& notification) override
    {
        presented.push_back(notification);
    }
};

UpdateStateRecord record(const QString& product, const QString& state,
                         const QString& buildId)
{
    UpdateStateRecord result;
    result.found = true;
    result.product = product;
    result.state = state;
    result.version = QStringLiteral("1.4.0");
    result.buildId = buildId;
    return result;
}

}

TEST(UpdateNotifierTest, NotifiesInstalledStudioUpdate)
{
    const auto notification = UpdateNotifier::evaluate(record(
        QStringLiteral("HarkenStudio"), QStringLiteral("Installed"),
        QStringLiteral("run1.aaaa1111")));
    EXPECT_TRUE(notification.show);
    EXPECT_TRUE(notification.title.contains(QStringLiteral("installed")));
    EXPECT_TRUE(notification.message.contains(QStringLiteral("1.4.0")));
}

TEST(UpdateNotifierTest, NotifiesStudioUpdateAwaitingManualInstall)
{
    const auto notification = UpdateNotifier::evaluate(record(
        QStringLiteral("HarkenStudio"), QStringLiteral("AwaitingUserInstall"),
        QStringLiteral("run1.aaaa1111")));
    EXPECT_TRUE(notification.show);
    EXPECT_TRUE(notification.message.contains(
        QStringLiteral("Open Harken Hub")));
}

TEST(UpdateNotifierTest, NotifiesHubUpdateAwaitingApproval)
{
    const auto notification = UpdateNotifier::evaluate(record(
        QStringLiteral("HarkenHub"),
        QStringLiteral("AwaitingUserApproval"),
        QStringLiteral("run2.bbbb2222")));
    EXPECT_TRUE(notification.show);
    EXPECT_TRUE(
        notification.title.contains(QStringLiteral("Harken Hub")));
}

TEST(UpdateNotifierTest, StaysQuietForIntermediateStates)
{
    EXPECT_FALSE(UpdateNotifier::evaluate(
                     record(QStringLiteral("HarkenStudio"),
                            QStringLiteral("Downloading"),
                            QStringLiteral("run1.aaaa1111")))
                     .show);
    EXPECT_FALSE(UpdateNotifier::evaluate(
                     record(QStringLiteral("HarkenStudio"),
                            QStringLiteral("Verified"),
                            QStringLiteral("run1.aaaa1111")))
                     .show);
    EXPECT_FALSE(UpdateNotifier::evaluate(
                     record(QStringLiteral("HarkenStudio"),
                            QStringLiteral("AwaitingBridge"),
                            QStringLiteral("run1.aaaa1111")))
                     .show);
    EXPECT_FALSE(UpdateNotifier::evaluate(
                     record(QStringLiteral("HarkenHub"),
                            QStringLiteral("Verified"),
                            QStringLiteral("run2.bbbb2222")))
                     .show);
    EXPECT_FALSE(UpdateNotifier::evaluate(
                     record(QStringLiteral("HarkenHub"),
                            QStringLiteral("AwaitingRestart"),
                            QStringLiteral("run2.bbbb2222")))
                     .show);
    EXPECT_FALSE(UpdateNotifier::evaluate(
                     record(QStringLiteral("HarkenStudio"),
                            QStringLiteral("VerificationFailed"),
                            QStringLiteral("run1.aaaa1111")))
                     .show);
}

TEST(UpdateNotifierTest, DeduplicatesRepeatedStates)
{
    RecordingNotifier notifier;
    const auto installed = record(QStringLiteral("HarkenStudio"),
                                  QStringLiteral("Installed"),
                                  QStringLiteral("run1.aaaa1111"));
    notifier.handleState(installed);
    notifier.handleState(installed);
    EXPECT_EQ(notifier.presented.size(), 1u);

    notifier.handleState(record(QStringLiteral("HarkenStudio"),
                                QStringLiteral("Installed"),
                                QStringLiteral("run3.cccc3333")));
    EXPECT_EQ(notifier.presented.size(), 2u);
}

TEST(UpdateNotifierTest, HubUpdatedNotificationDeduplicates)
{
    RecordingNotifier notifier;
    notifier.notifyHubUpdated(QStringLiteral("0.2.0"));
    notifier.notifyHubUpdated(QStringLiteral("0.2.0"));
    ASSERT_EQ(notifier.presented.size(), 1u);
    EXPECT_TRUE(notifier.presented[0].message.contains(
        QStringLiteral("0.2.0")));
}

TEST(UpdateNotifierTest, ActivationTargetsVersionsPage)
{
    EXPECT_EQ(UpdateNotifier::activationArguments(),
              QStringList{QStringLiteral("--show-versions")});
}
