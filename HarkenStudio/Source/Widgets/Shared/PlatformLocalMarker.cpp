#include "PlatformLocalMarker.h"

#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyDescriptor.h"
#include "Core/PropertyEditSession.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeLayout.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QtMath>

#include <algorithm>
#include <numeric>

namespace harken::studio {

PlatformLocalMarker::PlatformLocalMarker(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("platformLocalMarker"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAccessibleName(tr("Platform Mark"));
    setToolTip(tr("Platform Mark"));
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics& metrics) {
        setFixedWidth(metrics.inspector.platformMarkerWidth);
    });
    auto& manager = ProjectManager::instance();
    const auto changed = [this](const ProjectItem* item) {
        if (item && item->guid == item_) refresh();
    };
    connect(&manager, &ProjectManager::itemPropertyChanged, this, changed);
    connect(&manager, &ProjectManager::itemPropertyPreviewed, this, changed);
    connect(&manager, &ProjectManager::busChanged, this, [this] { refresh(); });
    connect(&manager, &ProjectManager::targetPlatformsChanged, this, [this] { refresh(); });
    connect(&manager, &ProjectManager::projectClosed, this, [this] { bind(nullptr, {}); });
    connect(&manager, &ProjectManager::itemDeleted, this, [this](const ProjectItem* item) {
        if (item && item->guid == item_) bind(nullptr, {});
    });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this] { refresh(); });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, [this] { update(); });
}

void PlatformLocalMarker::setValueFormatter(std::function<QString(const QVariant&)> formatter)
{
    formatter_ = std::move(formatter);
}

void PlatformLocalMarker::setValuesView(PlatformValuesView view)
{
    valuesView_ = std::move(view);
}

void PlatformLocalMarker::bind(const ProjectItem* item, const std::optional<PlatformLocalAddress>& address)
{
    item_ = item && address ? item->guid : QUuid{};
    address_ = address;
    refresh();
}

void PlatformLocalMarker::setEditable(bool editable)
{
    editable_ = editable;
    update();
}

void PlatformLocalMarker::setEdges(Qt::Edges edges)
{
    edges_ = edges;
    update();
}

ProjectItem* PlatformLocalMarker::boundItem() const
{
    return item_.isNull() ? nullptr : ProjectManager::instance().findItemByGuid(item_);
}

bool PlatformLocalMarker::interactive() const
{
    return state_ != State::None
        || (editable_ && boundItem() && PlatformContextService::instance().context().primary != ProjectPlatformId{});
}

void PlatformLocalMarker::refresh()
{
    auto next = State::None;
    const auto* project = ProjectManager::instance().currentProject();
    if (const auto* item = boundItem(); item && project) {
        const auto locals = PlatformLocalState::localPlatforms(*project, *item, *address_);
        const auto current = PlatformContextService::instance().context().primary;
        if (locals.contains(current)) next = State::CurrentPlatform;
        else if (!locals.isEmpty()) next = State::OtherPlatform;
    }
    setCursor(next == State::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
    if (next == state_) return;
    state_ = next;
    update();
}

void PlatformLocalMarker::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && state_ != State::None) showValues();
    event->accept();
}

void PlatformLocalMarker::contextMenuEvent(QContextMenuEvent* event)
{
    event->accept();
    if (!boundItem() || !editable_
        || PlatformContextService::instance().context().primary == ProjectPlatformId{}) return;
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("platformLocalMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const bool local = state_ == State::CurrentPlatform;
    auto* action = menu->addAction(local ? tr("Remove Platform Override") : tr("Override on Current Platform"));
    connect(action, &QAction::triggered, this, [this, local] { changeLocal(!local); });
    menu->popup(event->globalPos());
}

void PlatformLocalMarker::changeLocal(bool local)
{
    PropertyEditSession::finishAllActive(true);
    auto* item = boundItem();
    const auto* project = ProjectManager::instance().currentProject();
    if (!item || !project || !address_) return;
    auto& service = PlatformContextService::instance();
    const auto platform = service.context().primary;
    const auto result = local
        ? PlatformLocalState::setLocal(*project, *item, *address_, platform, this)
        : PlatformLocalState::clearLocal(*item, *address_, platform, this);
    if (result.success && local && service.context().all) service.setAll(false);
    refresh();
    emit localChanged();
}

void PlatformLocalMarker::showValues()
{
    auto* item = boundItem();
    const auto* project = ProjectManager::instance().currentProject();
    if (!item || !project) return;
    const auto locals = PlatformLocalState::localPlatforms(*project, *item, *address_);
    const auto current = PlatformContextService::instance().context().primary;
    auto* popup = new QWidget(this, Qt::Popup);
    popup->setObjectName(QStringLiteral("platformLocalPopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setAttribute(Qt::WA_StyledBackground);
    QtControlSkin::setSemanticProperty(popup, "surfaceRole", QStringLiteral("popup"));
    auto* layout = new QGridLayout(popup);
    ThemeLayout::margins(layout, ThemeLayout::Space::Panel);
    ThemeLayout::verticalSpacing(layout, ThemeLayout::Space::Tight);
    ThemeLayout::horizontalSpacing(layout, ThemeLayout::Space::Section);
    const auto platformName = [&](const ProjectPlatform& platform) {
        return platform.id == current ? tr("%1 (current)").arg(platform.name) : platform.name;
    };
    if (valuesView_) {
        QList<PlatformMarkValue> values;
        for (const auto& platform : project->platforms)
            values.append({platformName(platform),
                           PlatformLocalState::effectiveValue(*project, *item, *address_, platform.id)});
        valuesView_(layout, popup, values);
    }
    const bool scalar = PlatformLocalState::hasScalarValue(*address_);
    for (int row = 0; !valuesView_ && row < project->platforms.size(); ++row) {
        const auto& platform = project->platforms[row];
        auto* name = new QLabel(platformName(platform), popup);
        name->setTextFormat(Qt::PlainText);
        layout->addWidget(name, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        QString text;
        if (scalar) {
            const auto value = PlatformLocalState::effectiveValue(*project, *item, *address_, platform.id);
            text = !value.isValid() ? tr("None") : formatter_ ? formatter_(value) : value.toString();
        } else {
            text = locals.contains(platform.id) ? tr("Local") : tr("Shared");
        }
        auto* value = new QLabel(text, popup);
        value->setTextFormat(Qt::PlainText);
        layout->addWidget(value, row, 1, Qt::AlignRight | Qt::AlignVCenter);
    }
    popup->adjustSize();
    QPoint position = mapToGlobal(QPoint(0, height()));
    if (const auto* screen = QGuiApplication::screenAt(position)) {
        const QRect available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right() - popup->width() + 1)));
        position.setY(std::clamp(position.y(), available.top(),
            std::max(available.top(), available.bottom() - popup->height() + 1)));
    }
    popup->move(position);
    popup->show();
}

void PlatformLocalMarker::enterEvent(QEnterEvent* event)
{
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void PlatformLocalMarker::leaveEvent(QEvent* event)
{
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void PlatformLocalMarker::paintEvent(QPaintEvent*)
{
    const auto snapshot = ThemeManager::instance().snapshot();
    const auto& colors = snapshot->ui.colors;
    QPainter painter(this);
    painter.fillRect(rect(), colors.panelBackground);
    if (state_ != State::None) {
        QColor fill = editable_ ? colors.accent : colors.textDisabled;
        if (editable_ && state_ == State::OtherPlatform)
            fill.setAlphaF(fill.alphaF() * snapshot->thirdParty.qtAds.activeTabAccentOpacity);
        painter.fillRect(rect(), fill);
    }
    if (hovered_ && interactive()) painter.fillRect(rect(), colors.hoverOverlay);
    const int border = snapshot->ui.metrics.borderWidth;
    if (edges_ & Qt::TopEdge) painter.fillRect(0, 0, width(), border, colors.borderSecondary);
    if (edges_ & Qt::BottomEdge) painter.fillRect(0, height() - border, width(), border, colors.borderSecondary);
    if (edges_ & Qt::RightEdge) painter.fillRect(width() - border, 0, border, height(), colors.borderSecondary);
    if (edges_ & Qt::LeftEdge) painter.fillRect(0, 0, border, height(), colors.borderSecondary);
}

std::function<QString(const QVariant&)> PlatformLocalMarker::formatterFor(const PropertyDescriptor& descriptor)
{
    return [descriptor](const QVariant& value) -> QString {
        if (descriptor.type == PropType::ObjectRef) {
            auto& manager = ProjectManager::instance();
            const bool byGuid = value.metaType().id() == QMetaType::QUuid
                || value.metaType().id() == QMetaType::QString;
            const auto guid = value.metaType().id() == QMetaType::QUuid ? value.toUuid() : QUuid(value.toString());
            const auto* target = byGuid ? (guid.isNull() ? nullptr : manager.findItemByGuid(guid))
                                        : manager.findItemByObjectID(value.toUInt());
            if (target) return target->name;
            return (byGuid ? guid.isNull() : value.toUInt() == 0) ? tr("None") : tr("Missing");
        }
        if (descriptor.type == PropType::Bool) return value.toBool() ? tr("On") : tr("Off");
        for (const auto& segment : descriptor.segments)
            if (!segment.numeric && segment.value.toDouble() == value.toDouble()) return segment.text;
        const int decimals = descriptor.type == PropType::Int ? 0 : descriptor.decimals;
        return QString::number(value.toDouble() * descriptor.displayScale, 'f', decimals) + descriptor.unit;
    };
}

void showPlatformComparison(QGridLayout* layout, QWidget* popup, const QString& rowTitle,
                            const QList<PlatformMarkValue>& platforms, const std::vector<QStringList>& rows,
                            const QString& identical, Qt::Alignment valueAlignment)
{
    if (rows.empty()) {
        auto* label = new QLabel(identical, popup);
        label->setTextFormat(Qt::PlainText);
        layout->addWidget(label, 0, 0);
        return;
    }
    QStringList headers{rowTitle};
    for (const auto& entry : platforms) headers.append(entry.platform);
    auto* table = new TableWidget(TableOptions::display(TableBodySizing::Content), popup);
    const QFontMetrics metrics(table->font());
    const int padding = ThemeManager::instance().metrics().table.cellPaddingX * 2;
    QList<int> widths;
    for (int column = 0; column < headers.size(); ++column) {
        int width = metrics.horizontalAdvance(headers[column]);
        for (const auto& cells : rows) width = std::max(width, metrics.horizontalAdvance(cells[column]));
        widths.append(width + padding);
    }
    auto* view = table->createItemView(QStringLiteral("platformComparison"), headers, widths);
    view->setRootIsDecorated(false);
    view->setSelectionMode(QAbstractItemView::NoSelection);
    view->setFocusPolicy(Qt::NoFocus);
    for (int column = 1; column < headers.size(); ++column)
        view->headerItem()->setTextAlignment(column, valueAlignment | Qt::AlignVCenter);
    for (const auto& cells : rows) {
        auto* item = new QTreeWidgetItem(view, cells);
        for (int column = 1; column < cells.size(); ++column)
            item->setTextAlignment(column, valueAlignment | Qt::AlignVCenter);
    }
    view->ensurePolished();
    const double total = std::accumulate(widths.begin(), widths.end(), 0);
    double width = 0.0;
    for (int column = 0; column < headers.size(); ++column) {
        const int needed = std::max(static_cast<QAbstractItemView*>(view)->sizeHintForColumn(column),
                                    view->header()->sectionSizeHint(column));
        width = std::max(width, needed * total / widths[column]);
    }
    table->setMinimumWidth(qCeil(width) + view->frameWidth() * 2 + table->frameWidth() * 2);
    table->refreshLayout();
    layout->addWidget(table, 0, 0);
}

QWidget* attachPlatformMarker(QWidget* control, PlatformLocalMarker* marker, QWidget* parent,
                              PlatformMarkerHeight height)
{
    if (const auto* check = qobject_cast<QCheckBox*>(control); check && check->text().isEmpty())
        QtControlSkin::setSemanticProperty(control, "platformMarked", true);
    auto* host = new QWidget(parent);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    host->setSizePolicy(control->sizePolicy());
    layout->addWidget(control, control->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding ? 1 : 0);
    if (height == PlatformMarkerHeight::Control) {
        layout->addWidget(marker);
    } else {
        ThemeManager::instance().bindMetrics(marker, [marker](const ThemeMetrics& metrics) {
            marker->setFixedSize(metrics.inspector.platformMarkerWidth, QtControlSkin::current().fieldHeight());
        });
        layout->addWidget(marker, 0, Qt::AlignVCenter);
    }
    return host;
}

}
