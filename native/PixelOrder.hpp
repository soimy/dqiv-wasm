#pragma once
// The game's pixel convention is 0xAARRGGBB. Desktop GL accepts that memory
// order directly through GL_BGRA; WebGL2 has no BGRA format for texture uploads
// or readbacks, so on wasm the bytes are swizzled into RGBA order first. Both
// paths end up with the same texture contents and the same channel shifts.
//
// This lives in a header so the layout probe can assert the round trip without a
// GL context - byte order is the change most likely to be silently wrong, and it
// is the one part of the renderer that can be checked without a browser.
#include <cstdint>
#include <span>
#include <vector>
namespace pc {
// Channel shifts for a value this backend's GL reads back.
#ifdef __EMSCRIPTEN__
constexpr unsigned kShiftR=0,kShiftG=8,kShiftB=16;   // a readback gives 0xAABBGGRR
#else
constexpr unsigned kShiftR=16,kShiftG=8,kShiftB=0;   // a readback gives 0xAARRGGBB
#endif
// Pixels in the game's convention, in the byte order this backend's GL accepts.
// On desktop that is the order they are already in, so the pointer comes back
// unchanged; on wasm the bytes are swizzled into a scratch buffer first.
inline const std::uint32_t* uploadOrder(std::span<const std::uint32_t> pixels,std::vector<std::uint32_t>& scratch) {
#ifdef __EMSCRIPTEN__
    if(pixels.empty())return pixels.data();
    // This runs on 4 recipe planes of 256x704 every frame (2.9 MB), so it is fused
    // (no copy-then-rewrite) and swaps R and B with masks instead of four extracts:
    // 0xAARRGGBB -> 0xAABBGGRR. The buffer is kept across calls and written whole,
    // because resize would zero-fill 2.9 MB that is about to be overwritten.
    static thread_local std::vector<std::uint32_t> swizzled;
    (void)scratch;
    if(swizzled.size()<pixels.size())swizzled.resize(pixels.size());
    for(std::size_t i=0;i<pixels.size();++i) {
        const std::uint32_t p=pixels[i];
        swizzled[i]=(p&0xFF00FF00u)|((p&0xFFu)<<16)|((p>>16)&0xFFu);
    }
    return swizzled.data();
#else
    (void)scratch;return pixels.data();
#endif
}
// A value read back through this backend's format, put back in the game's
// convention. The round trip uploadOrder -> readback -> here must be identity,
// on both backends.
inline std::uint32_t fromReadback(std::uint32_t readback) {
    const unsigned alpha=readback>>24;
    return (alpha<<24)|(((readback>>kShiftR)&255)<<16)|(((readback>>kShiftG)&255)<<8)|((readback>>kShiftB)&255);
}
}
