#include "PaneSectionStack.h"
#include "PaneSection.h"

#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

PaneSectionStack::PaneSectionStack(const QString& settingsGroup, QWidget* parent)
    : QFrame(parent)
    , settingsGroup_(settingsGroup)
{
    setProperty("paneRole", "stack");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setProperty("splitterRole", QStringLiteral("border"));
    splitter_->setHandleWidth(1);
    splitter_->setChildrenCollapsible(false);
    layout->addWidget(splitter_, 1);
    collapsedSpace_ = new QWidget(this);
    collapsedSpace_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(collapsedSpace_, 1);
    connect(splitter_, &QSplitter::splitterMoved, this, [this]() {
        if (!std::all_of(sections_.cbegin(), sections_.cend(), [this](const PaneSection* section) {
                return !isShown(section) || section->isExpanded();
            }))
            return;
        const QList<int> sizes = splitter_->sizes();
        for (int index = 0; index < sections_.size(); ++index)
            if (isShown(sections_[index]))
                expandedSizes_[index] = sizes[index];
        save();
    });
}

PaneSection* PaneSectionStack::addSection(const QString& title, const QString& settingsId, int defaultSize)
{
    auto* section = new PaneSection(title, settingsId, splitter_);
    sections_.append(section);
    expandedSizes_.append(defaultSize);
    QSettings settings;
    const QList<QVariant> sizes = settings.value(settingsGroup_ + QStringLiteral("/sectionSizes")).toList();
    if (sizes.size() == sections_.size()
        && std::all_of(sizes.cbegin(), sizes.cend(), [](const QVariant& size) { return size.toInt() > 0; }))
        for (int index = 0; index < sizes.size(); ++index)
            expandedSizes_[index] = sizes[index].toInt();
    section->setExpanded(settings.value(settingsGroup_ + QLatin1Char('/') + settingsId
                                        + QStringLiteral("Expanded"), true).toBool());
    connect(section, &PaneSection::expandedChanged, this, [this]() {
        updateLayout();
        save();
    });
    updateLayout();
    return section;
}

void PaneSectionStack::setSectionVisible(PaneSection* section, bool visible)
{
    if (isShown(section) == visible)
        return;
    section->setVisible(visible);
    updateLayout();
}

bool PaneSectionStack::isShown(const PaneSection* section) const
{
    return !section->isHidden();
}

void PaneSectionStack::updateLayout()
{
    const auto open = [this](const PaneSection* section) { return isShown(section) && section->isExpanded(); };
    const bool anyOpen = std::any_of(sections_.cbegin(), sections_.cend(), open);
    const bool allOpen = std::all_of(sections_.cbegin(), sections_.cend(), [&](const PaneSection* section) {
        return !isShown(section) || section->isExpanded();
    });
    collapsedSpace_->setVisible(!anyOpen);
    layout()->activate();
    splitter_->refresh();
    const PaneSection* previous = nullptr;
    for (int index = 0; index < sections_.size(); ++index) {
        if (!isShown(sections_[index]))
            continue;
        if (previous)
            splitter_->handle(index)->setEnabled(open(previous) && open(sections_[index]));
        previous = sections_[index];
    }
    QList<int> sizes;
    for (int index = 0; index < sections_.size(); ++index) {
        const PaneSection* section = sections_[index];
        sizes.append(!isShown(section) ? 0
            : allOpen ? expandedSizes_[index]
            : section->isExpanded() ? 10000 : section->minimumHeight());
    }
    if (anyOpen)
        splitter_->setSizes(sizes);
}

void PaneSectionStack::save() const
{
    QSettings settings;
    QList<QVariant> sizes;
    for (int index = 0; index < sections_.size(); ++index) {
        settings.setValue(settingsGroup_ + QLatin1Char('/') + sections_[index]->settingsId()
                          + QStringLiteral("Expanded"), sections_[index]->isExpanded());
        sizes.append(expandedSizes_[index]);
    }
    settings.setValue(settingsGroup_ + QStringLiteral("/sectionSizes"), sizes);
}

}
