
#pragma once

#include "Core/ProjectTypes.h"
#include "Widgets/Shared/RefDomain.h"

#include <QList>
#include <QMetaObject>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <vector>

class QLabel;
class QMimeData;
class QToolButton;

namespace VNX {

class ObjectRefWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ObjectRefWidget(QWidget* parent = nullptr);
    ~ObjectRefWidget() override;

    void setRefDomain(RefDomain domain);
    RefDomain refDomain() const noexcept { return domain_; }

    void setFilterType(ProjectItem::Type t);
    ProjectItem::Type filterType() const noexcept { return filterType_; }
    void setFilterTypes(std::vector<ProjectItem::Type> types);
    void setDirectChildParentGuid(const QUuid& guid);

    void setSelectorTitle(const QString& title) { selectorTitle_ = title; }
    void setSelectorEnabled(bool enabled);
    void setReadOnly(bool readOnly);
    void setRequired(bool required);
    void setValidationError(const QString& error);
    void setSupplementalToolTip(const QString& text);
    bool isReadOnly() const noexcept { return readOnly_; }
    QSize minimumSizeHint() const override;
    void setCompact(bool compact);
    void setCompactPickable(bool v);
    void setIsSelectable(std::function<bool(uint32_t)> pred);
    void setEditGuard(std::function<bool()> guard);
    void cancelPendingSelection();
    void openSelector(QWidget* anchor = nullptr);

    void setCurrentObjectID(uint32_t objectID);
    void setUnresolvedReference(QString stableId);
    void setMixedValue();
    uint32_t currentObjectID() const noexcept { return objectID_; }

    bool canAcceptObject(uint32_t objectID) const;

    void setInheritSupport(bool supported) { inheritSupport_ = supported; }
    void setInherited(bool inherited, const QString& tooltip = {});
    bool isInherited() const noexcept { return inherited_; }

    void setClearSupport(bool supported) { clearSupport_ = supported; }

signals:
    void objectChanged(uint32_t newObjectID);

protected:
    bool event(QEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void bindChanged();
    void applySelection(uint32_t objectID);
    bool canEdit() const;
    bool canAcceptMime(const QMimeData* mime) const;
    void refreshDisplay();
    void refreshToolTip();
    void refreshMetrics();
    void refreshLayout();
    void refreshIcon();
    void refreshStyle();
    void updatePickVisibility();
    void setHovered(bool hovered);
    void setDropActive(bool active);

    RefDomain domain_ = RefDomain::ProjectObject;
    ProjectItem::Type filterType_ = ProjectItem::Type::Bus;
    std::vector<ProjectItem::Type> filterTypes_;
    QUuid directChildParentGuid_;
    QString selectorTitle_;
    QString unresolvedReference_;
    QString validationError_;
    QString supplementalToolTip_;
    bool mixedValue_ = false;
    uint32_t objectID_ = 0;
    bool readOnly_ = false;
    bool required_ = true;
    bool selectorEnabled_ = true;
    bool compact_ = false;
    bool compactPickable_ = false;
    bool dropActive_ = false;
    bool hasItem_ = false;
    bool inherited_ = false;
    bool inheritSupport_ = false;
    bool clearSupport_ = false;
    QString inheritedTooltip_;
    std::function<bool(uint32_t)> isSelectable_;
    std::function<bool()> editGuard_;
    uint64_t selectionGeneration_ = 0;
    QList<QMetaObject::Connection> changedConns_;
    QLabel* iconLabel_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QToolButton* pickButton_ = nullptr;
};

}
