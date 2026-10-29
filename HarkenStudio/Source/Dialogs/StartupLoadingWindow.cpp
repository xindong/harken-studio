#include "StartupLoadingWindow.h"
#include "StartupRenderer.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "harken_buildinfo.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QScreen>
#include <QShowEvent>
#include <QStyle>
#include <QThread>
#include <QToolButton>
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

StartupLoadingWindow::StartupLoadingWindow()
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint
                           | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint)
{
    setObjectName(QStringLiteral("startupLoadingWindow"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setFixedSize(kWindowWidth, kWindowHeight);
    setWindowTitle(tr("Harken Studio"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    renderer_ = std::make_unique<StartupRenderer>();
    renderer_->window()->installEventFilter(this);
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
    topography->setAttribute(Qt::WA_NativeWindow);
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

    auto& theme = ThemeManager::instance();
    const auto& metrics = theme.snapshot()->thirdParty.qtAds.metrics;
    closeButton_ = new QToolButton(this);
    closeButton_->setAttribute(Qt::WA_NativeWindow);
    closeButton_->setObjectName(QStringLiteral("startupLoadingCloseButton"));
    closeButton_->setProperty("flatIconButton", true);
    closeButton_->setIcon(theme.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::DockClose)).icon);
    closeButton_->setIconSize(QSize(metrics.titleBarIconSize, metrics.titleBarIconSize));
    const int buttonExtent = metrics.closeButtonExtent + metrics.closeButtonPadding * 2;
    closeButton_->setFixedSize(buttonExtent, buttonExtent);
    closeButton_->setToolTip(tr("Close"));
    closeButton_->setAccessibleName(tr("Close Harken Studio"));
    closeButton_->setFocusPolicy(Qt::NoFocus);
    closeButton_->move(width() - buttonExtent - metrics.titleBarButtonPadding,
                       metrics.titleBarButtonPadding);
    closeButton_->raise();
    connect(closeButton_, &QToolButton::clicked, this, &QWidget::close);

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
    if (failed_ || cancelling_)
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
    if (failed_ || cancelling_)
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
    if (cancelling_)
        return;
    failed_ = true;
    statusLabel_->setText(message.trimmed().isEmpty()
                              ? tr("Harken Studio could not start")
                              : message.trimmed());
    statusLabel_->setAccessibleDescription(statusLabel_->text());
    setProperty("startupState", QStringLiteral("failed"));
    style()->unpolish(this);
    style()->polish(this);
}

void StartupLoadingWindow::showCancelling()
{
    if (cancelling_)
        return;
    cancelling_ = true;
    closeButton_->setEnabled(false);
    statusLabel_->setText(tr("Closing Harken Studio…"));
    statusLabel_->setAccessibleDescription(statusLabel_->text());
}

void StartupLoadingWindow::dismiss()
{
    dismissing_ = true;
    close();
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

bool StartupLoadingWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (renderer_ && watched == renderer_->window()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && windowHandle()->startSystemMove())
                return true;
        } else if (event->type() == QEvent::Resize
                   || event->type() == QEvent::DevicePixelRatioChange) {
            renderer_->setViewport(renderer_->window()->size(),
                                   renderer_->window()->devicePixelRatio());
        }
    }
    return QWidget::eventFilter(watched, event);
}

void StartupLoadingWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && windowHandle()->startSystemMove()) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void StartupLoadingWindow::closeEvent(QCloseEvent* event)
{
    if (dismissing_) {
        QWidget::closeEvent(event);
        return;
    }
    event->ignore();
    if (cancelling_)
        return;
    showCancelling();
    emit cancelRequested();
}

void StartupLoadingWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!initialPositionSet_) {
        centerOnScreen();
        initialPositionSet_ = true;
    }
    if (renderer_) {
        auto* topography = findChild<QWidget*>(
            QStringLiteral("startupLoadingTopography"));
        if (topography)
            renderer_->setViewport(topography->size(), topography->devicePixelRatioF());
        renderer_->setExposed(true);
        renderer_->start();
    }
    closeButton_->raise();
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
