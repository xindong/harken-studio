#ifndef HARKEN_CORE_MONITOR_FIELD_H
#define HARKEN_CORE_MONITOR_FIELD_H

#if HARKEN_ENGINE_DEV
#define HARKEN_MONITOR_FIELD
#elif defined(_MSC_VER)
#define HARKEN_MONITOR_FIELD [[msvc::no_unique_address]]
#elif defined(__clang__) && !defined(__apple_build_version__) && __clang_major__ < 19
#define HARKEN_MONITOR_FIELD
#else
#define HARKEN_MONITOR_FIELD [[no_unique_address]]
#endif

#endif
