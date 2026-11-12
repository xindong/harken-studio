
#pragma once

#include "Core/ProjectTypes.h"
#include "Widgets/Shared/RefDomain.h"

#include <QColor>
#include <QIcon>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    virtual bool isAvailable(uint32_t id) const { return exists(id); }
    virtual QString displayName(uint32_t id) const = 0;
    virtual QString tooltip(uint32_t id) const = 0;
    virtual QIcon icon(uint32_t id, const QColor& tint) const = 0;
    virtual bool matchesFilter(uint32_t id, const RefFilter& filter) const = 0;
    virtual std::unique_ptr<QObject> connectChanged(
        QObject* ctx, std::function<void()> onChanged,
        std::optional<uint32_t> objectID = std::nullopt) const = 0;
    virtual void populate(QTreeWidget* tree, const SelectorRequest& request) const = 0;
};

const IRefDomainAdapter& refDomainAdapter(RefDomain domain);

}
