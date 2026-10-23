#include "StartupLoadingWindow.h"
#include "StartupRenderer.h"

#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QScreen>
#include <QShowEvent>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>

namespace {

constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 520;
constexpr int kTopographyHeight = 416;
constexpr int kContentHeight = kWindowHeight - kTopographyHeight;

QString versionString()
{
    return QCoreApplication::applicationVersion().isEmpty()
               ? QString::fromUtf8(Harken::BuildInfo::Version.data(),
                                   static_cast<int>(Harken::BuildInfo::Version.size()))
               : QCoreApplication::applicationVersion();
}

}

namespace VNX {

StartupLoadingWindow::StartupLoadingWindow(QWidget* parent)
    : QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint)
{
    setObjectName(QStringLiteral("startupLoadingWindow"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedSize(kWindowWidth, kWindowHeight);
    setWindowTitle(tr("Harken Studio"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    renderer_ = std::make_unique<StartupRenderer>();
    connect(renderer_.get(), &StartupRenderer::initialFramePresented,
            this, [this]() {
        if (!waitingForInitialFrame_)
            return;
        waitingForInitialFrame_ = false;
        setWindowOpacity(1.0);
    }, Qt::QueuedConnection);
    connect(renderer_.get(), &StartupRenderer::initializationFailed,
            this, [this]() {
        if (!waitingForInitialFrame_)
            return;
        waitingForInitialFrame_ = false;
        setWindowOpacity(1.0);
    }, Qt::QueuedConnection);
    auto* topography = QWidget::createWindowContainer(renderer_->window(), this);
    topography->setObjectName(QStringLiteral("startupLoadingTopography"));
    topography->setAttribute(Qt::WA_TransparentForMouseEvents);
    topography->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    topography->setFixedHeight(kTopographyHeight);
    layout->addWidget(topography);
    renderer_->setViewport(topography->size(), topography->devicePixelRatioF());

    auto* content = new QWidget(this);
    content->setObjectName(QStringLiteral("startupLoadingContent"));
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    content->setFixedHeight(kContentHeight);
    content->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(content);

    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(42, 8, 42, 8);
    contentLayout->setSpacing(0);

    auto* projectRow = new QHBoxLayout();
    projectRow->setSpacing(12);
    projectLabel_ = new QLabel(tr("No project selected"), content);
    projectLabel_->setObjectName(QStringLiteral("startupLoadingProject"));
    projectLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    projectLabel_->setAccessibleName(tr("Project name"));
    projectRow->addWidget(projectLabel_, 1);

    percentLabel_ = new QLabel(QStringLiteral("0%"), content);
    percentLabel_->setObjectName(QStringLiteral("startupLoadingPercent"));
    percentLabel_->setAccessibleName(tr("Startup progress percentage"));
    percentLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    projectRow->addWidget(percentLabel_);
    contentLayout->addLayout(projectRow);
    contentLayout->addSpacing(2);

    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(12);
    statusLabel_ = new QLabel(content);
    statusLabel_->setObjectName(QStringLiteral("startupLoadingStatus"));
    statusLabel_->setAccessibleName(tr("Startup status"));
    statusRow->addWidget(statusLabel_, 1);
    contentLayout->addLayout(statusRow);
    contentLayout->addSpacing(3);

    progressBar_ = new QProgressBar(content);
    progressBar_->setObjectName(QStringLiteral("startupLoadingProgress"));
    progressBar_->setRange(0, 100);
    progressBar_->setTextVisible(false);
    progressBar_->setAccessibleName(tr("Startup progress"));
    contentLayout->addWidget(progressBar_);

    contentLayout->addSpacing(4);
    auto* version = new QLabel(versionString(), content);
    version->setObjectName(QStringLiteral("startupLoadingVersion"));
    version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    contentLayout->addWidget(version);

    setPhase(StartupPhase::PreparingApplication);
}

StartupLoadingWindow::~StartupLoadingWindow() = default;

void StartupLoadingWindow::showAfterInitialFrame()
{
    waitingForInitialFrame_ = true;
    setWindowOpacity(0.0);
    show();
}

void StartupLoadingWindow::setProjectName(const QString& projectName)
{
    const QString name = projectName.trimmed();
    projectLabel_->setText(name.isEmpty() ? tr("No project selected") : name);
    projectLabel_->setAccessibleDescription(projectLabel_->text());
}

void StartupLoadingWindow::setPhase(StartupPhase phase, const QString& statusOverride)
{
    if (failed_)
        return;
    const StartupProgress next = startupProgressFor(phase);
    if (next.percent < progress_)
        return;
    phase_ = phase;
    progress_ = next.percent;
    const QString status = statusOverride.trimmed().isEmpty()
                               ? next.status
                               : statusOverride.trimmed();
    statusLabel_->setText(status + QStringLiteral("…"));
    statusLabel_->setAccessibleDescription(status);
    progressBar_->setValue(progress_);
    percentLabel_->setText(QStringLiteral("%1%").arg(progress_));
}

void StartupLoadingWindow::setProgress(int percent, const QString& status)
{
    if (failed_)
        return;
    const int next = qBound(progress_, percent, 99);
    const QString text = status.trimmed().isEmpty()
                             ? tr("Preparing editor")
                             : status.trimmed();
    progress_ = next;
    statusLabel_->setText(text + QStringLiteral("…"));
    statusLabel_->setAccessibleDescription(text);
    progressBar_->setValue(progress_);
    percentLabel_->setText(QStringLiteral("%1%").arg(progress_));
}

void StartupLoadingWindow::showFailure(const QString& message)
{
    Q_ASSERT(QThread::currentThread() == thread());
    failed_ = true;
    statusLabel_->setText(message.trimmed().isEmpty()
                              ? tr("Harken Studio could not start")
                              : message.trimmed());
    statusLabel_->setAccessibleDescription(statusLabel_->text());
    setProperty("startupState", QStringLiteral("failed"));
    style()->unpolish(this);
    style()->polish(this);
}

int StartupLoadingWindow::progressValue() const
{
    return progressBar_->value();
}

bool StartupLoadingWindow::renderThreadRunning() const
{
    return renderer_ && renderer_->isRunning();
}

quintptr StartupLoadingWindow::renderThreadId() const
{
    return renderer_ ? renderer_->threadId() : 0;
}

bool StartupLoadingWindow::renderSurfaceOwnedByGuiThread() const
{
    return renderer_ && renderer_->window()->thread() == thread();
}

void StartupLoadingWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    centerOnScreen();
    if (renderer_) {
        auto* topography = findChild<QWidget*>(
            QStringLiteral("startupLoadingTopography"));
        if (topography)
            renderer_->setViewport(topography->size(), topography->devicePixelRatioF());
        renderer_->setExposed(true);
        renderer_->start();
    }
}

void StartupLoadingWindow::hideEvent(QHideEvent* event)
{
    if (renderer_) {
        renderer_->setExposed(false);
        renderer_->stop();
    }
    QWidget::hideEvent(event);
}

void StartupLoadingWindow::centerOnScreen()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect available = screen->availableGeometry();
    move(available.center() - rect().center());
}

}
