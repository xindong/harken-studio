#ifndef HARKEN_TEST_ALLOCATION_PROBE_H
#define HARKEN_TEST_ALLOCATION_PROBE_H

#include <cstddef>

namespace allocation_probe {
extern thread_local bool enabled;
extern thread_local std::size_t count;
extern thread_local std::size_t bytes;
extern thread_local std::size_t failAt;
extern thread_local bool (*countIf)(void*) noexcept;
extern thread_local void* countContext;
}

#endif
