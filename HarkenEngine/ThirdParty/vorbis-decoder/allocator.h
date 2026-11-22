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
    unsigned compactCodebook;
    jmp_buf failure;
} HvAllocator;

enum { HV_PACKET_BUCKETS = 32, HV_WAYS = 4, HV_PACKET_SLOTS = HV_PACKET_BUCKETS * HV_WAYS, HV_RESOURCE_SLOTS = 16 };
typedef struct {
    uint64_t generation, offset;
    size_t begin, end;
    uint32_t lap;
    uint16_t frames, window;
} HvCachedPacket;
typedef struct {
    uint64_t generation;
    uintptr_t lastState;
    unsigned remaining;
} HvAdmission;

struct HvWorkspace {
    HvCachedPacket packets[HV_PACKET_SLOTS];
    HvAdmission admissions[HV_RESOURCE_SLOTS];
    size_t cacheTop, cacheLow, writeEnd;
    uint32_t lap;
    unsigned char victim[HV_PACKET_BUCKETS];
#ifdef HV_REUSE_STATS
    uint64_t tries, hits, copies, bypass, maximumPayload;
#endif
    unsigned char* data;
    size_t capacity, used, peak;
    jmp_buf failure;
    /* The reusable spectrum belongs to this workspace. A state address only
       controls admission; media generation and packet offset identify content. */
    uintptr_t ownerAddress;
    uint64_t preparedGeneration, preparedOffset;
    float** preparedPcm;
    long preparedEnd, preparedW;
    unsigned consecutive;
    int preparing;
};

void* hv_allocate(HvAllocator* allocator, size_t bytes);
void* hv_callocate(HvAllocator* allocator, size_t count, size_t size);
void* hv_reallocate(HvAllocator* allocator, void* data, size_t bytes);
void hv_free(HvAllocator* allocator, void* data);
void* hv_scratch_allocate(HvWorkspace* workspace, size_t bytes);
#endif
