#include "MediaServices.h"

#include <atomic>

namespace harken::studio::bridge {
namespace {

std::shared_ptr<MediaServices>& installedServices()
{
    static std::shared_ptr<MediaServices> services;
    return services;
}

}

void setMediaServices(std::shared_ptr<MediaServices> services)
{
    std::atomic_store(&installedServices(), std::move(services));
}

std::shared_ptr<MediaServices> mediaServices()
{
    return std::atomic_load(&installedServices());
}

}
