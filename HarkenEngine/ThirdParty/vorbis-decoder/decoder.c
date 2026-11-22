#include "private.h"
#include "vorbis/codec.h"
#include "codec_internal.h"
#include "registry.h"
#include "mdct.h"
#include "misc.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

union HvAllocation {
    max_align_t alignment;
    struct {
        HvAllocation* previous;
        HvAllocation* next;
        size_t bytes;
    } record;
};

struct HvTables {
    HvAllocator allocator;
    mdct_lookup mdct[8];
};
struct HvSetup {
    HvAllocator allocator;
    vorbis_info info;
    private_state backend;
    void* transform[2][1];
};
struct HvState {
    const HvSetup* setup;
    vorbis_dsp_state dsp;
    private_state backend;
    vorbis_block block;
    float* pcm[8];
    float* returned[8];
    ogg_int64_t sequence;
};

void* hv_allocate(HvAllocator* a, size_t bytes) {
    HvAllocation* h;
    if (bytes > SIZE_MAX - sizeof(*h)) longjmp(a->failure, 1);
    if (sizeof(*h) + bytes > a->limit - a->used) longjmp(a->failure, 2);
    h = (HvAllocation*)a->memory.allocate(a->memory.user, sizeof(*h) + bytes);
    if (!h) longjmp(a->failure, 1);
    h->record.previous = NULL; h->record.next = a->first; h->record.bytes = bytes;
    if (a->first) a->first->record.previous = h;
    a->first = h; a->used += sizeof(*h) + bytes;
    if (a->used > a->peak) a->peak = a->used;
    return h + 1;
}
void* hv_callocate(HvAllocator* a, size_t count, size_t size) {
    void* p;
    if (size && count > SIZE_MAX / size) longjmp(a->failure, 1);
    p = hv_allocate(a, count * size); memset(p, 0, count * size); return p;
}
void hv_free(HvAllocator* a, void* p) {
    HvAllocation* h;
    if (!p) return;
    h = (HvAllocation*)p - 1;
    if (h->record.previous) h->record.previous->record.next = h->record.next;
    else a->first = h->record.next;
    if (h->record.next) h->record.next->record.previous = h->record.previous;
    a->used -= sizeof(*h) + h->record.bytes;
    a->memory.release(a->memory.user, h);
}
void* hv_reallocate(HvAllocator* a, void* p, size_t bytes) {
    void* out = hv_allocate(a, bytes);
    if (p) {
        size_t old = ((HvAllocation*)p - 1)->record.bytes;
        memcpy(out, p, old < bytes ? old : bytes); hv_free(a, p);
    }
    return out;
}
static void hv_clear_allocator(HvAllocator* a) {
    while (a->first) hv_free(a, a->first + 1);
}

void* hv_scratch_allocate(HvWorkspace* w, size_t bytes) {
    size_t at = (w->used + 15u) & ~(size_t)15u;
    void* result;
    if (at > w->capacity || bytes > w->capacity - at) longjmp(w->failure, 1);
    result = w->data + at; w->used = at + bytes;
    if (w->used > w->cacheLow) {
        memset(w->packets, 0, sizeof(w->packets));
        w->writeEnd = w->cacheLow = w->cacheTop;
    }
    if (w->used > w->peak) w->peak = w->used;
    return result;
}
void* _vorbis_block_alloc(vorbis_block* vb, long bytes) {
    return hv_scratch_allocate((HvWorkspace*)vb->internal, (size_t)bytes);
}
void _vorbis_block_ripcord(vorbis_block* vb) {
    ((HvWorkspace*)vb->internal)->used = 0;
}

HvTables* hv_tables_create(HvMemory memory, size_t budget) {
    HvTables* t; int i;
    if (!memory.allocate || !memory.release || budget < sizeof(*t)) return NULL;
    t = (HvTables*)memory.allocate(memory.user, sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t)); t->allocator.memory = memory;
    t->allocator.limit = budget - sizeof(*t);
    if (setjmp(t->allocator.failure)) { hv_tables_destroy(t); return NULL; }
    for (i = 0; i < 8; ++i) mdct_init(&t->allocator, &t->mdct[i], 64 << i);
    return t;
}
void hv_tables_destroy(HvTables* t) {
    HvMemory memory;
    if (!t) return;
    memory = t->allocator.memory; hv_clear_allocator(&t->allocator); memory.release(memory.user, t);
}
size_t hv_tables_bytes(const HvTables* t) { return t ? sizeof(*t) + t->allocator.used : 0; }

static int hv_header(HvSetup* s, vorbis_comment* comment, const uint8_t* bytes, size_t count, int bos) {
    ogg_packet packet;
    memset(&packet, 0, sizeof(packet)); packet.packet = (unsigned char*)bytes;
    packet.bytes = (long)count; packet.b_o_s = bos;
    return vorbis_synthesis_headerin(&s->allocator, &s->info, comment, &packet);
}
static HvSetup* hv_setup_attempt(HvMemory memory, size_t budget, const HvTables* tables,
    const uint8_t* ident, size_t identBytes, const uint8_t* setup, size_t setupBytes, enum HvResult* result, unsigned compact, int* limitHit, int prepareWide) {
    HvSetup* s; codec_setup_info* ci; vorbis_comment comment; int i;
    static const uint8_t emptyComment[] = {3,'v','o','r','b','i','s',0,0,0,0,0,0,0,0,1};
    if (!result) return NULL;
    *result = HV_INVALID;
    if (!ident || !setup || identBytes != 30 || setupBytes < 7 || setupBytes > 65535 ||
        !memory.allocate || !memory.release) return NULL;
    *result = HV_MEMORY;
    if (budget < sizeof(*s)) return NULL;
    s = (HvSetup*)memory.allocate(memory.user, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s)); s->allocator.memory = memory; s->allocator.limit = budget - sizeof(*s);
    s->allocator.compactCodebook = compact;
    switch (setjmp(s->allocator.failure)) {
      case 0: break;
      case 2: *limitHit = 1;
      default: *result = HV_MEMORY; hv_setup_destroy(s); return NULL;
    }
    vorbis_info_init(&s->allocator, &s->info); vorbis_comment_init(&comment);
    *result = HV_INVALID;
    if (hv_header(s, &comment, ident, identBytes, 1)) goto fail;
    if (s->info.channels != 1 && s->info.channels != 2 && s->info.channels != 4 &&
        s->info.channels != 6 && s->info.channels != 8) { *result = HV_UNSUPPORTED; goto fail; }
    if (hv_header(s, &comment, emptyComment, sizeof(emptyComment), 0) ||
        hv_header(s, &comment, setup, setupBytes, 0)) {
        if (s->allocator.unsupported) *result = HV_UNSUPPORTED;
        goto fail;
    }
    vorbis_comment_clear(&s->allocator, &comment);
    ci = (codec_setup_info*)s->info.codec_setup;
    if (!tables && !prepareWide) {
        /* Header unpacking checks mapping, floor, residue and mode references.
           Check every Huffman tree, including books unused by the current modes. */
        for (i = 0; i < ci->books; ++i)
            if (hv_book_validate(ci->book_param[i])) goto fail;
        *result = HV_OK;
        return s;
    }
    ci->fullbooks = hv_callocate(&s->allocator, ci->books, sizeof(*ci->fullbooks));
    for (i = 0; i < ci->books; ++i) {
        if (vorbis_book_init_decode(&s->allocator, ci->fullbooks + i, ci->book_param[i])) goto fail;
        vorbis_staticbook_destroy(&s->allocator, ci->book_param[i]); ci->book_param[i] = NULL;
    }
    s->backend.modebits = ov_ilog(ci->modes - 1);
    for (i = 0; i < 2; ++i) {
        int index = ov_ilog(ci->blocksizes[i]) - 7;
        if (index < 0 || index >= 8) goto fail;
        s->backend.window[i] = index;
        s->transform[i][0] = tables ? (void*)&tables->mdct[index] : NULL;
        s->backend.transform[i] = s->transform[i];
    }
    s->backend.flr = hv_callocate(&s->allocator, ci->floors, sizeof(*s->backend.flr));
    s->backend.residue = hv_callocate(&s->allocator, ci->residues, sizeof(*s->backend.residue));
    {
        vorbis_dsp_state prototype;
        memset(&prototype, 0, sizeof(prototype)); prototype.vi = &s->info; prototype.backend_state = &s->backend;
        for (i = 0; i < ci->floors; ++i)
            s->backend.flr[i] = _floor_P[ci->floor_type[i]]->look(&s->allocator, &prototype, ci->floor_param[i]);
        for (i = 0; i < ci->residues; ++i)
            s->backend.residue[i] = _residue_P[ci->residue_type[i]]->look(&s->allocator, &prototype, ci->residue_param[i]);
    }
    *result = HV_OK; return s;
fail:
    hv_setup_destroy(s); return NULL;
}
HvSetup* hv_setup_prepare_wide(HvMemory memory, size_t budget,
    const uint8_t* ident, size_t identBytes, const uint8_t* setup, size_t setupBytes) {
    int limitHit = 0;
    enum HvResult result;
    return hv_setup_attempt(memory, budget, NULL, ident, identBytes,
        setup, setupBytes, &result, 0, &limitHit, 1);
}
void hv_setup_bind_tables(HvSetup* s, const HvTables* tables) {
    for (int i = 0; i < 2; ++i)
        s->transform[i][0] = (void*)&tables->mdct[s->backend.window[i]];
}
HvSetup* hv_setup_create_compact(HvMemory memory, size_t budget, const HvTables* tables,
    const uint8_t* ident, size_t identBytes, const uint8_t* setup, size_t setupBytes, enum HvResult* result) {
    int limitHit = 0;
    if (!tables) { if (result) *result = HV_INVALID; return NULL; }
    return hv_setup_attempt(memory, budget, tables, ident, identBytes, setup, setupBytes,
                            result, 1, &limitHit, 0);
}
HvSetup* hv_setup_create(HvMemory memory, size_t budget, const HvTables* tables,
    const uint8_t* ident, size_t identBytes, const uint8_t* setup, size_t setupBytes, enum HvResult* result) {
    int limitHit = 0;
    if (!tables) { if (result) *result = HV_INVALID; return NULL; }
    HvSetup* decoded = hv_setup_attempt(memory, budget, tables, ident, identBytes,
        setup, setupBytes, result, 0, &limitHit, 0);
    if (!decoded && limitHit)
        decoded = hv_setup_attempt(memory, budget, tables, ident, identBytes,
            setup, setupBytes, result, 1, &limitHit, 0);
    return decoded;
}
enum HvResult hv_inspect_packets(HvMemory memory, size_t budget,
    const uint8_t* ident, size_t identBytes, const uint8_t* header, size_t headerBytes,
    int (*validate)(const HvSetup*, const void*), const void* context) {
    int limitHit = 0;
    enum HvResult result;
    if (!validate) return HV_INVALID;
    HvSetup* layout = hv_setup_attempt(memory, budget, NULL, ident, identBytes,
        header, headerBytes, &result, 0, &limitHit, 0);
    if (layout) {
        result = validate(layout, context) ? HV_OK : HV_INVALID;
        hv_setup_destroy(layout);
    }
    return result;
}
void hv_setup_destroy(HvSetup* s) {
    HvMemory memory;
    if (!s) return;
    memory = s->allocator.memory; hv_clear_allocator(&s->allocator); memory.release(memory.user, s);
}
size_t hv_setup_bytes(const HvSetup* s) { return s ? sizeof(*s) + s->allocator.used : 0; }
size_t hv_setup_peak_bytes(const HvSetup* s) { return s ? sizeof(*s) + s->allocator.peak : 0; }
unsigned hv_setup_channels(const HvSetup* s) { return s ? (unsigned)s->info.channels : 0; }
unsigned hv_setup_blocksize(const HvSetup* s) {
    return s ? (unsigned)((codec_setup_info*)s->info.codec_setup)->blocksizes[1] : 0;
}

size_t hv_state_bytes(const HvSetup* s) {
    return s ? sizeof(HvState) + (size_t)hv_setup_channels(s) * hv_setup_blocksize(s) * sizeof(float) : 0;
}
HvState* hv_state_init(void* storage, size_t bytes, const HvSetup* setup) {
    HvState* state = (HvState*)storage; unsigned i;
    if (!state || !setup || bytes < hv_state_bytes(setup)) return NULL;
    memset(storage, 0, hv_state_bytes(setup)); state->setup = setup;
    for (i = 0; i < hv_setup_channels(setup); ++i)
        state->pcm[i] = (float*)(state + 1) + i * hv_setup_blocksize(setup);
    hv_state_restart(state); return state;
}
void hv_state_restart(HvState* s) {
    memset(&s->dsp, 0, sizeof(s->dsp)); memset(&s->block, 0, sizeof(s->block));
    s->backend = s->setup->backend; s->dsp.vi = (vorbis_info*)&s->setup->info;
    s->dsp.backend_state = &s->backend; s->dsp.pcm = s->pcm; s->dsp.pcmret = s->returned;
    s->dsp.pcm_storage = (int)hv_setup_blocksize(s->setup);
    s->block.vd = &s->dsp; s->sequence = 0; vorbis_synthesis_restart(&s->dsp);
}

/* PCM, residue partition pointers, residue-0 vector scratch, floor posts and
   aligned allocation overhead, bounded by eight channels and 8192 samples. */
#define HV_SCRATCH_BYTES (8u*8192u*sizeof(float) + 8u*4096u*sizeof(void*) + \
    4096u*(sizeof(long)+sizeof(float*)) + 8u*65u*sizeof(int) + 4096u)
size_t hv_workspace_bytes(void) { return sizeof(HvWorkspace) + 15u + HV_SCRATCH_BYTES; }
HvWorkspace* hv_workspace_init(void* storage, size_t bytes) {
    HvWorkspace* w = (HvWorkspace*)storage;
    uintptr_t start;
    if (!w || bytes < hv_workspace_bytes()) return NULL;
    memset(w, 0, sizeof(*w)); start = ((uintptr_t)(w + 1) + 15u) & ~(uintptr_t)15u;
    w->data = (unsigned char*)start; w->capacity = bytes - (start - (uintptr_t)storage);
    w->cacheTop = w->capacity & ~(size_t)15;
    w->writeEnd = w->cacheLow = w->cacheTop;
    return w;
}
size_t hv_workspace_high_water(const HvWorkspace* w) { return w ? w->peak : 0; }
/* Only enable reuse after observing two state addresses for one resource.
   A recycled address only affects admission. Packet identity uses generation and offset. */
static int hv_admit(HvWorkspace* w, HvState* s, uint64_t generation) {
    HvAdmission* a = &w->admissions[generation % HV_RESOURCE_SLOTS];
    if (!generation) return 0;
    if (a->generation != generation) {
        a->generation = generation; a->lastState = (uintptr_t)s; a->remaining = 0;
    } else if (a->lastState != (uintptr_t)s) {
        a->lastState = (uintptr_t)s; a->remaining = 32;
    } else if (a->remaining) {
        --a->remaining;
    }
    return a->remaining != 0;
}

/* Four choices per hash bucket. Only the current and previous arena laps
   can hold live payload. The previous lap is valid below the write cursor. */
static unsigned hv_bucket(uint64_t generation, uint64_t offset) {
    uint64_t key = offset ^ (generation * UINT64_C(0x9e3779b97f4a7c15));
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    return (unsigned)(key >> 48) % HV_PACKET_BUCKETS;
}
static int hv_live(const HvWorkspace* w, const HvCachedPacket* e) {
    return e->generation && (e->lap == w->lap ||
        (w->lap - e->lap == 1 && e->end <= w->writeEnd));
}
static HvCachedPacket* hv_find(HvWorkspace* w, uint64_t generation, uint64_t offset) {
    HvCachedPacket* first = &w->packets[hv_bucket(generation, offset) * HV_WAYS];
    unsigned i;
    for (i = 0; i < HV_WAYS; ++i) {
        HvCachedPacket* e = first + i;
        if (e->generation == generation && e->offset == offset && hv_live(w,e)) return e;
    }
    return NULL;
}

static void hv_store(HvWorkspace* w, const vorbis_block* b, uint64_t generation, uint64_t offset) {
    size_t plane = (size_t)b->pcmend * sizeof(float), bytes = plane * b->vd->vi->channels;
    size_t end = w->writeEnd, begin;
    unsigned bucket, i;
    int ch;
    HvCachedPacket* first;
    HvCachedPacket* entry;
    if (w->used > w->cacheTop || bytes > w->cacheTop - w->used) return;
    if (end < w->used || bytes > end - w->used) {
        end = w->cacheTop;
        if (++w->lap == 0) memset(w->packets, 0, sizeof(w->packets));
    }
    begin = end - bytes;
    bucket = hv_bucket(generation, offset);
    first = &w->packets[bucket * HV_WAYS];
    entry = first + (w->victim[bucket] % HV_WAYS);
    for (i = 0; i < HV_WAYS; ++i) if (!hv_live(w, first + i)) { entry = first + i; break; }
    w->victim[bucket] = (unsigned char)(entry - first + 1);
    entry->generation = generation; entry->offset = offset; entry->begin = begin; entry->end = end;
    entry->lap = w->lap; entry->frames = b->pcmend; entry->window = (int)b->W;
    for (ch = 0; ch < b->vd->vi->channels; ++ch)
        memcpy(w->data + begin + ch * plane, b->pcm[ch], plane);
    w->writeEnd = begin;
    if (begin < w->cacheLow) w->cacheLow = begin;
#ifdef HV_REUSE_STATS
    ++w->copies;
    {
        size_t live = 0;
        for (i = 0; i < HV_PACKET_SLOTS; ++i)
            if (hv_live(w,&w->packets[i])) live += w->packets[i].end - w->packets[i].begin;
        if (live > w->maximumPayload) w->maximumPayload = live;
    }
#endif
}

void hv_predecode_next(HvState* s, HvWorkspace* w, const uint8_t* packet, size_t bytes,
                       uint64_t generation, uint64_t offset) {
    if (!s || !w || !generation || !packet || !bytes || bytes > 65535 ||
        w->ownerAddress != (uintptr_t)s || w->consecutive < 2 || w->preparedPcm ||
        hv_find(w, generation, offset)) return;
    vorbis_block block = s->block;
    ogg_packet p = {0};
    p.packet = (unsigned char*)packet; p.bytes = (long)bytes;
    p.granulepos = -1; p.packetno = s->sequence;
    w->used = 0; block.internal = w; w->preparing = 1;
    if (setjmp(w->failure)) {
        w->preparing = 0;
        return;
    }
    if (vorbis_synthesis(&block, &p) == 0) {
        w->preparedGeneration = generation;
        w->preparedOffset = offset;
        w->preparedPcm = block.pcm;
        w->preparedW = block.W;
        w->preparedEnd = block.pcmend;
    }
    w->preparing = 0;
}

enum HvResult hv_decode_packet(HvState* s, HvWorkspace* w, const uint8_t* packet, size_t bytes,
                               float*** pcm, unsigned* frames) {
    return hv_decode_resident_packet(s, w, packet, bytes, 0, 0, pcm, frames);
}

enum HvResult hv_decode_resident_packet(HvState* s, HvWorkspace* w, const uint8_t* packet, size_t bytes,
                              uint64_t generation, uint64_t offset, float*** pcm, unsigned* frames) {
    float* planes[8];
    HvCachedPacket* e;
    int admit;
    ogg_packet p; float** output = NULL; int count, ch, i;
    if (!s || !w || !packet || !bytes || bytes > 65535 || !pcm || !frames) return HV_INVALID;
    float** pending = NULL;
    if (w->ownerAddress == (uintptr_t)s) {
        if (w->consecutive < 2) ++w->consecutive;
        if (w->preparedGeneration == generation && w->preparedOffset == offset)
            pending = w->preparedPcm;
    } else {
        w->ownerAddress = (uintptr_t)s;
        w->consecutive = 1;
    }
    w->preparedPcm = NULL;
    *pcm = NULL; *frames = 0;
    /* Keep the reserved prefix until the transform and the cache copy finish. */
    if (!pending) w->used = 0;
    s->block.internal = w;
    if (setjmp(w->failure)) return HV_MEMORY;
    memset(&p, 0, sizeof(p)); p.packet = (unsigned char*)packet; p.bytes = (long)bytes;
    p.granulepos = -1; p.packetno = s->sequence++;
    admit = hv_admit(w, s, generation);
    e = admit ? hv_find(w, generation, offset) : NULL;
#ifdef HV_REUSE_STATS
    ++w->tries;
    if (e) ++w->hits;
    if (!admit) ++w->bypass;
#endif
    if (e) {
        for (ch = 0; ch < s->dsp.vi->channels; ++ch)
            planes[ch] = (float*)(w->data + e->begin) + ch * e->frames;
        s->block.pcm = planes; s->block.W = e->window; s->block.pcmend = e->frames;
        s->block.sequence = p.packetno; s->block.granulepos = p.granulepos; s->block.eofflag = 0;
    } else {
        if (pending) {
            s->block.pcm = pending;
            s->block.W = w->preparedW;
            s->block.pcmend = w->preparedEnd;
            s->block.sequence = p.packetno;
            s->block.granulepos = p.granulepos;
            s->block.eofflag = 0;
            for (ch = 0; ch < s->dsp.vi->channels; ++ch)
                mdct_backward(s->setup->backend.transform[s->block.W][0], pending[ch], pending[ch]);
        } else if (vorbis_synthesis(&s->block, &p)) return HV_INVALID;
        if (admit) hv_store(w, &s->block, generation, offset);
    }
    if (vorbis_synthesis_blockin(&s->dsp, &s->block)) return HV_INVALID;
    s->block.pcm = NULL;
    count = vorbis_synthesis_pcmout(&s->dsp, &output);
    for (ch = 0; ch < s->dsp.vi->channels; ++ch) {
        unsigned invalid = 0;
        for (i = 0; i < count; ++i) {
            uint32_t bits;
            memcpy(&bits, output[ch] + i, sizeof(bits));
            invalid |= (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000);
        }
        if (invalid) return HV_INVALID;
    }
    *pcm = output; *frames = (unsigned)count;
    vorbis_synthesis_read(&s->dsp, count); return HV_OK;
}
long hv_packet_blocksize(const HvSetup* s, const uint8_t* bytes, size_t count) {
    ogg_packet p;
    if (!s || !bytes || !count || count > 65535) return -1;
    memset(&p, 0, sizeof(p)); p.packet = (unsigned char*)bytes; p.bytes = (long)count;
    return vorbis_packet_blocksize((vorbis_info*)&s->info, &p);
}
