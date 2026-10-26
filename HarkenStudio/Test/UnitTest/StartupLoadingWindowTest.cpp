#include "Core/StartupProgress.h"
#include "Dialogs/StartupLoadingWindow.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QProgressBar>
#include <QThread>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

TEST(StartupLoadingWindow, ShowsProjectAndDeterminateProgress)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    loading.setProjectName(QStringLiteral("Combat Audio"));
    loading.setPhase(VNX::StartupPhase::LoadingProjectContent);

    auto* project = loading.findChild<QLabel*>(
        QStringLiteral("startupLoadingProject"));
    auto* status = loading.findChild<QLabel*>(
        QStringLiteral("startupLoadingStatus"));
    auto* progress = loading.findChild<QProgressBar*>(
        QStringLiteral("startupLoadingProgress"));
    auto* percent = loading.findChild<QLabel*>(
        QStringLiteral("startupLoadingPercent"));
    ASSERT_NE(project, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(progress, nullptr);
    ASSERT_NE(percent, nullptr);
    EXPECT_EQ(project->text(), QStringLiteral("Combat Audio"));
    EXPECT_EQ(progress->minimum(), 0);
    EXPECT_EQ(progress->maximum(), 100);
    EXPECT_EQ(progress->value(), 65);
    EXPECT_EQ(percent->text(), QStringLiteral("65%"));
    EXPECT_FALSE(progress->isTextVisible());
}

TEST(StartupLoadingWindow, CustomProgressRemainsMonotonicAndBelowReady)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    loading.setProgress(72, QStringLiteral("Loading project content"));
    loading.setProgress(60, QStringLiteral("Reading project"));
    EXPECT_EQ(loading.progressValue(), 72);
    loading.setProgress(100, QStringLiteral("Preparing editor"));
    EXPECT_EQ(loading.progressValue(), 99);
}

TEST(StartupLoadingWindow, DoesNotRegressOrCompleteAfterFailure)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    loading.setPhase(VNX::StartupPhase::PreparingRuntime);
    loading.setPhase(VNX::StartupPhase::ReadingProject);
    EXPECT_EQ(loading.progressValue(), 82);

    loading.showFailure(QStringLiteral("Project is not readable"));
    loading.setPhase(VNX::StartupPhase::Ready);
    EXPECT_EQ(loading.progressValue(), 82);
}

TEST(StartupLoadingWindow, IsFramelessApplicationWindow)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    EXPECT_EQ(loading.windowType(), Qt::Window);
    EXPECT_EQ(loading.parentWidget(), nullptr);
    EXPECT_TRUE(loading.windowFlags().testFlag(Qt::FramelessWindowHint));
    const auto buttons = loading.findChildren<QAbstractButton*>();
    ASSERT_FALSE(buttons.isEmpty());
    for (const auto* button : buttons)
        EXPECT_EQ(button->window(), &loading);
}

TEST(StartupLoadingWindow, UserCloseWaitsForCancellationAndDismissDoesNotCancel)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    int cancellationRequests = 0;
    QObject::connect(&loading, &VNX::StartupLoadingWindow::cancelRequested,
                     &loading, [&]() { ++cancellationRequests; });
    loading.show();

    EXPECT_FALSE(loading.close());
    EXPECT_TRUE(loading.isVisible());
    EXPECT_EQ(cancellationRequests, 1);
    EXPECT_FALSE(loading.close());
    EXPECT_EQ(cancellationRequests, 1);

    loading.dismiss();
    EXPECT_FALSE(loading.isVisible());
    EXPECT_EQ(cancellationRequests, 1);

    VNX::StartupLoadingWindow completed;
    QObject::connect(&completed, &VNX::StartupLoadingWindow::cancelRequested,
                     &completed, [&]() { ++cancellationRequests; });
    completed.show();
    completed.dismiss();
    EXPECT_FALSE(completed.isVisible());
    EXPECT_EQ(cancellationRequests, 1);
}

TEST(StartupLoadingWindow, CancellationCannotBeOverwrittenByLateProgress)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    loading.setPhase(VNX::StartupPhase::LoadingProjectContent);
    loading.showCancelling();
    auto* status = loading.findChild<QLabel*>(
        QStringLiteral("startupLoadingStatus"));
    ASSERT_NE(status, nullptr);
    const QString cancellationStatus = status->text();
    const int cancellationProgress = loading.progressValue();

    loading.setProgress(90, QStringLiteral("Late project progress"));
    loading.setPhase(VNX::StartupPhase::Ready);
    loading.showFailure(QStringLiteral("Late project failure"));
    EXPECT_EQ(status->text(), cancellationStatus);
    EXPECT_EQ(loading.progressValue(), cancellationProgress);
}

TEST(StartupLoadingWindow, WaitsForInitialFrameBeforeBecomingVisible)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    loading.showAfterInitialFrame();
    EXPECT_EQ(loading.windowOpacity(), 0.0);

    QElapsedTimer timer;
    timer.start();
    while (loading.windowOpacity() == 0.0 && timer.elapsed() < 1000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);

    if (loading.renderThreadId() == 0)
        GTEST_SKIP() << "OpenGL render context is unavailable";
    EXPECT_EQ(loading.windowOpacity(), 1.0);
}

TEST(StartupLoadingWindow, UsesDedicatedRenderThread)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    EXPECT_TRUE(loading.renderSurfaceOwnedByGuiThread());
    loading.show();

    QElapsedTimer timer;
    timer.start();
    while (loading.renderThreadId() == 0 && timer.elapsed() < 1000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);

    if (loading.renderThreadId() == 0)
        GTEST_SKIP() << "OpenGL render context is unavailable";
    EXPECT_TRUE(loading.renderThreadRunning());
    EXPECT_NE(loading.renderThreadId(),
              reinterpret_cast<quintptr>(QThread::currentThreadId()));
    EXPECT_TRUE(loading.renderSurfaceOwnedByGuiThread());

    loading.dismiss();
    EXPECT_FALSE(loading.renderThreadRunning());
    EXPECT_EQ(loading.renderThreadId(), 0u);
}

TEST(StartupLoadingWindow, ReadyCompletesProgressAndPercentage)
{
    ensureApp();
    VNX::StartupLoadingWindow loading;
    loading.setPhase(VNX::StartupPhase::Ready);
    auto* percent = loading.findChild<QLabel*>(
        QStringLiteral("startupLoadingPercent"));
    ASSERT_NE(percent, nullptr);
    EXPECT_EQ(loading.progressValue(), 100);
    EXPECT_EQ(percent->text(), QStringLiteral("100%"));
}
