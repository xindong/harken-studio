#include "ObjectRefNavigation.h"

#include <QWidget>

namespace VNX {

ObjectRefNavigation::ObjectRefNavigation(QWidget* owner)
    : QObject(owner)
{
}

ObjectRefNavigation* ObjectRefNavigation::forWidget(QWidget* source)
{
    for (auto* owner = source; owner; owner = owner->parentWidget()) {
        if (auto* navigation = owner->findChild<ObjectRefNavigation*>(QString(), Qt::FindDirectChildrenOnly))
            return navigation;
    }
    return nullptr;
}

void ObjectRefNavigation::locate(QWidget* source, RefDomain domain, uint32_t id)
{
    if (auto* navigation = forWidget(source))
        emit navigation->locateRequested(domain, id);
}

void ObjectRefNavigation::navigate(QWidget* source, RefDomain domain, uint32_t id)
{
    if (auto* navigation = forWidget(source))
        emit navigation->navigateRequested(domain, id);
}

}
