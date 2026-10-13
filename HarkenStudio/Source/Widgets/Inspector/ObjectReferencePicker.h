
#pragma once

#include "Core/ProjectTypes.h"

#include <QString>
#include <QWidget>
#include <cstdint>

namespace VNX {

class ObjectRefWidget;

class IDeclarativeCustomWidget
{
public:
    virtual ~IDeclarativeCustomWidget() = default;
    virtual void bindProjectItem(ProjectItem* item) = 0;
};

class ObjectReferencePicker : public QWidget, public IDeclarativeCustomWidget
{
    Q_OBJECT
public:
    explicit ObjectReferencePicker(QWidget* parent = nullptr);
    ~ObjectReferencePicker() override;

    void setFilterType(ProjectItem::Type t);
    ProjectItem::Type filterType() const noexcept;

    void setCurrentObjectID(uint32_t objectID);
    uint32_t currentObjectID() const noexcept;

    void bindProjectItem(ProjectItem* item) override;

    void setActionFieldName(const QString& field) noexcept { actionFieldName_ = field; }
    void setActionId(const QString& actionId)      noexcept { actionId_       = actionId; }
    QString actionFieldName() const noexcept { return actionFieldName_; }
    QString actionId()        const noexcept { return actionId_; }

    static ProjectItem::Type parseTypeString(const QString& s) noexcept;

signals:
    void objectChanged(uint32_t newObjectID);

private:
    void onObjectPicked(uint32_t id);

    ObjectRefWidget* refWidget_ = nullptr;

    QString  actionFieldName_;
    QString  actionId_;
    uint32_t boundObjectID_ = 0;
};

}
