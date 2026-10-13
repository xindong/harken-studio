
#pragma once

#include "Core/ProjectTypes.h"
#include "Widgets/Shared/RefDomain.h"

#include <QColor>
#include <QIcon>
#include <QList>
#include <QMetaObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

class QObject;
class QTreeWidget;

namespace VNX {

struct SelectorRequest;

struct RefFilter
{
    ProjectItem::Type single = ProjectItem::Type::Bus;
    std::vector<ProjectItem::Type> multi;
    QUuid directChildParentGuid = {};
};

class IRefDomainAdapter
{
public:
    virtual ~IRefDomainAdapter() = default;

    virtual bool exists(uint32_t id) const = 0;
    virtual QString displayName(uint32_t id) const = 0;
    virtual QString tooltip(uint32_t id) const = 0;
    virtual QIcon icon(uint32_t id, const QColor& tint) const = 0;
    virtual bool matchesFilter(uint32_t id, const RefFilter& filter) const = 0;
    virtual QList<QMetaObject::Connection> connectChanged(
        QObject* ctx, std::function<void(uint32_t)> onChanged) const = 0;
    virtual void populate(QTreeWidget* tree, const SelectorRequest& request) const = 0;
};

const IRefDomainAdapter& refDomainAdapter(RefDomain domain);

}
