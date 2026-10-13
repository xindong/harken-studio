#include "ProjectPlatformActivity.h"

#include <QCoreApplication>

namespace VNX {

ProjectPlatformActivity& ProjectPlatformActivity::instance()
{
    static ProjectPlatformActivity activity;
    return activity;
}

ProjectPlatformActivity::ProjectPlatformActivity()
{
    if (const auto* app = QCoreApplication::instance()) moveToThread(app->thread());
}

ProjectPlatformActivity::Lease::Lease()
{
    ProjectPlatformActivity::instance().begin();
}

ProjectPlatformActivity::Lease::~Lease()
{
    ProjectPlatformActivity::instance().end();
}

std::shared_ptr<ProjectPlatformActivity::Lease> ProjectPlatformActivity::acquire()
{
    return std::make_shared<Lease>();
}

bool ProjectPlatformActivity::isBusy() const
{
    return active_.load(std::memory_order_acquire) != 0;
}

void ProjectPlatformActivity::begin()
{
    if (active_.fetch_add(1, std::memory_order_acq_rel) == 0) emit busyChanged(true);
}

void ProjectPlatformActivity::end()
{
    const auto previous = active_.fetch_sub(1, std::memory_order_acq_rel);
    Q_ASSERT(previous > 0);
    if (previous == 1) emit busyChanged(false);
}

}
