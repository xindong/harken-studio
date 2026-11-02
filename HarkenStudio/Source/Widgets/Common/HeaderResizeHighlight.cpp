#include "HeaderResizeHighlight.h"

#include "Core/ThemeManager.h"

#include <QEnterEvent>
#include <QHeaderView>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

namespace VNX {

HeaderResizeHighlight::HeaderResizeHighlight(QHeaderView* header, ThemeManager* theme)
    : QWidget(header->viewport()), header_(header), theme_(theme)
{
    setObjectName(QStringLiteral("tableHeaderResizeHighlight"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
    header_->viewport()->setMouseTracking(true);
    header_->viewport()->setAttribute(Qt::WA_Hover);
    header_->viewport()->installEventFilter(this);
    header_->installEventFilter(this);
    connect(header_, &QHeaderView::sectionResized, this, [this] { refresh(); });
    connect(header_, &QHeaderView::sectionMoved, this, [this] { refresh(); });
    connect(header_, &QHeaderView::sectionCountChanged, this, [this] { refresh(); });
    connect(header_, &QHeaderView::geometriesChanged, this, [this] { refresh(); });
    connect(theme_, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

bool HeaderResizeHighlight::canResize(int column) const
{
    if (column < 0 || column >= header_->count() || header_->isSectionHidden(column)
        || header_->sectionResizeMode(column) != QHeaderView::Interactive)
        return false;
    if (header_->stretchLastSection()) {
        for (int visual = header_->visualIndex(column) + 1; visual < header_->count(); ++visual)
            if (!header_->isSectionHidden(header_->logicalIndex(visual))) return true;
        return false;
    }
    return true;
}

int HeaderResizeHighlight::columnAt(int x) const
{
    const int grip = header_->style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, header_);
    for (int visual = 0; visual < header_->count(); ++visual) {
        const int column = header_->logicalIndex(visual);
        if (!canResize(column)) continue;
        const int edge = header_->sectionViewportPosition(column)
            + (header_->isRightToLeft() ? 0 : header_->sectionSize(column));
        if (qAbs(x - edge) < grip) return column;
    }
    return -1;
}

void HeaderResizeHighlight::refresh()
{
    const int column = resizingColumn_ >= 0 ? resizingColumn_
        : inside_ && !pressed_ ? columnAt(position_.x()) : -1;
    if (!header_->isEnabled() || header_->orientation() != Qt::Horizontal || !canResize(column)) {
        hide();
        return;
    }
    const int edge = header_->sectionViewportPosition(column)
        + (header_->isRightToLeft() ? 0 : header_->sectionSize(column));
    if (edge <= 0 || edge > header_->viewport()->width()) {
        hide();
        return;
    }
    setGeometry(edge - 1, 0, 1, header_->viewport()->height());
    raise();
    show();
}

bool HeaderResizeHighlight::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == header_->viewport()) {
        switch (event->type()) {
        case QEvent::Paint:
            refresh();
            break;
        case QEvent::Enter:
            position_ = static_cast<QEnterEvent*>(event)->position().toPoint();
            inside_ = true;
            refresh();
            break;
        case QEvent::Leave:
            inside_ = false;
            refresh();
            break;
        case QEvent::HoverMove:
            position_ = static_cast<QHoverEvent*>(event)->position().toPoint();
            inside_ = header_->viewport()->rect().contains(position_);
            refresh();
            break;
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            position_ = mouse->position().toPoint();
            inside_ = header_->viewport()->rect().contains(position_);
            pressed_ = mouse->buttons() != Qt::NoButton;
            if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
                resizingColumn_ = columnAt(position_.x());
            else if (!pressed_)
                resizingColumn_ = -1;
            refresh();
            break;
        }
        default:
            break;
        }
    }
    if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse
        || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::EnabledChange) {
        resizingColumn_ = -1;
        pressed_ = false;
        if (event->type() != QEvent::EnabledChange) inside_ = false;
        refresh();
    }
    return QWidget::eventFilter(watched, event);
}

void HeaderResizeHighlight::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), theme_->colors().accent);
}

}
