
#include "LayoutController.h"

#include "AppSettings.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "BuiltinLayouts.h"
#include "NamedLayoutStore.h"
#include "WorkspaceDockFactory.h"
#include "../Widgets/LogPanel/LogPanel.h"
#include "../Widgets/Monitor/MonitorPanelKeys.h"

#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
#include <DockAreaTitleBar.h>
#include <DockSplitter.h>
#include <FloatingDockContainer.h>
#include <DockWidgetTab.h>

#include <QWidget>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QInputDialog>
#include <QLineEdit>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QByteArray>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QSize>
#include <QTimer>
#include <QPointer>
#include <QScreen>
#include <algorithm>

namespace VNX {

namespace {
constexpr int kSidebarWidth = 320;
constexpr double kInformationZoneShare = 0.3;

ads::CDockSplitter* rootSplitterFor(ads::CDockWidget* dock)
{
    ads::CDockAreaWidget* area = dock ? dock->dockAreaWidget() : nullptr;
    auto* root = area ? qobject_cast<ads::CDockSplitter*>(area->parentWidget()) : nullptr;
    if (!root)
        return nullptr;
    while (auto* parent = qobject_cast<ads::CDockSplitter*>(root->parentWidget()))
        root = parent;
    return root;
}

bool splitterPathTo(ads::CDockSplitter* root, QWidget* node, QList<int>* out)
{
    out->clear();
    QWidget* cursor = node;
    while (cursor && cursor != root) {
        auto* parent = qobject_cast<ads::CDockSplitter*>(cursor->parentWidget());
        if (!parent)
            return false;
        const int index = parent->indexOf(cursor);
        if (index < 0)
            return false;
        out->prepend(index);
        cursor = parent;
    }
    return cursor == root;
}

ads::CDockSplitter* splitterAtPath(ads::CDockSplitter* root, const QList<int>& path)
{
    ads::CDockSplitter* cursor = root;
    for (int index : path) {
        if (!cursor || index < 0 || index >= cursor->count())
            return nullptr;
        cursor = qobject_cast<ads::CDockSplitter*>(cursor->widget(index));
    }
    return cursor;
}

struct DockRange
{
    QWidget* first = nullptr;
    QWidget* last = nullptr;
    QRect bounds;
};

bool isOnInnerSide(QWidget* content, QWidget* boundary, ads::DockWidgetArea edge)
{
    const Qt::Orientation orientation = edge == ads::BottomDockWidgetArea
        ? Qt::Vertical : Qt::Horizontal;
    QWidget* node = boundary;
    while (auto* parent = qobject_cast<ads::CDockSplitter*>(node->parentWidget())) {
        if (parent->isAncestorOf(content)) {
            QWidget* sibling = content;
            while (sibling->parentWidget() != parent)
                sibling = sibling->parentWidget();
            if (parent->orientation() != orientation)
                return false;
            return edge == ads::LeftDockWidgetArea
                ? parent->indexOf(sibling) > parent->indexOf(node)
                : parent->indexOf(sibling) < parent->indexOf(node);
        }
        node = parent;
    }
    return false;
}

QRect visibleBounds(QWidget* node, QWidget* container)
{
    return node->isHidden() ? QRect() : QRect(node->mapTo(container, QPoint()), node->size());
}

bool collectDockRanges(QWidget* node, const QSet<QWidget*>& excluded, QWidget* container,
                       QList<DockRange>& ranges)
{
    if (excluded.contains(node))
        return true;
    auto* splitter = qobject_cast<ads::CDockSplitter*>(node);
    if (!splitter)
        return false;
    const int start = ranges.size();
    bool containsExcluded = false;
    DockRange run;
    for (int i = 0; i < splitter->count(); ++i) {
        QWidget* child = splitter->widget(i);
        if (collectDockRanges(child, excluded, container, ranges)) {
            containsExcluded = true;
            if (run.first)
                ranges.append(run);
            run = {};
        } else {
            if (!run.first)
                run.first = child;
            run.last = child;
            run.bounds = run.bounds.united(visibleBounds(child, container));
        }
    }
    if (!containsExcluded) {
        ranges.erase(ranges.begin() + start, ranges.end());
    } else if (run.first) {
        ranges.append(run);
    }
    return containsExcluded;
}
}

LayoutController::LayoutController(MainWindow* host)
    : QObject(host)
    , host_(host)
{
}

void LayoutController::setDocks(const Docks& d)
{
    dockManager_          = d.dockManager;
    blueprintCanvasDock_  = d.blueprintCanvasDock;
    projectExplorerDock_  = d.projectExplorerDock;
    aiDock_               = d.aiDock;
    previewDock_ = d.previewDock;
}

void LayoutController::setWindowDockResolver(WindowDockResolver resolver)
{
    windowDockResolver_ = std::move(resolver);
}

void LayoutController::setInstanceSnapshotProvider(std::function<QList<VNX::DockInstanceSpec>()> provider)
{
    instanceSnapshotProvider_ = std::move(provider);
}

void LayoutController::setInstanceRestoreFactory(std::function<bool(const VNX::DockInstanceSpec&)> factory)
{
    instanceRestoreFactory_ = std::move(factory);
}

void LayoutController::setVisibleSingletonMaterializer(std::function<void()> materializer)
{
    visibleSingletonMaterializer_ = std::move(materializer);
}

void LayoutController::setSingletonSnapshotProvider(std::function<QVariantMap()> provider)
{
    singletonSnapshotProvider_ = std::move(provider);
}

void LayoutController::setSingletonStateRestorer(std::function<void(const QVariantMap&)> restorer)
{
    singletonStateRestorer_ = std::move(restorer);
}

void LayoutController::setInstanceCloser(std::function<void()> closer)
{
    instanceCloser_ = std::move(closer);
}

void LayoutController::setLayoutsMenu(QMenu* menu)
{
    layoutsMenu_ = menu;
}

void LayoutController::setPlacementHint(const QString& objectName, PreferredOpenArea area)
{
    openPlacementHints_.insert(objectName, area);
}


LayoutController::PreferredOpenArea LayoutController::openPlacementFor(const QString& objectName) const
{
    return openPlacementHints_.value(objectName, PreferredOpenArea::Float);
}

ads::CDockAreaWidget* LayoutController::centralTabArea() const
{
    if (blueprintCanvasDock_) {
        if (auto* area = blueprintCanvasDock_->dockAreaWidget())
            return area;
    }
    return nullptr;
}

void LayoutController::dockIntoCentralTab(ads::CDockWidget* dock)
{
    if (!dockManager_ || !dock)
        return;
    if (auto* area = centralTabArea()) {
        if (dock->dockAreaWidget() != area)
            dockManager_->addDockWidget(ads::CenterDockWidgetArea, dock, area);
    } else {
        dockManager_->addDockWidget(ads::TopDockWidgetArea, dock);
    }
}

bool LayoutController::isDockLocked(ads::CDockWidget* dock) const
{
    auto* act = dock ? dock->toggleViewAction() : nullptr;
    return act && !act->isEnabled();
}

void LayoutController::revealDock(ads::CDockWidget* dock)
{
    if (!dock)
        return;
    const bool wasClosed = dock->isClosed();
    switch (openPlacementFor(dock->objectName())) {
        case PreferredOpenArea::CentralTab:
            dockIntoCentralTab(dock);
            dock->toggleView(true);
            if (auto* area = dock->dockAreaWidget())
                area->setCurrentDockWidget(dock);
            dock->setAsCurrentTab();
            break;
        case PreferredOpenArea::Float: {
            const bool wasFloating = dock->isInFloatingContainer();
            dock->toggleView(true);
            const bool createFloating = wasClosed && (!wasFloating || !dock->isFloating());
            if (createFloating) {
                dock->setFloating();
                if (auto* fc = dock->floatingDockContainer()) {
                    const QSize hint = dock->widget() ? dock->widget()->sizeHint() : QSize();
                    const int w = std::max(hint.width(),  900);
                    const int h = std::max(hint.height(), 600);
                    fc->resize(w, h);
                }
            }
            if (auto* fc = dock->floatingDockContainer()) {
                const QRect available = (createFloating && host_ ? host_->screen() : fc->screen())->availableGeometry();
                if (createFloating || !available.contains(fc->frameGeometry())) {
                    const QSize frameSize = fc->frameGeometry().size() - fc->size();
                    fc->resize(fc->size().boundedTo(available.size() - frameSize));
                    const QSize size = fc->frameGeometry().size();
                    const QPoint center = host_ ? host_->frameGeometry().center() : available.center();
                    QPoint position = createFloating
                        ? center - QPoint(size.width() / 2, size.height() / 2)
                        : fc->frameGeometry().topLeft();
                    position.setX(qMax(available.left(), qMin(position.x(), available.right() - size.width() + 1)));
                    position.setY(qMax(available.top(), qMin(position.y(), available.bottom() - size.height() + 1)));
                    fc->move(position);
                }
            }
            break;
        }
        case PreferredOpenArea::RestoreSaved:
            reanchorFloatingZoneDock(dock);
            dock->toggleView(true);
            break;
    }
}

bool LayoutController::revealPanel(const QString& objectName, bool visible)
{
    if (!dockManager_)
        return false;
    ads::CDockWidget* target = dockManager_->findDockWidget(objectName);
    if (!target)
        return false;
    if (visible)
        revealDock(target);
    else
        target->toggleView(false);
    return true;
}


QByteArray migrateDockNames(const QByteArray& state)
{
    static const QList<QPair<QByteArray, QByteArray>> kRenames = {
        {"\"AuditionDock\"", "\"PreviewDock\""},
        {"\"MonitorMeterDock\"", "\"MeterDock_1\""},
        {"\"MeterDock\"", "\"MeterDock_1\""},
    };
    QByteArray migrated = state;
    for (const auto& rename : kRenames)
        migrated.replace(rename.first, rename.second);
    return migrated;
}

void LayoutController::restoreDockState(const QByteArray& state)
{
    if (!dockManager_)
        return;
    const QByteArray xml = state.startsWith("<?xml") ? state : qUncompress(state);
    if ((xml.contains("\"MeterDock\"") || xml.contains("\"MonitorMeterDock\""))
        && !dockManager_->findDockWidget(QStringLiteral("MeterDock_1")))
        ensureDocksForSpecs({{QStringLiteral("Meter"), 1, true, {}, {}}});
    collapsedZones_.clear();
    dockManager_->restoreState(migrateDockNames(xml));
    if (visibleSingletonMaterializer_)
        visibleSingletonMaterializer_();
    emit dockStateRestored();
}

ads::CDockWidget* LayoutController::dockForWindow(const QString& typeId) const
{
    return windowDockResolver_ ? windowDockResolver_(typeId) : nullptr;
}

void LayoutController::hideMonitorDocks()
{
    const QStringList typeIds = monitorPanelTypeIds();
    for (const QString& typeId : typeIds) {
        if (typeId == QLatin1String("MonitorCatalog")) continue;
        if (ads::CDockWidget* dock = dockForWindow(typeId))
            dock->toggleView(false);
    }
}

QList<LayoutController::ZoneSpec> LayoutController::defaultZoneSpecs() const
{
    return {
        { blueprintCanvasDock_,   ads::TopDockWidgetArea,    true  },
        { aiDock_,                ads::RightDockWidgetArea,  true  },
        { projectExplorerDock_,   ads::LeftDockWidgetArea,   true  },
        { previewDock_,  ads::BottomDockWidgetArea, false },
    };
}

void LayoutController::seedZoneHomeAreas()
{
    for (const ZoneSpec& s : defaultZoneSpecs())
        if (s.dock)
            zoneHomeArea_.insert(s.dock->objectName(), s.area);
    if (ads::CDockWidget* informationDock = dockForWindow(QStringLiteral("Information")))
        zoneHomeArea_.insert(informationDock->objectName(), ads::LeftDockWidgetArea);
}

void LayoutController::applyDefaultLayout()
{
    if (singletonStateRestorer_)
        singletonStateRestorer_({});
    collapsedZones_.clear();
    seedZoneHomeAreas();

    for (const ZoneSpec& s : defaultZoneSpecs())
        placeDock(s.dock, s.area, nullptr, s.open);

    if (ads::CDockWidget* informationDock = dockForWindow(QStringLiteral("Information"));
        informationDock && projectExplorerDock_) {
        placeDock(informationDock, ads::BottomDockWidgetArea,
                  projectExplorerDock_->dockAreaWidget(), true);
    }

    if (blueprintCanvasDock_) blueprintCanvasDock_->setAsCurrentTab();

    enforceDisabledDocks();

    if (visibleSingletonMaterializer_)
        visibleSingletonMaterializer_();

    relayoutDockSplitters(host_ && host_->isVisible());

    emit defaultLayoutSeeded();
}


bool LayoutController::isFixedSubtree(QWidget* w) const
{
    static const QSet<QString> kPeripheral = {
        QStringLiteral("AIDock"),
        QStringLiteral("ExplorerDock"),
        QStringLiteral("InformationDock"),
        QStringLiteral("PreviewDock"),
    };
    if (auto* area = qobject_cast<ads::CDockAreaWidget*>(w)) {
        const auto docks = area->dockWidgets();
        if (docks.isEmpty())
            return true;
        for (ads::CDockWidget* d : docks) {
            if (!d || !kPeripheral.contains(d->objectName()))
                return false;
        }
        return true;
    }
    if (auto* splitter = qobject_cast<ads::CDockSplitter*>(w)) {
        for (int i = 0; i < splitter->count(); ++i) {
            if (!isFixedSubtree(splitter->widget(i)))
                return false;
        }
        return true;
    }
    return true;
}

int LayoutController::fixedZoneExtent(QWidget* child, bool horizontal, int total) const
{
    ads::CDockAreaWidget* area = qobject_cast<ads::CDockAreaWidget*>(child);
    if (!area)
        area = child->findChild<ads::CDockAreaWidget*>();
    QString name;
    if (area && !area->dockWidgets().isEmpty() && area->dockWidgets().first())
        name = area->dockWidgets().first()->objectName();
    if (!horizontal) {
        if (name == QLatin1String("InformationDock"))
            return qMax(60, qRound(total * kInformationZoneShare));
        if (name == QLatin1String("ExplorerDock"))
            return qMax(60, total - qRound(total * kInformationZoneShare));
        if (name == QLatin1String("PreviewDock"))
            return qMax(1, child->minimumSizeHint().height());
        return qMax(60, child->sizeHint().height());
    }
    return kSidebarWidth;
}

ads::CDockWidget* LayoutController::createEditorDock(WorkspaceDockCreateSpec spec,
                                                    ads::DockWidgetArea edge)
{
    if (!dockManager_ || !spec.initiallyVisible)
        return WorkspaceDockFactory::createDock(dockManager_, spec);

    ads::CDockSplitter* root = nullptr;
    for (int i = 0; i < dockManager_->dockAreaCount() && !root; ++i) {
        auto* area = dockManager_->dockArea(i);
        if (area->dockWidgetsCount())
            root = rootSplitterFor(area->dockWidget(0));
    }
    if (!root) {
        spec.area = edge;
        return WorkspaceDockFactory::createDock(dockManager_, spec);
    }

    QWidget* left = zoneSubtree(projectExplorerDock_);
    QWidget* right = zoneSubtree(aiDock_);
    QWidget* bottom = zoneSubtree(previewDock_);
    const QList<QPair<QWidget*, ads::DockWidgetArea>> boundaries{
        {left, ads::LeftDockWidgetArea},
        {zoneSubtree(dockForWindow(QStringLiteral("Information"))), ads::LeftDockWidgetArea},
        {right, ads::RightDockWidgetArea},
        {bottom, ads::BottomDockWidgetArea}
    };
    QSet<QWidget*> excluded;
    for (const auto& boundary : boundaries)
        if (boundary.first)
            excluded.insert(boundary.first);

    QList<DockRange> ranges;
    if (!collectDockRanges(root, excluded, dockManager_, ranges))
        ranges.append({root, root, visibleBounds(root, dockManager_)});
    DockRange selected;
    qint64 largest = -1;
    for (const DockRange& range : ranges) {
        if (!std::all_of(boundaries.begin(), boundaries.end(), [&](const auto& boundary) {
                return !boundary.first
                    || (isOnInnerSide(range.first, boundary.first, boundary.second)
                        && isOnInnerSide(range.last, boundary.first, boundary.second));
            }))
            continue;
        const qint64 size = qint64(range.bounds.width()) * range.bounds.height();
        if (size > largest) {
            selected = range;
            largest = size;
        }
    }

    spec.area = edge;
    spec.anchor = selected.first;
    spec.lastAnchor = selected.last;
    spec.initialShare = selected.bounds.isEmpty() ? 0.0 : 1.0 / 3.0;
    if (!spec.anchor) {
        spec.anchor = right ? right : left ? left : bottom;
        spec.area = right ? ads::LeftDockWidgetArea
            : left ? ads::RightDockWidgetArea : ads::TopDockWidgetArea;
    }
    auto* dock = WorkspaceDockFactory::createDock(dockManager_, spec);
    relayoutDockSplitters(selected.bounds.isEmpty());
    return dock;
}

void LayoutController::relayoutDockSplitters(bool seedSizes)
{
    if (!dockManager_)
        return;

    const QList<ads::CDockSplitter*> splitters =
        dockManager_->findChildren<ads::CDockSplitter*>();

    for (ads::CDockSplitter* s : splitters) {
        if (!s || !s->hasVisibleContent())
            continue;
        if (host_ && s->window() != host_)
            continue;
        const int n = s->count();
        const bool horizontal = (s->orientation() == Qt::Horizontal);

        QVector<bool> fixed(n);
        for (int i = 0; i < n; ++i) {
            fixed[i] = isFixedSubtree(s->widget(i));
            s->setStretchFactor(i, fixed[i] ? 0 : 1);
        }

        if (!seedSizes)
            continue;

        const int total = horizontal ? s->width() : s->height();
        if (total <= 0)
            continue;

        QList<int> sizes;
        sizes.reserve(n);
        int fixedSum = 0, elasticCount = 0;
        for (int i = 0; i < n; ++i) {
            QWidget* child = s->widget(i);
            if (child->isHidden()) {
                sizes.append(0);
            } else if (fixed[i]) {
                const int ext = fixedZoneExtent(child, horizontal, total);
                sizes.append(ext);
                fixedSum += ext;
            } else {
                sizes.append(0);
                ++elasticCount;
            }
        }
        const int per = elasticCount > 0 ? qMax(0, total - fixedSum) / elasticCount : 0;
        for (int i = 0; i < n; ++i) {
            if (!fixed[i] && !s->widget(i)->isHidden())
                sizes[i] = per;
        }
        s->setSizes(sizes);
    }
}

void LayoutController::reanchorFloatingZoneDock(ads::CDockWidget* dock)
{
    if (!dockManager_ || !dock || !dock->isInFloatingContainer())
        return;
    const auto it = zoneHomeArea_.constFind(dock->objectName());
    if (it == zoneHomeArea_.constEnd())
        return;
    placeDock(dock, it.value(), nullptr, true);
}

QWidget* LayoutController::zoneSubtree(ads::CDockWidget* representative) const
{
    if (!dockManager_ || !representative)
        return nullptr;
    ads::CDockAreaWidget* area = representative->dockAreaWidget();
    if (!area || area->dockContainer() != dockManager_)
        return nullptr;

    const auto it = zoneHomeArea_.constFind(representative->objectName());
    if (it == zoneHomeArea_.constEnd())
        return area;

    const bool sideZone = (it.value() == ads::LeftDockWidgetArea
                        || it.value() == ads::RightDockWidgetArea);
    const Qt::Orientation edgeSplit = sideZone ? Qt::Horizontal : Qt::Vertical;

    QWidget* node = area;
    while (auto* parent = qobject_cast<ads::CDockSplitter*>(node->parentWidget())) {
        if (parent->orientation() == edgeSplit)
            return node;
        node = parent;
    }
    return area;
}

void LayoutController::applyZoneSizes(const ZoneCollapse& state)
{
    for (const auto& entry : state.splitterSizes) {
        ads::CDockSplitter* splitter = entry.first;
        if (!splitter)
            continue;
        const QList<int>& saved = entry.second;
        const QList<int> current = splitter->sizes();
        if (current.size() != saved.size())
            continue;

        int available = 0;
        for (int extent : current)
            available += extent;
        int intended = 0;
        for (int extent : saved)
            intended += extent;
        if (available == intended && !state.restored)
            continue;

        QList<int> elastic;
        for (int i = 0; i < saved.size(); ++i)
            if (!isFixedSubtree(splitter->widget(i)))
                elastic.append(i);

        QList<int> target = saved;
        if (!elastic.isEmpty()) {
            const int delta = available - intended;
            const int share = delta / elastic.size();
            for (int i : elastic)
                target[i] = qMax(0, target[i] + share);
            target[elastic.last()] += delta - share * elastic.size();
        }
        splitter->setSizes(target);
    }
}

QList<ads::CDockAreaWidget*> LayoutController::zoneAreas(QWidget* node)
{
    if (auto* area = qobject_cast<ads::CDockAreaWidget*>(node))
        return { area };
    return node->findChildren<ads::CDockAreaWidget*>();
}

QJsonArray LayoutController::collapsedZonesToJson() const
{
    QJsonArray zones;
    if (!dockManager_)
        return zones;

    for (auto zone = collapsedZones_.constBegin(); zone != collapsedZones_.constEnd(); ++zone) {
        ads::CDockSplitter* root =
            rootSplitterFor(dockManager_->findDockWidget(zone.key()));
        if (!root)
            continue;

        QJsonArray hidden;
        for (const QPointer<ads::CDockWidget>& dock : zone->hidden)
            if (dock)
                hidden.append(dock->objectName());
        if (hidden.isEmpty())
            continue;

        QJsonArray splitters;
        for (const auto& entry : zone->splitterSizes) {
            QList<int> path;
            if (!entry.first || !splitterPathTo(root, entry.first, &path))
                continue;
            QJsonArray pathJson;
            for (int index : path)
                pathJson.append(index);
            QJsonArray sizesJson;
            for (int extent : entry.second)
                sizesJson.append(extent);
            QJsonObject splitter;
            splitter[QLatin1String("path")] = pathJson;
            splitter[QLatin1String("sizes")] = sizesJson;
            splitters.append(splitter);
        }

        QJsonObject json;
        json[QLatin1String("key")] = zone.key();
        json[QLatin1String("hidden")] = hidden;
        json[QLatin1String("splitters")] = splitters;
        zones.append(json);
    }
    return zones;
}

void LayoutController::collapsedZonesFromJson(const QJsonArray& zones)
{
    if (!dockManager_)
        return;

    for (const QJsonValue& value : zones) {
        const QJsonObject json = value.toObject();
        const QString key = json.value(QLatin1String("key")).toString();
        ads::CDockSplitter* root = rootSplitterFor(dockManager_->findDockWidget(key));
        if (!root)
            continue;

        ZoneCollapse state;
        state.restored = true;
        for (const QJsonValue& name : json.value(QLatin1String("hidden")).toArray())
            if (ads::CDockWidget* dock = dockManager_->findDockWidget(name.toString()))
                state.hidden.append(dock);
        if (state.hidden.isEmpty())
            continue;

        for (const QJsonValue& entry : json.value(QLatin1String("splitters")).toArray()) {
            const QJsonObject splitterJson = entry.toObject();
            QList<int> path;
            for (const QJsonValue& index : splitterJson.value(QLatin1String("path")).toArray())
                path.append(index.toInt(-1));
            ads::CDockSplitter* splitter = splitterAtPath(root, path);
            if (!splitter)
                continue;
            QList<int> sizes;
            for (const QJsonValue& extent : splitterJson.value(QLatin1String("sizes")).toArray())
                sizes.append(extent.toInt());
            if (sizes.size() != splitter->count())
                continue;
            state.splitterSizes.append({ splitter, sizes });
        }
        if (state.splitterSizes.isEmpty())
            continue;

        collapsedZones_.insert(key, state);
    }
}

void LayoutController::setZoneCollapsed(ads::CDockWidget* representative, bool collapsed)
{
    if (!representative)
        return;
    const QString key = representative->objectName();

    if (!collapsed) {
        const auto it = collapsedZones_.find(key);
        if (it != collapsedZones_.end()) {
            const ZoneCollapse state = *it;
            collapsedZones_.erase(it);
            for (const QPointer<ads::CDockWidget>& dock : state.hidden)
                if (dock)
                    dock->toggleView(true);
            QTimer::singleShot(0, this, [this, state]() { applyZoneSizes(state); });
            return;
        }
        QWidget* node = zoneSubtree(representative);
        if (!node) {
            representative->toggleView(true);
            return;
        }
        for (ads::CDockAreaWidget* area : zoneAreas(node))
            for (ads::CDockWidget* dock : area->dockWidgets())
                dock->toggleView(true);
        relayoutDockSplitters(true);
        return;
    }

    QWidget* node = zoneSubtree(representative);
    if (!node) {
        representative->toggleView(false);
        return;
    }

    const QList<ads::CDockAreaWidget*> areas = zoneAreas(node);

    ZoneCollapse state;
    if (auto* parent = qobject_cast<ads::CDockSplitter*>(node->parentWidget()))
        state.splitterSizes.append({ parent, parent->sizes() });
    if (auto* splitter = qobject_cast<ads::CDockSplitter*>(node))
        state.splitterSizes.append({ splitter, splitter->sizes() });
    for (ads::CDockSplitter* splitter : node->findChildren<ads::CDockSplitter*>())
        state.splitterSizes.append({ splitter, splitter->sizes() });

    for (ads::CDockAreaWidget* area : areas)
        for (ads::CDockWidget* dock : area->openedDockWidgets())
            state.hidden.append(dock);

    if (state.hidden.isEmpty())
        return;

    for (const QPointer<ads::CDockWidget>& dock : state.hidden)
        if (dock)
            dock->toggleView(false);
    collapsedZones_.insert(key, state);
}

ads::CDockAreaWidget* LayoutController::placeDock(ads::CDockWidget* dock, ads::DockWidgetArea zone,
                                                 ads::CDockAreaWidget* anchor, bool open)
{
    if (!dock || !dockManager_)
        return anchor;
    ads::CDockAreaWidget* area = dockManager_->addDockWidget(zone, dock, anchor);
    dock->toggleView(open);
    if (open) {
        if (area)
            area->setCurrentDockWidget(dock);
        dock->setAsCurrentTab();
        if (dock->tabWidget())
            dock->tabWidget()->show();
    }
    return area;
}


void LayoutController::enforceDisabledDocks()
{
    hideMonitorDocks();
}


static QString projectLayoutCacheFile()
{
    const QString root = AssetRegistry::instance().projectRoot();
    if (root.isEmpty())
        return QString();
    return PathUtils::getDotCacheDir(root) + "/editor_layout.json";
}

void LayoutController::loadProjectLayout()
{
    QByteArray state;
    QList<VNX::DockInstanceSpec> specs;
    QVariantMap singletons;
    QJsonArray collapsed;

    const QString path = projectLayoutCacheFile();
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                file.readAll(), &parseError);
            const QJsonObject obj = document.isObject() ? document.object()
                                                          : QJsonObject();
            file.close();
            const bool valid = parseError.error == QJsonParseError::NoError
                && obj.value(QLatin1String("instances")).isArray()
                && obj.value(QLatin1String("collapsedZones")).isArray()
                && obj.value(QLatin1String("baseLayout")).isString()
                && (!obj.contains(QLatin1String("state"))
                    || obj.value(QLatin1String("state")).isString());
            if (valid) {
                state = QByteArray::fromBase64(
                    obj.value(QLatin1String("state")).toString().toLatin1());
                specs = workspaceWindowInstanceSpecsFromJson(
                    obj.value(QLatin1String("instances")).toArray());
                singletons = obj.value(QLatin1String("singletons")).toObject().toVariantMap();
                collapsed = obj.value(QLatin1String("collapsedZones")).toArray();
                currentLayoutName_ = obj.value(QLatin1String("baseLayout")).toString();
            }
        }
    }

    ensureDocksForSpecs(specs);
    seedZoneHomeAreas();

    if (dockManager_ && !state.isEmpty()) {
        if (singletonStateRestorer_)
            singletonStateRestorer_(singletons);
        restoreDockState(state);
        collapsedZonesFromJson(collapsed);
        currentLayoutFromCache_ = true;
        relayoutDockSplitters(false);
    } else {
        currentLayoutFromCache_ = false;
        currentLayoutName_.clear();
        applyDefaultLayout();
    }

    enforceDisabledDocks();
}

void LayoutController::saveProjectLayout()
{
    const QString path = projectLayoutCacheFile();
    if (path.isEmpty())
        return;
    if (!PathUtils::ensureDirectory(QFileInfo(path).absolutePath())) {
        LogPanel::logError(LogCategory::Layout,
                           tr("Failed to prepare the project layout folder."));
        return;
    }

    QJsonObject obj;
    if (dockManager_)
        obj["state"] = QString::fromLatin1(dockManager_->saveState().toBase64());
    obj["instances"] = workspaceWindowInstanceSpecsToJson(instanceSnapshotProvider_
                                        ? instanceSnapshotProvider_()
                                        : QList<VNX::DockInstanceSpec>());
    if (singletonSnapshotProvider_)
        obj["singletons"] = QJsonObject::fromVariantMap(singletonSnapshotProvider_());
    obj["collapsedZones"] = collapsedZonesToJson();
    obj["baseLayout"] = currentLayoutName_;

    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
        if (file.write(bytes) != bytes.size() || !file.commit())
            LogPanel::logError(LogCategory::Layout,
                               tr("Failed to commit project layout cache: %1").arg(path));
    } else {
        LogPanel::logError(LogCategory::Layout, tr("Failed to write project layout cache: %1").arg(path));
    }
}

void LayoutController::ensureDocksForSpecs(const QList<VNX::DockInstanceSpec>& specs)
{
    if (!instanceRestoreFactory_)
        return;

    QStringList dropped;
    for (const VNX::DockInstanceSpec& spec : specs) {
        const bool ok = instanceRestoreFactory_(spec);
        if (!ok)
            dropped.append(QStringLiteral("%1_%2").arg(spec.typeId).arg(spec.instanceId));
    }

    if (!dropped.isEmpty())
        qInfo() << "[LayoutController] dropped" << dropped.size()
                << "unrestorable docks:" << dropped.join(QStringLiteral(", "));
}

void LayoutController::selectDefaultLayout()
{
    if (instanceCloser_)
        instanceCloser_();
    currentLayoutName_.clear();
    applyDefaultLayout();
    saveProjectLayout();
    emit statusMessage(tr("Layout 'Default' loaded"));
}

bool LayoutController::selectBuiltinLayout(const QString& builtinId)
{
    if (builtinId == QLatin1String("default_layout")) {
        selectDefaultLayout();
        return true;
    }
    return false;
}

void LayoutController::onResetCurrentLayout()
{
    resetCurrentLayout();
}

bool LayoutController::resetCurrentLayout()
{
    if (currentLayoutName_.isEmpty()) {
        selectDefaultLayout();
        return true;
    }

    const QString name = currentLayoutName_;
    QJsonObject json;
    if (!NamedLayoutStore::read(name, &json)) {
        emit statusMessage(tr("Layout '%1' not found").arg(name));
        return false;
    }

    if (instanceCloser_)
        instanceCloser_();
    return applyNamedLayoutJson(name, json);
}

void LayoutController::onSaveNamedLayout()
{
    bool ok = false;
    QString name = QInputDialog::getText(host_, tr("Save Layout"),
                                         tr("Layout name:"), QLineEdit::Normal,
                                         QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    if (NamedLayoutStore::isReservedName(name)) {
        emit statusMessage(tr("Layout name '%1' is reserved").arg(name));
        return;
    }

    if (!dockManager_) return;
    QByteArray dockState = dockManager_->saveState();

    QJsonObject json;
    json["name"] = name;
    json["dockState"] = QString::fromLatin1(dockState.toBase64());
    json["instances"] = workspaceWindowInstanceSpecsToJson(instanceSnapshotProvider_
                                        ? instanceSnapshotProvider_()
                                        : QList<VNX::DockInstanceSpec>());
    if (singletonSnapshotProvider_)
        json["singletons"] = QJsonObject::fromVariantMap(singletonSnapshotProvider_());
    json["collapsedZones"] = collapsedZonesToJson();
    json["savedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    if (NamedLayoutStore::write(name, json)) {
        currentLayoutName_ = name;
        saveProjectLayout();
        emit statusMessage(tr("Layout '%1' saved").arg(name));
    } else {
        emit statusMessage(tr("Layout '%1' could not be saved").arg(name));
    }
}

void LayoutController::onLoadNamedLayout(const QString& name)
{
    loadNamedLayout(name);
}

bool LayoutController::loadNamedLayout(const QString& name)
{
    QJsonObject json;
    if (!NamedLayoutStore::read(name, &json)) {
        emit statusMessage(tr("Layout '%1' not found").arg(name));
        return false;
    }
    return applyNamedLayoutJson(name, json);
}

bool LayoutController::applyNamedLayoutJson(const QString& name, const QJsonObject& json)
{
    ensureDocksForSpecs(
        workspaceWindowInstanceSpecsFromJson(json.value("instances").toArray()));
    if (singletonStateRestorer_)
        singletonStateRestorer_(json.value(QLatin1String("singletons")).toObject().toVariantMap());

    QByteArray dockState = QByteArray::fromBase64(json["dockState"].toString().toLatin1());
    if (!dockState.isEmpty() && dockManager_) {
        restoreDockState(dockState);
        collapsedZonesFromJson(json.value(QLatin1String("collapsedZones")).toArray());
        relayoutDockSplitters(false);
    }
    enforceDisabledDocks();
    currentLayoutName_ = name;
    saveProjectLayout();
    emit statusMessage(tr("Layout '%1' loaded").arg(name));
    return true;
}

void LayoutController::onDeleteNamedLayout(const QString& name)
{
    if (NamedLayoutStore::remove(name)) {
        if (currentLayoutName_ == name)
            currentLayoutName_.clear();
        emit statusMessage(tr("Layout '%1' deleted").arg(name));
    }
}

void LayoutController::onNamedLayoutRenamed(const QString& oldName, const QString& newName)
{
    if (currentLayoutName_ != oldName)
        return;
    currentLayoutName_ = newName;
    saveProjectLayout();
}

void LayoutController::onNamedLayoutRemoved(const QString& name)
{
    if (currentLayoutName_ != name)
        return;
    currentLayoutName_.clear();
    saveProjectLayout();
}

void LayoutController::rebuildLayoutsMenu()
{
    if (!layoutsMenu_) return;
    layoutsMenu_->clear();
    delete layoutActionGroup_;
    layoutActionGroup_ = new QActionGroup(layoutsMenu_);
    layoutActionGroup_->setExclusive(true);

    const QList<NamedLayoutInfo> layouts = NamedLayoutStore::list();

    if (!currentLayoutName_.isEmpty()) {
        const bool exists = std::any_of(layouts.cbegin(), layouts.cend(),
            [this](const NamedLayoutInfo& info) { return info.name == currentLayoutName_; });
        if (!exists) {
            currentLayoutName_.clear();
            saveProjectLayout();
        }
    }

    for (const BuiltinLayouts::Entry& builtin : BuiltinLayouts::entries()) {
        const QString label = builtin.reservedName == QLatin1String("Default")
            ? tr("Default")
            : builtin.reservedName;
        QAction* builtinAction = layoutsMenu_->addAction(label);
        if (!builtin.menuActionObjectName.isEmpty())
            builtinAction->setObjectName(builtin.menuActionObjectName);
        builtinAction->setCheckable(true);
        builtinAction->setActionGroup(layoutActionGroup_);
        const bool isDefault = builtin.id == QLatin1String("default_layout");
        builtinAction->setChecked(isDefault
            ? currentLayoutName_.isEmpty()
            : currentLayoutName_ == builtin.id);
        const QString builtinId = builtin.id;
        connect(builtinAction, &QAction::triggered, this, [this, builtinId]() {
            selectBuiltinLayout(builtinId);
        });
    }

    for (const NamedLayoutInfo& info : layouts) {
        QAction* action = layoutsMenu_->addAction(info.name);
        action->setCheckable(true);
        action->setActionGroup(layoutActionGroup_);
        action->setChecked(info.name == currentLayoutName_);
        const QString presetName = info.name;
        connect(action, &QAction::triggered, this, [this, presetName]() {
            onLoadNamedLayout(presetName);
        });
    }

    layoutsMenu_->addSeparator();

    QAction* manageAction = layoutsMenu_->addAction(tr("Manage Layouts..."));
    manageAction->setObjectName("manageLayoutsAction");
    connect(manageAction, &QAction::triggered,
            this, &LayoutController::manageLayoutsRequested);

    QAction* resetAction = layoutsMenu_->addAction(tr("Reset Current Layout"));
    resetAction->setObjectName("resetLayoutAction");
    connect(resetAction, &QAction::triggered,
            this, &LayoutController::onResetCurrentLayout);

    QAction* saveAction = layoutsMenu_->addAction(tr("Save Current Layout..."));
    saveAction->setObjectName("saveLayoutAction");
    connect(saveAction, &QAction::triggered,
            this, &LayoutController::onSaveNamedLayout);
}

}
