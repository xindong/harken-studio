#include "ToolTip.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneHelpEvent>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QTabBar>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>

#include <algorithm>

namespace VNX {

namespace {

class ToolTipLabel final : public QLabel
{
public:
    ToolTipLabel()
        : QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
            | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    {
        setObjectName(QStringLiteral("studioToolTip"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_QuitOnClose, false);
        setFocusPolicy(Qt::NoFocus);
        setTextInteractionFlags(Qt::NoTextInteraction);
        setForegroundRole(QPalette::ToolTipText);
        setBackgroundRole(QPalette::ToolTipBase);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        setIndent(0);
        refreshAppearance();
        winId();
    }

    void refreshAppearance()
    {
        QPalette colors = QToolTip::palette();
        colors.setCurrentColorGroup(QPalette::Inactive);
        setPalette(colors);
        setFont(QToolTip::font());
        setMargin(1 + style()->pixelMetric(QStyle::PM_ToolTipLabelFrameWidth, nullptr, this));
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::ToolTipBase));
        painter.setPen(palette().color(QPalette::ToolTipText));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        painter.end();
        QLabel::paintEvent(event);
    }
};

class ToolTipController final : public QObject
{
public:
    explicit ToolTipController(QApplication* application)
        : QObject(application), label_(new ToolTipLabel)
    {
        expireTimer_.setSingleShot(true);
        connect(&expireTimer_, &QTimer::timeout, this, [this] { hide(); });
        application->installEventFilter(this);
    }

    ~ToolTipController() override
    {
        delete label_.data();
    }

    void show(const QPoint& position, const QString& text, QWidget* owner,
              const QRect& rect, int duration)
    {
        if (text.isEmpty()) {
            hide();
            return;
        }
        const QRect ownerRect = owner && rect.isNull() ? owner->rect() : rect;
        if (isVisible() && owner_ == owner && ownerRect_ == ownerRect && label_->text() == text)
            return;
        disconnect(ownerDestroyed_);
        owner_ = owner;
        ownerRect_ = ownerRect;
        if (owner)
            ownerDestroyed_ = connect(owner, &QObject::destroyed, this, [this] { hide(); });
        label_->refreshAppearance();
        label_->setLayoutDirection(owner ? owner->layoutDirection() : QApplication::layoutDirection());
        QScreen* screen = QGuiApplication::screenAt(position);
        if (!screen)
            screen = owner ? owner->screen() : QGuiApplication::primaryScreen();
        const QRect available = screen->availableGeometry();
        label_->setMaximumSize(available.size());
        label_->setWordWrap(Qt::mightBeRichText(text));
        label_->setText(text);
        if (label_->sizeHint().width() > available.width())
            label_->setWordWrap(true);
        QSize size = label_->sizeHint().boundedTo(available.size());
        if (label_->hasHeightForWidth())
            size.setHeight(std::min(available.height(), label_->heightForWidth(size.width())));
        label_->resize(size);
        QPoint topLeft = position + QPoint(2, 16);
        if (topLeft.x() + size.width() > available.right() + 1)
            topLeft.setX(position.x() - size.width() - 4);
        if (topLeft.y() + size.height() > available.bottom() + 1)
            topLeft.setY(position.y() - size.height() - 4);
        topLeft.setX(std::clamp(topLeft.x(), available.left(), available.right() - size.width() + 1));
        topLeft.setY(std::clamp(topLeft.y(), available.top(), available.bottom() - size.height() + 1));
        label_->move(topLeft);
        label_->show();
        label_->raise();
        expireTimer_.start(duration > 0 ? duration
            : 10000 + 40 * std::max(0, static_cast<int>(text.size()) - 100));
    }

    void hide()
    {
        expireTimer_.stop();
        disconnect(ownerDestroyed_);
        ownerDestroyed_ = {};
        owner_.clear();
        ownerRect_ = {};
        if (label_)
            label_->hide();
    }

    bool isVisible() const
    {
        return label_ && label_->isVisible();
    }

    QString text() const
    {
        return isVisible() ? label_->text() : QString();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == label_)
            return false;
        if (event->type() == QEvent::ToolTip) {
            if (auto* widget = qobject_cast<QWidget*>(watched))
                return showWidgetTip(widget, static_cast<QHelpEvent*>(event));
        } else if (event->type() == QEvent::GraphicsSceneHelp) {
            if (auto* scene = qobject_cast<QGraphicsScene*>(watched))
                return showGraphicsTip(scene, static_cast<QGraphicsSceneHelpEvent*>(event));
        }
        if (!isVisible())
            return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::TouchBegin:
        case QEvent::WindowDeactivate:
            hide();
            break;
        case QEvent::ApplicationStateChange:
            if (QGuiApplication::applicationState() != Qt::ApplicationActive)
                hide();
            break;
        case QEvent::Hide:
        case QEvent::Close:
            if (auto* widget = qobject_cast<QWidget*>(watched);
                owner_ && widget && (widget == owner_ || widget->isAncestorOf(owner_)))
                hide();
            break;
        case QEvent::MouseMove:
            if (owner_ && !ownerRect_.contains(owner_->mapFromGlobal(
                    static_cast<QMouseEvent*>(event)->globalPosition().toPoint())))
                hide();
            break;
        case QEvent::Leave:
            if (watched == owner_ && !ownerRect_.contains(owner_->mapFromGlobal(QCursor::pos())))
                hide();
            break;
        case QEvent::ApplicationPaletteChange:
        case QEvent::ApplicationFontChange:
            label_->refreshAppearance();
            break;
        default:
            break;
        }
        return false;
    }

private:
    bool showWidgetTip(QWidget* widget, QHelpEvent* event)
    {
        QString tip;
        QWidget* owner = widget;
        QRect area = widget->rect();
        auto* view = qobject_cast<QAbstractItemView*>(widget->parentWidget());
        auto* header = qobject_cast<QHeaderView*>(widget);
        if (!header && view && view->viewport() == widget)
            header = qobject_cast<QHeaderView*>(view);
        if (header && header->model()) {
            owner = header->viewport();
            const QPoint position = owner->mapFromGlobal(event->globalPos());
            const int section = header->logicalIndexAt(position);
            if (section >= 0) {
                tip = header->model()->headerData(section, header->orientation(), Qt::ToolTipRole).toString();
                area = header->orientation() == Qt::Horizontal
                    ? QRect(header->sectionViewportPosition(section), 0, header->sectionSize(section), owner->height())
                    : QRect(0, header->sectionViewportPosition(section), owner->width(), header->sectionSize(section));
            }
        } else if (view && view->viewport() == widget) {
            const QModelIndex index = view->indexAt(event->pos());
            tip = index.data(Qt::ToolTipRole).toString();
            area = view->visualRect(index);
        } else if (auto* tabs = qobject_cast<QTabBar*>(widget)) {
            const int tab = tabs->tabAt(event->pos());
            if (tab >= 0) {
                tip = tabs->tabToolTip(tab);
                area = tabs->tabRect(tab);
            }
        } else if (auto* menu = qobject_cast<QMenu*>(widget); menu && menu->toolTipsVisible()) {
            if (auto* action = menu->actionAt(event->pos())) {
                tip = action->toolTip();
                area = menu->actionGeometry(action);
            }
        }
        if (tip.isEmpty()) {
            tip = widget->toolTip();
            owner = widget;
            area = widget->rect();
        }
        if (tip.isEmpty())
            return false;
        show(event->globalPos(), tip, owner, area, owner->toolTipDuration());
        event->accept();
        return true;
    }

    bool showGraphicsTip(QGraphicsScene* scene, QGraphicsSceneHelpEvent* event)
    {
        QWidget* viewport = event->widget();
        auto* view = viewport ? qobject_cast<QGraphicsView*>(viewport->parentWidget()) : nullptr;
        const QTransform transform = view ? view->viewportTransform() : QTransform();
        const auto items = scene->items(event->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, transform);
        for (auto* item : items) {
            const QString tip = item->toolTip();
            if (tip.isEmpty())
                continue;
            const QRect area = view ? view->mapFromScene(item->sceneBoundingRect()).boundingRect() : QRect();
            show(event->screenPos(), tip, viewport, area, viewport ? viewport->toolTipDuration() : -1);
            event->accept();
            return true;
        }
        return false;
    }

    QPointer<ToolTipLabel> label_;
    QPointer<QWidget> owner_;
    QRect ownerRect_;
    QMetaObject::Connection ownerDestroyed_;
    QTimer expireTimer_;
};

ToolTipController* controller(bool create)
{
    static QPointer<ToolTipController> instance;
    if (!instance && create && qApp)
        instance = new ToolTipController(qApp);
    return instance;
}

}

void ToolTip::initialize()
{
    controller(true);
}

void ToolTip::showText(const QPoint& position, const QString& text, QWidget* owner,
                       const QRect& rect, int duration)
{
    if (auto* service = controller(true))
        service->show(position, text, owner, rect, duration);
}

void ToolTip::hideText()
{
    if (auto* service = controller(false))
        service->hide();
}

QString ToolTip::text()
{
    const auto* service = controller(false);
    return service ? service->text() : QString();
}

bool ToolTip::isVisible()
{
    const auto* service = controller(false);
    return service && service->isVisible();
}

}
