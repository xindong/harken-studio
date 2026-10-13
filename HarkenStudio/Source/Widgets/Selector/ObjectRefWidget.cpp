
#include "ObjectRefWidget.h"
#include "ObjectRefNavigation.h"
#include "Widgets/Common/ElidedLabel.h"

#include "RefDomainAdapter.h"
#include "SelectorWindow.h"
#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Widgets/Shared/RefIdMime.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QStyle>
#include <QToolButton>

#include <utility>
#include <algorithm>

namespace VNX {

ObjectRefWidget::ObjectRefWidget(QWidget* parent)
    : QWidget(parent)
{
    auto& theme = ThemeManager::instance();
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAcceptDrops(true);
    setAttribute(Qt::WA_StyledBackground, true);

    auto* h = new QHBoxLayout(this);
    h->setSizeConstraint(QLayout::SetNoConstraint);
    h->setContentsMargins(7, 0, 4, 0);
    h->setSpacing(6);

    iconLabel_ = new QLabel(this);
    iconLabel_->setAlignment(Qt::AlignCenter);
    iconLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    h->addWidget(iconLabel_);

    nameLabel_ = new ElidedLabel(this);
    nameLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameLabel_->setObjectName(QStringLiteral("refName"));
    nameLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    h->addWidget(nameLabel_, 1);

    pickButton_ = new QToolButton(this);
    pickButton_->setObjectName(QStringLiteral("refPick"));
    pickButton_->setIcon(theme.resolveIcon(
        QString::fromLatin1(ThemeAssetIds::SelectorTarget)).icon);
    pickButton_->setIconSize(QSize(20, 20));
    pickButton_->setAutoRaise(true);
    pickButton_->setFixedSize(20, 20);
    pickButton_->setProperty("flatIconButton", true);
    pickButton_->setCursor(Qt::PointingHandCursor);
    h->addWidget(pickButton_);

    connect(pickButton_, &QToolButton::clicked, this, [this] { openSelector(); });
    connect(&theme, &ThemeManager::themeChanged, this,
            [this]() { refreshDisplay(); });
    bindChanged();

    refreshDisplay();
}

ObjectRefWidget::~ObjectRefWidget()
{
    cancelPendingSelection();
}

void ObjectRefWidget::bindChanged()
{
    for (const QMetaObject::Connection& conn : changedConns_)
        QObject::disconnect(conn);
    changedConns_ = refDomainAdapter(domain_).connectChanged(this, [this](uint32_t id) {
        if (id == 0 || id == objectID_)
            refreshDisplay();
    });
}

void ObjectRefWidget::setRefDomain(RefDomain domain)
{
    if (domain_ == domain)
        return;
    cancelPendingSelection();
    domain_ = domain;
    mixedValue_ = false;
    objectID_ = 0;
    unresolvedReference_.clear();
    validationError_.clear();
    bindChanged();
    refreshDisplay();
}

void ObjectRefWidget::setSelectorEnabled(bool enabled)
{
    if (!enabled)
        cancelPendingSelection();
    selectorEnabled_ = enabled;
    updatePickVisibility();
    refreshDisplay();
}

void ObjectRefWidget::setReadOnly(bool readOnly)
{
    if (readOnly)
        cancelPendingSelection();
    readOnly_ = readOnly;
    updatePickVisibility();
    refreshDisplay();
}

void ObjectRefWidget::setRequired(bool required)
{
    if (required_ == required)
        return;
    required_ = required;
    refreshDisplay();
}

void ObjectRefWidget::setValidationError(const QString& error)
{
    if (validationError_ == error)
        return;
    validationError_ = error;
    refreshDisplay();
}

void ObjectRefWidget::setSupplementalToolTip(const QString& text)
{
    if (supplementalToolTip_ == text)
        return;
    supplementalToolTip_ = text;
    refreshToolTip();
}

void ObjectRefWidget::setFilterType(ProjectItem::Type type)
{
    if (filterType_ == type)
        return;
    cancelPendingSelection();
    filterType_ = type;
}

void ObjectRefWidget::setFilterTypes(std::vector<ProjectItem::Type> types)
{
    if (filterTypes_ == types)
        return;
    cancelPendingSelection();
    filterTypes_ = std::move(types);
}

void ObjectRefWidget::setDirectChildParentGuid(const QUuid& guid)
{
    if (directChildParentGuid_ == guid)
        return;
    cancelPendingSelection();
    directChildParentGuid_ = guid;
}

void ObjectRefWidget::setIsSelectable(std::function<bool(uint32_t)> predicate)
{
    cancelPendingSelection();
    isSelectable_ = std::move(predicate);
}

void ObjectRefWidget::setEditGuard(std::function<bool()> guard)
{
    cancelPendingSelection();
    editGuard_ = std::move(guard);
}

void ObjectRefWidget::cancelPendingSelection()
{
    ++selectionGeneration_;
    SelectorWindow::dismiss(this);
    setDropActive(false);
}

bool ObjectRefWidget::canEdit() const
{
    return isEnabled() && !readOnly_ && selectorEnabled_ && (!editGuard_ || editGuard_());
}

void ObjectRefWidget::setCompact(bool compact)
{
    compact_ = compact;
    auto* h = static_cast<QHBoxLayout*>(layout());
    h->setSpacing(compact ? 5 : 6);
    updatePickVisibility();
    refreshDisplay();
}

void ObjectRefWidget::setCompactPickable(bool v)
{
    compactPickable_ = v;
    updatePickVisibility();
    refreshDisplay();
}

void ObjectRefWidget::updatePickVisibility()
{
    pickButton_->setVisible((!compact_ || compactPickable_) && !readOnly_ && selectorEnabled_);
}

bool ObjectRefWidget::event(QEvent* e)
{
    const bool handled = QWidget::event(e);
    if (iconLabel_ && nameLabel_ && pickButton_) {
        if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange)
            refreshMetrics();
        else if (e->type() == QEvent::DevicePixelRatioChange)
            refreshIcon();
        else if (e->type() == QEvent::Resize || e->type() == QEvent::LayoutRequest)
            refreshLayout();
        else if (e->type() == QEvent::ToolTipChange)
            nameLabel_->setToolTip(toolTip());
    }
    return handled;
}

void ObjectRefWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && hasItem_ && objectID_ != 0)
        ObjectRefNavigation::locate(this, domain_, objectID_);
    QWidget::mousePressEvent(e);
}

void ObjectRefWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && hasItem_ && objectID_ != 0) {
        const uint32_t id = objectID_;
        const RefDomain domain = domain_;
        QMetaObject::invokeMethod(this, [this, id, domain] {
            if (domain_ == domain && objectID_ == id && refDomainAdapter(domain).exists(id))
                ObjectRefNavigation::navigate(this, domain, id);
        }, Qt::QueuedConnection);
    }
    QWidget::mouseDoubleClickEvent(e);
}

void ObjectRefWidget::enterEvent(QEnterEvent* e)
{
    setHovered(true);
    QWidget::enterEvent(e);
}

void ObjectRefWidget::leaveEvent(QEvent* e)
{
    setHovered(false);
    QWidget::leaveEvent(e);
}

void ObjectRefWidget::setHovered(bool hovered)
{
    QtControlSkin::setSemanticProperty(this, "hovered", hovered);
}

void ObjectRefWidget::setCurrentObjectID(uint32_t objectID)
{
    if (mixedValue_ || !unresolvedReference_.isEmpty() || objectID_ != objectID)
        cancelPendingSelection();
    mixedValue_ = false;
    unresolvedReference_.clear();
    validationError_.clear();
    objectID_ = objectID;
    refreshDisplay();
}

void ObjectRefWidget::setUnresolvedReference(QString stableId)
{
    if (mixedValue_ || objectID_ != 0 || unresolvedReference_ != stableId)
        cancelPendingSelection();
    mixedValue_ = false;
    objectID_ = 0;
    validationError_.clear();
    unresolvedReference_ = std::move(stableId);
    refreshDisplay();
}

void ObjectRefWidget::setMixedValue()
{
    if (!mixedValue_)
        cancelPendingSelection();
    objectID_ = 0;
    unresolvedReference_.clear();
    validationError_.clear();
    mixedValue_ = true;
    refreshDisplay();
}

void ObjectRefWidget::setInherited(bool inherited, const QString& tooltip)
{
    inheritedTooltip_ = tooltip;
    if (inherited_ == inherited) {
        if (inherited_) refreshDisplay();
        return;
    }
    cancelPendingSelection();
    inherited_ = inherited;
    refreshDisplay();
}

void ObjectRefWidget::refreshDisplay()
{
    refreshMetrics();
    const IRefDomainAdapter& adapter = refDomainAdapter(domain_);
    hasItem_ = adapter.exists(objectID_);
    if (hasItem_)
        nameLabel_->setText(adapter.displayName(objectID_));
    else if (mixedValue_)
        nameLabel_->setText(tr("Mixed"));
    else if (!unresolvedReference_.isEmpty() || objectID_ != 0)
        nameLabel_->setText(tr("Missing"));
    else
        nameLabel_->clear();
    refreshToolTip();
    pickButton_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::SelectorTarget)).icon);
    refreshIcon();

    setCursor(hasItem_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    refreshStyle();
}

void ObjectRefWidget::refreshToolTip()
{
    QString tip;
    if (hasItem_) {
        const QString target = refDomainAdapter(domain_).tooltip(objectID_);
        tip = target;
        if (inherited_)
            tip = inheritedTooltip_.isEmpty()
                ? tr("Inherited routing: %1\nSelect a bus to override.").arg(target)
                : inheritedTooltip_.arg(target);
    } else if (!mixedValue_) {
        if (!unresolvedReference_.isEmpty() || objectID_ != 0)
            tip = unresolvedReference_.isEmpty()
                ? tr("The referenced object no longer exists.")
                : tr("Missing: %1").arg(unresolvedReference_);
        else if (required_ && !inherited_)
            tip = tr("No object selected.");
    }
    if (!validationError_.isEmpty())
        tip = validationError_;
    if (!supplementalToolTip_.isEmpty())
        tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + supplementalToolTip_;
    setToolTip(tip);
}

void ObjectRefWidget::refreshMetrics()
{
    const int extent = ThemeManager::instance().metrics().objectIconExtent;
    iconLabel_->setFixedSize(extent, extent);
    refreshLayout();
}

QSize ObjectRefWidget::minimumSizeHint() const
{
    return QSize(0, QWidget::minimumSizeHint().height());
}

void ObjectRefWidget::refreshLayout()
{
    auto* h = static_cast<QHBoxLayout*>(layout());
    const int left = compact_ ? 5 : 7;
    const int right = compact_ ? 7 : 4;
    const int buttonWidth = pickButton_->isHidden() ? 0 : pickButton_->width();
    const int padding = std::clamp(contentsRect().width() - buttonWidth, 0, left + right);
    const int leftPadding = std::min(left, padding);
    h->setContentsMargins(leftPadding, 0, padding - leftPadding, 0);
    const int available = contentsRect().width() - padding;
    iconLabel_->setVisible(available >= iconLabel_->width()
        + (buttonWidth > 0 ? buttonWidth + 2 * h->spacing() : 0));
    nameLabel_->setVisible(available >= (buttonWidth > 0 ? buttonWidth + h->spacing() : 0));
}

void ObjectRefWidget::refreshIcon()
{
    if (!hasItem_) {
        iconLabel_->clear();
        return;
    }
    const bool dim = readOnly_ || !selectorEnabled_;
    const QIcon icon = refDomainAdapter(domain_).icon(objectID_, QColor());
    iconLabel_->setPixmap(icon.pixmap(iconLabel_->size(), devicePixelRatioF(),
                                     dim ? QIcon::Disabled : QIcon::Normal));
}

void ObjectRefWidget::refreshStyle()
{
    const bool dim = readOnly_ || !selectorEnabled_;
    const bool invalid = !mixedValue_ && (!validationError_.isEmpty()
        || (!hasItem_ && (objectID_ != 0 || !unresolvedReference_.isEmpty()
                         || (required_ && !inherited_))));
    QtControlSkin::setSemanticProperty(this, "inputInvalid", invalid);
    QtControlSkin::setSemanticProperty(this, "dimmed", dim);
    QtControlSkin::setSemanticProperty(nameLabel_, "dimmed", dim);
    QtControlSkin::setSemanticProperty(nameLabel_, "inherited", inherited_);
}

void ObjectRefWidget::openSelector(QWidget* anchor)
{
    if (!canEdit())
        return;
    cancelPendingSelection();
    SelectorRequest req;
    req.title = selectorTitle_;
    req.domain = domain_;
    req.filterType = filterType_;
    req.filterTypes = filterTypes_;
    req.directChildParentGuid = directChildParentGuid_;
    req.currentObjectID = inherited_ ? 0 : objectID_;
    req.isSelectable = isSelectable_;
    QPointer<ObjectRefWidget> self(this);
    req.onSelected = [self, generation = selectionGeneration_](uint32_t id) {
        if (self && self->selectionGeneration_ == generation)
            self->applySelection(id);
    };
    SelectorWindow::open(req, anchor ? anchor : this);
}

void ObjectRefWidget::applySelection(uint32_t objectID)
{
    if (!canAcceptObject(objectID))
        return;
    inherited_ = false;
    setCurrentObjectID(objectID);
    emit objectChanged(objectID);
}

bool ObjectRefWidget::canAcceptObject(uint32_t objectID) const
{
    if (!canEdit() || objectID == 0)
        return false;
    const IRefDomainAdapter& adapter = refDomainAdapter(domain_);
    return adapter.exists(objectID)
        && adapter.matchesFilter(objectID, {filterType_, filterTypes_, directChildParentGuid_})
        && (!isSelectable_ || isSelectable_(objectID));
}

bool ObjectRefWidget::canAcceptMime(const QMimeData* mime) const
{
    return canAcceptObject(RefIdMime::readSingle(mime, domain_));
}

void ObjectRefWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (!canAcceptMime(e->mimeData())) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    setDropActive(true);
}

void ObjectRefWidget::dragMoveEvent(QDragMoveEvent* e)
{
    if (!canAcceptMime(e->mimeData())) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
}

void ObjectRefWidget::dragLeaveEvent(QDragLeaveEvent* e)
{
    setDropActive(false);
    QWidget::dragLeaveEvent(e);
}

void ObjectRefWidget::dropEvent(QDropEvent* e)
{
    setDropActive(false);
    if (!canAcceptMime(e->mimeData())) {
        e->ignore();
        return;
    }
    const uint32_t id = RefIdMime::readSingle(e->mimeData(), domain_);
    e->setDropAction(Qt::CopyAction);
    e->accept();
    if (inherited_ || id != objectID_)
        applySelection(id);
}

void ObjectRefWidget::setDropActive(bool active)
{
    if (dropActive_ == active)
        return;
    dropActive_ = active;
    setProperty("dropActive", active);
    style()->unpolish(this);
    style()->polish(this);
}

void ObjectRefWidget::contextMenuEvent(QContextMenuEvent* e)
{
    const bool canReset = inheritSupport_ && !inherited_;
    const bool canClear = clearSupport_ && !inherited_
        && (objectID_ != 0 || !unresolvedReference_.isEmpty() || mixedValue_);
    if (!canEdit() || (!canReset && !canClear)) {
        QWidget::contextMenuEvent(e);
        return;
    }
    QPointer<ObjectRefWidget> self(this);
    QMenu menu;
    QAction* reset = canReset ? menu.addAction(tr("Reset to Inherited")) : nullptr;
    QAction* clear = canClear ? menu.addAction(tr("Clear Reference")) : nullptr;
    const uint64_t generation = selectionGeneration_;
    QAction* chosen = menu.exec(e->globalPos());
    if (self && chosen && (chosen == reset || chosen == clear)
        && self->selectionGeneration_ == generation && self->canEdit())
        emit self->objectChanged(0);
}

}
