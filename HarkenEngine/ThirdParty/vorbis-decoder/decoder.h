#ifndef HARKEN_PRIVATE_PACKET_DECODER_H
#define HARKEN_PRIVATE_PACKET_DECODER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct HvTables HvTables;
typedef struct HvSetup HvSetup;
typedef struct HvState HvState;
typedef struct HvWorkspace HvWorkspace;
typedef struct HvMemory {
    void* user;
    void* (*allocate)(void* user, size_t bytes);
    void (*release)(void* user, void* allocation);
} HvMemory;
enum HvResult { HV_OK = 0, HV_INVALID = 1, HV_UNSUPPORTED = 2, HV_MEMORY = 3 };

/* Creation and destruction are control-thread operations. Ownership of the
   tables outlives every setup; ownership of a setup outlives its states. */
HvTables* hv_tables_create(HvMemory memory, size_t budget);
void hv_tables_destroy(HvTables* tables);
size_t hv_tables_bytes(const HvTables* tables);
HvSetup* hv_setup_create(HvMemory memory, size_t budget, const HvTables* tables,
    const uint8_t* ident, size_t identBytes, const uint8_t* setup, size_t setupBytes,
    enum HvResult* result);
void hv_setup_destroy(HvSetup* setup);
size_t hv_setup_bytes(const HvSetup* setup);
size_t hv_setup_peak_bytes(const HvSetup* setup);
unsigned hv_setup_channels(const HvSetup* setup);
unsigned hv_setup_blocksize(const HvSetup* setup);

/* State and workspace backing is caller-owned, aligned for max_align_t.
   The fixed workspace bound covers all supported channels and block sizes. */
size_t hv_state_bytes(const HvSetup* setup);
HvState* hv_state_init(void* storage, size_t bytes, const HvSetup* setup);
void hv_state_restart(HvState* state);
size_t hv_workspace_bytes(void);
HvWorkspace* hv_workspace_init(void* storage, size_t bytes);
size_t hv_workspace_high_water(const HvWorkspace* workspace);

/* PCM is borrowed until the next call on this state. It is in Vorbis order.
   No allocation callbacks, locks, I/O or trigonometric functions are invoked. */
enum HvResult hv_decode_packet(HvState* state, HvWorkspace* workspace,
    const uint8_t* packet, size_t bytes, float*** pcm, unsigned* frames);
long hv_packet_blocksize(const HvSetup* setup, const uint8_t* packet, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
