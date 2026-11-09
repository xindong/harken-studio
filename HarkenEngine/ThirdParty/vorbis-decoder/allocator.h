#ifndef HARKEN_VORBIS_ALLOCATOR_H
#define HARKEN_VORBIS_ALLOCATOR_H
#include "decoder.h"
#include <setjmp.h>

typedef union HvAllocation HvAllocation;
typedef struct HvAllocator {
    HvMemory memory;
    HvAllocation* first;
    size_t used, peak, limit;
    int unsupported;
    jmp_buf failure;
} HvAllocator;

struct HvWorkspace {
    unsigned char* data;
    size_t capacity, used, peak;
    jmp_buf failure;
};

void* hv_allocate(HvAllocator* allocator, size_t bytes);
void* hv_callocate(HvAllocator* allocator, size_t count, size_t size);
void* hv_reallocate(HvAllocator* allocator, void* data, size_t bytes);
void hv_free(HvAllocator* allocator, void* data);
void* hv_scratch_allocate(HvWorkspace* workspace, size_t bytes);
#endif
