#include "TimelineHeaderColumn.h"

#include "TimelineEditorPanel.h"
#include "TimelineRenderer.h"
#include "Widgets/Common/AmplitudeCurvePopup.h"
#include "Widgets/Common/TableWidget.h"
#include "Core/QtControlSkin.h"

#include "Core/AuthoringPolicy.h"
#include "Core/FontManager.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeManager.h"
#include "Widgets/Shared/InlineRenameEdit.h"
#include "Widgets/Shared/RenameClickHandler.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace VNX {

namespace {

int laneIndexAt(const TimelineEditorPanel* panel, const QWidget* widget)
{
    for (const QWidget* current = widget; current; current = current->parentWidget()) {
        const QVariant guidValue = current->property("trackGuid");
        if (!guidValue.isValid())
            continue;
        const QUuid guid = QUuid::fromString(guidValue.toString());
        for (int index = 0; index < panel->laneCount(); ++index) {
            if (panel->laneGuid(index) == guid)
                return index;
        }
    }
    return -1;
}

}

TimelineHeaderColumn::TimelineHeaderColumn(TimelineEditorPanel* panel, QWidget* parent)
    : QWidget(parent)
    , panel_(panel)
{
    setObjectName(QStringLiteral("timelineHeaderColumn"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 1, 0);
    layout->setSpacing(0);
    table_ = new TableWidget(this);
    table_->setColumns(QStringLiteral("timelineTracks"), {
        {QStringLiteral("index"), QString(), 0},
        {QStringLiteral("track"), tr("Track"), 1}});
    table_->setHeaderVisible(false);
    table_->scrollArea()->viewport()->installEventFilter(this);
    laneLayout_ = table_->rowsLayout();
    layout->addWidget(table_);
    layout->addStretch(1);
    table_->setRowMoveHandler([this](int from, int to) { panel_->reorderLane(from, to); });
    table_->setRowSelectionHandler([this](QWidget* row) {
        panel_->selectLane(QUuid::fromString(row->property("trackGuid").toString()), true);
    });
    table_->setSelectionClearHandler([this] { panel_->selectLane({}); });

    new RenameClickHandler(this, this, [this](const QPoint& point) -> RenameClickHandler::Target {
        auto* name = qobject_cast<QLabel*>(childAt(point));
        if (!name || name->objectName() != QLatin1String("timelineLaneName")) return {};
        const int lane = laneIndexAt(panel_, name);
        if (lane < 0 || panel_->headerSelectedLane() != lane) return {};
        const QRect label = QStyle::alignedRect(name->layoutDirection(), name->alignment(),
            name->fontMetrics().size(Qt::TextSingleLine, name->text()), name->contentsRect());
        if (!label.contains(name->mapFrom(this, point))) return {};
        const QUuid guid = panel_->laneGuid(lane);
        return {guid, [this, guid] { renameLane(guid); }};
    });
}

void TimelineHeaderColumn::rebuildRows()
{
    while (auto* item = laneLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    table_->setVisible(panel_->viewKind() == TimelineEditorPanel::ViewKind::Segment);
    if (panel_->viewKind() != TimelineEditorPanel::ViewKind::Segment)
        return;
    for (int lane = 0; lane < panel_->laneCount(); ++lane) {
        auto* row = new QWidget(table_->rowsWidget());
        row->setObjectName(QStringLiteral("timelineLane"));
        const QUuid guid = panel_->laneGuid(lane);
        row->setProperty("trackGuid", guid.toString(QUuid::WithoutBraces));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto* number = table_->createRowNumber(row, lane);
        number->installEventFilter(this);
        auto* cell = table_->createCell(row);
        cell->setFocusProxy(row);
        cell->installEventFilter(this);
        auto* name = new QLabel(cell);
        name->setObjectName(QStringLiteral("timelineLaneName"));
        name->setTextFormat(Qt::PlainText);
        name->setText(panel_->laneName(lane));
        name->setToolTip(name->text());
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        name->setFocusProxy(row);
        name->installEventFilter(this);
        cell->addWidget(name);
        layout->addWidget(number);
        layout->addWidget(cell, 1);
        laneLayout_->addWidget(row);
        table_->registerRow(row, {number, cell});
        row->setFocusPolicy(Qt::StrongFocus);
        row->installEventFilter(this);
        row->show();
    }
    refreshRows();
}

void TimelineHeaderColumn::refreshRows()
{
    layout()->setContentsMargins(0, 0, panel_->laneHeaderResizeWidth() + 1, 0);
    laneLayout_->setSpacing(0);
    for (int lane = 0; lane < laneLayout_->count(); ++lane) {
        auto* row = laneLayout_->itemAt(lane)->widget();
        row->ensurePolished();
        const auto margins = row->contentsMargins();
        row->findChild<TableCell*>()->setMinimumHeight(
            panel_->laneHeight(lane) - margins.top() - margins.bottom());
    }
    table_->refreshLayout();
}

QRect TimelineHeaderColumn::laneRect(int index) const
{
    const auto* item = laneLayout_->itemAt(index);
    const auto* row = item ? item->widget() : nullptr;
    return row ? QRect(row->mapTo(this, QPoint()), row->size()) : QRect();
}

void TimelineHeaderColumn::paintEvent(QPaintEvent*)
{
    if (panel_->viewKind() != TimelineEditorPanel::ViewKind::Track)
        return;
    const auto theme = ThemeManager::instance().snapshot();
    QPainter painter(this);
    painter.fillRect(rect(), theme->ui.colors.bgSecondary);
    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics metrics(painter.font());
    painter.setPen(theme->ui.colors.textSecondary);
    const auto ticks = TimelineRenderer::amplitudeTicks(panel_->trackWaveformRect(),
        panel_->verticalZoom(), panel_->verticalScroll(), *theme, panel_->amplitudeExponent());
    for (const auto& tick : ticks) {
        if (tick.label.isEmpty())
            continue;
        painter.drawLine(width() - 5, tick.y, width() - 2, tick.y);
        int labelTop = tick.y - metrics.height() / 2;
        if (tick.major) {
            const QRect viewport = panel_->trackWaveformRect();
            labelTop = std::clamp(labelTop, viewport.top(),
                std::max(viewport.top(), viewport.bottom() - metrics.height() + 1));
        }
        painter.drawText(QRect(1, labelTop, width() - 9, metrics.height()),
            Qt::AlignRight | Qt::AlignVCenter, tick.label);
    }
}

void TimelineHeaderColumn::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && panel_->viewKind() == TimelineEditorPanel::ViewKind::Track) {
        panel_->zoomVertically(0, 1.0 / panel_->verticalZoom());
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool TimelineHeaderColumn::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        panel_->scrollTracks(static_cast<QWheelEvent*>(event));
        return true;
    }
    if (auto* number = qobject_cast<QLabel*>(watched);
        number && number->property("dragHandleRow").isValid()) {
        if (event->type() == QEvent::FocusIn
            || (event->type() == QEvent::MouseButtonRelease
                && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton
                && number->rect().contains(static_cast<QMouseEvent*>(event)->position().toPoint())))
            panel_->selectLane(panel_->laneGuid(laneIndexAt(panel_, number)), true);
        return false;
    }
    if (auto* widget = qobject_cast<QWidget*>(watched);
        widget && laneIndexAt(panel_, widget) >= 0) {
        if (event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            widget->setFocus(Qt::MouseFocusReason);
            panel_->selectLane(panel_->laneGuid(laneIndexAt(panel_, widget)), true);
            return true;
        }
        if ((event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            return true;
        }
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_F2) {
            renameLane(panel_->laneGuid(laneIndexAt(panel_, widget)));
            return true;
        }
    }
    if (auto* name = qobject_cast<QLabel*>(watched);
        name && name->objectName() == QLatin1String("timelineLaneName")
        && event->type() == QEvent::Resize) {
        if (auto* editor = name->findChild<InlineRenameEdit*>())
            editor->setGeometry(name->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void TimelineHeaderColumn::wheelEvent(QWheelEvent* event)
{
    panel_->scrollTracks(event);
}

void TimelineHeaderColumn::renameLane(const QUuid& guid)
{
    const auto* track = ProjectManager::instance().findItemByGuid(guid);
    if (!track) return;
    for (auto* name : findChildren<QLabel*>(QStringLiteral("timelineLaneName"))) {
        const int lane = laneIndexAt(panel_, name);
        if (lane < 0 || panel_->laneGuid(lane) != guid) continue;
        auto* editor = name->findChild<InlineRenameEdit*>();
        if (!editor) {
            editor = new InlineRenameEdit(track->name, InlineRenameEdit::projectItemHandler(guid), name);
            connect(editor, &InlineRenameEdit::finished, editor, &QObject::deleteLater);
        }
        editor->setGeometry(name->rect());
        editor->begin(track->name);
        return;
    }
}

QMenu* TimelineHeaderColumn::createLaneMenu(int laneIndex)
{
    if (panel_->viewKind() != TimelineEditorPanel::ViewKind::Segment)
        return nullptr;
    ProjectItem* track = ProjectManager::instance().findItemByGuid(
        panel_->laneGuid(laneIndex));
    if (!track)
        return nullptr;

    auto* menu = new QMenu(this);
    QAction* rename = menu->addAction(tr("Rename Track"));
    connect(rename, &QAction::triggered, this, [this, guid = track->guid] {
        renameLane(guid);
    });
    menu->addSeparator();
    QAction* blueprint = menu->addAction(tr("Open in Blueprint"));
    blueprint->setObjectName(QStringLiteral("timelineLaneMenuBlueprint"));
    connect(blueprint, &QAction::triggered, this, [this, laneIndex] {
        panel_->requestBlueprintForLane(laneIndex);
    });

    return menu;
}

void TimelineHeaderColumn::contextMenuEvent(QContextMenuEvent* event)
{
    if (panel_->viewKind() == TimelineEditorPanel::ViewKind::Track) {
        showAmplitudeCurveMenu(this, event->globalPos(), QRect(mapToGlobal(rect().topLeft()), size()),
            panel_->amplitudeExponent(), [this](double exponent) { panel_->setAmplitudeExponent(exponent); });
        event->accept();
        return;
    }
    const int laneIndex = laneIndexAt(panel_, childAt(event->pos()));
    if (laneIndex < 0) {
        QWidget::contextMenuEvent(event);
        return;
    }
    QMenu* menu = createLaneMenu(laneIndex);
    if (!menu) {
        QWidget::contextMenuEvent(event);
        return;
    }
    menu->exec(event->globalPos());
    menu->deleteLater();
    event->accept();
}

}
