#include "WorkspaceDockFactory.h"

#include "ContextInfo.h"
#include "ContextInfoService.h"
#include "WorkspaceWindowTypes.h"
#include "ThemeAssetCatalog.h"
#include "ThemeManager.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/Information/InformationPanel.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/PreviewPanel/PreviewPanel.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/TransitionEditor/TransitionEditorPanel.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockSplitter.h>

#include <QAction>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace VNX {

namespace {

QAction* makePinAction(QWidget* content, bool pinned,
                       const QString& pinTip = QObject::tr("Pin to current object"),
                       const QString& unpinTip = QObject::tr("Unpin (locked to current object)"))
{
    auto* action = new QAction(content);
    action->setObjectName(QStringLiteral("dockPinButton"));
    action->setCheckable(true);
    action->setChecked(pinned);
    const auto refresh = [action, pinTip, unpinTip] {
        action->setIcon(ThemeManager::instance().resolveIcon(
            QString::fromLatin1(action->isChecked() ? ThemeAssetIds::DockPinFilled
                                                   : ThemeAssetIds::DockPin)).icon);
        action->setText(action->isChecked() ? unpinTip : pinTip);
        action->setToolTip(action->text());
    };
    QObject::connect(action, &QAction::toggled, action, refresh);
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, action, refresh);
    refresh();
    return action;
}

template<typename Panel>
QAction* bindPinAction(Panel* panel)
{
    auto* action = makePinAction(panel, panel->isPinned());
    QObject::connect(action, &QAction::triggered, panel, &Panel::setPinned);
    QObject::connect(panel, &Panel::pinToggled, action, &QAction::setChecked);
    return action;
}

QAction* pinActionFor(QWidget* content)
{
    if (auto* panel = qobject_cast<InspectorPanel*>(content)) {
        auto* action = makePinAction(panel, panel->isPinned());
        QObject::connect(action, &QAction::triggered, panel, &InspectorPanel::setPinned);
        QObject::connect(panel, &InspectorPanel::pinToggled, action,
                         [action](int, bool pinned) { action->setChecked(pinned); });
        return action;
    }
    if (qobject_cast<InformationPanel*>(content)) {
        auto& service = ContextInfoService::instance();
        auto* action = makePinAction(content, service.isLocked(),
            QObject::tr("Lock to the current topic"),
            QObject::tr("Unlock (follow the pointer again)"));
        QObject::connect(action, &QAction::triggered, &service, &ContextInfoService::setLocked);
        QObject::connect(&service, &ContextInfoService::lockedChanged, action, &QAction::setChecked);
        return action;
    }
    if (auto* panel = qobject_cast<GridEditorPanel*>(content))
        return bindPinAction(panel);
    if (auto* panel = qobject_cast<PreviewPanel*>(content))
        return bindPinAction(panel);
    if (auto* panel = qobject_cast<TransitionEditorPanel*>(content))
        return bindPinAction(panel);
    if (auto* panel = qobject_cast<TimelineEditorPanel*>(content))
        return bindPinAction(panel);
    if (auto* panel = qobject_cast<ExternalEditorPanel*>(content))
        return bindPinAction(panel);
    if (auto* panel = qobject_cast<SourceEditorPanel*>(content))
        return bindPinAction(panel);
    return nullptr;
}

ads::CDockSplitter* makeSplitter(Qt::Orientation orientation)
{
    auto* splitter = new ads::CDockSplitter(orientation);
    splitter->setOpaqueResize(ads::CDockManager::testConfigFlag(
        ads::CDockManager::OpaqueSplitterResize));
    splitter->setChildrenCollapsible(false);
    return splitter;
}

struct SplitterSizes
{
    QPointer<ads::CDockSplitter> splitter;
    QList<QPointer<QWidget>> children;
    QList<int> sizes;
};

QWidget* groupAdjacentWidgets(QWidget* first, QWidget* last)
{
    if (!last || first == last)
        return first;
    auto* parent = qobject_cast<ads::CDockSplitter*>(first->parentWidget());
    parent->refresh();
    const QList<int> sizes = parent->sizes();
    const int begin = parent->indexOf(first);
    const int end = parent->indexOf(last);
    QList<QWidget*> children;
    QList<int> groupedSizes;
    int extent = 0;
    int visibleCount = 0;
    for (int i = begin; i <= end; ++i) {
        children.append(parent->widget(i));
        groupedSizes.append(sizes[i]);
        extent += sizes[i];
        visibleCount += !parent->widget(i)->isHidden();
    }
    extent += qMax(0, visibleCount - 1) * parent->handleWidth();
    auto* group = makeSplitter(parent->orientation());
    for (auto* child : children)
        group->addWidget(child);
    parent->insertWidget(begin, group);
    QList<int> remaining = sizes;
    for (int i = end; i >= begin; --i)
        remaining.removeAt(i);
    remaining.insert(begin, extent);
    parent->setSizes(remaining);
    group->setSizes(groupedSizes);
    group->setVisible(visibleCount > 0);
    return group;
}

void addDockBesideSubtree(ads::CDockManager* manager, ads::CDockWidget* dock,
                         ads::DockWidgetArea edge, QWidget*& anchor)
{
    auto* area = qobject_cast<ads::CDockAreaWidget*>(anchor);
    if (!anchor || area) {
        manager->addDockWidget(edge, dock, area);
        return;
    }

    auto* subtree = qobject_cast<ads::CDockSplitter*>(anchor);
    auto* leaf = subtree->findChild<ads::CDockAreaWidget*>();
    auto* leafParent = leaf->parentSplitter();
    const int leafIndex = leafParent->indexOf(leaf);
    leafParent->refresh();
    const QByteArray leafParentState = leafParent->saveState();
    auto* added = manager->addDockWidget(edge, dock, leaf);
    auto* insertion = added->parentSplitter();
    added->setParent(manager);
    if (insertion != leafParent) {
        leafParent->insertWidget(leafIndex, leaf);
        delete insertion;
    }
    leafParent->restoreState(leafParentState);

    const Qt::Orientation orientation = edge == ads::LeftDockWidgetArea
        || edge == ads::RightDockWidgetArea ? Qt::Horizontal : Qt::Vertical;
    const bool append = edge == ads::RightDockWidgetArea || edge == ads::BottomDockWidgetArea;
    auto* parent = qobject_cast<ads::CDockSplitter*>(subtree->parentWidget());
    if (!parent) {
        auto* contents = makeSplitter(subtree->orientation());
        const QByteArray state = subtree->saveState();
        while (subtree->count())
            contents->addWidget(subtree->widget(0));
        subtree->setOrientation(orientation);
        subtree->addWidget(contents);
        subtree->insertWidget(append ? 1 : 0, added);
        contents->restoreState(state);
        anchor = contents;
    } else if (parent->orientation() == orientation) {
        parent->insertWidget(parent->indexOf(subtree) + (append ? 1 : 0), added);
    } else {
        const int index = parent->indexOf(subtree);
        const QByteArray state = parent->saveState();
        auto* split = makeSplitter(orientation);
        split->addWidget(subtree);
        split->insertWidget(append ? 1 : 0, added);
        parent->insertWidget(index, split);
        parent->restoreState(state);
    }
    added->show();
}

}

void WorkspaceDockFactory::setContent(ads::CDockWidget* dock, QWidget* content,
                                      ads::CDockWidget::eInsertMode insertMode)
{
    if (content && dock) {
        for (const WorkspaceWindowDescriptor& descriptor : defaultWorkspaceWindowDescriptors()) {
            if (descriptor.dockObjectName == dock->objectName()) {
                ContextInfo::tag(content, ContextTopic::forPanel(descriptor.typeId));
                break;
            }
        }
    }
    dock->setWidget(content, insertMode);
    if (auto* action = pinActionFor(content)) {
        auto actions = dock->titleBarActions();
        actions.append(action);
        dock->setTitleBarActions(actions);
        auto* area = dock->dockAreaWidget();
        if (area && area->currentDockWidget() == dock)
            area->titleBar()->updateDockWidgetActionsButtons();
    }
}

ads::CDockWidget* WorkspaceDockFactory::createDetachedDock(const WorkspaceDockCreateSpec& spec)
{
    if (!spec.content)
        return nullptr;

    auto* dock = new ads::CDockWidget(spec.title);
    dock->setObjectName(spec.objectName);
    if (spec.minimumSizeFromContent)
        dock->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContent);
    setContent(dock, spec.content);
    dock->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, spec.deleteOnClose);
    return dock;
}

ads::CDockWidget* WorkspaceDockFactory::createDock(ads::CDockManager* dockManager,
                                                   const WorkspaceDockCreateSpec& spec)
{
    if (!dockManager)
        return nullptr;

    ads::CDockWidget* dock = createDetachedDock(spec);
    if (!dock)
        return nullptr;

    QWidget* target = groupAdjacentWidgets(spec.anchor, spec.lastAnchor);
    QPointer<ads::CDockSplitter> originalSplitter = target
        ? qobject_cast<ads::CDockSplitter*>(target->parentWidget()) : nullptr;
    QByteArray originalSplitterState;
    QList<QPointer<QWidget>> originalChildren;
    QList<int> originalSizes;
    QList<SplitterSizes> ancestorSizes;
    const bool preserveLayout = spec.initiallyVisible && target
        && spec.area != ads::NoDockWidgetArea && spec.area != ads::CenterDockWidgetArea;
    if (preserveLayout && originalSplitter) {
        originalSplitter->refresh();
        originalSplitterState = originalSplitter->saveState();
        originalSizes = originalSplitter->sizes();
        for (int i = 0; i < originalSplitter->count(); ++i)
            originalChildren.append(originalSplitter->widget(i));
        auto* parent = qobject_cast<ads::CDockSplitter*>(originalSplitter->parentWidget());
        while (parent) {
            parent->refresh();
            SplitterSizes saved{parent, {}, parent->sizes()};
            for (int i = 0; i < parent->count(); ++i) {
                auto* child = parent->widget(i);
                saved.children.append(child);
                if (child->isHidden())
                    saved.sizes[i] = parent->orientation() == Qt::Horizontal
                        ? child->width() : child->height();
            }
            ancestorSizes.prepend(saved);
            parent = qobject_cast<ads::CDockSplitter*>(parent->parentWidget());
        }
    }

    if (spec.area != ads::NoDockWidgetArea)
        addDockBesideSubtree(dockManager, dock, spec.area, target);
    else
        dockManager->addDockWidgetFloating(dock, spec.initiallyVisible);

    if (preserveLayout && originalSplitter
        && dock->dockAreaWidget()->parentSplitter() != originalSplitter)
        originalSplitter->restoreState(originalSplitterState);

    dock->toggleView(spec.initiallyVisible);
    if (preserveLayout) {
        QPointer<QWidget> anchor = target;
        QPointer<ads::CDockSplitter> splitter = dock->dockAreaWidget()->parentSplitter();
        const auto applyShare = [dock, anchor, splitter, originalSplitter,
                                  originalChildren, originalSizes, ancestorSizes,
                                  share = spec.initialShare]() {
            for (const auto& saved : ancestorSizes) {
                if (!saved.splitter || saved.splitter->count() != saved.children.size())
                    continue;
                bool unchanged = true;
                for (int i = 0; i < saved.children.size(); ++i)
                    unchanged &= saved.splitter->widget(i) == saved.children[i];
                if (unchanged)
                    saved.splitter->setSizes(saved.sizes);
            }
            if (share <= 0.0)
                return;
            auto* area = dock->dockAreaWidget();
            if (!anchor || !splitter || !area || area == anchor
                || area->parentSplitter() != splitter)
                return;
            QWidget* anchorChild = anchor;
            while (anchorChild && anchorChild->parentWidget() != splitter)
                anchorChild = anchorChild->parentWidget();
            if (!anchorChild || anchorChild == area)
                return;

            splitter->refresh();
            QList<int> sizes = splitter->sizes();
            int available = 0;
            for (int size : sizes)
                available += size;

            if (splitter == originalSplitter) {
                if (splitter->count() != originalChildren.size() + 1)
                    return;
                for (int i = 0; i < splitter->count(); ++i) {
                    QWidget* child = splitter->widget(i);
                    if (child == area || child == anchorChild)
                        continue;
                    const int oldIndex = originalChildren.indexOf(child);
                    if (oldIndex < 0)
                        return;
                    sizes[i] = originalSizes[oldIndex];
                    available -= sizes[i];
                }
            } else if (splitter->count() != 2) {
                return;
            }

            const int editorExtent = qRound(qMax(0, available) * share);
            sizes[splitter->indexOf(area)] = editorExtent;
            sizes[splitter->indexOf(anchorChild)] = qMax(0, available - editorExtent);
            splitter->setSizes(sizes);
        };
        applyShare();
        QTimer::singleShot(0, dock, applyShare);
    }
    return dock;
}

}
