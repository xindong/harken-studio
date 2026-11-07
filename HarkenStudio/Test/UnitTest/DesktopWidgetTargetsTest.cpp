#include "IPC/DesktopWidgetTargets.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QDialog>
#include <QJsonArray>
#include <QWidget>

using namespace VNX;

TEST(DesktopWidgetTargetsTest, DuplicateNamesRequireIdentityAndSupportScope)
{
    QWidget firstWindow;
    QWidget secondWindow;
    QWidget first(&firstWindow);
    QWidget second(&secondWindow);
    first.setObjectName("contractRepeatedControl");
    second.setObjectName(first.objectName());
    QJsonObject error;
    EXPECT_EQ(resolveDesktopWidget({{"name", first.objectName()}}, &error), nullptr);
    EXPECT_EQ(error.value("code"), "AMBIGUOUS_WIDGET");
    EXPECT_EQ(error.value("result").toObject().value("candidates").toArray().size(), 2);
    EXPECT_EQ(resolveDesktopWidget({{"widget_id", desktopWidgetId(&first)}}, &error), &first);
    EXPECT_EQ(resolveDesktopWidget({{"name", first.objectName()},
        {"scope_widget_id", desktopWidgetId(&secondWindow)}}, &error), &second);
    EXPECT_EQ(resolveDesktopWidget({{"widget_id", desktopWidgetId(&first)},
        {"scope_widget_id", desktopWidgetId(&secondWindow)}}, &error), nullptr);
    EXPECT_EQ(error.value("code"), "WIDGET_OUTSIDE_SCOPE");
}

TEST(DesktopWidgetTargetsTest, DestroyedIdentityNeverResolvesReplacement)
{
    auto* old = new QWidget;
    old->setObjectName("contractReplacedControl");
    const QString oldId = desktopWidgetId(old);
    delete old;
    QWidget replacement;
    replacement.setObjectName("contractReplacedControl");
    EXPECT_NE(desktopWidgetId(&replacement), oldId);
    QJsonObject error;
    EXPECT_EQ(resolveDesktopWidget({{"widget_id", oldId}}, &error), nullptr);
    EXPECT_EQ(error.value("code"), "STALE_WIDGET");
    EXPECT_EQ(resolveDesktopWidget({{"name", replacement.objectName()}}, &error), &replacement);
}

TEST(DesktopWidgetTargetsTest, QueriesInspectHiddenControlsButInteractionsRespectState)
{
    QWidget target;
    const QJsonObject selector{{"widget_id", desktopWidgetId(&target)}};
    QJsonObject error;
    EXPECT_EQ(resolveDesktopWidget(selector, &error), &target);
    EXPECT_EQ(resolveDesktopWidget(selector, &error, true), nullptr);
    EXPECT_EQ(error.value("code"), "WIDGET_NOT_VISIBLE");
    target.show();
    target.setEnabled(false);
    EXPECT_EQ(resolveDesktopWidget(selector, &error, true), nullptr);
    EXPECT_EQ(error.value("code"), "WIDGET_DISABLED");
    target.setEnabled(true);
    QDialog modal;
    modal.setModal(true);
    modal.show();
    EXPECT_EQ(resolveDesktopWidget(selector, &error, true), nullptr);
    EXPECT_EQ(error.value("code"), "WIDGET_BLOCKED_BY_DIALOG");
    EXPECT_EQ(resolveDesktopWidget({{"widget_id", desktopWidgetId(&modal)}}, &error, true), &modal);
}

TEST(DesktopWidgetTargetsTest, QueuedOperationsExposeDeletionAndStateChanges)
{
    bool ran = false;
    auto* target = new QWidget;
    target->show();
    const auto response = queueDesktopWidgetOperation(target, [&](QWidget*) {
        ran = true;
        return QJsonObject{};
    });
    ASSERT_EQ(response.value("status"), "ok");
    const auto id = response.value("result").toObject().value("operation_id").toString();
    EXPECT_EQ(desktopUiOperation(id).value("result").toObject().value("state"), "queued");
    delete target;
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    const auto result = desktopUiOperation(id).value("result").toObject();
    EXPECT_FALSE(ran);
    EXPECT_EQ(result.value("state"), "failed");
    EXPECT_EQ(result.value("error").toObject().value("code"), "STALE_WIDGET");

    QWidget hidden;
    hidden.show();
    const auto hiddenResponse = queueDesktopWidgetOperation(&hidden, [&](QWidget*) {
        ran = true;
        return QJsonObject{};
    });
    hidden.hide();
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    const auto hiddenId = hiddenResponse.value("result").toObject().value("operation_id").toString();
    const auto hiddenResult = desktopUiOperation(hiddenId).value("result").toObject();
    EXPECT_FALSE(ran);
    EXPECT_EQ(hiddenResult.value("state"), "failed");
    EXPECT_EQ(hiddenResult.value("error").toObject().value("code"), "WIDGET_NOT_VISIBLE");
}

TEST(DesktopWidgetTargetsTest, CompletionMeansCallbackReturnedAndErrorsRemainInspectable)
{
    QWidget target;
    target.show();
    QString id;
    bool observedRunning = false;
    const auto queued = queueDesktopWidgetOperation(&target, [&](QWidget*) {
        observedRunning = desktopUiOperation(id).value("result").toObject().value("state") == "running";
        return QJsonObject{{"status", "error"}, {"code", "DELIVERY_REJECTED"}, {"message", "Target rejected the operation."}};
    });
    id = queued.value("result").toObject().value("operation_id").toString();
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    EXPECT_TRUE(observedRunning);
    const auto result = desktopUiOperation(id).value("result").toObject();
    EXPECT_EQ(result.value("state"), "failed");
    EXPECT_EQ(result.value("error").toObject().value("code"), "DELIVERY_REJECTED");
    const auto succeeded = queueDesktopWidgetOperation(&target, [](QWidget*) { return QJsonObject{}; });
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    const auto successId = succeeded.value("result").toObject().value("operation_id").toString();
    EXPECT_EQ(desktopUiOperation(successId).value("result").toObject().value("state"), "succeeded");
}

TEST(DesktopWidgetTargetsTest, QueueRechecksSessionInterlocksBeforeDelivery)
{
    QWidget target;
    target.show();
    bool blocked = false;
    bool ran = false;
    const auto response = queueDesktopWidgetOperation(&target, [&](QWidget*) {
        ran = true;
        return QJsonObject{};
    }, {}, [&] {
        return blocked ? QJsonObject{{"status", "error"}, {"code", "SESSION_BUSY"},
            {"message", "Session state changed after queueing."}} : QJsonObject{};
    });
    blocked = true;
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    const auto id = response.value("result").toObject().value("operation_id").toString();
    const auto result = desktopUiOperation(id).value("result").toObject();
    EXPECT_FALSE(ran);
    EXPECT_EQ(result.value("state"), "failed");
    EXPECT_EQ(result.value("error").toObject().value("code"), "SESSION_BUSY");
}

TEST(DesktopWidgetTargetsTest, OperationHistoryIsBoundedAndExplicitAboutExpiration)
{
    QWidget target;
    target.show();
    QString firstId;
    for (int index = 0; index < 257; ++index) {
        const auto queued = queueDesktopWidgetOperation(&target, [](QWidget*) { return QJsonObject{}; });
        ASSERT_EQ(queued.value("status"), "ok");
        if (index == 0) firstId = queued.value("result").toObject().value("operation_id").toString();
        QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    }
    EXPECT_EQ(desktopUiOperation(firstId).value("code"), "UI_OPERATION_NOT_FOUND");
}
