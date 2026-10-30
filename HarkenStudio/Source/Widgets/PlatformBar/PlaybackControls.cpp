#include "PlaybackControls.h"

#include "Core/ContextInfo.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QHBoxLayout>
#include <QStyle>
#include <QToolButton>

namespace VNX {

PlaybackControls::PlaybackControls(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("playbackControls"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    stopBtn_ = new QToolButton(this);
    playBtn_ = new QToolButton(this);
    resetBtn_ = new QToolButton(this);
    stopBtn_->setObjectName(QStringLiteral("stopBtn"));
    playBtn_->setObjectName(QStringLiteral("playBtn"));
    resetBtn_->setObjectName(QStringLiteral("resetBtn"));
    stopBtn_->setAccessibleName(tr("Stop"));
    playBtn_->setAccessibleName(tr("Play"));
    resetBtn_->setAccessibleName(tr("Reset container selection"));
    stopBtn_->setToolTip(tr("Stop"));
    resetBtn_->setEnabled(false);
    for (auto* button : {stopBtn_, playBtn_, resetBtn_}) {
        button->setProperty("flatIconButton", true);
        button->setFixedSize(32, 22);
        button->setIconSize(QSize(20, 20));
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
    }
    connect(playBtn_, &QToolButton::clicked, this, &PlaybackControls::playClicked);
    connect(stopBtn_, &QToolButton::clicked, this, &PlaybackControls::stopClicked);
    connect(resetBtn_, &QToolButton::clicked, this, &PlaybackControls::resetClicked);
    ContextInfo::tag(playBtn_, ContextTopic::forPanel(QStringLiteral("audition.play")));
    ContextInfo::tag(stopBtn_, ContextTopic::forPanel(QStringLiteral("audition.stop")));
    ContextInfo::tag(resetBtn_, ContextTopic::forPanel(QStringLiteral("audition.reset")));
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, &PlaybackControls::applyTheme);
    connect(&ShortcutManager::instance(), &ShortcutManager::shortcutChanged,
            this, [this](const QString& id, const QKeySequence&) {
                if (id == Shortcuts::PlayStop || id == Shortcuts::PlaybackReset)
                    refreshShortcuts();
            });
    connect(&ShortcutManager::instance(), &ShortcutManager::shortcutsReset,
            this, &PlaybackControls::refreshShortcuts);
    setAuditionTarget(nullptr);
    setPlaying(false);
    applyTheme();
    refreshShortcuts();
}

void PlaybackControls::setAuditionTarget(ProjectItem* item)
{
    const bool playable = item && ItemTypeCatalog::isTriggerable(item->type)
        && ItemTypeCatalog::isUserFacingType(item->type);
    playBtn_->setEnabled(playable);
}

void PlaybackControls::setPlaying(bool playing)
{
    playBtn_->setProperty("playing", playing);
    playBtn_->style()->unpolish(playBtn_);
    playBtn_->style()->polish(playBtn_);
    playBtn_->update();
}

void PlaybackControls::setResetEnabled(bool enabled)
{
    resetBtn_->setEnabled(enabled);
}

bool PlaybackControls::isResetEnabled() const
{
    return resetBtn_->isEnabled();
}

void PlaybackControls::applyTheme()
{
    auto& manager = ThemeManager::instance();
    const auto theme = manager.snapshot();
    const auto& colors = theme->ui.colors;
    const ThemeAssetVariant playVariant{
        playBtn_->iconSize(), devicePixelRatioF(), 0, colors.auditionPlayActive};
    playBtn_->setIcon(manager.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AuditionPlay), playVariant).icon);
    const ThemeAssetVariant stopVariant{
        stopBtn_->iconSize(), devicePixelRatioF(), 0, colors.auditionStopActive};
    stopBtn_->setIcon(manager.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AuditionStop), stopVariant).icon);
    const ThemeAssetVariant resetVariant{
        resetBtn_->iconSize(), devicePixelRatioF(), 0, colors.warning};
    resetBtn_->setIcon(manager.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AuditionReset), resetVariant).icon);
}

void PlaybackControls::refreshShortcuts()
{
    auto& shortcuts = ShortcutManager::instance();
    const QString play = shortcuts.shortcut(Shortcuts::PlayStop).toString(QKeySequence::NativeText);
    playBtn_->setToolTip(play.isEmpty() ? tr("Play/Stop") : tr("Play/Stop (%1)").arg(play));
    const QString reset = shortcuts.shortcut(Shortcuts::PlaybackReset).toString(QKeySequence::NativeText);
    resetBtn_->setToolTip(reset.isEmpty()
        ? tr("Reset container selection")
        : tr("Reset container selection (%1)").arg(reset));
}

}
