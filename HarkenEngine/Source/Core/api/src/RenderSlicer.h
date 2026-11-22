// Fixed-cadence block staging for the device callback: a real device
// asks for an arbitrary number of frames per callback, but the mixer's
// ramp math advances one step per rendered block — rendering short
// blocks would compress ramp time. The engine therefore always renders
// FULL blockFrames blocks into a staging buffer and the callback copies
// out of it, carrying leftover staged frames across callbacks. Cost: up
// to one block of extra latency. Pure index arithmetic here — no state
// beyond the cursor, no allocation — so the cadence contract is
// unit-testable without a device.
#ifndef HARKEN_API_RENDER_SLICER_H
#define HARKEN_API_RENDER_SLICER_H

#include <cstdint>

namespace harken {

// Read position over the one staged engine block. Persistent across
// callbacks: leftover frames of the last rendered block are consumed
// before the next block is rendered.
struct BlockStagingCursor {
    std::uint32_t readFrames = 0;  // consumed from the staged block
    std::uint32_t availFrames = 0; // still unconsumed in the staged block
};

// Serves frameCount output frames from staged fixed-size engine blocks:
// renderBlock() must render exactly blockFrames frames into the staging
// storage; copy(dstOffsetFrames, stagingOffsetFrames, frames) moves
// staged frames to the output. renderBlock is invoked only when the
// staged block is fully consumed — every engine render is a whole
// block, and every output frame is covered exactly once, in order. A
// zero blockFrames serves nothing (the config layer guarantees a
// positive block size; refusing here keeps a corrupt value from
// spinning the loop forever).
template <typename RenderBlockFn, typename CopyFn>
inline void PumpStagedBlocks(BlockStagingCursor& cursor,
                             std::uint32_t frameCount,
                             std::uint32_t blockFrames,
                             RenderBlockFn&& renderBlock, CopyFn&& copy) {
    if (blockFrames == 0) {
        return;
    }
    std::uint32_t done = 0;
    while (done < frameCount) {
        if (cursor.availFrames == 0) {
            renderBlock();
            cursor.readFrames = 0;
            cursor.availFrames = blockFrames;
        }
        const std::uint32_t remaining = frameCount - done;
        const std::uint32_t frames = (remaining < cursor.availFrames)
                                         ? remaining
                                         : cursor.availFrames;
        copy(done, cursor.readFrames, frames);
        cursor.readFrames += frames;
        cursor.availFrames -= frames;
        done += frames;
    }
}

} // namespace harken

#endif // HARKEN_API_RENDER_SLICER_H
