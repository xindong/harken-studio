
#include "InspectorLaunchEntry.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace VNX {

namespace {
QString prettifySlot(const QString& slot)
{
    QString out;
    bool atWordStart = true;
    for (QChar c : slot)
    {
        if (c == QLatin1Char('_')) { out += QLatin1Char(' '); atWordStart = true; continue; }
        out += atWordStart ? c.toUpper() : c;
        atWordStart = false;
    }
    return out;
}
}

InspectorLaunchEntry::InspectorLaunchEntry(const QString& slot, QWidget* parent,
                                           const QString& displayName)
    : QWidget(parent)
    , slot_(slot)
    , placeholder_(displayName.isEmpty() ? prettifySlot(slot) : displayName)
{
    setObjectName(QStringLiteral("launchEntry_") + slot_);

    const auto& inspector = ThemeManager::instance().metrics().inspector;
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(inspector.pageMargin, 0, inspector.pageMargin, 0);
    h->setSpacing(inspector.sectionSpacing);
    reserveInspectorContentSlots(this);

    label_ = new QLabel(placeholder_, this);
    label_->setObjectName(QStringLiteral("launchEntryLabel_") + slot_);
    h->addWidget(label_, 1);

    button_ = new QToolButton(this);
    button_->setObjectName(QStringLiteral("launchEntryButton_") + slot_);
    const auto refreshIcon = [this]() {
        button_->setIcon(ThemeManager::instance().resolveIcon(
            QString::fromLatin1(ThemeAssetIds::InspectorDetach)).icon);
    };
    refreshIcon();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [refreshIcon]() { refreshIcon(); });
    button_->setAutoRaise(true);
    button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button_->setProperty("flatIconButton", true);
    button_->setToolTip(tr("Open %1 Editor").arg(placeholder_));
    h->addWidget(button_, 0);

    connect(button_, &QToolButton::clicked, this,
            [this] { emit launchRequested(slot_); });
}

void InspectorLaunchEntry::setSummary(const QString& summary)
{
    label_->setText(summary.isEmpty() ? placeholder_ : summary);
}

void InspectorLaunchEntry::setLaunchEnabled(bool enabled, const QString& disabledReason)
{
    button_->setEnabled(enabled);
    button_->setToolTip(enabled
        ? tr("Open %1 Editor").arg(placeholder_)
        : (disabledReason.isEmpty() ? tr("Coming soon") : disabledReason));
}

}
