#include "MatrixTableWidget.h"
#include "ReorderHeaderView.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QHeaderView>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

class MatrixHeaderView : public ReorderHeaderView
{
public:
    MatrixHeaderView(ThemeManager* theme, MatrixTableWidget* matrix, QWidget* parent)
        : ReorderHeaderView(Qt::Horizontal, parent), theme_(theme), matrix_(matrix)
    {
        setSectionsClickable(false);
        setHighlightSections(false);
        connect(this, &QHeaderView::geometriesChanged, this, [this] { positionWidgets(); });
        connect(this, &QHeaderView::sectionResized, this, [this] { positionWidgets(); });
        connect(this, &QHeaderView::sectionMoved, this, [this] { positionWidgets(); });
        connect(this, &ReorderHeaderView::sectionPositionsChanged, this, [this] { positionWidgets(); });
    }

    void setWidget(int column, QWidget* widget)
    {
        Q_ASSERT(column >= 0 && column < count());
        if (auto* old = cells_.take(column)) {
            old->hide();
            old->deleteLater();
        }
        auto* cell = new QFrame(viewport());
        cell->setProperty("matrixHeader", true);
        cell->setProperty("selected", column == matrix_->selectedColumn());
        cell->setAttribute(Qt::WA_StyledBackground);
        cell->setCursor(Qt::ArrowCursor);
        auto* layout = new QVBoxLayout(cell);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        widget->setParent(cell);
        widget->setMinimumWidth(0);
        widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(widget);
        cell->setToolTip(widget->toolTip());
        cells_.insert(column, cell);
        applyTheme();
        cell->show();
    }

    QWidget* widget(int column) const
    {
        auto* cell = cells_.value(column);
        return cell ? cell->layout()->itemAt(0)->widget() : nullptr;
    }

    void clearWidgets()
    {
        cancelDrag();
        for (auto* cell : cells_) {
            cell->hide();
            cell->deleteLater();
        }
        cells_.clear();
    }

    void setWarning(int column, bool warning)
    {
        QtControlSkin::setSemanticProperty(cells_.value(column), "warning", warning);
    }

    void refreshSelection()
    {
        for (auto it = cells_.cbegin(); it != cells_.cend(); ++it)
            QtControlSkin::setSemanticProperty(it.value(), "selected", it.key() == matrix_->selectedColumn());
        viewport()->update();
    }

    void setFirstMovableColumn(int column)
    {
        firstMovableColumn_ = column;
        cancelDrag();
        positionWidgets();
    }

    int gripWidth() const
    {
        return std::max(theme_->metrics().table.resizeHandleRadius,
            style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this));
    }

    void applyTheme()
    {
        const auto& metrics = theme_->metrics().table;
        int height = std::max(metrics.headerHeight,
            QtControlSkin(theme_->snapshot()).fieldHeight() + metrics.cellPaddingY * 2 + 1);
        const int padding = std::max(0, metrics.cellPaddingX - gripWidth());
        for (auto* cell : cells_) {
            cell->layout()->setContentsMargins(padding, metrics.cellPaddingY, padding, metrics.cellPaddingY);
            cell->layout()->setSpacing(0);
            cell->ensurePolished();
            height = std::max(height, cell->sizeHint().height() + 1);
        }
        setFixedHeight(height);
        positionWidgets();
    }

    void positionWidgets()
    {
        const int grip = gripWidth();
        QFrame* dragged = nullptr;
        for (auto it = cells_.cbegin(); it != cells_.cend(); ++it) {
            const int handle = dragHandleWidth(it.key());
            const auto rect = sectionDisplayRect(it.key());
            it.value()->setGeometry(rect.x() + grip + handle, 0,
                std::max(0, rect.width() - grip * 2 - handle), height() - 1);
            if (isDraggedSection(it.key())) dragged = it.value();
        }
        if (dragged) dragged->raise();
    }

protected:
    bool isMovableSection(int logical) const override
    {
        return logical >= firstMovableColumn_ && count() > firstMovableColumn_ + 1
            && ReorderHeaderView::isMovableSection(logical);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        const auto position = event->position().toPoint();
        const int column = logicalIndexAt(position);
        const auto* cell = cells_.value(column);
        const auto rect = sectionDisplayRect(column);
        if (event->button() == Qt::LeftButton && cell
            && position.x() >= rect.left() + gripWidth()
            && position.x() < rect.right() - gripWidth()) {
            if (event->modifiers() == Qt::ControlModifier && matrix_->selectedColumn() == column) {
                matrix_->clearSelection();
                event->accept();
                return;
            }
            matrix_->setSelectedColumn(column);
            matrix_->table()->setFocus(Qt::MouseFocusReason);
        }
        if (event->button() == Qt::LeftButton && cell && cell->geometry().contains(position)) {
            event->accept();
            return;
        }
        ReorderHeaderView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() == Qt::NoButton)
            unsetCursor();
        ReorderHeaderView::mouseMoveEvent(event);
        const QPoint position = event->position().toPoint();
        const int column = logicalIndexAt(position);
        if (event->buttons() == Qt::NoButton && cursor().shape() != Qt::SplitHCursor
            && column >= 0 && dragHandleRect(column).contains(position))
            setCursor(Qt::SizeHorCursor);
    }

    void paintEvent(QPaintEvent* event) override
    {
        ReorderHeaderView::paintEvent(event);
        QPainter painter(viewport());
        const int selected = matrix_->selectedColumn();
        if (selected >= 0) {
            const auto rect = sectionDisplayRect(selected).adjusted(0, 0, -1, -1);
            painter.fillRect(rect, theme_->colors().bgSelected);
            painter.fillRect(QRect(rect.left(), rect.top(), 2, rect.height()), theme_->colors().accent);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme_->colors().textMuted);
        for (int column = firstMovableColumn_; column < count(); ++column) {
            const auto rect = dragHandleRect(column);
            if (rect.width() < 6) continue;
            const auto center = rect.center();
            for (int x : {-2, 2})
                for (int y : {-4, 0, 4})
                    painter.drawEllipse(QPointF(center.x() + x, center.y() + y), 1, 1);
        }
    }

private:
    int dragHandleWidth(int column) const
    {
        return column >= firstMovableColumn_ && ReorderHeaderView::isMovableSection(column)
            ? std::min(theme_->metrics().table.compactColumnWidth,
                       std::max(0, sectionSize(column) - gripWidth() * 2)) : 0;
    }

    QRect dragHandleRect(int column) const
    {
        const auto rect = sectionDisplayRect(column);
        return QRect(rect.x() + gripWidth(), 0, dragHandleWidth(column), height() - 1);
    }

    ThemeManager* theme_;
    MatrixTableWidget* matrix_;
    QMap<int, QFrame*> cells_;
    int firstMovableColumn_ = 0;
};

MatrixTableWidget::MatrixTableWidget(QWidget* parent, ThemeManager* themeManager)
    : TableWidget({TableKind::Config, TableBodySizing::Expanding}, parent, themeManager)
    , themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
    setProperty("matrixTable", true);
    table_ = createCustomView(QStringLiteral("matrixTable"),
        [this](QWidget* parent, ThemeManager* theme) {
            auto* table = new QTableWidget(parent);
            table->setHorizontalHeader(new MatrixHeaderView(theme, this, table));
            return table;
        });
    header_ = static_cast<MatrixHeaderView*>(table_->horizontalHeader());
    table_->setShowGrid(true);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (table_->selectionModel()->hasSelection()) setSelectedColumn(-1);
    });
    connect(header_, &QHeaderView::sectionMoved, this, [this](int, int from, int to) {
        {
            const QSignalBlocker blocker(header_);
            header_->moveSection(to, from);
        }
        header_->positionWidgets();
        if (from >= firstMovableColumn_ && to >= firstMovableColumn_)
            emit columnMoveRequested(from, to);
    });
    applyTheme();
}

void MatrixTableWidget::setHeaderWidget(int column, QWidget* widget)
{
    header_->setWidget(column, widget);
}

QWidget* MatrixTableWidget::headerWidget(int column) const
{
    return header_->widget(column);
}

void MatrixTableWidget::clearHeaderWidgets()
{
    setSelectedColumn(-1);
    header_->clearWidgets();
    applyTheme();
}

void MatrixTableWidget::setHeaderWarning(int column, bool warning)
{
    header_->setWarning(column, warning);
}

void MatrixTableWidget::setColumnsReorderable(bool enabled, int firstMovableColumn)
{
    firstMovableColumn_ = firstMovableColumn;
    setColumnsMovable(enabled);
    header_->setFirstSectionMovable(firstMovableColumn == 0);
    header_->setFirstMovableColumn(firstMovableColumn);
}

void MatrixTableWidget::setSelectedColumn(int column)
{
    if (!header_->widget(column)) column = -1;
    if (selectedColumn_ == column) return;
    if (column >= 0) TableWidget::clearSelection();
    selectedColumn_ = column;
    header_->refreshSelection();
    emit columnSelectionChanged(column);
}

void MatrixTableWidget::clearSelection()
{
    TableWidget::clearSelection();
    setSelectedColumn(-1);
}

bool MatrixTableWidget::hasSelection() const
{
    return selectedColumn_ >= 0 || TableWidget::hasSelection();
}

void MatrixTableWidget::applyTheme()
{
    TableWidget::applyTheme();
    if (!header_) return;
    header_->applyTheme();
    const auto& metrics = themeManager_->metrics().table;
    table_->verticalHeader()->setDefaultSectionSize(std::max(metrics.rowHeight,
        QtControlSkin(themeManager_->snapshot()).fieldHeight() + metrics.cellPaddingY * 2 + 1));
}

}
